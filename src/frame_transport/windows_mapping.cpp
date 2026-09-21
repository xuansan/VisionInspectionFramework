#include <vision/frame_transport/mapping.hpp>
#define NOMINMAX
#include <windows.h>
#include <cstring>
#include <limits>
#include <utility>

namespace vision::frame_transport {
using namespace contracts;
namespace {
constexpr std::uint64_t magic=0x564953494F4E4631ULL;
constexpr std::uint32_t version=1;
std::uint64_t pitch(const PoolSpec& s) {return checked_add(checked_add(sizeof(SlotHeader),s.slot_bytes),63)&~63ULL;}
std::uint64_t total(const PoolSpec& s) {return checked_add(sizeof(PoolHeader),checked_mul(s.slot_count,pitch(s)));}
void validate(const PoolSpec& s) {
    if(s.run.value().size()+s.pool.value().size()>96||!s.slot_count||s.slot_count>4096||!s.slot_bytes||s.slot_bytes>1024ULL*1024*1024||
       total(s)>4ULL*1024*1024*1024||sizeof(std::size_t)<8)
        throw std::invalid_argument("Invalid mapping capacity");
}
std::wstring hex(std::string_view text) {
    constexpr wchar_t digits[]=L"0123456789abcdef";
    std::wstring result;
    for(unsigned char c:text) {result+=digits[c>>4];result+=digits[c&15];}
    return result;
}
void text(char* output,std::string_view input) {std::memset(output,0,129);std::memcpy(output,input.data(),input.size());}
bool same(const char* buffer,const std::string& value) {
    return std::memcmp(buffer,value.data(),value.size())==0&&buffer[value.size()]=='\0';
}
struct Lock {
    HANDLE handle;DWORD status;
    Lock(HANDLE h,DWORD timeout):handle(h),status(WaitForSingleObject(h,timeout)) {}
    ~Lock() {if(status==WAIT_OBJECT_0||status==WAIT_ABANDONED)ReleaseMutex(handle);}
};
}
struct Mapping::Impl {
    PoolSpec spec;
    Access access;
    HANDLE mapping{};
    void* view{};
    std::vector<HANDLE> mutexes;
    std::vector<bool> poisoned;
    Impl(PoolSpec s,Access a):spec(std::move(s)),access(a),poisoned(spec.slot_count,false) {}
    ~Impl() {
        if(view)UnmapViewOfFile(view);
        if(mapping)CloseHandle(mapping);
        for(auto handle:mutexes)CloseHandle(handle);
    }
    SlotHeader* slot(std::uint32_t index) const {
        return reinterpret_cast<SlotHeader*>(static_cast<std::byte*>(view)+sizeof(PoolHeader)+index*pitch(spec));
    }
    bool permit(const FrameLease& p) const {
        return p.run==spec.run&&p.pool==spec.pool&&p.slot<spec.slot_count&&p.generation&&p.lease&&p.owner.epoch;
    }
    MemoryStatus lock_result(const Lock& lock,std::uint32_t index) {
        if(lock.status==WAIT_ABANDONED)poisoned[index]=true;
        if(poisoned[index])return MemoryStatus::Abandoned;
        if(lock.status==WAIT_TIMEOUT)return MemoryStatus::Busy;
        return lock.status==WAIT_OBJECT_0?MemoryStatus::Ok:MemoryStatus::Invalid;
    }
};
std::wstring Mapping::object_name(const PoolSpec& s) {
    // Hex IDs make naming injective; unlike punctuation replacement, different IDs cannot alias.
    return L"Local\\VisionFrame-"+hex(s.run.value())+L"-"+hex(s.pool.value());
}
Mapping::Mapping(std::unique_ptr<Impl> impl):impl_(std::move(impl)) {}
Mapping::~Mapping()=default;
const PoolSpec& Mapping::spec() const {return impl_->spec;}
std::unique_ptr<Mapping> Mapping::create(PoolSpec spec) {
    validate(spec);
    auto impl=std::make_unique<Impl>(std::move(spec),Access::Owner);
    const auto length=total(impl->spec);
    const auto name=object_name(impl->spec);
    impl->mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,
        static_cast<DWORD>(length>>32),static_cast<DWORD>(length),name.c_str());
    const auto error=GetLastError();
    if(!impl->mapping||error==ERROR_ALREADY_EXISTS)throw std::runtime_error("Mapping create collision or permission failure");
    impl->view=MapViewOfFile(impl->mapping,FILE_MAP_ALL_ACCESS,0,0,static_cast<SIZE_T>(length));
    if(!impl->view)throw std::runtime_error("Mapping view failed");
    for(std::uint32_t i=0;i<impl->spec.slot_count;++i) {
        const auto mutex_name=name+L"-slot-"+std::to_wstring(i);
        auto handle=CreateMutexW(nullptr,FALSE,mutex_name.c_str());
        const auto code=GetLastError();
        if(!handle)throw std::runtime_error("Slot mutex create failed");
        impl->mutexes.push_back(handle);
        if(code==ERROR_ALREADY_EXISTS)throw std::runtime_error("Slot mutex collision");
    }
    Lock initialization(impl->mutexes[0],0);
    if(initialization.status!=WAIT_OBJECT_0)throw std::runtime_error("Pool initialization busy");
    auto& header=*static_cast<PoolHeader*>(impl->view);
    header={};header.total_bytes=length;header.slot_bytes=impl->spec.slot_bytes;
    header.slot_pitch=pitch(impl->spec);header.version=version;header.header_bytes=sizeof(PoolHeader);
    header.slot_count=impl->spec.slot_count;text(header.run,impl->spec.run.value());text(header.pool,impl->spec.pool.value());
    header.magic=magic;
    return std::unique_ptr<Mapping>(new Mapping(std::move(impl)));
}
std::unique_ptr<Mapping> Mapping::open(PoolSpec spec,Access access) {
    validate(spec);
    if(access==Access::Owner)throw std::invalid_argument("Owner must create unique pool");
    auto impl=std::make_unique<Impl>(std::move(spec),access);
    const auto name=object_name(impl->spec);
    const DWORD rights=access==Access::Reader?FILE_MAP_READ:FILE_MAP_ALL_ACCESS;
    impl->mapping=OpenFileMappingW(rights,FALSE,name.c_str());
    if(!impl->mapping)throw std::runtime_error("Cannot open expected pool");
    impl->view=MapViewOfFile(impl->mapping,rights,0,0,static_cast<SIZE_T>(total(impl->spec)));
    if(!impl->view)throw std::runtime_error("Unexpected pool mapping size");
    for(std::uint32_t i=0;i<impl->spec.slot_count;++i) {
        auto handle=OpenMutexW(SYNCHRONIZE|MUTEX_MODIFY_STATE,FALSE,(name+L"-slot-"+std::to_wstring(i)).c_str());
        if(!handle)throw std::runtime_error("Cannot open slot synchronization");
        impl->mutexes.push_back(handle);
    }
    Lock initialization(impl->mutexes[0],1000);
    if(initialization.status!=WAIT_OBJECT_0)throw std::runtime_error("Pool initialization unavailable");
    const auto& h=*static_cast<const PoolHeader*>(impl->view);
    if(h.magic!=magic||h.version!=version||h.header_bytes!=sizeof(PoolHeader)||h.reserved||
       h.total_bytes!=total(impl->spec)||h.slot_count!=impl->spec.slot_count||h.slot_bytes!=impl->spec.slot_bytes||
       h.slot_pitch!=pitch(impl->spec)||!same(h.run,impl->spec.run.value())||!same(h.pool,impl->spec.pool.value()))
        throw std::runtime_error("Pool header mismatch");
    return std::unique_ptr<Mapping>(new Mapping(std::move(impl)));
}
MemoryStatus Mapping::arm(const FrameLease& p) {
    auto& i=*impl_;
    if(i.access!=Access::Owner||!i.permit(p))return MemoryStatus::Invalid;
    Lock lock(i.mutexes[p.slot],0);
    const auto result=i.lock_result(lock,p.slot);
    if(result!=MemoryStatus::Ok)return result;
    auto& h=*i.slot(p.slot);
    if(p.generation<=h.generation)return MemoryStatus::Stale;
    h={};h.generation=p.generation;h.write_lease=p.lease;h.state=1;
    return MemoryStatus::Ok;
}
MemoryStatus Mapping::write(const FrameDescriptor& frame,std::span<const std::byte> bytes) {
    auto& i=*impl_;const auto& p=frame.permit;
    if(i.access!=Access::Writer||!i.permit(p))return MemoryStatus::Invalid;
    try {validate_layout(frame.layout,i.spec.slot_bytes);}catch(const std::invalid_argument&) {return MemoryStatus::Invalid;}
    if(bytes.size()!=frame.layout.length)return MemoryStatus::Invalid;
    Lock lock(i.mutexes[p.slot],0);
    const auto result=i.lock_result(lock,p.slot);if(result!=MemoryStatus::Ok)return result;
    auto& h=*i.slot(p.slot);
    if(h.generation!=p.generation||h.write_lease!=p.lease||h.state!=1)return MemoryStatus::Stale;
    std::memcpy(reinterpret_cast<std::byte*>(&h)+sizeof(SlotHeader)+frame.layout.offset,bytes.data(),bytes.size());
    h.length=frame.layout.length;h.stride=frame.layout.stride;h.offset=frame.layout.offset;
    h.width=frame.layout.width;h.height=frame.layout.height;h.format=static_cast<std::uint32_t>(frame.layout.format);
    text(h.frame,frame.frame.value());h.state=2;
    // ReleaseMutex is the publication boundary; reader must acquire this same mutex.
    return MemoryStatus::Ok;
}
MemoryStatus Mapping::read(const FrameDescriptor& frame,std::uint32_t wait,
    const std::function<void(std::span<const std::byte>)>& visitor) {
    auto& i=*impl_;const auto& p=frame.permit;
    if(i.access!=Access::Reader||!i.permit(p)||wait>1000||!visitor)return MemoryStatus::Invalid;
    try {validate_layout(frame.layout,i.spec.slot_bytes);}catch(const std::invalid_argument&) {return MemoryStatus::Invalid;}
    Lock lock(i.mutexes[p.slot],wait);
    const auto result=i.lock_result(lock,p.slot);if(result!=MemoryStatus::Ok)return result;
    const auto& h=*i.slot(p.slot);
    if(h.generation!=p.generation||h.state!=2||h.width!=frame.layout.width||h.height!=frame.layout.height||
       h.format!=static_cast<std::uint32_t>(frame.layout.format)||h.length!=frame.layout.length||
       h.stride!=frame.layout.stride||h.offset!=frame.layout.offset||!same(h.frame,frame.frame.value()))
        return MemoryStatus::Stale;
    visitor({reinterpret_cast<const std::byte*>(&h)+sizeof(SlotHeader)+h.offset,static_cast<std::size_t>(h.length)});
    return MemoryStatus::Ok;
}
} // namespace vision::frame_transport

