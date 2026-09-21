#include <vision/application/qt/station.hpp>
#include <vision/serialization/json_codec.hpp>
#include <QCoreApplication>
#include <QUuid>
#include <iostream>
using namespace vision;
namespace {
void check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
int run(QCoreApplication& app,const QStringList& args) {
    const auto scenario=args[1].toStdString();
    auto guard=std::make_shared<runtime::qt::StationGuard>("station-"+QUuid::createUuid().toString(QUuid::WithoutBraces));
    application::qt::StationConfig config;config.host=args[2];config.camera_manifest=args[3];config.algorithm_manifest=args[4];
    if(scenario=="ng")config.algorithm_parameters[1]=R"({"minimum":255,"maximum":255})";
    if(scenario=="timeout") {config.algorithm_parameters[0]=R"({"fault":"hang"})";config.budget_ns=200000000;}
    if(scenario=="missing") {config.camera_parameters[1]=R"({"width":8,"height":4,"fault":"missing"})";config.budget_ns=200000000;}
    if(scenario=="crash")config.algorithm_parameters[1]=R"({"fault":"crash"})";
    if(scenario=="duplicate"||scenario=="out_of_order")
        config.camera_parameters[0]="{\"width\":8,\"height\":4,\"fault\":\""+scenario+"\",\"fault_at\":\""+(scenario=="duplicate"?"2":"3")+"\"}";
    if(scenario=="pause"||scenario=="cancel")config.algorithm_parameters[1]=R"({"fault":"delay","delay_ms":80})";
    application::qt::Station station(guard,config);
    check(!station.start(contracts::Mode::Production),"Production enabled");
    check(station.start(),"start");
    unsigned submitted=0,completed=0;bool stopping=false,paused=false,held=false;
    const unsigned target=scenario=="normal"||scenario=="out_of_order"?3:scenario=="duplicate"||scenario=="pause"?2:1;
    QElapsedTimer time;time.start();QTimer loop;loop.setInterval(10);
    QObject::connect(&loop,&QTimer::timeout,&app,[&] {
        try {
            station.pulse();
            if(scenario=="capacity"&&submitted&&!held&&station.snapshot().counts.total) {
                check(station.trigger(2)==application::qt::TriggerStatus::Full,"unread result overwritten");held=true;
            }
            if(auto result=station.take_result()) {
                ++completed;
                check(result->result.inspection_id.value()=="inspection-"+std::to_string(completed),"wrong workpiece");
                check(result->result.mode==contracts::Mode::Demo,"wrong mode");
                check(result->result.recipe_hash==config.recipe_hash,"recipe mix");
                (void)serialization::decode_result(serialization::encode_result(*result));
                const bool failure=scenario=="timeout"||scenario=="missing"||scenario=="crash"||scenario=="cancel"||
                    ((scenario=="duplicate"||scenario=="out_of_order")&&completed==target);
                check(result->result.quality==(failure?contracts::QualityVerdict::Unknown:
                    scenario=="ng"?contracts::QualityVerdict::NG:contracts::QualityVerdict::OK),"quality");
                check(result->result.checks.size()==2,"required checks missing");
                check(station.snapshot().counts.total==completed,"counter mismatch");
                check(!station.take_result(),"duplicate terminal");
                check(station.trigger(completed)==application::qt::TriggerStatus::Duplicate,"duplicate trigger");
            }
            if(stopping) {
                if(station.snapshot().state==application::ProductionState::Stopped) {
                    check(station.snapshot().leases==0&&station.snapshot().tickets==0,"resource leak");
                    std::cout<<scenario<<": results="<<completed<<", leases=0, tickets=0\n";app.exit(0);
                }
                return;
            }
            if(scenario=="pause"&&completed==1&&station.snapshot().state==application::ProductionState::Paused&&!station.snapshot().active) {
                check(station.trigger(2)==application::qt::TriggerStatus::NotReady,"pause admitted");
                check(station.resume(),"resume");
            }
            if(completed==target&&!station.snapshot().active) {station.stop();stopping=true;return;}
            if(submitted<target&&station.snapshot().ready) {
                check(station.trigger(submitted+1)==application::qt::TriggerStatus::Accepted,"trigger");++submitted;
                check(station.trigger(submitted)==application::qt::TriggerStatus::Duplicate,"double admission");
                check(station.trigger(submitted+1)==application::qt::TriggerStatus::Full,"inflight capacity");
                if(scenario=="pause"&&!paused) {station.pause();paused=true;}
                if(scenario=="cancel")station.cancel();
            }
            check(time.elapsed()<10000,"test deadline");
        } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';app.exit(1);}
    });
    loop.start();return app.exec();
}
}
int main(int argc,char** argv) {QCoreApplication app(argc,argv);try{return run(app,app.arguments());}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
