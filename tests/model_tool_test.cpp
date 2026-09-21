#include <QCoreApplication>
#include <QProcess>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <vision/serialization/json_codec.hpp>
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);QTemporaryDir output;
    const auto args=app.arguments();const auto scenario=args[3];
    QJsonObject request{{"config",args[2]+"/"+(scenario=="segmentation"?"segmentation":scenario=="classification"?"classification":"detection")+".json"},
        {"image",args[2]+"/black.pgm"},{"width",8},{"height",8},{"channels",1},{"score",scenario=="invalid"?2:.25},
        {"roi",QJsonArray{0,0,0,0}},{"output",output.path()}};
    if(scenario=="physical")request["plc_output"]=true;
    QProcess process;process.start(args[1],{"--request",QString::fromUtf8(QJsonDocument(request).toJson(QJsonDocument::Compact))});
    if(!process.waitForStarted(3000)||!process.waitForFinished(20000))return 1;
    if(scenario=="invalid"||scenario=="physical")return process.exitCode()!=0?0:1;
    if(process.exitCode()!=0||process.exitStatus()!=QProcess::NormalExit)return 1;
    try {
        const auto value=QJsonDocument::fromJson(process.readAllStandardOutput()).object();
        if(value["mode"]!="Replay")return 1;
        const auto check=vision::serialization::decode_check(QJsonDocument(value["check"].toObject()).toJson(QJsonDocument::Compact).toStdString());
        if(check.quality!=vision::contracts::QualityVerdict::NG)return 1;
        if(scenario=="segmentation"&&check.masks.size()!=2)return 1;
        if(scenario=="classification"&&check.classification!=std::optional<std::string>("scratch"))return 1;
        if(scenario=="replay") {
            const auto original=QJsonDocument(value).toJson(QJsonDocument::Compact);
            process.start(args[1],{"--replay",value["manifest"].toString(),value["snapshot_hash"].toString(),output.path()});
            if(!process.waitForStarted(3000)||!process.waitForFinished(20000)||process.exitCode()!=0)return 1;
            const auto next=QJsonDocument::fromJson(process.readAllStandardOutput()).object();
            if(next["mode"]!="Replay"||next["snapshot_hash"]!=value["snapshot_hash"])return 1;
            auto first=value["check"].toObject(),second=next["check"].toObject();
            if(first["correlation"].toObject()["run_id"]==second["correlation"].toObject()["run_id"])return 1;
            first.remove("correlation");second.remove("correlation");
            if(first!=second||QJsonDocument(value).toJson(QJsonDocument::Compact)!=original)return 1;
        }
        return 0;
    }catch(...){return 1;}
}
