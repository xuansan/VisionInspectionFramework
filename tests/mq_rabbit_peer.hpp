#pragma once
#include <QTcpServer>
#include <QTcpSocket>
#include <nlohmann/json.hpp>
// Bounded AMQP protocol fixture for fault injection, NOT a RabbitMQ implementation.
class RabbitPeer:public QTcpServer {
    std::string scenario_;
    static QByteArray number(quint64 n,int bytes){QByteArray b;for(int i=bytes-1;i>=0;--i)b.append(char((n>>(i*8))&255));return b;}
    static QByteArray shortstr(const QByteArray& s){return number(s.size(),1)+s;}
    static QByteArray longstr(const QByteArray& s){return number(s.size(),4)+s;}
    static void frame(QTcpSocket* socket,int type,int channel,const QByteArray& body){
        socket->write(number(type,1)+number(channel,2)+number(body.size(),4)+body+QByteArray(1,char(206)));
    }
    static void method(QTcpSocket* socket,int channel,int cls,int id,const QByteArray& body={}){
        frame(socket,1,channel,number(cls,2)+number(id,2)+body);
    }
    struct State {QByteArray buffer,body;quint64 size{};bool header=false,delivered=false;};
public:
    unsigned port{};unsigned requests{};bool valid=true;
    explicit RabbitPeer(std::string scenario):scenario_(std::move(scenario)){
        if(!listen(QHostAddress::LocalHost,0))throw std::runtime_error("AMQP fixture bind");port=serverPort();
        QObject::connect(this,&QTcpServer::newConnection,this,[this]{
            while(auto* socket=nextPendingConnection()){
                auto state=std::make_shared<State>();
                QObject::connect(socket,&QTcpSocket::readyRead,this,[this,socket,state]{
                    state->buffer+=socket->readAll();
                    if(state->buffer.size()>65536){valid=false;socket->abort();return;}
                    auto value=[](const QByteArray& b,int offset,int count){quint64 n=0;for(int i=0;i<count;++i)n=(n<<8)|static_cast<unsigned char>(b[offset+i]);return n;};
                    if(!state->header){
                        if(state->buffer.size()<8)return;
                        if(state->buffer.left(8)!=QByteArray::fromHex("414d515000000901")){valid=false;socket->abort();return;}
                        state->buffer.remove(0,8);state->header=true;
                        method(socket,0,10,10,QByteArray::fromHex("0009")+number(0,4)+longstr("PLAIN")+longstr("en_US"));
                    }
                    while(state->buffer.size()>=8){
                        auto size=value(state->buffer,3,4);
                        if(size>32768){valid=false;socket->abort();return;}
                        if(state->buffer.size()<static_cast<qsizetype>(size+8))return;
                        auto type=value(state->buffer,0,1),channel=value(state->buffer,1,2);
                        const auto body=state->buffer.mid(7,static_cast<qsizetype>(size));state->buffer.remove(0,static_cast<qsizetype>(size+8));
                        if(type==8)continue;
                        if(type==2){
                            if(body.size()<14){valid=false;socket->abort();return;}
                            state->size=value(body,4,8);state->body.clear();
                            if(state->size>32768){valid=false;socket->abort();return;}
                        }else if(type==3){
                            state->body+=body;if(state->body.size()>32768){valid=false;socket->abort();return;}
                            if(state->body.size()==static_cast<qsizetype>(state->size)){
                                ++requests;state->delivered=false;
                                if(scenario_=="unroutable")method(socket,1,60,50,number(312,2)+shortstr("NO_ROUTE")+shortstr("")+shortstr("results"));
                                else if(scenario_=="disconnect")socket->abort();
                                else if(scenario_!="no-confirm")method(socket,1,60,80,number(1,8)+number(0,1));
                            }
                        }else if(type==1&&body.size()>=4){
                            auto cls=value(body,0,2),id=value(body,2,2);
                            if(cls==10&&id==11)method(socket,0,10,30,number(2,2)+number(4096,4)+number(1,2));
                            else if(cls==10&&id==31){}
                            else if(cls==10&&id==40)method(socket,0,10,41,shortstr(""));
                            else if(cls==20&&id==10)method(socket,static_cast<int>(channel),20,11,longstr(""));
                            else if(cls==85&&id==10)method(socket,1,85,11);
                            else if(cls==50&&id==10)method(socket,1,50,11,shortstr("reply")+number(0,4)+number(0,4));
                            else if(cls==60&&id==40){}
                            else if(cls==60&&id==70){
                                if(scenario_=="broker-only"||state->delivered){method(socket,1,60,72,shortstr(""));continue;}
                                try{
                                    const auto request=nlohmann::json::parse(state->body.toStdString());
                                    auto ack=nlohmann::json{{"event_id",request["event"]["event_id"]},
                                        {"output_instance_id",request["output_instance_id"]},{"attempt",request["attempt"]},{"state","BusinessAcked"}};
                                    if(scenario_=="wrong-id")ack["event_id"]="wrong";
                                    if(scenario_=="wrong-attempt")ack["attempt"]="999";
                                    auto text=ack.dump();if(scenario_=="malformed")text="{";
                                    if(scenario_=="oversized")text=std::string(8192,'x');
                                    if(scenario_=="duplicate")text="{\"event_id\":\"a\",\"event_id\":\"b\"}";
                                    method(socket,1,60,71,number(1,8)+number(0,1)+shortstr("")+shortstr("reply")+number(0,4));
                                    frame(socket,2,1,number(60,2)+number(0,2)+number(text.size(),8)+number(0,2));
                                    frame(socket,3,1,QByteArray::fromStdString(text));state->delivered=true;
                                }catch(...){valid=false;socket->abort();}
                            }else {valid=false;socket->abort();}
                        }
                    }
                });
            }
        });
    }
    ~RabbitPeer(){for(auto* s:findChildren<QTcpSocket*>())s->abort();}
};
