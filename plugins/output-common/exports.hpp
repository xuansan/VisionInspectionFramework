#pragma once
// Include after defining Output and VISION_OUTPUT_ID / VISION_OUTPUT_BUILD in each DLL.
namespace {
template<std::size_t N>void describe_copy(char(&out)[N],const char* text){const auto n=std::strlen(text);if(n<N)std::memcpy(out,text,n+1);}
}
VISION_PLUGIN_EXPORT vision::plugin_sdk::Status VISION_PLUGIN_CALL vision_plugin_describe(vision::plugin_sdk::Descriptor* out,std::uint32_t size) noexcept {
    using namespace vision::plugin_sdk;
    if(!out||size!=sizeof(Descriptor))return Status::Invalid;*out={};out->struct_size=size;out->version=api_version;
    out->kind=Kind::ResultOutput;out->threading=Threading::Serialized;out->max_instances=1;
    describe_copy(out->plugin_id,VISION_OUTPUT_ID);describe_copy(out->plugin_version,"0.1.0");describe_copy(out->build_id,VISION_OUTPUT_BUILD);
    describe_copy(out->platform,"windows");describe_copy(out->architecture,"x64");describe_copy(out->compiler_abi,"msvc-v143-md");
#ifdef _DEBUG
    describe_copy(out->runtime_variant,"Debug");
#else
    describe_copy(out->runtime_variant,"Release");
#endif
    return Status::Ok;
}
VISION_PLUGIN_EXPORT vision::plugin_sdk::Status VISION_PLUGIN_CALL vision_plugin_create(vision::plugin_sdk::Plugin** out) noexcept {
    using namespace vision::plugin_sdk;if(!out)return Status::Invalid;try{*out=new Output;return Status::Ok;}catch(...){*out=nullptr;return Status::Failed;}
}
VISION_PLUGIN_EXPORT vision::plugin_sdk::Status VISION_PLUGIN_CALL vision_plugin_destroy(vision::plugin_sdk::Plugin* p) noexcept {
    using namespace vision::plugin_sdk;auto* value=dynamic_cast<Output*>(p);if(!value)return Status::Invalid;delete value;return Status::Ok;
}
