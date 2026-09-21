#include "../demo_paths.hpp"
#include <vision/serialization/json_codec.hpp>
#include <QJsonObject>
#include <QJsonDocument>
#include <iostream>
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        const auto args=app.arguments();
        if(args.size()!=2)return 64;
        const auto scenario=args[1].toStdString();
        vision::application::qt::DemoRunner runner(demo_paths(),scenario);
        if(!runner.start())return 1;
        unsigned seen=0;QTimer loop;loop.setInterval(10);
        QObject::connect(&loop,&QTimer::timeout,&app,[&] {
            try {
                runner.pulse();
                for(const auto& event:runner.take_events()) {
                    ++seen;std::cout<<vision::serialization::encode_result(event)<<'\n';
                    const auto expected=scenario=="ng"?vision::contracts::QualityVerdict::NG:
                        scenario=="algorithm-crash"||scenario=="camera-missing"?vision::contracts::QualityVerdict::Unknown:vision::contracts::QualityVerdict::OK;
                    if(event.result.quality!=expected)throw std::runtime_error("Demo unexpected quality");
                }
                const auto s=runner.snapshot();
                if(!s.finished)return;
                for(const auto& d:s.deliveries) {
                    std::cout<<QJsonDocument(QJsonObject{{"event_id",QString::fromStdString(d.event_id)},
                        {"output",QString::fromStdString(d.output_id)},{"delivery",QString::fromStdString(d.state)},
                        {"mode","Demo"},{"durability","Volatile"}}).toJson(QJsonDocument::Compact).toStdString()<<'\n';
                    if(d.output_id=="display"&&d.state!="BusinessAcked")throw std::runtime_error("Healthy output failed");
                    if(d.output_id=="audit"&&scenario=="output-hang"&&d.state=="BusinessAcked")throw std::runtime_error("Hung output acked");
                }
                const auto expected=scenario=="algorithm-crash"||scenario=="camera-missing"?1U:3U;
                if(!s.error.empty()||seen!=expected||s.station.leases||s.station.tickets||s.deliveries.size()!=seen*2)
                    throw std::runtime_error("Demo outcome or cleanup");
                std::cout<<"Demo complete: results="<<seen<<", leases=0, tickets=0, production=false\n";app.exit(0);
            }catch(const std::exception& e){std::cerr<<e.what()<<'\n';runner.stop();app.exit(1);}
        });
        loop.start();return app.exec();
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
