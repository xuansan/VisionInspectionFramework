#include <vision/capture/qt/session.hpp>
#include <vision/serialization/json_codec.hpp>
#include <QJsonDocument>
#include <QJsonObject>

namespace vision::capture::qt {
using namespace contracts;
namespace {
runtime::SupervisorPolicy policy() {
    runtime::SupervisorPolicy p;p.restart_limit=0;p.progress_ns=11000000000ULL;return p;
}
std::string json(QJsonObject value) {return QJsonDocument(value).toJson(QJsonDocument::Compact).toStdString();}
}
Session::Session(std::shared_ptr<runtime::qt::StationGuard> guard,QString host,QString manifest,
    std::string parameters,std::string worker,ImageLayout layout,std::uint32_t slot_count,std::string recipe_hash)
    :layout_(layout),slots_(slot_count),worker_(std::move(worker)),clock_(std::make_shared<runtime::SteadyClock>()),
     host_(std::move(guard),std::move(host),{std::move(manifest),QString::fromStdString(parameters)},worker_,
        "{\"recipe_hash\":\""+recipe_hash+"\",\"config_revision\":\"1\"}",policy()) {
    validate_layout(layout_,16*1024*1024);
    if(!slots_||slots_>16)throw std::invalid_argument("Capture slot count");
    host_.on_task_message=[this](const ipc::Message& m){message(m);};
}
Session::~Session() {host_.on_task_message={};host_.stop();}
bool Session::start() {
    observe_exit();
    const auto s=host_.snapshot();
    if(s.process_alive||completion_||active_||
       (s.phase!=runtime::WorkerPhase::Stopped&&s.phase!=runtime::WorkerPhase::ManualIntervention)||
       (broker_&&broker_->snapshot().leases)||pool_sequence_==UINT64_MAX)return false;
    reader_.reset();mapping_.reset();broker_.reset();
    const auto pool=PoolId(worker_+"-"+std::to_string(++pool_sequence_));
    frame_transport::PoolSpec spec{RunId(host_.run_id()),pool,slots_,checked_add(layout_.offset,layout_.length)};
    mapping_=frame_transport::Mapping::create(spec);
    reader_=frame_transport::Mapping::open(spec,frame_transport::Access::Reader);
    broker_=std::make_unique<runtime::BufferBroker>(spec.run,pool,slots_,spec.slot_bytes,1,clock_);
    retiring_=false;return host_.start();
}
void Session::stop() {
    if(active_)terminate("Cancelled","CAPTURE.STOPPED");
    else host_.stop();
}
bool Session::ready() const {return broker_&&!active_&&!completion_&&!retiring_&&host_.ready_for_task();}
bool Session::capture(std::uint64_t budget) {
    if(!ready()||!budget||budget>10000000000ULL||sequence_==UINT64_MAX)return false;
    const auto due=runtime::deadline_after(*clock_,budget);
    const auto epoch=host_.snapshot().epoch;
    const auto grant=broker_->acquire({WorkerId(worker_),epoch},budget);
    if(!grant.lease)return false;
    FrameDescriptor frame{*grant.lease,FrameId("capture-"+std::to_string(++sequence_)),layout_};
    if(mapping_->arm(frame.permit)!=frame_transport::MemoryStatus::Ok) {
        broker_->release(frame.permit);retiring_=true;host_.stop();return false;
    }
    Correlation c{RunId(host_.run_id()),WorkerId(worker_),epoch,InspectionId("capture-"+std::to_string(sequence_)),
        CheckId("image"),TaskId("capture-"+std::to_string(sequence_)),1};
    const auto& spec=mapping_->spec();
    const auto payload=json({{"slot_count",static_cast<int>(spec.slot_count)},{"slot_bytes",QString::number(spec.slot_bytes)},
        {"write_frame",QJsonDocument::fromJson(QByteArray::fromStdString(serialization::encode_frame(frame,spec.slot_bytes))).object()},
        {"budget_ns",QString::number(budget)}});
    const auto accepted=host_.capture(c,payload,budget);
    if(accepted!=ipc::SendStatus::Accepted) {broker_->release(frame.permit);return false;}
    active_=std::move(frame);correlation_=std::move(c);active_epoch_=epoch;
    due_=due;return true; // Admission/serialization time is part of the task budget.
}
void Session::terminate(std::string state,std::string error) {
    if(!active_||retiring_)return;
    broker_->quarantine(active_->permit);retiring_=true;
    completion_=Completion{correlation_->task_id.value(),std::move(state),std::move(error),{}};
    host_.cancel(*correlation_);host_.stop(); // Keep active ownership until OS exit.
}
void Session::cancel() {terminate("Cancelled","CAPTURE.CANCELLED");}
void Session::observe_exit() {
    const auto s=host_.snapshot();
    if(active_&&!s.process_alive&&s.epoch==active_epoch_&&
       (s.phase==runtime::WorkerPhase::Stopped||s.phase==runtime::WorkerPhase::ManualIntervention)) {
        broker_->worker_exited(active_->permit.owner);
        if(!retiring_)completion_=Completion{correlation_->task_id.value(),"Failed","CAPTURE.WORKER_EXIT",{}};
        active_.reset();correlation_.reset();retiring_=true;
    }
}
void Session::pulse() {
    if(active_&&!retiring_&&clock_->now_ns()>=due_)terminate("TimedOut","CAPTURE.TIMEOUT");
    host_.pulse_control();observe_exit();
}
void Session::message(const ipc::Message& m) {
    if(m.header.type!="CaptureFinished"||!active_||!correlation_||m.correlation!=correlation_||retiring_)return;
    if(clock_->now_ns()>=due_) {terminate("TimedOut","CAPTURE.TIMEOUT");return;}
    const auto p=QJsonDocument::fromJson(QByteArray::fromStdString(m.payload)).object();
    const auto state=p["execution_state"].toString().toStdString();
    if(state=="Succeeded") {
        try {
            const auto received=serialization::decode_frame(json(p["frame"].toObject()),mapping_->spec().slot_bytes);
            if(received!=*active_) {terminate("Failed","CAPTURE.FRAME_MISMATCH");return;}
            // Probe immutable slot header before trusting the completion.
            // A broker reader lease is required before any read.
            auto published=broker_->publish(active_->permit,active_->frame,active_->layout,
                {{WorkerId("capture-consumer"),1}},30000000000ULL);
            if(published.status!=runtime::LeaseStatus::Accepted) {terminate("Failed","CAPTURE.PUBLISH_FAILED");return;}
            const auto frame=published.readers.at(0);
            if(reader_->read(frame,0,[](auto){})!=frame_transport::MemoryStatus::Ok) {
                broker_->release(frame.permit);
                terminate("Failed","CAPTURE.UNREADABLE_FRAME");return;
            }
            completion_=Completion{correlation_->task_id.value(),"Succeeded","",frame};
        } catch(...) {terminate("Failed","CAPTURE.INVALID_FRAME");return;}
    } else {
        // Public worker only sends this after camera_close completed successfully.
        broker_->release(active_->permit);
        completion_=Completion{correlation_->task_id.value(),state,p["error_code"].toString().toStdString(),{}};
    }
    active_.reset();correlation_.reset();
}
std::optional<Completion> Session::take_completion() {
    auto result=std::move(completion_);completion_.reset();return result;
}
bool Session::read(const FrameDescriptor& frame,const std::function<void(std::span<const std::byte>)>& visitor) {
    if(!broker_||!broker_->authorized(frame))return false;
    const auto status=reader_->read(frame,0,visitor);
    if(status==frame_transport::MemoryStatus::Abandoned) {
        broker_->quarantine(frame.permit);retiring_=true;host_.stop();
    }
    return status==frame_transport::MemoryStatus::Ok;
}
bool Session::release(const FrameDescriptor& frame) {
    if(!broker_||frame.permit.owner.worker.value()!="capture-consumer")return false;
    return broker_->release(frame.permit);
}
runtime::BufferSnapshot Session::buffers() const {return broker_?broker_->snapshot():runtime::BufferSnapshot{};}
std::optional<FrameDescriptor> Session::handoff(const FrameDescriptor& frame,FrameOwner owner,std::uint64_t budget) {
    if(!broker_||retiring_||frame.permit.owner.worker.value()!="capture-consumer")return {};
    return broker_->transfer(frame,std::move(owner),budget);
}
std::optional<FrameDescriptor> Session::reclaim_archive(const FrameDescriptor& frame,std::uint64_t budget) {
    if(!broker_||retiring_||frame.permit.owner.worker.value()!="frame-archive")return {};
    return broker_->transfer(frame,{WorkerId("capture-consumer"),host_.snapshot().epoch},budget);
}
void Session::quarantine(const FrameDescriptor& frame) {
    if(broker_)broker_->quarantine(frame.permit);
    retiring_=true;host_.stop(); // A crashed reader may abandon the slot mutex: replace the entire pool.
}
bool Session::consumer_stopped(const FrameDescriptor& frame) {return broker_&&broker_->release(frame.permit);}
frame_transport::PoolSpec Session::pool() const {
    if(!mapping_)throw std::logic_error("Capture pool not started");
    return mapping_->spec();
}
}
