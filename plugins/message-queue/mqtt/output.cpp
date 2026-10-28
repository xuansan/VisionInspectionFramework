#include "../../output-common/async.hpp"
#include <MQTTAsync.h>
#include <charconv>
#include <set>
#include <vector>
namespace {
using J=nlohmann::json;
class Output final:public vision::outputs::AsyncOutput {
public:
    ~Output(){finish();}
private:
    void configure(const J& j) override {
        if(j.size()!=5)throw std::invalid_argument("MQTT parameters");
        endpoint_=j.at("endpoint").get<std::string>();
        constexpr std::string_view prefix="tcp://127.0.0.1:";
        if(!endpoint_.starts_with(prefix))throw std::invalid_argument("MQTT loopback broker required");
        const auto port=std::string_view(endpoint_).substr(prefix.size());
        unsigned value=0;const auto parsed=std::from_chars(port.data(),port.data()+port.size(),value);
        if(port.empty()||parsed.ec!=std::errc{}||parsed.ptr!=port.data()+port.size()||value<1||value>65535)
            throw std::invalid_argument("MQTT port");
        auto text=[&](const char* field,std::size_t maximum){
            auto value=j.at(field).get<std::string>();
            if(value.empty()||value.size()>maximum)throw std::invalid_argument("MQTT identifier");
            for(unsigned char c:value)if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||
                c=='-'||c=='_'||c=='.'||c=='/'))throw std::invalid_argument("MQTT identifier character");
            return value;
        };
        publish_=text("publish_topic",128);ack_=text("ack_topic",128);client_id_=text("client_id",64);
        if(publish_==ack_)throw std::invalid_argument("MQTT distinct topics");
        const auto& timeout=j.at("timeout_ms");
        if(!timeout.is_number_integer()||timeout.get<std::int64_t>()<100||timeout.get<std::int64_t>()>800)
            throw std::invalid_argument("MQTT timeout");
        timeout_=timeout.get<unsigned>();
    }
    static void success(void* context,MQTTAsync_successData*) {
        auto& self=*static_cast<Output*>(context);std::lock_guard lock(self.mutex_);self.phase_done_=true;self.cv_.notify_all();
    }
    static void failure(void* context,MQTTAsync_failureData*) {
        auto& self=*static_cast<Output*>(context);std::lock_guard lock(self.mutex_);self.failed_=true;self.cv_.notify_all();
    }
    static void lost(void* context,char*) {
        failure(context,nullptr);
    }
    static int received(void* context,char* topic,int topic_length,MQTTAsync_message* message) {
        auto& self=*static_cast<Output*>(context);
        {
            std::lock_guard lock(self.mutex_);
            try {
                const auto length=topic_length?static_cast<std::size_t>(topic_length):std::strlen(topic);
                if(length>128||std::string_view(topic,length)!=self.ack_||message->retained||
                    message->payloadlen<=0||message->payloadlen>4096)throw std::runtime_error("MQTT ack bounds");
                std::vector<std::set<std::string>> keys;
                const auto* begin=static_cast<const char*>(message->payload);
                const auto ack=J::parse(begin,begin+message->payloadlen,[&](int depth,J::parse_event_t event,J& value){
                    if(depth>2)throw std::runtime_error("MQTT ack depth");
                    if(event==J::parse_event_t::object_start)keys.emplace_back();
                    if(event==J::parse_event_t::key&&!keys.back().insert(value.get<std::string>()).second)
                        throw std::runtime_error("MQTT duplicate key");
                    if(event==J::parse_event_t::object_end)keys.pop_back();
                    return true;
                });
                if(ack.size()!=4||ack.at("event_id")!=self.identity_.at("event").at("event_id")||
                    ack.at("output_instance_id")!=self.identity_.at("output_instance_id")||
                    ack.at("attempt")!=self.identity_.at("attempt")||ack.at("state")!="BusinessAcked")
                    throw std::runtime_error("MQTT business ack identity");
                self.business_acked_=true;
            }catch(...){self.failed_=true;}
            self.cv_.notify_all();
        }
        MQTTAsync_freeMessage(&message);MQTTAsync_free(topic);return 1;
    }
    void write(const J& request) override {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeout_);
        {std::lock_guard lock(mutex_);identity_=request;failed_=false;business_acked_=false;phase_done_=false;}
        struct Client {MQTTAsync handle{};~Client(){if(handle)MQTTAsync_destroy(&handle);}} client;
        MQTTAsync_createOptions create=MQTTAsync_createOptions_initializer;
        create.maxBufferedMessages=1;create.sendWhileDisconnected=0;
        if(MQTTAsync_createWithOptions(&client.handle,endpoint_.c_str(),client_id_.c_str(),MQTTCLIENT_PERSISTENCE_NONE,nullptr,&create)!=MQTTASYNC_SUCCESS)
            throw std::runtime_error("MQTT create");
        if(MQTTAsync_setCallbacks(client.handle,this,lost,received,nullptr)!=MQTTASYNC_SUCCESS)
            throw std::runtime_error("MQTT callbacks");
        auto wait=[&](bool business){
            std::unique_lock lock(mutex_);
            while(!failed_&&!stopped_&&(!phase_done_||(business&&!business_acked_))&&std::chrono::steady_clock::now()<deadline)
                cv_.wait_for(lock,std::chrono::milliseconds(5));
            if(failed_||stopped_||!phase_done_||(business&&!business_acked_))throw std::runtime_error("MQTT incomplete");
            phase_done_=false;
        };
        MQTTAsync_connectOptions connect=MQTTAsync_connectOptions_initializer;
        connect.cleansession=1;connect.keepAliveInterval=5;connect.connectTimeout=1;
        connect.MQTTVersion=MQTTVERSION_3_1_1;connect.automaticReconnect=0;
        connect.onSuccess=success;connect.onFailure=failure;connect.context=this;
        if(MQTTAsync_connect(client.handle,&connect)!=MQTTASYNC_SUCCESS)throw std::runtime_error("MQTT connect");
        wait(false);
        MQTTAsync_responseOptions response=MQTTAsync_responseOptions_initializer;
        response.onSuccess=success;response.onFailure=failure;response.context=this;
        if(MQTTAsync_subscribe(client.handle,ack_.c_str(),1,&response)!=MQTTASYNC_SUCCESS)throw std::runtime_error("MQTT subscribe");
        wait(false);
        const auto body=request.dump();
        MQTTAsync_message message=MQTTAsync_message_initializer;
        message.payload=const_cast<char*>(body.data());message.payloadlen=static_cast<int>(body.size());message.qos=1;message.retained=0;
        if(MQTTAsync_sendMessage(client.handle,publish_.c_str(),&message,&response)!=MQTTASYNC_SUCCESS)
            throw std::runtime_error("MQTT publish");
        wait(true);
    }
    std::string endpoint_,publish_,ack_,client_id_;unsigned timeout_{};
    std::mutex mutex_;std::condition_variable cv_;bool phase_done_{},business_acked_{},failed_{};J identity_;
};
}
#define VISION_OUTPUT_ID "vision.mqtt-output"
#define VISION_OUTPUT_BUILD "mqtt-output-v1"
#include "../../output-common/exports.hpp"
