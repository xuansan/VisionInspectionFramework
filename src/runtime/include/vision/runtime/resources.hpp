#pragma once
#include <vision/runtime/clock.hpp>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace vision::runtime {
using ResourceAmounts = std::map<std::string, std::uint64_t>;
enum class ReservationState { Reserved, Committed, Released, Expired };
enum class ReserveStatus { Accepted, InvalidRequest, CapacityExceeded, TicketLimit };
struct BudgetShared;
struct ReservationRecord;
class Reservation {
public:
    Reservation(const Reservation&) = delete;
    Reservation& operator=(const Reservation&) = delete;
    Reservation(Reservation&&) noexcept;
    Reservation& operator=(Reservation&&) noexcept;
    ~Reservation();
    bool commit();
    void release() noexcept;
    ReservationState state() const;
private:
    friend class ResourceBudget;
    Reservation(std::shared_ptr<BudgetShared>, std::shared_ptr<ReservationRecord>);
    std::shared_ptr<BudgetShared> owner_;
    std::shared_ptr<ReservationRecord> record_;
};
struct ReserveResult { ReserveStatus status; std::optional<Reservation> ticket; };
struct BudgetSnapshot { ResourceAmounts used; std::size_t active_tickets{}; };
class ResourceBudget {
public:
    ResourceBudget(ResourceAmounts capacities, std::size_t max_tickets, std::shared_ptr<Clock> clock);
    ReserveResult try_reserve(const ResourceAmounts& amounts, std::uint64_t ttl_ns);
    void expire_reserved();
    BudgetSnapshot snapshot() const;
private:
    std::shared_ptr<BudgetShared> state_;
};
} // namespace vision::runtime
