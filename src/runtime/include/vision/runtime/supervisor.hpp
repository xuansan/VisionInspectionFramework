#pragma once
#include <vision/runtime/clock.hpp>
#include <optional>
#include <string>

namespace vision::runtime {
enum class WorkerPhase { Stopped, Starting, Handshaking, Initializing, Ready, Draining, Stopping, Backoff, ManualIntervention };
enum class ProcessAction { Launch, Stop, Kill };
struct SupervisorPolicy {
    std::uint64_t stage_ns{2000000000}, heartbeat_ns{1000000000}, progress_ns{2000000000};
    std::uint64_t stop_ns{500000000}, backoff_ns{1000000000}, max_backoff_ns{4000000000};
    unsigned restart_limit{3};
};
struct WorkerSnapshot {
    WorkerPhase phase{WorkerPhase::Stopped};
    std::uint64_t epoch{};
    unsigned restarts{};
    bool process_alive{}, busy{};
    std::string reason;
};
// One control-loop owner. poll() returns at most one OS action; the adapter must execute it.
// An epoch is scoped by the adapter's unique run_id.
class Supervisor {
public:
    explicit Supervisor(SupervisorPolicy policy={});
    bool start(std::uint64_t now);
    void spawned(std::uint64_t epoch,std::uint64_t now);
    void authenticated(std::uint64_t epoch,std::uint64_t now);
    void initialized(std::uint64_t epoch,std::uint64_t now);
    void heartbeat(std::uint64_t epoch,std::uint64_t sequence,std::uint64_t now);
    void progress(std::uint64_t epoch,std::uint64_t sequence,std::uint64_t now);
    bool set_busy(std::uint64_t epoch,bool busy,std::uint64_t now);
    void fault(std::uint64_t epoch,std::string reason,std::uint64_t now,bool recoverable=true);
    void exited(std::uint64_t epoch,std::uint64_t now,bool clean=true);
    void stop(std::uint64_t now);
    bool rotate(std::uint64_t now); // Only after application and IPC have drained.
    std::optional<ProcessAction> poll(std::uint64_t now);
    WorkerSnapshot snapshot() const {return state_;}
private:
    void launch(std::uint64_t now);
    void stopping(std::uint64_t now,bool retry,bool graceful);
    bool current(std::uint64_t epoch) const;
    SupervisorPolicy policy_;
    WorkerSnapshot state_;
    std::uint64_t due_{},heartbeat_due_{},progress_due_{},heartbeat_seq_{},progress_seq_{};
    bool retry_{},rotation_{},kill_sent_{},fatal_{};
    std::optional<ProcessAction> action_;
};

// TTL is negotiated by configuration, never extended using a remote absolute clock.
// The first expired observation permanently revokes this instance. Recovery needs a new epoch.
class ControlLease {
public:
    ControlLease(std::string run,std::uint64_t epoch,std::uint64_t ttl_ns);
    bool renew(const std::string& run,std::uint64_t epoch,std::uint64_t sequence,std::uint64_t now);
    bool valid(std::uint64_t now);
private:
    std::string run_;
    std::uint64_t epoch_,ttl_,sequence_{},due_{};
    bool started_{},revoked_{};
};
} // namespace vision::runtime
