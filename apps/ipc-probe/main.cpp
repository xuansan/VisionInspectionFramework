#include <vision/ipc/qt/local_connection.hpp>
#include <QCoreApplication>
#include <QLocalServer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QUuid>
#include <QThread>
#include <iostream>
#include <memory>
#include <set>

using namespace vision::ipc;
using vision::ipc::qt::LocalConnection;
namespace {
const std::string config="{\"recipe_hash\":\"sha256:"+std::string(64,'a')+"\",\"config_revision\":\"1\"}";
const std::string task=R"({"operation":"Inspect","input_ref":"frame-1","budget_ns":"100000000"})";
const std::string finished=R"({"execution_state":"Succeeded","quality":"OK","result_ref":"result-1","error_code":null})";
Limits limits() {
    Limits l;
    l.max_payload=4096;l.normal_count=4;l.normal_bytes=16384;l.control_count=4;l.control_bytes=4096;
    l.receive_bytes=32768;l.handshake_ns=1000000000;l.partial_ns=100000000;l.request_ns=200000000;
    return l;
}
vision::ipc::qt::TransportLimits transport() {return {4096,1024,256,300000000};}
vision::contracts::Correlation correlation(const std::string& run) {
    return {vision::contracts::RunId(run),vision::contracts::WorkerId("probe"),7,
        vision::contracts::InspectionId("inspection"),vision::contracts::CheckId("check"),vision::contracts::TaskId("task"),1};
}
Message hello(const std::string& run,const std::string& token) {
    return {{1,0,"Hello","hello",run,"probe",7,1},{},hello_payload(token)};
}
bool raw_scenario(const QString& s) {
    return s=="partial-header"||s=="partial-body"||s=="malformed"||s=="oversize"||
           s=="sequence"||s=="slow-reader"||s=="silent"||s=="truncated"||s=="wrong-version";
}
int child_main(QCoreApplication& app,const QString& endpoint,const QString& scenario) {
    const auto run=qEnvironmentVariable("VISION_PROBE_RUN").toStdString();
    auto token=qEnvironmentVariable("VISION_PROBE_TOKEN").toStdString();
    // Credentials exist only in this process's environment; never print them.
    qunsetenv("VISION_PROBE_TOKEN");
    QTimer::singleShot(4000,&app,[&]{app.exit(80);});
    if(raw_scenario(scenario)) {
        QLocalSocket socket;
        socket.setReadBufferSize(1024);
        QObject::connect(&socket,&QLocalSocket::connected,&app,[&] {
            auto wire=frame(encode_message(hello(run,token)));
            if(scenario=="silent")return;
            if(scenario=="partial-header"||scenario=="truncated") {
                socket.write(wire.data(),2);
                if(scenario=="truncated")QTimer::singleShot(30,&app,[&]{socket.abort();});
            } else if(scenario=="partial-body") {
                socket.write(wire.data(),8);
            } else if(scenario=="malformed") {
                const auto bad=frame("{not valid json}");
                socket.write(bad.data(),static_cast<qint64>(bad.size()));
            } else if(scenario=="oversize") {
                socket.write(QByteArray::fromHex("ffffffff"));
            } else if(scenario=="wrong-version") {
                auto bad=encode_message(hello(run,token));
                const auto pos=bad.find("\"protocol_major\":1");
                bad.replace(pos,std::string("\"protocol_major\":1").size(),"\"protocol_major\":2");
                const auto packet=frame(bad);
                socket.write(packet.data(),static_cast<qint64>(packet.size()));
            } else {
                // Deliberate split: tests prefix fragmentation through the actual adapter.
                socket.write(wire.data(),2);
                QTimer::singleShot(10,&socket,[&,tail=QByteArray(wire.data()+2,static_cast<qsizetype>(wire.size()-2))]{
                    socket.write(tail);
                    if(scenario=="sequence") {
                        Message heartbeat{{1,0,"Heartbeat","c-1",run,"probe",7,9},{},R"({"progress_sequence":"0"})"};
                        const auto gap=frame(encode_message(heartbeat));
                        socket.write(gap.data(),static_cast<qint64>(gap.size()));
                    }
                });
            }
        });
        QObject::connect(&socket,&QLocalSocket::disconnected,&app,[&]{app.exit(0);});
        // Slow reader never consumes bytes. A finite lifetime prevents orphan probes.
        socket.connectToServer(endpoint);
        return app.exec();
    }
    if(scenario=="bad-token")token=std::string(48,'x');
    LocalConnection connection(new QLocalSocket,run,"probe",7,token,true,limits(),transport());
    connection.on_closed=[&](const std::string&){app.exit(scenario=="bad-token"||scenario=="disconnect"?0:81);};
    connection.on_message=[&](const Message& m) {
        if(m.header.type=="Configure") {
            if(connection.reply(m,"Ready",m.payload)!=SendStatus::Accepted)app.exit(82);
        } else if(m.header.type=="SubmitTask") {
            if(scenario=="request-timeout")return;
            if(scenario=="disconnect") {connection.close();return;}
            if(scenario=="late-response") {
                QTimer::singleShot(140,&connection,[&,m] {
                    connection.reply(m,"TaskFinished",finished);
                });
                return;
            }
            if(connection.reply(m,"TaskAccepted","{}")!=SendStatus::Accepted ||
               connection.reply(m,"TaskProgress",R"({"progress_sequence":"1"})")!=SendStatus::Accepted ||
               connection.reply(m,"TaskFinished",finished)!=SendStatus::Accepted)app.exit(83);
        } else if(m.header.type=="Stop")app.exit(0);
    };
    connection.connect_to(endpoint);
    return app.exec();
}
int parent_main(QCoreApplication& app,const QString& scenario) {
    QLocalServer listener;
    listener.setSocketOptions(QLocalServer::UserAccessOption);
    listener.setMaxPendingConnections(1);
    const auto endpoint="vision-ipc-"+QUuid::createUuid().toString(QUuid::WithoutBraces);
    if(!listener.listen(endpoint))return 30;
    const auto token=(QUuid::createUuid().toString(QUuid::WithoutBraces)+QUuid::createUuid().toString(QUuid::WithoutBraces)).toStdString();
    const auto run=QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    QProcess child;
    auto env=QProcessEnvironment::systemEnvironment();
    env.insert("VISION_PROBE_TOKEN",QString::fromStdString(token));env.insert("VISION_PROBE_RUN",QString::fromStdString(run));
    child.setProcessEnvironment(env);
    child.setProgram(QCoreApplication::applicationFilePath());
    child.setArguments({"--child",endpoint,scenario});
    std::unique_ptr<LocalConnection> connection;
    bool success=false,accepted=false,progress=false,timed_out=false,full=false;
    bool child_finished=false;
    int child_code=-1;
    qint64 max_socket=0;
    std::size_t max_queued=0,max_active=0;
    std::string expected_failure;
    if(scenario=="bad-token")expected_failure="HandshakeRejected";
    if(scenario=="partial-header"||scenario=="partial-body")expected_failure="PartialFrameTimeout";
    if(scenario=="silent")expected_failure="HandshakeTimeout";
    if(scenario=="malformed"||scenario=="wrong-version")expected_failure="InvalidMessage";
    if(scenario=="oversize")expected_failure="InvalidFrame";
    if(scenario=="sequence")expected_failure="SessionRejected";
    if(scenario=="truncated")expected_failure="TruncatedFrame";
    QTimer producer;
    producer.setInterval(1);
    QObject::connect(&producer,&QTimer::timeout,&app,[&] {
        if(!connection||connection->snapshot().channel.closed)return;
        for(int n=0;n<16;++n) {
            // Bounded repeated notifications fill the OS pipe when peer does not read.
            const auto result=connection->send("Fault","{\"code\":\"PROBE.DATA\",\"category\":\"Execution\",\"message\":\""+
                std::string(1800,'a')+"\",\"retryability\":\"Never\"}");
            if(result.status==SendStatus::Full) {full=true;break;}
            if(result.status!=SendStatus::Accepted)break;
        }
    });
    auto stop=[&] {
        if(connection->send("Stop",R"({"reason":"probe complete"})").status!=SendStatus::Accepted)app.exit(31);
    };
    QObject::connect(&listener,&QLocalServer::newConnection,&app,[&] {
        auto* socket=listener.nextPendingConnection();
        if(connection) {socket->abort();socket->deleteLater();return;}
        connection=std::make_unique<LocalConnection>(socket,run,"probe",7,token,false,limits(),transport());
        listener.close(); // One launch, one connection; no dangling listening endpoint.
        connection->on_ready=[&] {
            if(scenario=="slow-reader") {producer.start();return;}
            if(raw_scenario(scenario)||scenario=="bad-token")return;
            if(connection->send("Configure",config).status!=SendStatus::Accepted)app.exit(32);
        };
        connection->on_message=[&](const Message& m) {
            if(m.header.type=="Ready") {
                const auto timeout=scenario=="late-response"?50000000ULL:200000000ULL;
                if(connection->send("SubmitTask",task,correlation(run),timeout).status!=SendStatus::Accepted)app.exit(33);
            } else if(m.header.type=="TaskAccepted")accepted=true;
            else if(m.header.type=="TaskProgress")progress=true;
            else if(m.header.type=="TaskFinished") {
                if(scenario!="roundtrip"||!accepted||!progress) {app.exit(34);return;}
                success=true;stop();
            }
        };
        connection->on_request_event=[&](const RequestEvent& e) {
            if(scenario=="request-timeout"&&e.reason=="Timeout") {success=true;stop();}
            if(scenario=="late-response"&&e.reason=="Timeout")timed_out=true;
            if(scenario=="late-response"&&e.reason=="LateResponse") {success=timed_out;stop();}
            if(scenario=="disconnect"&&e.reason=="DisconnectedUnknown")success=true;
        };
        connection->on_closed=[&](const std::string& reason) {
            if(!expected_failure.empty())success=reason==expected_failure;
            if(scenario=="slow-reader") {
                producer.stop();
                success=reason=="WriteStallTimeout"&&full;
                // The raw peer might not see disconnect while its local receive buffer is full.
                if(child.state()!=QProcess::NotRunning)child.kill();
            }
        };
    });
    QTimer monitor;
    monitor.setInterval(5);
    QObject::connect(&monitor,&QTimer::timeout,&app,[&] {
        if(connection) {
            const auto s=connection->snapshot();
            max_socket=std::max(max_socket,s.socket_write_bytes);
            max_queued=std::max(max_queued,s.channel.queued_bytes);
            max_active=std::max(max_active,s.active_frame_bytes);
            if(max_socket>transport().write_buffer || max_queued>limits().normal_bytes+limits().control_bytes ||
               max_active>limits().max_payload+4)app.exit(35);
        }
        if(child_finished)app.exit(success&&(child_code==0||scenario=="slow-reader")?0:36);
    });
    monitor.start();
    QObject::connect(&child,&QProcess::finished,&app,[&](int code,QProcess::ExitStatus status) {
        child_finished=true;child_code=status==QProcess::NormalExit?code:-1;
    });
    QObject::connect(&child,&QProcess::errorOccurred,&app,[&](QProcess::ProcessError error) {
        if(error==QProcess::FailedToStart)app.exit(37);
    });
    QTimer::singleShot(6000,&app,[&]{app.exit(38);});
    child.start();
    const auto result=app.exec();
    if(child.state()!=QProcess::NotRunning) {child.kill();child.waitForFinished(1000);}
    std::cout<<"scenario="<<scenario.toStdString()<<" result="<<result
        <<" socket_high_water="<<max_socket<<" queued_high_water="<<max_queued<<" active_high_water="<<max_active<<"\n";
    return result;
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    const auto args=app.arguments();
    try {
        if(args.size()==4&&args[1]=="--child")return child_main(app,args[2],args[3]);
        const QString scenario=args.size()==2?args[1]:"roundtrip";
        const std::set<QString> allowed={"roundtrip","bad-token","partial-header","partial-body","malformed","oversize",
            "sequence","slow-reader","silent","truncated","wrong-version","request-timeout","disconnect","late-response"};
        if(args.size()>2||!allowed.contains(scenario))return 2;
        return parent_main(app,scenario);
    } catch(const std::exception& e) {std::cerr<<"Probe failed: "<<e.what()<<"\n";return 3;}
}
