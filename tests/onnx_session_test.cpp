#include <vision/application/qt/station.hpp>
#include <vision/serialization/json_codec.hpp>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include <QTemporaryDir>
#include <iostream>
using namespace vision;
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    QTemporaryDir masks_a,masks_b;
    try {
        auto guard=std::make_shared<runtime::qt::StationGuard>("onnx-"+QUuid::createUuid().toString(QUuid::WithoutBraces));
        application::qt::StationConfig config;config.host=app.arguments()[1];config.camera_manifest=app.arguments()[2];config.algorithm_manifest=app.arguments()[3];
        config.budget_ns=3000000000;
        const auto parameters=QJsonDocument(QJsonObject{{"config",app.arguments()[4]}}).toJson(QJsonDocument::Compact).toStdString();
        config.algorithm_parameters={parameters,parameters};
        if(app.arguments()[4].contains("segmentation")) {
            config.algorithm_parameters={
                QJsonDocument(QJsonObject{{"config",app.arguments()[4]},{"mask_root",masks_a.path()}}).toJson(QJsonDocument::Compact).toStdString(),
                QJsonDocument(QJsonObject{{"config",app.arguments()[4]},{"mask_root",masks_b.path()}}).toJson(QJsonDocument::Compact).toStdString()};
        }
        application::qt::Station station(guard,config);if(!station.start())return 1;
        bool submitted=false,stopping=false;unsigned results=0;QElapsedTimer time;time.start();QTimer loop;loop.setInterval(10);
        QObject::connect(&loop,&QTimer::timeout,&app,[&] {
            try {
                station.pulse();
                if(auto event=station.take_result()) {
                    ++results;(void)serialization::decode_result(serialization::encode_result(*event));
                    const bool uncertain=app.arguments()[4].contains("uncertain");
                    if(event->result.quality!=(uncertain?contracts::QualityVerdict::Unknown:contracts::QualityVerdict::NG)||event->result.checks.size()!=2){
                        std::cerr<<serialization::encode_result(*event)<<'\n';
                        for(const auto& worker:station.workers())std::cerr<<"worker epoch="<<worker.epoch<<" reason="<<worker.reason<<'\n';
                        throw std::runtime_error("ONNX quality");
                    }
                    for(const auto& check:event->result.checks) {
                        if(!check.model_hash)throw std::runtime_error("Model evidence missing");
                        if(app.arguments()[4].contains("classification")) {
                            if(check.measurements.empty()||(!uncertain&&!check.classification))throw std::runtime_error("Classification evidence missing");
                            if(uncertain&&(!check.error||check.error->code!="INFERENCE.LOW_CONFIDENCE"||check.classification))
                                throw std::runtime_error("Uncertain classification");
                        } else if(check.defects.empty())throw std::runtime_error("Detection evidence missing");
                        if(app.arguments()[4].contains("segmentation")&&check.masks.size()!=check.defects.size())
                            throw std::runtime_error("Mask references missing");
                    }
                    station.stop();stopping=true;
                }
                if(stopping&&station.snapshot().state==application::ProductionState::Stopped) {
                    if(results!=1||station.snapshot().leases||station.snapshot().tickets)throw std::runtime_error("ONNX cleanup");
                    app.exit(0);
                } else if(!submitted&&station.snapshot().ready) {
                    if(station.trigger(1)!=application::qt::TriggerStatus::Accepted)throw std::runtime_error("ONNX admission");submitted=true;
                }
                if(time.elapsed()>12000)throw std::runtime_error("ONNX session deadline");
            }catch(const std::exception& e){std::cerr<<e.what()<<'\n';station.stop();app.exit(1);}
        });
        loop.start();return app.exec();
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
