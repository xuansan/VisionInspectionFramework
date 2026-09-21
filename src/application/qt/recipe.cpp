#include <vision/application/qt/recipe.hpp>
#include <QFile>
#include <QSaveFile>
#include <QCryptographicHash>
#include <nlohmann/json.hpp>
namespace vision::application::qt {
using J=nlohmann::json;
RecipeSnapshot parse_recipe(std::string bytes,const RecipePaths& paths) {
    if(bytes.empty()||bytes.size()>16384)throw std::invalid_argument("Recipe document limit");
    std::vector<std::set<std::string>> keys;
    const auto p=J::parse(bytes,[&](int depth,J::parse_event_t event,J& value) {
        if(depth>8)throw std::invalid_argument("Recipe depth");
        if(event==J::parse_event_t::object_start)keys.emplace_back();
        if(event==J::parse_event_t::key&&!keys.back().insert(value.get<std::string>()).second)throw std::invalid_argument("Duplicate recipe key");
        if(event==J::parse_event_t::object_end)keys.pop_back();return true;
    });
    if(!p.is_object()||p.size()!=8||p.at("schema_version")!=1||p.at("mode")!="Demo"||
       !p.at("cameras").is_array()||p.at("cameras").size()!=2)throw std::invalid_argument("Demo recipe fields");
    (void)contracts::TaskId(p.at("recipe_id").get<std::string>());
    (void)contracts::TaskId(p.at("version").get<std::string>());
    auto integer=[](const J& value,unsigned maximum) {
        if(!value.is_number_integer()||value.get<std::int64_t>()<0||value.get<std::uint64_t>()>maximum)
            throw std::invalid_argument("Demo recipe integer");
        return value.get<unsigned>();
    };
    const auto width=integer(p.at("width"),4096),height=integer(p.at("height"),4096),duration=integer(p.at("deadline_ms"),10000);
    if(!width||!height||!duration||static_cast<std::uint64_t>(width)*height>16*1024*1024)throw std::invalid_argument("Demo recipe limits");
    StationConfig c;c.host=paths.host;c.camera_manifest=paths.camera;c.algorithm_manifest=paths.algorithm;
    c.layout={width,height,width,0,static_cast<std::uint64_t>(width)*height,contracts::PixelFormat::Mono8};
    c.budget_ns=static_cast<std::uint64_t>(duration)*1000000;
    const auto canonical=p.dump();
    c.recipe_hash="sha256:"+QCryptographicHash::hash(QByteArray::fromStdString(canonical),QCryptographicHash::Sha256).toHex().toStdString();
    for(std::size_t i=0;i<2;++i) {
        const auto& camera=p["cameras"][i];
        if(!camera.is_object()||camera.size()!=3)throw std::invalid_argument("Camera recipe fields");
        const auto seed=integer(camera.at("seed"),UINT32_MAX),minimum=integer(camera.at("minimum"),255),maximum=integer(camera.at("maximum"),255);
        if(minimum>maximum)throw std::invalid_argument("Brightness range");
        c.camera_parameters[i]=J{{"width",width},{"height",height},{"seed",seed}}.dump();
        c.algorithm_parameters[i]=J{{"minimum",minimum},{"maximum",maximum}}.dump();
    }
    return {canonical,c.recipe_hash,std::move(c)};
}
RecipeSnapshot load_recipe(const QString& file_name,const RecipePaths& paths) {
    QFile file(file_name);
    if(!file.open(QIODevice::ReadOnly)||file.size()>16384)throw std::invalid_argument("Cannot read bounded recipe");
    return parse_recipe(file.readAll().toStdString(),paths);
}
bool save_recipe(const QString& file_name,const RecipeSnapshot& snapshot) {
    try {
        auto valid=parse_recipe(snapshot.canonical,{snapshot.config.host,snapshot.config.camera_manifest,snapshot.config.algorithm_manifest});
        if(valid.hash!=snapshot.hash)return false;
        QSaveFile file(file_name);file.setDirectWriteFallback(false);
        if(!file.open(QIODevice::WriteOnly))return false;
        const auto bytes=QByteArray::fromStdString(valid.canonical);
        if(file.write(bytes)!=bytes.size()) {file.cancelWriting();return false;}
        return file.commit();
    }catch(...) {return false;}
}
RecipeRuntime::RecipeRuntime(QString station_key,QString file,RecipePaths paths)
    :station_key_(std::move(station_key)),file_(std::move(file)),paths_(std::move(paths)) {}
RecipeRuntime::~RecipeRuntime() {stop();}
bool RecipeRuntime::launch(const RecipeSnapshot& snapshot) {
    station_.reset();guard_.reset();
    try {
        guard_=std::make_shared<runtime::qt::StationGuard>(station_key_);
        station_=std::make_unique<Station>(guard_,snapshot.config);
        return station_->start();
    }catch(...) {return false;}
}
bool RecipeRuntime::start() {
    if(phase_!=RecipePhase::Stopped||stop_requested_)return false;
    try {candidate_=load_recipe(file_,paths_);}catch(...) {error_="RECIPE.INVALID";return false;}
    phase_=RecipePhase::Starting;
    if(!launch(*candidate_)) {phase_=RecipePhase::Faulted;error_="RECIPE.START_FAILED";return false;}
    return true;
}
bool RecipeRuntime::change(std::string document) {
    if(phase_!=RecipePhase::Active||result_||!active_)return false;
    try {
        auto next=parse_recipe(std::move(document),paths_);
        if(next.hash==active_->hash)return true;
        // Preserve a verified last-good snapshot before replacing the selected file.
        if(!save_recipe(file_+".last-good",*active_)||!save_recipe(file_,next)) {
            error_="RECIPE.SAVE_FAILED";return false;
        }
        candidate_=std::move(next);error_.clear();station_->pause();
        phase_=RecipePhase::Draining;stopping_old_=false;rolling_back_=false;return true;
    }catch(...) {error_="RECIPE.INVALID";return false;}
}
bool RecipeRuntime::exited() const {
    if(!station_)return true;
    for(const auto& worker:station_->workers())if(worker.process_alive)return false;
    return !station_->snapshot().leases&&!station_->snapshot().tickets;
}
void RecipeRuntime::pulse() {
    if(!station_)return;
    station_->pulse();
    if(!result_)result_=station_->take_result();
    if(stop_requested_) {
        if(exited()) {station_.reset();guard_.reset();phase_=RecipePhase::Stopped;}
        return;
    }
    if(phase_==RecipePhase::Draining) {
        if(station_->snapshot().active||result_)return;
        if(!stopping_old_) {station_->stop();stopping_old_=true;}
        if(!exited())return;
        phase_=RecipePhase::Applying;
        if(!launch(*candidate_)) {rolling_back_=true;phase_=RecipePhase::RollingBack;if(station_)station_->stop();}
    }
    if(phase_==RecipePhase::Starting||phase_==RecipePhase::Applying) {
        if(station_->snapshot().ready) {
            active_=std::move(candidate_);candidate_.reset();phase_=RecipePhase::Active;return;
        }
        if(station_->snapshot().state==ProductionState::Faulted) {
            station_->stop();
            if(active_) {rolling_back_=true;phase_=RecipePhase::RollingBack;}
            else {phase_=RecipePhase::Faulted;error_="RECIPE.INITIALIZE_FAILED";}
        }
    }
    if(phase_==RecipePhase::RollingBack) {
        if(rolling_back_) {
            if(!exited())return;
            if(!active_||!save_recipe(file_,*active_)||!launch(*active_)) {
                phase_=RecipePhase::Faulted;error_="RECIPE.ROLLBACK_FAILED";if(station_)station_->stop();return;
            }
            rolling_back_=false;error_="RECIPE.APPLY_FAILED_ROLLED_BACK";
        } else if(station_->snapshot().ready) {candidate_.reset();phase_=RecipePhase::Active;}
        else if(station_->snapshot().state==ProductionState::Faulted) {
            station_->stop();phase_=RecipePhase::Faulted;error_="RECIPE.ROLLBACK_FAILED";
        }
    }
}
void RecipeRuntime::stop() {
    stop_requested_=true;if(station_)station_->stop();else phase_=RecipePhase::Stopped;
}
TriggerStatus RecipeRuntime::trigger(std::uint64_t sequence) {
    return phase_==RecipePhase::Active&&!stop_requested_&&!result_?station_->trigger(sequence):TriggerStatus::NotReady;
}
std::optional<contracts::ResultEnvelope> RecipeRuntime::take_result() {
    auto result=std::move(result_);result_.reset();return result;
}
StationSnapshot RecipeRuntime::snapshot() const {
    return station_?station_->snapshot():StationSnapshot{ProductionState::Stopped,false,false,{},0,0};
}
}
