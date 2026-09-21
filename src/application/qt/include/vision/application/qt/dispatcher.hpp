#pragma once
#include <vision/runtime/qt/process_host.hpp>
#include <map>
namespace vision::application::qt {
struct OutputConfig {std::string id,parameters{"{}"};QString manifest;std::size_t capacity{8};};
struct Delivery {std::string event_id,output_id,state,error;std::uint64_t attempt{};};
// Bounded, volatile Demo outbox. Snapshot persistence is deliberately external (T24).
class Dispatcher {
public:
    Dispatcher(std::shared_ptr<runtime::qt::StationGuard>,QString host,std::vector<OutputConfig>);
    ~Dispatcher();
    bool start();
    bool submit(const contracts::ResultEnvelope&,std::uint64_t ttl_ns=1000000000);
    void pulse();
    void stop();
    bool ready() const;
    bool idle() const;
    std::vector<Delivery> deliveries() const;
    std::vector<runtime::WorkerSnapshot> workers() const;
    std::string checkpoint() const;
    bool restore(std::string_view); // Before start only; never restores physical outputs.
private:
    struct Intent {Delivery delivery;std::uint64_t expires_utc{},due{};};
    struct Record {contracts::ResultEnvelope event;std::vector<Intent> intents;};
    struct Lane {
        OutputConfig config;
        std::unique_ptr<runtime::qt::ProcessHost> host;
        std::optional<contracts::Correlation> active;
        std::string event;
        std::uint64_t due{};
        bool failed{};
    };
    void message(std::size_t,const ipc::Message&);
    void failed(std::size_t);
    runtime::SteadyClock clock_;
    std::vector<Lane> lanes_;
    std::map<std::string,Record> records_;
    std::uint64_t sequence_{};
    bool started_{},stopped_{};
};
}
