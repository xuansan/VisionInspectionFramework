#include <vision/plugin_sdk/api.hpp>
#include <nlohmann/json.hpp>
#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <thread>
#include <future>
#include <QTcpServer>
#include <QTcpSocket>
#include <QCoreApplication>
#include <QTimer>
#ifdef TEST_S7
#include <snap7.h>
#endif
using namespace vision::plugin_sdk;
using J=nlohmann::json;
extern "C" Status VISION_PLUGIN_CALL vision_plugin_create(Plugin**) noexcept;
extern "C" Status VISION_PLUGIN_CALL vision_plugin_destroy(Plugin*) noexcept;
void check(bool v,const char* reason){if(!v)throw std::runtime_error(reason);}
Bytes bytes(const std::string& s){return {reinterpret_cast<const std::uint8_t*>(s.data()),static_cast<std::uint32_t>(s.size())};}
struct Instance{
    Plugin* p{};Instance(){check(vision_plugin_create(&p)==Status::Ok,"create");}
    ~Instance(){vision_plugin_destroy(p);}
    Communication& device(){return *dynamic_cast<Communication*>(p);}
};
#include "plc_test_server.hpp"
int main(int argc,char** argv){
    QCoreApplication app(argc,argv);
    if(argc!=2)return 1;
    Server server;server.fault=argv[1];
    QTcpServer tcp;check(tcp.listen(QHostAddress::LocalHost,0),"listen");
    const auto port=tcp.serverPort();
#ifdef TEST_S7
    tcp.close();
    auto snap=Srv_Create();check(snap!=0,"server create");
    auto local=port;
    check(Srv_SetParam(snap,p_u16_LocalPort,&local)==0,"server port");
    check(Srv_SetRWAreaCallback(snap,exchange,&server)==0,"callback");
    check(Srv_StartTo(snap,"127.0.0.1")==0,"server start");
#else
    QObject::connect(&tcp,&QTcpServer::newConnection,&app,[&]{
        auto* socket=tcp.nextPendingConnection();
        auto pending=std::make_shared<QByteArray>();
        QObject::connect(socket,&QTcpSocket::readyRead,socket,[&,socket,pending]{
            *pending+=socket->readAll();
            auto get=[](const QByteArray& b,int i){return (static_cast<unsigned char>(b[i])<<8)|static_cast<unsigned char>(b[i+1]);};
            while(pending->size()>=7){
                int size=6+get(*pending,4);if(pending->size()<size)return;
                auto request=pending->first(size);pending->remove(0,size);
                if(server.fault=="timeout")continue;
                if(server.fault=="disconnect"){socket->abort();return;}
                QByteArray pdu;
                if(request[7]==16){
                    if(size!=61){socket->abort();return;}
                    std::memcpy(server.command.data(),request.constData()+13,48);++server.writes;pdu=request.mid(7,5);
                }else{
                    pdu.resize(52);pdu[0]=3;pdu[1]=50;server.reply(pdu.data()+2);
                }
                auto response=request.first(4);response+=char(0);response+=char(pdu.size()+1);response+=request[6];response+=pdu;
                socket->write(response);
            }
        });
    });
#endif
    std::atomic<int> result{-1};
    auto job=std::async(std::launch::async,[&]{
        try {
            J config={{"address","127.0.0.1"},{"port",port},{"write_base",100},{"read_base",200}};
#ifdef TEST_S7
            config.update(J{{"rack",0},{"slot",1},{"db",1}});
            constexpr unsigned max_write=65488,max_read=65486;
#else
            config["unit"]=1;
            constexpr unsigned max_write=65512,max_read=65511;
#endif
            for(const char* key:{"write_base","read_base"}) {
                Instance invalid;auto c=config;c[key]=(std::string(key)=="write_base"?max_write:max_read)+1;
                check(invalid.device().initialize(bytes(c.dump()))==Status::Invalid,"overflow config");
                Instance edge;c[key]=(std::string(key)=="write_base"?max_write:max_read);
                check(edge.device().initialize(bytes(c.dump()))==Status::Ok,"edge config");
            }
            {Instance overlap;auto c=config;c["read_base"]=100;
             check(overlap.device().initialize(bytes(c.dump()))==Status::Invalid,"overlap");}
            {Instance invalid;auto c=config;c["address"]="127.0.0.1.invalid";
             check(invalid.device().initialize(bytes(c.dump()))==Status::Invalid,"non numeric address");}
            {Instance invalid;auto text=config.dump();text.insert(1,"\"port\":102,");
             check(invalid.device().initialize(bytes(text))==Status::Invalid,"duplicate key");}
            {QTcpServer unused;check(unused.listen(QHostAddress::LocalHost,0),"unused port");
             auto c=config;c["port"]=unused.serverPort();unused.close();
             Instance refused;check(refused.device().initialize(bytes(c.dump()))==Status::Ok,"refused config");
             check(refused.device().connect(bytes(R"({"session_id":"refused"})"))==Status::Failed,"refused connection");}
            Instance instance;auto& d=instance.device();
            check(d.initialize(bytes(config.dump()))==Status::Ok,"initialize");
            check(d.connect(bytes(R"({"session_id":"session-a"})"))==Status::Ok,"connect");
            check(d.connect(bytes(R"({"session_id":"session-b"})"))==Status::Invalid,"session replacement");
            J request={{"session_id","session-a"},{"command_sequence","1"},{"ready",true},{"result_id","result-a"},{"quality","NG"}};
            auto stale=request;stale["session_id"]="session-b";
            check(d.try_command(bytes(stale.dump()))==Status::Invalid&&server.writes==0,"stale session wrote");
            auto bad=request;bad["quality"]="invalid";
            check(d.try_command(bytes(bad.dump()))==Status::Invalid&&server.writes==0,"bad verdict wrote");
#ifdef TEST_S7
            if(server.fault=="disconnect")check(Srv_Stop(snap)==0,"disconnect server");
#endif
            auto status=d.try_command(bytes(request.dump()));
            if(server.fault=="normal"){
                check(status==Status::Ok,"exchange");
                check(d.try_command(bytes(request.dump()))==Status::Busy,"pending report overwritten");
                std::array<std::uint8_t,1024> out{};Buffer b{out.data(),1,9};
                check(d.poll_input(b)==Status::Invalid&&b.size==0,"short report");
                b.capacity=1024;check(d.poll_input(b)==Status::Ok,"poll");
                auto report=J::parse(out.data(),out.data()+b.size);
                check(report["ack_id"]=="result-a"&&report["command_sequence"]=="1"&&report["ready"]==true,"business ack");
                check(d.try_command(bytes(request.dump()))==Status::Invalid&&server.writes==1,"duplicate wrote");
                request["command_sequence"]="2";check(d.try_command(bytes(request.dump()))==Status::Ok,"next sequence");
            }else {
                check(status==Status::Failed,"fault accepted");
                request["command_sequence"]="2";
                auto count=server.writes.load();
                check(d.try_command(bytes(request.dump()))==Status::Cancelled&&server.writes==count,"uncertain write replayed");
            }
            d.request_stop();check(d.try_command(bytes(request.dump()))==Status::Cancelled,"stop");
            result=0;
        }catch(const std::exception& e){std::cerr<<e.what()<<'\n';result=1;}
    });
    QTimer timer;timer.setInterval(5);
    QObject::connect(&timer,&QTimer::timeout,&app,[&]{if(result>=0)app.exit(result);});
    timer.start();auto status=app.exec();job.get();
#ifdef TEST_S7
    Srv_Stop(snap);Srv_Destroy(&snap);
#endif
    return status;
}
