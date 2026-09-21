#pragma once
#include <vision/contracts/validation.hpp>
#include <set>

namespace vision::inspection {
using namespace contracts;
struct ExpectedCheck { Correlation correlation; bool required{true}; };
enum class AcceptStatus { Accepted, Finalized, Duplicate, Stale, UnknownCheck, Invalid, AlreadyFinalized };

// Single owner (application event thread). No callbacks, SDKs, clocks or I/O.
class Inspection {
public:
    Inspection(InspectionResult context, std::vector<ExpectedCheck> expected);
    AcceptStatus accept(const CheckResult& report);
    bool terminate(InspectionState reason, const std::string& message);
    const InspectionResult& snapshot() const noexcept { return result_; }
    std::optional<InspectionResult> take_finalized();
private:
    void aggregate();
    InspectionResult result_;
    std::vector<ExpectedCheck> expected_;
    bool emitted_{false};
};
struct Counts { std::uint64_t total{}, ok{}, ng{}, unknown{}; };
enum class CountStatus { Counted, Duplicate, Full, Invalid };
class ResultCounter {
public:
    explicit ResultCounter(std::size_t capacity) : capacity_(capacity) {
        if(!capacity) throw std::invalid_argument("Counter capacity must be positive");
    }
    CountStatus add(const InspectionResult& result);
    Counts snapshot() const noexcept { return counts_; }
private:
    std::size_t capacity_;
    std::set<std::pair<std::string, std::string>> seen_;
    Counts counts_;
};
} // namespace vision::inspection
