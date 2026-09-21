#include <vision/runtime/qt/process_host.hpp>
#include <QCryptographicHash>
#include <QProcessEnvironment>
#include <QUuid>
#include <QJsonDocument>
#include <QJsonObject>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace vision::runtime::qt {
StationGuard::StationGuard(const QString& station) {
    if(station.isEmpty()||station!=station.trimmed())throw std::invalid_argument("Invalid station identity");
    run_=QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
#ifdef _WIN32
    const auto key=QCryptographicHash::hash(station.toUtf8(),QCryptographicHash::Sha256).toHex();
    const auto name=(QStringLiteral("Global\\VisionFrameworkStation-")+QString::fromLatin1(key)).toStdWString();
    auto handle=CreateMutexW(nullptr,TRUE,name.c_str());
    const auto error=GetLastError();
    if(!handle||error==ERROR_ALREADY_EXISTS) {
        if(handle)CloseHandle(handle);
        throw std::runtime_error("Station is owned or inaccessible");
    }
    auto job=CreateJobObjectW(nullptr,nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!job||!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&info,sizeof(info))) {
        if(job)CloseHandle(job);
        ReleaseMutex(handle);CloseHandle(handle);
        throw std::runtime_error("Cannot create process ownership job");
    }
    mutex_=handle;job_=job;
#else
    throw std::runtime_error("StationGuard is implemented only for Windows");
