#include <vision/plugin_runtime/loader.hpp>
#include <vision/application/qt/dispatcher.hpp>
#include <vision/application/demo.hpp>
#include <vision/contracts/frame.hpp>
#include <QCoreApplication>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUuid>
#include <QElapsedTimer>
#include <QTimer>
#include <iostream>
#include <thread>
#include "network_durable.hpp"
#ifdef VISION_TEST_MYSQL
#include "../plugins/database/common/mysql.hpp"
using Client=vision::database::Mysql;
#else
#include "../plugins/database/common/postgresql.hpp"
using Client=vision::database::Postgres;
#endif
using namespace vision;
using J=nlohmann::json;
namespace {
void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
J config(unsigned port) {
    return {{"host","127.0.0.1"},{"port",port},{"database","vision_test"},{"username_env","VISION_DB_USER"},
        {"password_env","VISION_DB_PASSWORD"},{"timeout_ms",1500},{"tls","disable-loopback"},{"ca_file",""},{"archive_root",""}};
}
J event_json() {
    auto j=J::parse(serialization::encode_result(*application::run_demo(application::DemoScenario::Ng).event));
    auto& c=j["result"]["checks"][0];
    c["defects"]=J::array({{{"class_id",1},{"label","\xe5\x88\x92\xe7\x97\x95'\";DROP TABLE vision_results;--"},
        {"score",0.9},{"box_xyxy",J::array({1,1,2,2})},{"coordinate_space","original_pixels"}}});
    j["sequence"]="18446744073709551615";j["emitted_at_unix_ns"]="18446744073709551615";
    (void)serialization::decode_result(j.dump());return j;
}
J envelope(const J& event,unsigned attempt=1) {
    return {{"event",event},{"output_instance_id","database"},{"attempt",std::to_string(attempt)},{"budget_ns","5000000000"}};
}
J submit(plugin_runtime::Library& plugin,const J& request) {
    require(plugin.output_submit(request.dump())==plugin_sdk::Status::Ok,"Submit");
    require(plugin.output_submit(request.dump())==plugin_sdk::Status::Busy,"Backpressure");
    QElapsedTimer timer;timer.start();std::string body;
    for(;;) {
        QCoreApplication::processEvents();
        const auto status=plugin.output_poll(body);
        if(status==plugin_sdk::Status::Ok)break;
        require(status==plugin_sdk::Status::Busy&&timer.elapsed()<6000,"Report deadline");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const auto report=J::parse(body);
    require(report.at("event_id")==request.at("event").at("event_id")&&
        report.at("attempt")==request.at("attempt")&&report.at("output_instance_id")==request.at("output_instance_id"),"Report identity");
    return report;
}
int process(const QString& manifest,const J& params,const contracts::ResultEnvelope& event,bool success,bool concurrent) {
    auto guard=std::make_shared<runtime::qt::StationGuard>("db-"+QUuid::createUuid().toString(QUuid::WithoutBraces));
    application::qt::Dispatcher dispatcher(guard,QString::fromUtf8(VISION_DEMO_HOST),
        concurrent?std::vector<application::qt::OutputConfig>{{"database",params.dump(),manifest,1},{"database2",params.dump(),manifest,1},{"peer","{}",QString::fromUtf8(VISION_DEMO_OUTPUT),1}}:
        std::vector<application::qt::OutputConfig>{{"database",params.dump(),manifest,1},{"peer","{}",QString::fromUtf8(VISION_DEMO_OUTPUT),1}});
    require(dispatcher.start(),"Dispatcher start");
    bool submitted=false,stopping=false,finished=false,healthy_first=false;
    QElapsedTimer elapsed;elapsed.start();QTimer timer;timer.setInterval(5);
    QObject::connect(&timer,&QTimer::timeout,QCoreApplication::instance(),[&]{
        dispatcher.pulse();
        if(!submitted&&dispatcher.ready())submitted=dispatcher.submit(event,2000000000ULL);
        bool peer=false,database=false,second=!concurrent;
        for(const auto& d:dispatcher.deliveries()) {
            if(d.output_id=="peer"&&d.state=="BusinessAcked")peer=true;
            if(d.output_id=="database")database=success?d.state=="BusinessAcked":(d.state=="Failed"||d.state=="Unknown");
            if(d.output_id=="database2"&&d.state=="BusinessAcked")second=true;
        }
        if(peer&&!database)healthy_first=true;
        if(submitted&&!stopping&&dispatcher.idle()) {
            finished=peer&&database&&second&&(success||healthy_first);stopping=true;dispatcher.stop();
        }
        if(stopping) {
            bool alive=false;for(const auto& w:dispatcher.workers())alive|=w.process_alive;
            if(!alive)QCoreApplication::exit(finished?0:1);
        }
        if(elapsed.elapsed()>7500){dispatcher.stop();QCoreApplication::exit(1);}
    });
    timer.start();return QCoreApplication::exec();
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        const auto args=app.arguments();const auto scenario=args[2].toStdString();
        QTcpServer silent;require(silent.listen(QHostAddress::LocalHost,0),"Bind");
        auto parameters=config(silent.serverPort());
        if(const auto* value=std::getenv("VISION_DB_TEST_CONFIG"))parameters=J::parse(value);
        const auto manifest=std::filesystem::path(args[1].toStdWString());
        auto event=event_json();
        if(scenario=="invalid") {
            std::vector<J> bad;
            auto add=[&](const char* field,const J& value){auto c=parameters;c[field]=value;bad.push_back(c);};
            add("port",0);add("port",65536);add("timeout_ms",0);add("host","example.com");add("host","127.0.0.1;password=leak");
            add("database","x;DROP TABLE y");add("tls","require");add("ca_file","unused");add("extra",1);
            add("username_env","MISSING_VISION_DATABASE_TEST_USER");add("password_env","BAD;KEY");
            add("archive_root","Z:/definitely-not-a-vision-directory");add("port",1.25);add("tls","verify-full");
            for(const auto& c:bad) {
                bool rejected=false;
                try{auto p=plugin_runtime::Library::load(manifest,c.dump());rejected=p->initialize()!=plugin_sdk::Status::Ok;}
                catch(const std::exception&){rejected=true;}
                require(rejected,"Invalid configuration accepted");
            }
            return 0;
        }
        if(scenario=="offline"){parameters["port"]=silent.serverPort();silent.close();}
        if(scenario=="silent"||scenario=="cancel"||scenario=="isolation")parameters["port"]=silent.serverPort();
        const bool local=scenario=="offline"||scenario=="silent"||scenario=="cancel"||scenario=="isolation";
        if(local) {
            _putenv_s("VISION_DB_USER","test");_putenv_s("VISION_DB_PASSWORD","test");
            if(scenario=="isolation")return process(args[1],parameters,serialization::decode_result(event.dump()),false,false);
            auto p=plugin_runtime::Library::load(manifest,parameters.dump());require(p->initialize()==plugin_sdk::Status::Ok,"Initialize");
            if(scenario=="cancel") {
                require(p->output_submit(envelope(event).dump())==plugin_sdk::Status::Ok,"Cancel submit");
                std::this_thread::sleep_for(std::chrono::milliseconds(25));QElapsedTimer timer;timer.start();p->request_stop();
                require(p->close()==plugin_sdk::Status::Ok&&timer.elapsed()<1800,"Cancel cleanup");
                auto replacement=plugin_runtime::Library::load(manifest,parameters.dump());
                require(replacement->initialize()==plugin_sdk::Status::Ok,"Reload");replacement->request_stop();
                require(replacement->close()==plugin_sdk::Status::Ok,"Reload close");return 0;
            }
            require(submit(*p,envelope(event)).at("state")=="Failed","Failure expected");p->request_stop();require(p->close()==plugin_sdk::Status::Ok,"Close");return 0;
        }
        const auto conf=database::Config::parse(parameters);
        J admin_params=parameters;admin_params["username_env"]="VISION_DB_ADMIN";
        std::atomic<bool> stopped=false;database::Deadline admin_deadline(conf,envelope(event),stopped);
        Client admin(database::Config::parse(admin_params),admin_deadline);
        if(scenario=="tls-ca")parameters["ca_file"]=std::getenv("VISION_DB_WRONG_CA");
        if(scenario=="tls-host")parameters["host"]="localhost";
        if(scenario=="wrong-version")admin.query("UPDATE vision_schema_version SET version=2 WHERE id=1");
        if(scenario=="missing-table")admin.query("DROP TABLE vision_defects");
        if(scenario=="permission")parameters["username_env"]="VISION_DB_READONLY";
        if(scenario=="rollback") {
#ifdef VISION_TEST_MYSQL
            admin.query("CREATE TRIGGER vision_test_fail BEFORE INSERT ON vision_defects FOR EACH ROW SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='test rollback'");
#else
            admin.query("ALTER TABLE vision_defects ADD CONSTRAINT vision_test_fail CHECK (class_id<0)");
#endif
        }
        QTemporaryDir root;
        if(scenario=="images") {
            const auto result=serialization::decode_result(event.dump());
            storage::Store store(std::filesystem::path(root.path().toStdWString()));
            const std::string recipe="database-test-recipe";
            event["result"]["recipe_hash"]=inference::sha256(std::as_bytes(std::span(recipe.data(),recipe.size())));
            contracts::FrameDescriptor frame{{result.result.run_id,contracts::PoolId("pool"),
                {contracts::WorkerId("frame-archive"),1},0,1,2},contracts::FrameId("frame"),{8,4,8,0,32,contracts::PixelFormat::Mono8}};
            const std::vector<std::byte> pixels(32,std::byte{42});
            store.archive_frame(frame,32,result.result.inspection_id.value(),"camera-a",event["result"]["recipe_hash"],recipe,pixels);
            store.commit(serialization::decode_result(event.dump()),{"database"});
            parameters["archive_root"]=root.path().toStdString();
        }
        const bool failing=scenario=="wrong-version"||scenario=="missing-table"||scenario=="permission"||
            scenario=="rollback"||scenario=="lock-timeout"||scenario=="tls-ca"||scenario=="tls-host";
        if(scenario=="concurrent"||scenario=="process") {
            require(process(args[1],parameters,serialization::decode_result(event.dump()),true,scenario=="concurrent")==0,"Process output");
        }else if(scenario=="durable"||scenario=="recovery") {
            const auto e=serialization::decode_result(event.dump());
            {storage::Store store(std::filesystem::path(root.path().toStdWString()));store.commit(e,{"database"});}
            const J plan={{"root",root.path().toStdString()},{"host",VISION_DEMO_HOST},{"outputs",J::array({
                {{"id","database"},{"manifest",args[1].toStdString()},{"parameters",parameters},{"replay_policy","NonPhysicalIdempotent"}}})}};
            require(rejects_network_replay(QString::fromUtf8(VISION_DATABASE_AGENT),plan),"Replay policy");
            if(scenario=="recovery") {
                database::write_event(admin,envelope(event),conf);
                storage::Store store(std::filesystem::path(root.path().toStdWString()));
                const auto now=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
                require(store.claim("database","abandoned-worker",now-4000).has_value(),"Abandoned lease");
            }
            require(run_network_durable(QString::fromUtf8(VISION_DATABASE_AGENT),plan)==0,"Durable output");
        }else {
            if(scenario=="lock-timeout") {
                database::write_event(admin,envelope(event),conf);
                admin.query("BEGIN");
                admin.query("SELECT result_json FROM vision_results WHERE event_id="+admin.param(1)+" FOR UPDATE",{event.at("event_id")});
            }
            auto plugin=plugin_runtime::Library::load(manifest,parameters.dump());
            require(plugin->initialize()==plugin_sdk::Status::Ok,"Initialize");
            const auto first=submit(*plugin,envelope(event));
            if(first.at("state")!=(failing?"Failed":"BusinessAcked"))std::cerr<<first.dump()<<'\n';
            require(first.at("state")==(failing?"Failed":"BusinessAcked"),"Database outcome");
            if(failing)require(first.at("error_code").get<std::string>().starts_with("DB."),"Database error code");
            if(scenario=="repeat"||scenario=="conflict"||scenario=="identity-conflict") {
                if(scenario=="conflict")event["result"]["reason"]="same identity with conflicting content";
                if(scenario=="identity-conflict")event["event_id"]="other-event";
                const auto second=submit(*plugin,envelope(event,2));
                require(second.at("state")==(scenario=="repeat"?"BusinessAcked":"Failed"),"Duplicate semantics");
            }
            plugin->request_stop();require(plugin->close()==plugin_sdk::Status::Ok,"Close");
            if(scenario=="lock-timeout")admin.query("ROLLBACK");
        }
        database::Deadline verify_deadline(conf,envelope(event),stopped);
        Client verify(database::Config::parse(admin_params),verify_deadline);
        auto inspect=[&](const char* table) {
            return verify.query(std::string("SELECT COUNT(*) FROM ")+table).values.at(0).at(0);
        };
        require(inspect("vision_results")==((failing&&scenario!="lock-timeout")?"0":"1"),"Result transaction count");
        if(scenario!="missing-table")
            require(inspect("vision_defects")==((failing&&scenario!="lock-timeout")?"0":"1"),"Defect transaction count");
        require(inspect("vision_images")==(scenario=="images"?"1":"0"),"Image reference count");
        if(!failing) {
            const auto row=verify.query("SELECT sequence_no,emitted_at_unix_ns,result_json FROM vision_results").values.at(0);
            require(row[0]=="18446744073709551615"&&row[1]=="18446744073709551615","Unsigned 64-bit preservation");
            require(J::parse(row[2]).at("result").at("checks")[0].at("defects")[0].at("label")==event.at("result").at("checks")[0].at("defects")[0].at("label"),"Unicode/SQL parameter preservation");
        }
        std::cout<<"PASS "<<scenario<<"\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
