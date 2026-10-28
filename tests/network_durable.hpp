#pragma once
#include <QProcess>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <nlohmann/json.hpp>
#include <iostream>
inline bool rejects_network_replay(const QString& agent,nlohmann::json config) {
    config["outputs"][0].erase("replay_policy");
    QProcess process;process.start(agent,QStringList{"--stdin"});
    if(!process.waitForStarted(2000))return false;
    process.write(QByteArray::fromStdString(config.dump()));process.closeWriteChannel();
    if(!process.waitForFinished(3000)){process.kill();process.waitForFinished(2000);return false;}
    return process.exitStatus()==QProcess::NormalExit&&process.exitCode()==1&&
        process.readAllStandardError().contains("Replay output not permitted");
}
// The test peer remains responsive while the real durable agent and worker run.
inline int run_network_durable(const QString& agent,const nlohmann::json& config) {
    QProcess process;process.start(agent,QStringList{"--stdin"});
    if(!process.waitForStarted(2000))return 1;
    process.write(QByteArray::fromStdString(config.dump()));process.closeWriteChannel();
    QElapsedTimer timer;timer.start();
    while(process.state()!=QProcess::NotRunning&&timer.elapsed()<8000){
        QCoreApplication::processEvents();process.waitForFinished(2);
    }
    if(process.state()!=QProcess::NotRunning){process.kill();process.waitForFinished(2000);return 1;}
    if(process.exitCode()!=0||process.exitStatus()!=QProcess::NormalExit){std::cerr<<process.readAllStandardError().toStdString()<<process.readAllStandardOutput().toStdString();return 1;}
    try {
        const auto report=nlohmann::json::parse(process.readAllStandardOutput().toStdString());
        const bool ok=report.at("pending")==0&&report.at("acknowledged")==1&&report.at("unconfirmed")==0;
        if(!ok)std::cerr<<report.dump()<<'\n';
        return ok?0:1;
    }catch(...){return 1;}
}
