#pragma once
#include <vision/application/qt/station.hpp>
#include <vision/application/qt/device.hpp>
#include <vision/application/qt/dispatcher.hpp>
#include <vision/application/qt/durable_session.hpp>
namespace vision::application::qt {
struct DemoPaths {QString host,camera,algorithm,device,output,recipe;};
struct DemoSnapshot {
    StationSnapshot station;
    DeviceSnapshot device;
    std::vector<Delivery> deliveries;
    std::vector<runtime::WorkerSnapshot> workers;
    std::string last_result,physical,error;
    std::uint64_t results{};
    bool finished{};
    DurableSnapshot durable;
};
class DemoRunner {
public:
    DemoRunner(DemoPaths,std::string scenario,unsigned count=3,std::optional<DurableDemoConfig> durable={});
    ~DemoRunner();
    bool start();
    void pulse();
    void pause();
    bool resume();
    void stop();
    DemoSnapshot snapshot() const;
    std::vector<contracts::ResultEnvelope> take_events();
private:
    void send_physical(const contracts::ResultEnvelope&);
    std::shared_ptr<runtime::qt::StationGuard> guard_;
    std::unique_ptr<Station> station_;
    std::unique_ptr<DeviceSession> device_;
    std::unique_ptr<Dispatcher> dispatcher_;
    std::unique_ptr<DurableSession> durable_;
    std::string scenario_,last_result_,physical_,error_;
    std::vector<contracts::ResultEnvelope> events_;
    unsigned target_,results_{},pulse_count_{};
    std::uint64_t arrival_seen_{},deadline_{};
    runtime::SteadyClock clock_;
    bool started_{},stopping_{},finished_{},physical_pending_{},arrival_high_{},commit_pending_{},files_draining_{};
};
}
