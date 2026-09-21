#include <vision/application/qt/device.hpp>
#include <vision/application/qt/station.hpp>
#include <QCoreApplication>
#include <QUuid>
#include <iostream>
#include <thread>
using namespace vision;
namespace {
void check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
int run(QCoreApplication& app,const QStringList& args) {
    const auto scenario=args[1].toStdString();
    auto guard=std::make_shared<runtime::qt::StationGuard>("device-test-"+QUuid::createUuid().toString(QUuid::WithoutBraces));
    const auto fault=scenario=="lost_ack"||scenario=="old_ack"||scenario=="disconnect"||scenario=="hang"||scenario=="crash"?scenario:"none";
    application::qt::DeviceSession device(guard,args[2],args[3],"{\"fault\":\""+fault+"\"}");
    application::qt::StationConfig config;config.host=args[2];config.camera_manifest=args[4];config.algorithm_manifest=args[5];
    application::qt::Station station(guard,config);
    check(device.start()&&station.start(),"start");
    bool stopping=false,submitted=false,delivered=false,paused=false,frozen=false;
    unsigned results=0,high_ticks=0,low_ticks=0;std::uint64_t arrival_seen=0;
    QElapsedTimer time;time.start();QTimer loop;loop.setInterval(10);
    QObject::connect(&loop,&QTimer::timeout,&app,[&] {
        try {
            station.pulse();
            if(stopping) {
                device.pulse(false,false);
                if(!device.worker().process_alive&&station.snapshot().state==application::ProductionState::Stopped) {
                    check(station.snapshot().counts.total==results,"count");
                    std::cout<<scenario<<": results="<<results<<", delivered="<<delivered<<"\n";app.exit(0);
                }
                return;
            }
            bool high=station.snapshot().ready||submitted;
            if(scenario=="edges"&&arrival_seen==1) {
                if(high_ticks++<20)high=true;
                else if(low_ticks++<5)high=false;
                else high=true;
            }
            const bool safe=scenario!="safety";
            device.pulse(!paused&&station.snapshot().ready,high,true,safe);
            const auto state=device.snapshot();
            if(scenario=="lease"&&state.online&&!frozen) {
                frozen=true;std::this_thread::sleep_for(std::chrono::milliseconds(250));
                check(!device.snapshot().ready&&!device.snapshot().online,"stale ready");
            }
            if(state.online&&safe&&scenario!="lease"&&state.arrival_sequence>arrival_seen&&station.snapshot().ready) {
                check(state.arrival_sequence==arrival_seen+1,"duplicate edge");
                arrival_seen=state.arrival_sequence;
                check(station.trigger(arrival_seen)==application::qt::TriggerStatus::Accepted,"arrival trigger");
                submitted=true;
                if(scenario=="pause") {station.pause();paused=true;}
            }
            if(auto event=station.take_result()) {
                ++results;
                check(event->result.quality==contracts::QualityVerdict::OK,"station result");
                if(scenario!="edges"||results==2) {
                    check(device.submit(*event),"result submit");
                    check(!device.submit(*event),"duplicate physical delivery");
                }
            }
            if(auto report=device.take_report()) {
                const bool unknown=scenario=="lost_ack"||scenario=="old_ack";
                check(report->state==(unknown?contracts::DeliveryState::Unknown:contracts::DeliveryState::BusinessAcked),"ACK state");
                delivered=!unknown;
                if(unknown)check(!device.snapshot().ready,"unknown retained ready");
                device.stop();station.stop();stopping=true;
            }
            const bool unavailable=scenario=="disconnect"||scenario=="hang"||scenario=="crash"||scenario=="lease";
            if(unavailable&&time.elapsed()>500&&!state.online) {
                check(results==0||scenario=="lease","unavailable triggered");device.stop();station.stop();stopping=true;
            }
            if(scenario=="safety"&&time.elapsed()>400&&state.online) {
                check(!state.ready&&!state.safety_ok&&results==0,"safety interlock");
                device.stop();station.stop();stopping=true;
            }
            check(time.elapsed()<8000,"device deadline");
        }catch(const std::exception& e) {std::cerr<<e.what()<<'\n';app.exit(1);}
    });
    loop.start();return app.exec();
}
}
int main(int argc,char** argv) {QCoreApplication app(argc,argv);try{return run(app,app.arguments());}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
