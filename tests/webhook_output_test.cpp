#include <httplib.h>
#include <nlohmann/json.hpp>
#include <vision/plugin_runtime/loader.hpp>
#include <vision/application/qt/dispatcher.hpp>
#include <vision/application/demo.hpp>
#include <QCoreApplication>
#include <QUuid>
#include <iostream>
#include <thread>
#include <vision/storage/store.hpp>
#include <QTemporaryDir>
#include "network_durable.hpp"
using namespace vision;
using J=nlohmann::json;
namespace {
struct Server {
    httplib::Server server;
    int port{};
    std::thread thread;
    std::atomic<unsigned> requests{};
    std::atomic<bool> valid{true};
    Server(const std::string& scenario,const std::string& event_id) {
        server.set_read_timeout(1);server.set_write_timeout(1);
        server.Post("/results",[&,scenario,event_id](const auto& request,auto& response) {
            ++requests;
            try {
                const auto envelope=J::parse(request.body);
                if(envelope.at("event").at("event_id")!=event_id||
                    envelope.at("output_instance_id")!="webhook"||envelope.at("attempt")!="1"||
                    request.get_header_value("Authorization")!="Bearer 01234567890123456789012345678901"||
                    request.get_header_value("Idempotency-Key")!=event_id)valid=false;
            }catch(...){valid=false;}
            if(scenario=="slow"||scenario=="cancel")std::this_thread::sleep_for(std::chrono::milliseconds(700));
            if(scenario=="reject"){response.status=503;return;}
            if(scenario=="redirect"){response.set_redirect("/results",307);return;}
            if(scenario=="empty"){response.status=202;return;}
            if(scenario=="malformed"){response.set_content("{","application/json");return;}
            if(scenario=="oversized"){response.set_content(std::string(8192,'x'),"application/json");return;}
            if(scenario=="duplicate"){response.set_content("{\"event_id\":\"wrong\",\"event_id\":\""+event_id+"\",\"state\":\"BusinessAcked\"}","application/json");return;}
            response.set_content(J{{"event_id",scenario=="wrong-id"?"wrong":event_id},
                {"state",scenario=="no-ack"?"Accepted":"BusinessAcked"}}.dump(),"application/json");
        });
        port=server.bind_to_any_port("127.0.0.1");
        if(port<=0)throw std::runtime_error("Server bind");
        if(scenario=="offline"){server.stop();return;}
        thread=std::thread([this]{server.listen_after_bind();});
    }
    ~Server(){server.stop();if(thread.joinable())thread.join();}
};
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        const auto args=app.arguments();
        const auto scenario=args[2].toStdString();
        auto event=*application::run_demo(application::DemoScenario::Ng).event;
        Server server(scenario,event.event_id.value());
        _putenv_s("VISION_WEBHOOK_TEST_TOKEN","01234567890123456789012345678901");
        const auto params=J{{"url","http://127.0.0.1:"+std::to_string(server.port)+"/results"},
            {"token_env","VISION_WEBHOOK_TEST_TOKEN"},{"timeout_ms",300}}.dump();
        if(args.size()==6){
            QTemporaryDir root;
            {storage::Store store(std::filesystem::path(root.path().toStdWString()));store.commit(event,{"webhook"});}
            const auto config=J{{"root",root.path().toStdString()},{"host",args[3].toStdString()},
                {"outputs",J::array({{{"id","webhook"},{"manifest",args[1].toStdString()},
                    {"parameters",J::parse(params)},{"replay_policy","NonPhysicalIdempotent"}}})}};
            if(!rejects_network_replay(args[5],config)||server.requests!=0)return 1;
            return run_network_durable(args[5],config);
        }
        if(args.size()>3) {
            auto guard=std::make_shared<runtime::qt::StationGuard>("webhook-"+QUuid::createUuid().toString(QUuid::WithoutBraces));
            application::qt::Dispatcher dispatcher(guard,args[3],{
                {"webhook",params,args[1],1},{"peer","{}",args[4],1}});
            if(!dispatcher.start())return 1;
            bool submitted=false,stopping=false,saw_peer=false;
            QElapsedTimer elapsed;elapsed.start();QTimer timer;timer.setInterval(5);
            QObject::connect(&timer,&QTimer::timeout,&app,[&] {
                dispatcher.pulse();
                if(!submitted&&dispatcher.ready())submitted=dispatcher.submit(event,2000000000ULL);
                for(const auto& report:dispatcher.deliveries())
                    if(report.output_id=="peer"&&report.state=="BusinessAcked")saw_peer=true;
                if(submitted&&!stopping&&dispatcher.idle()){
                    const auto reports=dispatcher.deliveries();
                    bool webhook=false;
                    for(const auto& report:reports)if(report.output_id=="webhook")
                        webhook=report.state==(scenario=="success"?"BusinessAcked":"Failed");
                    if(!webhook||!saw_peer||!server.valid){dispatcher.stop();app.exit(1);return;}
                    dispatcher.stop();stopping=true;
                }
                if(stopping){
                    bool alive=false;for(const auto& worker:dispatcher.workers())alive|=worker.process_alive;
                    if(!alive)app.exit(0);
                }
                if(elapsed.elapsed()>6000){dispatcher.stop();app.exit(1);}
            });
            timer.start();return app.exec();
        }
        const auto manifest=std::filesystem::path(args[1].toStdWString());
        if(scenario=="invalid"){
            for(const auto& url:{"http://example.com/results","https://example.com/#secret",
                "https://user@example.com/results","https://example.com/?secret=x","http://127.0.0.1.evil/results",
                "https://example.com/\r\nX: bad"}){
                bool rejected=false;
                try {
                    auto config=J::parse(params);config["url"]=url;
                    auto plugin=plugin_runtime::Library::load(manifest,config.dump());
                    rejected=plugin->initialize()!=plugin_sdk::Status::Ok;
                    plugin->close();
                }catch(...){rejected=true;}
                if(!rejected)throw std::runtime_error("Unsafe URL accepted");
            }
            _putenv_s("VISION_WEBHOOK_TEST_TOKEN","bad\r\nheader");
            auto plugin=plugin_runtime::Library::load(manifest,params);
            return plugin->initialize()==plugin_sdk::Status::Invalid?0:1;
        }
        auto plugin=plugin_runtime::Library::load(manifest,params);
        if(plugin->initialize()!=plugin_sdk::Status::Ok)throw std::runtime_error("Initialize");
        const auto request=J{{"event",J::parse(serialization::encode_result(event))},
            {"output_instance_id","webhook"},{"attempt","1"},{"budget_ns","2000000000"}}.dump();
        const auto rounds=scenario=="repeat"?2:1;
        for(int round=0;round<rounds;++round) {
            if(plugin->output_submit(request)!=plugin_sdk::Status::Ok)throw std::runtime_error("Submit");
            if(plugin->output_submit(request)!=plugin_sdk::Status::Busy)throw std::runtime_error("Queue bound");
            if(scenario=="cancel"){
                for(unsigned n=0;n<100&&!server.requests;++n)std::this_thread::sleep_for(std::chrono::milliseconds(2));
                plugin->request_stop();
                std::string report;
                if(plugin->output_poll(report)!=plugin_sdk::Status::Cancelled)throw std::runtime_error("Cancel");
                break;
            }
            std::string report;auto status=plugin_sdk::Status::Busy;
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
            while(status==plugin_sdk::Status::Busy&&std::chrono::steady_clock::now()<deadline){
                status=plugin->output_poll(report);std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            const auto expected=scenario=="success"||scenario=="repeat"?"BusinessAcked":"Failed";
            if(status!=plugin_sdk::Status::Ok)throw std::runtime_error("Report deadline");
            const auto ack=J::parse(report);
            if(ack.at("state")!=expected||ack.at("event_id")!=event.event_id.value()||
                ack.at("output_instance_id")!="webhook"||ack.at("attempt")!="1")
                throw std::runtime_error("Report identity/state");
        }
        plugin->request_stop();
        if(plugin->close()!=plugin_sdk::Status::Ok||!server.valid)throw std::runtime_error("Close/request contract");
        if(scenario=="repeat"&&server.requests!=2)throw std::runtime_error("Repeat");
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
