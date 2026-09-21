#pragma once
#include <vision/application/qt/station.hpp>
namespace vision::application::qt {
struct RecipePaths {QString host,camera,algorithm;};
struct RecipeSnapshot {std::string canonical,hash;StationConfig config;};
RecipeSnapshot parse_recipe(std::string,const RecipePaths&);
RecipeSnapshot load_recipe(const QString&,const RecipePaths&);
// Atomic visibility on the same filesystem. Does not claim power-loss durability.
bool save_recipe(const QString&,const RecipeSnapshot&);
enum class RecipePhase {Stopped,Starting,Active,Draining,Applying,RollingBack,Faulted};
class RecipeRuntime {
public:
    RecipeRuntime(QString station_key,QString file,RecipePaths);
    ~RecipeRuntime();
    bool start();
    bool change(std::string document); // Validate/save first; freeze admission and drain existing work.
    void pulse();
    void stop();
    TriggerStatus trigger(std::uint64_t);
    std::optional<contracts::ResultEnvelope> take_result();
    RecipePhase phase() const {return phase_;}
    std::string active_hash() const {return active_?active_->hash:"";}
    std::string error() const {return error_;}
    StationSnapshot snapshot() const;
private:
    bool launch(const RecipeSnapshot&);
    bool exited() const;
    QString station_key_,file_;
    RecipePaths paths_;
    std::shared_ptr<runtime::qt::StationGuard> guard_;
    std::unique_ptr<Station> station_;
    std::optional<RecipeSnapshot> active_,candidate_;
    std::optional<contracts::ResultEnvelope> result_;
    RecipePhase phase_{RecipePhase::Stopped};
    std::string error_;
    bool stopping_old_{},rolling_back_{},stop_requested_{};
};
}
