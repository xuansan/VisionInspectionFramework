#include <vision/plugin_runtime/loader.hpp>
#include <vision/ipc/qt/local_connection.hpp>
#include <vision/runtime/supervisor.hpp>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <filesystem>
#define NOMINMAX
#include <windows.h>
using namespace vision;
namespace {
std::uint64_t birth(HANDLE process) {
    FILETIME c,e,k,u;
    if(!GetProcessTimes(process,&c,&e,&k,&u))throw std::runtime_error("Parent identity unavailable");
    return (static_cast<std::uint64_t>(c.dwHighDateTime)<<32)|c.dwLowDateTime;
}
struct Watchdog {
    HANDLE parent{};
    std::atomic<ULONGLONG> expiry{GetTickCount64()+5000};
    std::jthread thread;
    Watchdog() {
        bool pid_ok=false,time_ok=false;
        const auto pid=qEnvironmentVariable("VISION_PARENT_PID").toULong(&pid_ok);
        const auto created=qEnvironmentVariable("VISION_PARENT_CREATED").toULongLong(&time_ok);
        if(!pid_ok||!time_ok||!pid||!created)throw std::runtime_error("Missing parent identity");
        parent=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
        if(!parent)throw std::runtime_error("Parent unavailable");
        if(birth(parent)!=created||WaitForSingleObject(parent,0)!=WAIT_TIMEOUT) {
            CloseHandle(parent);parent=nullptr;throw std::runtime_error("Stale parent identity");
        }
        thread=std::jthread([this](std::stop_token stop) {
            while(!stop.stop_requested()) {
                const auto wait=WaitForSingleObject(parent,20);
                if(wait!=WAIT_TIMEOUT||GetTickCount64()>=expiry.load())TerminateProcess(GetCurrentProcess(),42);
            }
        });
    }
    void refresh(ULONGLONG ttl) {
        const auto now=GetTickCount64();
        if(now>=expiry.load()) {TerminateProcess(GetCurrentProcess(),42);return;}
        expiry=now+ttl;
    }
    ~Watchdog() {thread.request_stop();thread.join();CloseHandle(parent);}
};
class Executor {
public:
    Executor(QObject& target,std::filesystem::path manifest,std::string params)
        :target_(target),manifest_(std::move(manifest)),params_(std::move(params)),thread_([this]{run();}) {}
    ~Executor() {shutdown();thread_.join();}
    bool initialize(std::function<void(bool)> done) {
        return post([this,done=std::move(done)] {
            bool ok=false;
            try {
                auto shared=std::shared_ptr<plugin_runtime::Library>(plugin_runtime::Library::load(manifest_,params_));
                {std::lock_guard guard(library_mutex_);library_=shared;}
                ok=(shared->manifest().kind=="Algorithm"||shared->manifest().kind=="Camera"||shared->manifest().kind=="Communication"||shared->manifest().kind=="ResultOutput")&&shared->initialize()==plugin_sdk::Status::Ok;
            } catch(...) {}
            QMetaObject::invokeMethod(&target_,[done,ok]{done(ok);},Qt::QueuedConnection);
        });
    }
    bool execute(std::string request,std::function<void(plugin_sdk::Status,std::string)> done) {
        return post([this,request=std::move(request),done=std::move(done)] {
            std::string result;auto status=plugin_sdk::Status::Failed;
            try {
                std::shared_ptr<plugin_runtime::Library> lib;
                {std::lock_guard guard(library_mutex_);lib=library_;}
                if(lib)status=lib->execute(request,result);
            } catch(...) {}
            QMetaObject::invokeMethod(&target_,[done,status,result=std::move(result)]{done(status,result);},Qt::QueuedConnection);
        });
    }
    bool deliver(std::string request,std::function<void(bool,std::string)> done) {
        cancelled_=false;
        return post([this,request=std::move(request),done=std::move(done)] {
            bool ok=false;std::string response;
            try {
                std::shared_ptr<plugin_runtime::Library> lib;
                {std::lock_guard guard(library_mutex_);lib=library_;}
                const auto p=QJsonDocument::fromJson(QByteArray::fromStdString(request)).object();
                const auto budget=contracts::parse_u64(p["budget_ns"].toString().toStdString());
                const auto started=std::chrono::steady_clock::now();
                if(lib&&lib->output_submit(request)==plugin_sdk::Status::Ok)for(;;) {
                    if(cancelled_||static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now()-started).count())>=budget)break;
                    const auto status=lib->output_poll(response);
                    if(status==plugin_sdk::Status::Busy) {std::this_thread::sleep_for(std::chrono::milliseconds(2));continue;}
                    ok=status==plugin_sdk::Status::Ok;break;
                }
            }catch(...) {}
            QMetaObject::invokeMethod(&target_,[done,ok,response=std::move(response)]{done(ok,response);},Qt::QueuedConnection);
        });
    }
    bool device(std::string request,std::function<void(bool,std::string)> done) {
        return post([this,request=std::move(request),done=std::move(done)] {
            bool ok=false;std::string response;
            try {
                std::shared_ptr<plugin_runtime::Library> lib;
                {std::lock_guard guard(library_mutex_);lib=library_;}
                ok=lib&&lib->device_exchange(request,response)==plugin_sdk::Status::Ok;
            }catch(...) {}
            QMetaObject::invokeMethod(&target_,[done,ok,response=std::move(response)]{done(ok,response);},Qt::QueuedConnection);
        });
    }
    bool capture(std::string request,std::function<void(bool,std::string)> done) {
        cancelled_=false;
        return post([this,request=std::move(request),done=std::move(done)] {
            bool safe=false;std::string response;
            std::shared_ptr<plugin_runtime::Library> lib;
            {std::lock_guard guard(library_mutex_);lib=library_;}
            try {
                const auto p=QJsonDocument::fromJson(QByteArray::fromStdString(request)).object();
                const auto f=p["write_frame"].toObject();
                auto json=[](QJsonObject value) {return QJsonDocument(value).toJson(QJsonDocument::Compact).toStdString();};
                const auto bytes=contracts::parse_u64(p["slot_bytes"].toString().toStdString());
                const auto expected=serialization::decode_frame(json(f),bytes);
                QJsonObject settings{{"run_id",f["run_id"]},{"pool_id",f["pool_id"]},{"worker_id",f["worker_id"]},
                    {"worker_epoch",f["worker_epoch"]},{"slot_count",p["slot_count"]},{"slot_bytes",p["slot_bytes"]}};
                auto state=QString("Failed");QString error="CAMERA.FAILED";QJsonValue result=QJsonValue::Null;
                const auto budget=contracts::parse_u64(p["budget_ns"].toString().toStdString());
                const auto started=std::chrono::steady_clock::now();
                auto status=lib?lib->camera_open(json(settings)):plugin_sdk::Status::Failed;
                if(status==plugin_sdk::Status::Ok&&!cancelled_)status=lib->camera_trigger(json(f));
                if(status==plugin_sdk::Status::Ok) {
                    for(;;) {
                        if(cancelled_) {state="Cancelled";error="CAMERA.CANCELLED";break;}
                        const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-started).count();
                        if(static_cast<std::uint64_t>(elapsed)>=budget) {state="TimedOut";error="CAMERA.TIMEOUT";break;}
                        std::string descriptor;status=lib->camera_poll(descriptor);
                        if(status==plugin_sdk::Status::Busy) {std::this_thread::sleep_for(std::chrono::milliseconds(2));continue;}
                        if(status==plugin_sdk::Status::Ok) {
                            const auto received=serialization::decode_frame(descriptor,bytes);
                            if(received==expected) {state="Succeeded";error.clear();result=QJsonDocument::fromJson(QByteArray::fromStdString(descriptor)).object();}
                            else error="CAMERA.FRAME_MISMATCH";
                        } else if(status==plugin_sdk::Status::Cancelled) {state="Cancelled";error="CAMERA.CANCELLED";}
                        break;
                    }
                }
                if(cancelled_) {state="Cancelled";error="CAMERA.CANCELLED";result=QJsonValue::Null;}
                // Completion certifies that the plugin has stopped accessing the write slot.
                safe=lib&&lib->camera_close()==plugin_sdk::Status::Ok;
                response=json({{"execution_state",state},{"frame",result},
                    {"error_code",error.isEmpty()?QJsonValue(QJsonValue::Null):QJsonValue(error)}});
            } catch(...) {
                // Cannot certify absence of writes after an unexpected boundary failure.
                safe=false;
            }
            QMetaObject::invokeMethod(&target_,[done,safe,response=std::move(response)]{done(safe,response);},Qt::QueuedConnection);
        });
    }
    void request_stop() {cancelled_=true;std::lock_guard guard(library_mutex_);if(library_)library_->request_stop();}
    void shutdown() {{std::lock_guard guard(mutex_);stopping_=true;}cv_.notify_one();}
    bool finished() const {return finished_.load();}
    bool cleanup_ok() const {return cleanup_ok_.load();}
