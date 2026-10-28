#include <vision/plugin_runtime/loader.hpp>
#include <vision/application/qt/dispatcher.hpp>
#include <vision/application/demo.hpp>
#include <nlohmann/json.hpp>
#include <QCoreApplication>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUuid>
#include <iostream>
#include <thread>
#include <vision/storage/store.hpp>
#include <QTemporaryDir>
#include "network_durable.hpp"
using namespace vision;
using J=nlohmann::json;
namespace {
// A bounded MQTT test peer, not a production broker or client implementation.
QByteArray packet(unsigned char type,const QByteArray& body) {
    QByteArray value;value.append(static_cast<char>(type));auto size=static_cast<unsigned>(body.size());
    do {auto byte=size%128;size/=128;if(size)byte|=128;value.append(static_cast<char>(byte));}while(size);
    value+=body;return value;
}
QByteArray text(const QByteArray& value) {
    QByteArray out;out.append(static_cast<char>(value.size()>>8));out.append(static_cast<char>(value.size()&255));return out+value;
}
class Broker:public QTcpServer {
public:
    Broker(std::string scenario):scenario_(std::move(scenario)) {
        if(!listen(QHostAddress::LocalHost,0))throw std::runtime_error("Broker bind");
        QObject::connect(this,&QTcpServer::newConnection,this,[this]{
            while(hasPendingConnections()){
                auto* socket=nextPendingConnection();auto buffer=std::make_shared<QByteArray>();
                QObject::connect(socket,&QTcpSocket::readyRead,this,[this,socket,buffer]{
                    *buffer+=socket->readAll();
                    if(buffer->size()>65536){valid=false;socket->abort();return;}
                    while(buffer->size()>=2){
                        unsigned size=0,multiplier=1;int offset=1;bool complete=false;
                        for(;offset<buffer->size()&&offset<=4;++offset){
                            const auto byte=static_cast<unsigned char>((*buffer)[offset]);size+=(byte&127)*multiplier;multiplier*=128;
                            if(!(byte&128)){++offset;complete=true;break;}
                        }
                        if(!complete)return;
                        if(size>32768){valid=false;socket->abort();return;}
                        if(buffer->size()<offset+static_cast<int>(size))return;
                        const auto type=static_cast<unsigned char>((*buffer)[0]);
                        const auto body=buffer->mid(offset,static_cast<int>(size));
                        buffer->remove(0,offset+static_cast<int>(size));
                        handle(socket,type,body);
                    }
                });
            }
        });
    }
    ~Broker(){for(auto* socket:findChildren<QTcpSocket*>())socket->abort();}
    bool valid=true;unsigned published{};
private:
    void handle(QTcpSocket* socket,unsigned char type,const QByteArray& body) {
        if(scenario_=="silent")return;
        if((type>>4)==1){
            socket->write(packet(0x20,QByteArray::fromHex(scenario_=="connect-reject"?"0005":"0000")));return;
        }
        if((type>>4)==8){
            if(body.size()<5){valid=false;socket->abort();return;}
            socket->write(packet(0x90,body.left(2)+QByteArray(1,scenario_=="subscribe-reject"?char(0x80):char(1))));return;
        }
        if((type>>4)==3){
            ++published;
            try {
                if(body.size()<4||type!=0x32)throw std::runtime_error("Publish QoS");
                const auto topic_length=(static_cast<unsigned char>(body[0])<<8)|static_cast<unsigned char>(body[1]);
                if(topic_length+4>body.size()||body.mid(2,topic_length)!="inspection/results")throw std::runtime_error("Publish topic");
                const auto request=J::parse(body.mid(topic_length+4).toStdString());
                if(request.at("output_instance_id")!="mqtt"||request.at("attempt")!="1")throw std::runtime_error("Publish identity");
                if(scenario_=="disconnect"){socket->abort();return;}
                if(scenario_!="no-puback")socket->write(packet(0x40,body.mid(topic_length+2,2)));
                if(scenario_=="broker-only")return;
                auto ack=J{{"event_id",request.at("event").at("event_id")},
                    {"output_instance_id",request.at("output_instance_id")},{"attempt",request.at("attempt")},{"state","BusinessAcked"}};
                if(scenario_=="wrong-id")ack["event_id"]="wrong";
                if(scenario_=="wrong-attempt")ack["attempt"]="2";
                std::string payload=ack.dump();
                if(scenario_=="malformed")payload="{";
                if(scenario_=="oversized")payload=std::string(8192,'x');
                const auto flags=scenario_=="retained"?0x31:0x30;
                socket->write(packet(static_cast<unsigned char>(flags),text("inspection/acks")+QByteArray::fromStdString(payload)));
            }catch(...){valid=false;socket->abort();}
            return;
        }
        if((type>>4)==12){socket->write(packet(0xd0,{}));return;}
        if((type>>4)==14){socket->disconnectFromHost();return;}
    }
    std::string scenario_;
};
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        const auto args=app.arguments();const auto scenario=args[2].toStdString();
        Broker broker(scenario);const auto port=broker.serverPort();if(scenario=="offline")broker.close();
        auto config=J{{"endpoint","tcp://127.0.0.1:"+std::to_string(port)},
            {"publish_topic","inspection/results"},{"ack_topic","inspection/acks"},{"client_id","vision-test"},{"timeout_ms",400}};
        auto event=*application::run_demo(application::DemoScenario::Ng).event;
        if(args.size()==6){
            QTemporaryDir root;
            {storage::Store store(std::filesystem::path(root.path().toStdWString()));store.commit(event,{"mqtt"});}
            const auto plan=J{{"root",root.path().toStdString()},{"host",args[3].toStdString()},
                {"outputs",J::array({{{"id","mqtt"},{"manifest",args[1].toStdString()},
                    {"parameters",config},{"replay_policy","NonPhysicalIdempotent"}}})}};
            if(!rejects_network_replay(args[5],plan))return 1;
            return run_network_durable(args[5],plan);
        }
        if(args.size()>3){
            auto guard=std::make_shared<runtime::qt::StationGuard>("mqtt-"+QUuid::createUuid().toString(QUuid::WithoutBraces));
            application::qt::Dispatcher dispatcher(guard,args[3],{{"mqtt",config.dump(),args[1],1},{"peer","{}",args[4],1}});
            if(!dispatcher.start())throw std::runtime_error("Dispatcher");
            bool submitted=false,stopping=false;QElapsedTimer elapsed;elapsed.start();QTimer timer;timer.setInterval(5);
            QObject::connect(&timer,&QTimer::timeout,&app,[&]{
                dispatcher.pulse();
                if(!submitted&&dispatcher.ready())submitted=dispatcher.submit(event,2000000000ULL);
                if(submitted&&!stopping&&dispatcher.idle()){
                    bool mqtt=false,peer=false;
                    for(const auto& d:dispatcher.deliveries()){
                        if(d.output_id=="mqtt")mqtt=d.state==(scenario=="success"?"BusinessAcked":"Failed");
                        if(d.output_id=="peer")peer=d.state=="BusinessAcked";
                    }
                    if(!mqtt||!peer||!broker.valid){dispatcher.stop();app.exit(1);return;}
                    dispatcher.stop();stopping=true;
                }
                if(stopping){bool alive=false;for(const auto& w:dispatcher.workers())alive|=w.process_alive;if(!alive)app.exit(0);}
                if(elapsed.elapsed()>6000){dispatcher.stop();app.exit(1);}
            });
            timer.start();return app.exec();
        }
        const auto manifest=std::filesystem::path(args[1].toStdWString());
        if(scenario=="invalid"){
            for(const auto& endpoint:{"tcp://example.com:1883","tcp://127.0.0.1:0","tcp://127.0.0.1:1883/bad"}){
                bool rejected=false;
                try{config["endpoint"]=endpoint;auto p=plugin_runtime::Library::load(manifest,config.dump());rejected=p->initialize()!=plugin_sdk::Status::Ok;p->close();}
                catch(...){rejected=true;}
                if(!rejected)throw std::runtime_error("Invalid endpoint");
            }
            return 0;
        }
        auto plugin=plugin_runtime::Library::load(manifest,config.dump());
        if(plugin->initialize()!=plugin_sdk::Status::Ok)throw std::runtime_error("Initialize");
        const auto request=J{{"event",J::parse(serialization::encode_result(event))},
            {"output_instance_id","mqtt"},{"attempt","1"},{"budget_ns","2000000000"}}.dump();
        const auto rounds=scenario=="repeat"?2:1;
        for(int round=0;round<rounds;++round){
            if(plugin->output_submit(request)!=plugin_sdk::Status::Ok)throw std::runtime_error("Submit");
            if(plugin->output_submit(request)!=plugin_sdk::Status::Busy)throw std::runtime_error("Queue bound");
            std::string report;auto status=plugin_sdk::Status::Busy;
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(4);
            while(status==plugin_sdk::Status::Busy&&std::chrono::steady_clock::now()<deadline){
                app.processEvents();status=plugin->output_poll(report);std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if(status!=plugin_sdk::Status::Ok)throw std::runtime_error("Report timeout");
            const auto ack=J::parse(report);
            if(ack.at("state")!=(scenario=="success"||scenario=="repeat"?"BusinessAcked":"Failed")||
                ack.at("event_id")!=event.event_id.value()||ack.at("output_instance_id")!="mqtt")
                throw std::runtime_error("Report identity/state");
        }
        plugin->request_stop();if(plugin->close()!=plugin_sdk::Status::Ok||!broker.valid)throw std::runtime_error("Close/broker");
        if(scenario=="repeat"&&broker.published!=2)throw std::runtime_error("Repeat");
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
