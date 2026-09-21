#include <vision/ipc/message.hpp>
#include <nlohmann/json.hpp>
#include <vision/serialization/json_codec.hpp>
#include <cmath>

namespace vision::ipc {
namespace {
using J=nlohmann::json;
J parse(std::string_view text) {
    if(text.empty() || text.size()>1048576) throw ProtocolError("IPC document limit");
    std::vector<std::set<std::string>> keys;
    return J::parse(text.begin(),text.end(),[&](int depth,J::parse_event_t event,J& value) {
        if(depth>32) throw ProtocolError("IPC nesting limit");
        if(event==J::parse_event_t::object_start) keys.emplace_back();
        if(event==J::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second)
            throw ProtocolError("Duplicate IPC key");
        if(event==J::parse_event_t::object_end) keys.pop_back();
        return true;
    });
}
void fields(const J& j,std::initializer_list<const char*> required) {
    if(!j.is_object() || j.size()!=required.size()) throw ProtocolError("IPC object fields mismatch");
    for(const auto* key:required) if(!j.contains(key)) throw ProtocolError("Missing IPC field");
}
std::string text(const J& j,std::size_t max=128) {
    if(!j.is_string()) throw ProtocolError("IPC string expected");
    auto s=j.get<std::string>();
    if(s.empty() || s.size()>max || s.find('\0')!=std::string::npos) throw ProtocolError("IPC string bounds");
    return s;
}
std::string id(const J& j) {auto s=text(j);(void)contracts::TaskId(s);return s;}
std::uint64_t u64(const J& j,bool positive=false) {
    const auto value=contracts::parse_u64(text(j,20));
    if(positive&&!value) throw ProtocolError("Positive IPC integer required");
    return value;
}
unsigned integer(const J& j,unsigned max) {
    if(!j.is_number_unsigned() && !(j.is_number_integer()&&j.get<std::int64_t>()>=0)) throw ProtocolError("IPC integer expected");
    const auto value=j.get<std::uint64_t>();
    if(value>max)throw ProtocolError("IPC integer bounds");
    return static_cast<unsigned>(value);
}
std::string choice(const J& j,std::initializer_list<std::string_view> allowed) {
    const auto s=text(j);
    for(auto item:allowed)if(item==s)return s;
    throw ProtocolError("Unknown IPC enum");
}
void hash(const J& j) {if(!contracts::valid_hash(text(j)))throw ProtocolError("IPC hash format");}
void correlation_payload(const Message& m) {
    if(!m.correlation)throw ProtocolError("Task correlation required");
}
void check_payload(const Message& m,const J& p) {
    const auto& t=m.header.type;
    if(t=="Hello") {
        fields(p,{"token","required_capabilities"});
        if(text(p["token"],128).size()<32)throw ProtocolError("Invalid launch credential");
        if(!p["required_capabilities"].is_array() || p["required_capabilities"].size()>32)throw ProtocolError("Capability limit");
        std::set<std::string> seen;
        for(const auto& cap:p["required_capabilities"])if(!seen.insert(id(cap)).second)throw ProtocolError("Duplicate capability");
    } else if(t=="Configure") {
        fields(p,{"recipe_hash","config_revision"});hash(p["recipe_hash"]);u64(p["config_revision"],true);
    } else if(t=="Ready") {
        fields(p,{"recipe_hash","config_revision"});hash(p["recipe_hash"]);u64(p["config_revision"],true);
    } else if(t=="SubmitTask") {
        correlation_payload(m);
        fields(p,{"operation","input_ref","budget_ns"});
        choice(p["operation"],{"Inspect","Capture","Persist"});
        id(p["input_ref"]);u64(p["budget_ns"],true);
    } else if(t=="CaptureTask"||t=="InspectTask") {
        correlation_payload(m);
        const auto key=t=="CaptureTask"?"write_frame":"read_frame";
        if(t=="CaptureTask")fields(p,{"slot_count","slot_bytes","write_frame","budget_ns"});
        else fields(p,{"slot_count","slot_bytes","read_frame","budget_ns"});
        const auto count=integer(p["slot_count"],4096);
        const auto bytes=u64(p["slot_bytes"],true);
        if(!count||bytes>1024ULL*1024*1024||contracts::checked_mul(count,bytes)>4ULL*1024*1024*1024)
            throw ProtocolError("Capture pool limits");
        u64(p["budget_ns"],true);
        const auto frame=serialization::decode_frame(p[key].dump(),bytes);
        if(frame.permit.slot>=count||frame.permit.run.value()!=m.header.run_id||
           frame.permit.owner.worker.value()!=m.header.worker_id||frame.permit.owner.epoch!=m.header.epoch)
            throw ProtocolError("Capture frame identity");
    } else if(t=="CaptureFinished") {
        correlation_payload(m);
        fields(p,{"execution_state","frame","error_code"});
        const auto state=choice(p["execution_state"],{"Succeeded","Failed","Cancelled","TimedOut"});
        if(state=="Succeeded") {
            if(!p["error_code"].is_null())throw ProtocolError("Successful capture error");
            const auto frame=serialization::decode_frame(p["frame"].dump(),1024ULL*1024*1024);
            if(frame.permit.run.value()!=m.header.run_id||frame.permit.owner.worker.value()!=m.header.worker_id||
               frame.permit.owner.epoch!=m.header.epoch)throw ProtocolError("Capture response identity");
        } else {
            if(!p["frame"].is_null())throw ProtocolError("Failed capture frame");
            id(p["error_code"]);
        }
    } else if(t=="DeviceTask"||t=="DeviceFinished") {
        correlation_payload(m);
        if(t=="DeviceTask") {
            fields(p,{"session_id","command_sequence","ready","arrival","position","safety_ok","result_id","quality"});
            if(!p["arrival"].is_boolean())throw ProtocolError("Device arrival boolean");
            const auto q=choice(p["quality"],{"Unknown","OK","NG"});
            if(p["result_id"].is_null()) {if(q!="Unknown")throw ProtocolError("Empty device result quality");}
            else {id(p["result_id"]);if(q=="Unknown")throw ProtocolError("Unknown physical result");}
        } else {
            fields(p,{"session_id","command_sequence","ready","arrival_sequence","position","safety_ok","ack_id"});
            u64(p["arrival_sequence"]);if(!p["ack_id"].is_null())id(p["ack_id"]);
        }
        id(p["session_id"]);u64(p["command_sequence"],true);
        for(const auto* field:{"ready","position","safety_ok"})if(!p[field].is_boolean())throw ProtocolError("Device boolean");
    } else if(t=="CheckFinished") {
        correlation_payload(m);fields(p,{"check"});
        const auto result=serialization::decode_check(p["check"].dump());
        if(result.correlation!=m.correlation||result.defects.size()>64)throw ProtocolError("Check identity/bounds");
    } else if(t=="TaskAccepted") {
        correlation_payload(m);fields(p,{});
    } else if(t=="TaskProgress") {
        correlation_payload(m);fields(p,{"progress_sequence"});u64(p["progress_sequence"],true);
    } else if(t=="TaskFinished") {
        correlation_payload(m);
        fields(p,{"execution_state","quality","result_ref","error_code"});
        const auto state=choice(p["execution_state"],{"Succeeded","Failed","Cancelled","TimedOut"});
        const auto q=choice(p["quality"],{"Unknown","OK","NG"});
        if(state=="Succeeded") {
            if(q=="Unknown" || !p["error_code"].is_null())throw ProtocolError("Invalid successful task");
            id(p["result_ref"]);
        } else {
            if(q!="Unknown" || !p["result_ref"].is_null())throw ProtocolError("Failed task cannot carry success result");
            id(p["error_code"]);
        }
    } else if(t=="Cancel") {
        correlation_payload(m);fields(p,{"reason"});text(p["reason"],256);
    } else if(t=="Heartbeat") {
        fields(p,{"progress_sequence"});u64(p["progress_sequence"]);
    } else if(t=="Fault") {
        fields(p,{"code","category","message","retryability"});
        id(p["code"]);choice(p["category"],{"Configuration","Protocol","Device","Execution","Resource","Persistence","Delivery","Internal"});
        text(p["message"],2048);choice(p["retryability"],{"Never","AfterRecovery","Safe"});
    } else if(t=="Drain") {
        fields(p,{"budget_ns"});u64(p["budget_ns"],true);
    } else if(t=="Stop") {
        fields(p,{"reason"});text(p["reason"],256);
    } else if(t=="LeaseRelease") {
        fields(p,{"pool_id","lease_id","slot_id","slot_generation"});
        id(p["pool_id"]);id(p["lease_id"]);u64(p["slot_id"]);u64(p["slot_generation"],true);
    } else if(t=="ResultEvent") {
        fields(p,{"event"});
        const auto event=serialization::decode_result(p["event"].dump());
        if(event.result.run_id.value()!=m.header.run_id)throw ProtocolError("Result event run mismatch");
    } else if(t=="DeliveryTask") {
        correlation_payload(m);fields(p,{"event","output_instance_id","attempt","budget_ns"});
        (void)serialization::decode_result(p["event"].dump());
        id(p["output_instance_id"]);u64(p["attempt"],true);u64(p["budget_ns"],true);
    } else if(t=="DeliveryReport"||t=="DeliveryFinished") {
        if(t=="DeliveryFinished")correlation_payload(m);
        fields(p,{"event_id","output_instance_id","state","attempt","error_code"});
        id(p["event_id"]);id(p["output_instance_id"]);u64(p["attempt"],true);
        const auto state=choice(p["state"],{"Pending","Accepted","DurablyQueued","TransportConfirmed","BusinessAcked","Failed","Expired","Unknown"});
        if(state=="Failed"||state=="Expired"||state=="Unknown")id(p["error_code"]);
        else if(!p["error_code"].is_null())throw ProtocolError("Unexpected delivery error");
    } else throw ProtocolError("Unknown IPC message type");
    const bool task=t=="SubmitTask"||t=="TaskAccepted"||t=="TaskProgress"||t=="TaskFinished"||t=="Cancel"||
        t=="CaptureTask"||t=="CaptureFinished"||t=="InspectTask"||t=="DeviceTask"||t=="DeviceFinished"||t=="DeliveryTask"||t=="DeliveryFinished"||t=="CheckFinished";
    if(!task && t!="Fault" && m.correlation)throw ProtocolError("Unexpected task correlation");
}
J correlation_json(const contracts::Correlation& c) {
    return {{"run_id",c.run_id.value()},{"worker_id",c.worker_id.value()},{"worker_epoch",std::to_string(c.worker_epoch)},
        {"inspection_id",c.inspection_id.value()},{"check_id",c.check_id.value()},{"task_id",c.task_id.value()},{"attempt",std::to_string(c.attempt)}};
}
template<class F> auto boundary(F f) {
    try{return f();}
    catch(const ProtocolError&){throw;}
    catch(const std::invalid_argument&){throw ProtocolError("IPC field value invalid");}
    catch(const J::exception&){throw ProtocolError("Malformed IPC JSON");}
    catch(const serialization::ProtocolError&){throw ProtocolError("Invalid embedded result event");}
}
}
Message decode_message(std::string_view json) {
    return boundary([&] {
        const auto j=parse(json);
        fields(j,{"protocol_major","protocol_minor","message_type","request_id","run_id","worker_id","worker_epoch","sequence","correlation","payload"});
        Message m{{integer(j["protocol_major"],65535),integer(j["protocol_minor"],65535),
            id(j["message_type"]),id(j["request_id"]),id(j["run_id"]),id(j["worker_id"]),u64(j["worker_epoch"],true),u64(j["sequence"],true)}, {},{}};
        if(m.header.major!=1 || m.header.minor!=0)throw ProtocolError("Unsupported IPC version");
        if(!j["correlation"].is_null()) {
            const auto& c=j["correlation"];
            fields(c,{"run_id","worker_id","worker_epoch","inspection_id","check_id","task_id","attempt"});
            m.correlation=contracts::Correlation{contracts::RunId(id(c["run_id"])),contracts::WorkerId(id(c["worker_id"])),
                u64(c["worker_epoch"],true),contracts::InspectionId(id(c["inspection_id"])),contracts::CheckId(id(c["check_id"])),
                contracts::TaskId(id(c["task_id"])),u64(c["attempt"],true)};
            if(m.correlation->run_id.value()!=m.header.run_id || m.correlation->worker_id.value()!=m.header.worker_id ||
               m.correlation->worker_epoch!=m.header.epoch)throw ProtocolError("IPC correlation identity mismatch");
        }
        check_payload(m,j["payload"]);
        m.payload=j["payload"].dump();
        return m;
    });
}
std::string encode_message(const Message& m) {
    return boundary([&] {
        const auto& h=m.header;
        J j{{"protocol_major",h.major},{"protocol_minor",h.minor},{"message_type",h.type},{"request_id",h.request_id},
            {"run_id",h.run_id},{"worker_id",h.worker_id},{"worker_epoch",std::to_string(h.epoch)},
            {"sequence",std::to_string(h.sequence)},{"correlation",m.correlation?correlation_json(*m.correlation):J(nullptr)},{"payload",parse(m.payload)}};
        auto result=j.dump();
        (void)decode_message(result);
        return result;
    });
}
bool control_type(std::string_view t) noexcept {
    return t=="Hello"||t=="Heartbeat"||t=="Stop"||t=="Cancel"||t=="Fault"||t=="Drain";
}
HelloPayload read_hello(const Message& m) {
    return boundary([&] {
        if(m.header.type!="Hello")throw ProtocolError("Hello required");
        const auto p=parse(m.payload);check_payload(m,p);
        HelloPayload result{text(p["token"],128),{}};
        for(const auto& cap:p["required_capabilities"])result.required.insert(id(cap));
        return result;
    });
}
std::string hello_payload(std::string_view token) {
    return J{{"token",token},{"required_capabilities",J::array({"capture-v1","inspect-v1","device-v1","delivery-v1","inference-v1"})}}.dump();
}
} // namespace vision::ipc
