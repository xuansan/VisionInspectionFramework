#include <vision/capture/qt/session.hpp>
#include <QCoreApplication>
#include <QUuid>
#include <nlohmann/json.hpp>
#include <iostream>
using namespace vision;
int main(int argc,char** argv){
    QCoreApplication app(argc,argv);
    auto args=app.arguments();if(args.size()!=4)return 1;
    const auto fault=args[3].toStdString();
    auto guard=std::make_shared<runtime::qt::StationGuard>("hik-"+QUuid::createUuid().toString(QUuid::WithoutBraces));
    auto params=[](std::string serial){return nlohmann::json{{"serial",serial},{"width",8},{"height",4},
        {"format","Mono8"},{"exposure_us",1000},{"gain",1}}.dump();};
    contracts::ImageLayout layout{8,4,8,0,32,contracts::PixelFormat::Mono8};
    capture::qt::Session primary(guard,args[1],args[2],params(fault),"hik-a",layout,2);
    capture::qt::Session peer(guard,args[1],args[2],params("normal"),"hik-b",layout,2);
    if(!primary.start()||!peer.start())return 2;
    bool submitted=false,done=false,active=false,stopping=false;
    unsigned healthy=0;QTimer timer;timer.setInterval(10);
    QObject::connect(&timer,&QTimer::timeout,&app,[&]{
        primary.pulse();peer.pulse();
        if(auto c=primary.take_completion()){
            const bool success=fault=="normal";
            if((c->state=="Succeeded")!=success||bool(c->frame)!=success){app.exit(3);return;}
            if(c->frame&&!primary.release(*c->frame)){app.exit(4);return;}
            done=true;
        }
        if(auto c=peer.take_completion()){
            if(c->state!="Succeeded"||!c->frame){app.exit(5);return;}
            bool pixels_ok=false;
            if(!peer.read(*c->frame,[&](auto pixels){pixels_ok=pixels.size()==32;
                for(auto p:pixels)pixels_ok=pixels_ok&&p==std::byte{73};})||!pixels_ok||!peer.release(*c->frame)){app.exit(6);return;}
            ++healthy;active=false;
        }
        if(stopping){
            if(!primary.worker().process_alive&&!peer.worker().process_alive)
                app.exit(primary.buffers().leases==0&&peer.buffers().leases==0?0:7);
            return;
        }
        if(!submitted&&primary.ready()){submitted=primary.capture(200000000);if(!submitted){app.exit(8);return;}}
        // The last healthy capture must run after the primary fault has been observed.
        if(!active&&peer.ready()&&(healthy<3||(done&&healthy<4))){active=peer.capture(1000000000);if(!active){app.exit(9);return;}}
        if(done&&healthy>=4){primary.stop();peer.stop();stopping=true;}
    });
    QTimer::singleShot(8000,&app,[&]{app.exit(10);});
    timer.start();return app.exec();
}
