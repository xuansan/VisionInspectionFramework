#include <vision/plugin_runtime/loader.hpp>
#include <vision/runtime/qt/process_host.hpp>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUuid>
#include <QDir>
#include <QProcessEnvironment>
#include <iostream>
#define NOMINMAX
#include <windows.h>
using namespace vision;
using namespace vision::plugin_sdk;
namespace {
void check(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
const std::string config="{\"recipe_hash\":\"sha256:"+std::string(64,'a')+"\",\"config_revision\":\"1\"}";
QJsonObject manifest(const QString& binary) {
    return {{"schema_version",1},{"sdk_api_version",1},{"plugin_id","test.algorithm"},{"plugin_version","0.1.0"},
        {"kind","Algorithm"},{"build_id","test-v1"},{"platform","windows"},{"architecture","x64"},{"compiler_abi","msvc-v143-md"},
        {"runtime_variant","Release"},{"entry_library",binary},{"capabilities",QJsonArray{"synthetic"}},
        {"threading_model","Serialized"},{"max_instances",1},
        {"parameters_schema",QJsonObject{{"type","object"},{"properties",QJsonObject{{"mode",QJsonObject{{"type","string"}}}}},{"additionalProperties",false}}},
        {"dependencies",QJsonArray{}},{"license_metadata",QJsonObject{{"spdx","NOASSERTION"},{"file","LICENSE.txt"}}}};
}
void write(const QString& path,const QJsonObject& value) {
    QFile file(path);check(file.open(QIODevice::WriteOnly),"manifest write");
    check(file.write(QJsonDocument(value).toJson())>0,"manifest bytes");
}
int loader(const QString& path,QJsonObject m,const QString& bad_api,const QString& missing) {
    const auto fs=std::filesystem::path(path.toStdWString());
    write(path,m);
    auto lib=plugin_runtime::Library::load(fs,"{}");
    check(lib->initialize()==Status::Ok,"initialize");
    check(lib->initialize()==Status::Invalid,"double initialize");
    std::string result;check(lib->execute("{}",result)==Status::Ok&&!result.empty(),"execute");
    check(lib->close()==Status::Ok&&lib->close()==Status::Ok,"close idempotence");
    for(int n=0;n<7;++n) {
        auto wrong=m;
        if(n==0)wrong["build_id"]="wrong";
        if(n==1)wrong["compiler_abi"]="wrong";
        if(n==2)wrong["entry_library"]="../escape.dll";
        if(n==3)wrong["dependencies"]=QJsonArray{"unresolved"};
        if(n==4)wrong["entry_library"]=bad_api;
        if(n==5)wrong["entry_library"]=missing;
        if(n==6)wrong["threading_model"]="Reentrant";
        write(path,wrong);bool rejected=false;
        try {auto invalid=plugin_runtime::Library::load(fs,"{}");}catch(...) {rejected=true;}
        check(rejected,"invalid plugin accepted");
    }
    write(path,m);
    qputenv("VISION_TEST_FACTORY_FAIL","1");
    bool factory_rejected=false;
    try {auto invalid=plugin_runtime::Library::load(fs,"{}");}catch(...) {factory_rejected=true;}
    qunsetenv("VISION_TEST_FACTORY_FAIL");
    check(factory_rejected,"factory failure accepted");
    bool rejected=false;try {auto invalid=plugin_runtime::Library::load(fs,R"({"unknown":1})");}catch(...) {rejected=true;}
    check(rejected,"unknown parameters accepted");
    for(const auto* mode:{"init-fail","execute-fail","oversize","destroy-fail"}) {
        auto item=plugin_runtime::Library::load(fs,std::string("{\"mode\":\"")+mode+"\"}");
        const auto initialized=item->initialize();
        if(std::string(mode)=="init-fail")check(initialized==Status::Failed,"initialize failure hidden");
        else {
            check(initialized==Status::Ok,"initialize failure");
            const auto executed=item->execute("{}",result);
            if(std::string(mode)=="oversize")check(executed==Status::Invalid,"oversize accepted");
            if(std::string(mode)=="execute-fail")check(executed==Status::Failed,"failure hidden");
        }
        check(item->close()==(std::string(mode)=="destroy-fail"?Status::Failed:Status::Ok),"destroy status lost");
        check(item->close()==(std::string(mode)=="destroy-fail"?Status::Failed:Status::Ok),"repeated close hid original destroy failure");
    }
    return 0;
}
int integration(QCoreApplication& app,const QString& scenario,const QString& host_path,const QString& manifest_path) {
    auto guard=std::make_shared<runtime::qt::StationGuard>("plugin-"+QUuid::createUuid().toString(QUuid::WithoutBraces));
    runtime::SupervisorPolicy policy;policy.restart_limit=0;policy.progress_ns=200000000;policy.stage_ns=1500000000;
    const auto mode=scenario=="isolation"?"hang":scenario;
    const auto parameters=QString("{\"mode\":\"")+mode+"\"}";
    runtime::qt::ProcessHost host(guard,host_path,{manifest_path,parameters},"plugin",config,policy);
    std::unique_ptr<runtime::qt::ProcessHost> healthy;
    if(scenario=="isolation") {
        healthy=std::make_unique<runtime::qt::ProcessHost>(guard,host_path,QStringList{manifest_path,"{}"},"healthy",config,policy);
        check(healthy->start(),"healthy start");
    }
    bool submitted=false,finished=false,stopped=false;int terminals=0;
    std::optional<contracts::Correlation> correlation;
    host.on_task_message=[&](const ipc::Message& message) {
        if(message.header.type!="TaskFinished")return;
        ++terminals;check(terminals==1,"duplicate terminal");finished=true;
        const auto payload=QJsonDocument::fromJson(QByteArray::fromStdString(message.payload)).object();
        const auto expected=scenario=="cancel"?"Cancelled":scenario=="execute-fail"||scenario=="oversize"?"Failed":"Succeeded";
        check(payload["execution_state"].toString()==expected,"wrong plugin result");
        if(QString(expected)!="Succeeded")check(payload["quality"].toString()=="Unknown","failure became OK");
    };
    check(host.start(),"plugin host start");
    QTimer loop;loop.setInterval(20);
    QObject::connect(&loop,&QTimer::timeout,&app,[&] {
        if((scenario!="lease"&&scenario!="freeze")||!submitted)host.pulse_control();
        if(healthy)healthy->pulse_control();
        const auto s=host.snapshot();
        if(host.ready_for_task()&&!submitted) {
            if(scenario=="lease"||scenario=="freeze") {
                submitted=true;if(scenario=="freeze")Sleep(1200);
                return;
            }
            correlation=contracts::Correlation{contracts::RunId(host.run_id()),contracts::WorkerId("plugin"),s.epoch,
                contracts::InspectionId("inspection"),contracts::CheckId("check"),contracts::TaskId("task"),1};
            check(host.submit(*correlation,R"({"operation":"Inspect","input_ref":"synthetic","budget_ns":"1000000000"})",1000000000)==ipc::SendStatus::Accepted,"submit");
            submitted=true;
        }
        if(scenario=="cancel"&&submitted&&!finished)host.cancel(*correlation);
        if(finished&&!stopped) {host.stop();stopped=true;}
        if(stopped&&s.phase==runtime::WorkerPhase::Stopped) {
            check(terminals==1,"missing terminal");
            if(scenario=="destroy-fail")check(host.last_exit_code()==70,"destroy error hidden");
            app.exit(0);
        }
        if(s.phase==runtime::WorkerPhase::ManualIntervention) {
            check(scenario=="crash"||scenario=="hang"||scenario=="isolation"||scenario=="init-fail"||scenario=="bad-result"||
                scenario=="destroy-fail"||scenario=="lease"||scenario=="freeze","unexpected host fault");
            if(scenario=="destroy-fail") {check(finished,"destroy before execution");app.exit(0);return;}
            if(scenario=="lease"||scenario=="freeze")check(host.last_exit_code()==42,"public host lease did not revoke");
            check(!finished,"bad plugin generated successful terminal");
            if(scenario=="hang"||scenario=="isolation")check(s.reason=="ProgressTimeout","heartbeats hid plugin hang");
            if(healthy)check(healthy->snapshot().phase==runtime::WorkerPhase::Ready,"healthy peer affected");
            app.exit(0);
        }
    });
    loop.start();QTimer::singleShot(6000,&app,[&]{app.exit(91);});
    check(app.exec()==0,"plugin integration timeout");return 0;
}
int orphan_parent(const QString& host_path,bool stale) {
    QLocalServer server;
    const auto endpoint="startup-"+QUuid::createUuid().toString(QUuid::WithoutBraces);
    check(server.listen(endpoint),"startup endpoint");
    FILETIME created,ended,kernel,user;
    check(GetProcessTimes(GetCurrentProcess(),&created,&ended,&kernel,&user)!=0,"parent creation");
    const auto birth=(static_cast<std::uint64_t>(created.dwHighDateTime)<<32)|created.dwLowDateTime;
    auto env=QProcessEnvironment::systemEnvironment();
    env.insert("VISION_PARENT_PID",QString::number(GetCurrentProcessId()));
    env.insert("VISION_PARENT_CREATED",QString::number(birth+(stale?1:0)));
    env.insert("VISION_HOST_RUN","orphan-test");env.insert("VISION_HOST_WORKER","worker");
    env.insert("VISION_HOST_EPOCH","1");env.insert("VISION_HOST_TOKEN",QString(48,'t'));
    env.insert("VISION_HOST_ENDPOINT",endpoint);
    QProcess worker;worker.setProcessEnvironment(env);
    // No station Job assignment here: exercise startup watchdog itself.
    worker.start(host_path,{"missing-manifest.json","{}"});
    check(worker.waitForStarted(3000),"orphan spawn");
    if(stale) {
        check(worker.waitForFinished(2000)&&worker.exitCode()==65,"stale parent not rejected");return 0;
    }
    check(server.waitForNewConnection(2000),"worker did not start its watchdog and connect");
    HANDLE child=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,static_cast<DWORD>(worker.processId()));
    check(child!=nullptr,"child identity");
    std::cout<<worker.processId()<<std::endl;
    // External test kills this parent before destructor; no Qt/Job cleanup may help.
    Sleep(8000);CloseHandle(child);worker.kill();worker.waitForFinished(1000);return 92;
}
int orphan(const QString& host_path,bool stale) {
    if(stale)return orphan_parent(host_path,true);
    QProcess parent;parent.start(QCoreApplication::applicationFilePath(),{"--orphan-parent",host_path});
    check(parent.waitForStarted(3000)&&parent.waitForReadyRead(3000),"orphan parent start");
    const auto pid=parent.readAllStandardOutput().trimmed().toUInt();
    HANDLE child=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION|PROCESS_TERMINATE,FALSE,pid);
    check(child!=nullptr,"orphan child handle");
    parent.kill();check(parent.waitForFinished(2000),"orphan parent kill");
    const auto waited=WaitForSingleObject(child,2000);DWORD code=0;GetExitCodeProcess(child,&code);
    if(waited!=WAIT_OBJECT_0)TerminateProcess(child,93);
    CloseHandle(child);
    check(waited==WAIT_OBJECT_0&&(code==42||code==65),"startup orphan outlived parent");
    return 0;
}
// Exercise the actual public watchdog without a competing Supervisor::Kill.
// A socket close can otherwise race TerminateProcess and overwrite exit code 42.
int watchdog(QCoreApplication& app,const QString& host_path,const QString& manifest_path,bool freeze) {
    QLocalServer server;
    const auto endpoint="watchdog-"+QUuid::createUuid().toString(QUuid::WithoutBraces);
    check(server.listen(endpoint),"watchdog endpoint");
    FILETIME created,ended,kernel,user;
    check(GetProcessTimes(GetCurrentProcess(),&created,&ended,&kernel,&user)!=0,"watchdog parent identity");
    const auto birth=(static_cast<std::uint64_t>(created.dwHighDateTime)<<32)|created.dwLowDateTime;
    auto env=QProcessEnvironment::systemEnvironment();
    env.insert("VISION_PARENT_PID",QString::number(GetCurrentProcessId()));
    env.insert("VISION_PARENT_CREATED",QString::number(birth));
    env.insert("VISION_HOST_RUN","watchdog-run");env.insert("VISION_HOST_WORKER","worker");
    env.insert("VISION_HOST_EPOCH","1");env.insert("VISION_HOST_TOKEN",QString(48,'t'));
    env.insert("VISION_HOST_ENDPOINT",endpoint);
    std::unique_ptr<ipc::qt::LocalConnection> link;
    QElapsedTimer since_ready;
    bool ready=false;
    QObject::connect(&server,&QLocalServer::newConnection,&app,[&] {
        check(!link,"duplicate watchdog connection");
        link=std::make_unique<ipc::qt::LocalConnection>(server.nextPendingConnection(),"watchdog-run","worker",1,std::string(48,'t'),false);
        link->on_ready=[&] {
            check(link->send("Configure",config).status==ipc::SendStatus::Accepted,"watchdog configure");
        };
        link->on_message=[&](const ipc::Message& m) {
            if(m.header.type!="Ready")return;
            check(!ready,"duplicate watchdog Ready");ready=true;since_ready.start();
            check(link->send("Heartbeat",R"({"progress_sequence":"1"})").status==ipc::SendStatus::Accepted,"watchdog initial lease");
            if(freeze)QTimer::singleShot(100,&app,[]{Sleep(1200);});
            // Deliberately no more control renewals, and no supervisor kill on socket close.
        };
    });
    QProcess worker;worker.setProcessEnvironment(env);
    QObject::connect(&worker,&QProcess::finished,&app,[&](int code,QProcess::ExitStatus) {
        check(ready&&since_ready.elapsed()>=900&&since_ready.elapsed()<3500,"watchdog expiry interval");
        check(code==42,"public watchdog did not exit independently");app.exit(0);
    });
    worker.start(host_path,{manifest_path,"{}"});
    QTimer timeout;
    timeout.setSingleShot(true);QObject::connect(&timeout,&QTimer::timeout,&app,[&]{app.exit(94);});timeout.start(6000);
    const auto result=app.exec();
    if(worker.state()!=QProcess::NotRunning) {worker.kill();worker.waitForFinished(1000);}
    check(result==0,"watchdog test timeout");
    return 0;
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    try {
        const auto args=app.arguments();
        if(args.size()==3&&args[1]=="--orphan-parent")return orphan_parent(args[2],false);
        check(args.size()==6,"arguments");
        if(args[1]=="orphan"||args[1]=="stale-parent")return orphan(args[2],args[1]=="stale-parent");
        QTemporaryDir dir(QDir::currentPath()+"/out/plugin-test-XXXXXX");check(dir.isValid(),"test package");
        // The destroy-failure loader case intentionally pins a DLL until process exit.
        // Keep this fixture as a test artifact instead of trying to delete a mapped DLL.
        if(args[1]=="loader") {dir.setAutoRemove(false);std::cout<<"Retained loader fixture: "<<dir.path().toStdString()<<'\n';}
        const auto good=dir.filePath("good.dll"),bad=dir.filePath("bad.dll"),missing=dir.filePath("missing.dll");
        check(QFile::copy(args[3],good)&&QFile::copy(args[4],bad)&&QFile::copy(args[5],missing),"copy test DLLs");
        const auto path=dir.filePath("manifest.json");auto m=manifest("good.dll");write(path,m);
        const auto result=args[1]=="loader"?loader(path,m,"bad.dll","missing.dll"):
            (args[1]=="lease"||args[1]=="freeze")?watchdog(app,args[2],path,args[1]=="freeze"):integration(app,args[1],args[2],path);
        std::cout<<args[1].toStdString()<<" passed\n";return result;
    }catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
