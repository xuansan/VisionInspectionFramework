#include <vision/runtime/buffer_broker.hpp>
#include <algorithm>
#include <set>

namespace vision::runtime {
using namespace contracts;
BufferBroker::BufferBroker(RunId run,PoolId pool,std::uint32_t count,std::uint64_t bytes,
    std::size_t readers,std::shared_ptr<Clock> clock)
    :run_(std::move(run)),pool_(std::move(pool)),bytes_(bytes),max_readers_(readers),clock_(std::move(clock)) {
    if(!count||count>4096||!bytes||bytes>1024ULL*1024*1024||!readers||readers>64||!clock_)
        throw std::invalid_argument("Invalid buffer capacity");
    if(checked_mul(count,bytes)>4ULL*1024*1024*1024)throw std::invalid_argument("Pool exceeds 4GiB");
    slots_.resize(count);
}
BufferBroker::Slot* BufferBroker::match(const FrameLease& lease) {
    if(lease.run!=run_||lease.pool!=pool_||lease.slot>=slots_.size())return nullptr;
    auto& slot=slots_[lease.slot];
    if(slot.generation!=lease.generation)return nullptr;
    const auto it=slot.holders.find(lease.lease);
    return it!=slot.holders.end()&&it->second.permit==lease?&slot:nullptr;
}
void BufferBroker::free_if_empty(Slot& slot) {
    if(slot.holders.empty()) {slot.phase=SlotPhase::Free;slot.frame.reset();}
}
void BufferBroker::tick() {
    const auto now=clock_->now_ns();
    for(auto& slot:slots_)for(const auto& [id,h]:slot.holders) {
        (void)id;
        if(now>=h.due) {slot.phase=SlotPhase::Retiring;break;}
    }
}
WriteGrant BufferBroker::acquire(FrameOwner writer,std::uint64_t ttl) {
    tick();
    if(!writer.epoch||!ttl||ttl>UINT64_MAX-clock_->now_ns()||next_lease_==UINT64_MAX)
        return {LeaseStatus::Invalid,{}};
    for(std::uint32_t i=0;i<slots_.size();++i) {
        auto& slot=slots_[i];
        if(slot.phase!=SlotPhase::Free||slot.generation==UINT64_MAX)continue;
        FrameLease lease{run_,pool_,std::move(writer),i,slot.generation+1,next_lease_+1};
        slot.holders.emplace(lease.lease,Holding{lease,deadline_after(*clock_,ttl)});
        ++next_lease_;++slot.generation;slot.phase=SlotPhase::Writing;
        std::size_t used=0;for(const auto& s:slots_)if(s.phase!=SlotPhase::Free)++used;
        high_water_=std::max(high_water_,used);
        return {LeaseStatus::Accepted,std::move(lease)};
    }
    return {LeaseStatus::Full,{}};
}
PublishGrant BufferBroker::publish(const FrameLease& writer,FrameId frame,ImageLayout layout,
    const std::vector<FrameOwner>& readers,std::uint64_t ttl) {
    tick();
    auto slot=match(writer);
    if(!slot)return {LeaseStatus::Stale,{}};
    if(slot->phase==SlotPhase::Retiring)return {LeaseStatus::Expired,{}};
    if(slot->phase!=SlotPhase::Writing)return {LeaseStatus::Stale,{}};
    if(readers.empty()||readers.size()>max_readers_||!ttl||ttl>UINT64_MAX-clock_->now_ns()||
       readers.size()>UINT64_MAX-next_lease_)return {LeaseStatus::Invalid,{}};
    try {validate_layout(layout,bytes_);}catch(const std::invalid_argument&) {return {LeaseStatus::Invalid,{}};}
    std::set<std::pair<std::string,std::uint64_t>> unique;
    for(const auto& reader:readers)
        if(!reader.epoch||!unique.emplace(reader.worker.value(),reader.epoch).second)return {LeaseStatus::Invalid,{}};
    std::map<std::uint64_t,Holding> pending;
    std::vector<FrameDescriptor> result;
    auto next=next_lease_;
    const auto due=deadline_after(*clock_,ttl);
    for(const auto& reader:readers) {
        FrameLease lease{run_,pool_,reader,writer.slot,writer.generation,++next};
        pending.emplace(next,Holding{lease,due});result.push_back({lease,frame,layout});
    }
    // Allocate every consumer first: failure cannot publish a partially granted frame.
    slot->frame=std::move(frame);slot->layout=layout;slot->holders=std::move(pending);
    next_lease_=next;slot->phase=SlotPhase::Published;
    return {LeaseStatus::Accepted,std::move(result)};
}
bool BufferBroker::release(const FrameLease& lease) {
    auto slot=match(lease);if(!slot)return false;
    slot->holders.erase(lease.lease);free_if_empty(*slot);return true;
}
bool BufferBroker::authorized(const FrameDescriptor& frame) {
    tick();auto slot=match(frame.permit);
    return slot&&slot->phase==SlotPhase::Published&&slot->frame==frame.frame&&slot->layout==frame.layout;
}
std::optional<FrameDescriptor> BufferBroker::transfer(const FrameDescriptor& frame,FrameOwner reader,std::uint64_t ttl) {
    if(!authorized(frame)||!reader.epoch||!ttl||ttl>UINT64_MAX-clock_->now_ns()||next_lease_==UINT64_MAX)return {};
    auto slot=match(frame.permit);
    auto next=frame;
    next.permit.owner=std::move(reader);next.permit.lease=next_lease_+1;
    slot->holders.emplace(next.permit.lease,Holding{next.permit,deadline_after(*clock_,ttl)});
    slot->holders.erase(frame.permit.lease);++next_lease_;
    return next;
}
void BufferBroker::quarantine(const FrameLease& lease) {
    if(auto slot=match(lease))slot->phase=SlotPhase::Retiring;
}
void BufferBroker::worker_exited(const FrameOwner& owner) {
    for(auto& slot:slots_) {
        bool removed=false;
        for(auto it=slot.holders.begin();it!=slot.holders.end();) {
            if(it->second.permit.owner==owner) {it=slot.holders.erase(it);removed=true;}
            else ++it;
        }
        if(removed) {slot.phase=SlotPhase::Retiring;free_if_empty(slot);}
    }
}
BufferSnapshot BufferBroker::snapshot() const {
    BufferSnapshot result;result.high_water=high_water_;
    for(const auto& s:slots_) {
        result.leases+=s.holders.size();
        switch(s.phase) {
        case SlotPhase::Free:++result.free;break;
        case SlotPhase::Writing:++result.writing;break;
        case SlotPhase::Published:++result.published;break;
        case SlotPhase::Retiring:++result.retiring;break;
        }
    }
    return result;
}
std::optional<SlotSnapshot> BufferBroker::inspect(std::uint32_t index) const {
    if(index>=slots_.size())return {};
    const auto& s=slots_[index];
    SlotSnapshot result{s.phase,s.generation,{}};
    for(const auto& [id,h]:s.holders) {(void)id;result.holders.push_back(h.permit);}
    return result;
}
} // namespace vision::runtime
