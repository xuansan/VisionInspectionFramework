#include <vision/plugin_runtime/loader.hpp>
#include <vision/frame_transport/mapping.hpp>
#include <vision/runtime/buffer_broker.hpp>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QUuid>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
using namespace vision;
using namespace contracts;
using J=nlohmann::json;
void check(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);QTemporaryDir temp;
    try {
        const auto config=std::filesystem::path(temp.path().toStdWString())/"config.json";
        {std::ofstream f(config);f<<J{{"mode","measure"},{"min_width",4},{"max_width",4},{"min_height",3},{"max_height",3}};}
        auto path_utf8=config.u8string();
        auto lib=plugin_runtime::Library::load(std::filesystem::path(app.arguments()[1].toStdWString()),
            J{{"config",std::string(reinterpret_cast<const char*>(path_utf8.data()),path_utf8.size())}}.dump());
        check(lib->initialize()==plugin_sdk::Status::Ok,"init");
        RunId run(QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString());PoolId pool("opencv");
        FrameOwner writer{WorkerId("camera"),1},reader{WorkerId("algorithm"),1};
        auto clock=std::make_shared<runtime::FakeClock>();
        runtime::BufferBroker broker(run,pool,1,128,1,clock);
        auto mapping=frame_transport::Mapping::create({run,pool,1,128});
        auto lease=broker.acquire(writer,10000000000ULL);
        FrameDescriptor frame{*lease.lease,FrameId("opencv-frame"),{8,6,12,7,72,PixelFormat::Mono8}};
        check(mapping->arm(frame.permit)==frame_transport::MemoryStatus::Ok,"arm");
        std::vector<std::byte> pixels(72);
        for(int y=2;y<5;++y)for(int x=1;x<5;++x)pixels[y*12+x]=std::byte{255};
        auto writer_mapping=frame_transport::Mapping::open({run,pool,1,128},frame_transport::Access::Writer);
        check(writer_mapping->write(frame,pixels)==frame_transport::MemoryStatus::Ok,"write");
        auto grant=broker.publish(frame.permit,frame.frame,frame.layout,{reader},10000000000ULL);
        const auto request=J{{"slot_count",1},{"slot_bytes","128"},{"budget_ns","3000000000"},
            {"read_frame",J::parse(serialization::encode_frame(grant.readers.at(0),128))}};
        std::string response;
        check(lib->execute(request.dump(),response)==plugin_sdk::Status::Ok,"execute");
        check(J::parse(response)["evidence"]["quality"]=="OK","shared geometry result");
        auto bad=request;bad["budget_ns"]="1";
        check(lib->execute(bad.dump(),response)==plugin_sdk::Status::Ok,"deadline response");
        check(J::parse(response)["evidence"]["quality"]=="Unknown","deadline not NG");
        bad=request;bad["read_frame"]=J::object();
        check(lib->execute(bad.dump(),response)==plugin_sdk::Status::Ok,"invalid evidence");
        check(J::parse(response)["evidence"]["execution_state"]=="Failed","invalid failed");
        check(broker.release(grant.readers[0].permit),"release");
        lib->request_stop();
        check(lib->execute(request.dump(),response)==plugin_sdk::Status::Cancelled,"cancel");
        check(lib->close()==plugin_sdk::Status::Ok,"destroy");
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
