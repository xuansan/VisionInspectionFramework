#pragma once
#include <vision/application/qt/dispatcher.hpp>
namespace vision::application::qt {
struct DurableDemoConfig {QString root,files,sqlite_manifest,file_manifest,agent,archive_program;};
struct DurableSnapshot {
    std::string persistence{"Disabled"},file_output{"Disabled"},error;
    std::uint64_t committed{};
    bool agent_alive{};
    QString root,files;
};
// Application adapter: metadata IO runs in the SQLite worker; file delivery runs in a separate process.
class DurableSession {
public:
    DurableSession(std::shared_ptr<runtime::qt::StationGuard>,QString host,DurableDemoConfig);
    ~DurableSession();
    bool start();
    bool ready() const;
    bool submit(const contracts::ResultEnvelope&);
    void pulse();
    std::optional<contracts::ResultEnvelope> take_committed();
    void drain_files();
    bool files_finished() const;
    void stop();
    DurableSnapshot snapshot() const;
    std::vector<Delivery> deliveries() const {return storage_.deliveries();}
    std::vector<runtime::WorkerSnapshot> workers() const {return storage_.workers();}
private:
    void consume_output();
    std::shared_ptr<runtime::qt::StationGuard> guard_;
    QString host_;
    DurableDemoConfig config_;
    Dispatcher storage_;
    QProcess agent_;
    runtime::SteadyClock clock_;
    std::optional<contracts::ResultEnvelope> pending_,committed_;
    DurableSnapshot state_;
    QByteArray output_;
    std::uint64_t deadline_{};
    bool stopped_{},draining_{},output_overflow_{};
};
}
