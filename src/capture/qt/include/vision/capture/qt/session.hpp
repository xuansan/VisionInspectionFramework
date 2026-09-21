#pragma once
#include <vision/runtime/qt/process_host.hpp>
#include <vision/runtime/buffer_broker.hpp>
#include <vision/frame_transport/mapping.hpp>

namespace vision::capture::qt {
struct Completion {
    std::string task_id,state,error;
    std::optional<contracts::FrameDescriptor> frame;
};
// Coordinator-thread adapter. Caller must pulse() from its real control loop.
// Completion capacity is one; take_completion() transfers ownership, not frame release.
class Session final {
public:
    Session(std::shared_ptr<runtime::qt::StationGuard> guard,QString host,QString manifest,
        std::string parameters,std::string worker,contracts::ImageLayout layout,std::uint32_t slot_count=2,
        std::string recipe_hash="sha256:"+std::string(64,'a'));
    ~Session();
    bool start();
    void stop();
    void pulse();
    bool capture(std::uint64_t budget_ns);
    void cancel();
    bool ready() const;
    std::optional<Completion> take_completion();
    bool read(const contracts::FrameDescriptor&,const std::function<void(std::span<const std::byte>)>&);
    bool release(const contracts::FrameDescriptor&);
    std::optional<contracts::FrameDescriptor> handoff(const contracts::FrameDescriptor&,
        contracts::FrameOwner,std::uint64_t budget_ns);
    // Caller must certify the archive process has exited before reclaiming its reader lease.
    std::optional<contracts::FrameDescriptor> reclaim_archive(const contracts::FrameDescriptor&,std::uint64_t budget_ns);
    void quarantine(const contracts::FrameDescriptor&);
    bool consumer_stopped(const contracts::FrameDescriptor&); // Caller certifies no further access.
    frame_transport::PoolSpec pool() const;
    runtime::BufferSnapshot buffers() const;
    runtime::WorkerSnapshot worker() const {return host_.snapshot();}
private:
    void message(const ipc::Message&);
    void terminate(std::string state,std::string error);
    void observe_exit();
    contracts::ImageLayout layout_;
    std::uint32_t slots_;
    std::string worker_;
    std::shared_ptr<runtime::SteadyClock> clock_;
    std::unique_ptr<runtime::BufferBroker> broker_;
    std::unique_ptr<frame_transport::Mapping> mapping_,reader_;
    std::optional<contracts::FrameDescriptor> active_;
    std::optional<contracts::Correlation> correlation_;
    std::optional<Completion> completion_;
    std::uint64_t sequence_{},pool_sequence_{},due_{},active_epoch_{};
    bool retiring_{};
    runtime::qt::ProcessHost host_; // Destroy the process before mappings/broker.
};
}
