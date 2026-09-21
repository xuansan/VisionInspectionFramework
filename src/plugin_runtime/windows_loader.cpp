#include <vision/plugin_runtime/loader.hpp>
#include <fstream>
#include <cstring>
#include <array>
#define NOMINMAX
#include <windows.h>

namespace vision::plugin_runtime {
using namespace plugin_sdk;
namespace {
template<std::size_t N> std::string bounded(const char(&value)[N]) {
    const auto end=static_cast<const char*>(std::memchr(value,0,N));
    if(!end)throw std::runtime_error("Unterminated plugin descriptor");
    return std::string(value,end);
}
std::string read(const std::filesystem::path& path) {
    const auto length=std::filesystem::file_size(path);
    if(length>serialization::max_document_bytes)throw std::runtime_error("Oversized manifest");
    std::ifstream stream(path,std::ios::binary);
    if(!stream)throw std::runtime_error("Cannot open manifest");
    std::string result(static_cast<std::size_t>(length),'\0');
    if(!stream.read(result.data(),static_cast<std::streamsize>(result.size())))throw std::runtime_error("Manifest read failed");
    return result;
}
Kind kind(const std::string& value) {
    if(value=="Camera")return Kind::Camera;if(value=="Algorithm")return Kind::Algorithm;
    if(value=="Communication")return Kind::Communication;if(value=="ObjectStorage")return Kind::ObjectStorage;
    return Kind::ResultOutput;
}
Threading threading(const std::string& value) {
    if(value=="Serialized")return Threading::Serialized;
    if(value=="Reentrant")return Threading::Reentrant;
    return Threading::DedicatedThread;
}
}
struct Library::Impl {
    serialization::PluginManifest manifest;
    std::string parameters;
    HMODULE module{};
    Plugin* instance{};
    Destroy destroy{};
    bool initialized{},closed{},pinned{},camera_opened{};
    Status close_status{Status::Ok};
    Impl(serialization::PluginManifest m,std::string p):manifest(std::move(m)),parameters(std::move(p)) {}
    ~Impl() {
        // A failing destroy cannot prove absence of callbacks: keep the module mapped until process exit.
        if(instance&&destroy) {if(destroy(instance)!=Status::Ok)pinned=true;}
        if(module&&!pinned)FreeLibrary(module);
    }
};
Library::Library(std::unique_ptr<Impl> impl):impl_(std::move(impl)) {}
Library::~Library()=default;
const serialization::PluginManifest& Library::manifest() const {return impl_->manifest;}
std::unique_ptr<Library> Library::load(const std::filesystem::path& path,std::string parameters) {
    const auto canonical=std::filesystem::canonical(path);
    auto m=serialization::decode_manifest(read(canonical));
    serialization::validate_parameters(m,parameters);
#ifdef _DEBUG
    constexpr auto variant="Debug";
#else
    constexpr auto variant="Release";
#endif
    if(m.platform!="windows"||m.architecture!="x64"||m.compiler_abi!="msvc-v143-md"||
       m.runtime_variant!=variant||!m.dependencies.empty())
        throw std::runtime_error("Unsupported plugin build or dependencies");
    const auto binary=std::filesystem::canonical(canonical.parent_path()/m.entry_library);
    if(binary.parent_path()!=canonical.parent_path())throw std::runtime_error("Plugin escapes package");
    auto impl=std::make_unique<Impl>(std::move(m),std::move(parameters));
    impl->module=LoadLibraryExW(binary.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!impl->module)throw std::runtime_error("Plugin DLL load failed");
    auto describe=reinterpret_cast<Describe>(GetProcAddress(impl->module,"vision_plugin_describe"));
    auto create=reinterpret_cast<Create>(GetProcAddress(impl->module,"vision_plugin_create"));
    impl->destroy=reinterpret_cast<Destroy>(GetProcAddress(impl->module,"vision_plugin_destroy"));
    if(!describe||!create||!impl->destroy)throw std::runtime_error("Missing plugin export");
    Descriptor d{};
    if(describe(&d,sizeof(d))!=Status::Ok||d.struct_size!=sizeof(d)||d.version!=api_version)
        throw std::runtime_error("Plugin API mismatch");
    const auto& expected=impl->manifest;
    if(bounded(d.plugin_id)!=expected.plugin_id.value()||bounded(d.plugin_version)!=expected.plugin_version||
       bounded(d.build_id)!=expected.build_id||bounded(d.platform)!=expected.platform||
       bounded(d.architecture)!=expected.architecture||bounded(d.compiler_abi)!=expected.compiler_abi||
       bounded(d.runtime_variant)!=expected.runtime_variant||d.kind!=kind(expected.kind)||
       d.threading!=threading(expected.threading_model)||d.max_instances!=expected.max_instances)
        throw std::runtime_error("Manifest differs from exported descriptor");
    if(create(&impl->instance)!=Status::Ok||!impl->instance)throw std::runtime_error("Plugin factory failed");
    return std::unique_ptr<Library>(new Library(std::move(impl)));
}
Status Library::initialize() {
    if(impl_->initialized||impl_->closed)return Status::Invalid;
    const auto& p=impl_->parameters;
    const auto status=impl_->instance->initialize({reinterpret_cast<const std::uint8_t*>(p.data()),static_cast<std::uint32_t>(p.size())});
    impl_->initialized=status==Status::Ok;return status;
}
Status Library::execute(std::string_view request,std::string& response) {
    response.clear();
    if(!impl_->initialized||impl_->closed||request.size()>max_payload)return Status::Invalid;
    if(impl_->manifest.kind!="Algorithm")return Status::Unsupported;
    auto algorithm=dynamic_cast<Algorithm*>(impl_->instance);
    if(!algorithm)return Status::Invalid;
    std::array<std::uint8_t,max_payload> storage{};
    Buffer out{storage.data(),static_cast<std::uint32_t>(storage.size()),0};
    const auto status=algorithm->execute({reinterpret_cast<const std::uint8_t*>(request.data()),static_cast<std::uint32_t>(request.size())},out);
    if(out.data!=storage.data()||out.capacity!=storage.size()||out.size>storage.size())return Status::Invalid;
    if(status==Status::Ok)response.assign(reinterpret_cast<const char*>(storage.data()),out.size);
    return status;
}
void Library::request_stop() noexcept {if(impl_->instance&&!impl_->closed)impl_->instance->request_stop();}
Status Library::output_submit(std::string_view request) {
    if(!impl_->initialized||impl_->closed||request.size()>max_payload||impl_->manifest.kind!="ResultOutput")return Status::Invalid;
    auto output=dynamic_cast<ResultOutput*>(impl_->instance);if(!output)return Status::Invalid;
    return output->try_submit({reinterpret_cast<const std::uint8_t*>(request.data()),static_cast<std::uint32_t>(request.size())});
}
Status Library::output_poll(std::string& response) {
    response.clear();
    if(!impl_->initialized||impl_->closed||impl_->manifest.kind!="ResultOutput")return Status::Invalid;
    auto output=dynamic_cast<ResultOutput*>(impl_->instance);if(!output)return Status::Invalid;
    std::array<std::uint8_t,max_payload> storage{};
    Buffer out{storage.data(),static_cast<std::uint32_t>(storage.size()),0};
    const auto status=output->poll_report(out);
    if(out.data!=storage.data()||out.capacity!=storage.size()||out.size>storage.size())return Status::Invalid;
    if(status==Status::Ok)response.assign(reinterpret_cast<const char*>(storage.data()),out.size);
    return status;
}
Status Library::device_exchange(std::string_view request,std::string& response) {
    response.clear();
    if(!impl_->initialized||impl_->closed||request.size()>max_payload||impl_->manifest.kind!="Communication")return Status::Invalid;
    auto device=dynamic_cast<Communication*>(impl_->instance);if(!device)return Status::Invalid;
    const Bytes input{reinterpret_cast<const std::uint8_t*>(request.data()),static_cast<std::uint32_t>(request.size())};
    auto status=device->connect(input);
    if(status!=Status::Ok)return status;
    status=device->try_command(input);if(status!=Status::Ok)return status;
    std::array<std::uint8_t,max_payload> storage{};
    Buffer out{storage.data(),static_cast<std::uint32_t>(storage.size()),0};
    status=device->poll_input(out);
    if(out.data!=storage.data()||out.capacity!=storage.size()||out.size>storage.size())return Status::Invalid;
    if(status==Status::Ok)response.assign(reinterpret_cast<const char*>(storage.data()),out.size);
    return status;
}
Status Library::camera_output(bool enumerate,std::string& output) {
    output.clear();
    if(!impl_->initialized||impl_->closed)return Status::Invalid;
    if(impl_->manifest.kind!="Camera")return Status::Unsupported;
    auto camera=dynamic_cast<Camera*>(impl_->instance);
    if(!camera||(!enumerate&&!impl_->camera_opened))return Status::Invalid;
    std::array<std::uint8_t,max_payload> storage{};
    Buffer out{storage.data(),static_cast<std::uint32_t>(storage.size()),0};
    const auto status=enumerate?camera->enumerate(out):camera->poll_frame(out);
    if(out.data!=storage.data()||out.capacity!=storage.size()||out.size>storage.size())return Status::Invalid;
    if(status==Status::Ok)output.assign(reinterpret_cast<const char*>(storage.data()),out.size);
    return status;
}
Status Library::camera_enumerate(std::string& devices) {return camera_output(true,devices);}
Status Library::camera_poll(std::string& descriptor) {return camera_output(false,descriptor);}
Status Library::camera_open(std::string_view settings) {
    if(!impl_->initialized||impl_->closed||impl_->camera_opened||settings.size()>max_payload)return Status::Invalid;
    if(impl_->manifest.kind!="Camera")return Status::Unsupported;
    auto camera=dynamic_cast<Camera*>(impl_->instance);if(!camera)return Status::Invalid;
    const auto result=camera->open({reinterpret_cast<const std::uint8_t*>(settings.data()),static_cast<std::uint32_t>(settings.size())});
    impl_->camera_opened=result==Status::Ok;return result;
}
Status Library::camera_trigger(std::string_view permit) {
    if(!impl_->initialized||impl_->closed||!impl_->camera_opened||permit.size()>max_payload)return Status::Invalid;
    auto camera=dynamic_cast<Camera*>(impl_->instance);if(!camera)return Status::Invalid;
    return camera->trigger({reinterpret_cast<const std::uint8_t*>(permit.data()),static_cast<std::uint32_t>(permit.size())});
}
Status Library::camera_close() {
    if(impl_->closed||!impl_->initialized)return Status::Invalid;
    if(impl_->manifest.kind!="Camera")return Status::Unsupported;
    if(!impl_->camera_opened)return Status::Ok;
    auto camera=dynamic_cast<Camera*>(impl_->instance);if(!camera)return Status::Invalid;
    const auto result=camera->close();
    if(result==Status::Ok)impl_->camera_opened=false;
    return result;
}
Status Library::close() noexcept {
    if(impl_->closed)return impl_->close_status;
    Status camera_status=Status::Ok;
    if(impl_->camera_opened) {
        auto camera=dynamic_cast<Camera*>(impl_->instance);
        camera_status=camera?camera->close():Status::Invalid;
    }
    impl_->closed=true;
    const auto status=impl_->destroy(impl_->instance);
    impl_->instance=nullptr;
    if(status!=Status::Ok)impl_->pinned=true;
    impl_->close_status=status!=Status::Ok?status:camera_status;
    return impl_->close_status;
}
} // namespace vision::plugin_runtime
