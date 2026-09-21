#include <vision/plugin_sdk/api.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstring>
#define NOMINMAX
#include <windows.h>
using namespace vision::plugin_sdk;
namespace {
class Output final : public ResultOutput {
public:
    Status initialize(Bytes input) noexcept override {
        try {fault_=nlohmann::json::parse(input.data,input.data+input.size).value("fault",std::string("none"));return Status::Ok;}
        catch(...) {return Status::Invalid;}
    }
    void request_stop() noexcept override {stopped_=true;}
    Status try_submit(Bytes input) noexcept override {
        try {
            if(stopped_)return Status::Cancelled;
            if(!report_.empty())return Status::Busy;
            if(fault_=="hang")for(;;)std::this_thread::sleep_for(std::chrono::milliseconds(20));
            if(fault_=="crash")TerminateProcess(GetCurrentProcess(),75);
            const auto p=nlohmann::json::parse(input.data,input.data+input.size);
            report_=nlohmann::json{{"event_id",fault_=="wrong_ack"?nlohmann::json("wrong-event"):p["event"]["event_id"]},
                {"output_instance_id",p["output_instance_id"]},{"attempt",p["attempt"]},
                {"state",fault_=="fail"?"Failed":"BusinessAcked"},{"error_code",fault_=="fail"?nlohmann::json("OUTPUT.REJECTED"):nlohmann::json(nullptr)}}.dump();
            return Status::Ok;
        }catch(...) {return Status::Invalid;}
    }
    Status poll_report(Buffer& out) noexcept override {
        if(stopped_)return Status::Cancelled;
        if(report_.empty()||fault_=="lost_ack")return Status::Busy;
        if(report_.size()>out.capacity)return Status::Invalid;
        std::memcpy(out.data,report_.data(),report_.size());out.size=static_cast<std::uint32_t>(report_.size());
        report_.clear();return Status::Ok;
    }
private:
    std::atomic<bool> stopped_{};std::string fault_,report_;
};
template<std::size_t N> void copy(char(&out)[N],const char* text) {const auto n=std::strlen(text);if(n<N)std::memcpy(out,text,n+1);}
}
VISION_PLUGIN_EXPORT Status VISION_PLUGIN_CALL vision_plugin_describe(Descriptor* out,std::uint32_t size) noexcept {
    if(!out||size!=sizeof(Descriptor))return Status::Invalid;
    *out={};out->struct_size=size;out->version=api_version;out->kind=Kind::ResultOutput;
    out->threading=Threading::Serialized;out->max_instances=1;
    copy(out->plugin_id,"vision.test-output");copy(out->plugin_version,"0.1.0");copy(out->build_id,"test-output-v1");
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
    try {*out=new Output;return Status::Ok;}catch(...) {*out=nullptr;return Status::Failed;}
}
VISION_PLUGIN_EXPORT Status VISION_PLUGIN_CALL vision_plugin_destroy(Plugin* p) noexcept {
    auto instance=dynamic_cast<Output*>(p);if(!instance)return Status::Invalid;delete instance;return Status::Ok;
}
