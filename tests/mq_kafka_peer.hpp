#pragma once
#include <librdkafka/rdkafka.h>
#include <librdkafka/rdkafka_mock.h>
#include <atomic>
#include <thread>
#include <memory>
#include <nlohmann/json.hpp>
// Upstream's protocol mock cluster, NOT an Apache Kafka production broker.
class KafkaPeer {
    rd_kafka_t* owner_{};rd_kafka_mock_cluster_t* cluster_{};std::jthread responder_;
    std::atomic<bool> ready_{};
public:
    unsigned port{};std::atomic<unsigned> requests{};std::atomic<bool> valid{true};
    explicit KafkaPeer(std::string scenario){
        std::string endpoint;
        if(const char* external=std::getenv("VISION_KAFKA_TEST_ENDPOINT"))endpoint=external;
        else {
        char error[512];auto* config=rd_kafka_conf_new();
        rd_kafka_conf_set(config,"log_level","0",error,sizeof(error));
        owner_=rd_kafka_new(RD_KAFKA_PRODUCER,config,error,sizeof(error));
        if(!owner_)throw std::runtime_error("Kafka mock owner");
        cluster_=rd_kafka_mock_cluster_new(owner_,1);
        if(!cluster_){rd_kafka_destroy(owner_);owner_=nullptr;throw std::runtime_error("Kafka mock cluster");}
        endpoint=rd_kafka_mock_cluster_bootstraps(cluster_);
        rd_kafka_mock_topic_create(cluster_,"results",1,1);rd_kafka_mock_topic_create(cluster_,"receipts",1,1);
        }
        port=static_cast<unsigned>(std::stoul(endpoint.substr(endpoint.rfind(':')+1)));
        responder_=std::jthread([this,scenario,endpoint](std::stop_token stop){
            try{
                auto make=[&](rd_kafka_type_t type){
                    auto* c=rd_kafka_conf_new();char err[512];
                    rd_kafka_conf_set(c,"bootstrap.servers",endpoint.c_str(),err,sizeof(err));
                    rd_kafka_conf_set(c,"log_level","0",err,sizeof(err));
                    rd_kafka_conf_set(c,"fetch.wait.max.ms","10",err,sizeof(err));
                    rd_kafka_conf_set(c,"linger.ms","0",err,sizeof(err));
                    return rd_kafka_new(type,c,err,sizeof(err));
                };
                std::unique_ptr<rd_kafka_t,decltype(&rd_kafka_destroy)> consumerOwner(make(RD_KAFKA_CONSUMER),rd_kafka_destroy);
                std::unique_ptr<rd_kafka_t,decltype(&rd_kafka_destroy)> producerOwner(make(RD_KAFKA_PRODUCER),rd_kafka_destroy);
                auto* consumer=consumerOwner.get();auto* producer=producerOwner.get();
                if(!consumer||!producer)throw std::runtime_error("Kafka test client");
                std::unique_ptr<rd_kafka_topic_t,decltype(&rd_kafka_topic_destroy)> topicOwner(
                    rd_kafka_topic_new(consumer,"results",nullptr),rd_kafka_topic_destroy);
                auto* topic=topicOwner.get();
                if(!topic)throw std::runtime_error("Kafka test topic");
                std::int64_t low{},high{};
                if(rd_kafka_query_watermark_offsets(consumer,"results",0,&low,&high,2000)||
                    rd_kafka_consume_start(topic,0,high))throw std::runtime_error("Kafka test offset");
                ready_=true;
                while(!stop.stop_requested()){
                    auto* msg=rd_kafka_consume(topic,0,10);if(!msg)continue;
                    if(!msg->err){
                        ++requests;
                        try{
                            auto request=nlohmann::json::parse(static_cast<const char*>(msg->payload),
                                static_cast<const char*>(msg->payload)+msg->len);
                            if(!msg->key||std::string_view(static_cast<const char*>(msg->key),msg->key_len)!="station")valid=false;
                            auto ack=nlohmann::json{{"event_id",request["event"]["event_id"]},
                                {"output_instance_id",request["output_instance_id"]},{"attempt",request["attempt"]},{"state","BusinessAcked"}};
                            if(scenario=="wrong-id")ack["event_id"]="wrong";
                            if(scenario=="wrong-attempt")ack["attempt"]="999";
                            auto text=ack.dump();
                            if(scenario=="malformed")text="{";
                            if(scenario=="oversized")text=std::string(8192,'x');
                            if(scenario=="duplicate")text="{\"event_id\":\"a\",\"event_id\":\"b\"}";
                            if(scenario!="broker-only"){
                                auto* replies=rd_kafka_topic_new(producer,"receipts",nullptr);
                                if(!replies||rd_kafka_produce(replies,0,RD_KAFKA_MSG_F_COPY,
                                    text.data(),text.size(),nullptr,0,nullptr))valid=false;
                                rd_kafka_flush(producer,500);
                                if(replies)rd_kafka_topic_destroy(replies);
                            }
                        }catch(...){valid=false;}
                    }
                    rd_kafka_message_destroy(msg);
                }
                rd_kafka_consume_stop(topic,0);
            }catch(...){valid=false;}
        });
        for(unsigned i=0;i<200&&!ready_&&valid;++i)std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if(!ready_){
            responder_.request_stop();if(responder_.joinable())responder_.join();
            if(cluster_)rd_kafka_mock_cluster_destroy(cluster_);
            if(owner_)rd_kafka_destroy(owner_);
            cluster_=nullptr;owner_=nullptr;throw std::runtime_error("Kafka test consumer start");
        }
    }
    ~KafkaPeer(){
        responder_.request_stop();if(responder_.joinable())responder_.join();
        if(cluster_)rd_kafka_mock_cluster_destroy(cluster_);if(owner_)rd_kafka_destroy(owner_);
    }
};
