#include "../apps/demo_paths.hpp"
#include <QFile>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <iostream>
using namespace vision::application::qt;
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        auto paths=demo_paths();QFile source(paths.recipe);
        if(!source.open(QIODevice::ReadOnly))return 1;
        const auto original=source.readAll();
        QTemporaryDir directory;if(!directory.isValid())return 1;
        paths.recipe=directory.filePath("recipe.json");
        auto write=[&](QByteArray bytes) {
            QFile file(paths.recipe);
            if(!file.open(QIODevice::WriteOnly)||file.write(bytes)!=bytes.size())throw std::runtime_error("test recipe write");
        };
        for(int n=0;n<9;++n) {
            auto value=QJsonDocument::fromJson(original).object();
            switch(n) {
            case 0:value["mode"]="Production";break;
            case 1:value["width"]=0;break;
            case 2:value["width"]=1.5;break;
            case 3:value["height"]=4097;break;
            case 4:value["deadline_ms"]=0;break;
            case 5:value["unexpected"]=true;break;
            case 6:value["cameras"]=QJsonArray{};break;
            case 7:{
                auto cameras=value["cameras"].toArray();auto camera=cameras[0].toObject();
                camera["minimum"]=256;cameras[0]=camera;value["cameras"]=cameras;break;
            }
            case 8:value["recipe_id"]="bad id";break;
            }
            write(QJsonDocument(value).toJson());
            bool rejected=false;try {DemoRunner runner(paths,"normal");}catch(...) {rejected=true;}
            if(!rejected)throw std::runtime_error("Invalid recipe accepted");
        }
        auto duplicate=original;duplicate.insert(duplicate.indexOf('{')+1,"\"mode\":\"Production\",");
        write(duplicate);
        bool rejected=false;try {DemoRunner runner(paths,"normal");}catch(...) {rejected=true;}
        if(!rejected)throw std::runtime_error("Duplicate recipe key accepted");
        write(original);
        {DemoRunner runner(paths,"normal");}
        std::cout<<"Demo recipe: 10 invalid documents rejected; valid recipe accepted\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
