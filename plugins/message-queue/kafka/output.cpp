#include "../common.hpp"
#include <librdkafka/rdkafka.h>
namespace {
using namespace vision::mq;
struct Client {rd_kafka_t* value{};~Client(){if(value)rd_kafka_destroy(value);}};
struct Topic {rd_kafka_topic_t* value{};~Topic(){if(value)rd_kafka_topic_destroy(value);}};
void setting(rd_kafka_conf_t* c,const char* key,const std::string& value) {
    char error[512];
    if(rd_kafka_conf_set(c,key,value.c_str(),error,sizeof(error))!=RD_KAFKA_CONF_OK)
        throw std::invalid_argument(std::string("Kafka setting: ")+key+": "+error);
}
class Output final:public vision::outputs::AsyncOutput {
public:
    ~Output(){finish();}
private:
    void configure(const J& j) override {
        if(j.size()!=6)throw std::invalid_argument("Kafka parameters");
        port_=port(j);timeout_=timeout(j);topic_=name(j,"topic");ack_topic_=name(j,"ack_topic");key_=name(j,"partition_key");
        if(topic_==ack_topic_)throw std::invalid_argument("Kafka distinct topics");
        const auto& p=j.at("ack_partition");
        if(!p.is_number_integer()||p.get<std::int64_t>()<0||p.get<std::int64_t>()>1023)
            throw std::invalid_argument("Kafka ack partition");
        partition_=p.get<int>();
    }
    static void delivered(rd_kafka_t*,const rd_kafka_message_t* message,void* opaque) {
        auto& self=*static_cast<Output*>(opaque);
        self.delivered_=true;self.delivery_ok_=message->err==RD_KAFKA_RESP_ERR_NO_ERROR;
    }
    void write(const J& request) override {
        Deadline deadline(timeout_,stopped_);delivered_=false;delivery_ok_=false;
        auto client=[&](rd_kafka_type_t type){
            std::unique_ptr<rd_kafka_conf_t,decltype(&rd_kafka_conf_destroy)> config(rd_kafka_conf_new(),rd_kafka_conf_destroy);
            setting(config.get(),"bootstrap.servers","127.0.0.1:"+std::to_string(port_));
            setting(config.get(),"socket.timeout.ms","500");
            setting(config.get(),"socket.connection.setup.timeout.ms","1000");
            setting(config.get(),"reconnect.backoff.max.ms","100");
            setting(config.get(),"reconnect.backoff.ms","100");
            setting(config.get(),"allow.auto.create.topics","false");
            setting(config.get(),"message.max.bytes","32768");
            setting(config.get(),"receive.message.max.bytes","1000000");
            setting(config.get(),"log_level","0");
            setting(config.get(),"topic.metadata.refresh.fast.interval.ms","10");
            if(type==RD_KAFKA_PRODUCER){
                setting(config.get(),"acks","all");
                setting(config.get(),"enable.idempotence","true");
                setting(config.get(),"message.send.max.retries","1");
                setting(config.get(),"message.timeout.ms",std::to_string(timeout_));
                setting(config.get(),"queue.buffering.max.messages","1");
                setting(config.get(),"queue.buffering.max.kbytes","64");
                setting(config.get(),"linger.ms","0");
                rd_kafka_conf_set_dr_msg_cb(config.get(),delivered);
                rd_kafka_conf_set_opaque(config.get(),this);
            }else{
                setting(config.get(),"enable.auto.commit","false");setting(config.get(),"enable.auto.offset.store","false");
                setting(config.get(),"queued.min.messages","1");setting(config.get(),"queued.max.messages.kbytes","16");
                setting(config.get(),"fetch.max.bytes","65536");
                setting(config.get(),"fetch.message.max.bytes","65536");setting(config.get(),"fetch.wait.max.ms","10");
                setting(config.get(),"fetch.queue.backoff.ms","10");
            }
            char error[512];auto* value=rd_kafka_new(type,config.get(),error,sizeof(error));
            if(!value)throw std::runtime_error(std::string("Kafka client: ")+error);
            config.release();return value;
        };
        Client producer{client(RD_KAFKA_PRODUCER)},consumer{client(RD_KAFKA_CONSUMER)};
        Topic replies{rd_kafka_topic_new(consumer.value,ack_topic_.c_str(),nullptr)};
        if(!replies.value)throw std::runtime_error("Kafka reply topic");
        std::int64_t low{},high{};
        if(rd_kafka_query_watermark_offsets(consumer.value,ack_topic_.c_str(),partition_,&low,&high,deadline.remaining()))
            throw std::runtime_error("Kafka reply watermark");
        if(rd_kafka_consume_start(replies.value,partition_,high))throw std::runtime_error("Kafka consume");
        struct Stop {rd_kafka_topic_t* topic;int partition;~Stop(){rd_kafka_consume_stop(topic,partition);}} stop{replies.value,partition_};
        const auto body=request.dump();
        Topic results{rd_kafka_topic_new(producer.value,topic_.c_str(),nullptr)};
        if(!results.value)throw std::runtime_error("Kafka result topic");
        if(rd_kafka_produce(results.value,RD_KAFKA_PARTITION_UA,RD_KAFKA_MSG_F_COPY,
            const_cast<char*>(body.data()),body.size(),key_.data(),key_.size(),nullptr))
            throw std::runtime_error("Kafka enqueue");
        bool business=false;
        while(!delivered_||!business){
            (void)deadline.remaining();rd_kafka_poll(producer.value,0);
            if(delivered_&&!delivery_ok_)throw std::runtime_error("Kafka broker rejected");
            std::unique_ptr<rd_kafka_message_t,decltype(&rd_kafka_message_destroy)> message(
                rd_kafka_consume(replies.value,partition_,std::min(10,deadline.remaining())),rd_kafka_message_destroy);
            if(!message)continue;
            if(message->err==RD_KAFKA_RESP_ERR__PARTITION_EOF)continue;
            if(message->err)throw std::runtime_error("Kafka consume failure");
            if(!ack(std::string_view(static_cast<char*>(message->payload),message->len),request))
                throw std::runtime_error("Kafka business identity");
            business=true;
        }
    }
    unsigned port_{},timeout_{};int partition_{};std::string topic_,ack_topic_,key_;
    bool delivered_{},delivery_ok_{};
};
}
#define VISION_OUTPUT_ID "vision.kafka-output"
#define VISION_OUTPUT_BUILD "kafka-output-v1"
#include "../../output-common/exports.hpp"
