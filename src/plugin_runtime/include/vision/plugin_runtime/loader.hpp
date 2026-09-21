#pragma once
#include <vision/plugin_sdk/api.hpp>
#include <vision/serialization/json_codec.hpp>
#include <filesystem>
#include <memory>

namespace vision::plugin_runtime {
// Call load/initialize/execute/close on one plugin thread, request_stop is the sole concurrent call.
class Library {
public:
    static std::unique_ptr<Library> load(const std::filesystem::path& manifest_path,std::string parameters);
    ~Library();
    Library(const Library&)=delete;
    Library& operator=(const Library&)=delete;
    plugin_sdk::Status initialize();
    plugin_sdk::Status execute(std::string_view request,std::string& response);
    plugin_sdk::Status device_exchange(std::string_view request,std::string& response);
    plugin_sdk::Status output_submit(std::string_view request);
    plugin_sdk::Status output_poll(std::string& response);
    plugin_sdk::Status camera_enumerate(std::string& devices);
    plugin_sdk::Status camera_open(std::string_view settings);
    plugin_sdk::Status camera_trigger(std::string_view permit);
    plugin_sdk::Status camera_poll(std::string& descriptor);
    plugin_sdk::Status camera_close();
    void request_stop() noexcept;
    plugin_sdk::Status close() noexcept;
    const serialization::PluginManifest& manifest() const;
private:
    plugin_sdk::Status camera_output(bool enumerate,std::string& output);
    struct Impl;
    explicit Library(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
} // namespace vision::plugin_runtime
