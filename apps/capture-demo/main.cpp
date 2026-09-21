#include <vision/capture/qt/session.hpp>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include <iostream>
#include <array>

int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        const auto args=app.arguments();
        if(args.size()!=3) {
            std::cerr<<"Usage: vision-capture-demo <vision-worker-host.exe> <sim-camera.json>\n";
            return 64;
        }
        using namespace vision;
        auto guard=std::make_shared<runtime::qt::StationGuard>("capture-demo-"+QUuid::createUuid().toString(QUuid::WithoutBraces));
        const contracts::ImageLayout layout{64,48,64,0,3072,contracts::PixelFormat::Mono8};
        capture::qt::Session a(guard,args[1],args[2],R"({"width":64,"height":48,"seed":42})","camera-a",layout);
        capture::qt::Session b(guard,args[1],args[2],R"({"width":64,"height":48,"seed":43})","camera-b",layout);
        if(!a.start()||!b.start())return 1;
        std::array<capture::qt::Session*,2> cameras{&a,&b};
        std::array<unsigned,2> submitted{},completed{};
        bool stopping=false;
        QTimer loop;loop.setInterval(10);
        QObject::connect(&loop,&QTimer::timeout,&app,[&] {
            try {
                for(std::size_t i=0;i<cameras.size();++i) {
                    auto& camera=*cameras[i];camera.pulse();
                    if(auto result=camera.take_completion()) {
                        if(result->state!="Succeeded"||!result->frame)throw std::runtime_error("Capture did not succeed");
                        std::uint64_t sum=0;
                        if(!camera.read(*result->frame,[&](auto pixels) {
                            for(auto pixel:pixels)sum+=std::to_integer<unsigned char>(pixel);
                        }))throw std::runtime_error("Frame is not readable");
                        if(!camera.release(*result->frame))throw std::runtime_error("Frame release failed");
                        ++completed[i];
                        QJsonObject output{{"mode","Demo"},{"camera",static_cast<int>(i+1)},
                            {"task_id",QString::fromStdString(result->task_id)},{"state","Captured"},
                            {"bytes",QString::number(layout.length)},{"pixel_sum",QString::number(sum)},
                            {"production_result",false}};
                        std::cout<<QJsonDocument(output).toJson(QJsonDocument::Compact).toStdString()<<std::endl;
                    }
                    if(!stopping&&submitted[i]<3&&camera.ready()) {
                        if(!camera.capture(1000000000))throw std::runtime_error("Capture admission failed");
                        ++submitted[i];
                    }
                }
                if(completed[0]==3&&completed[1]==3&&!stopping) {a.stop();b.stop();stopping=true;}
                if(stopping&&!a.worker().process_alive&&!b.worker().process_alive) {
                    if(a.buffers().leases||b.buffers().leases)throw std::runtime_error("Outstanding leases");
                    app.exit(0);
                }
            } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';app.exit(1);}
        });
        QTimer timeout;timeout.setSingleShot(true);
        QObject::connect(&timeout,&QTimer::timeout,&app,[&]{std::cerr<<"Demo timeout\n";app.exit(2);});
        loop.start();timeout.start(8000);
        return app.exec();
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
