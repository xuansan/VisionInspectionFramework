#include <vision/application/qt/dispatcher.hpp>
#include <vision/application/demo.hpp>
#include <QCoreApplication>
#include <QUuid>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <iostream>
using namespace vision;
namespace {
void check(bool v,const char* m) {if(!v)throw std::runtime_error(m);}
int run(QCoreApplication& app,const QStringList& args) {
    const auto scenario=args[1].toStdString();
    auto guard=std::make_shared<runtime::qt::StationGuard>("outputs-"+QUuid::createUuid().toString(QUuid::WithoutBraces));
    std::vector<application::qt::OutputConfig> configs{{"healthy","{}",args[3],8},{"other","{}",args[3],2}};
    if(scenario=="hang"||scenario=="crash"||scenario=="lost_ack"||scenario=="fail"||scenario=="wrong_ack")
        configs[1].parameters="{\"fault\":\""+scenario+"\"}";
    auto output=std::make_unique<application::qt::Dispatcher>(guard,args[2],configs);
    check(output->start(),"start");
    auto event=*application::run_demo(application::DemoScenario::Ok).event;
    unsigned submitted=0;bool stopping=false,recovered=false,healthy_early=false;
    QElapsedTimer time;time.start();QTimer loop;loop.setInterval(10);
    QObject::connect(&loop,&QTimer::timeout,&app,[&] {
        try {
            if(stopping) {
                output->pulse();
                bool alive=false;for(const auto& w:output->workers())alive|=w.process_alive;
                if(!alive) {std::cout<<scenario<<": delivery checks passed\n";app.exit(0);}return;
            }
            bool all_ready=true;for(const auto& worker:output->workers())all_ready&=worker.phase==runtime::WorkerPhase::Ready;
            if(!submitted&&all_ready&&output->ready()) {
                check(output->submit(event,2000000000),"submit");++submitted;
                check(!output->submit(event),"duplicate event");
                if(scenario=="capacity")for(unsigned i=0;i<4;++i) {
                    check(output->submit(*application::run_demo(application::DemoScenario::Ng).event,2000000000),"capacity intent");++submitted;
                }
                if(scenario=="restore_pending"||scenario=="expired"||scenario=="bad_snapshot") {
                    auto checkpoint=output->checkpoint();output.reset();
                    output=std::make_unique<application::qt::Dispatcher>(guard,args[2],configs);
                    if(scenario=="expired") {
                        auto root=QJsonDocument::fromJson(QByteArray::fromStdString(checkpoint)).object();
                        auto rows=root["rows"].toArray();auto row=rows[0].toObject();auto intents=row["intents"].toArray();
                        for(qsizetype i=0;i<intents.size();++i) {auto item=intents[i].toObject();item["expires_utc"]="1";intents[i]=item;}
                        row["intents"]=intents;rows[0]=row;root["rows"]=rows;checkpoint=QJsonDocument(root).toJson().toStdString();
                    }
                    if(scenario=="bad_snapshot") {
                        check(!output->restore("{}"),"bad snapshot accepted");
                        check(!output->restore(R"({"version":2,"version":1,"durability":"Volatile","rows":[]})"),"duplicate field accepted");
                        check(output->deliveries().empty(),"partial restore");
                    }
                    check(output->restore(checkpoint),"restore pending");check(output->start(),"restart pending");recovered=true;
                }
            }
            output->pulse();
            if(scenario=="restore_accepted"&&submitted&&!recovered) {
                const auto rows=output->deliveries();bool accepted=false;for(const auto& r:rows)accepted|=r.state=="Accepted";
                if(accepted) {
                    auto checkpoint=output->checkpoint();output.reset();output=std::make_unique<application::qt::Dispatcher>(guard,args[2],configs);
                    check(output->restore(checkpoint),"restore accepted");check(output->start(),"restart accepted");recovered=true;
                }
            }
            const auto rows=output->deliveries();
            if(scenario=="hang"&&rows.size()==2&&rows[0].state=="BusinessAcked"&&rows[1].state=="Accepted")healthy_early=true;
            if(submitted&&output->idle()) {
                check(rows.size()==submitted*2,"intent count");
                if(scenario=="restore_acked"&&!recovered) {
                    auto checkpoint=output->checkpoint();output.reset();output=std::make_unique<application::qt::Dispatcher>(guard,args[2],configs);
                    check(output->restore(checkpoint),"restore acked");check(output->start(),"restart acked");recovered=true;return;
                }
                for(const auto& row:rows) {
                    if(scenario=="expired") {check(row.state=="Expired"&&row.attempt==0,"expired delivered");continue;}
                    if(row.output_id=="healthy")check(row.state=="BusinessAcked","healthy output failed");
                    else if(scenario=="hang"||scenario=="crash"||scenario=="lost_ack"||scenario=="wrong_ack")check(row.state=="Unknown","uncertain output");
                    else if(scenario=="fail")check(row.state=="Failed","failure output");
                    else if(scenario=="capacity")check(row.state=="BusinessAcked"||row.state=="Failed","capacity output");
                    else check(row.state=="BusinessAcked","output ack");
                    if(scenario=="restore_accepted")check(row.attempt==2,"recovery attempt/key");
                    if(scenario=="restore_acked")check(row.attempt==1,"acked resent");
                }
                if(scenario=="hang")check(healthy_early,"healthy blocked by hang");
                if(scenario=="capacity") {
                    unsigned full=0;for(const auto& r:rows)if(r.error=="OUTPUT.CAPACITY_OR_UNAVAILABLE")++full;
                    check(full==3,"per output bound");
                }
                output->stop();stopping=true;
            }
            check(time.elapsed()<10000,"dispatcher deadline");
        }catch(const std::exception& e) {std::cerr<<e.what()<<'\n';app.exit(1);}
    });
    loop.start();return app.exec();
}
}
int main(int argc,char** argv) {QCoreApplication app(argc,argv);try{return run(app,app.arguments());}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
