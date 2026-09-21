#include <vision/inspection/rules.hpp>
#include <algorithm>
#include <cmath>
#include <set>

namespace vision::inspection {
void validate_rule(const Rule& rule) {
    std::visit([](const auto& r) {
        using T = std::decay_t<decltype(r)>;
        if constexpr(std::is_same_v<T, ForbiddenClass> || std::is_same_v<T, CountRange>) {
            if(r.class_id < 0) throw std::invalid_argument("Negative class ID");
            if constexpr(std::is_same_v<T, CountRange>)
                if(r.minimum > r.maximum) throw std::invalid_argument("Inverted count range");
        } else if constexpr(std::is_same_v<T, AllowedClassification>) {
            if(r.labels.empty() || r.labels.size() > 256) throw std::invalid_argument("Invalid label set");
            std::set<std::string> unique;
            for(const auto& label : r.labels)
                if(label.empty() || label.size() > 256 || !unique.insert(label).second)
                    throw std::invalid_argument("Invalid or duplicate label");
        } else {
            if(r.name.empty() || r.name.size() > 128 || r.unit.empty() || r.unit.size() > 32 ||
               !std::isfinite(r.minimum) || !std::isfinite(r.maximum) || r.minimum > r.maximum ||
               (r.minimum == r.maximum && (!r.include_minimum || !r.include_maximum)))
                throw std::invalid_argument("Invalid measurement range");
        }
    }, rule);
}
RuleDecision evaluate(const Rule& rule, const Observations& o) {
    try { validate_rule(rule); } catch(const std::invalid_argument& e) { return {contracts::QualityVerdict::Unknown, e.what()}; }
    if(o.detections.size() > 4096 || o.measurements.size() > 256)
        return {contracts::QualityVerdict::Unknown, "Observation limit exceeded"};
    for(const auto& d : o.detections)
        if(d.class_id < 0 || d.label.empty() || d.label.size() > 256 ||
           !std::isfinite(d.score) || d.score < 0 || d.score > 1 ||
           !std::isfinite(d.x1) || !std::isfinite(d.y1) || !std::isfinite(d.x2) || !std::isfinite(d.y2) ||
           d.x1 < 0 || d.y1 < 0 || d.x2 <= d.x1 || d.y2 <= d.y1)
            return {contracts::QualityVerdict::Unknown, "Malformed detection"};
    for(const auto& m : o.measurements)
        if(!std::isfinite(m.value) || m.unit.empty() || m.name.empty())
            return {contracts::QualityVerdict::Unknown, "Malformed measurement"};
    return std::visit([&](const auto& r) -> RuleDecision {
        using T = std::decay_t<decltype(r)>;
        bool ok = false;
        if constexpr(std::is_same_v<T, ForbiddenClass> || std::is_same_v<T, CountRange>) {
            const auto count = std::count_if(o.detections.begin(), o.detections.end(),
                [&](const auto& d) { return d.class_id == r.class_id; });
            if constexpr(std::is_same_v<T, ForbiddenClass>) ok = count == 0;
            else ok = count >= r.minimum && count <= r.maximum;
        } else if constexpr(std::is_same_v<T, AllowedClassification>) {
            if(!o.classification || o.classification->empty())
                return {contracts::QualityVerdict::Unknown, "Classification missing"};
            ok = std::find(r.labels.begin(), r.labels.end(), *o.classification) != r.labels.end();
        } else {
            const auto count = std::count_if(o.measurements.begin(), o.measurements.end(),
                [&](const auto& m) { return m.name == r.name; });
            if(count != 1) return {contracts::QualityVerdict::Unknown, "Missing or duplicate measurement"};
            const auto& m = *std::find_if(o.measurements.begin(), o.measurements.end(),
                [&](const auto& value) { return value.name == r.name; });
            if(m.unit != r.unit) return {contracts::QualityVerdict::Unknown, "Measurement unit mismatch"};
            ok = (r.include_minimum ? m.value >= r.minimum : m.value > r.minimum) &&
                 (r.include_maximum ? m.value <= r.maximum : m.value < r.maximum);
        }
        return {ok ? contracts::QualityVerdict::OK : contracts::QualityVerdict::NG, std::nullopt};
    }, rule);
}
} // namespace vision::inspection
