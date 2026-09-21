#include <vision/runtime/resources.hpp>
#include <mutex>
#include <limits>
#include <utility>

namespace vision::runtime {
struct ReservationRecord {
    std::uint64_t id, expires_at;
    ResourceAmounts amounts;
    ReservationState state{ReservationState::Reserved};
};
struct BudgetShared {
    mutable std::mutex mutex;
    ResourceAmounts capacities, used;
    std::map<std::uint64_t, std::shared_ptr<ReservationRecord>> records;
    std::shared_ptr<Clock> clock;
    std::size_t max_tickets{};
    std::uint64_t next_id{1};
};
static void release_locked(BudgetShared& s, ReservationRecord& r, ReservationState final_state) {
    if(r.state == ReservationState::Released || r.state == ReservationState::Expired) return;
    for(const auto& [key, amount] : r.amounts) s.used.at(key) -= amount;
    r.state = final_state;
    s.records.erase(r.id);
}
static void expire_locked(BudgetShared& s) {
    const auto now = s.clock->now_ns();
    for(auto it = s.records.begin(); it != s.records.end();) {
        auto record = it->second;
        ++it;
        if(record->state == ReservationState::Reserved && now >= record->expires_at)
            release_locked(s, *record, ReservationState::Expired);
    }
}
Reservation::Reservation(std::shared_ptr<BudgetShared> s, std::shared_ptr<ReservationRecord> r)
    : owner_(std::move(s)), record_(std::move(r)) {}
Reservation::Reservation(Reservation&& other) noexcept
    : owner_(std::move(other.owner_)), record_(std::move(other.record_)) {}
Reservation& Reservation::operator=(Reservation&& other) noexcept {
    if(this != &other) {
        release();
        owner_ = std::move(other.owner_);
        record_ = std::move(other.record_);
    }
    return *this;
}
Reservation::~Reservation() { release(); }
bool Reservation::commit() {
    if(!owner_ || !record_) return false;
    std::lock_guard lock(owner_->mutex);
    expire_locked(*owner_);
    if(record_->state == ReservationState::Committed) return true;
    if(record_->state != ReservationState::Reserved) return false;
    record_->state = ReservationState::Committed;
    return true;
}
void Reservation::release() noexcept {
    if(!owner_ || !record_) return;
    std::lock_guard lock(owner_->mutex);
    release_locked(*owner_, *record_, ReservationState::Released);
}
ReservationState Reservation::state() const {
    if(!owner_ || !record_) return ReservationState::Released;
    std::lock_guard lock(owner_->mutex);
    expire_locked(*owner_);
    return record_->state;
}
ResourceBudget::ResourceBudget(ResourceAmounts capacities, std::size_t max_tickets, std::shared_ptr<Clock> clock)
    : state_(std::make_shared<BudgetShared>()) {
    if(capacities.empty() || capacities.size() > 64 || !max_tickets || !clock)
        throw std::invalid_argument("Invalid resource budget");
    for(const auto& [key, amount] : capacities)
        if(key.empty() || key.size() > 128 || amount == 0) throw std::invalid_argument("Invalid capacity");
    state_->capacities = std::move(capacities);
    state_->used = state_->capacities;
    for(auto& [key, amount] : state_->used) { (void)key; amount = 0; }
    state_->clock = std::move(clock);
    state_->max_tickets = max_tickets;
}
ReserveResult ResourceBudget::try_reserve(const ResourceAmounts& amounts, std::uint64_t ttl_ns) {
    std::lock_guard lock(state_->mutex);
    expire_locked(*state_);
    const auto now = state_->clock->now_ns();
    if(amounts.empty() || !ttl_ns || ttl_ns > std::numeric_limits<std::uint64_t>::max() - now)
        return {ReserveStatus::InvalidRequest, std::nullopt};
    for(const auto& [key, amount] : amounts) {
        const auto cap = state_->capacities.find(key);
        if(cap == state_->capacities.end() || !amount) return {ReserveStatus::InvalidRequest, std::nullopt};
        if(amount > cap->second - state_->used.at(key)) return {ReserveStatus::CapacityExceeded, std::nullopt};
    }
    if(state_->records.size() >= state_->max_tickets || state_->next_id == std::numeric_limits<std::uint64_t>::max())
        return {ReserveStatus::TicketLimit, std::nullopt};
    auto r = std::make_shared<ReservationRecord>(ReservationRecord{state_->next_id, now + ttl_ns, amounts});
    state_->records.emplace(r->id, r); // Allocate before modifying capacity balances.
    ++state_->next_id;
    for(const auto& [key, amount] : amounts) state_->used.at(key) += amount;
    return {ReserveStatus::Accepted, Reservation(state_, std::move(r))};
}
void ResourceBudget::expire_reserved() { std::lock_guard lock(state_->mutex); expire_locked(*state_); }
BudgetSnapshot ResourceBudget::snapshot() const {
    std::lock_guard lock(state_->mutex);
    expire_locked(*state_);
    return {state_->used, state_->records.size()};
}
} // namespace vision::runtime