private:
    bool post(std::function<void()> command) {
        std::lock_guard guard(mutex_);
        if(stopping_||command_)return false;
        command_=std::move(command);cv_.notify_one();return true;
    }
    void run() {
        for(;;) {
            std::function<void()> command;
            {std::unique_lock lock(mutex_);cv_.wait(lock,[&]{return stopping_||bool(command_);});
                if(stopping_&&!command_)break;
                command=std::move(command_);command_={};executing_=true;}
            command();
            {std::lock_guard guard(mutex_);executing_=false;}
        }
        {std::lock_guard guard(library_mutex_);
            if(library_) {cleanup_ok_=library_->close()==plugin_sdk::Status::Ok;library_.reset();}}
        finished_=true;
    }
    QObject& target_;
    std::filesystem::path manifest_;
    std::string params_;
    std::mutex mutex_,library_mutex_;
    std::condition_variable cv_;
    std::function<void()> command_;
    bool stopping_{},executing_{};
    std::shared_ptr<plugin_runtime::Library> library_;
    std::atomic<bool> finished_{},cleanup_ok_{true};
    std::atomic<bool> cancelled_{};
    std::thread thread_;
};
int host(QCoreApplication& app) {
    const auto args=app.arguments();if(args.size()!=3)return 64;
    Watchdog watchdog;
    const auto run=qEnvironmentVariable("VISION_HOST_RUN").toStdString();
    const auto worker=qEnvironmentVariable("VISION_HOST_WORKER").toStdString();
    const auto epoch=contracts::parse_u64(qEnvironmentVariable("VISION_HOST_EPOCH").toStdString());
    auto token=qEnvironmentVariable("VISION_HOST_TOKEN").toStdString();qunsetenv("VISION_HOST_TOKEN");
    const auto endpoint=qEnvironmentVariable("VISION_HOST_ENDPOINT");
    ipc::qt::LocalConnection link(new QLocalSocket,run,worker,epoch,token,true);
    Executor executor(app,std::filesystem::path(args[1].toStdWString()),args[2].toStdString());
    QElapsedTimer clock;clock.start();runtime::ControlLease lease(run,epoch,1000000000);
    bool configured=false,initializing=false,stopping=false;
    std::optional<ipc::Message> active;std::uint64_t sequence=0;
    auto stop=[&] {
        if(stopping)return;
        stopping=true;watchdog.expiry=GetTickCount64()+1000;executor.request_stop();executor.shutdown();
    };
    link.on_closed=[&](const std::string&){stop();};
    link.on_message=[&](const ipc::Message& m) {
        if(stopping)return;
        if(m.header.type=="Configure") {
            if(initializing||configured) {link.close("DuplicateConfigure");return;}
            initializing=true;
            if(!executor.initialize([&,request=m](bool ok) {
                if(stopping)return;
                if(!ok) {
                    link.send("Fault",R"({"code":"PLUGIN.INITIALIZE_FAILED","category":"Configuration","message":"Plugin validation or initialization failed","retryability":"Never"})");
                    stop();return;
                }
                configured=true;watchdog.refresh(1000);
                if(link.reply(request,"Ready",request.payload)!=ipc::SendStatus::Accepted)stop();
            }))stop();
        } else if(m.header.type=="Heartbeat"&&configured) {
            const auto seq=contracts::parse_u64(QJsonDocument::fromJson(QByteArray::fromStdString(m.payload)).object()
                .value("progress_sequence").toString().toStdString());
            if(!lease.renew(m.header.run_id,m.header.epoch,seq,static_cast<std::uint64_t>(clock.nsecsElapsed()))) {stop();return;}
            watchdog.refresh(1000);
        } else if(m.header.type=="SubmitTask"||m.header.type=="InspectTask") {
            if(!configured||active||!lease.valid(static_cast<std::uint64_t>(clock.nsecsElapsed()))) {link.close("TaskNotReady");return;}
            active=m;
            if(link.reply(m,"TaskAccepted","{}")!=ipc::SendStatus::Accepted) {stop();return;}
            if(!executor.execute(m.payload,[&,request=m](plugin_sdk::Status status,std::string response) {
                if(stopping)return;
                if(status!=plugin_sdk::Status::Ok)response=status==plugin_sdk::Status::Cancelled?
                    R"({"execution_state":"Cancelled","quality":"Unknown","result_ref":null,"error_code":"PLUGIN.CANCELLED"})":
                    R"({"execution_state":"Failed","quality":"Unknown","result_ref":null,"error_code":"PLUGIN.EXECUTION_FAILED"})";
                auto type=std::string("TaskFinished");
                const auto object=QJsonDocument::fromJson(QByteArray::fromStdString(response)).object();
                if(status==plugin_sdk::Status::Ok&&request.header.type=="InspectTask"&&object.contains("evidence")) {
                    auto evidence=object["evidence"].toObject();
                    evidence["correlation"]=QJsonDocument::fromJson(QByteArray::fromStdString(ipc::encode_message(request))).object()["correlation"];
                    response=QJsonDocument(QJsonObject{{"check",evidence}}).toJson(QJsonDocument::Compact).toStdString();
                    type="CheckFinished";
                }
                if(link.reply(request,type,response)!=ipc::SendStatus::Accepted) {link.close("InvalidPluginResult");return;}
                active.reset();
            }))stop();
        } else if(m.header.type=="DeliveryTask") {
            if(!configured||active||!lease.valid(static_cast<std::uint64_t>(clock.nsecsElapsed()))) {link.close("OutputNotReady");return;}
            active=m;
            if(link.reply(m,"TaskAccepted","{}")!=ipc::SendStatus::Accepted) {stop();return;}
            if(!executor.deliver(m.payload,[&,request=m](bool ok,std::string response) {
                if(stopping)return;
                if(!ok||link.reply(request,"DeliveryFinished",response)!=ipc::SendStatus::Accepted) {link.close("DeliveryUnknown");return;}
                active.reset();
            }))stop();
        } else if(m.header.type=="DeviceTask") {
            if(!configured||active||!lease.valid(static_cast<std::uint64_t>(clock.nsecsElapsed()))) {link.close("DeviceNotReady");return;}
            active=m;
            if(link.reply(m,"TaskAccepted","{}")!=ipc::SendStatus::Accepted) {stop();return;}
            if(!executor.device(m.payload,[&,request=m](bool ok,std::string response) {
                if(stopping)return;
                if(!ok||link.reply(request,"DeviceFinished",response)!=ipc::SendStatus::Accepted) {link.close("DeviceExchangeFailed");return;}
                active.reset();
            }))stop();
        } else if(m.header.type=="CaptureTask") {
            if(!configured||active||!lease.valid(static_cast<std::uint64_t>(clock.nsecsElapsed()))) {link.close("CaptureNotReady");return;}
            active=m;
            if(link.reply(m,"TaskAccepted","{}")!=ipc::SendStatus::Accepted) {stop();return;}
            if(!executor.capture(m.payload,[&,request=m](bool safe,std::string response) {
                if(stopping)return;
                if(!safe) {link.close("CaptureAccessUncertain");return;}
                if(link.reply(request,"CaptureFinished",response)!=ipc::SendStatus::Accepted) {link.close("InvalidCaptureResult");return;}
                active.reset();
            }))stop();
        } else if(m.header.type=="Cancel"&&active&&active->correlation==m.correlation)executor.request_stop();
        else if(m.header.type=="Stop"||m.header.type=="Drain")stop();
    };
    QTimer timer;timer.setInterval(30);
    QObject::connect(&timer,&QTimer::timeout,&app,[&] {
        if(stopping) {if(executor.finished())app.exit(executor.cleanup_ok()?0:70);return;}
        if(configured) {
            if(sequence==UINT64_MAX) {stop();return;}
            link.send("Heartbeat","{\"progress_sequence\":\""+std::to_string(++sequence)+"\"}");
        }
    });
    timer.start();link.connect_to(endpoint);const auto result=app.exec();stop();return result;
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);try {return host(app);}catch(...) {return 65;}
}
