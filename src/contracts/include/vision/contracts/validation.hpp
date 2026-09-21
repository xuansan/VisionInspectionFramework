#pragma once
#include <vision/contracts/types.hpp>
#include <cmath>
#include <map>
#include <set>

namespace vision::contracts {
inline void validate(const CheckResult& value) {
    if(!terminal(value.state)) throw std::invalid_argument("Check result must be terminal");
    if(value.correlation.worker_epoch == 0 || value.correlation.attempt == 0)
        throw std::invalid_argument("Epoch and attempt must be positive");
    if(value.state == TaskState::Succeeded) {
        if((value.quality != QualityVerdict::OK && value.quality != QualityVerdict::NG) || value.error)
            throw std::invalid_argument("Successful check needs known quality and no error");
    } else if(value.quality != QualityVerdict::Unknown || !value.error) {
        throw std::invalid_argument("Failed check needs Unknown quality and an error");
    }
    if(value.model_hash && !valid_hash(*value.model_hash)) throw std::invalid_argument("Invalid model hash");
    if(value.masks.size()>64)throw std::invalid_argument("Mask count");
    std::uint64_t mask_bytes=0;std::set<std::uint32_t> instances;
    for(const auto& mask:value.masks) {
        const auto size=static_cast<std::uint64_t>(mask.width)*mask.height;mask_bytes+=size;
        if(!valid_hash(mask.hash)||!size||size>1024*1024||mask_bytes>16*1024*1024||
           mask.instance>=value.defects.size()||!instances.insert(mask.instance).second)
            throw std::invalid_argument("Mask reference bounds");
    }
    if(value.defects.size() > 4096 || value.measurements.size() > 256) throw std::invalid_argument("Evidence limit exceeded");
    for(const auto& d : value.defects)
        if(d.class_id < 0 || d.label.empty() || d.label.size() > 256 ||
           !std::isfinite(d.score) || d.score < 0 || d.score > 1 ||
           !std::isfinite(d.x1) || !std::isfinite(d.y1) || !std::isfinite(d.x2) || !std::isfinite(d.y2) ||
           d.x1 < 0 || d.y1 < 0 || d.x2 <= d.x1 || d.y2 <= d.y1)
            throw std::invalid_argument("Invalid defect evidence");
    for(const auto& m : value.measurements)
        if(m.name.empty() || m.name.size() > 128 || m.unit.empty() || m.unit.size() > 32 || !std::isfinite(m.value))
            throw std::invalid_argument("Measurement needs finite value and unit");
    if(value.classification && (value.classification->empty() || value.classification->size() > 256))
        throw std::invalid_argument("Invalid classification");
    if(value.error) {
        const auto& e = *value.error;
        if(e.code.empty() || e.code.size() > 128 || e.origin.empty() || e.origin.size() > 128 ||
           e.message.empty() || e.message.size() > 2048 ||
           (e.vendor_code && e.vendor_code->size() > 256) ||
           e.category < ErrorCategory::Configuration || e.category > ErrorCategory::Internal ||
           e.retryability < Retryability::Never || e.retryability > Retryability::Safe ||
           (e.correlation && *e.correlation != value.correlation))
            throw std::invalid_argument("Invalid error or mismatched correlation");
    }
}
inline void validate(const InspectionResult& r) {
    if(!terminal(r.state) || r.mode < Mode::Demo || r.mode > Mode::Production ||
       !valid_hash(r.recipe_hash) || r.reason.empty() || r.reason.size() > 2048 ||
       r.expected_checks.empty() || r.expected_checks.size() > 64 || r.checks.size() > 64)
        throw std::invalid_argument("Invalid finalized inspection");
    std::map<std::string,bool> expected;
    bool required=false, bad=false, ng=false;
    for(const auto& e:r.expected_checks) {
        if(!expected.emplace(e.check_id.value(),e.required).second) throw std::invalid_argument("Duplicate expected check");
        required |= e.required;
    }
    if(!required) throw std::invalid_argument("No required check");
    std::set<std::string> received;
    for(const auto& c:r.checks) {
        validate(c);
        const auto& key=c.correlation.check_id.value();
        if(c.correlation.run_id!=r.run_id || c.correlation.inspection_id!=r.inspection_id ||
           !expected.contains(key) || !received.insert(key).second) throw std::invalid_argument("Result identity mismatch");
        if(expected.at(key)) { bad |= c.state!=TaskState::Succeeded; ng |= c.quality==QualityVerdict::NG; }
    }
    if(r.state==InspectionState::Completed) {
        for(const auto& [key,req]:expected) if(req && !received.contains(key)) bad=true;
        if(bad || r.quality!=(ng ? QualityVerdict::NG : QualityVerdict::OK))
            throw std::invalid_argument("Inconsistent completed inspection");
    } else if(r.quality!=QualityVerdict::Unknown) throw std::invalid_argument("Failure must be Unknown");
}
} // namespace vision::contracts
