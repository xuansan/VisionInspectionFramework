#include <vision/application/qt/device.hpp>
#include <vision/application/demo.hpp>
#include <QTcpServer>
#include <QTcpSocket>
#include <QCoreApplication>
#include <QJsonObject>
#include <QJsonDocument>
#include <QUuid>
#include <iostream>
using namespace vision;
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);const auto args=app.arguments();const auto scenario=args[3];
    QTcpServer server;if(!server.listen(QHostAddress::LocalHost,0))return 1;
    QByteArray pending;std::vector<std::uint16_t> registers(24);unsigned exchanges=0;
    QObject::connect(&server,&QTcpServer::newConnection,&app,[&] {
        auto* socket=server.nextPendingConnection();
        QObject::connect(socket,&QTcpSocket::readyRead,&app,[&,socket] {
            pending+=socket->readAll();
            auto number=[](const QByteArray& b,int p){return (static_cast<unsigned char>(b[p])<<8)|static_cast<unsigned char>(b[p+1]);};
            while(pending.size()>=7) {
                const int length=number(pending,4);if(length<2||length>254){socket->abort();return;}if(pending.size()<6+length)return;
                const auto request=pending.first(6+length);pending.remove(0,6+length);
                if(scenario=="timeout")continue;
                if(scenario=="disconnect"){socket->abort();return;}
                QByteArray pdu;auto put=[&](unsigned n){pdu.append(static_cast<char>(n>>8));pdu.append(static_cast<char>(n));};
                if(request[7]==16) {
                    if(number(request,8)!=100||number(request,10)!=24||request.size()!=61){app.exit(2);return;}
                    for(unsigned i=0;i<24;++i)registers[i]=static_cast<std::uint16_t>(number(request,13+2*i));
                    pdu=request.mid(7,5);
                } else if(request[7]==3) {
                    pdu.append(char(3));pdu.append(char(50));
                    for(unsigned i=0;i<12;++i)put(registers[i]);
                    put(registers[12]?7:6);put(0);put(0);put(0);put(1);
                    for(unsigned i=13;i<21;++i)put(scenario=="old_ack"?0:registers[i]);
                    ++exchanges;
                } else {app.exit(2);return;}
                QByteArray response=request.first(4);
                response.append(char(0));response.append(static_cast<char>(pdu.size()+1));response.append(request[6]);response+=pdu;
                if(scenario=="wrong_transaction")response[1]=char(static_cast<unsigned char>(response[1])+1);
                if(scenario=="fragment") {
                    socket->write(response.first(3));QTimer::singleShot(2,socket,[socket,response]{socket->write(response.mid(3));});
                } else socket->write(response);
            }
        });
    });
    auto guard=std::make_shared<runtime::qt::StationGuard>("modbus-test-"+QUuid::createUuid().toString(QUuid::WithoutBraces));
    const auto parameters=QJsonDocument(QJsonObject{{"address","127.0.0.1"},{"port",server.serverPort()},{"unit",1},{"write_base",100},{"read_base",200}}).toJson(QJsonDocument::Compact).toStdString();
    application::qt::DeviceSession device(guard,args[1],args[2],parameters);if(!device.start())return 1;
    QElapsedTimer time;time.start();bool submitted=false,stopping=false,acked=false;QTimer timer;timer.setInterval(10);
    QObject::connect(&timer,&QTimer::timeout,&app,[&] {
        device.pulse(true,false,false,false); // Physical input must come from TCP registers, not these demo flags.
        if(!stopping&&device.snapshot().online&&!submitted) {
            auto event=*application::run_demo(application::DemoScenario::Ng).event;
            event.result.run_id=contracts::RunId(guard->run_id());
            for(auto& c:event.result.checks)c.correlation.run_id=event.result.run_id;
            submitted=device.submit(event);if(!submitted){app.exit(1);return;}
        }
        if(auto report=device.take_report()) {
            acked=report->state==contracts::DeliveryState::BusinessAcked;
            if((scenario=="normal"||scenario=="fragment")!=acked){app.exit(1);return;}
            device.stop();stopping=true;
        }
        if(!stopping&&time.elapsed()>600&&scenario!="normal"&&scenario!="fragment") {
            if(device.snapshot().online){app.exit(1);return;}device.stop();stopping=true;
        }
        if(stopping&&!device.worker().process_alive){app.exit((scenario=="normal"||scenario=="fragment")?acked&&exchanges>=2?0:1:0);return;}
        if(time.elapsed()>6000){device.stop();app.exit(1);}
    });
    timer.start();return app.exec();
}
