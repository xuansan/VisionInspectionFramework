#include <vision/application/qt/station.hpp>
#include <chrono>
namespace vision::application::qt {
using namespace contracts;
Station::Station(std::shared_ptr<runtime::qt::StationGuard> guard,StationConfig config)
    :config_(std::move(config)),run_(guard?guard->run_id():""),clock_(std::make_shared<runtime::SteadyClock>()),
     budget_({{"camera-a",1},{"camera-b",1},{"algorithm-a",1},{"algorithm-b",1},{"result",1}},1,clock_) {
    if(!guard||!valid_hash(config_.recipe_hash)||!config_.budget_ns||config_.budget_ns>10000000000ULL)
        throw std::invalid_argument("Station configuration");
    for(std::size_t i=0;i<2;++i) {
        cameras_[i]=std::make_unique<capture::qt::Session>(guard,config_.host,config_.camera_manifest,
            config_.camera_parameters[i],"camera-"+std::to_string(i),config_.layout,2,config_.recipe_hash);
        algorithms_[i]=std::make_unique<algorithm::qt::Session>(guard,config_.host,config_.algorithm_manifest,
            config_.algorithm_parameters[i],"algorithm-"+std::to_string(i),*cameras_[i],config_.recipe_hash);
    }
    if(config_.archive)archive_=std::make_unique<FrameArchive>(guard,*config_.archive,std::array{cameras_[0].get(),cameras_[1].get()});
}
Station::~Station() {stop();}
bool Station::start(Mode mode) {
    if(stopping_||!production_.start(mode))return false;
    for(std::size_t i=0;i<2;++i)if(!cameras_[i]->start()||!algorithms_[i]->start()) {
        production_.fault();for(auto& a:algorithms_)a->stop();for(auto& c:cameras_)c->stop();return false;
    }
    return true;
}
bool Station::resources_ready() const {
    return cameras_[0]->ready()&&cameras_[1]->ready()&&algorithms_[0]->ready()&&algorithms_[1]->ready();
}
bool Station::settled() const {
    if(archive_&&archive_->busy())return false;
    for(std::size_t i=0;i<2;++i)
        if(cameras_[i]->buffers().leases||cameras_[i]->worker().busy||algorithms_[i]->worker().busy)return false;
    return true;
}
TriggerStatus Station::trigger(std::uint64_t sequence) {
    if(!sequence||events_==UINT64_MAX)return TriggerStatus::Invalid;
    if(sequence<=last_trigger_)return TriggerStatus::Duplicate;
    if(!production_.accepts())return TriggerStatus::NotReady;
    if(inspection_||result_||ticket_)return TriggerStatus::Full;
    if(!resources_ready()||counter_.snapshot().total>=4096)return TriggerStatus::NotReady;
    due_=runtime::deadline_after(*clock_,config_.budget_ns);
    auto reserved=budget_.try_reserve({{"camera-a",1},{"camera-b",1},{"algorithm-a",1},{"algorithm-b",1},{"result",1}},config_.budget_ns);
    if(!reserved.ticket||!reserved.ticket->commit())return TriggerStatus::Full;
    const InspectionId id("inspection-"+std::to_string(sequence));
    std::vector<inspection::ExpectedCheck> expected;
    for(std::size_t i=0;i<2;++i) {
        expected_[i]=algorithms_[i]->next_identity(id,CheckId("brightness-"+std::to_string(i)));
        expected.push_back({*expected_[i],true});
    }
    inspection_=std::make_unique<inspection::Inspection>(
        InspectionResult{RunId(run_),id,WorkpieceId("piece-"+std::to_string(sequence)),StationId("demo-station"),Mode::Demo,config_.recipe_hash},
        std::move(expected));
    ticket_=std::move(reserved.ticket);last_trigger_=sequence;terminal_=false;
    for(auto& camera:cameras_) {
        const auto now=clock_->now_ns();
        if(now>=due_||!camera->capture(due_-now)) {terminate(InspectionState::Failed,"CAPTURE.ADMISSION_FAILED");break;}
    }
    return TriggerStatus::Accepted;
}
void Station::terminate(InspectionState state,std::string error) {
    if(!inspection_||terminal_)return;
    inspection_->terminate(state,error);
    if(archive_)archive_->stop();
    for(std::size_t i=0;i<2;++i)if(awaiting_archive_[i]){cameras_[i]->release(*awaiting_archive_[i]);awaiting_archive_[i].reset();}
    for(auto& a:algorithms_)a->cancel();
    for(auto& c:cameras_)c->cancel();
    finish_if_ready();
}
void Station::finish_if_ready() {
    if(!inspection_||terminal_)return;
    if(auto result=inspection_->take_finalized()) {
        terminal_=true;
        if(counter_.add(*result)!=inspection::CountStatus::Counted)throw std::logic_error("Result count invariant");
        const auto utc=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        result_=ResultEnvelope{1,EventId(run_+"-event-"+std::to_string(++events_)),events_,static_cast<std::uint64_t>(utc),std::move(*result)};
        if(result_->result.state!=InspectionState::Completed)production_.fault();
    }
}
void Station::inspect_frame(std::size_t i,FrameDescriptor frame) {
    if(!inspection_||terminal_){cameras_[i]->release(frame);return;}
    const auto now=clock_->now_ns();
    if(now>=due_){cameras_[i]->release(frame);terminate(InspectionState::TimedOut,"WORKPIECE.TIMEOUT");}
    else if(!algorithms_[i]->inspect(frame,expected_[i]->inspection_id,expected_[i]->check_id,due_-now)){
        cameras_[i]->release(frame);terminate(InspectionState::Failed,"ALGORITHM.ADMISSION_FAILED");
    }
}
void Station::pulse() {
    for(auto& c:cameras_)c->pulse();
    for(auto& a:algorithms_)a->pulse();
    if(archive_){
        archive_->pulse();
        if(!archive_->error().empty()&&inspection_&&!terminal_)terminate(InspectionState::Failed,archive_->error());
        if(auto frames=archive_->take())for(std::size_t i=0;i<2;++i)inspect_frame(i,(*frames)[i]);
    }
    if(inspection_&&!terminal_&&clock_->now_ns()>=due_)terminate(InspectionState::TimedOut,"WORKPIECE.TIMEOUT");
    for(std::size_t i=0;i<2;++i) {
        if(auto captured=cameras_[i]->take_completion()) {
            if(!inspection_||terminal_) {if(captured->frame)cameras_[i]->release(*captured->frame);}
            else if(captured->state!="Succeeded"||!captured->frame) {
                terminate(captured->state=="TimedOut"?InspectionState::TimedOut:InspectionState::Failed,captured->error);
            } else {
                if(archive_)awaiting_archive_[i]=*captured->frame;
                else inspect_frame(i,*captured->frame);
            }
        }
        if(auto done=algorithms_[i]->take_completion();done&&inspection_&&!terminal_) {
            CheckResult check=done->evidence.value_or(CheckResult{done->correlation});
            check.state=done->state=="Succeeded"?TaskState::Succeeded:done->state=="TimedOut"?TaskState::TimedOut:
                done->state=="Cancelled"?TaskState::Cancelled:TaskState::Failed;
            check.quality=done->quality=="OK"?QualityVerdict::OK:done->quality=="NG"?QualityVerdict::NG:QualityVerdict::Unknown;
            if(check.state!=TaskState::Succeeded)check.error=Error{done->error,ErrorCategory::Execution,done->error,
                Retryability::Never,"algorithm",done->correlation,{}};
            const auto accepted=inspection_->accept(check);
            if(accepted!=inspection::AcceptStatus::Accepted&&accepted!=inspection::AcceptStatus::Finalized)
                terminate(InspectionState::Failed,"RESULT.IDENTITY_MISMATCH");
            finish_if_ready();
        }
    }
    if(archive_&&inspection_&&!terminal_&&awaiting_archive_[0]&&awaiting_archive_[1]){
        const auto now=clock_->now_ns();
        if(now>=due_)terminate(InspectionState::TimedOut,"WORKPIECE.TIMEOUT");
        else {
            auto frames=std::array{*awaiting_archive_[0],*awaiting_archive_[1]};
            awaiting_archive_[0].reset();awaiting_archive_[1].reset();
            archive_->submit(std::move(frames),expected_[0]->inspection_id.value(),due_-now);
        }
    }
    if(inspection_&&terminal_&&settled()) {
        ticket_->release();ticket_.reset();inspection_.reset();
    }
    if(production_.state()==ProductionState::Starting) {
        if(resources_ready())production_.ready(true);
        else for(const auto& w:workers())if(w.phase==runtime::WorkerPhase::ManualIntervention)production_.fault();
    } else if(production_.accepts()&&!inspection_&&!resources_ready())production_.fault();
    if(stopping_) {
        bool alive=false;for(const auto& w:workers())alive|=w.process_alive;
        if(!alive&&settled())production_.stopped();
    }
}
void Station::pause() {production_.pause();}
bool Station::resume() {return production_.resume(resources_ready()&&!inspection_);}
void Station::cancel() {terminate(InspectionState::Cancelled,"WORKPIECE.CANCELLED");}
void Station::stop() {
    stopping_=true;terminate(InspectionState::Cancelled,"STATION.STOPPED");production_.drain();
    if(archive_)archive_->stop();
    for(auto& a:algorithms_)if(a)a->stop();
    for(auto& c:cameras_)if(c)c->stop();
}
StationSnapshot Station::snapshot() const {
    std::size_t leases=0;for(const auto& c:cameras_)leases+=c->buffers().leases;
    return {production_.state(),production_.accepts()&&!inspection_&&!result_&&!ticket_&&resources_ready()&&counter_.snapshot().total<4096,
        bool(inspection_),counter_.snapshot(),leases,budget_.snapshot().active_tickets,archive_?archive_->staged():0,
        archive_&&archive_->alive(),archive_?archive_->error():""};
}
std::optional<ResultEnvelope> Station::take_result() {auto r=std::move(result_);result_.reset();return r;}
std::array<runtime::WorkerSnapshot,4> Station::workers() const {
    return {cameras_[0]->worker(),cameras_[1]->worker(),algorithms_[0]->worker(),algorithms_[1]->worker()};
}
}
