#include <vision/runtime/qt/process_host.hpp>
#include <vision/runtime/scheduler.hpp>
#include <QCoreApplication>
#include <QThread>
#include <QUuid>
#include <QJsonDocument>
#include <QJsonObject>
#include <iostream>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <stdexcept>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
using namespace vision::runtime;
using namespace vision::runtime::qt;
using namespace vision::ipc;
using vision::ipc::qt::LocalConnection;
namespace {
const std::string config="{\"recipe_hash\":\"sha256:"+std::string(64,'a')+"\",\"config_revision\":\"1\"}";
void require(bool condition,const char* message) {if(!condition)throw std::runtime_error(message);}
int worker(QCoreApplication& app,const QString& mode) {
    const auto run=qEnvironmentVariable("VISION_HOST_RUN").toStdString();
    const auto worker_id=qEnvironmentVariable("VISION_HOST_WORKER").toStdString();
    const auto epoch=qEnvironmentVariable("VISION_HOST_EPOCH").toULongLong();
    auto token=qEnvironmentVariable("VISION_HOST_TOKEN").toStdString();qunsetenv("VISION_HOST_TOKEN");
    LocalConnection link(new QLocalSocket,run,worker_id,epoch,token,true);
    QElapsedTimer clock;clock.start();ControlLease lease(run,epoch,500000000);
    bool configured=false,leased=false;
    auto lease_expired=[&] {
        const auto marker=qEnvironmentVariable("VISION_TEST_LEASE_MARKER");
        if(!marker.isEmpty()) {
            QFile proof(marker);
            if(!proof.open(QIODevice::WriteOnly)||proof.write("ControlLeaseExpired")!=19||!proof.flush()) {app.exit(97);return;}
            proof.close();
        }
        app.exit(42);
    };
    std::optional<Message> active;
    bool terminal_sent=false;
    std::uint64_t sequence=0;
    link.on_closed=[&](const std::string&){app.exit(0);};
    link.on_message=[&](const Message& m) {
        if(m.header.type=="Configure") {
            if(mode=="no-ready")return;
            link.reply(m,"Ready",m.payload);configured=true;
            if(mode=="crash")QTimer::singleShot(80,&app,[&]{app.exit(37);});
            if(mode=="hang")QTimer::singleShot(80,&app,[]{QThread::msleep(5000);});
        } else if(m.header.type=="SubmitTask") {
            active=m;terminal_sent=false;link.reply(m,"TaskAccepted","{}");
            if(mode=="task-timeout"||mode=="task-cancel")return;
            QTimer::singleShot(mode=="task-late"?200:5,&app,[&] {
                if(active&&!terminal_sent) {
                    terminal_sent=true;
                    link.reply(*active,"TaskFinished",R"({"execution_state":"Succeeded","quality":"OK","result_ref":"result","error_code":null})");
                }
            });
        } else if(m.header.type=="Cancel"&&mode=="task-cancel"&&active&&!terminal_sent) {
            terminal_sent=true;
            link.reply(*active,"TaskFinished",R"({"execution_state":"Cancelled","quality":"Unknown","result_ref":null,"error_code":"CANCELLED"})");
        } else if(m.header.type=="Heartbeat") {
            const auto seq=QJsonDocument::fromJson(QByteArray::fromStdString(m.payload)).object()
                .value("progress_sequence").toString().toULongLong();
            leased=lease.renew(m.header.run_id,m.header.epoch,seq,static_cast<std::uint64_t>(clock.nsecsElapsed()));
            if(!leased)lease_expired();
        } else if(m.header.type=="Stop"&&mode!="ignore-stop")app.exit(0);
    };
    QTimer beat;beat.setInterval(30);
    QObject::connect(&beat,&QTimer::timeout,&app,[&] {
        if(!configured)return;
        const auto now=static_cast<std::uint64_t>(clock.nsecsElapsed());
        if((leased&&!lease.valid(now))||(!leased&&now>1500000000)) {lease_expired();return;}
        link.send("Heartbeat","{\"progress_sequence\":\""+std::to_string(++sequence)+"\"}");
    });
    beat.start();link.connect_to(qEnvironmentVariable("VISION_HOST_ENDPOINT"));
    QTimer::singleShot(8000,&app,[&]{app.exit(90);});
    return app.exec();
}
int tasks(QCoreApplication& app,const QString& scenario) {
    auto guard=std::make_shared<StationGuard>("task-"+QUuid::createUuid().toString(QUuid::WithoutBraces));
    SupervisorPolicy policy;policy.restart_limit=0;policy.progress_ns=300000000;
    Limits limits;if(scenario=="auto-rotation")limits.session_requests=3;
    ProcessHost host(guard,QCoreApplication::applicationFilePath(),{"--worker",scenario},"worker",config,policy,limits);
    auto clock=std::make_shared<SteadyClock>();
    ResourceBudget budget({{"slots",1}},1,clock);
    TaskScheduler scheduler(vision::contracts::RunId(host.run_id()),2,budget,clock);
    std::optional<vision::contracts::Correlation> active;
    std::optional<vision::contracts::TaskState> outcome;
    int completions=0;bool late=false,held=false,stopping=false;
    host.on_task_message=[&](const Message& message) {
        if(message.header.type!="TaskFinished")return;
        require(message.correlation.has_value(),"missing correlation");
        const auto state=QJsonDocument::fromJson(QByteArray::fromStdString(message.payload)).object()
            .value("execution_state").toString()=="Cancelled"?vision::contracts::TaskState::Cancelled:vision::contracts::TaskState::Succeeded;
        const bool accepted=scheduler.finish(*message.correlation,state);
        if(scenario=="task-late"||scenario=="task-cancel") {require(!accepted,"terminal replaced");late=true;}
        else require(accepted,"valid result rejected");
    };
    require(host.start(),"task host start");
    QTimer loop;loop.setInterval(10);
    QObject::connect(&loop,&QTimer::timeout,&app,[&] {
        host.pulse_control();scheduler.tick();
        const auto snap=host.snapshot();
        if(active&&!snap.process_alive&&(snap.phase==WorkerPhase::ManualIntervention||snap.phase==WorkerPhase::Backoff||snap.phase==WorkerPhase::Stopped))
            scheduler.worker_exited(active->worker_id,active->worker_epoch);
        if(auto cancel=scheduler.take_cancel())host.cancel(*cancel);
        if(auto terminal=scheduler.take_terminal()) {
            require(!outcome,"duplicate terminal");outcome=terminal->state;++completions;
            if(scenario=="task-late"||scenario=="task-timeout"||scenario=="task-cancel") {
                require(budget.snapshot().used.at("slots")==1,"resource released before stop");
                held=true;
            }
        }
        if(outcome&&scheduler.snapshot().retained==0) {
            if(scenario=="task-late")require(late&&held&&*outcome==vision::contracts::TaskState::TimedOut,"late result changed outcome");
            if(scenario=="task-timeout")require(held&&*outcome==vision::contracts::TaskState::TimedOut,"missing timeout");
            if(scenario=="task-cancel")require(late&&held&&*outcome==vision::contracts::TaskState::Cancelled,"missing cancellation");
            require(budget.snapshot().used.at("slots")==0,"resource leak");
            if(scenario!="auto-rotation"||completions==6) {
                if(scenario=="auto-rotation")require(snap.epoch>=3&&snap.restarts==0,"session budget did not rotate");
                if(!stopping) {host.stop();stopping=true;}
                if(host.snapshot().phase==WorkerPhase::Stopped||host.snapshot().phase==WorkerPhase::ManualIntervention)app.exit(0);
                return;
            }
            outcome.reset();active.reset();
        }
        if(!active&&host.ready_for_task()) {
            const auto duration=(scenario=="task-late"||scenario=="task-timeout")?100000000ULL:1000000000ULL;
            auto admitted=scheduler.enqueue(vision::contracts::WorkerId("worker"),snap.epoch,
                vision::contracts::InspectionId("inspection"),vision::contracts::CheckId("check"),{{"slots",1}},duration);
            require(admitted.correlation.has_value(),"admission rejected");active=admitted.correlation;
            auto dispatch=scheduler.dispatch(active->worker_id,active->worker_epoch);require(dispatch.has_value(),"dispatch rejected");
            const auto payload=std::string(R"({"operation":"Inspect","input_ref":"frame","budget_ns":")")+std::to_string(dispatch->remaining_ns)+"\"}";
            // Keep IPC open longer than application deadline to exercise a genuine late TaskFinished.
            require(host.submit(*active,payload,1000000000)==SendStatus::Accepted,"transport rejected task");
            if(scenario=="task-cancel")require(scheduler.cancel(*active),"cancel rejected");
        }
    });
    loop.start();QTimer::singleShot(6000,&app,[&]{app.exit(94);});
    require(app.exec()==0,"task integration timeout");
    return 0;
}
int callbacks(QCoreApplication& app,bool timeout=false) {
    QLocalServer server;
    require(server.listen("vision-callback-"+QUuid::createUuid().toString(QUuid::WithoutBraces)),"listen");
    LocalConnection client(new QLocalSocket,"run","worker",1,std::string(48,'t'),true);
    std::unique_ptr<LocalConnection> peer;
    int delivered=0,closed=0,requests=0;
    QObject::connect(&server,&QLocalServer::newConnection,&app,[&] {
        peer=std::make_unique<LocalConnection>(server.nextPendingConnection(),"run","worker",1,std::string(48,'t'),false);
        peer->on_message=[&](const Message&) {
            if(++requests==2&&!timeout)client.close();
        };
    });
    client.on_ready=[&] {
        require(client.send("Configure",config,{},50000000).status==SendStatus::Accepted,"first configure");
        require(client.send("Configure",config,{},50000000).status==SendStatus::Accepted,"second configure");
    };
    client.on_request_event=[&](const RequestEvent&) {++delivered;throw std::runtime_error("test consumer");};
    client.on_closed=[&](const std::string&) {++closed;app.exit(0);};
    QTimer::singleShot(3000,&app,[&]{app.exit(91);});
    client.connect_to(server.serverName());
    const auto code=app.exec();
    require(code==0&&delivered==2&&closed==1,"callback failure suppressed final notifications");
    return 0;
}
int job_owner(QCoreApplication& app,const QString& station) {
    StationGuard guard(station);
    QProcess sleeper;
    sleeper.start(QCoreApplication::applicationFilePath(),{"--job-sleeper"});
    require(sleeper.waitForStarted(3000),"job sleeper start");
    require(guard.attach(sleeper.processId()),"job attach");
    std::cout<<sleeper.processId()<<std::endl;
    QTimer::singleShot(8000,&app,[&]{app.exit(95);});
    return app.exec();
}
int job_test() {
#ifdef _WIN32
    QProcess owner;
    owner.start(QCoreApplication::applicationFilePath(),{"--job-owner","job-"+QUuid::createUuid().toString(QUuid::WithoutBraces)});
    require(owner.waitForStarted(3000)&&owner.waitForReadyRead(3000),"job owner readiness");
    bool valid=false;
    const auto pid=owner.readAllStandardOutput().trimmed().toUInt(&valid);
    require(valid&&pid!=0,"invalid owned PID");
    struct Handle {
        HANDLE value;
        ~Handle() {if(value)CloseHandle(value);}
    } child{OpenProcess(SYNCHRONIZE|PROCESS_TERMINATE,FALSE,pid)};
    require(child.value!=nullptr,"child process handle");
    require(WaitForSingleObject(child.value,0)==WAIT_TIMEOUT,"child already exited");
    owner.kill();require(owner.waitForFinished(3000),"owner kill");
    const auto result=WaitForSingleObject(child.value,2000);
    if(result!=WAIT_OBJECT_0)TerminateProcess(child.value,96); // Exact owned test process, no name/PID enumeration.
    require(result==WAIT_OBJECT_0,"job failed to terminate orphan after owner death");
    return 0;
#else
    return 1;
#endif
}
int parent(QCoreApplication& app,const QString& scenario) {
    if(scenario=="callbacks"||scenario=="callback-timeouts")return callbacks(app,scenario=="callback-timeouts");
    if(scenario=="job")return job_test();
    if(scenario.startsWith("task-")||scenario=="auto-rotation")return tasks(app,scenario);
    QTemporaryDir lease_proof(QDir::currentPath()+"/lease-proof-XXXXXX");
    require(lease_proof.isValid(),"lease proof directory");
    const auto marker=lease_proof.filePath("expired.txt");
    if(scenario=="lease"||scenario=="freeze")
        qputenv("VISION_TEST_LEASE_MARKER",QDir::current().relativeFilePath(marker).toLatin1());
    const auto station="test-"+QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto guard=std::make_shared<StationGuard>(station);
    if(scenario=="station") {
        QProcess contender;contender.start(QCoreApplication::applicationFilePath(),{"--lock",station});
        require(contender.waitForFinished(3000),"contender timeout");
        require(contender.exitCode()==23,"second controller acquired station");
        guard.reset();
        QProcess successor;successor.start(QCoreApplication::applicationFilePath(),{"--lock",station});
        require(successor.waitForFinished(3000)&&successor.exitCode()==0,"station did not release");
        return 0;
    }
    SupervisorPolicy p;p.stage_ns=500000000;p.heartbeat_ns=300000000;p.progress_ns=250000000;
    p.stop_ns=100000000;p.backoff_ns=30000000;p.max_backoff_ns=100000000;p.restart_limit=1;
    if(scenario=="lease"||scenario=="freeze")p.restart_limit=0;
    const auto mode=scenario=="crash"||scenario=="hang"||scenario=="no-ready"||scenario=="ignore-stop"?scenario:QString("normal");
    const auto program=scenario=="launch-fail"?QString("Z:/nonexistent/vision-worker.exe"):QCoreApplication::applicationFilePath();
    ProcessHost host(guard,program,{"--worker",mode},"worker",config,p);
    std::unique_ptr<ProcessHost> healthy;
    if(scenario=="isolation") {
        healthy=std::make_unique<ProcessHost>(guard,QCoreApplication::applicationFilePath(),QStringList{"--worker","normal"},
            "healthy",config,p);
        require(healthy->start(),"healthy start");
    }
    require(host.start(),"host start");
    bool ready=false,acted=false,rotated=false,stopping=false;
    std::uint64_t busy_epoch=0;
    QElapsedTimer elapsed;elapsed.start();
    QTimer control;control.setInterval(20);
    QObject::connect(&control,&QTimer::timeout,&app,[&] {
        const auto s=host.snapshot();
        if(healthy)healthy->pulse_control();
        if(s.phase==WorkerPhase::Ready) {
            ready=true;
            if(scenario!="lease"||!acted)host.pulse_control();
            if(!acted) {
                acted=true;
                if(scenario=="freeze") {
                    QThread::msleep(800); // Deliberately freeze the actual coordinator event loop.
                    return;
                }
            }
            if((scenario=="progress"||scenario=="isolation")&&busy_epoch!=s.epoch) {
                require(host.set_busy(true),"busy rejected");busy_epoch=s.epoch;
            }
            if(scenario=="rotation"&&!rotated&&elapsed.elapsed()>150)rotated=host.rotate();
            if((scenario=="normal"||scenario=="ignore-stop")&&elapsed.elapsed()>180) {host.stop();stopping=true;}
            if(scenario=="rotation"&&s.epoch==2) {host.stop();stopping=true;}
        }
        if(s.phase==WorkerPhase::Stopped&&stopping) {
            require(ready,"stopped before ready");
            if(scenario=="rotation")require(rotated&&s.epoch==2&&s.restarts==0,"rotation accounting");
            app.exit(0);
        }
        if(s.phase==WorkerPhase::ManualIntervention) {
            require(scenario!="normal"&&scenario!="rotation"&&scenario!="ignore-stop","unexpected manual state");
            require(!s.process_alive,"restart before exit");
            if(scenario=="lease"||scenario=="freeze") {
                QFile proof(marker);
                require(proof.open(QIODevice::ReadOnly)&&proof.readAll()=="ControlLeaseExpired",
                    "lease did not revoke independently");
                // Socket teardown can make Supervisor kill an already-exiting child.
                // Observe the lease action itself rather than a racing final OS exit code.
            }
            else if(scenario=="launch-fail")require(s.restarts==0&&s.epoch==1,"configuration failure retried");
            else require(s.restarts==1&&s.epoch==2,"unbounded or missing recovery");
            if(scenario=="hang")require(s.reason=="HeartbeatTimeout","hang not detected");
            if(scenario=="progress"||scenario=="isolation")require(s.reason=="ProgressTimeout","heartbeat hid progress stall");
            if(healthy) {
                require(healthy->snapshot().phase==WorkerPhase::Ready&&healthy->snapshot().epoch==1,"peer affected");
                healthy->stop();
            }
            app.exit(0);
        }
    });
    control.start();
    QTimer::singleShot(7000,&app,[&]{app.exit(92);});
    const auto code=app.exec();
    require(code==0,"host scenario timeout");
    const auto events=host.take_events();
    require(!events.empty()&&events.size()<=128,"missing lifecycle records");
    if(ready) {
        bool saw_ready=false;
        for(const auto& event:events)if(event.worker.phase==WorkerPhase::Ready)saw_ready=true;
        require(saw_ready,"Ready missing from lifecycle records");
    }
    return 0;
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        const auto args=app.arguments();
        if(args.size()==2&&args[1]=="--job-sleeper") {QThread::msleep(10000);return 95;}
        if(args.size()==3&&args[1]=="--job-owner")return job_owner(app,args[2]);
        if(args.size()==3&&args[1]=="--worker")return worker(app,args[2]);
        if(args.size()==3&&args[1]=="--lock") {
            try {StationGuard lock(args[2]);return 0;}catch(...) {return 23;}
        }
        require(args.size()==2,"scenario required");
        const auto result=parent(app,args[1]);
        std::cout<<args[1].toStdString()<<" passed\n";return result;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
