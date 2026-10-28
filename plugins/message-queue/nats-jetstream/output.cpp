#include "../common.hpp"
#include <nats/nats.h>
namespace {
using namespace vision::mq;
void check(natsStatus status){if(status!=NATS_OK)throw std::runtime_error("NATS operation failed");}
class Output final:public vision::outputs::AsyncOutput {
public:
    ~Output(){finish();}
private:
    void configure(const J& j) override {
        if(j.size()!=5)throw std::invalid_argument("NATS parameters");
        port_=port(j);timeout_=timeout(j);subject_=name(j,"subject");ack_=name(j,"ack_subject");stream_=name(j,"stream");
        if(subject_==ack_)throw std::invalid_argument("NATS distinct subjects");
    }
    void write(const J& request) override {
        Deadline deadline(timeout_,stopped_);
        struct Resources {
            natsOptions* options{};natsConnection* connection{};natsSubscription* subscription{};
            jsCtx* context{};natsMsg* message{};jsPubAck* publication{};
            ~Resources(){
                if(subscription)natsSubscription_Destroy(subscription);
                if(message)natsMsg_Destroy(message);if(publication)jsPubAck_Destroy(publication);
                if(context)jsCtx_Destroy(context);
                if(connection){natsConnection_Close(connection);natsConnection_Destroy(connection);}
                if(options)natsOptions_Destroy(options);
            }
        } r;
        check(natsOptions_Create(&r.options));
        auto url="nats://127.0.0.1:"+std::to_string(port_);
        check(natsOptions_SetURL(r.options,url.c_str()));check(natsOptions_SetTimeout(r.options,deadline.remaining()));
        check(natsOptions_SetAllowReconnect(r.options,false));
        check(natsOptions_SetMaxPendingMsgs(r.options,4));check(natsOptions_SetMaxPendingBytes(r.options,16384));
        check(natsConnection_Connect(&r.connection,r.options));
        check(natsConnection_SubscribeSync(&r.subscription,r.connection,ack_.c_str()));
        check(natsSubscription_SetPendingLimits(r.subscription,4,16384));
        check(natsConnection_FlushTimeout(r.connection,deadline.remaining()));
        jsOptions options{};check(jsOptions_Init(&options));options.Wait=deadline.remaining();
        check(natsConnection_JetStream(&r.context,r.connection,&options));
        auto body=request.dump();
        check(natsMsg_Create(&r.message,subject_.c_str(),nullptr,body.data(),static_cast<int>(body.size())));
        check(natsMsgHeader_Set(r.message,"Vision-Reply-Subject",ack_.c_str()));
        jsPubOptions publish{};check(jsPubOptions_Init(&publish));publish.MaxWait=deadline.remaining();
        publish.ExpectStream=stream_.c_str();
        jsErrCode error{};
        check(js_PublishMsg(&r.publication,r.context,r.message,&publish,&error));
        if(!r.publication||!r.publication->Stream||r.publication->Stream!=stream_||!r.publication->Sequence)
            throw std::runtime_error("JetStream publication identity");
        // No cross-attempt broker dedup: a retried event must reach the idempotent business consumer.
        for(;;){
            natsMsg* message=nullptr;
            auto status=natsSubscription_NextMsg(&message,r.subscription,std::min(10,deadline.remaining()));
            if(status==NATS_TIMEOUT)continue;check(status);
            std::unique_ptr<natsMsg,decltype(&natsMsg_Destroy)> owner(message,natsMsg_Destroy);
            const auto length=natsMsg_GetDataLength(message);
            if(length<=0||length>4096||!ack(std::string_view(natsMsg_GetData(message),static_cast<std::size_t>(length)),request))
                throw std::runtime_error("NATS business identity");
            return;
        }
    }
    unsigned port_{},timeout_{};std::string subject_,ack_,stream_;
};
}
#define VISION_OUTPUT_ID "vision.nats-output"
#define VISION_OUTPUT_BUILD "nats-output-v1"
#include "../../output-common/exports.hpp"
