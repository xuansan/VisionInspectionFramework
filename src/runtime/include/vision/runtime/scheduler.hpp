#pragma once
#include <vision/contracts/types.hpp>
#include <vision/runtime/resources.hpp>
#include <map>
#include <deque>

namespace vision::runtime {
enum class AdmissionStatus { Accepted, Full, Invalid, NoResources };
struct Admission {
    AdmissionStatus status;
    std::optional<contracts::Correlation> correlation;
};
struct Dispatch {
    contracts::Correlation correlation;
    std::uint64_t remaining_ns;
};
struct TaskTerminal {
    contracts::Correlation correlation;
    contracts::TaskState state;
};
struct TaskEvent {std::uint64_t time_ns;contracts::Correlation correlation;contracts::TaskState state;std::string reason;};
struct SchedulerSnapshot {
    std::size_t retained{},queued{},running{},awaiting_stop{},unconsumed_terminals{};
};
// Single control-loop owner. One executing task per worker/epoch.
// The budget and clock must outlive this scheduler. Destroy only after owned workers exit.
class TaskScheduler {
public:
    TaskScheduler(contracts::RunId run,std::size_t capacity,ResourceBudget& budget,std::shared_ptr<Clock> clock);
    Admission enqueue(contracts::WorkerId worker,std::uint64_t epoch,contracts::InspectionId inspection,
        contracts::CheckId check,const ResourceAmounts& resources,std::uint64_t duration_ns);
    std::optional<Dispatch> dispatch(const contracts::WorkerId& worker,std::uint64_t epoch);
    bool finish(const contracts::Correlation& identity,contracts::TaskState result);
    bool cancel(const contracts::Correlation& identity);
    // Called only with proof of OS process exit, never on a socket disconnect alone.
    void worker_exited(const contracts::WorkerId& worker,std::uint64_t epoch);
    // Failed send before any bytes were accepted; no execution could have started.
    bool dispatch_rejected(const contracts::Correlation& identity);
    void tick();
    std::optional<contracts::Correlation> take_cancel();
    std::optional<TaskTerminal> take_terminal();
    SchedulerSnapshot snapshot() const;
    std::vector<TaskEvent> take_events();
    std::uint64_t dropped_events() const {return dropped_events_;}
private:
    struct Entry {
        contracts::Correlation identity;
        std::uint64_t deadline;
        Reservation reservation;
        contracts::TaskState state{contracts::TaskState::Queued};
        bool execution_stopped{true},reported{},cancel_pending{};
    };
    void terminate(Entry& entry,contracts::TaskState state);
    void collect();
    Entry* find(const contracts::Correlation& identity);
    void record(const Entry& entry,std::string reason);
    contracts::RunId run_;
    std::size_t capacity_;
    ResourceBudget& budget_;
    std::shared_ptr<Clock> clock_;
    std::uint64_t next_id_{};
    std::map<std::uint64_t,Entry> entries_;
    std::deque<TaskEvent> events_;
    std::uint64_t dropped_events_{};
};
} // namespace vision::runtime
