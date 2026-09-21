#include <vision/storage/store.hpp>
#include <vision/application/demo.hpp>
#include <nlohmann/json.hpp>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QProcess>
#include <QFile>
#include <QDir>
#include <iostream>
using namespace vision;
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);QTemporaryDir root,files;
    try{
        const auto args=app.arguments();const auto scenario=args.size()>4?args[4]:QString("normal");
        const bool rejected=scenario!="normal";
        auto event=*application::run_demo(application::DemoScenario::Ng).event;
        {
            storage::Store store(std::filesystem::path(root.path().toStdWString()));store.commit(event,{"file"});
            if(rejected)store.observe(event.event_id.value(),"file",1,scenario=="expired"?"Expired":"Failed");
            if(scenario=="mixed")store.commit(*application::run_demo(application::DemoScenario::Ok).event,{"file"});
        }
        const auto config=nlohmann::json{{"root",root.path().toStdString()},{"host",args[2].toStdString()},
            {"outputs",nlohmann::json::array({{{"id","file"},{"manifest",args[3].toStdString()},
            {"parameters",{{"root",files.path().toStdString()},{"format","jsonl"},{"max_files",8}}}}})}}.dump();
        const auto input=root.path()+"/agent.json";QFile file(input);
        if(!file.open(QIODevice::WriteOnly)||file.write(QByteArray::fromStdString(config))<0)return 1;file.close();
        auto run=[&]{
            QProcess process;process.setProgram(args[1]);process.setArguments({input});process.start();
            if(!process.waitForStarted(2000)||!process.waitForFinished(18000)){process.kill();process.waitForFinished();throw std::runtime_error("Agent timeout");}
            if(process.exitCode()!=(rejected?1:0)){std::cerr<<process.readAllStandardError().toStdString();throw std::runtime_error("Agent exit outcome");}
            const auto report=nlohmann::json::parse(process.readAllStandardOutput().toStdString());
            if(report.at("pending")!=0||report.at("unconfirmed")!=(rejected?1:0)||
                report.at("acknowledged")!=(scenario=="normal"||scenario=="mixed"?1:0))throw std::runtime_error("Agent durable totals");
        };
        run();run();
        storage::Store store(std::filesystem::path(root.path().toStdWString()));
        if(!store.pending().empty()||store.query(0).size()!=(scenario=="mixed"?2U:1U))return 1;
        const auto output=QDir(files.path()).entryList({"*.jsonl"},QDir::Files);return output.size()==(scenario=="normal"||scenario=="mixed"?1:0)?0:1;
    }catch(const std::exception& e){std::cerr<<e.what();return 1;}
}
