#include <vision/application/qt/trial.hpp>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUuid>
namespace vision::application::qt {
namespace {std::string json(const QJsonObject& o){return QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString();}}
TrialSession::TrialSession(TrialConfig c) {
    guard_=std::make_shared<runtime::qt::StationGuard>("trial-"+QUuid::createUuid().toString(QUuid::WithoutBraces));
    const QFileInfo file(c.image);
    const auto files=QString::fromUtf8(QJsonDocument(QJsonArray{file.fileName()}).toJson(QJsonDocument::Compact));
    const auto channels=c.layout.format==contracts::PixelFormat::Mono8?1:3;
    camera_=std::make_unique<capture::qt::Session>(guard_,c.host,c.camera_manifest,
        json({{"source","fixed"},{"root",file.absolutePath()},{"files_json",files},{"width",static_cast<int>(c.layout.width)},
            {"height",static_cast<int>(c.layout.height)},{"channels",channels},{"pixel_hash",QString::fromStdString(c.pixel_hash)}}),"trial-camera",c.layout,2,c.recipe_hash);
    algorithm_=std::make_unique<algorithm::qt::Session>(guard_,c.host,c.algorithm_manifest,
        json({{"config",c.model_config},{"mask_root",c.mask_root}}),"trial-algorithm",*camera_,c.recipe_hash);
}
TrialSession::~TrialSession(){stop();}
bool TrialSession::start(){elapsed_.start();return camera_->start()&&algorithm_->start();}
void TrialSession::stop(){stopping_=true;algorithm_->stop();camera_->stop();}
bool TrialSession::finished() const {
    return stopping_&&!camera_->worker().process_alive&&!algorithm_->worker().process_alive&&!camera_->buffers().leases;
}
void TrialSession::pulse() {
    camera_->pulse();algorithm_->pulse();
    if(stopping_)return;
    if(elapsed_.elapsed()>15000) {stop();return;}
    if(!submitted_&&camera_->ready()&&algorithm_->ready()) {
        submitted_=true;if(!camera_->capture(3000000000ULL))stop();
    }
    if(auto captured=camera_->take_completion()) {
        if(!captured->frame){stop();return;}
        if(!algorithm_->inspect(*captured->frame,contracts::InspectionId("trial-inspection"),contracts::CheckId("trial-check"),5000000000ULL)) {
            camera_->release(*captured->frame);stop();return;
        }
    }
    if(auto completed=algorithm_->take_completion()) {
        if(completed->evidence)result_=std::move(completed->evidence);
        stop();
    }
}
std::optional<contracts::CheckResult> TrialSession::take_result(){auto value=std::move(result_);result_.reset();return value;}
}
