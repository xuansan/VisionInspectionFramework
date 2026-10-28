#pragma once
#include <nats/nats.h>
#include <QProcess>
#include <QTemporaryDir>
#include <QTcpServer>
#include <atomic>
#include <thread>
class NatsPeer {
    QTemporaryDir directory_;QProcess process_;
    natsConnection* connection_{};natsSubscription* subscription_{};jsCtx* context_{};
    std::jthread responder_;
public:
    unsigned port{};std::atomic<unsigned> requests{};std::atomic<bool> valid{true};
    explicit NatsPeer(std::string scenario){
        QTcpServer reserve;reserve.listen(QHostAddress::LocalHost,0);port=reserve.serverPort();reserve.close();
        process_.start(QString::fromUtf8(VISION_TEST_NATS_SERVER),{"-js","-a","127.0.0.1","-p",QString::number(port),"-sd",directory_.path()});
        if(!process_.waitForStarted(2000))throw std::runtime_error("NATS server start");
        const auto endpoint="nats://127.0.0.1:"+std::to_string(port);
        for(unsigned n=0;n<100&&!connection_;++n){
            natsConnection_ConnectTo(&connection_,endpoint.c_str());if(!connection_)std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if(!connection_)throw std::runtime_error("NATS service unavailable");
        auto check=[](natsStatus s){if(s!=NATS_OK)throw std::runtime_error("NATS test setup");};
        check(natsConnection_JetStream(&context_,connection_,nullptr));
        jsStreamConfig config{};check(jsStreamConfig_Init(&config));
        config.Name="VISION";const char* subjects[]={"results"};config.Subjects=subjects;config.SubjectsLen=1;
        config.Storage=js_FileStorage;config.MaxMsgs=100;config.MaxBytes=1024*1024;config.MaxMsgSize=32768;
        jsStreamInfo* info=nullptr;jsErrCode code{};
        check(js_AddStream(&info,context_,&config,nullptr,&code));jsStreamInfo_Destroy(info);
        check(natsConnection_SubscribeSync(&subscription_,connection_,"results"));
        check(natsSubscription_SetPendingLimits(subscription_,4,131072));
        check(natsConnection_FlushTimeout(connection_,1000));
        responder_=std::jthread([this,scenario](std::stop_token stop){
            while(!stop.stop_requested()){
                natsMsg* msg=nullptr;const auto s=natsSubscription_NextMsg(&msg,subscription_,20);
                if(s==NATS_TIMEOUT)continue;if(s!=NATS_OK){valid=false;return;}
                ++requests;
                try{
                    auto request=nlohmann::json::parse(natsMsg_GetData(msg),natsMsg_GetData(msg)+natsMsg_GetDataLength(msg));
                    auto ack=nlohmann::json{{"event_id",request["event"]["event_id"]},
                        {"output_instance_id",request["output_instance_id"]},{"attempt",request["attempt"]},{"state","BusinessAcked"}};
                    if(scenario=="wrong-id")ack["event_id"]="wrong";
                    if(scenario=="wrong-attempt")ack["attempt"]="999";
                    auto text=ack.dump();
                    if(scenario=="malformed")text="{";
                    if(scenario=="oversized")text=std::string(8192,'x');
                    if(scenario=="duplicate")text="{\"event_id\":\"a\",\"event_id\":\"b\"}";
                    const char* reply=nullptr;
                    if(natsMsgHeader_Get(msg,"Vision-Reply-Subject",&reply)!=NATS_OK||!reply)throw std::runtime_error("Reply header");
                    if(scenario!="broker-only")natsConnection_Publish(connection_,reply,text.data(),static_cast<int>(text.size()));
                }catch(...){valid=false;}
                natsMsg_Destroy(msg);
            }
        });
    }
    ~NatsPeer(){
        responder_.request_stop();if(responder_.joinable())responder_.join();
        if(subscription_)natsSubscription_Destroy(subscription_);if(context_)jsCtx_Destroy(context_);
        if(connection_){natsConnection_Close(connection_);natsConnection_Destroy(connection_);}
        process_.kill();process_.waitForFinished(2000);
    }
};
