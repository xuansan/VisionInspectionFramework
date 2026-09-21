#include <vision/application/qt/device.hpp>
#include <vision/serialization/json_codec.hpp>
#include <QJsonObject>
#include <QJsonDocument>
namespace vision::application::qt {
namespace {
runtime::SupervisorPolicy policy() {runtime::SupervisorPolicy p;p.restart_limit=0;return p;}
}
DeviceSession::DeviceSession(std::shared_ptr<runtime::qt::StationGuard> guard,QString host,QString manifest,std::string parameters)
    :session_(guard->run_id()+"-device"),
     host_(guard,std::move(host),{std::move(manifest),QString::fromStdString(parameters)},"device",
        "{\"recipe_hash\":\"sha256:"+std::string(64,'a')+"\",\"config_revision\":\"1\"}",policy()) {
    host_.on_task_message=[this](const ipc::Message& m){message(m);};
}
DeviceSession::~DeviceSession() {host_.on_task_message={};host_.stop();}
bool DeviceSession::start() {return !faulted_&&host_.start();}
void DeviceSession::stop() {fail();}
void DeviceSession::fail() {
    faulted_=true;snapshot_={};
    if(pending_) {report_=PhysicalReport{pending_->first,contracts::DeliveryState::Unknown};pending_.reset();}
    active_.reset();host_.stop();
}
bool DeviceSession::submit(const contracts::ResultEnvelope& event) {
    try {(void)serialization::encode_result(event);}catch(...) {return false;}
    const auto s=snapshot();
    if(faulted_||pending_||report_||sent_.size()>=4096||!s.online||!s.position||!s.safety_ok||
       event.result.mode!=contracts::Mode::Demo||event.result.run_id.value()!=host_.run_id()||
       event.result.state!=contracts::InspectionState::Completed||event.result.quality==contracts::QualityVerdict::Unknown||
       sent_.contains(event.event_id.value()))return false;
    pending_={event.event_id.value(),std::string(contracts::name(event.result.quality))};
    sent_.insert(event.event_id.value());sent_pending_=false;return true;
}
DeviceSnapshot DeviceSession::snapshot() const {
    if(faulted_||!fresh_||clock_.now_ns()>=fresh_||!host_.snapshot().process_alive)return {};
    return snapshot_;
}
void DeviceSession::pulse(bool ready,bool arrival,bool position,bool safety_ok) {
    host_.pulse_control();
    const auto now=clock_.now_ns();
    if(faulted_)return;
    if((active_&&now>=due_)||(fresh_&&now>=fresh_)||host_.snapshot().phase==runtime::WorkerPhase::ManualIntervention) {fail();return;}
    if(active_||!host_.ready_for_task()||now<next_)return;
    if(sequence_==UINT64_MAX) {fail();return;}
    ++sequence_;
    contracts::Correlation id{contracts::RunId(host_.run_id()),contracts::WorkerId("device"),host_.snapshot().epoch,
        contracts::InspectionId("device-session"),contracts::CheckId("exchange"),contracts::TaskId("device-"+std::to_string(sequence_)),1};
    QJsonObject payload{{"session_id",QString::fromStdString(session_)},{"command_sequence",QString::number(sequence_)},
        {"ready",ready},{"arrival",arrival},{"position",position},{"safety_ok",safety_ok},
        {"result_id",QJsonValue::Null},{"quality","Unknown"}};
    if(pending_&&!sent_pending_) {payload["result_id"]=QString::fromStdString(pending_->first);payload["quality"]=QString::fromStdString(pending_->second);sent_pending_=true;}
    due_=runtime::deadline_after(clock_,150000000);
    if(host_.device(id,QJsonDocument(payload).toJson(QJsonDocument::Compact).toStdString(),150000000)!=ipc::SendStatus::Accepted) {fail();return;}
    active_=std::move(id);next_=now+20000000;
}
void DeviceSession::message(const ipc::Message& m) {
    if(m.header.type!="DeviceFinished"||!active_||m.correlation!=active_||faulted_)return;
    if(clock_.now_ns()>=due_) {fail();return;}
    const auto p=QJsonDocument::fromJson(QByteArray::fromStdString(m.payload)).object();
    snapshot_={true,p["ready"].toBool(),p["position"].toBool(),p["safety_ok"].toBool(),
        contracts::parse_u64(p["arrival_sequence"].toString().toStdString())};
    fresh_=runtime::deadline_after(clock_,200000000);
    if(pending_&&sent_pending_) {
        if(p["ack_id"].toString().toStdString()!=pending_->first) {fail();return;}
        report_=PhysicalReport{pending_->first,contracts::DeliveryState::BusinessAcked};pending_.reset();sent_pending_=false;
    } else if(!p["ack_id"].isNull()) {fail();return;}
    active_.reset();
}
std::optional<PhysicalReport> DeviceSession::take_report() {auto r=std::move(report_);report_.reset();return r;}
}
