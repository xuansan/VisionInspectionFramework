#include <vision/capture/qt/session.hpp>
#include <vision/capture/source.hpp>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QDir>
#include <QUuid>
#include <QJsonDocument>
#include <QJsonObject>
#include <fstream>
#include <iostream>
using namespace vision;
namespace {
void check(bool value,const char* error) {if(!value)throw std::runtime_error(error);}
std::string params(std::string fault) {
    return QJsonDocument(QJsonObject{{"width",8},{"height",4},{"seed",42},{"fault",QString::fromStdString(fault)},
        {"fault_at",fault=="duplicate"?"2":fault=="out_of_order"?"3":"1"},{"delay_ms",200}}).toJson(QJsonDocument::Compact).toStdString();
}
int run(QCoreApplication& app,const QStringList& args) {
    const auto scenario=args[1].toStdString();
    auto guard=std::make_shared<runtime::qt::StationGuard>("capture-session-"+QUuid::createUuid().toString(QUuid::WithoutBraces));
    const contracts::ImageLayout layout{8,4,8,0,32,contracts::PixelFormat::Mono8};
    const auto fault=scenario=="recovery"?"crash":scenario=="timeout"||scenario=="cancel"||scenario=="isolation"?"missing":
        scenario=="crash"||scenario=="disconnect"||scenario=="duplicate"||scenario=="out_of_order"?scenario:"none";
    auto parameters=params(fault);
    if(scenario=="pixel-hash") {
        auto p=QJsonDocument::fromJson(QByteArray::fromStdString(parameters)).object();
        p["pixel_hash"]="sha256:"+QString(64,'0');parameters=QJsonDocument(p).toJson(QJsonDocument::Compact).toStdString();
    }
    if(scenario=="recovery") {
        auto p=QJsonDocument::fromJson(QByteArray::fromStdString(parameters)).object();p["fault_at"]="3";
        parameters=QJsonDocument(p).toJson(QJsonDocument::Compact).toStdString();
    }
    QTemporaryDir files(QDir::currentPath()+"/out/capture-flow-XXXXXX");check(files.isValid(),"asset root");
    capture::SourceConfig config;config.width=8;config.height=4;config.seed=42;
    capture::Source expected(config);
    if(scenario=="replay") {
        for(unsigned n=1;n<=3;++n) {
            auto image=expected.read(n);
            std::ofstream file(std::filesystem::path(files.filePath(QString::number(n)+".pgm").toStdWString()),std::ios::binary);
            file<<"P5\n8 4\n255\n";file.write(reinterpret_cast<const char*>(image.pixels.data()),32);
        }
        auto p=QJsonDocument::fromJson(QByteArray::fromStdString(parameters)).object();
        p["source"]="sequence";p["root"]=files.path();p["files_json"]=R"(["1.pgm","2.pgm","3.pgm"])";
        parameters=QJsonDocument(p).toJson(QJsonDocument::Compact).toStdString();
    }
    capture::qt::Session primary(guard,args[2],args[3],parameters,"camera-a",layout,2);
    capture::qt::Session peer(guard,args[2],args[3],params("none"),"camera-b",layout,2);
    check(primary.start()&&peer.start(),"start sessions");
    unsigned submitted=0,completed=0,peer_completed=0;
    bool peer_active=false,cancelled=false,restarted=false,stopping=false,saw_failure=false,backpressure=false;
    std::uint64_t old_epoch=0;
    std::vector<contracts::FrameDescriptor> held;
    std::optional<contracts::FrameDescriptor> old_frame;
    QElapsedTimer elapsed;elapsed.start();
    QTimer loop;loop.setInterval(10);
    QObject::connect(&loop,&QTimer::timeout,&app,[&] {
        try {
            primary.pulse();peer.pulse();
            if(auto c=primary.take_completion()) {
                ++completed;
                if(c->state=="Succeeded") {
                    check(bool(c->frame),"success frame missing");
                    const auto sequence=restarted?completed-(scenario=="recovery"?3:1):completed;
                    check(primary.read(*c->frame,[&](auto pixels) {
                        check(std::vector<std::byte>(pixels.begin(),pixels.end())==expected.read(sequence).pixels,"primary pixels");
                    }),"primary read");
                    check(!peer.read(*c->frame,[](auto){}),"cross-session frame accepted");
                    if(scenario=="backpressure")held.push_back(*c->frame);
                    else {check(primary.release(*c->frame),"primary release");check(!primary.release(*c->frame),"double release");}
                    if(!old_frame)old_frame=c->frame;
                } else {
                    saw_failure=true;check(!c->frame,"failure returned frame");
                    if(scenario=="cancel")check(c->state=="Cancelled","cancel became success");
                    if(scenario=="timeout"||scenario=="isolation")check(c->state=="TimedOut","missing state");
                }
            }
            if(auto c=peer.take_completion()) {
                check(c->state=="Succeeded"&&c->frame,"healthy peer failed");
                ++peer_completed;peer_active=false;
                check(peer.read(*c->frame,[&](auto pixels) {
                    check(std::vector<std::byte>(pixels.begin(),pixels.end())==expected.read(peer_completed).pixels,"peer pixels");
                }),"peer read");check(peer.release(*c->frame),"peer release");
            }
            if(stopping) {
                if(!primary.worker().process_alive&&!peer.worker().process_alive) {
                    check(primary.buffers().leases==0&&peer.buffers().leases==0,"remaining leases");
                    app.exit(0);
                }
                return;
            }
            const auto peer_target=scenario=="isolation"?6U:4U;
            if(!peer_active&&peer_completed<peer_target&&peer.ready()&&
               (scenario!="isolation"||peer_completed<4||saw_failure)) {
                check(peer.capture(1000000000),"peer capture");peer_active=true;
            }
            if(scenario=="cancel"&&submitted==1&&!cancelled) {
                primary.cancel();cancelled=true;
                check(primary.buffers().leases==1,"cancel reclaimed before OS exit");
                check(!primary.capture(1000000000),"cancel accepted successor before exit");
            }
            if(scenario=="backpressure"&&held.size()==2&&!backpressure) {
                check(primary.ready(),"backpressure readiness");
                check(!primary.capture(1000000000),"pool accepted beyond capacity");
                check(primary.buffers().leases==2,"pool lost held readers");
                check(primary.release(held.front()),"held release");held.erase(held.begin());
                backpressure=true;
            }
            unsigned target=3;
            if(scenario=="recovery")target=4;
            if(scenario=="timeout"||scenario=="cancel"||scenario=="crash"||scenario=="disconnect"||scenario=="isolation")target=1;
            if(scenario=="duplicate")target=2;
            const bool restart_due=!restarted&&((scenario=="restart"&&completed==1)||(scenario=="recovery"&&completed==3));
            if(submitted<target&&primary.ready()&&!restart_due) {
                check(primary.capture(scenario=="timeout"||scenario=="isolation"?100000000:1000000000),"primary capture");
                ++submitted;
            }
            if(restart_due) {
                old_epoch=primary.worker().epoch;primary.stop();
                if(!primary.worker().process_alive) {
                    check(primary.start(),"explicit restart");
                    restarted=true;
                    check(primary.worker().epoch>old_epoch,"epoch not advanced");
                    check(old_frame&&!primary.read(*old_frame,[](auto){}),"old pool accepted");
                }
                return;
            }
            if(completed>=target&&peer_completed>=peer_target) {
                if(fault!="none"||scenario=="pixel-hash")check(saw_failure,"fault did not fail capture");
                if(scenario=="backpressure")check(backpressure,"backpressure not exercised");
                for(const auto& f:held)check(primary.release(f),"final reader release");
                held.clear();primary.stop();peer.stop();stopping=true;
            }
        } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';app.exit(2);}
    });
    loop.start();
    QTimer deadline;deadline.setSingleShot(true);
    QObject::connect(&deadline,&QTimer::timeout,&app,[&] {std::cerr<<"capture flow timeout\n";app.exit(3);});
    deadline.start(8000);
    const auto result=app.exec();
    check(result==0,"capture flow failed");
    std::cout<<scenario<<": captures="<<completed<<" peer="<<peer_completed<<" leases=0\n";return 0;
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {check(app.arguments().size()==4,"arguments");return run(app,app.arguments());}
    catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
