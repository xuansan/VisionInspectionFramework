#include <vision/runtime/scheduler.hpp>

namespace vision::runtime {
using contracts::TaskState;
TaskScheduler::TaskScheduler(contracts::RunId run,std::size_t capacity,ResourceBudget& budget,std::shared_ptr<Clock> clock)
    :run_(std::move(run)),capacity_(capacity),budget_(budget),clock_(std::move(clock)) {
    if(!capacity||capacity>65536||!clock_)throw std::invalid_argument("Invalid scheduler limits");
}
Admission TaskScheduler::enqueue(contracts::WorkerId worker,std::uint64_t epoch,contracts::InspectionId inspection,
    contracts::CheckId check,const ResourceAmounts& resources,std::uint64_t duration) {
    tick();
    if(!epoch||!duration||duration>UINT64_MAX-clock_->now_ns()||next_id_==UINT64_MAX)
        return {AdmissionStatus::Invalid,{}};
    if(entries_.size()>=capacity_)return {AdmissionStatus::Full,{}};
    const auto due=deadline_after(*clock_,duration);
    auto reserved=budget_.try_reserve(resources,duration);
    if(reserved.status!=ReserveStatus::Accepted)return {AdmissionStatus::NoResources,{}};
    if(!reserved.ticket->commit())return {AdmissionStatus::NoResources,{}};
    const auto id=next_id_+1;
    contracts::Correlation identity{run_,std::move(worker),epoch,std::move(inspection),std::move(check),
        contracts::TaskId("task-"+std::to_string(id)),1};
    entries_.emplace(id,Entry{identity,due,std::move(*reserved.ticket)});
    next_id_=id;
    record(entries_.at(id),"Admitted");
    return {AdmissionStatus::Accepted,std::move(identity)};
}
TaskScheduler::Entry* TaskScheduler::find(const contracts::Correlation& identity) {
    // Bounded by capacity, compares every identity field rather than trusting just task_id.
    for(auto& [id,e]:entries_) { (void)id;if(e.identity==identity)return &e; }
    return nullptr;
}
void TaskScheduler::terminate(Entry& e,TaskState state) {
    if(contracts::terminal(e.state))return;
    e.state=state;
    if(e.execution_stopped)e.reservation.release();
    else e.cancel_pending=true;
    record(e,"Terminal");
}
void TaskScheduler::collect() {
    for(auto it=entries_.begin();it!=entries_.end();) {
        if(it->second.reported&&it->second.execution_stopped)it=entries_.erase(it);
        else ++it;
    }
}
void TaskScheduler::tick() {
    const auto now=clock_->now_ns();
    for(auto& [id,e]:entries_) { (void)id;if(now>=e.deadline)terminate(e,TaskState::TimedOut); }
    collect();
}
std::optional<Dispatch> TaskScheduler::dispatch(const contracts::WorkerId& worker,std::uint64_t epoch) {
    tick();
    for(const auto& [id,e]:entries_) {
        (void)id;
        if(e.identity.worker_id==worker&&!e.execution_stopped)return {};
    }
    for(auto& [id,e]:entries_) {
        (void)id;
        if(e.state==TaskState::Queued&&e.identity.worker_id==worker&&e.identity.worker_epoch==epoch) {
            const auto now=clock_->now_ns();
            if(now>=e.deadline) {terminate(e,TaskState::TimedOut);continue;}
            e.state=TaskState::Running;e.execution_stopped=false;
            record(e,"Dispatched");
            return Dispatch{e.identity,e.deadline-now};
        }
    }
    return {};
}
bool TaskScheduler::finish(const contracts::Correlation& identity,TaskState result) {
    if(!contracts::terminal(result))return false;
    tick();auto e=find(identity);
    if(!e||e->execution_stopped)return false;
    const bool accepted=!contracts::terminal(e->state);
    // Even a late terminal proves this particular execution stopped, but cannot change its verdict.
    e->execution_stopped=true;e->cancel_pending=false;e->reservation.release();
    if(accepted)terminate(*e,result);
    else record(*e,"LateExecutionStopped");
    collect();return accepted;
}
bool TaskScheduler::cancel(const contracts::Correlation& identity) {
    tick();auto e=find(identity);
    if(!e||contracts::terminal(e->state))return false;
    terminate(*e,TaskState::Cancelled);return true;
}
bool TaskScheduler::dispatch_rejected(const contracts::Correlation& identity) {
    tick();
    auto e=find(identity);
    if(!e||e->execution_stopped)return false;
    e->execution_stopped=true;e->cancel_pending=false;e->reservation.release();
    if(contracts::terminal(e->state))record(*e,"UnsentExecutionStopped");
    else terminate(*e,TaskState::Failed);
    collect();return true;
}
void TaskScheduler::worker_exited(const contracts::WorkerId& worker,std::uint64_t epoch) {
    tick();
    for(auto& [id,e]:entries_) {
        (void)id;
        if(e.identity.worker_id==worker&&e.identity.worker_epoch==epoch) {
            e.execution_stopped=true;e.cancel_pending=false;e.reservation.release();
            if(contracts::terminal(e.state))record(e,"WorkerExited");
            else terminate(e,TaskState::Failed);
        }
    }
    collect();
}
std::optional<contracts::Correlation> TaskScheduler::take_cancel() {
    tick();
    for(auto& [id,e]:entries_) {
        (void)id;
        if(e.cancel_pending) {e.cancel_pending=false;return e.identity;}
    }
    return {};
}
std::optional<TaskTerminal> TaskScheduler::take_terminal() {
    tick();
    for(auto& [id,e]:entries_) {
        (void)id;
        if(contracts::terminal(e.state)&&!e.reported) {
            auto result=TaskTerminal{e.identity,e.state};e.reported=true;collect();return result;
        }
    }
    return {};
}
SchedulerSnapshot TaskScheduler::snapshot() const {
    SchedulerSnapshot result;result.retained=entries_.size();
    for(const auto& [id,e]:entries_) {
        (void)id;
        if(e.state==TaskState::Queued)++result.queued;
        if(e.state==TaskState::Running)++result.running;
        if(contracts::terminal(e.state)) {
            if(!e.execution_stopped)++result.awaiting_stop;
            if(!e.reported)++result.unconsumed_terminals;
        }
    }
    return result;
}
void TaskScheduler::record(const Entry& entry,std::string reason) {
    if(events_.size()==256) {events_.pop_front();if(dropped_events_!=UINT64_MAX)++dropped_events_;}
    events_.push_back({clock_->now_ns(),entry.identity,entry.state,std::move(reason)});
}
std::vector<TaskEvent> TaskScheduler::take_events() {
    std::vector<TaskEvent> result(events_.begin(),events_.end());events_.clear();return result;
}
} // namespace vision::runtime
