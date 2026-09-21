#include <vision/plugin_runtime/loader.hpp>
#include <vision/application/demo.hpp>
#include <vision/inference/engine.hpp>
#include <nlohmann/json.hpp>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <thread>
using namespace vision;
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);QTemporaryDir root;const auto format=app.arguments()[2].toStdString();
    try {
        auto plugin=plugin_runtime::Library::load(std::filesystem::path(app.arguments()[1].toStdWString()),
            nlohmann::json{{"root",root.path().toStdString()},{"format",format},{"max_files",2}}.dump());
        if(plugin->initialize()!=plugin_sdk::Status::Ok)return 1;
        auto submit=[&](const contracts::ResultEnvelope& event,bool success) {
            const auto request=nlohmann::json{{"event",nlohmann::json::parse(serialization::encode_result(event))},
                {"output_instance_id","file"},{"attempt","1"},{"budget_ns","1000000000"}}.dump();
            if(plugin->output_submit(request)!=plugin_sdk::Status::Ok)throw std::runtime_error("Submit");
            std::string report;auto status=plugin_sdk::Status::Busy;
            for(unsigned n=0;n<1000&&status==plugin_sdk::Status::Busy;++n){status=plugin->output_poll(report);std::this_thread::sleep_for(std::chrono::milliseconds(1));}
            if(status!=plugin_sdk::Status::Ok||(nlohmann::json::parse(report)["state"]=="BusinessAcked")!=success)throw std::runtime_error("Report");
        };
        auto event=*application::run_demo(application::DemoScenario::Ng).event;
        event.result.workpiece_id=contracts::WorkpieceId("-formula");
        submit(event,true);submit(event,true);
        auto conflicting=event;conflicting.result.workpiece_id=contracts::WorkpieceId("changed");submit(conflicting,false);
        for(unsigned n=0;n<3;++n){event.event_id=contracts::EventId("file-"+std::to_string(n));submit(event,true);}
        plugin->request_stop();if(plugin->close()!=plugin_sdk::Status::Ok)return 1;
        unsigned count=0;
        for(const auto& file:std::filesystem::directory_iterator(std::filesystem::path(root.path().toStdWString()))) {
            if(file.path().extension()!="."+format)continue;++count;
            const auto bytes=inference::read_file(file.path(),65536);const std::string text(reinterpret_cast<const char*>(bytes.data()),bytes.size());
            if(format=="jsonl"){(void)serialization::decode_result(text);if(text.back()!='\n')return 1;}
            else if(text.find("\"'-formula\"")==std::string::npos||text.find("schema_version,event_id")!=0)return 1;
        }
        return count==2?0:1;
    }catch(...){return 1;}
}
