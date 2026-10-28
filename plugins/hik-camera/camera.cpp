#include "backend.hpp"
#include <vision/plugin_sdk/api.hpp>
#include <vision/frame_transport/mapping.hpp>
#include <vision/serialization/json_codec.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <cstring>
#include <set>
#include <cmath>
#include <optional>
#include <stdexcept>
using namespace vision;
using namespace plugin_sdk;
namespace {
using J=nlohmann::json;
J parse(Bytes b) {
    if(!b.data||!b.size||b.size>16384)throw std::invalid_argument("HIK JSON size");
    std::vector<std::set<std::string>> keys;
    return J::parse(b.data,b.data+b.size,[&](int depth,J::parse_event_t event,J& v){
        if(depth>6)throw std::invalid_argument("HIK depth");
        if(event==J::parse_event_t::object_start)keys.emplace_back();
        if(event==J::parse_event_t::key&&!keys.back().insert(v.get<std::string>()).second)throw std::invalid_argument("HIK duplicate");
        if(event==J::parse_event_t::object_end)keys.pop_back();return true;
    });
}
void fields(const J& j,std::initializer_list<const char*> names) {
    std::set<std::string> allowed;for(auto s:names)allowed.insert(s);
    if(!j.is_object()||j.size()!=allowed.size())throw std::invalid_argument("HIK fields");
    for(auto it=j.begin();it!=j.end();++it)if(!allowed.contains(it.key()))throw std::invalid_argument("HIK field");
}
unsigned uint(const J& v,unsigned max) {
    if(!v.is_number_integer()||v.get<std::int64_t>()<=0||v.get<std::uint64_t>()>max)throw std::invalid_argument("HIK range");
    return v.get<unsigned>();
}
Status output(Buffer& b,const std::string& s) {
    b.size=0;if(!b.data||s.size()>b.capacity)return Status::Busy;
    std::memcpy(b.data,s.data(),s.size());b.size=static_cast<std::uint32_t>(s.size());return Status::Ok;
}
class HikCamera final:public Camera {
public:
    Status initialize(Bytes b) noexcept override {
        try {
            if(backend_)return Status::Invalid;
            auto j=parse(b);fields(j,{"serial","width","height","format","exposure_us","gain"});
            config_.serial=j.at("serial").get<std::string>();config_.format=j.at("format");
            if(config_.serial.empty()||config_.serial.size()>64||config_.serial.find('\0')!=std::string::npos)return Status::Invalid;
            config_.width=uint(j.at("width"),4096);config_.height=uint(j.at("height"),4096);
            const auto channels=config_.format=="Mono8"?1:config_.format=="RGB8"||config_.format=="BGR8"?3:0;
            if(!channels)return Status::Invalid;
            if(!j["exposure_us"].is_number()||!j["gain"].is_number())return Status::Invalid;
            config_.exposure=j["exposure_us"].get<float>();config_.gain=j["gain"].get<float>();
            if(!std::isfinite(config_.exposure)||config_.exposure<=0||config_.exposure>1000000||!std::isfinite(config_.gain)||config_.gain<0||config_.gain>100)return Status::Invalid;
            layout_={config_.width,config_.height,config_.width*static_cast<unsigned>(channels),0,
                static_cast<std::uint64_t>(config_.width)*config_.height*channels,
                channels==1?contracts::PixelFormat::Mono8:config_.format=="RGB8"?contracts::PixelFormat::RGB8:contracts::PixelFormat::BGR8};
            contracts::validate_layout(layout_,16*1024*1024);
            backend_=hik::make_backend();return Status::Ok;
        }catch(...){return Status::Invalid;}
    }
    void request_stop() noexcept override {stopped_=true;}
    Status enumerate(Buffer& b) noexcept override {
        try{return backend_?output(b,backend_->enumerate()):Status::Invalid;}catch(...){return Status::Failed;}
    }
    Status open(Bytes b) noexcept override {
        try {
            if(!backend_||mapping_||stopped_)return Status::Invalid;
            auto j=parse(b);fields(j,{"run_id","pool_id","worker_id","worker_epoch","slot_count","slot_bytes"});
            frame_transport::PoolSpec spec{contracts::RunId(j.at("run_id").get<std::string>()),contracts::PoolId(j.at("pool_id").get<std::string>()),
                uint(j.at("slot_count"),16),contracts::parse_u64(j.at("slot_bytes").get<std::string>())};
            const auto epoch=contracts::parse_u64(j.at("worker_epoch").get<std::string>());if(!epoch)return Status::Invalid;
            contracts::validate_layout(layout_,spec.slot_bytes);
            auto owner=contracts::FrameOwner{contracts::WorkerId(j.at("worker_id").get<std::string>()),epoch};
            auto map=frame_transport::Mapping::open(spec,frame_transport::Access::Writer);
            if(!opened_){backend_->open(config_);opened_=true;}
            mapping_=std::move(map);owner_=owner;return Status::Ok;
        }catch(...){return Status::Failed;}
    }
    Status trigger(Bytes b) noexcept override {
        try {
            if(stopped_)return Status::Cancelled;if(!mapping_)return Status::Invalid;if(pending_)return Status::Busy;
            if(!b.data||!b.size||b.size>max_payload)return Status::Invalid;
            auto f=serialization::decode_frame({reinterpret_cast<const char*>(b.data),b.size},mapping_->spec().slot_bytes);
            if(f.permit.run!=mapping_->spec().run||f.permit.pool!=mapping_->spec().pool||f.permit.owner!=*owner_||
                f.permit.slot>=mapping_->spec().slot_count||f.layout!=layout_||f.permit.lease<=last_lease_)return Status::Invalid;
            last_lease_=f.permit.lease;pending_=f;backend_->trigger();return Status::Ok;
        }catch(...){stopped_=true;return Status::Failed;}
    }
    Status poll_frame(Buffer& b) noexcept override {
        b.size=0;
        try {
            if(stopped_)return Status::Cancelled;if(!pending_||!mapping_)return Status::Busy;
            if(completion_.empty()) {
                if(pixels_.empty()&&!backend_->read(pixels_))return Status::Busy;
                if(pixels_.size()!=layout_.length)throw std::runtime_error("HIK frame size");
                if(stopped_)return Status::Cancelled;
                auto s=mapping_->write(*pending_,pixels_);
                if(s==frame_transport::MemoryStatus::Busy)return Status::Busy;
                if(s!=frame_transport::MemoryStatus::Ok)throw std::runtime_error("HIK slot");
                completion_=serialization::encode_frame(*pending_,mapping_->spec().slot_bytes);pixels_.clear();
            }
            auto s=output(b,completion_);if(s==Status::Ok){completion_.clear();pending_.reset();}return s;
        }catch(...){stopped_=true;return Status::Failed;}
    }
    Status close() noexcept override {
        // No asynchronous SDK callback accesses the mapping. Unfinished acquisition poisons this instance.
        if(pending_)stopped_=true;
        pending_.reset();pixels_.clear();completion_.clear();mapping_.reset();owner_.reset();return Status::Ok;
    }
private:
    hik::Config config_;contracts::ImageLayout layout_{};
    std::unique_ptr<hik::Backend> backend_;bool opened_{};std::atomic<bool> stopped_{};
    std::unique_ptr<frame_transport::Mapping> mapping_;std::optional<contracts::FrameOwner> owner_;
    std::optional<contracts::FrameDescriptor> pending_;std::vector<std::byte> pixels_;std::string completion_;std::uint64_t last_lease_{};
};
template<std::size_t N>void copy(char(&out)[N],const char* s){if(std::strlen(s)<N)std::memcpy(out,s,std::strlen(s)+1);}
}
VISION_PLUGIN_EXPORT Status VISION_PLUGIN_CALL vision_plugin_describe(Descriptor* d,std::uint32_t size) noexcept {
    if(!d||size!=sizeof(*d))return Status::Invalid;*d={};d->struct_size=size;d->version=api_version;d->kind=Kind::Camera;d->threading=Threading::Serialized;d->max_instances=1;
    copy(d->plugin_id,"vision.hik-camera");copy(d->plugin_version,"0.1.0");copy(d->build_id,"hik-camera-v1");copy(d->platform,"windows");copy(d->architecture,"x64");copy(d->compiler_abi,"msvc-v143-md");
#ifdef _DEBUG
    copy(d->runtime_variant,"Debug");
#else
    copy(d->runtime_variant,"Release");
#endif
    return Status::Ok;
}
VISION_PLUGIN_EXPORT Status VISION_PLUGIN_CALL vision_plugin_create(Plugin** p) noexcept {if(!p)return Status::Invalid;try{*p=new HikCamera;return Status::Ok;}catch(...){*p=nullptr;return Status::Failed;}}
VISION_PLUGIN_EXPORT Status VISION_PLUGIN_CALL vision_plugin_destroy(Plugin* p) noexcept {auto* c=dynamic_cast<HikCamera*>(p);if(!c)return Status::Invalid;c->close();delete c;return Status::Ok;}
