#pragma once
#include <vision/runtime/supervisor.hpp>
#include <vision/ipc/qt/local_connection.hpp>
#include <QLocalServer>
#include <QProcess>
#include <QElapsedTimer>
#include <memory>

namespace vision::runtime::qt {
struct HostEvent {std::uint64_t time_ns;WorkerSnapshot worker;};
// One per coordinator, shared by all hosts. Windows only; lock is machine-wide.
// The job kills owned descendants when the coordinator exits, including abnormal exit.
class StationGuard {
public:
    explicit StationGuard(const QString& station);
    ~StationGuard();
    StationGuard(const StationGuard&)=delete;
    StationGuard& operator=(const StationGuard&)=delete;
    bool attach(qint64 pid);
    const std::string& run_id() const {return run_;}
private:
    std::string run_;
    void* mutex_{};
    void* job_{};
};
class ProcessHost final : public QObject {
public:
    ProcessHost(std::shared_ptr<StationGuard> guard,QString program,QStringList arguments,
        std::string worker,std::string config,SupervisorPolicy policy={},ipc::Limits limits={},QObject* parent=nullptr);
    ~ProcessHost() override;
    bool start();
    void stop();
    bool shutdown_and_wait(); // Final teardown only. False never certifies resource release.
    bool rotate();
    void pulse_control(); // Call only from the actual coordination loop.
    bool set_busy(bool busy);
    ipc::SendStatus submit(const contracts::Correlation& identity,std::string payload,std::uint64_t timeout_ns);
    ipc::SendStatus inspect(const contracts::Correlation& identity,std::string payload,std::uint64_t timeout_ns);
    ipc::SendStatus deliver(const contracts::Correlation& identity,std::string payload,std::uint64_t timeout_ns);
    ipc::SendStatus device(const contracts::Correlation& identity,std::string payload,std::uint64_t timeout_ns);
    ipc::SendStatus capture(const contracts::Correlation& identity,std::string payload,std::uint64_t timeout_ns);
    ipc::SendStatus cancel(const contracts::Correlation& identity);
    bool ready_for_task() const;
    std::function<void(const ipc::Message&)> on_task_message;
    WorkerSnapshot snapshot() const {return supervisor_.snapshot();}
    std::string run_id() const {return run_;}
    std::optional<int> last_exit_code() const {return last_exit_code_;}
    std::vector<HostEvent> take_events();
    std::uint64_t dropped_events() const {return dropped_events_;}
    qint64 process_id() const {return process_?process_->processId():0;}
private:
    ipc::SendStatus submit_request(std::string type,const contracts::Correlation& identity,std::string payload,std::uint64_t timeout_ns);
    std::uint64_t now() const;
    void check();
    void launch();
    void observe();
    std::shared_ptr<StationGuard> guard_;
    QString program_;
    QStringList arguments_;
    std::string run_,worker_,config_,token_;
    Supervisor supervisor_;
    ipc::Limits limits_;
    QLocalServer server_;
    QProcess* process_{};
    ipc::qt::LocalConnection* connection_{};
    QElapsedTimer clock_;
    QTimer timer_;
    std::uint64_t control_sequence_{};
    bool checking_{},destroying_{};
    bool rotation_requested_{};
    std::optional<int> last_exit_code_;
    std::optional<contracts::Correlation> active_task_;
    std::deque<HostEvent> events_;
    WorkerSnapshot last_observed_;
    std::uint64_t dropped_events_{};
};
} // namespace vision::runtime::qt