#endif
}
StationGuard::~StationGuard() {
#ifdef _WIN32
    if(job_)CloseHandle(job_);
    if(mutex_) {ReleaseMutex(mutex_);CloseHandle(mutex_);}
#endif
}
bool StationGuard::attach(qint64 pid) {
#ifdef _WIN32
    if(pid<=0||pid>MAXDWORD)return false;
    auto process=OpenProcess(PROCESS_SET_QUOTA|PROCESS_TERMINATE,FALSE,static_cast<DWORD>(pid));
    if(!process)return false;
    const bool ok=AssignProcessToJobObject(job_,process)!=0;
    CloseHandle(process);return ok;
#else
    (void)pid;return false;
#endif
}
ProcessHost::ProcessHost(std::shared_ptr<StationGuard> guard,QString program,QStringList args,
    std::string worker,std::string config,SupervisorPolicy policy,ipc::Limits limits,QObject* parent)
    :QObject(parent),guard_(std::move(guard)),program_(std::move(program)),arguments_(std::move(args)),
     run_(guard_?guard_->run_id():std::string{}),worker_(std::move(worker)),
     config_(std::move(config)),supervisor_(policy),limits_(limits) {
    if(!guard_||program_.isEmpty()||worker_.empty())throw std::invalid_argument("Invalid process host");
    (void)contracts::WorkerId(worker_);
    (void)ipc::encode_message({{1,0,"Configure","config",run_,worker_,1,1},{},config_});
    ipc::Channel validate_limits(run_,worker_,1,std::string(48,'v'),true,limits_,0);
    clock_.start();timer_.setInterval(10);
    QObject::connect(&timer_,&QTimer::timeout,this,[this]{check();});
    server_.setSocketOptions(QLocalServer::UserAccessOption);
    QObject::connect(&server_,&QLocalServer::newConnection,this,[this] {
        while(server_.hasPendingConnections()) {
            auto socket=server_.nextPendingConnection();
            if(connection_||!process_||process_->state()!=QProcess::Running) {
                socket->abort();socket->deleteLater();continue;
            }
            const auto epoch=supervisor_.snapshot().epoch;
            connection_=new ipc::qt::LocalConnection(socket,run_,worker_,epoch,token_,false,limits_,{},this);
            connection_->on_ready=[this,epoch] {
                supervisor_.authenticated(epoch,now());
                observe();
                if(supervisor_.snapshot().phase!=WorkerPhase::Initializing)return;
                if(connection_->send("Configure",config_).status!=ipc::SendStatus::Accepted)
                    supervisor_.fault(epoch,"ConfigureRejected",now(),false);
            };
            connection_->on_message=[this,epoch](const ipc::Message& m) {
                const auto state=supervisor_.snapshot();
                if(epoch!=state.epoch||!state.process_alive||
                   (state.phase!=WorkerPhase::Initializing&&state.phase!=WorkerPhase::Ready))return;
                if(m.header.type=="Ready")supervisor_.initialized(epoch,now());
                else if(m.header.type=="Fault") {
                    const auto fault=QJsonDocument::fromJson(QByteArray::fromStdString(m.payload)).object();
                    supervisor_.fault(epoch,fault.value("code").toString().toStdString(),now(),
                        fault.value("retryability").toString()!="Never");
                }
                else if(m.header.type=="Heartbeat") {
                    const auto object=QJsonDocument::fromJson(QByteArray::fromStdString(m.payload)).object();
                    supervisor_.heartbeat(epoch,object.value("progress_sequence").toString().toULongLong(),now());
                } else if(m.header.type=="TaskProgress") {
                    const auto object=QJsonDocument::fromJson(QByteArray::fromStdString(m.payload)).object();
                    supervisor_.progress(epoch,object.value("progress_sequence").toString().toULongLong(),now());
                }
                if(m.header.type=="TaskFinished"||m.header.type=="CaptureFinished"||m.header.type=="DeviceFinished"||m.header.type=="DeliveryFinished"||m.header.type=="CheckFinished") {
                    active_task_.reset();supervisor_.set_busy(epoch,false,now());
                }
                if(m.header.type=="TaskAccepted"||m.header.type=="TaskProgress"||m.header.type=="TaskFinished"||m.header.type=="CaptureFinished"||m.header.type=="DeviceFinished"||m.header.type=="DeliveryFinished"||m.header.type=="CheckFinished")
                    if(on_task_message)on_task_message(m);
                observe();
            };
            connection_->on_request_event=[this,epoch](const ipc::RequestEvent& e) {
                if(e.reason!="LateResponse")supervisor_.fault(epoch,e.reason,now());
                observe();
            };
            connection_->on_closed=[this,epoch](const std::string& reason) {
                if(!destroying_)supervisor_.fault(epoch,reason,now(),
                    reason!="HandshakeRejected"&&reason!="ResponseMismatch"&&reason!="InvalidMessage");
                observe();
            };
        }
    });
}
ProcessHost::~ProcessHost() {
    shutdown_and_wait();
}
bool ProcessHost::shutdown_and_wait() {
    destroying_=true;timer_.stop();server_.close();
    if(connection_) {connection_->on_closed={};connection_->on_request_event={};connection_->close();}
    if(process_) {
        QObject::disconnect(process_,nullptr,this,nullptr);
        if(process_->state()!=QProcess::NotRunning) {process_->kill();process_->waitForFinished(1000);}
    }
    return !process_||process_->state()==QProcess::NotRunning;
}
std::uint64_t ProcessHost::now() const {return static_cast<std::uint64_t>(clock_.nsecsElapsed());}
void ProcessHost::observe() {
    const auto s=supervisor_.snapshot();
    if(s.phase==last_observed_.phase&&s.epoch==last_observed_.epoch&&s.restarts==last_observed_.restarts&&
       s.reason==last_observed_.reason&&s.process_alive==last_observed_.process_alive&&s.busy==last_observed_.busy)return;
    if(events_.size()==128) {events_.pop_front();if(dropped_events_!=UINT64_MAX)++dropped_events_;}
    events_.push_back({now(),s});last_observed_=s;
}
std::vector<HostEvent> ProcessHost::take_events() {
    std::vector<HostEvent> result(events_.begin(),events_.end());events_.clear();return result;
}
bool ProcessHost::start() {const bool ok=supervisor_.start(now());if(ok) {timer_.start();check();}return ok;}
void ProcessHost::stop() {supervisor_.stop(now());check();}
bool ProcessHost::set_busy(bool busy) {
    if(active_task_&&!busy)return false;
    const auto result=supervisor_.set_busy(supervisor_.snapshot().epoch,busy,now());observe();return result;
}
bool ProcessHost::ready_for_task() const {
    const auto state=supervisor_.snapshot();
    return connection_&&!rotation_requested_&&state.phase==WorkerPhase::Ready&&!state.busy&&
        !connection_->snapshot().channel.rotation_required;
}
ipc::SendStatus ProcessHost::submit(const contracts::Correlation& identity,std::string payload,std::uint64_t timeout) {
    return submit_request("SubmitTask",identity,std::move(payload),timeout);
}
ipc::SendStatus ProcessHost::inspect(const contracts::Correlation& identity,std::string payload,std::uint64_t timeout) {
    return submit_request("InspectTask",identity,std::move(payload),timeout);
}
ipc::SendStatus ProcessHost::deliver(const contracts::Correlation& identity,std::string payload,std::uint64_t timeout) {
    return submit_request("DeliveryTask",identity,std::move(payload),timeout);
}
ipc::SendStatus ProcessHost::device(const contracts::Correlation& identity,std::string payload,std::uint64_t timeout) {
    return submit_request("DeviceTask",identity,std::move(payload),timeout);
}
ipc::SendStatus ProcessHost::capture(const contracts::Correlation& identity,std::string payload,std::uint64_t timeout) {
    return submit_request("CaptureTask",identity,std::move(payload),timeout);
}
ipc::SendStatus ProcessHost::submit_request(std::string type,const contracts::Correlation& identity,std::string payload,std::uint64_t timeout) {
    const auto state=supervisor_.snapshot();
    if(!connection_||rotation_requested_||state.phase!=WorkerPhase::Ready||state.busy)return ipc::SendStatus::NotReady;
    if(identity.run_id.value()!=run_||identity.worker_id.value()!=worker_||identity.worker_epoch!=state.epoch)
        return ipc::SendStatus::Invalid;
    if(connection_->snapshot().channel.rotation_required) {rotation_requested_=true;return ipc::SendStatus::Full;}
    const auto sent=connection_->send(std::move(type),std::move(payload),identity,timeout);
    if(sent.status==ipc::SendStatus::Accepted) {
        active_task_=identity;supervisor_.set_busy(state.epoch,true,now());
        observe();
    }
    return sent.status;
}
ipc::SendStatus ProcessHost::cancel(const contracts::Correlation& identity) {
    if(!connection_||!active_task_||*active_task_!=identity)return ipc::SendStatus::Invalid;
    return connection_->send("Cancel",R"({"reason":"SchedulerCancellation"})",identity).status;
}
bool ProcessHost::rotate() {
    if(!connection_||supervisor_.snapshot().phase!=WorkerPhase::Ready||supervisor_.snapshot().busy)return false;
    rotation_requested_=true;check();return true;
}
void ProcessHost::pulse_control() {
    if(!connection_||rotation_requested_||supervisor_.snapshot().phase!=WorkerPhase::Ready)return;
    if(control_sequence_==UINT64_MAX) {supervisor_.fault(supervisor_.snapshot().epoch,"ControlSequenceExhausted",now());return;}
    const auto payload="{\"progress_sequence\":\""+std::to_string(++control_sequence_)+"\"}";
    if(connection_->send("Heartbeat",payload).status!=ipc::SendStatus::Accepted)
        supervisor_.fault(supervisor_.snapshot().epoch,"ControlHeartbeatRejected",now());
}
void ProcessHost::launch() {
    if(connection_) {connection_->on_closed={};connection_->on_request_event={};delete connection_;connection_=nullptr;}
    if(process_) {delete process_;process_=nullptr;}
    server_.close();control_sequence_=0;rotation_requested_=false;active_task_.reset();
    token_=(QUuid::createUuid().toString(QUuid::WithoutBraces)+QUuid::createUuid().toString(QUuid::WithoutBraces)).toStdString();
    const auto endpoint=QStringLiteral("vision-host-")+QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto epoch=supervisor_.snapshot().epoch;
    if(!server_.listen(endpoint)) {supervisor_.fault(epoch,"ListenFailed",now(),false);supervisor_.exited(epoch,now());return;}
    process_=new QProcess(this);
    auto env=QProcessEnvironment::systemEnvironment();
    env.insert("VISION_HOST_ENDPOINT",endpoint);env.insert("VISION_HOST_TOKEN",QString::fromStdString(token_));
    env.insert("VISION_HOST_RUN",QString::fromStdString(run_));
    env.insert("VISION_HOST_WORKER",QString::fromStdString(worker_));
    env.insert("VISION_HOST_EPOCH",QString::number(epoch));
#ifdef _WIN32
    FILETIME created,ended,kernel,user;
    if(!GetProcessTimes(GetCurrentProcess(),&created,&ended,&kernel,&user)) {
        supervisor_.fault(epoch,"ParentIdentityUnavailable",now(),false);supervisor_.exited(epoch,now());return;
    }
    const auto birth=(static_cast<std::uint64_t>(created.dwHighDateTime)<<32)|created.dwLowDateTime;
    env.insert("VISION_PARENT_PID",QString::number(GetCurrentProcessId()));
    env.insert("VISION_PARENT_CREATED",QString::number(birth));
#endif
    process_->setProcessEnvironment(env);
    // Never accumulate arbitrary SDK stdout/stderr in coordinator memory.
    process_->setStandardOutputFile(QProcess::nullDevice());process_->setStandardErrorFile(QProcess::nullDevice());
    QObject::connect(process_,&QProcess::started,this,[this,epoch] {
        supervisor_.spawned(epoch,now());
        if(!guard_->attach(process_->processId()))supervisor_.fault(epoch,"JobAttachFailed",now(),false);
        observe();
        check();
    });
    QObject::connect(process_,&QProcess::finished,this,[this,epoch](int code,QProcess::ExitStatus status) {
        last_exit_code_=code;
        supervisor_.exited(epoch,now(),status==QProcess::NormalExit&&code==0);observe();server_.close();
    });
    QObject::connect(process_,&QProcess::errorOccurred,this,[this,epoch](QProcess::ProcessError error) {
        if(error==QProcess::FailedToStart) {supervisor_.fault(epoch,"LaunchFailed",now(),false);supervisor_.exited(epoch,now());observe();}
    });
    process_->start(program_,arguments_);
}
void ProcessHost::check() {
    if(checking_||destroying_)return;
    checking_=true;
    observe();
    if(connection_&&supervisor_.snapshot().phase==WorkerPhase::Ready) {
        const auto snap=connection_->snapshot();
        if(snap.channel.rotation_required&&!supervisor_.snapshot().busy)rotation_requested_=true;
        if(rotation_requested_&&!snap.channel.pending&&!snap.channel.queued_count&&
           !snap.active_frame_bytes&&!snap.socket_write_bytes)supervisor_.rotate(now());
    }
    if(auto action=supervisor_.poll(now())) {
        if(*action==ProcessAction::Launch)launch();
        else if(*action==ProcessAction::Stop) {
            if(connection_)connection_->send("Stop",R"({"reason":"SupervisorStop"})");
        } else if(process_&&process_->state()!=QProcess::NotRunning)process_->kill();
        else supervisor_.exited(supervisor_.snapshot().epoch,now());
    }
    observe();
    checking_=false;
}
} // namespace vision::runtime::qt
