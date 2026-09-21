#pragma once
#include <vision/algorithm/qt/session.hpp>
namespace vision::application::qt {
struct TrialConfig {
    QString host,camera_manifest,algorithm_manifest,image,model_config,mask_root;
    contracts::ImageLayout layout;
    std::string recipe_hash;
    std::string pixel_hash;
};
// Offline only: one file camera and one algorithm, no PLC, output dispatcher or production counter.
class TrialSession {
public:
    explicit TrialSession(TrialConfig);
    ~TrialSession();
    bool start();
    void pulse();
    void stop();
    bool finished() const;
    std::optional<contracts::CheckResult> take_result();
private:
    std::shared_ptr<runtime::qt::StationGuard> guard_;
    std::unique_ptr<capture::qt::Session> camera_;
    std::unique_ptr<algorithm::qt::Session> algorithm_;
    std::optional<contracts::CheckResult> result_;
    QElapsedTimer elapsed_;
    bool submitted_{},stopping_{};
};
using ReplayService=TrialSession;
}
