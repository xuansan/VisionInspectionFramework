#pragma once
#include <vision/plugin_sdk/api.hpp>
#include <vision/serialization/json_codec.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <cstring>
namespace vision::outputs {
// One queued request, one report, one IO thread. Never performs IO under the queue mutex.
class AsyncOutput:public plugin_sdk::ResultOutput {
public:
    ~AsyncOutput(){request_stop();if(thread_.joinable())thread_.join();}
    plugin_sdk::Status initialize(plugin_sdk::Bytes bytes) noexcept override {
        try {
            configure(nlohmann::json::parse(bytes.data,bytes.data+bytes.size));
            thread_=std::thread([this]{loop();});return plugin_sdk::Status::Ok;
        }catch(...){return plugin_sdk::Status::Invalid;}
    }
    void request_stop() noexcept override {stopped_=true;cv_.notify_all();}
    plugin_sdk::Status try_submit(plugin_sdk::Bytes bytes) noexcept override {
        try {
            if(!bytes.data||!bytes.size||bytes.size>32768)return plugin_sdk::Status::Invalid;
            const auto request=nlohmann::json::parse(bytes.data,bytes.data+bytes.size);
            (void)serialization::decode_result(request.at("event").dump());
            (void)contracts::OutputId(request.at("output_instance_id").get<std::string>());
            (void)contracts::parse_u64(request.at("attempt").get<std::string>());
            std::lock_guard lock(mutex_);
            if(stopped_)return plugin_sdk::Status::Cancelled;if(busy_||!report_.empty())return plugin_sdk::Status::Busy;
            pending_=request;busy_=true;cv_.notify_one();return plugin_sdk::Status::Ok;
        }catch(...){return plugin_sdk::Status::Invalid;}
    }
    plugin_sdk::Status poll_report(plugin_sdk::Buffer& out) noexcept override {
        std::lock_guard lock(mutex_);
        if(stopped_)return plugin_sdk::Status::Cancelled;if(report_.empty())return plugin_sdk::Status::Busy;
        if(report_.size()>out.capacity)return plugin_sdk::Status::Invalid;
        std::memcpy(out.data,report_.data(),report_.size());out.size=static_cast<std::uint32_t>(report_.size());report_.clear();return plugin_sdk::Status::Ok;
    }
protected:
    virtual void configure(const nlohmann::json&)=0;
    virtual void write(const nlohmann::json&)=0;
    virtual void io_stopped() noexcept {}
    void finish(){request_stop();if(thread_.joinable())thread_.join();}
    std::atomic<bool> stopped_{};
private:
    void loop() {
        struct Cleanup {AsyncOutput* owner;~Cleanup(){owner->io_stopped();}} cleanup{this};
        for(;;) {
            nlohmann::json request;
            {std::unique_lock lock(mutex_);cv_.wait(lock,[&]{return stopped_||!pending_.is_null();});if(stopped_)return;request=std::move(pending_);pending_=nullptr;}
            bool success=true;try{write(request);}catch(...){success=false;}
            const auto report=nlohmann::json{{"event_id",request["event"]["event_id"]},{"output_instance_id",request["output_instance_id"]},
                {"attempt",request["attempt"]},{"state",success?"BusinessAcked":"Failed"},
                {"error_code",success?nlohmann::json(nullptr):nlohmann::json("OUTPUT.WRITE_FAILED")}}.dump();
            {std::lock_guard lock(mutex_);report_=report;busy_=false;}
        }
    }
    std::mutex mutex_;std::condition_variable cv_;std::thread thread_;nlohmann::json pending_;std::string report_;bool busy_{};
};
}
