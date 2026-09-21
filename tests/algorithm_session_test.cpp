#include <vision/algorithm/qt/session.hpp>
#include <QCoreApplication>
#include <QUuid>
#include <iostream>
using namespace vision;
namespace {
void check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
int run(QCoreApplication& app,const QStringList& args) {
    const auto scenario=args[1].toStdString();
    auto guard=std::make_shared<runtime::qt::StationGuard>("algorithm-"+QUuid::createUuid().toString(QUuid::WithoutBraces));
    const contracts::ImageLayout layout{8,4,8,0,32,contracts::PixelFormat::Mono8};
    capture::qt::Session camera(guard,args[2],args[3],R"({"width":8,"height":4,"seed":42})","camera",layout);
    capture::qt::Session peer_camera(guard,args[2],args[3],R"({"width":8,"height":4})","peer-camera",layout);
    const auto parameters=scenario=="ng"?R"({"minimum":255,"maximum":255})":
        scenario=="timeout"||scenario=="isolation"?R"({"fault":"hang"})":
        scenario=="cancel"?R"({"fault":"delay","delay_ms":1000})":
        scenario=="crash"||scenario=="recovery"?R"({"fault":"crash"})":"{}";
    algorithm::qt::Session algorithm(guard,args[2],args[4],parameters,"algorithm",camera);
    algorithm::qt::Session peer(guard,args[2],args[4],"{}","peer",peer_camera);
    check(camera.start()&&algorithm.start()&&peer_camera.start()&&peer.start(),"start");
    unsigned submitted=0,completed=0,peer_completed=0;
    bool capture_pending=false,peer_pending=false,stopping=false,cancelled=false,restarted=false;
    std::optional<contracts::FrameDescriptor> old;
    QElapsedTimer time;time.start();QTimer loop;loop.setInterval(10);
    QObject::connect(&loop,&QTimer::timeout,&app,[&] {
        try {
            camera.pulse();peer_camera.pulse();algorithm.pulse();peer.pulse();
            if(auto c=camera.take_completion()) {
                check(c->state=="Succeeded"&&c->frame,"capture");
                old=c->frame;
                if(scenario=="invalid") {
                    auto bad=*c->frame;++bad.permit.generation;
                    check(!algorithm.inspect(bad,contracts::InspectionId("part"),contracts::CheckId("light"),1000000000),"stale admitted");
                    check(camera.read(*c->frame,[](auto){}),"invalid request lost original lease");
                }
                check(algorithm.inspect(*c->frame,contracts::InspectionId("part"),contracts::CheckId("light"),
                    scenario=="timeout"||scenario=="isolation"?150000000:1000000000),"inspect");
                check(!camera.read(*c->frame,[](auto){}),"old lease read");
                check(!camera.release(*c->frame),"old lease release");++submitted;capture_pending=false;
            }
            if(auto c=peer_camera.take_completion()) {
                check(c->state=="Succeeded"&&c->frame,"peer capture");
                check(peer.inspect(*c->frame,contracts::InspectionId("peer-part"),contracts::CheckId("light"),1000000000),"peer inspect");
                peer_pending=false;
            }
            if(scenario=="cancel"&&submitted&&!cancelled) {algorithm.cancel();cancelled=true;}
            if(auto c=algorithm.take_completion()) {
                ++completed;
                if(scenario=="normal"||scenario=="invalid"||scenario=="ng") {
                    check(c->state=="Succeeded","success state");check(c->quality==(scenario=="ng"?"NG":"OK"),"quality");
                } else {
                    check(c->quality=="Unknown","failed quality");
                    check(c->state==(scenario=="cancel"?"Cancelled":scenario=="timeout"||scenario=="isolation"?"TimedOut":"Failed"),"failure state");
                }
            }
            if(auto c=peer.take_completion()) {check(c->state=="Succeeded"&&c->quality=="OK","peer affected");++peer_completed;}
            if(stopping) {
                if(!camera.worker().process_alive&&!algorithm.worker().process_alive&&
                   !peer_camera.worker().process_alive&&!peer.worker().process_alive) {
                    check(!camera.buffers().leases&&!peer_camera.buffers().leases,"leaked leases");
                    std::cout<<scenario<<": completed="<<completed<<", healthy="<<peer_completed<<", leases=0\n";app.exit(0);
                }
                return;
            }
            if(scenario=="recovery"&&completed==1&&!restarted&&!algorithm.worker().process_alive&&
               !camera.worker().process_alive&&camera.buffers().leases==0) {
                check(camera.start()&&algorithm.start(),"restart");restarted=true;
                check(!camera.read(*old,[](auto){}),"old pool"); 
            }
            const unsigned target=scenario=="recovery"?2:scenario=="normal"?3:1;
            if(completed==target&&peer_completed>=6) {
                stopping=true;algorithm.stop();peer.stop();camera.stop();peer_camera.stop();return;
            }
            if(submitted<target&&!capture_pending&&camera.ready()&&algorithm.ready()) {
                check(camera.capture(1000000000),"capture submit");capture_pending=true;
            }
            if(peer_completed<6&&!peer_pending&&peer_camera.ready()&&peer.ready()) {
                check(peer_camera.capture(1000000000),"peer submit");peer_pending=true;
            }
            check(time.elapsed()<10000,"test deadline");
        } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';app.exit(1);}
    });
    loop.start();return app.exec();
}
}
int main(int argc,char** argv) {QCoreApplication app(argc,argv);try{return run(app,app.arguments());}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
