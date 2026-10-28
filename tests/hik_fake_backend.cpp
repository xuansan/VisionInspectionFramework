#include "../plugins/hik-camera/backend.hpp"
#include <vision/plugin_sdk/api.hpp>
#include <vision/frame_transport/mapping.hpp>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <thread>
#include <chrono>
#include <iostream>
#include <array>

extern "C" vision::plugin_sdk::Status VISION_PLUGIN_CALL vision_plugin_create(
    vision::plugin_sdk::Plugin**) noexcept;
extern "C" vision::plugin_sdk::Status VISION_PLUGIN_CALL vision_plugin_destroy(
    vision::plugin_sdk::Plugin*) noexcept;
#define NOMINMAX
#include <windows.h>
namespace vision::hik {
class Fake final:public Backend {
    Config c;bool pending{};
public:
    std::string enumerate() override{return R"({"devices":[{"id":"normal","simulated":true}]})";}
    void open(const Config& v) override{c=v;if(c.serial=="absent")throw std::runtime_error("absent");}
    void trigger() override{pending=true;}
    bool read(std::vector<std::byte>& out) override {
        if(c.serial=="crash")TerminateProcess(GetCurrentProcess(),87);
        if(c.serial=="hang")for(;;)std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if(c.serial=="missing")return false;
        if(c.serial=="disconnect")throw std::runtime_error("disconnect");
        if(!pending)return false;
        out.assign(c.width*c.height*(c.format=="Mono8"?1:3),std::byte{73});
        if(c.serial=="bad-size")out.pop_back();pending=false;return true;
    }
};
std::unique_ptr<Backend> make_backend(){return std::make_unique<Fake>();}
}

