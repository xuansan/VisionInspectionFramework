#include <vision/plugin_runtime/loader.hpp>
#include <vision/application/qt/dispatcher.hpp>
#include <vision/application/demo.hpp>
#include <vision/storage/store.hpp>
#include <QCoreApplication>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUuid>
#include <iostream>
#include <thread>
#include "network_durable.hpp"
#include "mq_rabbit_peer.hpp"
#ifdef VISION_TEST_RABBITMQ
#include "mq_rabbit_real.hpp"
#endif
#ifdef VISION_TEST_KAFKA
#include "mq_kafka_peer.hpp"
#endif
#ifdef VISION_TEST_NATS
#include "mq_nats_peer.hpp"
#endif
using namespace vision;
using J=nlohmann::json;
int main(int argc,char** argv){
    QCoreApplication app(argc,argv);
    try{
        const auto args=app.arguments();const auto protocol=args[2].toStdString(),scenario=args[3].toStdString();
        QTcpServer silent;
        if(!silent.listen(QHostAddress::LocalHost,0))throw std::runtime_error("Test bind");
        unsigned port=silent.serverPort();
        if(scenario=="offline")silent.close();
        std::unique_ptr<RabbitPeer> rabbit;
#ifdef VISION_TEST_RABBITMQ
        std::unique_ptr<RabbitRealPeer> real_rabbit;
        if(protocol=="rabbitmq"&&std::getenv("VISION_RABBITMQ_TEST_PORT")){
            real_rabbit=std::make_unique<RabbitRealPeer>(static_cast<unsigned>(std::stoul(std::getenv("VISION_RABBITMQ_TEST_PORT"))),scenario);
            port=real_rabbit->port;
        }else
#endif
        if(protocol=="rabbitmq"&&scenario!="offline"&&scenario!="silent"&&scenario!="invalid"&&scenario!="cancel"){
            rabbit=std::make_unique<RabbitPeer>(scenario);port=rabbit->port;
        }
#ifdef VISION_TEST_KAFKA
        std::unique_ptr<KafkaPeer> kafka;
        if(protocol=="kafka"&&scenario!="offline"&&scenario!="silent"&&scenario!="invalid"&&scenario!="cancel"){
            kafka=std::make_unique<KafkaPeer>(scenario);port=kafka->port;
        }
#endif
#ifdef VISION_TEST_NATS
        std::unique_ptr<NatsPeer> nats;
        if(protocol=="nats"&&scenario!="offline"&&scenario!="silent"&&scenario!="invalid"&&scenario!="cancel"){
            nats=std::make_unique<NatsPeer>(scenario);port=nats->port;
        }
#endif
        J config;
        if(protocol=="kafka")config={{"port",port},{"timeout_ms",800},{"topic","results"},{"ack_topic","receipts"},{"ack_partition",0},{"partition_key","station"}};
        else if(protocol=="rabbitmq"){
            _putenv_s("VISION_MQ_USER","guest");_putenv_s("VISION_MQ_PASSWORD","guest");
            config={{"port",port},{"timeout_ms",800},{"exchange",""},{"routing_key","results"},{"vhost","/"},
                {"username_env","VISION_MQ_USER"},{"password_env","VISION_MQ_PASSWORD"}};
        }else config={{"port",port},{"timeout_ms",800},{"subject","results"},{"ack_subject","receipts"},{"stream","VISION"}};
        // Explicit external local service configuration for integration drivers.
        if(const char* external=std::getenv("VISION_MQ_TEST_CONFIG"))config=J::parse(external);
        const auto event=*application::run_demo(application::DemoScenario::Ng).event;
        const bool success=scenario=="success"||scenario=="repeat";
        auto peer_valid=[&]{
            if(rabbit&&(!rabbit->valid||rabbit->requests!=(scenario=="repeat"?2U:1U)))return false;
#ifdef VISION_TEST_RABBITMQ
            if(real_rabbit&&(!real_rabbit->valid||real_rabbit->requests!=(scenario=="repeat"?2U:1U)))return false;
#endif
#ifdef VISION_TEST_KAFKA
            if(kafka&&(!kafka->valid||kafka->requests!=(scenario=="repeat"?2U:1U)))return false;
#endif
#ifdef VISION_TEST_NATS
            if(nats&&(!nats->valid||nats->requests!=(scenario=="repeat"?2U:1U)))return false;
#endif
            return true;
        };
        if(args.size()==7){
            QTemporaryDir root;
            {storage::Store store(std::filesystem::path(root.path().toStdWString()));store.commit(event,{"mq"});}
            const auto plan=J{{"root",root.path().toStdString()},{"host",args[4].toStdString()},
                {"outputs",J::array({{{"id","mq"},{"manifest",args[1].toStdString()},
                    {"parameters",config},{"replay_policy","NonPhysicalIdempotent"}}})}};
            if(!rejects_network_replay(args[6],plan))return 1;
            const auto result=run_network_durable(args[6],plan);
            return result==0&&peer_valid()?0:1;
        }
        if(args.size()==6){
            auto guard=std::make_shared<runtime::qt::StationGuard>("mq-"+QUuid::createUuid().toString(QUuid::WithoutBraces));
            application::qt::Dispatcher dispatcher(guard,args[4],{{"mq",config.dump(),args[1],1},{"peer","{}",args[5],1}});
            if(!dispatcher.start())throw std::runtime_error("Start");
            bool submitted=false,stopping=false,peer=false;QElapsedTimer elapsed;elapsed.start();QTimer timer;timer.setInterval(5);
            QObject::connect(&timer,&QTimer::timeout,&app,[&]{
                dispatcher.pulse();
                if(!submitted&&dispatcher.ready())submitted=dispatcher.submit(event,2000000000ULL);
                for(const auto& delivery:dispatcher.deliveries())if(delivery.output_id=="peer"&&delivery.state=="BusinessAcked")peer=true;
                if(submitted&&!stopping&&dispatcher.idle()){
                    bool found=false;
                    for(const auto& delivery:dispatcher.deliveries())if(delivery.output_id=="mq")
                        found=success?delivery.state=="BusinessAcked":(delivery.state=="Failed"||delivery.state=="Unknown");
                    if(!found||!peer){dispatcher.stop();app.exit(1);return;}
                    stopping=true;dispatcher.stop();
                }
                if(stopping){bool alive=false;for(const auto& w:dispatcher.workers())alive|=w.process_alive;if(!alive)app.exit(0);}
                if(elapsed.elapsed()>7000){dispatcher.stop();app.exit(1);}
            });
            timer.start();const auto result=app.exec();return result==0&&peer_valid()?0:1;
        }
        const auto manifest=std::filesystem::path(args[1].toStdWString());
        if(scenario=="invalid"){
            for(const auto& field:{"port","timeout_ms"}){
                auto bad=config;bad[field]=-1;
                bool rejected=false;
                try{auto p=plugin_runtime::Library::load(manifest,bad.dump());rejected=p->initialize()!=plugin_sdk::Status::Ok;}
                catch(const std::exception&){rejected=true;}
                if(!rejected)throw std::runtime_error("Invalid accepted");
            }
            auto bad=config;bad["extra"]="not permitted";
            try{auto p=plugin_runtime::Library::load(manifest,bad.dump());return p->initialize()==plugin_sdk::Status::Invalid?0:1;}
            catch(const std::exception&){return 0;}
        }
        auto plugin=plugin_runtime::Library::load(manifest,config.dump());
        if(plugin->initialize()!=plugin_sdk::Status::Ok)throw std::runtime_error("Initialize");
        const auto request=J{{"event",J::parse(serialization::encode_result(event))},{"output_instance_id","mq"},
            {"attempt","1"},{"budget_ns","1000000000"}}.dump();
        if(scenario=="cancel"){
            if(plugin->output_submit(request)!=plugin_sdk::Status::Ok)throw std::runtime_error("Cancel submit");
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            QElapsedTimer elapsed;elapsed.start();plugin->request_stop();
            if(plugin->close()!=plugin_sdk::Status::Ok||elapsed.elapsed()>2000)throw std::runtime_error("Cancel close");
            auto replacement=plugin_runtime::Library::load(manifest,config.dump());
            if(replacement->initialize()!=plugin_sdk::Status::Ok)throw std::runtime_error("Reload");
            replacement->request_stop();return replacement->close()==plugin_sdk::Status::Ok?0:1;
        }
        for(int n=0;n<(scenario=="repeat"?2:1);++n){
            if(plugin->output_submit(request)!=plugin_sdk::Status::Ok||plugin->output_submit(request)!=plugin_sdk::Status::Busy)
                throw std::runtime_error("Bounded queue");
            std::string report;auto status=plugin_sdk::Status::Busy;QElapsedTimer timer;timer.start();
            while(status==plugin_sdk::Status::Busy&&timer.elapsed()<5000){
                QCoreApplication::processEvents();status=plugin->output_poll(report);std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if(status!=plugin_sdk::Status::Ok)throw std::runtime_error("Report deadline");
            auto ack=J::parse(report);
            if(ack.at("state")!=(success?"BusinessAcked":"Failed")||ack.at("event_id")!=event.event_id.value()||
                ack.at("output_instance_id")!="mq"||ack.at("attempt")!="1"){
                std::cerr<<report<<'\n';throw std::runtime_error("Result state/identity");
            }
        }
        plugin->request_stop();if(plugin->close()!=plugin_sdk::Status::Ok)throw std::runtime_error("Close");
        if(!peer_valid())throw std::runtime_error("Message peer contract");
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
