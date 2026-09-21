#include <vision/application/qt/durable_session.hpp>
#include <nlohmann/json.hpp>
namespace vision::application::qt {
using J=nlohmann::json;
DurableSession::DurableSession(std::shared_ptr<runtime::qt::StationGuard> guard,QString host,DurableDemoConfig config)
    :guard_(std::move(guard)),host_(std::move(host)),config_(std::move(config)),
    storage_(guard_,host_,{{"storage",J{{"root",config_.root.toStdString()},{"outputs_json","[\"file\"]"}}.dump(),config_.sqlite_manifest,1}}) {
    if(config_.root.isEmpty()||config_.files.isEmpty()||config_.agent.isEmpty()||config_.file_manifest.isEmpty())
        throw std::invalid_argument("Durable Demo paths");
    state_.root=config_.root;state_.files=config_.files;state_.persistence="NotCommitted";state_.file_output="NotStarted";
    agent_.setProcessChannelMode(QProcess::MergedChannels);
    QObject::connect(&agent_,&QProcess::readyRead,[this]{consume_output();});
    QObject::connect(&agent_,&QProcess::started,[this]{
        if(!guard_->attach(agent_.processId())){
            state_.file_output="Failed";state_.error="DURABLE.JOB_ASSIGN";agent_.kill();return;
        }
        const auto config=J{{"root",config_.root.toStdString()},{"host",host_.toStdString()},
            {"outputs",J::array({{{"id","file"},{"manifest",config_.file_manifest.toStdString()},
                {"parameters",{{"root",config_.files.toStdString()},{"format","jsonl"},{"max_files",64}}}}})}}.dump();
        if(config.size()>65536||agent_.write(config.data(),static_cast<qint64>(config.size()))!=static_cast<qint64>(config.size())){
            state_.file_output="Failed";state_.error="DURABLE.CONFIG_WRITE";agent_.kill();
        }
        agent_.closeWriteChannel();
    });
    QObject::connect(&agent_,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),[this](int code,QProcess::ExitStatus status){
        consume_output();
        if(state_.file_output!="Running")return;
        try {
            if(stopped_||output_overflow_||status!=QProcess::NormalExit||code!=0)throw std::runtime_error("Agent exit");
            const auto report=J::parse(output_.toStdString());
            if(report.at("pending")!=0||report.at("unconfirmed")!=0||
                !report.at("acknowledged").is_number_unsigned()||report.at("acknowledged").get<std::uint64_t>()<state_.committed||
                report.at("failed_lane")!=false||report.at("production_ready")!=false)
                throw std::runtime_error("Agent receipt");
            state_.file_output="BusinessAcked";
        }catch(...){state_.file_output="Failed";state_.error="DURABLE.FILE_DELIVERY_FAILED";}
    });
    QObject::connect(&agent_,&QProcess::errorOccurred,[this](QProcess::ProcessError error){
        if(error==QProcess::FailedToStart){state_.file_output="Failed";state_.error="DURABLE.AGENT_START";}
    });
}
DurableSession::~DurableSession(){
    stop();agent_.disconnect();
    if(agent_.state()!=QProcess::NotRunning){agent_.kill();agent_.waitForFinished(2000);}
}
void DurableSession::consume_output(){
    const auto bytes=agent_.readAll();
    if(bytes.size()>8192-output_.size()){
        output_overflow_=true;state_.file_output="Failed";state_.error="DURABLE.AGENT_OUTPUT_LIMIT";agent_.kill();return;
    }
    output_+=bytes;
}
bool DurableSession::start(){return storage_.start();}
bool DurableSession::ready() const {return !stopped_&&!draining_&&!pending_&&!committed_&&state_.error.empty()&&storage_.ready();}
bool DurableSession::submit(const contracts::ResultEnvelope& event){
    if(!ready()||!storage_.submit(event,2000000000ULL))return false;
    pending_=event;state_.persistence="Pending";return true;
}
void DurableSession::pulse(){
    storage_.pulse();
    if(!stopped_&&!pending_&&!draining_&&state_.error.empty()){
        for(const auto& worker:storage_.workers())if(worker.phase==runtime::WorkerPhase::ManualIntervention){
            state_.persistence="Unavailable";state_.error="DURABLE.STORAGE_UNAVAILABLE";break;
        }
    }
    if(pending_&&!stopped_){
        for(const auto& delivery:storage_.deliveries()){
            if(delivery.event_id!=pending_->event_id.value())continue;
            if(delivery.state=="BusinessAcked"){
                committed_=std::move(pending_);pending_.reset();++state_.committed;state_.persistence="Durable";break;
            }
            if(delivery.state=="Failed"||delivery.state=="Unknown"||delivery.state=="Expired"){
                pending_.reset();state_.persistence=delivery.state;state_.error="DURABLE.COMMIT_UNCONFIRMED";break;
            }
        }
    }
    if(draining_&&agent_.state()!=QProcess::NotRunning&&clock_.now_ns()>=deadline_){
        state_.file_output="Unknown";state_.error="DURABLE.AGENT_DEADLINE";agent_.kill();
    }
}
std::optional<contracts::ResultEnvelope> DurableSession::take_committed(){
    auto event=std::move(committed_);committed_.reset();return event;
}
void DurableSession::drain_files(){
    if(stopped_||draining_||pending_||committed_)return;
    draining_=true;state_.file_output="Running";deadline_=runtime::deadline_after(clock_,20000000000ULL);
    agent_.setProgram(config_.agent);agent_.setArguments({"--stdin"});agent_.start();
}
bool DurableSession::files_finished() const {
    return draining_&&agent_.state()==QProcess::NotRunning&&state_.file_output!="Running";
}
void DurableSession::stop(){
    if(stopped_)return;stopped_=true;storage_.stop();
    if(pending_){state_.persistence="Unknown";state_.file_output="PendingRecovery";pending_.reset();}
    committed_.reset();
    if(state_.file_output=="Running"){state_.file_output="Unknown";agent_.kill();}
    else if(state_.file_output=="NotStarted")state_.file_output=state_.committed?"PendingRecovery":"NotSent";
}
DurableSnapshot DurableSession::snapshot() const {auto result=state_;result.agent_alive=agent_.state()!=QProcess::NotRunning;return result;}
}
