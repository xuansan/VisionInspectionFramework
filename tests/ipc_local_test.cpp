#include <vision/ipc/framing.hpp>
#include <vision/ipc/session.hpp>
#include <QCoreApplication>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTimer>
#include <QUuid>
#include <QJsonDocument>
#include <QJsonObject>
#include <iostream>
#include <memory>

// A real child process probe, not a production worker or supervisor.
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    const auto args=app.arguments();
    if(args.size()==3 && args[1]=="--child") {
        QLocalSocket socket;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout,&QTimer::timeout,&app,[&]{app.exit(10);});
        QObject::connect(&socket,&QLocalSocket::connected,&app,[&] {
            QJsonObject hello{{"type","Hello"},{"token",qEnvironmentVariable("VISION_TEST_TOKEN")}};
            const auto bytes=QJsonDocument(hello).toJson(QJsonDocument::Compact);
            const auto packet=vision::ipc::frame(std::string_view(bytes.constData(),static_cast<std::size_t>(bytes.size())));
            socket.write(packet.data(),2);
            QTimer::singleShot(10,&socket,[&,tail=QByteArray(packet.data()+2,static_cast<qsizetype>(packet.size()-2))] {
                socket.write(tail);
            });
        });
        vision::ipc::Decoder decoder(4096,4);
        QObject::connect(&socket,&QLocalSocket::readyRead,&app,[&] {
            try {
                const auto bytes=socket.readAll();
                decoder.feed(std::string_view(bytes.constData(),static_cast<std::size_t>(bytes.size())));
                if(decoder.available()) app.exit(decoder.pop()=="accepted" ? 0 : 11);
            } catch(...) {app.exit(12);}
        });
        socket.setReadBufferSize(8192);
        socket.connectToServer(args[2]);
        QObject::connect(&socket,&QLocalSocket::disconnected,&app,[&]{
            if(qEnvironmentVariable("VISION_TEST_REJECT")=="1") app.exit(0);
        });
        timeout.start(3000);
        return app.exec();
    }
    QLocalServer server;
    server.setSocketOptions(QLocalServer::UserAccessOption);
    server.setMaxPendingConnections(1);
    const auto endpoint="vision-test-"+QUuid::createUuid().toString(QUuid::WithoutBraces);
    if(!server.listen(endpoint)) return 20;
    const auto token=QUuid::createUuid().toString(QUuid::WithoutBraces);
    const bool reject=args.contains("--reject-token");
    vision::ipc::Session session("test-run","test-worker",1,token.toStdString(),100);
    QProcess child;
    auto environment=QProcessEnvironment::systemEnvironment();
    environment.insert("VISION_TEST_TOKEN",reject?QString(36,'x'):token);
    environment.insert("VISION_TEST_REJECT",reject?"1":"0");
    child.setProcessEnvironment(environment);
    child.setProgram(QCoreApplication::applicationFilePath());
    child.setArguments({"--child",endpoint});
    bool accepted=false;
    bool rejected=false;
    QObject::connect(&server,&QLocalServer::newConnection,&app,[&] {
        auto* peer=server.nextPendingConnection();
        peer->setParent(&server);
        peer->setReadBufferSize(8192);
        auto decoder=std::make_shared<vision::ipc::Decoder>(4096,4);
        QObject::connect(peer,&QLocalSocket::readyRead,&app,[&,peer,decoder] {
            try {
                const auto bytes=peer->readAll();
                decoder->feed(std::string_view(bytes.constData(),static_cast<std::size_t>(bytes.size())));
                if(decoder->available()) {
                    const auto payload=decoder->pop();
                    const auto json=QJsonDocument::fromJson(QByteArray::fromStdString(payload)).object();
                    if(json.size()!=2 || json["type"]!="Hello") {peer->abort();app.exit(21);return;}
                    const vision::ipc::Header header{1,0,"Hello","hello-1","test-run","test-worker",1,1};
                    if(session.hello(header,json["token"].toString().toStdString(),{},0)!=vision::ipc::SessionStatus::Accepted) {
                        rejected=true;peer->disconnectFromServer();return;
                    }
                    accepted=true;
                    const auto packet=vision::ipc::frame("accepted");
                    peer->write(packet.data(),static_cast<qint64>(packet.size()));
                }
            } catch(...) {peer->abort();app.exit(22);}
        });
    });
    QObject::connect(&child,&QProcess::finished,&app,[&](int code,QProcess::ExitStatus status) {
        app.exit(status==QProcess::NormalExit && code==0 && (reject ? rejected&&!accepted : accepted) ? 0 : 23);
    });
    QObject::connect(&child,&QProcess::errorOccurred,&app,[&](QProcess::ProcessError){app.exit(24);});
    QTimer::singleShot(5000,&app,[&]{app.exit(25);});
    child.start();
    const auto result=app.exec();
    if(child.state()!=QProcess::NotRunning) { child.kill(); child.waitForFinished(1000); }
    if(result==0) std::cout<<"Real child process: fragmented frame and launch-token exchange passed\n";
    return result;
}
