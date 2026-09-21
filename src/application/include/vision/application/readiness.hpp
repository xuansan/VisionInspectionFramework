#pragma once
#include <vision/contracts/types.hpp>
#include <array>
#include <vector>
namespace vision::application {
// Application policy only: evidence comes from adapters, never from the supervisor's heartbeat alone.
enum class Requirement : std::size_t {
    CameraValidated, PlcValidated, ControlLease, Safety, Position,
    RecipeSnapshot, DurableResults, RecoveryReconciled, RequiredOutputs, Count
};
inline constexpr std::array<const char*,static_cast<std::size_t>(Requirement::Count)> requirement_names{
    "camera.validation","plc.validation","control.lease","safety","position",
    "recipe.snapshot","results.durable","recovery.reconciled","outputs.required"};
struct ReadinessEvidence {
    std::string run,recipe_hash;
    std::uint64_t epoch{},observed_ns{},expires_ns{};
    bool satisfied{};
};
struct ReadinessDecision {
    bool prerequisites_ready{};
    std::vector<std::string> blockers;
};
class Readiness {
public:
    Readiness(std::string run,std::string recipe,std::uint64_t epoch)
        :run_(std::move(run)),recipe_(std::move(recipe)),epoch_(epoch) {
        (void)contracts::RunId(run_);
        if(!epoch_||recipe_.empty())throw std::invalid_argument("Readiness identity");
    }
    bool observe(Requirement requirement,ReadinessEvidence evidence) {
        const auto index=static_cast<std::size_t>(requirement);
        if(index>=evidence_.size()||evidence.run!=run_||evidence.recipe_hash!=recipe_||
            evidence.epoch!=epoch_||evidence.expires_ns<=evidence.observed_ns||
            evidence.observed_ns<evidence_[index].observed_ns)return false;
        evidence_[index]=std::move(evidence);return true;
    }
    ReadinessDecision evaluate(std::uint64_t now) const {
        ReadinessDecision decision;
        for(std::size_t i=0;i<evidence_.size();++i){
            const auto& item=evidence_[i];
            if(!item.satisfied||item.run!=run_||now<item.observed_ns||now>=item.expires_ns)
                decision.blockers.emplace_back(requirement_names[i]);
        }
        decision.prerequisites_ready=decision.blockers.empty();return decision;
    }
private:
    std::string run_,recipe_;std::uint64_t epoch_;
    std::array<ReadinessEvidence,static_cast<std::size_t>(Requirement::Count)> evidence_{};
};
// Fail closed until the real hardware and durable orchestration acceptance is complete.
inline constexpr bool production_integration_validated=false;
}
