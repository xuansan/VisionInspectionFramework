#pragma once
#include <vision/runtime/qt/process_host.hpp>
#include <set>
namespace vision::application::qt {
struct DeviceSnapshot {bool online{},ready{},position{},safety_ok{};std::uint64_t arrival_sequence{};};
struct PhysicalReport {std::string event_id;contracts::DeliveryState state;};
class DeviceSession {
public:
    DeviceSession(std::shared_ptr<runtime::qt::StationGuard>,QString host,QString manifest,std::string parameters);
    ~DeviceSession();
    bool start();
    void stop();
    void pulse(bool ready,bool arrival,bool position=true,bool safety_ok=true);
    bool submit(const contracts::ResultEnvelope&);
    DeviceSnapshot snapshot() const;
    std::optional<PhysicalReport> take_report();
    runtime::WorkerSnapshot worker() const {return host_.snapshot();}
private:
    void message(const ipc::Message&);
    void fail();
    runtime::SteadyClock clock_;
    std::string session_;
    std::optional<contracts::Correlation> active_;
    std::optional<std::pair<std::string,std::string>> pending_;
    std::optional<PhysicalReport> report_;
    std::set<std::string> sent_;
    std::uint64_t sequence_{},due_{},fresh_{},next_{};
    DeviceSnapshot snapshot_;
    bool sent_pending_{},faulted_{};
    runtime::qt::ProcessHost host_;
};
}
