#include <vision/inspection/inspection.hpp>
#include <algorithm>

namespace vision::inspection {
Inspection::Inspection(InspectionResult context, std::vector<ExpectedCheck> expected)
    : result_(std::move(context)), expected_(std::move(expected)) {
    if(expected_.empty() || expected_.size() > 64 || !valid_hash(result_.recipe_hash) ||
       !result_.checks.empty() || !result_.expected_checks.empty() || result_.state != InspectionState::Pending ||
       result_.quality != QualityVerdict::Unknown || result_.mode < Mode::Demo || result_.mode > Mode::Production)
        throw std::invalid_argument("Invalid new inspection context");
    std::set<std::string> ids;
    bool required = false;
    for(const auto& e : expected_) {
        if(e.correlation.run_id != result_.run_id || e.correlation.inspection_id != result_.inspection_id ||
           !e.correlation.attempt || !e.correlation.worker_epoch ||
           !ids.insert(e.correlation.check_id.value()).second)
            throw std::invalid_argument("Invalid or duplicate expected check");
        required |= e.required;
        result_.expected_checks.push_back({e.correlation.check_id, e.required});
    }
    if(!required) throw std::invalid_argument("At least one required check");
    result_.state = InspectionState::Running;
}
AcceptStatus Inspection::accept(const CheckResult& report) {
    if(terminal(result_.state)) return AcceptStatus::AlreadyFinalized;
    const auto expected = std::find_if(expected_.begin(), expected_.end(),
        [&](const auto& e) { return e.correlation.check_id == report.correlation.check_id; });
    if(expected == expected_.end()) return AcceptStatus::UnknownCheck;
    if(expected->correlation != report.correlation) return AcceptStatus::Stale;
    if(std::any_of(result_.checks.begin(), result_.checks.end(),
        [&](const auto& r) { return r.correlation.check_id == report.correlation.check_id; }))
        return AcceptStatus::Duplicate;
    try { validate(report); } catch(const std::invalid_argument&) { return AcceptStatus::Invalid; }
    result_.checks.push_back(report);
    aggregate();
    return terminal(result_.state) ? AcceptStatus::Finalized : AcceptStatus::Accepted;
}
void Inspection::aggregate() {
    bool failed = false, timed_out = false, cancelled = false, ng = false;
    for(const auto& e : expected_) {
        if(!e.required) continue;
        const auto found = std::find_if(result_.checks.begin(), result_.checks.end(),
            [&](const auto& r) { return r.correlation.check_id == e.correlation.check_id; });
        if(found == result_.checks.end()) return;
        timed_out |= found->state == TaskState::TimedOut;
        failed |= found->state == TaskState::Failed;
        cancelled |= found->state == TaskState::Cancelled;
        ng |= found->quality == QualityVerdict::NG;
    }
    result_.state = timed_out ? InspectionState::TimedOut : failed ? InspectionState::Failed :
        cancelled ? InspectionState::Cancelled : InspectionState::Completed;
    result_.quality = (failed || timed_out || cancelled) ? QualityVerdict::Unknown :
        ng ? QualityVerdict::NG : QualityVerdict::OK;
    result_.reason = result_.state == InspectionState::Completed ? "AllRequiredChecksSucceeded" : "RequiredCheckUnsuccessful";
}
bool Inspection::terminate(InspectionState reason, const std::string& message) {
    if(terminal(result_.state)) return false;
    if((reason != InspectionState::TimedOut && reason != InspectionState::Cancelled && reason != InspectionState::Failed) ||
       message.empty() || message.size() > 2048)
        throw std::invalid_argument("Invalid termination");
    for(const auto& e : expected_) {
        if(std::any_of(result_.checks.begin(), result_.checks.end(),
           [&](const auto& r) { return r.correlation.check_id == e.correlation.check_id; })) continue;
        const auto state = reason == InspectionState::TimedOut ? TaskState::TimedOut :
            reason == InspectionState::Cancelled ? TaskState::Cancelled : TaskState::Failed;
        CheckResult missing{e.correlation};
        missing.state = state;
        missing.error = Error{reason == InspectionState::TimedOut ? "TASK.DEADLINE_EXCEEDED" :
            reason == InspectionState::Cancelled ? "TASK.CANCELLED" : "TASK.INTERRUPTED",
            ErrorCategory::Execution, message, Retryability::Never, "inspection", e.correlation, std::nullopt};
        result_.checks.push_back(std::move(missing));
    }
    result_.state = reason;
    result_.quality = QualityVerdict::Unknown;
    result_.reason = message;
    return true;
}
std::optional<InspectionResult> Inspection::take_finalized() {
    if(!terminal(result_.state) || emitted_) return std::nullopt;
    std::optional<InspectionResult> copy{result_};
    emitted_ = true;
    return copy;
}
CountStatus ResultCounter::add(const InspectionResult& result) {
    try { validate(result); } catch(const std::invalid_argument&) { return CountStatus::Invalid; }
    const auto key = std::make_pair(result.run_id.value(), result.inspection_id.value());
    if(seen_.contains(key)) return CountStatus::Duplicate;
    if(seen_.size() >= capacity_) return CountStatus::Full;
    seen_.insert(key);
    ++counts_.total;
    if(result.quality == QualityVerdict::OK) ++counts_.ok;
    else if(result.quality == QualityVerdict::NG) ++counts_.ng;
    else ++counts_.unknown;
    return CountStatus::Counted;
}
} // namespace vision::inspection
