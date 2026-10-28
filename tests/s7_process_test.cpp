#include <vision/capture/qt/session.hpp>
#include "plc_test_server.hpp"
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
    Server server;server.fault=scenario.toStdString();
    QTcpServer reservation;if(!reservation.listen(QHostAddress::LocalHost,0))return 1;
    auto port=reservation.serverPort();reservation.close();
    auto snap=Srv_Create();
    if(!snap||Srv_SetParam(snap,p_u16_LocalPort,&port)||Srv_SetRWAreaCallback(snap,exchange,&server)||Srv_StartTo(snap,"127.0.0.1"))return 1;
    auto guard=std::make_shared<runtime::qt::StationGuard>("s7-test-"+QUuid::createUuid().toString(QUuid::WithoutBraces));
    const auto parameters=QJsonDocument(QJsonObject{{"address","127.0.0.1"},{"port",port},{"rack",0},{"slot",1},{"db",1},{"write_base",100},{"read_base",200}}).toJson(QJsonDocument::Compact).toStdString();
    application::qt::DeviceSession device(guard,args[1],args[2],parameters);if(!device.start())return 1;
    capture::qt::Session peer(guard,args[1],QString::fromUtf8(VISION_DEMO_CAMERA),
        R"({"width":8,"height":4,"seed":42,"fault":"none","fault_at":"1","delay_ms":0})",
        "healthy-camera",{8,4,8,0,32,contracts::PixelFormat::Mono8},2);
    if(!peer.start())return 1;
    unsigned healthy=0;bool peer_active=false,unknown=false;
    QElapsedTimer time;time.start();bool submitted=false,stopping=false,acked=false;QTimer timer;timer.setInterval(10);
    QObject::connect(&timer,&QTimer::timeout,&app,[&] {
        peer.pulse();
        if(auto completion=peer.take_completion()){
            if(completion->state!="Succeeded"||!completion->frame||!peer.release(*completion->frame)){app.exit(1);return;}
            ++healthy;peer_active=false;
        }
        if(!peer_active&&peer.ready()&&(healthy<2||(stopping&&healthy<3))){
            peer_active=peer.capture(1000000000);if(!peer_active){app.exit(1);return;}
        }
        device.pulse(true,false,false,false); // Physical input must come from TCP registers, not these demo flags.
        if(!stopping&&device.snapshot().online&&!submitted) {
            if(scenario=="disconnect")Srv_Stop(snap);
            auto event=*application::run_demo(application::DemoScenario::Ng).event;
            event.result.run_id=contracts::RunId(guard->run_id());
            for(auto& c:event.result.checks)c.correlation.run_id=event.result.run_id;
            submitted=device.submit(event);if(!submitted){app.exit(1);return;}
        }
        if(auto report=device.take_report()) {
            unknown=report->state==contracts::DeliveryState::Unknown;
            acked=report->state==contracts::DeliveryState::BusinessAcked;
            if((scenario=="normal")!=acked){app.exit(1);return;}
            device.stop();stopping=true;
        }
        if(!stopping&&time.elapsed()>600&&scenario!="normal") {
            if(device.snapshot().online){app.exit(1);return;}device.stop();stopping=true;
        }
        if(stopping&&healthy>=3){peer.stop();}
        if(stopping&&healthy>=3&&!peer.worker().process_alive&&!device.worker().process_alive){
            if((scenario=="old_ack"||scenario=="disconnect")&&(!submitted||!unknown)){app.exit(1);return;}
            app.exit((scenario=="normal")?acked&&server.writes>=2?0:1:0);return;}
        if(time.elapsed()>6000){device.stop();app.exit(1);}
    });
    timer.start();auto result=app.exec();Srv_Stop(snap);Srv_Destroy(&snap);return result;
}
