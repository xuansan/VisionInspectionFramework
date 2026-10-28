#include <httplib.h>
#include <nlohmann/json.hpp>
#include <vision/plugin_runtime/loader.hpp>
#include <vision/application/qt/dispatcher.hpp>
#include <vision/application/demo.hpp>
#include <vision/storage/store.hpp>
#include <vision/inference/engine.hpp>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QUuid>
#include <iostream>
#include <thread>
#include <mutex>
#include "network_durable.hpp"
#define NOMINMAX
#include <windows.h>
using namespace vision;
using J=nlohmann::json;
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);QTemporaryDir temporary;
    httplib::Server server;std::thread listener;
    int result=1;
    try {
        const auto args=app.arguments();const auto scenario=args[2].toStdString();
        std::atomic<bool> signed_requests{true};
        std::atomic<unsigned> puts{},gets{};
        std::mutex mutex;std::map<std::string,std::string> objects;
        // Drain callbacks before their captured local state is destroyed, including on exceptions.
        struct ServerDrain {
            httplib::Server& server;std::thread& listener;
            ~ServerDrain(){server.stop();if(listener.joinable())listener.join();}
        } drain{server,listener};
        server.set_read_timeout(1);server.set_write_timeout(1);server.set_payload_max_length(16*1024*1024);
        auto auth=[&](const httplib::Request& r){
            if(!r.get_header_value("Authorization").starts_with("AWS4-HMAC-SHA256")||
                r.get_header_value("x-amz-date").empty())signed_requests=false;
        };
        server.Put(R"(/images/(.*))",[&](const auto& r,auto& p){
            auth(r);++puts;
            if(scenario=="slow")std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            if(scenario=="reject"||scenario=="retry-limit"){p.status=503;return;}
            std::lock_guard guard(mutex);objects[r.path]=r.body;p.status=200;
        });
        server.Get(R"(/images/(.*))",[&](const auto& r,auto& p){
            auth(r);++gets;std::lock_guard guard(mutex);
            if(!objects.contains(r.path)){p.status=404;return;}
            p.set_content(scenario=="corrupt"?"corrupt":objects[r.path],"application/octet-stream");
        });
        const auto port=server.bind_to_any_port("127.0.0.1");if(port<=0)throw std::runtime_error("Bind");
        listener=std::thread([&]{server.listen_after_bind();});
        const auto* external=std::getenv("VISION_S3_PLUGIN_ENDPOINT");
        if(!external){
            _putenv_s("VISION_S3_PLUGIN_ACCESS","local-test-access");
            _putenv_s("VISION_S3_PLUGIN_SECRET","local-test-secret");
        }
        const auto root=std::filesystem::path(temporary.path().toStdWString());
        auto event=*application::run_demo(application::DemoScenario::Ng).event;
        const std::string recipe="s3-test-recipe";
        event.result.recipe_hash=inference::sha256(std::as_bytes(std::span(recipe.data(),recipe.size())));
        contracts::FrameDescriptor frame{{event.result.run_id,contracts::PoolId("pool"),
            {contracts::WorkerId("frame-archive"),1},0,1,2},contracts::FrameId("frame"),
            {8,4,8,0,32,contracts::PixelFormat::Mono8}};
        const std::vector<std::byte> pixels(32,std::byte{42});
        std::vector<std::string> images;
        {
            storage::Store store(root);
            if(scenario!="no-frames"){
                images.push_back(store.archive_frame(frame,32,event.result.inspection_id.value(),"camera-a",event.result.recipe_hash,recipe,pixels));
                images.push_back(store.archive_frame(frame,32,event.result.inspection_id.value(),"camera-b",event.result.recipe_hash,recipe,pixels));
            }
            if(scenario!="uncommitted")store.commit(event,{"s3"});
        }
        const auto params=J{{"root",temporary.path().toStdString()},
            {"endpoint",external?std::string(external):"http://127.0.0.1:"+std::to_string(port)},{"bucket",external?"inspection-test":"images"},
            {"access_env","VISION_S3_PLUGIN_ACCESS"},{"secret_env","VISION_S3_PLUGIN_SECRET"}}.dump();
        HANDLE lock=INVALID_HANDLE_VALUE;
        if(scenario=="locked")lock=CreateFileW((root/L".upload-lock").c_str(),GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(args.size()==6){
            const auto config=J{{"root",temporary.path().toStdString()},{"host",args[3].toStdString()},
                {"outputs",J::array({{{"id","s3"},{"manifest",args[1].toStdString()},{"parameters",J::parse(params)}}})}};
            result=run_network_durable(args[5],config);
        }else if(args.size()>3){
            auto guard=std::make_shared<runtime::qt::StationGuard>("s3-"+QUuid::createUuid().toString(QUuid::WithoutBraces));
            application::qt::Dispatcher dispatcher(guard,args[3],{{"s3",params,args[1],1},{"peer","{}",args[4],1}});
            if(!dispatcher.start())throw std::runtime_error("Dispatcher start");
            bool submitted=false,stopping=false;QElapsedTimer elapsed;elapsed.start();QTimer timer;timer.setInterval(5);
            QObject::connect(&timer,&QTimer::timeout,&app,[&]{
                dispatcher.pulse();
                if(!submitted&&dispatcher.ready())submitted=dispatcher.submit(event,5000000000ULL);
                if(submitted&&!stopping&&dispatcher.idle()){
                    bool peer=false,s3=false;
                    for(const auto& report:dispatcher.deliveries()){
                        if(report.output_id=="peer")peer=report.state=="BusinessAcked";
                        if(report.output_id=="s3")s3=report.state==(scenario=="success"?"BusinessAcked":scenario=="slow"?"Unknown":"Failed");
                    }
                    if(!peer||!s3){dispatcher.stop();app.exit(1);return;}
                    dispatcher.stop();stopping=true;
                }
                if(stopping){
                    bool alive=false;for(const auto& w:dispatcher.workers())alive|=w.process_alive;
                    if(!alive)app.exit(0);
                }
                if(elapsed.elapsed()>7000){dispatcher.stop();app.exit(1);}
            });
            timer.start();result=app.exec();
        }else{
            auto plugin=plugin_runtime::Library::load(std::filesystem::path(args[1].toStdWString()),params);
            if(plugin->initialize()!=plugin_sdk::Status::Ok)throw std::runtime_error("Initialize");
            const auto request=J{{"event",J::parse(serialization::encode_result(event))},
                {"output_instance_id","s3"},{"attempt","1"},{"budget_ns","5000000000"}}.dump();
            for(unsigned round=0;round<(scenario=="repeat"?2U:scenario=="retry-limit"?4U:1U);++round){
                if(plugin->output_submit(request)!=plugin_sdk::Status::Invalid)throw std::runtime_error("Wrong kind accepted");
                if(plugin->storage_submit(request)!=plugin_sdk::Status::Ok)throw std::runtime_error("Submit");
                if(plugin->storage_submit(request)!=plugin_sdk::Status::Busy)throw std::runtime_error("Queue bound");
                std::string report;auto status=plugin_sdk::Status::Busy;
                for(unsigned n=0;n<3000&&status==plugin_sdk::Status::Busy;++n){
                    status=plugin->storage_poll(report);std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                if(status!=plugin_sdk::Status::Ok)throw std::runtime_error("Report");
                const auto ack=J::parse(report);
                if(ack.at("state")!=(scenario=="success"||scenario=="repeat"?"BusinessAcked":"Failed")||
                    ack.at("event_id")!=event.event_id.value()||ack.at("output_instance_id")!="s3")
                    throw std::runtime_error("Ack");
            }
            plugin->request_stop();if(plugin->close()!=plugin_sdk::Status::Ok)throw std::runtime_error("Close");
            result=0;
        }
        if(lock!=INVALID_HANDLE_VALUE)CloseHandle(lock);
        if(result==0){
            storage::Store store(root);
            for(const auto& id:images){
                const auto expected=scenario=="success"||scenario=="repeat"?"Available":scenario=="retry-limit"&&id==images.front()?"Failed":"Pending";
                if(store.image_state(id)!=expected)throw std::runtime_error("Premature available");
                if(store.image(id)!=pixels)throw std::runtime_error("Local image changed");
            }
            if(!signed_requests)throw std::runtime_error("Missing signature");
            if(scenario=="repeat"&&(puts!=2||gets!=4))throw std::runtime_error("Duplicate upload");
            if(scenario=="retry-limit"&&puts!=3)throw std::runtime_error("Upload retry bound");
            if((scenario=="uncommitted"||scenario=="no-frames"||scenario=="locked")&&puts)
                throw std::runtime_error("Unexpected network IO");
        }
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';result=1;}
    server.stop();if(listener.joinable())listener.join();return result;
}
