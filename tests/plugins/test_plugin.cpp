#include <vision/plugin_sdk/api.hpp>
#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>
#include <new>
#define NOMINMAX
#include <windows.h>
using namespace vision::plugin_sdk;
namespace {
class TestAlgorithm final:public Algorithm {
public:
    Status initialize(Bytes p) noexcept override {
        try {parameters.assign(reinterpret_cast<const char*>(p.data),p.size);}
        catch(...) {return Status::Failed;}
        return parameters.find("init-fail")!=std::string::npos?Status::Failed:Status::Ok;
    }
    void request_stop() noexcept override {stop=true;}
    Status execute(Bytes,Buffer& result) noexcept override {
        if(parameters.find("crash")!=std::string::npos)
            RaiseException(0xC0000005,EXCEPTION_NONCONTINUABLE,0,nullptr);
        if(parameters.find("hang")!=std::string::npos)Sleep(10000);
        if(parameters.find("cancel")!=std::string::npos) {
            while(!stop.load())Sleep(5);
            return Status::Cancelled;
        }
        if(parameters.find("execute-fail")!=std::string::npos)return Status::Failed;
        if(parameters.find("oversize")!=std::string::npos) {result.size=result.capacity+1;return Status::Ok;}
        const char* response=parameters.find("bad-result")!=std::string::npos?"{}":
            R"({"execution_state":"Succeeded","quality":"OK","result_ref":"synthetic-result","error_code":null})";
        const auto size=std::strlen(response);
        if(result.capacity<size)return Status::Busy;
        std::memcpy(result.data,response,size);result.size=static_cast<std::uint32_t>(size);return Status::Ok;
    }
    std::string parameters;
    std::atomic<bool> stop{};
};
template<std::size_t N> void copy(char(&out)[N],const char* value) {
    const auto count=std::strlen(value);
    if(count<N) {std::memcpy(out,value,count);out[count]=0;}
}
}
VISION_PLUGIN_EXPORT Status VISION_PLUGIN_CALL vision_plugin_describe(Descriptor* out,std::uint32_t size) noexcept {
    if(!out||size!=sizeof(Descriptor))return Status::Invalid;
    *out={};out->struct_size=sizeof(Descriptor);out->version=api_version;
#ifdef VISION_BAD_API
    out->version=99;
#endif
    out->kind=Kind::Algorithm;out->threading=Threading::Serialized;out->max_instances=1;
    copy(out->plugin_id,"test.algorithm");copy(out->plugin_version,"0.1.0");copy(out->build_id,"test-v1");
    copy(out->platform,"windows");copy(out->architecture,"x64");copy(out->compiler_abi,"msvc-v143-md");
#ifdef _DEBUG
    copy(out->runtime_variant,"Debug");
#else
    copy(out->runtime_variant,"Release");
#endif
    return Status::Ok;
}
#ifndef VISION_MISSING_FACTORY
VISION_PLUGIN_EXPORT Status VISION_PLUGIN_CALL vision_plugin_create(Plugin** out) noexcept {
    if(!out)return Status::Invalid;
    if(GetEnvironmentVariableA("VISION_TEST_FACTORY_FAIL",nullptr,0)>0) {*out=nullptr;return Status::Failed;}
    *out=new(std::nothrow) TestAlgorithm;
    return *out?Status::Ok:Status::Failed;
}
#endif
VISION_PLUGIN_EXPORT Status VISION_PLUGIN_CALL vision_plugin_destroy(Plugin* plugin) noexcept {
    auto instance=dynamic_cast<TestAlgorithm*>(plugin);
    if(!instance)return Status::Invalid;
    const bool fail=instance->parameters.find("destroy-fail")!=std::string::npos;
    delete instance;return fail?Status::Failed:Status::Ok;
}
