#include <vision/application/qt/dispatcher.hpp>
#include <vision/application/demo.hpp>
#include <vision/inference/engine.hpp>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#define NOMINMAX
#include <windows.h>
using namespace vision;
int main(int argc,char** argv){
    QCoreApplication app(argc,argv);QTemporaryDir root;const auto args=app.arguments();
    auto event=*application::run_demo(application::DemoScenario::Ng).event;
    const auto id=event.event_id.value(),hash=inference::sha256(std::as_bytes(std::span(id.data(),id.size()))).substr(7);
    const auto partial=std::filesystem::path(root.path().toStdWString())/(hash+".partial");
    struct Handle{HANDLE value;~Handle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}};
    Handle locked{CreateFileW(partial.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr)};
    if(locked.value==INVALID_HANDLE_VALUE)return 1;
    const auto parameters=QJsonDocument(QJsonObject{{"root",root.path()},{"format","jsonl"},{"max_files",8}}).toJson(QJsonDocument::Compact).toStdString();
    auto guard=std::make_shared<runtime::qt::StationGuard>("file-"+QUuid::createUuid().toString(QUuid::WithoutBraces));
    application::qt::Dispatcher dispatcher(guard,args[1],{{"file",parameters,args[2],2},{"peer","{}",args[3],2}});
    if(!dispatcher.start())return 1;
    bool submitted=false,stopping=false;QElapsedTimer elapsed;elapsed.start();QTimer timer;timer.setInterval(10);
    QObject::connect(&timer,&QTimer::timeout,&app,[&]{
        dispatcher.pulse();
        if(!submitted&&dispatcher.ready())submitted=dispatcher.submit(event,5000000000ULL);
        if(submitted&&!stopping&&dispatcher.idle()){
            const auto reports=dispatcher.deliveries();if(reports.size()!=2){app.exit(1);return;}
            for(const auto& report:reports)if(report.state!=(report.output_id=="file"?"Failed":"BusinessAcked")){app.exit(1);return;}
            dispatcher.stop();stopping=true;
        }
        if(stopping){bool alive=false;for(const auto& worker:dispatcher.workers())alive|=worker.process_alive;if(!alive){app.exit(0);return;}}
        if(elapsed.elapsed()>9000){dispatcher.stop();app.exit(1);}
    });
    timer.start();return app.exec();
}
