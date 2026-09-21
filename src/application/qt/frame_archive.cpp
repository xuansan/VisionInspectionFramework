#include <vision/application/qt/frame_archive.hpp>
#include <vision/serialization/json_codec.hpp>
#include <QCryptographicHash>
#include <nlohmann/json.hpp>
namespace vision::application::qt {
using J=nlohmann::json;
FrameArchive::FrameArchive(std::shared_ptr<runtime::qt::StationGuard> guard,FrameArchiveConfig config,
    std::array<capture::qt::Session*,2> sources):guard_(std::move(guard)),config_(std::move(config)),sources_(sources) {
    if(!guard_||config_.program.isEmpty()||config_.root.isEmpty()||!sources_[0]||!sources_[1])throw std::invalid_argument("Archive configuration");
    process_.setProcessChannelMode(QProcess::MergedChannels);
    QObject::connect(&process_,&QProcess::readyRead,[this]{consume();});
    QObject::connect(&process_,&QProcess::started,[this]{
        if(stopped_||!error_.empty()||clock_.now_ns()>=due_){fail("ARCHIVE.CANCELLED");return;}
        if(!guard_->attach(process_.processId())){fail("ARCHIVE.JOB_ASSIGN");return;}
        if(process_.write(request_)!=request_.size()){fail("ARCHIVE.CONFIG_WRITE");return;}
        process_.closeWriteChannel();
    });
    QObject::connect(&process_,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),[this](int code,QProcess::ExitStatus status){finish(code,status);});
    QObject::connect(&process_,&QProcess::errorOccurred,[this](QProcess::ProcessError e){
        if(e==QProcess::FailedToStart){fail("ARCHIVE.START_FAILED");if(!alive())release();}
    });
}
FrameArchive::~FrameArchive(){
    stop();process_.disconnect();
    if(alive()){process_.kill();process_.waitForFinished(2000);}
    if(!alive())release(); // Never certify release while a reader may still access the mapping.
}
void FrameArchive::submit(std::array<contracts::FrameDescriptor,2> frames,std::string inspection,std::uint64_t budget){
    if(busy()||completed_||stopped_||!error_.empty())throw std::logic_error("Archive busy");
    frames_[0]=std::move(frames[0]);frames_[1]=std::move(frames[1]);inspection_=std::move(inspection);
    due_=runtime::deadline_after(clock_,budget);output_.clear();
    try {
        if(!budget||sequence_==UINT64_MAX)throw std::runtime_error("Archive budget");
        ++sequence_;J items=J::array();
        for(std::size_t i=0;i<2;++i){
            auto grant=sources_[i]->handoff(*frames_[i],{contracts::WorkerId("frame-archive"),sequence_},budget);
            if(!grant)throw std::runtime_error("Archive handoff");frames_[i]=std::move(*grant);
            const auto spec=sources_[i]->pool();
            items.push_back({{"frame",J::parse(serialization::encode_frame(*frames_[i],spec.slot_bytes))},
                {"slot_count",spec.slot_count},{"slot_bytes",std::to_string(spec.slot_bytes)},{"channel","camera-"+std::to_string(i)}});
        }
        const auto text=J{{"root",config_.root.toStdString()},{"run",guard_->run_id()},{"inspection",inspection_},
            {"recipe_hash",config_.recipe_hash},{"recipe_body",config_.recipe_body},{"frames",items}}.dump();
        if(text.size()>65536)throw std::runtime_error("Archive config limit");
        request_=QByteArray::fromStdString(text);process_.setProgram(config_.program);process_.start();
    }catch(...){fail("ARCHIVE.ADMISSION_FAILED");if(!alive())release();}
}
void FrameArchive::consume(){
    const auto bytes=process_.readAll();
    if(bytes.size()>8192-output_.size()){fail("ARCHIVE.OUTPUT_LIMIT");return;}output_+=bytes;
}
void FrameArchive::fail(std::string reason){
    if(error_.empty())error_=std::move(reason);
    for(std::size_t i=0;i<2;++i)if(frames_[i])sources_[i]->quarantine(*frames_[i]);
    if(alive())process_.kill();
}
void FrameArchive::release(){
    for(std::size_t i=0;i<2;++i)if(frames_[i]){sources_[i]->consumer_stopped(*frames_[i]);frames_[i].reset();}
}
void FrameArchive::finish(int code,QProcess::ExitStatus status){
    consume();
    try {
        if(stopped_||!error_.empty()||clock_.now_ns()>=due_||code!=0||status!=QProcess::NormalExit)throw std::runtime_error("Archive failed");
        const auto report=J::parse(output_.toStdString());
        J ids=J::array();
        for(unsigned i=0;i<2;++i){
            const auto key=guard_->run_id()+"\n"+inspection_+"\ncamera-"+std::to_string(i);
            ids.push_back("frame-"+QCryptographicHash::hash(QByteArray::fromStdString(key),QCryptographicHash::Sha256).toHex().toStdString());
        }
        if(report.at("run")!=guard_->run_id()||report.at("inspection")!=inspection_||report.at("images")!=ids||report.at("state")!="LocallyStaged")
            throw std::runtime_error("Archive receipt");
        for(std::size_t i=0;i<2;++i){
            const auto now=clock_.now_ns();if(now>=due_)throw std::runtime_error("Archive lease deadline");
            auto returned=sources_[i]->reclaim_archive(*frames_[i],due_-now);
            if(!returned)throw std::runtime_error("Archive lease return");frames_[i]=std::move(*returned);
        }
        completed_=std::array{*frames_[0],*frames_[1]};frames_[0].reset();frames_[1].reset();staged_+=2;
    }catch(...){fail("ARCHIVE.UNCONFIRMED");release();}
}
void FrameArchive::pulse(){if(busy()&&clock_.now_ns()>=due_){fail("ARCHIVE.TIMEOUT");if(!alive())release();}}
void FrameArchive::stop(){
    stopped_=true;
    if(completed_){for(std::size_t i=0;i<2;++i)sources_[i]->release((*completed_)[i]);completed_.reset();}
    if(busy()){fail("ARCHIVE.STOPPED");if(!alive())release();}
}
std::optional<std::array<contracts::FrameDescriptor,2>> FrameArchive::take(){auto result=std::move(completed_);completed_.reset();return result;}
}
