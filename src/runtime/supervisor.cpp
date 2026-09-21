#include <vision/runtime/supervisor.hpp>
#include <algorithm>

namespace vision::runtime {
namespace {
std::uint64_t after(std::uint64_t now,std::uint64_t duration) {
    if(duration>UINT64_MAX-now)throw std::overflow_error("Supervisor clock overflow");
    return now+duration;
}
}
Supervisor::Supervisor(SupervisorPolicy p):policy_(p) {
    if(!p.stage_ns||!p.heartbeat_ns||!p.progress_ns||!p.stop_ns||!p.backoff_ns||
       p.max_backoff_ns<p.backoff_ns||p.restart_limit>100)
        throw std::invalid_argument("Invalid supervisor policy");
}
bool Supervisor::current(std::uint64_t epoch) const {return epoch==state_.epoch&&epoch!=0;}
void Supervisor::launch(std::uint64_t now) {
    if(state_.epoch==UINT64_MAX) {
        state_.phase=WorkerPhase::ManualIntervention;state_.reason="EpochExhausted";return;
    }
    ++state_.epoch;state_.phase=WorkerPhase::Starting;state_.busy=false;
    heartbeat_seq_=progress_seq_=0;due_=after(now,policy_.stage_ns);
    action_=ProcessAction::Launch;kill_sent_=false;
}
bool Supervisor::start(std::uint64_t now) {
    if(state_.process_alive||action_||(state_.phase!=WorkerPhase::Stopped&&state_.phase!=WorkerPhase::ManualIntervention))
        return false;
    state_.restarts=0;state_.reason.clear();retry_=rotation_=fatal_=false;launch(now);return true;
}
void Supervisor::spawned(std::uint64_t epoch,std::uint64_t now) {
    if(!current(epoch))return;
    state_.process_alive=true;
    if(state_.phase==WorkerPhase::Starting) {
        if(now>=due_) {fault(epoch,"StageTimeout",now);return;}
        state_.phase=WorkerPhase::Handshaking;due_=after(now,policy_.stage_ns);
    }
}
void Supervisor::authenticated(std::uint64_t epoch,std::uint64_t now) {
    if(!current(epoch)||state_.phase!=WorkerPhase::Handshaking)return;
    if(now>=due_) {fault(epoch,"StageTimeout",now);return;}
    state_.phase=WorkerPhase::Initializing;due_=after(now,policy_.stage_ns);
}
void Supervisor::initialized(std::uint64_t epoch,std::uint64_t now) {
    if(!current(epoch)||state_.phase!=WorkerPhase::Initializing)return;
    if(now>=due_) {fault(epoch,"StageTimeout",now);return;}
    state_.phase=WorkerPhase::Ready;
    heartbeat_due_=after(now,policy_.heartbeat_ns);progress_due_=after(now,policy_.progress_ns);
}
void Supervisor::heartbeat(std::uint64_t epoch,std::uint64_t seq,std::uint64_t now) {
    if(!current(epoch)||state_.phase!=WorkerPhase::Ready||seq<=heartbeat_seq_)return;
    if(now>=heartbeat_due_) {fault(epoch,"HeartbeatTimeout",now);return;}
    heartbeat_seq_=seq;heartbeat_due_=after(now,policy_.heartbeat_ns);
}
void Supervisor::progress(std::uint64_t epoch,std::uint64_t seq,std::uint64_t now) {
    if(!current(epoch)||state_.phase!=WorkerPhase::Ready||!state_.busy||seq<=progress_seq_)return;
    if(now>=progress_due_) {fault(epoch,"ProgressTimeout",now);return;}
    progress_seq_=seq;progress_due_=after(now,policy_.progress_ns);
}
bool Supervisor::set_busy(std::uint64_t epoch,bool busy,std::uint64_t now) {
    if(!current(epoch)||state_.phase!=WorkerPhase::Ready)return false;
    if(busy&&!state_.busy) {progress_seq_=0;progress_due_=after(now,policy_.progress_ns);}
    state_.busy=busy;return true;
}
void Supervisor::stopping(std::uint64_t now,bool retry,bool graceful) {
    retry_=retry;state_.phase=WorkerPhase::Stopping;due_=after(now,policy_.stop_ns);
    action_=graceful?ProcessAction::Stop:ProcessAction::Kill;kill_sent_=!graceful;
}
void Supervisor::fault(std::uint64_t epoch,std::string reason,std::uint64_t now,bool recoverable) {
    if(!current(epoch)||state_.phase==WorkerPhase::Stopped||state_.phase==WorkerPhase::Backoff||
       state_.phase==WorkerPhase::ManualIntervention||state_.phase==WorkerPhase::Stopping)return;
    state_.reason=std::move(reason);rotation_=false;fatal_=!recoverable;stopping(now,true,false);
}
void Supervisor::exited(std::uint64_t epoch,std::uint64_t now,bool clean) {
    if(!current(epoch)||state_.phase==WorkerPhase::Stopped||state_.phase==WorkerPhase::Backoff||
       state_.phase==WorkerPhase::ManualIntervention)return;
    if(state_.phase!=WorkerPhase::Stopping) {retry_=true;rotation_=false;state_.reason="UnexpectedExit";}
    if(rotation_&&!clean) {rotation_=false;state_.reason="RotationExitFailure";}
    state_.process_alive=false;state_.busy=false;action_.reset();
    if(fatal_) {state_.phase=WorkerPhase::ManualIntervention;return;}
    if(!retry_) {state_.phase=WorkerPhase::Stopped;return;}
    if(!rotation_&&state_.restarts>=policy_.restart_limit) {
        state_.phase=WorkerPhase::ManualIntervention;return;
    }
    if(!rotation_)++state_.restarts;
    auto delay=policy_.backoff_ns;
    for(unsigned n=1;n<state_.restarts&&delay<policy_.max_backoff_ns;++n)
        delay=delay>policy_.max_backoff_ns/2?policy_.max_backoff_ns:delay*2;
    due_=after(now,delay);state_.phase=WorkerPhase::Backoff;
}
void Supervisor::stop(std::uint64_t now) {
    retry_=rotation_=false;
    if(state_.phase==WorkerPhase::Stopped||state_.phase==WorkerPhase::ManualIntervention)return;
    if(state_.phase==WorkerPhase::Backoff||(state_.phase==WorkerPhase::Starting&&action_==ProcessAction::Launch)) {
        action_.reset();state_.phase=WorkerPhase::Stopped;return;
    }
    if(state_.phase!=WorkerPhase::Stopping)stopping(now,false,true);
}
bool Supervisor::rotate(std::uint64_t now) {
    if(state_.phase!=WorkerPhase::Ready||state_.busy)return false;
    rotation_=true;state_.reason="SessionRotation";stopping(now,true,true);return true;
}
std::optional<ProcessAction> Supervisor::poll(std::uint64_t now) {
    if(!action_) {
        switch(state_.phase) {
        case WorkerPhase::Starting:case WorkerPhase::Handshaking:case WorkerPhase::Initializing:
            if(now>=due_)fault(state_.epoch,"StageTimeout",now);
            break;
        case WorkerPhase::Ready:
            if(now>=heartbeat_due_)fault(state_.epoch,"HeartbeatTimeout",now);
            else if(state_.busy&&now>=progress_due_)fault(state_.epoch,"ProgressTimeout",now);
            break;
        case WorkerPhase::Stopping:
            if(now>=due_&&!kill_sent_) {
                if(rotation_) {rotation_=false;state_.reason="RotationStopTimeout";}
                action_=ProcessAction::Kill;kill_sent_=true;
            }
            break;
        case WorkerPhase::Backoff:if(now>=due_)launch(now);break;
        default:break;
        }
    }
    auto result=action_;action_.reset();return result;
}
ControlLease::ControlLease(std::string run,std::uint64_t epoch,std::uint64_t ttl)
    :run_(std::move(run)),epoch_(epoch),ttl_(ttl) {
    if(run_.empty()||!epoch_||!ttl_)throw std::invalid_argument("Invalid control lease");
}
bool ControlLease::valid(std::uint64_t now) {
    if(started_&&now>=due_)revoked_=true;
    return started_&&!revoked_;
}
bool ControlLease::renew(const std::string& run,std::uint64_t epoch,std::uint64_t seq,std::uint64_t now) {
    valid(now);
    if(revoked_||run!=run_||epoch!=epoch_||seq<=sequence_)return false;
    due_=after(now,ttl_);sequence_=seq;started_=true;return true;
}
} // namespace vision::runtime
