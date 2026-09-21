#include <vision/plugin_sdk/api.hpp>
#include <vision/contracts/types.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstring>
#define NOMINMAX
#include <windows.h>
using namespace vision;
using namespace plugin_sdk;
namespace {
using J=nlohmann::json;
class Device final : public Communication {
public:
    Status initialize(Bytes input) noexcept override {
        try {fault_=J::parse(input.data,input.data+input.size).value("fault",std::string("none"));return Status::Ok;}
        catch(...) {return Status::Invalid;}
    }
    void request_stop() noexcept override {stopped_=true;}
    Status connect(Bytes input) noexcept override {
        try {
            const auto s=J::parse(input.data,input.data+input.size).at("session_id").get<std::string>();
            (void)contracts::TaskId(s);
            if(session_.empty())session_=s;
            return session_==s&&!stopped_?Status::Ok:Status::Invalid;
        }catch(...) {return Status::Invalid;}
    }
    Status try_command(Bytes input) noexcept override {
        try {
            if(stopped_)return Status::Cancelled;
            const auto p=J::parse(input.data,input.data+input.size);
            const auto sequence=contracts::parse_u64(p.at("command_sequence").get<std::string>());
            if(p.at("session_id")!=session_||sequence<=sequence_)return Status::Invalid;
            sequence_=sequence;
            if(fault_=="disconnect")return Status::Failed;
            if(fault_=="crash")TerminateProcess(GetCurrentProcess(),74);
            if(fault_=="hang")for(;;)std::this_thread::sleep_for(std::chrono::milliseconds(20));
            const bool arrival=p.at("arrival").get<bool>();
            if(arrival&&!arrival_) {if(arrivals_==UINT64_MAX)return Status::Failed;++arrivals_;}
            arrival_=arrival;position_=p.at("position");safety_=p.at("safety_ok");
            ready_=p.at("ready").get<bool>()&&position_&&safety_;
            due_=std::chrono::steady_clock::now()+std::chrono::milliseconds(200);
            ack_=nullptr;
            if(!p.at("result_id").is_null()) {
                if(!position_||!safety_)return Status::Failed;
                if(fault_=="old_ack")ack_="old-result";
                else if(fault_!="lost_ack")ack_=p.at("result_id");
            }
            return Status::Ok;
        }catch(...) {return Status::Invalid;}
    }
    Status poll_input(Buffer& out) noexcept override {
        try {
            const auto text=J{{"session_id",session_},{"command_sequence",std::to_string(sequence_)},
                {"ready",ready_&&!stopped_&&std::chrono::steady_clock::now()<due_},
                {"arrival_sequence",std::to_string(arrivals_)},{"position",position_},{"safety_ok",safety_},{"ack_id",ack_}}.dump();
            if(text.size()>out.capacity)return Status::Invalid;
            std::memcpy(out.data,text.data(),text.size());out.size=static_cast<std::uint32_t>(text.size());return Status::Ok;
        }catch(...) {return Status::Failed;}
    }
private:
    std::atomic<bool> stopped_{};
    std::string session_,fault_;
    std::uint64_t sequence_{},arrivals_{};
    bool arrival_{},ready_{},position_{},safety_{};
    J ack_;
    std::chrono::steady_clock::time_point due_;
};
template<std::size_t N> void copy(char(&out)[N],const char* text) {const auto n=std::strlen(text);if(n<N)std::memcpy(out,text,n+1);}
}
VISION_PLUGIN_EXPORT Status VISION_PLUGIN_CALL vision_plugin_describe(Descriptor* out,std::uint32_t size) noexcept {
    if(!out||size!=sizeof(Descriptor))return Status::Invalid;
    *out={};out->struct_size=size;out->version=api_version;out->kind=Kind::Communication;
    out->threading=Threading::Serialized;out->max_instances=1;
    copy(out->plugin_id,"vision.sim-device");copy(out->plugin_version,"0.1.0");copy(out->build_id,"sim-device-v1");
    copy(out->platform,"windows");copy(out->architecture,"x64");copy(out->compiler_abi,"msvc-v143-md");
#ifdef _DEBUG
    copy(out->runtime_variant,"Debug");
#else
    copy(out->runtime_variant,"Release");
#endif
    return Status::Ok;
}
VISION_PLUGIN_EXPORT Status VISION_PLUGIN_CALL vision_plugin_create(Plugin** out) noexcept {
    if(!out)return Status::Invalid;
    try {*out=new Device;return Status::Ok;}catch(...) {*out=nullptr;return Status::Failed;}
}
VISION_PLUGIN_EXPORT Status VISION_PLUGIN_CALL vision_plugin_destroy(Plugin* p) noexcept {
    auto instance=dynamic_cast<Device*>(p);if(!instance)return Status::Invalid;delete instance;return Status::Ok;
}
