#pragma once
#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#define VISION_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#define VISION_PLUGIN_CALL __cdecl
#else
#define VISION_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#define VISION_PLUGIN_CALL
#endif
namespace vision::plugin_sdk {
inline constexpr std::uint32_t api_version=1;
inline constexpr std::uint32_t max_payload=65536;
enum class Status : std::uint32_t { Ok, Busy, Invalid, Failed, Cancelled, Unsupported };
enum class Kind : std::uint32_t { Camera=1, Algorithm, Communication, ObjectStorage, ResultOutput };
enum class Threading : std::uint32_t { Serialized=1, Reentrant, DedicatedThread };
struct Bytes {const std::uint8_t* data;std::uint32_t size;};
struct Buffer {std::uint8_t* data;std::uint32_t capacity;std::uint32_t size;};
// Fixed-size strings MUST be NUL terminated; descriptor is copied before use.
struct Descriptor {
    std::uint32_t struct_size,version;
    Kind kind;
    Threading threading;
    std::uint32_t max_instances;
    char plugin_id[129],plugin_version[65],build_id[129],platform[16],architecture[16],compiler_abi[65],runtime_variant[16];
};
class Plugin {
public:
    virtual Status initialize(Bytes parameters) noexcept=0;
    // Only this method may be called concurrently with initialize/execute by the host.
    virtual void request_stop() noexcept=0;
protected:
    virtual ~Plugin()=default; // Only module's destroy export owns deletion.
};
class Algorithm : public Plugin {
public:
    // Request and response use versioned JSON. Output is TaskFinished payload (not an authority to change prior terminals).
    virtual Status execute(Bytes request,Buffer& response) noexcept=0;
};
class Camera : public Plugin {
public:
    virtual Status enumerate(Buffer& devices) noexcept=0;
    virtual Status open(Bytes settings) noexcept=0;
    virtual Status trigger(Bytes bounded_write_permit) noexcept=0;
    virtual Status poll_frame(Buffer& descriptor) noexcept=0;
    virtual Status close() noexcept=0;
};
class Communication : public Plugin {
public:
    virtual Status connect(Bytes settings) noexcept=0;
    virtual Status try_command(Bytes command) noexcept=0;
    virtual Status poll_input(Buffer& snapshot) noexcept=0;
};
class ObjectStorage : public Plugin {
public:
    virtual Status try_operation(Bytes operation) noexcept=0;
    virtual Status poll_completion(Buffer& report) noexcept=0;
};
class ResultOutput : public Plugin {
public:
    virtual Status try_submit(Bytes envelope) noexcept=0; // Bounded, no remote ACK wait.
    virtual Status poll_report(Buffer& report) noexcept=0;
};
using Describe=Status(VISION_PLUGIN_CALL*)(Descriptor*,std::uint32_t) noexcept;
using Create=Status(VISION_PLUGIN_CALL*)(Plugin**) noexcept;
using Destroy=Status(VISION_PLUGIN_CALL*)(Plugin*) noexcept;
// Symbols: vision_plugin_describe, vision_plugin_create, vision_plugin_destroy.
// Fixed MSVC ABI + dynamic CRT build identity must match; this is not a cross-compiler ABI.
} // namespace vision::plugin_sdk
