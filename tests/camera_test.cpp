#include <vision/plugin_runtime/loader.hpp>
#include <vision/capture/source.hpp>
#include <vision/frame_transport/mapping.hpp>
#include <vision/runtime/buffer_broker.hpp>
#include <QCoreApplication>
#include <QProcess>
#include <QTemporaryDir>
#include <QUuid>
#include <QDir>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <thread>
#include <array>
#define NOMINMAX
#include <windows.h>

using namespace vision;
using namespace contracts;
using namespace plugin_sdk;
using J=nlohmann::json;
namespace {
void check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
const ImageLayout layout{8,4,8,0,32,PixelFormat::Mono8};
struct Fixture {
    RunId run{QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString()};
    PoolId pool{"capture"};
    FrameOwner owner{WorkerId("camera"),1},reader{WorkerId("test-reader"),1};
    std::shared_ptr<runtime::FakeClock> clock=std::make_shared<runtime::FakeClock>();
    runtime::BufferBroker broker{run,pool,2,32,1,clock};
    frame_transport::PoolSpec spec{run,pool,2,32};
    std::unique_ptr<frame_transport::Mapping> mapping=frame_transport::Mapping::create(spec);
    std::string settings() const {
        return J{{"run_id",run.value()},{"pool_id",pool.value()},{"worker_id",owner.worker.value()},
            {"worker_epoch","1"},{"slot_count",2},{"slot_bytes","32"}}.dump();
    }
    FrameDescriptor grant(unsigned n) {
        auto grant=broker.acquire(owner,10000000000);check(bool(grant.lease),"write grant");
        FrameDescriptor d{*grant.lease,FrameId("frame-"+std::to_string(n)),layout};
        check(mapping->arm(d.permit)==frame_transport::MemoryStatus::Ok,"arm");
        return d;
    }
    void verify(const FrameDescriptor& d,std::uint64_t sequence) {
        const auto published=broker.publish(d.permit,d.frame,d.layout,{reader},10000000000);
        check(published.status==runtime::LeaseStatus::Accepted,"publish");
        check(broker.authorized(published.readers.at(0)),"authorization");
        auto read=frame_transport::Mapping::open(spec,frame_transport::Access::Reader);
        capture::SourceConfig config;config.width=8;config.height=4;config.seed=42;
        const auto expected=capture::Source(config).read(sequence);
        check(read->read(published.readers[0],0,[&](auto pixels) {
            check(std::vector<std::byte>(pixels.begin(),pixels.end())==expected.pixels,"pixel mismatch");
        })==frame_transport::MemoryStatus::Ok,"read");
        check(broker.release(published.readers[0].permit),"release reader");
    }
};
std::unique_ptr<plugin_runtime::Library> load(const QString& manifest,const J& parameters) {
    auto lib=plugin_runtime::Library::load(std::filesystem::path(manifest.toStdWString()),parameters.dump());
    check(lib->initialize()==Status::Ok,"camera init");return lib;
}
J params(const std::string& fault="none") {
    return {{"width",8},{"height",4},{"seed",42},{"fault",fault},{"fault_at","1"},{"delay_ms",60}};
}
int child(const QStringList& args) {
    check(args.size()==6,"child arguments");
    auto parameters=J::parse(args[3].toStdString());
    auto lib=load(args[2],parameters);
    check(lib->camera_open(args[4].toStdString())==Status::Ok,"child open");
    check(lib->camera_trigger(args[5].toStdString())==Status::Ok,"child trigger");
    std::string result;
    check(lib->camera_poll(result)==Status::Ok,"child poll");
    std::cout<<result<<std::endl;
    check(lib->close()==Status::Ok,"child destroy");
    return 0;
}
int local(const QString& manifest,const std::string& scenario) {
    Fixture f;auto config=params();
    if(scenario=="delay"||scenario=="missing"||scenario=="disconnect")config=params(scenario);
    if(scenario=="duplicate"||scenario=="out_of_order") {config=params(scenario);config["fault_at"]="3";}
    auto lib=load(manifest,config);
    std::string result;
    check(lib->camera_enumerate(result)==Status::Ok&&J::parse(result)["devices"][0]["simulated"]==true,"enumerate");
    check(lib->camera_poll(result)==Status::Invalid,"poll before open");
    check(lib->execute("{}",result)==Status::Unsupported,"camera executed as algorithm");
    check(lib->camera_open(f.settings())==Status::Ok,"open");
    check(lib->camera_open(f.settings())==Status::Invalid,"double open");
    check(lib->camera_poll(result)==Status::Busy,"empty poll");
    const auto first=f.grant(1);
    if(scenario=="lifecycle") {
        auto wrong=first;wrong.permit.owner.epoch=2;
        check(lib->camera_trigger(serialization::encode_frame(wrong,32))==Status::Invalid,"wrong epoch accepted");
        wrong=first;wrong.permit.run=RunId("wrong");
        check(lib->camera_trigger(serialization::encode_frame(wrong,32))==Status::Invalid,"wrong run accepted");
        wrong=first;wrong.layout.width=7;
        check(lib->camera_trigger(serialization::encode_frame(wrong,32))==Status::Invalid,"wrong layout accepted");
        check(lib->camera_trigger("{\"bad\":1}")==Status::Invalid,"bad permit accepted");
    }
    const auto permit=serialization::encode_frame(first,32);
    check(lib->camera_trigger(permit)==Status::Ok,"trigger");
    check(lib->camera_trigger(permit)==Status::Busy,"unbounded pending camera queue");
    if(scenario=="missing"||scenario=="delay")check(lib->camera_poll(result)==Status::Busy,"injection did not delay");
    if(scenario=="missing"||scenario=="cancel") {
        if(scenario=="missing") {
            std::this_thread::sleep_for(std::chrono::milliseconds(90));
            check(lib->camera_poll(result)==Status::Busy&&result.empty(),"missing frame fabricated completion");
        }
        lib->request_stop();check(lib->camera_poll(result)==Status::Cancelled,"cancel missing");
        check(lib->camera_trigger(permit)==Status::Cancelled,"trigger after stop");
        check(f.broker.snapshot().leases==1,"cancel prematurely freed coordinator lease");
        check(lib->camera_close()==Status::Ok,"close after cancel");
        check(f.broker.release(first.permit),"release after stopped access");
    } else if(scenario=="disconnect") {
        check(lib->camera_poll(result)==Status::Failed,"disconnect hidden");
        check(lib->camera_trigger(permit)==Status::Failed,"disconnected trigger");
        check(lib->camera_close()==Status::Ok,"close disconnected");
        check(f.broker.release(first.permit),"disconnect release after close");
        check(lib->camera_open(f.settings())==Status::Ok,"reopen");
    } else {
        if(scenario=="delay")std::this_thread::sleep_for(std::chrono::milliseconds(90));
        check(lib->camera_poll(result)==Status::Ok,"poll");
        check(serialization::decode_frame(result,32)==first,"first identity");
        f.verify(first,1);
        check(lib->camera_trigger(permit)==Status::Invalid,"replayed permit accepted");
        if(scenario=="duplicate"||scenario=="out_of_order") {
            const auto second=f.grant(2);
            check(lib->camera_trigger(serialization::encode_frame(second,32))==Status::Ok,"second trigger");
            check(lib->camera_poll(result)==Status::Ok,"second poll");f.verify(second,2);
            const auto third=f.grant(3);
            check(lib->camera_trigger(serialization::encode_frame(third,32))==Status::Ok,"third trigger");
            check(lib->camera_poll(result)==Status::Ok,"injected stale poll");
            const auto stale=serialization::decode_frame(result,32);
            check(stale==(scenario=="duplicate"?second:first),"wrong stale frame injection");
            check(stale!=third,"injection silently corrected");
            check(f.broker.publish(stale.permit,stale.frame,stale.layout,{f.reader},100).status==runtime::LeaseStatus::Stale,
                "stale notification published");
            check(f.broker.snapshot().writing==1,"stale freed active writer");
            check(lib->camera_close()==Status::Ok,"stale close");
            check(f.broker.release(third.permit),"stale release after close");
        }
    }
    check(lib->close()==Status::Ok&&lib->close()==Status::Ok,"destroy");
    check(lib->camera_trigger(permit)==Status::Invalid,"use after destroy");
    return 0;
}
int process(const QString& manifest,bool crash) {
    Fixture f;const auto frame=f.grant(1);
    QProcess worker;worker.start(QCoreApplication::applicationFilePath(),{"--child",manifest,
        QString::fromStdString(params(crash?"crash":"none").dump()),QString::fromStdString(f.settings()),
        QString::fromStdString(serialization::encode_frame(frame,32))});
    check(worker.waitForStarted(2000)&&worker.waitForFinished(4000),"camera process wait");
    if(crash) {
        check(worker.exitCode()==86,"wrong crash exit");
        check(worker.readAllStandardOutput().isEmpty(),"crash fabricated frame");
        check(f.broker.snapshot().writing==1,"crash freed without OS exit reconciliation");
        f.broker.worker_exited(f.owner);check(f.broker.snapshot().free==2,"exit cleanup");
        // Second live camera session proves the crashed plugin did not poison the parent/other pool.
        return process(manifest,false);
    }
    check(worker.exitCode()==0,"camera child error");
    const auto descriptor=serialization::decode_frame(worker.readAllStandardOutput().toStdString(),32);
    check(descriptor==frame,"child frame identity");f.verify(descriptor,1);
    return 0;
}
int replay(const QString& manifest,bool sequence) {
    QTemporaryDir dir(QDir::currentPath()+"/out/camera-assets-XXXXXX");check(dir.isValid(),"replay directory");
    const auto name=QString::fromUtf8("\xe5\x9b\xbe\xe5\x83\x8f.pgm");
    const auto path=std::filesystem::path(dir.filePath(name).toStdWString());
    capture::SourceConfig source;source.width=8;source.height=4;source.seed=42;
    const auto image=capture::Source(source).read(1);
    {std::ofstream file(path,std::ios::binary);file<<"P5\n8 4\n255\n";
        file.write(reinterpret_cast<const char*>(image.pixels.data()),static_cast<std::streamsize>(image.pixels.size()));}
    const auto image2=capture::Source(source).read(2);
    const auto second_path=std::filesystem::path(dir.filePath("second.pgm").toStdWString());
    {std::ofstream file(second_path,std::ios::binary);file<<"P5\n8 4\n255\n";
        file.write(reinterpret_cast<const char*>(image2.pixels.data()),static_cast<std::streamsize>(image2.pixels.size()));}
    auto config=params();config["source"]=sequence?"sequence":"fixed";config["root"]=dir.path().toUtf8().toStdString();
    config["files_json"]=(sequence?J::array({name.toUtf8().toStdString(),"second.pgm"}):J::array({name.toUtf8().toStdString()})).dump();
    Fixture f;auto lib=load(manifest,config);check(lib->camera_open(f.settings())==Status::Ok,"file open");
    const auto frame=f.grant(1);
    check(lib->camera_trigger(serialization::encode_frame(frame,32))==Status::Ok,"file trigger");
    std::string result;check(lib->camera_poll(result)==Status::Ok,"file poll");f.verify(frame,1);
    if(sequence) {
        const auto frame2=f.grant(2);
        check(lib->camera_trigger(serialization::encode_frame(frame2,32))==Status::Ok,"sequence trigger");
        check(lib->camera_poll(result)==Status::Ok,"sequence poll");f.verify(frame2,2);
    }
    // Malformed asset after initialization fails explicitly without successful frame.
    {std::ofstream file(path,std::ios::binary|std::ios::trunc);file<<"P5\n8 4\n255\nbroken";}
    const auto second=f.grant(2);
    check(lib->camera_trigger(serialization::encode_frame(second,32))==Status::Ok,"broken trigger");
    check(lib->camera_poll(result)==Status::Failed&&result.empty(),"broken file became success");
    check(lib->close()==Status::Ok,"file close");
    check(f.broker.release(second.permit),"file failure release");
    return 0;
}
int buffers(const QString& manifest) {
    // Direct SDK call verifies small caller buffers do not lose completed frames.
    const auto binary=std::filesystem::path(manifest.toStdWString()).parent_path()/L"vision-sim-camera.dll";
    struct Module {
        HMODULE module{};
        Plugin* plugin{};
        Destroy destroy{};
        ~Module() {if(plugin&&destroy)destroy(plugin);if(module)FreeLibrary(module);}
    } module;
    module.module=LoadLibraryExW(binary.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    check(module.module!=nullptr,"direct camera library");
    auto create=reinterpret_cast<Create>(GetProcAddress(module.module,"vision_plugin_create"));
    module.destroy=reinterpret_cast<Destroy>(GetProcAddress(module.module,"vision_plugin_destroy"));
    check(create&&module.destroy&&create(&module.plugin)==Status::Ok,"direct camera factory");
    auto camera=dynamic_cast<Camera*>(module.plugin);check(camera!=nullptr,"direct camera type");
    auto input=[](const std::string& text) {return Bytes{reinterpret_cast<const std::uint8_t*>(text.data()),static_cast<std::uint32_t>(text.size())};};
    auto parameters=params().dump();
    check(camera->initialize(input(parameters))==Status::Ok,"direct initialize");
    Fixture f;const auto settings=f.settings();
    check(camera->open(input(settings))==Status::Ok,"direct open");
    const auto frame=f.grant(1);const auto permit=serialization::encode_frame(frame,32);
    check(camera->trigger(input(permit))==Status::Ok,"direct trigger");
    std::array<std::uint8_t,1> tiny{};
    Buffer limited_output{tiny.data(),1,99};
    check(camera->poll_frame(limited_output)==Status::Busy&&limited_output.size==0,"small buffer lost completion");
    check(camera->trigger(input(permit))==Status::Busy,"small output released pending capacity");
    std::array<std::uint8_t,max_payload> data{};
    Buffer output{data.data(),static_cast<std::uint32_t>(data.size()),0};
    check(camera->poll_frame(output)==Status::Ok,"completion retry");
    check(serialization::decode_frame({reinterpret_cast<const char*>(output.data),output.size},32)==frame,"retry identity");
    f.verify(frame,1);
    check(camera->poll_frame(output)==Status::Busy,"duplicate unsolicited completion");
    check(camera->close()==Status::Ok,"direct close");
    // Repeat initialization must not replace a configured source.
    check(camera->initialize(input(parameters))==Status::Invalid,"double initialize");
    return 0;
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    try {
        const auto args=app.arguments();
        if(args.size()>1&&args[1]=="--child")return child(args);
        check(args.size()==3,"camera test arguments");
        const auto scenario=args[1].toStdString();
        if(scenario=="process"||scenario=="crash")process(args[2],scenario=="crash");
        else if(scenario=="replay"||scenario=="sequence")replay(args[2],scenario=="sequence");
        else if(scenario=="buffers")buffers(args[2]);
        else local(args[2],scenario);
        std::cout<<scenario<<" passed\n";return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
