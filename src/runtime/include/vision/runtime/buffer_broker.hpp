#pragma once
#include <vision/contracts/frame.hpp>
#include <vision/runtime/clock.hpp>
#include <map>
#include <memory>

namespace vision::runtime {
enum class SlotPhase { Free, Writing, Published, Retiring };
enum class LeaseStatus { Accepted, Full, Invalid, Stale, Expired };
struct WriteGrant {LeaseStatus status;std::optional<contracts::FrameLease> lease;};
struct PublishGrant {LeaseStatus status;std::vector<contracts::FrameDescriptor> readers;};
struct BufferSnapshot {
    std::size_t free{},writing{},published{},retiring{},leases{},high_water{};
};
struct SlotSnapshot {
    SlotPhase phase;
    std::uint64_t generation;
    std::vector<contracts::FrameLease> holders;
};
// Single coordinator thread owns the ledger; trusted OS-exit facts come from Supervisor.
class BufferBroker {
public:
    BufferBroker(contracts::RunId run,contracts::PoolId pool,std::uint32_t slots,
        std::uint64_t slot_bytes,std::size_t max_readers,std::shared_ptr<Clock> clock);
    WriteGrant acquire(contracts::FrameOwner writer,std::uint64_t ttl_ns);
    PublishGrant publish(const contracts::FrameLease& writer,contracts::FrameId frame,contracts::ImageLayout layout,
        const std::vector<contracts::FrameOwner>& readers,std::uint64_t ttl_ns);
    bool release(const contracts::FrameLease& lease); // Caller certifies it no longer accesses bytes.
    bool authorized(const contracts::FrameDescriptor& frame);
    // Old reader certifies access has stopped. Atomically replaces its permission.
    std::optional<contracts::FrameDescriptor> transfer(const contracts::FrameDescriptor&,
        contracts::FrameOwner reader,std::uint64_t ttl_ns);
    void quarantine(const contracts::FrameLease& lease);
    void worker_exited(const contracts::FrameOwner& owner);
    void tick();
    BufferSnapshot snapshot() const;
    std::optional<SlotSnapshot> inspect(std::uint32_t index) const;
private:
    struct Holding {contracts::FrameLease permit;std::uint64_t due;};
    struct Slot {
        SlotPhase phase{SlotPhase::Free};
        std::uint64_t generation{};
        std::map<std::uint64_t,Holding> holders;
        std::optional<contracts::FrameId> frame;
        contracts::ImageLayout layout;
    };
    Slot* match(const contracts::FrameLease& lease);
    void free_if_empty(Slot& slot);
    contracts::RunId run_;
    contracts::PoolId pool_;
    std::uint64_t bytes_,next_lease_{};
    std::size_t max_readers_,high_water_{};
    std::shared_ptr<Clock> clock_;
    std::vector<Slot> slots_;
};
} // namespace vision::runtime
