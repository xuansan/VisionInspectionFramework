#include <vision/algorithm/qt/session.hpp>
#include <vision/serialization/json_codec.hpp>
#include <QJsonDocument>
#include <QJsonObject>
namespace vision::algorithm::qt {
namespace {
std::string json(QJsonObject p) {return QJsonDocument(p).toJson(QJsonDocument::Compact).toStdString();}
runtime::SupervisorPolicy policy() {runtime::SupervisorPolicy p;p.restart_limit=0;p.progress_ns=11000000000ULL;return p;}
}
Session::Session(std::shared_ptr<runtime::qt::StationGuard> guard,QString host,QString manifest,
    std::string parameters,std::string worker,capture::qt::Session& source,std::string recipe_hash)
    :source_(source),worker_(std::move(worker)),clock_(std::make_shared<runtime::SteadyClock>()),
     host_(std::make_unique<runtime::qt::ProcessHost>(std::move(guard),std::move(host),
        QStringList{std::move(manifest),QString::fromStdString(parameters)},worker_,
        "{\"recipe_hash\":\""+recipe_hash+"\",\"config_revision\":\"1\"}",policy())) {
    host_->on_task_message=[this](const ipc::Message& m){message(m);};
}
Session::~Session() {
    host_->on_task_message={};
    if(frame_)source_.quarantine(*frame_);
    const auto exited=host_->shutdown_and_wait();
    host_.reset();
    if(frame_&&exited)source_.consumer_stopped(*frame_);
}
bool Session::start() {
    observe_exit();
    if(frame_||completion_)return false;
    const auto ok=host_->start();if(ok)retiring_=false;return ok;
}
bool Session::ready() const {return !frame_&&!completion_&&!retiring_&&host_->ready_for_task();}
contracts::Correlation Session::next_identity(contracts::InspectionId inspection,contracts::CheckId check) const {
    if(sequence_==UINT64_MAX)throw std::overflow_error("Algorithm task identity exhausted");
    return {contracts::RunId(host_->run_id()),contracts::WorkerId(worker_),host_->snapshot().epoch,
        std::move(inspection),std::move(check),contracts::TaskId("inspect-"+std::to_string(sequence_+1)),1};
}
bool Session::inspect(const contracts::FrameDescriptor& frame,contracts::InspectionId inspection,
    contracts::CheckId check,std::uint64_t budget) {
    if(!ready()||!budget||budget>10000000000ULL||sequence_==UINT64_MAX)return false;
    const auto due=runtime::deadline_after(*clock_,budget);
    const auto epoch=host_->snapshot().epoch;
    if(frame.permit.run.value()!=host_->run_id())return false;
    auto grant=source_.handoff(frame,{contracts::WorkerId(worker_),epoch},budget);
    if(!grant)return false;
    contracts::Correlation id{contracts::RunId(host_->run_id()),contracts::WorkerId(worker_),epoch,
        std::move(inspection),std::move(check),contracts::TaskId("inspect-"+std::to_string(++sequence_)),1};
    try {
        const auto spec=source_.pool();
        auto payload=json({{"slot_count",static_cast<int>(spec.slot_count)},{"slot_bytes",QString::number(spec.slot_bytes)},
            {"read_frame",QJsonDocument::fromJson(QByteArray::fromStdString(serialization::encode_frame(*grant,spec.slot_bytes))).object()},
            {"budget_ns",QString::number(budget)}});
        if(host_->inspect(id,std::move(payload),budget)!=ipc::SendStatus::Accepted) {
            source_.consumer_stopped(*grant);return false;
        }
    } catch(...) {source_.consumer_stopped(*grant);throw;}
    frame_=std::move(grant);identity_=std::move(id);due_=due;return true;
}
void Session::terminate(std::string state,std::string error) {
    if(!frame_||retiring_)return;
    completion_=Completion{*identity_,std::move(state),"Unknown",std::move(error)};
    retiring_=true;source_.quarantine(*frame_);
    host_->cancel(*identity_);host_->stop();
}
void Session::cancel() {terminate("Cancelled","ALGORITHM.CANCELLED");}
void Session::stop() {if(frame_)terminate("Cancelled","ALGORITHM.STOPPED");else host_->stop();}
void Session::observe_exit() {
    if(!frame_)return;
    const auto s=host_->snapshot();
    if(!s.process_alive&&s.epoch==identity_->worker_epoch&&
        (s.phase==runtime::WorkerPhase::Stopped||s.phase==runtime::WorkerPhase::ManualIntervention)) {
        if(!retiring_) {
            completion_=Completion{*identity_,"Failed","Unknown","ALGORITHM.WORKER_EXIT"};
            source_.quarantine(*frame_);retiring_=true;
        }
        source_.consumer_stopped(*frame_);frame_.reset();identity_.reset();
    }
}
void Session::pulse() {
    if(frame_&&!retiring_&&clock_->now_ns()>=due_)terminate("TimedOut","ALGORITHM.TIMEOUT");
    host_->pulse_control();observe_exit();
}
void Session::message(const ipc::Message& m) {
    if((m.header.type!="TaskFinished"&&m.header.type!="CheckFinished")||!frame_||!identity_||m.correlation!=identity_||retiring_)return;
    if(clock_->now_ns()>=due_) {terminate("TimedOut","ALGORITHM.TIMEOUT");return;}
    const auto p=QJsonDocument::fromJson(QByteArray::fromStdString(m.payload)).object();
    if(m.header.type=="CheckFinished") {
        auto evidence=serialization::decode_check(json(p["check"].toObject()));
        completion_=Completion{*identity_,std::string(contracts::name(evidence.state)),std::string(contracts::name(evidence.quality)),
            evidence.error?evidence.error->code:"",std::move(evidence)};
    } else completion_=Completion{*identity_,p["execution_state"].toString().toStdString(),
        p["quality"].toString().toStdString(),p["error_code"].toString().toStdString(),{}};
    source_.consumer_stopped(*frame_);frame_.reset();identity_.reset();
}
std::optional<Completion> Session::take_completion() {auto c=std::move(completion_);completion_.reset();return c;}
}
