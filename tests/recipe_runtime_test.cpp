#include "../apps/demo_paths.hpp"
#include <vision/application/qt/recipe.hpp>
#include <QTemporaryDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <iostream>
using namespace vision;
using namespace application::qt;
namespace {
void check(bool v,const char* m) {if(!v)throw std::runtime_error(m);}
QByteArray read(const QString& path) {QFile f(path);check(f.open(QIODevice::ReadOnly),"read");return f.readAll();}
void write(const QString& path,const QByteArray& data) {QFile f(path);check(f.open(QIODevice::WriteOnly),"write");check(f.write(data)==data.size(),"write bytes");}
int run(QCoreApplication& app,const QString& scenario) {
    auto paths=demo_paths();QTemporaryDir dir;check(dir.isValid(),"temporary directory");
    const auto original=read(paths.recipe);
    auto next=QJsonDocument::fromJson(original).object();next["version"]="1.0.1";
    auto cameras=next["cameras"].toArray();auto camera=cameras[1].toObject();camera["minimum"]=255;cameras[1]=camera;next["cameras"]=cameras;
    const auto changed=QJsonDocument(next).toJson().toStdString();
    const auto manifest=read(paths.algorithm);
    const auto package=QFileInfo(paths.algorithm).absolutePath();
    check(QFile::copy(package+"/vision-basic-algorithm.dll",dir.filePath("vision-basic-algorithm.dll")),"copy plugin");
    paths.algorithm=dir.filePath("basic-algorithm.json");write(paths.algorithm,manifest);
    RecipePaths libraries{paths.host,paths.camera,paths.algorithm};
    const auto file=dir.filePath("active.json");write(file,original);
    const auto initial=load_recipe(file,libraries),updated=parse_recipe(changed,libraries);
    check(initial.hash!=updated.hash,"snapshot hash");
    check(!save_recipe(dir.filePath("missing/sub/file.json"),updated),"invalid save succeeded");
    check(load_recipe(file,libraries).hash==initial.hash,"failed save changed old file");
    RecipeRuntime runtime("recipe-"+QUuid::createUuid().toString(QUuid::WithoutBraces),file,libraries);
    check(runtime.start(),"start");bool triggered=false,change_requested=false,new_trigger=false,stopping=false,restored=false;
    unsigned results=0;std::string first_run;
    QElapsedTimer time;time.start();QTimer loop;loop.setInterval(10);
    QObject::connect(&loop,&QTimer::timeout,&app,[&] {
        try {
            runtime.pulse();
            if(auto event=runtime.take_result()) {
                ++results;
                if(results==1) {
                    check(event->result.recipe_hash==initial.hash,"inflight recipe changed");
                    check(event->result.quality==contracts::QualityVerdict::OK,"first result");
                    first_run=event->result.run_id.value();
                } else {
                    check(event->result.recipe_hash==(scenario=="switch"?updated.hash:initial.hash),"active snapshot");
                    check(event->result.run_id.value()!=first_run,"run namespace reused");
                    check(event->result.quality==(scenario=="switch"?contracts::QualityVerdict::NG:contracts::QualityVerdict::OK),"new result");
                }
            }
            if(stopping) {
                if(runtime.phase()==RecipePhase::Stopped) {check(!runtime.snapshot().leases&&!runtime.snapshot().tickets,"leak");app.exit(0);}
                return;
            }
            if(runtime.phase()==RecipePhase::Active&&!triggered) {
                check(!runtime.change("{}"),"invalid document");check(runtime.active_hash()==initial.hash,"invalid changed active");
                check(runtime.trigger(1)==TriggerStatus::Accepted,"first trigger");triggered=true;
                if(scenario=="rollback"||scenario=="rollback-fail")write(paths.algorithm,"{}");
                if(scenario=="save-fail") {
                    QFile blocker(file+".last-good");check(blocker.open(QIODevice::WriteOnly),"blocker");
                    check(!runtime.change(changed),"locked backup save succeeded");blocker.close();
                    check(runtime.active_hash()==initial.hash&&runtime.phase()==RecipePhase::Active,"save failure state");
                } else {
                    check(runtime.change(changed),"change");change_requested=true;
                    check(runtime.trigger(2)==TriggerStatus::NotReady,"admission during change");
                    check(runtime.active_hash()==initial.hash,"premature activation");
                }
            }
            if(scenario=="rollback"&&runtime.phase()==RecipePhase::RollingBack&&!restored) {write(paths.algorithm,manifest);restored=true;}
            if(scenario=="rollback-fail"&&runtime.phase()==RecipePhase::Faulted) {
                check(runtime.error()=="RECIPE.ROLLBACK_FAILED","rollback fault");
                check(runtime.trigger(2)==TriggerStatus::NotReady,"fault admitted");runtime.stop();stopping=true;
            }
            if(scenario=="save-fail"&&results==1&&!runtime.snapshot().active) {runtime.stop();stopping=true;}
            if(change_requested&&results==1&&runtime.phase()==RecipePhase::Active&&!new_trigger) {
                check(runtime.active_hash()==(scenario=="switch"?updated.hash:initial.hash),"activation");
                check(load_recipe(file,libraries).hash==runtime.active_hash(),"disk active mismatch");
                check(load_recipe(file+".last-good",libraries).hash==initial.hash,"last good");
                check(runtime.trigger(1)==TriggerStatus::Accepted,"new run trigger");new_trigger=true;
            }
            if(results==2) {runtime.stop();stopping=true;}
            check(time.elapsed()<12000,"recipe test deadline");
        }catch(const std::exception& e){std::cerr<<e.what()<<'\n';runtime.stop();app.exit(1);}
    });
    loop.start();return app.exec();
}
}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{return run(app,app.arguments().at(1));}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
