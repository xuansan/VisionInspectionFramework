#include <vision/plugin_sdk/api.hpp>
#include <vision/serialization/json_codec.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <cstring>
using namespace vision;
namespace {
// Bounded in-memory receiver example. BusinessAcked means this receiver processed the event;
// it is NOT durable storage or an external subscriber ACK. Do not add blocking IO here.
class Output final:public plugin_sdk::ResultOutput {
public:
    plugin_sdk::Status initialize(plugin_sdk::Bytes b) noexcept override {
        try{
            if(!b.data||!b.size||b.size>1024)return plugin_sdk::Status::Invalid;
            const auto j=nlohmann::json::parse(b.data,b.data+b.size);
            if(!j.is_object()||j.size()>1)return plugin_sdk::Status::Invalid;
            mode_=j.value("mode",std::string("normal"));
            if(mode_!="normal"&&mode_!="reject"&&mode_!="drop-ack")return plugin_sdk::Status::Invalid;
            return plugin_sdk::Status::Ok;
        }catch(...){return plugin_sdk::Status::Invalid;}
    }
    void request_stop() noexcept override {stopped_=true;}
    plugin_sdk::Status try_submit(plugin_sdk::Bytes b) noexcept override {
        try{
            if(stopped_)return plugin_sdk::Status::Cancelled;
            if(!report_.empty())return plugin_sdk::Status::Busy;
            if(!b.data||!b.size||b.size>32768)return plugin_sdk::Status::Invalid;
            const auto j=nlohmann::json::parse(b.data,b.data+b.size);
            const auto event=serialization::decode_result(j.at("event").dump());
            (void)contracts::OutputId(j.at("output_instance_id").get<std::string>());
            if(!contracts::parse_u64(j.at("attempt").get<std::string>()))return plugin_sdk::Status::Invalid;
            report_=nlohmann::json{{"event_id",event.event_id.value()},{"output_instance_id",j.at("output_instance_id")},
                {"attempt",j.at("attempt")},{"state",mode_=="reject"?"Failed":"BusinessAcked"},
                {"error_code",mode_=="reject"?nlohmann::json("OUTPUT.REJECTED"):nlohmann::json(nullptr)}}.dump();
            return plugin_sdk::Status::Ok;
        }catch(...){return plugin_sdk::Status::Invalid;}
    }
    plugin_sdk::Status poll_report(plugin_sdk::Buffer& out) noexcept override {
        if(stopped_)return plugin_sdk::Status::Cancelled;
        if(report_.empty()||mode_=="drop-ack")return plugin_sdk::Status::Busy;
        if(!out.data||out.capacity<report_.size())return plugin_sdk::Status::Invalid;
        std::memcpy(out.data,report_.data(),report_.size());out.size=static_cast<std::uint32_t>(report_.size());report_.clear();return plugin_sdk::Status::Ok;
    }
private:
    std::atomic<bool> stopped_{};std::string mode_,report_;
};
template<std::size_t N> void text(char(&out)[N],const char* value){const auto n=std::strlen(value);if(n<N)std::memcpy(out,value,n+1);}
}
VISION_PLUGIN_EXPORT plugin_sdk::Status VISION_PLUGIN_CALL vision_plugin_describe(plugin_sdk::Descriptor* out,std::uint32_t size) noexcept {
    if(!out||size!=sizeof(*out))return plugin_sdk::Status::Invalid;
    *out={};out->struct_size=size;out->version=plugin_sdk::api_version;out->kind=plugin_sdk::Kind::ResultOutput;
    out->threading=plugin_sdk::Threading::Serialized;out->max_instances=1;
    text(out->plugin_id,"user.@PROJECT_NAME@");text(out->plugin_version,"0.1.0");text(out->build_id,"@PROJECT_NAME@-v1");
    text(out->platform,"windows");text(out->architecture,"x64");text(out->compiler_abi,"msvc-v143-md");
#ifdef _DEBUG
    text(out->runtime_variant,"Debug");
#else
    text(out->runtime_variant,"Release");
#endif
    return plugin_sdk::Status::Ok;
}
VISION_PLUGIN_EXPORT plugin_sdk::Status VISION_PLUGIN_CALL vision_plugin_create(plugin_sdk::Plugin** out) noexcept {
    if(!out)return plugin_sdk::Status::Invalid;
    try{*out=new Output;return plugin_sdk::Status::Ok;}catch(...){*out=nullptr;return plugin_sdk::Status::Failed;}
}
VISION_PLUGIN_EXPORT plugin_sdk::Status VISION_PLUGIN_CALL vision_plugin_destroy(plugin_sdk::Plugin* p) noexcept {
    auto value=dynamic_cast<Output*>(p);if(!value)return plugin_sdk::Status::Invalid;delete value;return plugin_sdk::Status::Ok;
}
