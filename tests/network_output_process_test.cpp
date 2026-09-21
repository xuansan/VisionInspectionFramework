#include <vision/application/qt/dispatcher.hpp>
#include <vision/application/demo.hpp>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUuid>
#include <vector>
using namespace vision;
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);const auto args=app.arguments();QTemporaryDir root;
    QTcpServer reservation;if(!reservation.listen(QHostAddress::LocalHost,0))return 1;
    const auto port=reservation.serverPort();reservation.close();
    qputenv("VISION_HTTP_PROCESS_TOKEN","01234567890123456789012345678901");
    const auto params=QJsonDocument(QJsonObject{{"root",root.path()},{"port",port},{"token_env","VISION_HTTP_PROCESS_TOKEN"}}).toJson(QJsonDocument::Compact).toStdString();
    auto guard=std::make_shared<runtime::qt::StationGuard>("http-"+QUuid::createUuid().toString(QUuid::WithoutBraces));
    application::qt::Dispatcher dispatcher(guard,args[1],{{"http",params,args[2],2},{"peer","{}",args[3],2}});
    if(!dispatcher.start())return 1;
    std::vector<std::unique_ptr<QTcpSocket>> clients;
    auto event=*application::run_demo(application::DemoScenario::Ng).event;
    bool submitted=false,stopping=false;QElapsedTimer elapsed;elapsed.start();QTimer timer;timer.setInterval(10);
    QObject::connect(&timer,&QTimer::timeout,&app,[&]{
        dispatcher.pulse();
        if(!submitted&&dispatcher.ready()){
            // Incomplete requests occupy HTTP connections, while output/control remain independent.
            if(args[4]=="slow")for(unsigned i=0;i<20;++i){
                auto socket=std::make_unique<QTcpSocket>();
                socket->connectToHost(QHostAddress::LocalHost,port);
                if(socket->waitForConnected(30))socket->write("GET /api/v1/events HTTP/1.1\r\n");
                clients.push_back(std::move(socket));
            }
            submitted=dispatcher.submit(event,5000000000ULL);
        }
        if(submitted&&!stopping&&dispatcher.idle()){
            const auto reports=dispatcher.deliveries();
            if(reports.size()!=2){app.exit(1);return;}
            for(const auto& report:reports)if(report.state!="BusinessAcked"){app.exit(1);return;}
            dispatcher.stop();stopping=true;
        }
        if(stopping){
            bool alive=false;for(const auto& worker:dispatcher.workers())alive|=worker.process_alive;
            if(!alive){app.exit(0);return;}
        }
        if(elapsed.elapsed()>9000){dispatcher.stop();app.exit(1);}
    });
    timer.start();return app.exec();
}
