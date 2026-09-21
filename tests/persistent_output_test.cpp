#include <vision/application/qt/dispatcher.hpp>
#include <vision/application/demo.hpp>
#include <vision/storage/store.hpp>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUuid>
#include <fstream>
using namespace vision;
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);const auto args=app.arguments();QTemporaryDir temp;
    auto guard=std::make_shared<runtime::qt::StationGuard>("persistent-"+QUuid::createUuid().toString(QUuid::WithoutBraces));
    const auto parameters=QJsonDocument(QJsonObject{{"root",temp.path()},{"outputs_json","[\"file\",\"http\"]"}}).toJson(QJsonDocument::Compact).toStdString();
    application::qt::Dispatcher dispatcher(guard,args[1],{{"storage",parameters,args[2],2},{"peer","{}",args[3],2}});
    if(args[4]=="corrupt"){std::ofstream file(std::filesystem::path(temp.path().toStdWString())/"results.sqlite");file<<"broken";}
    if(!dispatcher.start())return 1;
    auto event=*application::run_demo(application::DemoScenario::Ng).event;
    bool submitted=false,stopping=false;QElapsedTimer time;time.start();QTimer timer;timer.setInterval(10);
    QObject::connect(&timer,&QTimer::timeout,&app,[&] {
        dispatcher.pulse();
        if(!submitted&&dispatcher.ready())submitted=dispatcher.submit(event,5000000000ULL);
        if(submitted&&!stopping&&dispatcher.idle()) {
            const auto reports=dispatcher.deliveries();bool peer=false,stored=false;
            for(const auto& report:reports){if(report.output_id=="peer")peer=report.state=="BusinessAcked";else stored=report.state=="BusinessAcked";}
            if(!peer||stored!=(args[4]=="normal")){app.exit(1);return;}
            dispatcher.stop();stopping=true;
        }
        if(stopping) {
            bool alive=false;for(const auto& worker:dispatcher.workers())alive|=worker.process_alive;
            if(!alive) {
                try{if(args[4]=="normal"){storage::Store store(std::filesystem::path(temp.path().toStdWString()));if(store.query(0).size()!=1||store.pending().size()!=2){app.exit(1);return;}}app.exit(0);}
                catch(...){app.exit(1);}return;
            }
        }
        if(time.elapsed()>9000){dispatcher.stop();app.exit(1);}
    });
    timer.start();return app.exec();
}
