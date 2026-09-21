#include <httplib.h>
#include <nlohmann/json.hpp>
#include <vision/plugin_runtime/loader.hpp>
#include <vision/application/demo.hpp>
#include <vision/storage/store.hpp>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <thread>
#include <iostream>
using namespace vision;
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);QTemporaryDir root;
    try {
        httplib::Server reservation;const auto port=reservation.bind_to_any_port("127.0.0.1");reservation.stop();
        _putenv_s("VISION_HTTP_TEST_TOKEN","01234567890123456789012345678901");
        storage::Store store(std::filesystem::path(root.path().toStdWString()));
        auto event=*application::run_demo(application::DemoScenario::Ng).event;store.commit(event,{"http"});
        auto plugin=plugin_runtime::Library::load(std::filesystem::path(app.arguments()[1].toStdWString()),
            nlohmann::json{{"root",root.path().toStdString()},{"port",port},{"token_env","VISION_HTTP_TEST_TOKEN"}}.dump());
        if(plugin->initialize()!=plugin_sdk::Status::Ok)throw std::runtime_error("Initialize");
        httplib::Client client("127.0.0.1",port);client.set_connection_timeout(1);client.set_read_timeout(2);
        const httplib::Headers auth{{"Authorization","Bearer 01234567890123456789012345678901"}};
        auto get=[&](std::string path,int status,bool authenticated=true){
            auto response=client.Get(path,authenticated?auth:httplib::Headers{});
            if(!response||response->status!=status)throw std::runtime_error(path+" unexpected status");
            return response->body;
        };
        get("/api/v1/health",401,false);
        if(nlohmann::json::parse(get("/api/v1/health",200))["production_ready"]!=false)return 1;
        const auto history=nlohmann::json::parse(get("/api/v1/inspections?limit=1",200));
        if(history["items"].size()!=1)return 1;
        get("/api/v1/inspections?limit=201",400);get("/api/v1/inspections/missing",404);
        const auto request=nlohmann::json{{"event",nlohmann::json::parse(serialization::encode_result(event))},
            {"output_instance_id","http"},{"attempt","1"},{"budget_ns","1000000000"}}.dump();
        if(plugin->output_submit(request)!=plugin_sdk::Status::Ok)return 1;
        std::string report;auto status=plugin_sdk::Status::Busy;
        for(unsigned n=0;n<1000&&status==plugin_sdk::Status::Busy;++n){status=plugin->output_poll(report);std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        if(status!=plugin_sdk::Status::Ok||nlohmann::json::parse(report)["state"]!="BusinessAcked")return 1;
        if(get("/api/v1/events",200).find(event.event_id.value())==std::string::npos)return 1;
        auto expired=client.Get("/api/v1/events",httplib::Headers{{"Authorization",auth.begin()->second},{"Last-Event-ID","old-epoch/1"}});
        if(!expired||expired->status!=409)return 1;
        const std::string bytes="image";store.stage_image("image-1",std::as_bytes(std::span(bytes.data(),bytes.size())));
        get("/api/v1/images/image-1/access",409);store.available("image-1");
        auto access=nlohmann::json::parse(get("/api/v1/images/image-1/access",200));
        if(get(access["url"],200)!=bytes)return 1;
        get("/api/v1/images/image-1/content?expires=1",403);
        store.deleted("image-1");get("/api/v1/images/image-1/access",404);
        plugin->request_stop();return plugin->close()==plugin_sdk::Status::Ok?0:1;
    }catch(const std::exception& e){std::cerr<<e.what();return 1;}
}
