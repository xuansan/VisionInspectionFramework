#pragma once
#include <vision/contracts/frame.hpp>
#include <memory>
#include <span>
#include <functional>

namespace vision::frame_transport {
struct PoolSpec {
    contracts::RunId run;
    contracts::PoolId pool;
    std::uint32_t slot_count;
    std::uint64_t slot_bytes;
};
enum class Access { Owner, Writer, Reader };
enum class MemoryStatus { Ok, Busy, Abandoned, Invalid, Stale };
// Explicit wire layout. No pointers/STL/atomics are stored in shared memory.
struct alignas(8) PoolHeader {
    std::uint64_t magic,total_bytes,slot_bytes,slot_pitch;
    std::uint32_t version,header_bytes,slot_count,reserved;
    char run[129],pool[129];
    std::uint8_t padding[14];
};
struct alignas(8) SlotHeader {
    std::uint64_t generation,write_lease,length,stride,offset;
    std::uint32_t state,width,height,format;
    char frame[129];
    std::uint8_t padding[7];
};
static_assert(sizeof(PoolHeader)==320);
static_assert(sizeof(SlotHeader)==192);
// Each object must be used by one owning thread. Reader maps bytes with FILE_MAP_READ.
// read() visitor must not retain spans or release its broker lease before returning.
class Mapping {
public:
    static std::unique_ptr<Mapping> create(PoolSpec spec);
    static std::unique_ptr<Mapping> open(PoolSpec spec,Access access);
    ~Mapping();
    Mapping(const Mapping&)=delete;
    Mapping& operator=(const Mapping&)=delete;
    MemoryStatus arm(const contracts::FrameLease& writer);
    MemoryStatus write(const contracts::FrameDescriptor& writer,std::span<const std::byte> bytes);
    MemoryStatus read(const contracts::FrameDescriptor& reader,std::uint32_t wait_ms,
        const std::function<void(std::span<const std::byte>)>& visitor);
    const PoolSpec& spec() const;
    static std::wstring object_name(const PoolSpec& spec);
private:
    struct Impl;
    explicit Mapping(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
// Owns a bounded copy; never holds a formal frame lease.
class LatestPreview {
public:
    explicit LatestPreview(std::size_t max_bytes):limit_(max_bytes) {
        if(!limit_)throw std::invalid_argument("Empty preview budget");
    }
    bool replace(std::span<const std::byte> bytes) {
        if(bytes.size()>limit_)return false;
        bytes_.assign(bytes.begin(),bytes.end());return true;
    }
    std::span<const std::byte> bytes() const {return bytes_;}
private:
    std::size_t limit_;
    std::vector<std::byte> bytes_;
};
} // namespace vision::frame_transport

