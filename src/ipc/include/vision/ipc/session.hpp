#pragma once
#include <algorithm>
#include <vision/contracts/types.hpp>
#include <vision/ipc/framing.hpp>
#include <deque>
#include <map>
#include <set>

namespace vision::ipc {
struct Header {
    unsigned major{1},minor{};
    std::string type,request_id,run_id,worker_id;
    std::uint64_t epoch{},sequence{};
};
enum class SessionStatus { Accepted, NotAuthenticated, VersionMismatch, IdentityMismatch, SequenceMismatch, UnknownType, Closed };
// Peer identity is the worker session in both directions, never the sender's role.
class Session {
public:
    Session(std::string run,std::string worker,std::uint64_t epoch,std::string token,std::uint64_t deadline)
        : run_(std::move(run)),worker_(std::move(worker)),token_(std::move(token)),epoch_(epoch),deadline_(deadline) {
        (void)contracts::RunId(run_); (void)contracts::WorkerId(worker_);
        if(!epoch_ || token_.size()<32 || token_.size()>128) throw std::invalid_argument("Invalid session credentials");
    }
    SessionStatus hello(const Header& h,std::string_view token,const std::set<std::string>& required,std::uint64_t now) {
        if(closed_) return SessionStatus::Closed;
        if(authenticated_ || now>=deadline_ || token!=token_ || h.type!="Hello" || h.sequence!=1 ||
           (!std::all_of(required.begin(),required.end(),[](const auto& c){return c=="capture-v1"||c=="inspect-v1"||c=="device-v1"||c=="delivery-v1"||c=="inference-v1";}))) {close();return SessionStatus::NotAuthenticated;}
        const auto status=validate(h);
        if(status!=SessionStatus::Accepted) {close();return status;}
        authenticated_=true; last_=1; token_.clear(); return SessionStatus::Accepted;
    }
    SessionStatus receive(const Header& h) {
        if(closed_) return SessionStatus::Closed;
        if(!authenticated_) {close();return SessionStatus::NotAuthenticated;}
        const auto status=validate(h);
        if(status!=SessionStatus::Accepted) {close();return status;}
        if(h.type=="Hello" || last_==UINT64_MAX || h.sequence!=last_+1) {close();return SessionStatus::SequenceMismatch;}
        last_=h.sequence;return SessionStatus::Accepted;
    }
    void close() noexcept {closed_=true;token_.clear();}
    bool authenticated() const noexcept {return authenticated_&&!closed_;}
private:
    SessionStatus validate(const Header& h) const {
        if(h.major!=1 || h.minor!=0) return SessionStatus::VersionMismatch;
        if(h.run_id!=run_ || h.worker_id!=worker_ || h.epoch!=epoch_) return SessionStatus::IdentityMismatch;
        static const std::set<std::string> types={"Hello","Configure","Ready","SubmitTask","TaskAccepted","TaskProgress",
            "TaskFinished","Cancel","Heartbeat","Fault","Drain","Stop","LeaseRelease","ResultEvent","DeliveryReport",
            "CaptureTask","CaptureFinished","InspectTask","DeviceTask","DeviceFinished","DeliveryTask","DeliveryFinished","CheckFinished"};
        if(!types.contains(h.type)) return SessionStatus::UnknownType;
        try {(void)contracts::TaskId(h.request_id);} catch(const std::invalid_argument&) {return SessionStatus::IdentityMismatch;}
        return SessionStatus::Accepted;
    }
    std::string run_,worker_,token_;
    std::uint64_t epoch_,deadline_,last_{};
    bool authenticated_{},closed_{};
};

struct Outbound { std::string type,payload; };
// Queue complete *unframed* messages; assign sequence when dequeued so control
// prioritization cannot reorder already assigned wire sequence numbers.
class Outbox {
public:
    Outbox(std::size_t normal_count,std::size_t normal_bytes,std::size_t control_count,std::size_t control_bytes)
        : normal_{normal_count,normal_bytes},control_{control_count,control_bytes} {
        if(!normal_count||!normal_bytes||!control_count||!control_bytes) throw std::invalid_argument("Positive outbox limits required");
    }
    bool try_push(Outbound message) {
        if(closed_) return false;
        if(message.type.empty() || message.type.size()>64) return false;
        auto& lane=is_control(message.type)?control_:normal_;
        if(message.payload.empty() || message.payload.size()>1048576 ||
           lane.messages.size()>=lane.max_count || message.payload.size()>lane.max_bytes-lane.bytes) return false;
        const auto size=message.payload.size();
        lane.messages.push_back(std::move(message));lane.bytes+=size;return true;
    }
    std::optional<Outbound> pop() {
        auto& lane=control_.messages.empty()?normal_:control_;
        if(lane.messages.empty()) return {};
        auto message=std::move(lane.messages.front());lane.messages.pop_front();lane.bytes-=message.payload.size();return message;
    }
    void close() {closed_=true;normal_.messages.clear();control_.messages.clear();normal_.bytes=control_.bytes=0;}
    std::size_t bytes() const {return normal_.bytes+control_.bytes;}
private:
    static bool is_control(std::string_view type) {return type=="Heartbeat"||type=="Stop"||type=="Cancel"||type=="Fault"||type=="Drain";}
    struct Lane {
        std::size_t max_count,max_bytes,bytes{};
        std::deque<Outbound> messages;
    };
    Lane normal_,control_;
    bool closed_{};
};
class PendingRequests {
public:
    explicit PendingRequests(std::size_t limit):limit_(limit) {if(!limit)throw std::invalid_argument("Positive pending limit");}
    bool add(std::string id,std::uint64_t deadline,std::uint64_t now) {
        (void)contracts::TaskId(id);
        if(closed_ || deadline<=now || entries_.size()>=limit_)return false;
        return entries_.emplace(std::move(id),deadline).second;
    }
    bool complete(const std::string& id) {return entries_.erase(id)!=0;}
    std::vector<std::string> expire(std::uint64_t now) {
        std::vector<std::string> ids;
        for(const auto& [id,deadline]:entries_) if(now>=deadline)ids.push_back(id);
        for(const auto& id:ids)entries_.erase(id);
        return ids;
    }
    std::vector<std::string> disconnect() {
        std::vector<std::string> ids;for(const auto& [id,deadline]:entries_){(void)deadline;ids.push_back(id);}
        entries_.clear();closed_=true;return ids; // Outcome unknown; never imply "not executed".
    }
private:
    std::size_t limit_;bool closed_{};
    std::map<std::string,std::uint64_t> entries_;
};
} // namespace vision::ipc
