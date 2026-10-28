#include "../common.hpp"
#include <winsock2.h>
#include <rabbitmq-c/amqp.h>
#include <rabbitmq-c/tcp_socket.h>
#include <cstdlib>
namespace {
using namespace vision::mq;
class Output final:public vision::outputs::AsyncOutput {
public:
    ~Output(){finish();}
private:
    void configure(const J& j) override {
        if(j.size()!=7)throw std::invalid_argument("RabbitMQ parameters");
        port_=port(j);timeout_=timeout(j);exchange_=name(j,"exchange",true);routing_=name(j,"routing_key");
        vhost_=j.at("vhost").get<std::string>();
        if(vhost_.empty()||vhost_.size()>128||vhost_.find('\0')!=std::string::npos)throw std::invalid_argument("RabbitMQ vhost");
        auto secret=[&](const char* key){
            auto env=name(j,key);const char* s=std::getenv(env.c_str());
            if(!s||!*s||std::strlen(s)>128)throw std::invalid_argument("RabbitMQ credentials");
            return std::string(s);
        };
        user_=secret("username_env");password_=secret("password_env");
    }
    void write(const J& request) override {
        Deadline deadline(timeout_,stopped_);
        struct Connection {
            amqp_connection_state_t value=amqp_new_connection();
            ~Connection(){if(value)amqp_destroy_connection(value);}
        } conn;
        if(!conn.value)throw std::runtime_error("RabbitMQ allocation");
        auto duration=[&]{const auto ms=deadline.remaining();return timeval{ms/1000,(ms%1000)*1000};};
        auto bounds=[&]{auto t=duration();if(amqp_set_rpc_timeout(conn.value,&t)!=AMQP_STATUS_OK)
            throw std::runtime_error("RabbitMQ deadline");};
        auto check=[](amqp_rpc_reply_t r){if(r.reply_type!=AMQP_RESPONSE_NORMAL)throw std::runtime_error("RabbitMQ RPC");};
        auto* socket=amqp_tcp_socket_new(conn.value);auto t=duration();
        if(!socket||amqp_socket_open_noblock(socket,"127.0.0.1",static_cast<int>(port_),&t)!=AMQP_STATUS_OK)
            throw std::runtime_error("RabbitMQ connect");
        t=duration();amqp_set_handshake_timeout(conn.value,&t);
        check(amqp_login(conn.value,vhost_.c_str(),2,4096,1,AMQP_SASL_METHOD_PLAIN,user_.c_str(),password_.c_str()));
        bounds();amqp_channel_open(conn.value,1);check(amqp_get_rpc_reply(conn.value));
        bounds();amqp_confirm_select(conn.value,1);check(amqp_get_rpc_reply(conn.value));
        bounds();auto* queue=amqp_queue_declare(conn.value,1,amqp_empty_bytes,0,0,1,1,amqp_empty_table);
        check(amqp_get_rpc_reply(conn.value));if(!queue)throw std::runtime_error("RabbitMQ reply queue");
        std::string reply(static_cast<char*>(queue->queue.bytes),queue->queue.len);
        const auto body=request.dump();const auto id=request.at("event").at("event_id").get<std::string>();
        amqp_basic_properties_t properties{};
        properties._flags=AMQP_BASIC_CONTENT_TYPE_FLAG|AMQP_BASIC_DELIVERY_MODE_FLAG|AMQP_BASIC_MESSAGE_ID_FLAG|AMQP_BASIC_REPLY_TO_FLAG;
        properties.content_type=amqp_cstring_bytes("application/json");properties.delivery_mode=2;
        properties.message_id=amqp_cstring_bytes(id.c_str());properties.reply_to=amqp_cstring_bytes(reply.c_str());
        if(amqp_basic_publish(conn.value,1,amqp_cstring_bytes(exchange_.c_str()),amqp_cstring_bytes(routing_.c_str()),
            1,0,&properties,amqp_bytes_t{body.size(),const_cast<char*>(body.data())})!=AMQP_STATUS_OK)
            throw std::runtime_error("RabbitMQ publish");
        bool confirmed=false;
        while(!confirmed){
            amqp_frame_t frame{};t=duration();t.tv_sec=0;t.tv_usec=std::min<long>(t.tv_usec,10000);
            auto code=amqp_simple_wait_frame_noblock(conn.value,&frame,&t);
            if(code==AMQP_STATUS_TIMEOUT)continue;
            if(code!=AMQP_STATUS_OK)throw std::runtime_error("RabbitMQ frame");
            if(frame.frame_type==AMQP_FRAME_HEARTBEAT)continue;
            if(frame.frame_type!=AMQP_FRAME_METHOD||frame.channel!=1||frame.payload.method.id!=AMQP_BASIC_ACK_METHOD)
                throw std::runtime_error("RabbitMQ rejected or unroutable");
            auto* ack=static_cast<amqp_basic_ack_t*>(frame.payload.method.decoded);
            if(ack->delivery_tag!=1)throw std::runtime_error("RabbitMQ publish identity");
            confirmed=true;
        }
        // Poll a private reply queue; there is no consumer-ack/publisher-confirm ambiguity.
        for(;;){
            bounds();auto response=amqp_basic_get(conn.value,1,amqp_cstring_bytes(reply.c_str()),1);check(response);
            if(response.reply.id==AMQP_BASIC_GET_EMPTY_METHOD){std::this_thread::sleep_for(std::chrono::milliseconds(5));continue;}
            if(response.reply.id!=AMQP_BASIC_GET_OK_METHOD)throw std::runtime_error("RabbitMQ get");
            // Read header/body frames ourselves to reject oversized content BEFORE library allocation.
            amqp_frame_t frame{};t=duration();
            if(amqp_simple_wait_frame_noblock(conn.value,&frame,&t)!=AMQP_STATUS_OK||
                frame.frame_type!=AMQP_FRAME_HEADER||frame.channel!=1||frame.payload.properties.body_size>4096)
                throw std::runtime_error("RabbitMQ reply header");
            auto size=static_cast<std::size_t>(frame.payload.properties.body_size);std::string text;
            while(text.size()<size){
                t=duration();
                if(amqp_simple_wait_frame_noblock(conn.value,&frame,&t)!=AMQP_STATUS_OK||
                    frame.frame_type!=AMQP_FRAME_BODY||frame.channel!=1||frame.payload.body_fragment.len>size-text.size())
                    throw std::runtime_error("RabbitMQ reply body");
                text.append(static_cast<char*>(frame.payload.body_fragment.bytes),frame.payload.body_fragment.len);
            }
            if(!ack(text,request))throw std::runtime_error("RabbitMQ business identity");
            return;
        }
    }
    unsigned port_{},timeout_{};std::string exchange_,routing_,vhost_,user_,password_;
};
}
#define VISION_OUTPUT_ID "vision.rabbitmq-output"
#define VISION_OUTPUT_BUILD "rabbitmq-output-v1"
#include "../../output-common/exports.hpp"
