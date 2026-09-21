#pragma once
#include <vision/contracts/validation.hpp>
#include <vision/inspection/recipe.hpp>
#include <vision/contracts/frame.hpp>

namespace vision::serialization {
inline constexpr std::size_t max_document_bytes = 1024 * 1024;
class ProtocolError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
struct PluginManifest {
    contracts::PluginId plugin_id;
    std::string plugin_version, kind, build_id, platform, architecture, compiler_abi, runtime_variant;
    std::string entry_library, threading_model, parameters_schema;
    std::uint32_t max_instances{};
    std::vector<std::string> capabilities, dependencies;
    std::string license_spdx, license_file;
};
// Throws ProtocolError on malformed or unsupported documents. No JSON types in public interfaces.
contracts::ResultEnvelope decode_result(std::string_view document);
std::string encode_result(const contracts::ResultEnvelope& envelope);
contracts::CheckResult decode_check(std::string_view document);
std::string encode_check(const contracts::CheckResult&);
contracts::FrameDescriptor decode_frame(std::string_view document,std::uint64_t slot_bytes);
std::string encode_frame(const contracts::FrameDescriptor& descriptor,std::uint64_t slot_bytes);
inspection::Recipe decode_recipe(std::string_view document);
PluginManifest decode_manifest(std::string_view document);
// Validates the documented flat scalar parameter schema subset. Does not mutate defaults.
// while_running validates a partial update and rejects any immutable parameter key.
void validate_parameters(const PluginManifest& manifest, std::string_view document, bool while_running = false);
} // namespace vision::serialization
