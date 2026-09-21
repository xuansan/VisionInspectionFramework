#pragma once
#include <vision/inspection/inspection.hpp>
#include <vision/runtime/resources.hpp>

namespace vision::application {
enum class DemoScenario { Ok, Ng, Failure, Timeout, Overload };
struct DemoOutcome {
    bool admitted{};
    std::string message;
    std::optional<contracts::ResultEnvelope> event;
    inspection::Counts counts;
    runtime::BudgetSnapshot resources;
};
// Synchronous, tiny deterministic synthetic example. Never touches hardware or networks.
DemoOutcome run_demo(DemoScenario scenario);
} // namespace vision::application
