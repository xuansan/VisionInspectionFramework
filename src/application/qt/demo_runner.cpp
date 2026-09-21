#include <vision/application/qt/recipe.hpp>
#include <vision/application/qt/demo_runner.hpp>
#include <vision/serialization/json_codec.hpp>
#include <QFile>
#include <QCryptographicHash>
#include <QUuid>
#include <nlohmann/json.hpp>
namespace vision::application::qt {
using J=nlohmann::json;
DemoRunner::DemoRunner(DemoPaths paths,std::string scenario,unsigned count,std::optional<DurableDemoConfig> durable)
    :guard_(std::make_shared<runtime::qt::StationGuard>("interactive-demo")),
     scenario_(std::move(scenario)),target_(count) {
    if(!count||count>32||(scenario_!="normal"&&scenario_!="ng"&&scenario_!="algorithm-crash"&&
       scenario_!="output-hang"&&scenario_!="camera-missing"))throw std::invalid_argument("Demo scenario/count");
    const auto recipe=load_recipe(paths.recipe,{paths.host,paths.camera,paths.algorithm});
    auto c=recipe.config;
    if(scenario_=="ng")c.algorithm_parameters[1]=R"({"minimum":255,"maximum":255})";
    if(scenario_=="algorithm-crash") {c.algorithm_parameters[0]=R"({"fault":"crash"})";target_=1;}
    if(scenario_=="camera-missing") {
        auto p=J::parse(c.camera_parameters[0]);p["fault"]="missing";c.camera_parameters[0]=p.dump();
        c.budget_ns=200000000;target_=1;
    }
    // Fault injection is part of this immutable Demo snapshot, not a silent runtime edit.
    const auto effective=J{{"base",c.recipe_hash},{"base_recipe",J::parse(recipe.canonical)},{"scenario",scenario_},{"camera_parameters",c.camera_parameters},
        {"algorithm_parameters",c.algorithm_parameters},{"budget_ns",c.budget_ns}}.dump();
    c.recipe_hash="sha256:"+QCryptographicHash::hash(QByteArray::fromStdString(effective),QCryptographicHash::Sha256).toHex().toStdString();
    if(durable&&!durable->archive_program.isEmpty())c.archive=FrameArchiveConfig{durable->archive_program,durable->root,c.recipe_hash,effective};
    station_=std::make_unique<Station>(guard_,c);
    device_=std::make_unique<DeviceSession>(guard_,paths.host,paths.device,"{}");
    dispatcher_=std::make_unique<Dispatcher>(guard_,paths.host,std::vector<OutputConfig>{
        {"display","{}",paths.output,8},{"audit",scenario_=="output-hang"?R"({"fault":"hang"})":"{}",paths.output,8}});
    if(durable)durable_=std::make_unique<DurableSession>(guard_,paths.host,std::move(*durable));
}
DemoRunner::~DemoRunner() {stop();}
bool DemoRunner::start() {
    if(started_)return false;started_=true;deadline_=runtime::deadline_after(clock_,30000000000ULL);
    if(!station_->start()||!device_->start()||!dispatcher_->start()||(durable_&&!durable_->start())) {error_="DEMO.START_FAILED";stop();return false;}
    return true;
}
void DemoRunner::pause() {station_->pause();arrival_high_=false;}
bool DemoRunner::resume() {return station_->resume();}
void DemoRunner::stop() {
    if(stopping_)return;stopping_=true;
    station_->stop();device_->stop();dispatcher_->stop();
    if(durable_)durable_->stop();
    if(physical_pending_) {physical_pending_=false;physical_="Unknown";}
    else if(commit_pending_)physical_="NotSent:CommitUnconfirmed";
}
void DemoRunner::send_physical(const contracts::ResultEnvelope& event){
    if(stopping_)return;
    if(event.result.state==contracts::InspectionState::Completed) {
        if(!device_->submit(event)) {error_="DEMO.PLC_REJECTED";stop();return;}
        physical_pending_=true;physical_="Pending";
    } else physical_="NotSent:UnknownQuality";
}
void DemoRunner::pulse() {
    if(!started_||finished_)return;
    station_->pulse();dispatcher_->pulse();
    if(durable_)durable_->pulse();
    if(stopping_) {
        device_->pulse(false,false);
        bool alive=false;for(const auto& w:snapshot().workers)alive|=w.process_alive;
        if(!alive&&!station_->snapshot().archive_alive&&(!durable_||!durable_->snapshot().agent_alive)&&station_->snapshot().leases==0&&station_->snapshot().tickets==0)finished_=true;
        return;
    }
    if(clock_.now_ns()>=deadline_) {error_="DEMO.DEADLINE";stop();return;}
    if(durable_){
        const auto status=durable_->snapshot();
        if(!status.error.empty()&&!files_draining_){error_=status.error;stop();return;}
        if(auto committed=durable_->take_committed()){
            commit_pending_=false;send_physical(*committed);
            if(stopping_)return;
        }
    }
    const auto s=station_->snapshot();
    const bool ready=s.ready&&!physical_pending_&&!commit_pending_&&results_<target_&&(!durable_||durable_->ready());
    // Hold low for several exchanges before each new simulated arrival edge.
    if(!ready) {arrival_high_=false;pulse_count_=0;}
    else if(++pulse_count_>=5)arrival_high_=true;
    device_->pulse(ready,arrival_high_);
    const auto d=device_->snapshot();
    if(d.online&&d.ready&&d.position&&d.safety_ok&&ready&&d.arrival_sequence>arrival_seen_) {
        if(d.arrival_sequence!=arrival_seen_+1||station_->trigger(d.arrival_sequence)!=TriggerStatus::Accepted) {
            error_="DEMO.TRIGGER_REJECTED";stop();return;
        }
        arrival_seen_=d.arrival_sequence;
    }
    if(auto event=station_->take_result()) {
        ++results_;last_result_=serialization::encode_result(*event);
        if(events_.size()>=32) {error_="DEMO.EVENT_CAPACITY";stop();return;}
        if(event->result.state!=contracts::InspectionState::Completed)target_=results_;
        events_.push_back(*event);
        if(!dispatcher_->submit(*event,1500000000)) {error_="DEMO.OUTBOX_FULL";stop();return;}
        if(durable_){
            if(!durable_->submit(*event)){error_="DURABLE.COMMIT_REJECTED";stop();return;}
            commit_pending_=true;physical_="NotSent:AwaitingCommit";
        } else send_physical(*event);
    }
    if(auto report=device_->take_report()) {
        physical_pending_=false;physical_=report->state==contracts::DeliveryState::BusinessAcked?"BusinessAcked":"Unknown";
        if(report->state!=contracts::DeliveryState::BusinessAcked) {error_="DEMO.PLC_UNKNOWN";stop();return;}
    }
    if(results_>=target_&&!physical_pending_&&!commit_pending_&&dispatcher_->idle()&&!station_->snapshot().active){
        if(durable_){
            if(!files_draining_){files_draining_=true;durable_->drain_files();}
            if(durable_->files_finished())stop();
        }else stop();
    }
}
DemoSnapshot DemoRunner::snapshot() const {
    auto workers=dispatcher_->workers();workers.push_back(device_->worker());
    for(const auto& worker:station_->workers())workers.push_back(worker);
    auto deliveries=dispatcher_->deliveries();
    if(durable_){
        for(const auto& worker:durable_->workers())workers.push_back(worker);
        for(const auto& delivery:durable_->deliveries())deliveries.push_back(delivery);
    }
    return {station_->snapshot(),device_->snapshot(),std::move(deliveries),std::move(workers),
        last_result_,physical_,error_,results_,finished_,durable_?durable_->snapshot():DurableSnapshot{}};
}
std::vector<contracts::ResultEnvelope> DemoRunner::take_events() {auto e=std::move(events_);events_.clear();return e;}
}
