#pragma once
#include <vision/capture/qt/session.hpp>
namespace vision::algorithm::qt {
struct Completion {contracts::Correlation correlation;std::string state,quality,error;std::optional<contracts::CheckResult> evidence;};
// Source must outlive this adapter; all calls belong to the coordinator thread.
class Session final {
public:
    Session(std::shared_ptr<runtime::qt::StationGuard>,QString host,QString manifest,
        std::string parameters,std::string worker,capture::qt::Session& source,
        std::string recipe_hash="sha256:"+std::string(64,'a'));
    ~Session();
    bool start();
    void stop();
    void pulse();
    bool ready() const;
    contracts::Correlation next_identity(contracts::InspectionId,contracts::CheckId) const;
    bool inspect(const contracts::FrameDescriptor&,contracts::InspectionId,contracts::CheckId,std::uint64_t budget_ns);
    void cancel();
    std::optional<Completion> take_completion();
    runtime::WorkerSnapshot worker() const {return host_->snapshot();}
private:
    void message(const ipc::Message&);
    void terminate(std::string state,std::string error);
    void observe_exit();
    capture::qt::Session& source_;
    std::string worker_;
    std::shared_ptr<runtime::SteadyClock> clock_;
    std::optional<contracts::FrameDescriptor> frame_;
    std::optional<contracts::Correlation> identity_;
    std::optional<Completion> completion_;
    std::uint64_t due_{},sequence_{};
    bool retiring_{};
    std::unique_ptr<runtime::qt::ProcessHost> host_;
};
}
