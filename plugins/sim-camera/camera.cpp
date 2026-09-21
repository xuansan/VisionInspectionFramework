#include <vision/plugin_sdk/api.hpp>
#include <vision/capture/source.hpp>
#include <vision/frame_transport/mapping.hpp>
#include <vision/serialization/json_codec.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <cstring>
#include <set>
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

using namespace vision;
using namespace plugin_sdk;
namespace {
using J=nlohmann::json;
J parse(Bytes bytes) {
    if(!bytes.data||!bytes.size||bytes.size>max_payload)throw std::invalid_argument("Camera JSON size");
    std::vector<std::set<std::string>> keys;
    return J::parse(bytes.data,bytes.data+bytes.size,[&](int depth,J::parse_event_t event,J& value) {
        if(depth>8)throw std::invalid_argument("Camera JSON depth");
        if(event==J::parse_event_t::object_start)keys.emplace_back();
        if(event==J::parse_event_t::key&&!keys.back().insert(value.get<std::string>()).second)
            throw std::invalid_argument("Camera duplicate key");
        if(event==J::parse_event_t::object_end)keys.pop_back();
        return true;
    });
}
void fields(const J& j,std::initializer_list<const char*> names,bool required) {
    if(!j.is_object())throw std::invalid_argument("Camera object required");
    std::set<std::string> allowed;for(auto name:names)allowed.insert(name);
    for(auto it=j.begin();it!=j.end();++it)if(!allowed.contains(it.key()))throw std::invalid_argument("Unknown camera field");
    if(required&&j.size()!=allowed.size())throw std::invalid_argument("Missing camera field");
}
std::uint32_t number(const J& j,const char* key,std::uint32_t fallback,std::uint32_t limit) {
    if(!j.contains(key))return fallback;
    const auto& value=j.at(key);
    if(!value.is_number_unsigned()&&!(value.is_number_integer()&&value.get<std::int64_t>()>=0))
        throw std::invalid_argument("Camera unsigned number required");
    const auto n=value.get<std::uint64_t>();if(n>limit)throw std::invalid_argument("Camera number limit");
    return static_cast<std::uint32_t>(n);
}
std::string text(const J& j,const char* key,std::string fallback,std::size_t limit=4096) {
    if(!j.contains(key))return fallback;
    auto value=j.at(key).get<std::string>();
    if(value.size()>limit||value.find('\0')!=std::string::npos)throw std::invalid_argument("Camera string limit");
    return value;
}
Status output(Buffer& buffer,const std::string& value) {
    buffer.size=0;
    if(!buffer.data||value.size()>buffer.capacity)return Status::Busy;
    std::memcpy(buffer.data,value.data(),value.size());buffer.size=static_cast<std::uint32_t>(value.size());return Status::Ok;
}
class SimCamera final:public Camera {
public:
    Status initialize(Bytes parameters) noexcept override {
        try {
            if(source_)return Status::Invalid;
            auto j=parse(parameters);
            fields(j,{"source","root","files_json","width","height","channels","seed","fault","fault_at","delay_ms","pixel_hash"},false);
            pixel_hash_=text(j,"pixel_hash","");
            if(!pixel_hash_.empty()&&!contracts::valid_hash(pixel_hash_))return Status::Invalid;
            capture::SourceConfig config;
            const auto mode=text(j,"source","synthetic");
            if(mode=="fixed")config.kind=capture::SourceKind::Fixed;
            else if(mode=="sequence")config.kind=capture::SourceKind::Sequence;
            else if(mode!="synthetic")return Status::Invalid;
            config.width=number(j,"width",64,4096);config.height=number(j,"height",48,4096);
            config.channels=number(j,"channels",1,3);config.seed=number(j,"seed",1,UINT32_MAX);
            const auto root=text(j,"root","");config.root=std::filesystem::path(std::u8string(root.begin(),root.end()));
            const auto list=text(j,"files_json","[]",32768);
            const auto files=parse({reinterpret_cast<const std::uint8_t*>(list.data()),static_cast<std::uint32_t>(list.size())});
            if(!files.is_array()||files.size()>256)return Status::Invalid;
            for(const auto& item:files) {
                const auto path=item.get<std::string>();
                if(path.empty()||path.size()>4096||path.find('\0')!=std::string::npos)return Status::Invalid;
                config.files.emplace_back(std::u8string(path.begin(),path.end()));
            }
            fault_=text(j,"fault","none");
            const std::set<std::string> faults{"none","delay","missing","duplicate","out_of_order","disconnect","crash"};
            if(!faults.contains(fault_))return Status::Invalid;
            fault_at_=contracts::parse_u64(text(j,"fault_at","1",20));
            delay_=number(j,"delay_ms",50,10000);
            if(!fault_at_)return Status::Invalid;
            source_=std::make_unique<capture::Source>(std::move(config));return Status::Ok;
        } catch(...) {return Status::Invalid;}
    }
    void request_stop() noexcept override {stopped_=true;}
    Status enumerate(Buffer& devices) noexcept override {
        try {return source_?output(devices,R"({"devices":[{"id":"sim-camera-0","simulated":true}]})"):Status::Invalid;}
        catch(...) {return Status::Failed;}
    }
    Status open(Bytes settings) noexcept override {
        try {
            if(!source_||mapping_)return Status::Invalid;
            const auto j=parse(settings);
            fields(j,{"run_id","pool_id","worker_id","worker_epoch","slot_count","slot_bytes"},true);
            frame_transport::PoolSpec spec{contracts::RunId(text(j,"run_id","",128)),
                contracts::PoolId(text(j,"pool_id","",128)),number(j,"slot_count",0,4096),
                contracts::parse_u64(text(j,"slot_bytes","",20))};
            const auto epoch=contracts::parse_u64(text(j,"worker_epoch","",20));if(!epoch)return Status::Invalid;
            auto owner=contracts::FrameOwner{contracts::WorkerId(text(j,"worker_id","",128)),epoch};
            contracts::validate_layout(source_->layout(),spec.slot_bytes);
            auto mapping=frame_transport::Mapping::open(spec,frame_transport::Access::Writer);
            owner_=std::move(owner);mapping_=std::move(mapping);stopped_=false;disconnected_=false;
            pending_.reset();completion_.clear();return Status::Ok;
        } catch(...) {return Status::Invalid;}
    }
    Status trigger(Bytes permit) noexcept override {
        try {
            if(!mapping_||disconnected_)return Status::Failed;
            if(stopped_)return Status::Cancelled;
            if(pending_)return Status::Busy;
            if(!permit.data||!permit.size||permit.size>max_payload||sequence_==UINT64_MAX)return Status::Invalid;
            auto frame=serialization::decode_frame({reinterpret_cast<const char*>(permit.data),permit.size},mapping_->spec().slot_bytes);
            if(frame.permit.run!=mapping_->spec().run||frame.permit.pool!=mapping_->spec().pool||
               frame.permit.owner!=*owner_||frame.permit.slot>=mapping_->spec().slot_count||frame.layout!=source_->layout())
                return Status::Invalid;
            // Monotonically increasing grants prevent accidental replay/rewrite by the same session.
            if(frame.permit.lease<=last_lease_)return Status::Invalid;
            last_lease_=frame.permit.lease;pending_=std::move(frame);++sequence_;
            due_=std::chrono::steady_clock::now()+std::chrono::milliseconds(sequence_==fault_at_&&fault_=="delay"?delay_:0);
            return Status::Ok;
        } catch(...) {return Status::Invalid;}
    }
    Status poll_frame(Buffer& descriptor) noexcept override {
        descriptor.size=0;
        try {
            if(!mapping_||disconnected_)return Status::Failed;
            if(stopped_) {pending_.reset();completion_.clear();return Status::Cancelled;}
            if(!pending_)return Status::Busy;
            if(std::chrono::steady_clock::now()<due_)return Status::Busy;
            if(completion_.empty()) {
                const bool inject=sequence_==fault_at_;
                if(inject&&fault_=="missing")return Status::Busy;
                if(inject&&fault_=="disconnect") {pending_.reset();disconnected_=true;return Status::Failed;}
                if(inject&&fault_=="crash")TerminateProcess(GetCurrentProcess(),86);
                if(inject&&(fault_=="duplicate"||fault_=="out_of_order")) {
                    const auto back=fault_=="duplicate"?1U:2U;
                    if(history_.size()<back) {pending_.reset();return Status::Invalid;}
                    completion_=history_[history_.size()-back];
                } else {
                    auto image=source_->read(sequence_);
                    if(!pixel_hash_.empty()) {
                        unsigned char digest[32]{};
                        if(BCryptHash(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,reinterpret_cast<PUCHAR>(image.pixels.data()),
                            static_cast<ULONG>(image.pixels.size()),digest,32)<0)throw std::runtime_error("Image hash");
                        constexpr char hex[]="0123456789abcdef";std::string hash="sha256:";
                        for(auto value:digest){hash+=hex[value>>4];hash+=hex[value&15];}
                        if(hash!=pixel_hash_)throw std::runtime_error("Replay pixel hash mismatch");
                    }
                    if(stopped_) {pending_.reset();return Status::Cancelled;}
                    const auto written=mapping_->write(*pending_,image.pixels);
                    if(written==frame_transport::MemoryStatus::Busy)return Status::Busy;
                    if(written!=frame_transport::MemoryStatus::Ok) {pending_.reset();return Status::Failed;}
                    completion_=serialization::encode_frame(*pending_,mapping_->spec().slot_bytes);
                }
            }
            if(stopped_) {pending_.reset();completion_.clear();return Status::Cancelled;}
            const auto status=output(descriptor,completion_);
            if(status==Status::Ok) {
                if(history_.size()==2)history_.erase(history_.begin());
                history_.push_back(completion_);completion_.clear();pending_.reset();
            }
            return status;
        } catch(...) {pending_.reset();completion_.clear();return Status::Failed;}
    }
    Status close() noexcept override {
        pending_.reset();completion_.clear();mapping_.reset();owner_.reset();
        stopped_=true;disconnected_=false;last_lease_=0;return Status::Ok;
    }
private:
    std::string pixel_hash_;
    std::unique_ptr<capture::Source> source_;
    std::unique_ptr<frame_transport::Mapping> mapping_;
    std::optional<contracts::FrameOwner> owner_;
    std::optional<contracts::FrameDescriptor> pending_;
    std::atomic<bool> stopped_{};
    bool disconnected_{};
    std::uint64_t sequence_{},fault_at_{1},last_lease_{};
    std::uint32_t delay_{};
    std::string fault_,completion_;
    std::vector<std::string> history_; // At most two descriptors, never image buffers.
    std::chrono::steady_clock::time_point due_;
};
template<std::size_t N> void copy(char(&out)[N],const char* value) {
    const auto size=std::strlen(value);if(size<N)std::memcpy(out,value,size+1);
}
}
VISION_PLUGIN_EXPORT Status VISION_PLUGIN_CALL vision_plugin_describe(Descriptor* out,std::uint32_t size) noexcept {
    if(!out||size!=sizeof(Descriptor))return Status::Invalid;
    *out={};out->struct_size=size;out->version=api_version;out->kind=Kind::Camera;
    out->threading=Threading::Serialized;out->max_instances=1;
    copy(out->plugin_id,"vision.sim-camera");copy(out->plugin_version,"0.1.0");copy(out->build_id,"sim-camera-v1");
    copy(out->platform,"windows");copy(out->architecture,"x64");copy(out->compiler_abi,"msvc-v143-md");
#ifdef _DEBUG
    copy(out->runtime_variant,"Debug");
#else
    copy(out->runtime_variant,"Release");
#endif
    return Status::Ok;
}
VISION_PLUGIN_EXPORT Status VISION_PLUGIN_CALL vision_plugin_create(Plugin** out) noexcept {
    if(!out)return Status::Invalid;
    try {*out=new SimCamera;return Status::Ok;}catch(...) {*out=nullptr;return Status::Failed;}
}
VISION_PLUGIN_EXPORT Status VISION_PLUGIN_CALL vision_plugin_destroy(Plugin* plugin) noexcept {
    auto camera=dynamic_cast<SimCamera*>(plugin);if(!camera)return Status::Invalid;
    camera->close();delete camera;return Status::Ok;
}
