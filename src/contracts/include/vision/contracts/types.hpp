#pragma once
#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <optional>

namespace vision::contracts {
template<class Tag> class Id {
public:
    explicit Id(std::string value) : value_(std::move(value)) {
        if(value_.empty() || value_.size() > 128)
            throw std::invalid_argument("ID must have 1..128 ASCII identifier characters");
        for(const unsigned char c : value_)
            if(!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':'))
                throw std::invalid_argument("Invalid ID character");
    }
    const std::string& value() const noexcept { return value_; }
    bool operator==(const Id&) const = default;
private:
    std::string value_;
};
#define VISION_ID(Name) struct Name##Tag {}; using Name = Id<Name##Tag>;
VISION_ID(RunId) VISION_ID(WorkerId) VISION_ID(WorkpieceId) VISION_ID(InspectionId)
VISION_ID(CheckId) VISION_ID(TaskId) VISION_ID(FrameId) VISION_ID(EventId)
VISION_ID(TriggerId) VISION_ID(StationId) VISION_ID(CameraId) VISION_ID(PluginId)
VISION_ID(PoolId) VISION_ID(LeaseId) VISION_ID(OutputId)
#undef VISION_ID

inline std::uint64_t parse_u64(std::string_view text) {
    if(text.empty() || (text.size() > 1 && text.front() == '0'))
        throw std::invalid_argument("Expected canonical uint64 decimal string");
    for(char c : text) if(c < '0' || c > '9')
        throw std::invalid_argument("Expected uint64 digits");
    std::uint64_t result{};
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), result);
    if(ec != std::errc{} || end != text.data() + text.size())
        throw std::invalid_argument("uint64 overflow");
    return result;
}
inline bool valid_hash(std::string_view value) {
    if(value.size() != 71 || !value.starts_with("sha256:")) return false;
    for(char c : value.substr(7)) if(!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
enum class TaskState { Queued, Running, Succeeded, Failed, Cancelled, TimedOut };
enum class InspectionState { Pending, Running, Completed, Failed, Cancelled, TimedOut };
enum class QualityVerdict { Unknown, OK, NG };
enum class PersistenceState { Pending, Durable, Failed };
enum class ImageState { NotRequested, Pending, Available, Failed, Deleted };
enum class DeliveryState { Pending, Accepted, DurablyQueued, TransportConfirmed, BusinessAcked, Failed, Expired, Unknown };
enum class Mode { Demo, Replay, Production };
enum class ErrorCategory { Configuration, Protocol, Device, Execution, Resource, Persistence, Delivery, Internal };
enum class Retryability { Never, AfterRecovery, Safe };

struct Correlation {
    RunId run_id;
    WorkerId worker_id;
    std::uint64_t worker_epoch{};
    InspectionId inspection_id;
    CheckId check_id;
    TaskId task_id;
    std::uint64_t attempt{};
    bool operator==(const Correlation&) const = default;
};
struct Error {
    std::string code;
    ErrorCategory category{ErrorCategory::Internal};
    std::string message;
    Retryability retryability{Retryability::Never};
    std::string origin;
    std::optional<Correlation> correlation;
    std::optional<std::string> vendor_code;
};
inline bool terminal(TaskState s) {
    return s == TaskState::Succeeded || s == TaskState::Failed ||
           s == TaskState::Cancelled || s == TaskState::TimedOut;
}
inline bool terminal(InspectionState s) {
    return s == InspectionState::Completed || s == InspectionState::Failed ||
           s == InspectionState::Cancelled || s == InspectionState::TimedOut;
}
inline std::string_view name(QualityVerdict v) {
    switch(v) { case QualityVerdict::Unknown: return "Unknown"; case QualityVerdict::OK: return "OK"; case QualityVerdict::NG: return "NG"; }
    throw std::invalid_argument("Unknown quality");
}
inline std::string_view name(TaskState v) {
    switch(v) { case TaskState::Queued:return "Queued"; case TaskState::Running:return "Running";
    case TaskState::Succeeded:return "Succeeded"; case TaskState::Failed:return "Failed";
    case TaskState::Cancelled:return "Cancelled"; case TaskState::TimedOut:return "TimedOut"; }
    throw std::invalid_argument("Unknown task state");
}
inline std::string_view name(InspectionState v) {
    switch(v) { case InspectionState::Pending:return "Pending"; case InspectionState::Running:return "Running";
    case InspectionState::Completed:return "Completed"; case InspectionState::Failed:return "Failed";
    case InspectionState::Cancelled:return "Cancelled"; case InspectionState::TimedOut:return "TimedOut"; }
    throw std::invalid_argument("Unknown inspection state");
}
struct Defect {
    std::int32_t class_id{};
    std::string label;
    double score{};
    double x1{}, y1{}, x2{}, y2{};
};
struct Measurement { std::string name; double value{}; std::string unit; };
struct MaskReference {
    std::string hash;
    std::uint32_t width{},height{},instance{};
};
struct CheckResult {
    Correlation correlation;
    TaskState state{TaskState::Failed};
    QualityVerdict quality{QualityVerdict::Unknown};
    std::vector<Defect> defects;
    std::vector<Measurement> measurements;
    std::optional<std::string> classification;
    std::optional<Error> error;
    std::uint64_t elapsed_ns{};
    std::optional<std::string> model_hash;
    std::vector<MaskReference> masks;
};
struct CheckSpec { CheckId check_id; bool required{true}; };
struct InspectionResult {
    RunId run_id;
    InspectionId inspection_id;
    WorkpieceId workpiece_id;
    StationId station_id;
    Mode mode{Mode::Demo};
    std::string recipe_hash;
    InspectionState state{InspectionState::Pending};
    QualityVerdict quality{QualityVerdict::Unknown};
    std::vector<CheckSpec> expected_checks;
    std::vector<CheckResult> checks;
    std::string reason;
};
struct ResultEnvelope {
    std::uint32_t schema_version{1};
    EventId event_id;
    std::uint64_t sequence{};
    std::uint64_t emitted_at_unix_ns{};
    InspectionResult result;
};
} // namespace vision::contracts
