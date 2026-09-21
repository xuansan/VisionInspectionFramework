#include <vision/ipc/channel.hpp>
#include <algorithm>
#include <limits>
#include <nlohmann/json.hpp>

namespace vision::ipc {
namespace {
std::uint64_t deadline(std::uint64_t now,std::uint64_t duration) {
    if(!duration||duration>UINT64_MAX-now)throw std::invalid_argument("Invalid IPC deadline");
    return now+duration;
}
bool request_type(std::string_view t) {return t=="Configure"||t=="SubmitTask"||t=="CaptureTask"||t=="InspectTask"||t=="DeviceTask"||t=="DeliveryTask";}
bool response_type(std::string_view t) {return t=="Ready"||t=="TaskAccepted"||t=="TaskProgress"||t=="TaskFinished"||t=="CaptureFinished"||t=="DeviceFinished"||t=="DeliveryFinished"||t=="CheckFinished";}
bool response_matches(const Message& request,const Message& response) {
    if(request.correlation!=response.correlation)return false;
    if(request.header.type=="Configure")return response.header.type=="Ready"&&request.payload==response.payload;
    if(request.header.type=="CaptureTask") {
        if(response.header.type=="TaskAccepted")return true;
        if(response.header.type!="CaptureFinished")return false;
        const auto result=nlohmann::json::parse(response.payload);
        return result["execution_state"]!="Succeeded"||
            result["frame"]==nlohmann::json::parse(request.payload)["write_frame"];
    }
    if(request.header.type=="DeliveryTask") {
        if(response.header.type=="TaskAccepted")return true;
        if(response.header.type!="DeliveryFinished")return false;
        const auto a=nlohmann::json::parse(request.payload),b=nlohmann::json::parse(response.payload);
        return a["event"]["event_id"]==b["event_id"]&&a["output_instance_id"]==b["output_instance_id"]&&a["attempt"]==b["attempt"];
    }
    if(request.header.type=="DeviceTask") {
        if(response.header.type=="TaskAccepted")return true;
        if(response.header.type!="DeviceFinished")return false;
        const auto a=nlohmann::json::parse(request.payload),b=nlohmann::json::parse(response.payload);
        return a["session_id"]==b["session_id"]&&a["command_sequence"]==b["command_sequence"];
    }
    if(request.header.type=="InspectTask"&&response.header.type=="CheckFinished")return true;
    return (request.header.type=="SubmitTask"||request.header.type=="InspectTask") && (response.header.type=="TaskAccepted" ||
        response.header.type=="TaskProgress" || response.header.type=="TaskFinished");
}
Limits checked(Limits l) {
    if(!l.max_payload||l.max_payload>1048576||!l.normal_count||l.normal_count>4096||
       !l.control_count||l.control_count>4096||!l.receive_count||l.receive_count>4096||
       !l.pending_count||l.pending_count>4096||!l.session_requests||l.session_requests>65536||
       !l.normal_bytes||l.normal_bytes>64*1048576||l.control_bytes<1024||l.control_bytes>64*1048576||
       !l.receive_bytes||l.receive_bytes>64*1048576||
       !l.handshake_ns||!l.partial_ns||!l.request_ns)throw std::invalid_argument("Invalid IPC limits");
    return l;
}
}
Channel::Channel(std::string run,std::string worker,std::uint64_t epoch,std::string token,bool initiator,
                 Limits limits,std::uint64_t now)
    : run_(std::move(run)),worker_(std::move(worker)),token_(std::move(token)),epoch_(epoch),
      handshake_deadline_(deadline(now,limits.handshake_ns)),initiator_(initiator),limits_(checked(limits)),
      decoder_(limits_.max_payload,limits_.receive_count),
      session_(run_,worker_,epoch_,token_,handshake_deadline_) {
    if(initiator_) {
        Message hello{{1,0,"Hello","hello",run_,worker_,epoch_,1},{},hello_payload(token_)};
        if(enqueue(std::move(hello),false)!=SendStatus::Accepted)throw std::invalid_argument("Hello exceeds channel limits");
    }
}
std::string Channel::local_id(std::uint64_t n) const {return std::string(initiator_?"c-":"s-")+std::to_string(n);}
SendStatus Channel::enqueue(Message message,bool request) {
    message.header.sequence=UINT64_MAX; // Budget for the longest possible decimal sequence.
    try {
        auto canonical=encode_message(message);
        message=decode_message(canonical);
        if(canonical.size()>limits_.max_payload)return SendStatus::Invalid;
        const auto cost=canonical.size()+4;
        const bool control=control_type(message.header.type);
        auto& lane=control?control_:normal_;
        const auto count_limit=control?limits_.control_count:limits_.normal_count;
        const auto byte_limit=control?limits_.control_bytes:limits_.normal_bytes;
        if(lane.queue.size()>=count_limit || cost>byte_limit-lane.bytes)return SendStatus::Full;
        lane.queue.push_back({std::move(message),cost,request});
        lane.bytes+=cost;
        return SendStatus::Accepted;
    } catch(const ProtocolError&) {return SendStatus::Invalid;}
}
SendResult Channel::send(std::string type,std::string payload,std::optional<contracts::Correlation> correlation,
                         std::uint64_t now,std::uint64_t timeout_ns) {
    tick(now);
    if(closed_)return {SendStatus::Closed,{}};
    if(!session_.authenticated())return {SendStatus::NotReady,{}};
    if(type=="Hello"||response_type(type))return {SendStatus::Invalid,{}};
    const bool request=request_type(type);
    if(next_id_==UINT64_MAX || (request&&(issued_requests_>=limits_.session_requests ||
       pending_.size()>=limits_.pending_count)))return {SendStatus::Full,{}};
    const auto id=local_id(next_id_+1);
    Message message{{1,0,std::move(type),id,run_,worker_,epoch_,1},std::move(correlation),std::move(payload)};
    std::uint64_t due{};
    try {if(request)due=deadline(now,timeout_ns?timeout_ns:limits_.request_ns);}
    catch(const std::invalid_argument&){return {SendStatus::Invalid,{}};}
    // Store the canonical request for matching configuration replies.
    try {message=decode_message(encode_message(message));}catch(const ProtocolError&){return {SendStatus::Invalid,{}};}
    if(request)pending_.emplace(id,Pending{message,due});
    SendStatus result;
    try {result=enqueue(std::move(message),request);}
    catch(...) {if(request)pending_.erase(id);throw;}
    if(result!=SendStatus::Accepted) {if(request)pending_.erase(id);return {result,{}};}
    ++next_id_;
    if(request)++issued_requests_;
    return {result,id};
}
SendStatus Channel::reply(const Message& request,std::string type,std::string payload,std::uint64_t now) {
    tick(now);
    if(closed_)return SendStatus::Closed;
    if(!session_.authenticated())return SendStatus::NotReady;
    if(!received_requests_.contains(request.header.request_id) || request.header.run_id!=run_ ||
       request.header.worker_id!=worker_ || request.header.epoch!=epoch_)return SendStatus::Invalid;
    Message response{{1,0,std::move(type),request.header.request_id,run_,worker_,epoch_,1},request.correlation,std::move(payload)};
    try {response=decode_message(encode_message(response));}
    catch(const ProtocolError&){return SendStatus::Invalid;}
    if(!response_matches(request,response))return SendStatus::Invalid;
    return enqueue(std::move(response),false);
}
void Channel::event(std::string id,std::string reason) {
    if(events_.size()>=limits_.pending_count*2) {fail("DiagnosticQueueFull");return;}
    events_.push_back({std::move(id),std::move(reason)});
}
void Channel::fail(std::string reason) {
    if(closed_)return;
    closed_=true;failure_=std::move(reason);session_.close();token_.clear();
    // Bounded final events replace an overflowing diagnostic queue.
    if(events_.size()+pending_.size()>limits_.pending_count*3)events_.clear();
    for(const auto& [id,p]:pending_) {(void)p;events_.push_back({id,"DisconnectedUnknown"});}
    pending_.clear();normal_={};control_={};received_.clear();received_bytes_=0;
}
void Channel::disconnect(std::string reason) {
    try {decoder_.finish();}catch(const FrameError&) {reason="TruncatedFrame";}
    fail(std::move(reason));
}
void Channel::tick(std::uint64_t now) {
    if(closed_)return;
    if(!session_.authenticated()&&now>=handshake_deadline_) {fail("HandshakeTimeout");return;}
    if(partial_started_ && now-*partial_started_>=limits_.partial_ns) {fail("PartialFrameTimeout");return;}
    std::vector<std::string> expired;
    for(const auto& [id,p]:pending_)if(!p.terminal_received&&now>=p.deadline)expired.push_back(id);
    for(const auto& id:expired) {
        // Overflow closes the channel; keep ownership until the notification is queued.
        event(id,"Timeout");
        if(closed_)return;
        pending_.erase(id);
    }
}
void Channel::receive(Message message,std::size_t cost,std::uint64_t now) {
    if(!session_.authenticated()) {
        if(message.header.type!="Hello") {fail("ExpectedHello");return;}
        const auto hello=read_hello(message);
        if(session_.hello(message.header,hello.token,hello.required,now)!=SessionStatus::Accepted) {fail("HandshakeRejected");return;}
        if(!initiator_) {
            Message reply{{1,0,"Hello","hello",run_,worker_,epoch_,1},{},hello_payload(token_)};
            if(enqueue(std::move(reply),false)!=SendStatus::Accepted) {fail("HandshakeSendFull");return;}
        }
        token_.clear();
        return;
    }
    if(session_.receive(message.header)!=SessionStatus::Accepted) {fail("SessionRejected");return;}
    if(response_type(message.header.type)) {
        const auto it=pending_.find(message.header.request_id);
        if(it==pending_.end()) {
            const auto& id=message.header.request_id;
            const std::string prefix=initiator_?"c-":"s-";
            if(!id.starts_with(prefix)) {fail("UnknownResponse");return;}
            try {const auto n=contracts::parse_u64(std::string_view(id).substr(2));
                if(!n||n>next_id_) {fail("UnknownResponse");return;}}
            catch(const std::invalid_argument&) {fail("UnknownResponse");return;}
            event(id,"LateResponse");
            return;
        }
        if(!response_matches(it->second.request,message)) {fail("ResponseMismatch");return;}
        if(it->second.terminal_received) {event(message.header.request_id,"LateResponse");return;}
    } else if(request_type(message.header.type)) {
        if(received_requests_.size()>=limits_.session_requests ||
           !received_requests_.insert(message.header.request_id).second) {fail("DuplicateOrExcessRequest");return;}
    }
    if(received_.size()>=limits_.receive_count || cost>limits_.receive_bytes-received_bytes_) {fail("ReceiveQueueFull");return;}
    if(message.header.type=="Ready"||message.header.type=="TaskFinished"||message.header.type=="CaptureFinished"||message.header.type=="DeviceFinished"||message.header.type=="DeliveryFinished"||message.header.type=="CheckFinished")
        pending_.at(message.header.request_id).terminal_received=true;
    received_.emplace_back(std::move(message),cost);received_bytes_+=cost;
}
void Channel::feed(std::string_view bytes,std::uint64_t now) {
    tick(now);
    if(closed_)return;
    try {
        // Per-byte framing preserves the original start time even when a complete
        // frame is followed by a partial next frame in the same read.
        for(const char c:bytes) {
            if(!decoder_.partial())partial_started_=now;
            decoder_.feed(std::string_view(&c,1));
            if(decoder_.available()) {
                const auto json=decoder_.pop();
                partial_started_.reset();
                receive(decode_message(json),json.size()+4,now);
                if(closed_)return;
            }
        }
    } catch(const FrameError&) {fail("InvalidFrame");}
      catch(const ProtocolError&) {fail("InvalidMessage");}
}
std::optional<std::string> Channel::next_wire(std::uint64_t now) {
    tick(now);
    if(closed_)return {};
    for(;;) {
        auto& lane=control_.queue.empty()?normal_:control_;
        if(lane.queue.empty())return {};
        auto queued=std::move(lane.queue.front());lane.queue.pop_front();lane.bytes-=queued.cost;
        if(queued.request&&!pending_.contains(queued.message.header.request_id))continue;
        if(sent_sequence_==UINT64_MAX) {fail("SequenceExhausted");return {};}
        queued.message.header.sequence=++sent_sequence_;
        return frame(encode_message(queued.message),limits_.max_payload);
    }
}
std::optional<Message> Channel::pop() {
    if(received_.empty())return {};
    auto item=std::move(received_.front());received_.pop_front();received_bytes_-=item.second;
    if(item.first.header.type=="Ready"||item.first.header.type=="TaskFinished"||item.first.header.type=="CaptureFinished"||item.first.header.type=="DeviceFinished"||item.first.header.type=="DeliveryFinished"||item.first.header.type=="CheckFinished")
        pending_.erase(item.first.header.request_id);
    return std::move(item.first);
}
std::vector<RequestEvent> Channel::take_events() {
    auto result=std::move(events_);events_.clear();return result;
}
ChannelSnapshot Channel::snapshot() const {
    return {session_.authenticated(),closed_,normal_.bytes+control_.bytes,normal_.queue.size()+control_.queue.size(),
        received_bytes_,pending_.size(),failure_,issued_requests_>=limits_.session_requests||
        received_requests_.size()>=limits_.session_requests};
}
} // namespace vision::ipc
