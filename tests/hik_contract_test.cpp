#include <vision/plugin_sdk/api.hpp>
#include <vision/frame_transport/mapping.hpp>
#include <vision/serialization/json_codec.hpp>
#include <nlohmann/json.hpp>
#include <array>
#include <iostream>
using namespace vision;
using namespace plugin_sdk;
using namespace contracts;
extern "C" Status VISION_PLUGIN_CALL vision_plugin_create(Plugin**) noexcept;
extern "C" Status VISION_PLUGIN_CALL vision_plugin_destroy(Plugin*) noexcept;
void check(bool ok,const char* reason){if(!ok)throw std::runtime_error(reason);}
Bytes bytes(const std::string& s){return {reinterpret_cast<const std::uint8_t*>(s.data()),static_cast<std::uint32_t>(s.size())};}
struct Instance {
    Plugin* p{};
    Instance(){check(vision_plugin_create(&p)==Status::Ok,"create");}
    ~Instance(){vision_plugin_destroy(p);}
    Camera& camera(){return *dynamic_cast<Camera*>(p);}
};
int main(){
    try {
        for(const auto* scenario:{"normal","absent","disconnect","bad-size","missing"})
        for(const auto* format:{"Mono8","RGB8","BGR8"}) {
            Instance instance;auto& camera=instance.camera();
            const unsigned channels=std::string(format)=="Mono8"?1:3;
            auto config=nlohmann::json{{"serial",scenario},{"width",8},{"height",4},{"format",format},{"exposure_us",1000},{"gain",1}}.dump();
            check(camera.initialize(bytes(config))==Status::Ok,"initialize");
            check(camera.initialize(bytes(config))==Status::Invalid,"double initialize");
            std::array<std::uint8_t,1024> devices{};
            Buffer enumeration{devices.data(),static_cast<std::uint32_t>(devices.size()),0};
            check(camera.enumerate(enumeration)==Status::Ok&&enumeration.size>0,"enumerate");
            check(nlohmann::json::parse(devices.data(),devices.data()+enumeration.size)["devices"].size()==1,"device list");
            check(camera.open(bytes("bad"))==Status::Failed,"malformed open");
            frame_transport::PoolSpec spec{RunId("hik-contract"),PoolId("pool"),2,4096};
            auto pool=frame_transport::Mapping::create(spec);
            auto settings=nlohmann::json{{"run_id","hik-contract"},{"pool_id","pool"},{"worker_id","hik"},
                {"worker_epoch","1"},{"slot_count",2},{"slot_bytes","4096"}}.dump();
            auto opened=camera.open(bytes(settings));
            if(std::string(scenario)=="absent"){check(opened==Status::Failed,"absent");continue;}
            check(opened==Status::Ok,"open");
            FrameDescriptor frame{{spec.run,spec.pool,{WorkerId("hik"),1},0,1,1},FrameId("frame"),
                {8,4,8*channels,0,32*channels,channels==1?PixelFormat::Mono8:std::string(format)=="RGB8"?PixelFormat::RGB8:PixelFormat::BGR8}};
            check(pool->arm(frame.permit)==frame_transport::MemoryStatus::Ok,"arm");
            auto encoded=serialization::encode_frame(frame,4096);
            check(camera.trigger(bytes(encoded))==Status::Ok,"trigger");
            check(camera.trigger(bytes(encoded))==Status::Busy,"pending backpressure");
            std::array<std::uint8_t,4096> data{};
            Buffer buffer{data.data(),static_cast<std::uint32_t>(data.size()),123};
            auto status=camera.poll_frame(buffer);
            if(std::string(scenario)=="normal") {
                check(status==Status::Ok,"normal poll");
                check(serialization::decode_frame({reinterpret_cast<char*>(data.data()),buffer.size},4096)==frame,"descriptor");
                auto reader=frame_transport::Mapping::open(spec,frame_transport::Access::Reader);
                check(reader->read(frame,0,[&](auto pixels){
                    check(pixels.size()==32*channels,"size");
                    for(auto p:pixels)check(p==std::byte{73},"pixel");
                })==frame_transport::MemoryStatus::Ok,"read pixels");
                check(camera.trigger(bytes(encoded))==Status::Invalid,"duplicate lease");
                check(camera.close()==Status::Ok&&camera.open(bytes(settings))==Status::Ok,"reopen mapping");
                frame.permit.lease=2;frame.permit.generation=2;
                check(pool->arm(frame.permit)==frame_transport::MemoryStatus::Ok,"rearm");
                check(camera.trigger(bytes(serialization::encode_frame(frame,4096)))==Status::Ok,"second trigger");
                Buffer small{data.data(),1,123};
                check(camera.poll_frame(small)==Status::Busy&&small.size==0,"short completion buffer");
                check(camera.poll_frame(buffer)==Status::Ok,"retry completion");
            } else if(std::string(scenario)=="missing") {
                check(status==Status::Busy&&buffer.size==0,"missing");
                check(camera.poll_frame(buffer)==Status::Busy,"missing repeat");
                check(camera.close()==Status::Ok,"close pending");
                check(camera.open(bytes(settings))==Status::Invalid,"unfinished instance poisoned");
            } else {
                check(status==Status::Failed&&buffer.size==0,"bad frame or disconnect");
                check(camera.poll_frame(buffer)==Status::Cancelled,"failure terminal");
            }
            camera.request_stop();
            check(camera.trigger(bytes(encoded))==Status::Cancelled,"stop");
            check(camera.close()==Status::Ok,"close");
        }
        Instance invalid;
        check(invalid.camera().initialize(bytes("{}"))==Status::Invalid,"invalid config");
        check(invalid.camera().initialize(bytes(R"({"serial":"normal","width":0,"height":4,"format":"Mono8","exposure_us":1000,"gain":1})"))==Status::Invalid,"zero width");
        std::cout<<"HIK acquisition, pixels, lifecycle and failures passed\n";
        return 0;
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
