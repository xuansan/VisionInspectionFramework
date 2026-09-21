#pragma once
#include <vision/contracts/types.hpp>
#include <variant>

namespace vision::inspection {
struct ForbiddenClass { std::int32_t class_id{}; };
struct CountRange { std::int32_t class_id{}; std::uint32_t minimum{}, maximum{}; };
struct AllowedClassification { std::vector<std::string> labels; };
struct MeasurementRange {
    std::string name, unit;
    double minimum{}, maximum{};
    bool include_minimum{true}, include_maximum{true};
};
using Rule = std::variant<ForbiddenClass, CountRange, AllowedClassification, MeasurementRange>;
struct Observations {
    std::vector<contracts::Defect> detections;
    std::optional<std::string> classification;
    std::vector<contracts::Measurement> measurements;
};
struct RuleDecision {
    contracts::QualityVerdict quality{contracts::QualityVerdict::Unknown};
    std::optional<std::string> error;
};
void validate_rule(const Rule& rule);
RuleDecision evaluate(const Rule& rule, const Observations& observations);
} // namespace vision::inspection
