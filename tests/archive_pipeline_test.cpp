#include "../apps/demo_paths.hpp"
#include <vision/storage/store.hpp>
#include <vision/inference/engine.hpp>
#include <vision/serialization/json_codec.hpp>
#include <nlohmann/json.hpp>
#include <QTemporaryDir>
#include <QFile>
#include <sqlite3.h>
#include <iostream>
using namespace vision;
using J=nlohmann::json;
int main(int argc,char** argv){
    QCoreApplication app(argc,argv);const auto args=app.arguments();QTemporaryDir root,files;
    const auto scenario=args[1].toStdString();
    sqlite3* locked=nullptr;
    struct Unlock {sqlite3*& db;~Unlock(){if(db){sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr);sqlite3_close(db);}}} unlock{locked};
    try {
        if(scenario=="corrupt"){QFile f(root.filePath("results.sqlite"));if(!f.open(QIODevice::WriteOnly))return 1;f.write("corrupt");}
        if(scenario=="locked"){
            {storage::Store store(std::filesystem::path(root.path().toStdWString()));}
            const auto db=root.filePath("results.sqlite").toUtf8();
            if(sqlite3_open(db.constData(),&locked)!=SQLITE_OK||sqlite3_exec(locked,"BEGIN IMMEDIATE",nullptr,nullptr,nullptr)!=SQLITE_OK)return 1;
        }
        const bool storage_failed=scenario=="corrupt"||scenario=="locked";
        const bool fault=scenario=="hang"||scenario=="crash"||scenario=="overflow"||scenario=="malformed";
        if(fault)qputenv("VISION_TEST_ARCHIVE_FAULT",QByteArray::fromStdString(scenario));
        const bool failed=fault||scenario=="startfail"||storage_failed;
        const auto program=scenario=="startfail"?root.filePath("missing.exe"):fault?args[6]:args[5];
        application::qt::DurableDemoConfig config{root.path(),files.path(),args[2],args[3],args[4],program};
        const auto kind=scenario=="ng"?"ng":scenario=="unknown"?"algorithm-crash":"normal";
        const unsigned target=scenario=="endurance"?16U:3U;
        auto runner=std::make_unique<application::qt::DemoRunner>(demo_paths(),kind,target,config);
        if(!runner->start())return 1;
        QElapsedTimer clock;clock.start();QTimer timer;timer.setInterval(10);
        unsigned runs=0,events=0;bool stopped=false,paused=false,resumed=false,saw_reader=false;
        QObject::connect(&timer,&QTimer::timeout,&app,[&]{try{
            runner->pulse();const auto s=runner->snapshot();saw_reader|=s.station.archive_alive;
            for(const auto& event:runner->take_events()){
                ++events;
                if((failed||scenario=="unknown")&&event.result.quality!=contracts::QualityVerdict::Unknown)throw std::runtime_error("Unknown quality expected");
                if(scenario=="ng"&&event.result.quality!=contracts::QualityVerdict::NG)throw std::runtime_error("NG quality expected");
            }
            if(s.durable.persistence=="Pending"&&s.physical!="NotSent:AwaitingCommit")throw std::runtime_error("PLC before result commit");
            if(scenario=="stop"&&s.station.archive_alive&&!stopped){stopped=true;runner->stop();}
            if(scenario=="pause"&&s.station.archive_alive&&!paused){paused=true;runner->pause();}
            if(paused&&!resumed&&!s.station.active){if(!runner->resume())throw std::runtime_error("Resume");resumed=true;}
            if(clock.elapsed()>25000)throw std::runtime_error("Archive test deadline");
            if(!s.finished)return;
            if(s.station.leases||s.station.tickets||s.station.archive_alive||s.durable.agent_alive)throw std::runtime_error("Archive resource leak");
            for(const auto& w:s.workers)if(w.process_alive)throw std::runtime_error("Worker leak");
            if(scenario=="stop"){
                if(!stopped||s.physical=="BusinessAcked")throw std::runtime_error("Stop not exercised");
            }else if(storage_failed){
                if(s.error!="DURABLE.COMMIT_UNCONFIRMED"||s.durable.committed||s.physical!="NotSent:CommitUnconfirmed"||s.station.archive_error.empty())
                    throw std::runtime_error("Archive storage fail-open");
            }else{
                const unsigned expected=failed||scenario=="unknown"?1:target;
                if(!s.error.empty()||s.results!=expected||s.durable.committed!=expected||s.durable.file_output!="BusinessAcked")
                    throw std::runtime_error("Archive pipeline: "+s.error+" / "+s.durable.error+" / "+s.station.archive_error);
                if(s.physical!=(failed||scenario=="unknown"?"NotSent:UnknownQuality":"BusinessAcked"))throw std::runtime_error("Physical outcome");
                if(failed){if(s.station.archive_error.empty()||s.station.archived_frames)throw std::runtime_error("Archive fault not exposed");}
                else if(s.station.archived_frames!=2*expected)throw std::runtime_error("Archive count");
                if(scenario=="pause"&&!resumed)throw std::runtime_error("Pause not exercised");
            }
            runner.reset();++runs;
            if(scenario=="restart"&&runs==1){runner=std::make_unique<application::qt::DemoRunner>(demo_paths(),"normal",target,config);if(!runner->start())throw std::runtime_error("Restart");return;}
            if(storage_failed){std::cout<<"archive storage failure isolated\n";app.exit(0);return;}
            storage::Store store(std::filesystem::path(root.path().toStdWString()));
            if(scenario!="stop"){
                const auto rows=store.query(0,200);if(rows.size()!=events)throw std::runtime_error("Result count");
                if(!rows.empty()){
                    const auto last=serialization::decode_result(rows.back());
                    const auto references=store.frames(last.result.run_id.value(),last.result.inspection_id.value());
                    if(scenario=="trace-missing"){
                        if(references.empty())throw std::runtime_error("Missing trace fixture");
                        std::filesystem::remove(std::filesystem::path(root.path().toStdWString())/(references[0].image+".stage"));
                    }
                    QFile request(files.filePath("trace.json"));if(!request.open(QIODevice::WriteOnly))throw std::runtime_error("Trace config");
                    request.write(QByteArray::fromStdString(J{{"root",root.path().toStdString()},{"event_id",last.event_id.value()}}.dump()));request.close();
                    QProcess tool;tool.start(args[7],{"trace",request.fileName()});
                    if(!tool.waitForFinished(5000)||tool.exitStatus()!=QProcess::NormalExit||tool.exitCode()!=0)throw std::runtime_error("Trace command");
                    const auto trace=J::parse(tool.readAllStandardOutput().toStdString());
                    if(trace.at("images").size()!=references.size()||trace.at("result").at("event_id")!=last.event_id.value()||trace.at("production_ready")!=false)throw std::runtime_error("Trace identity");
                    for(unsigned i=0;i<trace.at("images").size();++i)
                        if(trace.at("images")[i].at("local_integrity")!=(scenario!="trace-missing"||i!=0))throw std::runtime_error("Trace integrity");
                    if(scenario=="trace-missing"){std::cout<<"trace missing local file correctly reported\\n";app.exit(0);return;}
                }
                for(const auto& body:rows){
                    const auto event=serialization::decode_result(body);const auto frames=store.frames(event.result.run_id.value(),event.result.inspection_id.value());
                    if(frames.size()!=(failed?0U:2U))throw std::runtime_error("Frame trace count");
                    for(unsigned i=0;i<frames.size();++i){
                        const auto& f=frames[i];const auto descriptor=serialization::decode_frame(f.descriptor,16*1024*1024);
                        const auto recipe=J::parse(f.recipe_body);
                        const auto parameters=J::parse(recipe.at("camera_parameters")[i].get<std::string>());
                        const auto sequence=std::stoull(descriptor.frame.value().substr(8));
                        std::uint64_t state=(parameters.at("seed").get<std::uint64_t>()<<32)^sequence^0x9e3779b97f4a7c15ULL;
                        std::vector<std::byte> expected(static_cast<std::size_t>(descriptor.layout.length));
                        for(auto& pixel:expected){state^=state>>12;state^=state<<25;state^=state>>27;pixel=static_cast<std::byte>((state*2685821657736338717ULL)>>56);}
                        if(store.image(f.image)!=expected||f.channel!="camera-"+std::to_string(i)||f.recipe_hash!=event.result.recipe_hash||
                            inference::sha256(std::as_bytes(std::span(f.recipe_body.data(),f.recipe_body.size())))!=f.recipe_hash||store.image_state(f.image)!="Pending")
                            throw std::runtime_error("Independent pixels/recipe/local-state verification");
                    }
                }
            }
            if(scenario!="startfail"&&!saw_reader)throw std::runtime_error("No archive process observed");
            std::cout<<"archive "<<scenario<<": pixels, trace, isolation and leases passed\n";app.exit(0);
        }catch(const std::exception& e){std::cerr<<e.what()<<'\n';if(runner){std::cerr<<runner->snapshot().last_result<<'\n';runner->stop();}app.exit(1);}});
        timer.start();return app.exec();
    }catch(const std::exception& e){std::cerr<<e.what();return 1;}
}
