#pragma once
#include <vision/inspection/rules.hpp>

namespace vision::inspection {
struct RecipeCheck {
    contracts::CheckId check_id;
    contracts::CameraId camera_id;
    bool required{true};
    Rule rule;
    std::optional<std::string> model_hash;
};
struct Recipe {
    std::string recipe_id, version, content_hash;
    std::uint64_t deadline_ms{};
    std::string trigger_mode, association, image_policy, traceability;
    std::vector<RecipeCheck> checks;
    std::vector<contracts::OutputId> outputs;
};
} // namespace vision::inspection
