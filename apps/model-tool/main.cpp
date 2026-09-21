#include <vision/application/qt/trial.hpp>
#include <vision/inference/engine.hpp>
#include <vision/serialization/json_codec.hpp>
#include <vision/capture/source.hpp>
#include <vision/replay/assets.hpp>
#include <nlohmann/json.hpp>
#include <QCoreApplication>
#include <QSaveFile>
#include <QDir>
#include <QTimer>
#include <iostream>
#include <set>
#include <io.h>
#include <fcntl.h>
using namespace vision;
namespace {
std::string utf8(const std::filesystem::path& p){const auto s=p.generic_u8string();return {reinterpret_cast<const char*>(s.data()),s.size()};}
int execute(QCoreApplication& app,const replay::Snapshot& snapshot,const QString& output) {
    if(!QDir(output).mkpath("masks"))throw std::runtime_error("Mask directory");
    application::qt::ReplayService session({QString::fromUtf8(VISION_DEMO_HOST),QString::fromUtf8(VISION_DEMO_CAMERA),
        QString::fromUtf8(VISION_ONNX_MANIFEST),QString::fromStdString(utf8(snapshot.image)),
        QString::fromStdString(utf8(snapshot.config)),output+"/masks",snapshot.layout,snapshot.hash,snapshot.pixel_hash});
    if(!session.start())throw std::runtime_error("Replay start");
    std::optional<contracts::CheckResult> result;
    QTimer timer;timer.setInterval(10);
    QObject::connect(&timer,&QTimer::timeout,&app,[&] {
        try {
            session.pulse();if(auto value=session.take_result())result=std::move(value);
            if(session.finished()) {
                if(!result){app.exit(1);return;}
                std::cout<<nlohmann::json{{"mode","Replay"},{"recipe_hash",snapshot.hash},{"snapshot_hash",snapshot.hash},
                    {"manifest",utf8(snapshot.manifest)},{"check",nlohmann::json::parse(serialization::encode_check(*result))}}.dump()<<'\n';
                app.exit(0);
            }
        }catch(const std::exception& e){std::cerr<<e.what()<<'\n';session.stop();app.exit(1);}
    });
    timer.start();return app.exec();
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        if(argc==5&&app.arguments()[1]=="--replay") {
            const auto snapshot=replay::load_snapshot(inference::utf8_path(app.arguments()[2].toStdString()),app.arguments()[3].toStdString());
            return execute(app,snapshot,app.arguments()[4]);
        }
        if(argc==3&&app.arguments()[1]=="--preview") {
            const auto bytes=inference::read_file(inference::utf8_path(app.arguments()[2].toStdString()),16*1024*1024+4096);
            const auto image=capture::decode_pnm(bytes);
            if(static_cast<std::uint64_t>(image.layout.width)*image.layout.height>4*1024*1024)throw std::invalid_argument("Preview pixels");
            // Dedicated display pipe, not the runtime control-message channel.
            _setmode(_fileno(stdout),_O_BINARY);
            std::cout.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));return 0;
        }
        if(argc!=3||app.arguments()[1]!="--request"||app.arguments()[2].toUtf8().size()>16384)return 2;
        const auto r=nlohmann::json::parse(app.arguments()[2].toStdString());
        const std::set<std::string> allowed{"config","image","width","height","channels","score","roi","output"};
        for(auto it=r.begin();it!=r.end();++it)if(!allowed.contains(it.key()))throw std::invalid_argument("Request field");
        auto integer=[&](const char* name,unsigned max) {
            const auto& v=r.at(name);if(!v.is_number_integer()||v.get<std::int64_t>()<=0||v.get<std::uint64_t>()>max)throw std::invalid_argument("Image dimension");
            return v.get<unsigned>();
        };
        const auto w=integer("width",4096),h=integer("height",4096),ch=integer("channels",3);
        if(ch!=1&&ch!=3)throw std::invalid_argument("Channels");
        const contracts::ImageLayout layout{w,h,static_cast<std::uint64_t>(w)*ch,0,static_cast<std::uint64_t>(w)*h*ch,
            ch==1?contracts::PixelFormat::Mono8:contracts::PixelFormat::RGB8};
        contracts::validate_layout(layout,16*1024*1024);
        const auto config_path=inference::utf8_path(r.at("config").get<std::string>());
        auto config=inference::read_config(config_path);
        auto data=inference::read_file(config_path,65536);
        auto model=nlohmann::json::parse(reinterpret_cast<const char*>(data.data()),reinterpret_cast<const char*>(data.data()+data.size()));
        // nlohmann serializes u8string as an array; use explicit UTF8 bytes.
        const auto path=std::filesystem::absolute(config.model).generic_u8string();
        model["model"]=std::string(reinterpret_cast<const char*>(path.data()),path.size());
        model["score"]=r.at("score");model["roi"]=r.at("roi");
        const auto output=QString::fromStdString(r.at("output").get<std::string>());
        if(!QDir(output).exists())throw std::invalid_argument("Output directory");
        const auto serialized=model.dump();
        QSaveFile file(output+"/effective.json");
        if(!file.open(QIODevice::WriteOnly)||file.write(serialized.data(),static_cast<qint64>(serialized.size()))!=static_cast<qint64>(serialized.size())||!file.commit())
            throw std::runtime_error("Effective config save");
        (void)inference::read_config(inference::utf8_path((output+"/effective.json").toStdString()));
        if(!QDir(output).mkpath("assets"))throw std::runtime_error("Asset root");
        const auto snapshot=replay::create_snapshot(inference::utf8_path((output+"/effective.json").toStdString()),
            inference::utf8_path(r.at("image").get<std::string>()),inference::utf8_path((output+"/assets").toStdString()));
        if(snapshot.layout.width!=layout.width||snapshot.layout.height!=layout.height||snapshot.layout.format!=layout.format)
            throw std::invalid_argument("Image request mismatch");
        return execute(app,snapshot,output);
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
