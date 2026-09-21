#include <vision/application/qt/dispatcher.hpp>
#include <vision/application/demo.hpp>
#include <QCoreApplication>
#include <QUuid>
#include <iostream>
int main(int argc,char** argv){
    QCoreApplication app(argc,argv);const auto args=app.arguments();if(args.size()!=5)return 64;
    const auto mode=args[1].toStdString();if(mode!="normal"&&mode!="reject"&&mode!="drop-ack")return 64;
    using namespace vision;
    try{
        auto guard=std::make_shared<runtime::qt::StationGuard>("@PROJECT_NAME@-"+QUuid::createUuid().toString(QUuid::WithoutBraces));
        application::qt::Dispatcher output(guard,args[2],{{"custom","{\"mode\":\""+mode+"\"}",args[3],1},{"healthy","{}",args[4],1}});
        if(!output.start())return 1;
        auto event=*application::run_demo(application::DemoScenario::Ok).event;
        bool submitted=false,stopping=false,healthy_early=false;QElapsedTimer elapsed;elapsed.start();QTimer timer;timer.setInterval(10);
        QObject::connect(&timer,&QTimer::timeout,&app,[&]{try{
            output.pulse();
            if(elapsed.elapsed()>10000)throw std::runtime_error("Output test deadline");
            if(stopping){bool alive=false;for(const auto& w:output.workers())alive|=w.process_alive;if(!alive){std::cout<<"@PROJECT_NAME@: "<<mode<<" isolated receiver passed\n";app.exit(0);}return;}
            if(!submitted&&output.ready()){
                if(!output.submit(event,2000000000)||output.submit(event,2000000000))throw std::runtime_error("Admission/duplicate");submitted=true;
            }
            const auto rows=output.deliveries();
            if(rows.size()==2&&rows[0].state=="Accepted"&&rows[1].state=="BusinessAcked")healthy_early=true;
            if(submitted&&output.idle()){
                if(rows.size()!=2||rows[1].state!="BusinessAcked"||rows[0].state!=(mode=="normal"?"BusinessAcked":mode=="reject"?"Failed":"Unknown"))throw std::runtime_error("Delivery outcome");
                if(mode=="drop-ack"&&!healthy_early)throw std::runtime_error("Healthy lane was blocked");
                output.stop();stopping=true;
            }
        }catch(const std::exception& e){std::cerr<<e.what();output.stop();app.exit(1);}});
        timer.start();return app.exec();
    }catch(const std::exception& e){std::cerr<<e.what();return 1;}
}
