#include "../apps/demo_paths.hpp"
#include <vision/storage/store.hpp>
#include <vision/inference/engine.hpp>
#include <vision/serialization/json_codec.hpp>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <sqlite3.h>
#include <iostream>
#include <set>
using namespace vision;
int main(int argc,char** argv){
    QCoreApplication app(argc,argv);const auto args=app.arguments();
    QTemporaryDir root,files;const auto scenario=args[1];
    sqlite3* locked=nullptr;
    try{
        const auto path=std::filesystem::path(root.path().toStdWString());
        if(scenario=="corrupt"){QFile file(root.filePath("results.sqlite"));if(!file.open(QIODevice::WriteOnly))return 1;file.write("corrupt");}
        if(scenario=="locked"){
            {storage::Store store(path);}
            const auto db=(path/"results.sqlite").u8string();
            if(sqlite3_open(reinterpret_cast<const char*>(db.c_str()),&locked)!=SQLITE_OK)return 1;
            if(sqlite3_exec(locked,"BEGIN IMMEDIATE",nullptr,nullptr,nullptr)!=SQLITE_OK)return 1;
        }
        application::qt::DurableDemoConfig config{scenario=="initfail"?root.filePath("missing"):root.path(),scenario=="outputfail"?files.filePath("missing"):files.path(),
            args[2],args[3],scenario=="agentfail"?files.filePath("missing-agent.exe"):args[4]};
        const unsigned target=scenario=="endurance"?16U:3U;
        const auto kind=scenario=="ng"?"ng":scenario=="unknown"?"algorithm-crash":"normal";
        auto runner=std::make_unique<application::qt::DemoRunner>(demo_paths(),kind,target,config);
        if(!runner->start())return 1;
        QElapsedTimer elapsed;elapsed.start();QTimer timer;timer.setInterval(10);
        unsigned runs=0,ticks=0,events=0;bool paused=false,resumed=false,stopped=false;
        std::set<std::string> ids;
        QObject::connect(&timer,&QTimer::timeout,&app,[&]{
            try{
                ++ticks;runner->pulse();auto state=runner->snapshot();
                for(const auto& event:runner->take_events()){
                    ++events;ids.insert(event.event_id.value());
                    if((scenario=="ng"&&event.result.quality!=contracts::QualityVerdict::NG)||
                        (scenario=="unknown"&&event.result.quality!=contracts::QualityVerdict::Unknown))
                        throw std::runtime_error("Quality");
                }
                if(state.durable.persistence=="Pending"&&state.physical!="NotSent:AwaitingCommit")throw std::runtime_error("PLC before commit");
                if(scenario=="pause"&&!paused&&state.station.state==application::ProductionState::Running){
                    runner->pause();paused=true;
                }else if(paused&&!resumed&&!state.station.active){if(!runner->resume())throw std::runtime_error("Resume");resumed=true;}
                if((scenario=="stop"&&state.station.active||scenario=="stop-pending"&&state.durable.persistence=="Pending")&&!stopped){
                    stopped=true;runner->stop();
                }
                if(elapsed.elapsed()>25000)throw std::runtime_error("Deadline");
                if(!state.finished)return;
                if(state.station.leases||state.station.tickets||state.durable.agent_alive)throw std::runtime_error("Resource leak");
                for(const auto& worker:state.workers)if(worker.process_alive)throw std::runtime_error("Worker leak");
                if(scenario=="initfail"){
                    if(state.error!="DURABLE.STORAGE_UNAVAILABLE"||state.results||state.durable.committed||events||
                        elapsed.elapsed()>5000)throw std::runtime_error("Initialization failure not isolated before trigger");
                }else if(scenario=="corrupt"||scenario=="locked"){
                    if(state.error!="DURABLE.COMMIT_UNCONFIRMED"||state.results!=1||state.durable.committed||
                        state.physical!="NotSent:CommitUnconfirmed")throw std::runtime_error("Storage fail-open");
                }else if(scenario=="stop"||scenario=="stop-pending"){
                    if(!stopped||state.physical=="BusinessAcked"||state.durable.file_output=="BusinessAcked")throw std::runtime_error("Stop side effect");
                }else{
                    const auto expected=scenario=="unknown"?1U:target;
                    if(!state.error.empty()||state.results!=expected||state.durable.committed!=expected||
                        state.durable.persistence!="Durable")throw std::runtime_error("Persistence outcome: error="+state.error+
                            " results="+std::to_string(state.results)+" committed="+std::to_string(state.durable.committed)+
                            " persistence="+state.durable.persistence+" physical="+state.physical+" storage_error="+state.durable.error);
                    if(state.durable.file_output!=(scenario=="outputfail"||scenario=="agentfail"?"Failed":"BusinessAcked"))throw std::runtime_error("File outcome");
                    if(state.physical!=(scenario=="unknown"?"NotSent:UnknownQuality":"BusinessAcked"))throw std::runtime_error("Physical outcome");
                    if(scenario=="pause"&&!resumed)throw std::runtime_error("Pause not exercised");
                }
                runner.reset();++runs;
                if(scenario=="restart"&&runs==1){
                    runner=std::make_unique<application::qt::DemoRunner>(demo_paths(),"normal",3,config);
                    if(!runner->start())throw std::runtime_error("Restart");return;
                }
                if(locked){sqlite3_exec(locked,"ROLLBACK",nullptr,nullptr,nullptr);sqlite3_close(locked);locked=nullptr;}
                if(scenario!="corrupt"&&scenario!="stop"&&scenario!="stop-pending"&&scenario!="initfail"){
                    storage::Store store(path);const auto rows=store.query(0,200);
                    if(rows.size()!=(scenario=="locked"?0:events))throw std::runtime_error("DB rows");
                    for(const auto& body:rows)if(!ids.contains(serialization::decode_result(body).event_id.value()))throw std::runtime_error("DB identity");
                    if(scenario!="outputfail"&&scenario!="agentfail"&&scenario!="locked"){
                        const auto names=QDir(files.path()).entryList({"*.jsonl"},QDir::Files);
                        if(names.size()!=static_cast<int>(events)||!store.pending().empty())throw std::runtime_error("Files/intents");
                        for(const auto& name:names){
                            const auto bytes=inference::read_file(std::filesystem::path(files.filePath(name).toStdWString()),65536);
                            const auto record=serialization::decode_result(std::string(reinterpret_cast<const char*>(bytes.data()),bytes.size()));
                            if(!ids.contains(record.event_id.value()))throw std::runtime_error("File identity");
                        }
                    }
                }
                if(ticks<5)throw std::runtime_error("Simulation not exercised");
                std::cout<<scenario.toStdString()<<": simulated lifecycle, DB/file/PLC boundaries passed\n";app.exit(0);
            }catch(const std::exception& e){
                std::cerr<<e.what()<<'\n';
                if(runner){
                    for(const auto& worker:runner->snapshot().workers)std::cerr<<"worker epoch="<<worker.epoch<<" reason="<<worker.reason<<'\n';
                    std::cerr<<runner->snapshot().last_result<<'\n';runner->stop();
                }
                app.exit(1);
            }
        });
        timer.start();const auto result=app.exec();
        if(locked){sqlite3_exec(locked,"ROLLBACK",nullptr,nullptr,nullptr);sqlite3_close(locked);}
        return result;
    }catch(const std::exception& e){std::cerr<<e.what();if(locked)sqlite3_close(locked);return 1;}
}
