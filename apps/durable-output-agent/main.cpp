#include <vision/runtime/qt/process_host.hpp>
#include <vision/storage/store.hpp>
#include <vision/inference/engine.hpp>
#include <vision/serialization/json_codec.hpp>
#include <nlohmann/json.hpp>
#include <QCoreApplication>
#include <iostream>
#include <set>
using namespace vision;
using J=nlohmann::json;
namespace {
std::uint64_t now_ms(){return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());}
J read(const QString& file){
    const auto bytes=inference::read_file(std::filesystem::path(file.toStdWString()),65536);
    return J::parse(reinterpret_cast<const char*>(bytes.data()),reinterpret_cast<const char*>(bytes.data()+bytes.size()));
}
struct Lane {
    std::string id;
    std::unique_ptr<runtime::qt::ProcessHost> host;
    std::optional<storage::ClaimedIntent> claim;
    std::optional<contracts::Correlation> identity;
    std::uint64_t deadline{};
    bool failed{};
};
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        if(app.arguments().size()!=2)return 2;
        const auto config=[&]{
            if(app.arguments()[1]!="--stdin")return read(app.arguments()[1]);
            std::string text;char byte;
            while(std::cin.get(byte)){if(text.size()>=65536)throw std::invalid_argument("Agent config bound");text+=byte;}
            return J::parse(text);
        }();
        if(config.size()!=3||!config.contains("root")||!config.contains("host")||!config.contains("outputs")||
            !config["outputs"].is_array()||config["outputs"].empty()||config["outputs"].size()>8)return 2;
        const auto root=inference::utf8_path(config.at("root").get<std::string>());
        const auto root_text=std::filesystem::weakly_canonical(root).u8string();
        auto guard=std::make_shared<runtime::qt::StationGuard>("durable-"+QString::fromStdString(
            inference::sha256(std::as_bytes(std::span(root_text.data(),root_text.size()))).substr(7)));
        storage::Store store(root);
        std::vector<Lane> lanes;std::set<std::string> ids;
        std::uint64_t sequence=0;
        for(const auto& output:config["outputs"]){
            if(output.size()!=3)throw std::invalid_argument("Output configuration");
            Lane lane;lane.id=output.at("id");(void)contracts::OutputId(lane.id);
            if(lane.id.size()>100||!ids.insert(lane.id).second)throw std::invalid_argument("Output identity");
            const auto manifest=QString::fromStdString(output.at("manifest").get<std::string>());
            const auto metadata=read(manifest);const auto plugin=metadata.at("plugin_id").get<std::string>();
            // Replay allow-list is deliberately limited to audited nonphysical sinks.
            if(metadata.at("kind")!="ResultOutput"||(plugin!="vision.file-output"&&plugin!="vision.http-output"))
                throw std::invalid_argument("Replay output not permitted");
            runtime::SupervisorPolicy policy;policy.restart_limit=0;
            lane.host=std::make_unique<runtime::qt::ProcessHost>(guard,QString::fromStdString(config.at("host").get<std::string>()),
                QStringList{manifest,QString::fromStdString(output.at("parameters").dump())},"output-"+lane.id,
                "{\"recipe_hash\":\"sha256:"+std::string(64,'a')+"\",\"config_revision\":\"1\"}",policy);
            lanes.push_back(std::move(lane));
        }
        for(auto& lane:lanes){
            auto* current=&lane;
            lane.host->on_task_message=[&,current](const ipc::Message& message){
                if(!current->claim||!current->identity||message.header.type!="DeliveryFinished"||message.correlation!=current->identity)return;
                try {
                    const auto report=J::parse(message.payload);const auto& claim=*current->claim;
                    if(report.at("event_id")!=claim.event||report.at("output_instance_id")!=claim.output||
                        report.at("attempt")!=std::to_string(claim.attempt)||now_ms()>=current->deadline)throw std::runtime_error("Receipt identity/expiry");
                    const auto state=report.at("state").get<std::string>();
                    if(state=="BusinessAcked"||state=="Failed"){store.settle(claim,state,now_ms());if(state=="Failed")current->failed=true;}
                    else current->failed=true; // Ambiguous receipt keeps lease; a later run may retry the same idempotency key.
                }catch(...){current->failed=true;}
                current->claim.reset();current->identity.reset();
            };
            if(!lane.host->start())lane.failed=true;
        }
        bool stopping=false;QElapsedTimer elapsed;elapsed.start();QTimer timer;timer.setInterval(20);
        QObject::connect(&timer,&QTimer::timeout,&app,[&]{
            try {
                bool active=false,waiting=false,alive=false;
                for(auto& lane:lanes){
                    lane.host->pulse_control();alive|=lane.host->snapshot().process_alive;
                    if(stopping)continue;
                    if(lane.failed){lane.host->stop();continue;}
                    if(lane.host->snapshot().phase==runtime::WorkerPhase::ManualIntervention){lane.failed=true;continue;}
                    if(lane.claim){
                        active=true;
                        if(now_ms()>=lane.deadline){lane.failed=true;lane.host->stop();lane.claim.reset();lane.identity.reset();}
                        continue;
                    }
                    if(!lane.host->ready_for_task()){waiting=true;continue;}
                    auto claim=store.claim(lane.id,guard->run_id()+":"+std::to_string(lane.host->snapshot().epoch),now_ms());
                    if(!claim)continue;
                    const auto event=serialization::decode_result(claim->body);
                    contracts::Correlation identity{contracts::RunId(lane.host->run_id()),contracts::WorkerId("output-"+lane.id),
                        lane.host->snapshot().epoch,event.result.inspection_id,contracts::CheckId("delivery"),
                        contracts::TaskId("durable-"+std::to_string(++sequence)),1};
                    const auto now=now_ms();
                    if(claim->expires_ms<=now)continue;
                    const auto milliseconds=std::min<std::uint64_t>(1000,claim->expires_ms-now);
                    const auto request=J{{"event",J::parse(claim->body)},{"output_instance_id",lane.id},
                        {"attempt",std::to_string(claim->attempt)},{"budget_ns",std::to_string(milliseconds*1000000)}}.dump();
                    lane.deadline=now+milliseconds;lane.claim=std::move(claim);lane.identity=identity;
                    if(lane.host->deliver(identity,request,milliseconds*1000000)!=ipc::SendStatus::Accepted)lane.failed=true;
                    active=true;
                }
                if(stopping){
                    if(!alive){
                        bool failed=false;storage::DeliveryTotals totals;
                        for(const auto& lane:lanes){
                            failed|=lane.failed;const auto current=store.delivery_totals(lane.id);
                            totals.total+=current.total;totals.acknowledged+=current.acknowledged;
                            totals.pending+=current.pending;totals.unconfirmed+=current.unconfirmed;
                        }
                        std::cout<<J{{"pending",totals.pending},{"acknowledged",totals.acknowledged},
                            {"unconfirmed",totals.unconfirmed},{"failed_lane",failed},{"production_ready",false}}.dump()<<'\n';
                        app.exit(failed||totals.unconfirmed>totals.pending?1:totals.pending?3:0);
                    }return;
                }
                // Bounded drain invocation. A supervisor/scheduler may invoke it again for remaining leases.
                if(elapsed.elapsed()>15000||(!active&&!waiting)){
                    stopping=true;for(auto& lane:lanes)lane.host->stop();
                }
            }catch(const std::exception& e){std::cerr<<e.what();for(auto& lane:lanes)lane.host->stop();app.exit(1);}
        });
        timer.start();return app.exec();
    }catch(const std::exception& e){std::cerr<<e.what();return 1;}
}
