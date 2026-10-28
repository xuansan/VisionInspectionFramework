#pragma once
#include <winsock2.h>
#include <rabbitmq-c/amqp.h>
#include <rabbitmq-c/tcp_socket.h>
#include <atomic>
#include <thread>
#include <nlohmann/json.hpp>
// Business consumer simulator against an independently started real RabbitMQ server.
class RabbitRealPeer {
    amqp_connection_state_t connection_{};
    std::jthread responder_;
public:
    unsigned port{};
    std::atomic<unsigned> requests{};
    std::atomic<bool> valid{true};
    RabbitRealPeer(unsigned endpoint,std::string scenario):port(endpoint){
        connection_=amqp_new_connection();
        auto check=[](amqp_rpc_reply_t r){if(r.reply_type!=AMQP_RESPONSE_NORMAL)throw std::runtime_error("RabbitMQ real setup");};
        try{
            timeval timeout{2,0};amqp_set_rpc_timeout(connection_,&timeout);
            amqp_set_handshake_timeout(connection_,&timeout);
            auto* socket=amqp_tcp_socket_new(connection_);
            if(!socket||amqp_socket_open_noblock(socket,"127.0.0.1",static_cast<int>(port),&timeout)!=AMQP_STATUS_OK)
                throw std::runtime_error("RabbitMQ real connect");
            check(amqp_login(connection_,"/",2,4096,1,AMQP_SASL_METHOD_PLAIN,"guest","guest"));
            amqp_channel_open(connection_,1);check(amqp_get_rpc_reply(connection_));
            amqp_queue_declare(connection_,1,amqp_cstring_bytes("results"),0,0,0,0,amqp_empty_table);
            check(amqp_get_rpc_reply(connection_));
            amqp_queue_purge(connection_,1,amqp_cstring_bytes("results"));check(amqp_get_rpc_reply(connection_));
        }catch(...){amqp_destroy_connection(connection_);connection_=nullptr;throw;}
        responder_=std::jthread([this,scenario](std::stop_token stop){
            while(!stop.stop_requested()){
                timeval timeout{0,200000};amqp_set_rpc_timeout(connection_,&timeout);
                auto response=amqp_basic_get(connection_,1,amqp_cstring_bytes("results"),1);
                if(response.reply_type!=AMQP_RESPONSE_NORMAL){valid=false;return;}
                if(response.reply.id==AMQP_BASIC_GET_EMPTY_METHOD){std::this_thread::sleep_for(std::chrono::milliseconds(5));continue;}
                amqp_message_t message{};response=amqp_read_message(connection_,1,&message,0);
                if(response.reply_type!=AMQP_RESPONSE_NORMAL){valid=false;return;}
                ++requests;
                try{
                    const auto request=nlohmann::json::parse(static_cast<const char*>(message.body.bytes),
                        static_cast<const char*>(message.body.bytes)+message.body.len);
                    auto reply=message.properties.reply_to;
                    if(!(message.properties._flags&AMQP_BASIC_REPLY_TO_FLAG)||!reply.len)
                        throw std::runtime_error("RabbitMQ real reply_to");
                    auto ack=nlohmann::json{{"event_id",request["event"]["event_id"]},
                        {"output_instance_id",request["output_instance_id"]},{"attempt",request["attempt"]},{"state","BusinessAcked"}};
                    if(scenario=="wrong-id")ack["event_id"]="wrong";
                    if(scenario=="wrong-attempt")ack["attempt"]="999";
                    auto text=ack.dump();
                    if(scenario=="malformed")text="{";
                    if(scenario=="oversized")text=std::string(8192,'x');
                    if(scenario=="duplicate")text="{\"event_id\":\"a\",\"event_id\":\"b\"}";
                    if(scenario!="broker-only"&&amqp_basic_publish(connection_,1,amqp_empty_bytes,reply,0,0,nullptr,
                        amqp_bytes_t{text.size(),text.data()})!=AMQP_STATUS_OK)valid=false;
                }catch(...){valid=false;}
                amqp_destroy_message(&message);amqp_maybe_release_buffers(connection_);
            }
        });
    }
    ~RabbitRealPeer(){
        responder_.request_stop();if(responder_.joinable())responder_.join();
        if(connection_)amqp_destroy_connection(connection_);
    }
};
