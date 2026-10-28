#include <vision/application/qt/station.hpp>
#include <vision/serialization/json_codec.hpp>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QUuid>
#include <QProcess>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
using namespace vision;
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);QTemporaryDir temp;
    try {
        const auto scenario=app.arguments()[4];
        if(scenario=="restart") {
            for(int i=0;i<2;++i) {
                QProcess process;
                process.start(QCoreApplication::applicationFilePath(),{app.arguments()[1],app.arguments()[2],app.arguments()[3],"ok"});
                if(!process.waitForStarted(3000)||!process.waitForFinished(10000)||
                    process.exitStatus()!=QProcess::NormalExit||process.exitCode()!=0) {
                    std::cerr<<process.readAllStandardError().toStdString();
                    throw std::runtime_error("Fresh station restart");
                }
            }
            return 0;
        }
        const auto file=std::filesystem::path(temp.path().toStdWString())/"config.json";
        nlohmann::json params{{"mode","components"},{"threshold",255},{"min_count",0},{"max_count",0}};
        if(scenario=="ng")params["min_count"]=params["max_count"]=1;
        if(scenario=="invalid-roi")params["roi"]={{"x",1000},{"y",1000},{"width",10},{"height",10}};
        {std::ofstream f(file);f<<params;}
        application::qt::StationConfig config;config.host=app.arguments()[1];config.camera_manifest=app.arguments()[2];
        config.algorithm_manifest=app.arguments()[3];config.budget_ns=3000000000ULL;
        auto path=file.u8string();
        const auto parameters=nlohmann::json{{"config",std::string(reinterpret_cast<const char*>(path.data()),path.size())}}.dump();
        config.algorithm_parameters={parameters,parameters};
        auto guard=std::make_shared<runtime::qt::StationGuard>("opencv-"+QUuid::createUuid().toString(QUuid::WithoutBraces));
        application::qt::Station station(guard,config);if(!station.start())throw std::runtime_error("start");
        std::unique_ptr<runtime::qt::ProcessHost> faulty;
        bool fault_submitted=false,fault_observed=false;
        if(scenario=="isolation-hang"||scenario=="isolation-crash") {
            const auto binary=std::filesystem::path(app.arguments()[5].toStdWString());
            std::filesystem::copy_file(binary,std::filesystem::path(temp.path().toStdWString())/binary.filename());
            const auto manifest=std::filesystem::path(temp.path().toStdWString())/"fault.json";
            nlohmann::json j{{"schema_version",1},{"sdk_api_version",1},{"plugin_id","test.algorithm"},{"plugin_version","0.1.0"},
                {"kind","Algorithm"},{"build_id","test-v1"},{"platform","windows"},{"architecture","x64"},
                {"compiler_abi","msvc-v143-md"},{"runtime_variant","Release"},{"entry_library",binary.filename().string()},
                {"capabilities",{"synthetic"}},{"threading_model","Serialized"},{"max_instances",1},
                {"parameters_schema",{{"type","object"},{"properties",{{"mode",{{"type","string"}}}}},{"additionalProperties",false}}},
                {"dependencies",nlohmann::json::array()},{"license_metadata",{{"spdx","NOASSERTION"},{"file","LICENSE.txt"}}}};
            {std::ofstream f(manifest);f<<j;}
            runtime::SupervisorPolicy policy;policy.restart_limit=0;policy.progress_ns=200000000;policy.stage_ns=1500000000;
            const std::string host_config="{\"recipe_hash\":\"sha256:"+std::string(64,'a')+"\",\"config_revision\":\"1\"}";
            faulty=std::make_unique<runtime::qt::ProcessHost>(guard,config.host,
                QStringList{QString::fromStdWString(manifest.wstring()),scenario=="isolation-hang"?"{\"mode\":\"hang\"}":"{\"mode\":\"crash\"}"},
                "faulty-peer",host_config,policy);
            if(!faulty->start())throw std::runtime_error("Fault peer start");
        }
        bool submitted=false,stopping=false;unsigned results=0;
        QElapsedTimer time;time.start();QTimer loop;loop.setInterval(10);
        QObject::connect(&loop,&QTimer::timeout,&app,[&] {
            try {
                station.pulse();
                if(faulty) {
                    faulty->pulse_control();
                    if(faulty->ready_for_task()&&!fault_submitted) {
                        contracts::Correlation identity{contracts::RunId(faulty->run_id()),contracts::WorkerId("faulty-peer"),
                            faulty->snapshot().epoch,contracts::InspectionId("fault-inspection"),contracts::CheckId("fault-check"),
                            contracts::TaskId("fault-task"),1};
                        if(faulty->submit(identity,R"({"operation":"Inspect","input_ref":"synthetic","budget_ns":"1000000000"})",
                            1000000000ULL)!=ipc::SendStatus::Accepted)throw std::runtime_error("Fault submit");
                        fault_submitted=true;
                    }
                    if(faulty->snapshot().phase==runtime::WorkerPhase::ManualIntervention) {
                        if(!fault_submitted)throw std::runtime_error("Fault before task");
                        if(scenario=="isolation-hang"&&faulty->snapshot().reason!="ProgressTimeout")
                            throw std::runtime_error("Hang not supervised");
                        fault_observed=true;
                    }
                }
                if(auto event=station.take_result()) {
                    ++results;(void)serialization::decode_result(serialization::encode_result(*event));
                    const auto expected=scenario=="invalid-roi"?contracts::QualityVerdict::Unknown:
                        scenario=="ng"?contracts::QualityVerdict::NG:contracts::QualityVerdict::OK;
                    if(scenario!="cancel"&&(event->result.quality!=expected||event->result.checks.size()!=2)) {
                        std::cerr<<serialization::encode_result(*event)<<'\n';throw std::runtime_error("OpenCV result");
                    }
                    if(scenario!="cancel")for(const auto& check:event->result.checks) {
                        if(scenario=="invalid-roi") {if(!check.error)throw std::runtime_error("Missing execution error");}
                        else if(check.measurements.empty())throw std::runtime_error("Missing measurements");
                    }
                    station.stop();stopping=true;
                }
                if(stopping&&station.snapshot().state==application::ProductionState::Stopped) {
                    if(station.snapshot().leases||station.snapshot().tickets)throw std::runtime_error("Lease cleanup");
                    if(scenario!="cancel"&&results!=1U)throw std::runtime_error("Result count");
                    app.exit(0);
                } else if(!submitted&&station.snapshot().ready&&(!faulty||fault_observed)) {
                    if(station.trigger(1)!=application::qt::TriggerStatus::Accepted)throw std::runtime_error("Admission");
                    submitted=true;
                    if(scenario=="cancel") {station.stop();stopping=true;}
                }
                if(time.elapsed()>22000)throw std::runtime_error("Session deadline");
            }catch(const std::exception& e){std::cerr<<e.what()<<'\n';station.stop();app.exit(1);}
        });
        loop.start();return app.exec();
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
