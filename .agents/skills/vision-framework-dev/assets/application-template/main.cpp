#include <vision/application/qt/demo_runner.hpp>
#include <vision/serialization/json_codec.hpp>
#include <QApplication>
#include <QWidget>
#include <QVBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QPlainTextEdit>
#include <QElapsedTimer>
#include <QTimer>
#include <iostream>
int main(int argc,char** argv){
    QApplication app(argc,argv);
    using namespace vision;
    const auto args=app.arguments();const bool smoke=args.size()==3&&args[1]=="--smoke";
    if(args.size()!=1&&!smoke)return 64;
    const auto scenario=smoke?args[2].toStdString():"normal";
    if(scenario!="normal"&&scenario!="ng"&&scenario!="stop"&&scenario!="restart")return 64;
    QWidget window;window.setWindowTitle("@PROJECT_NAME@ - Demo");window.resize(900,600);
    QVBoxLayout layout(&window);
    QLabel status(QStringLiteral("模拟相机 / 模拟PLC；结果分发为内存；Production禁用"));layout.addWidget(&status);
    QPushButton start(QStringLiteral("开始模拟")),stop(QStringLiteral("停止"));layout.addWidget(&start);layout.addWidget(&stop);
    QPlainTextEdit results;results.setReadOnly(true);results.setMaximumBlockCount(1000);layout.addWidget(&results);
    std::unique_ptr<application::qt::DemoRunner> runner;
    unsigned seen=0,completed_runs=0;bool requested_stop=false;QElapsedTimer elapsed;
    QObject::connect(&start,&QPushButton::clicked,&window,[&]{
        try{
            if(runner&&!runner->snapshot().finished)return;
            runner.reset();seen=0;requested_stop=false;results.clear();
            application::qt::DemoPaths paths{QString::fromUtf8(VISION_DEMO_HOST),QString::fromUtf8(VISION_DEMO_CAMERA),
                QString::fromUtf8(VISION_DEMO_ALGORITHM),QString::fromUtf8(VISION_DEMO_DEVICE),QString::fromUtf8(VISION_DEMO_OUTPUT),
                QCoreApplication::applicationDirPath()+"/recipe.json"};
            runner=std::make_unique<application::qt::DemoRunner>(paths,scenario=="ng"?"ng":"normal");
            if(!runner->start())throw std::runtime_error("Demo start failed");elapsed.start();start.setEnabled(false);
        }catch(const std::exception& e){status.setText(QString::fromUtf8(e.what()));if(smoke)app.exit(1);}
    });
    QObject::connect(&stop,&QPushButton::clicked,&window,[&]{if(runner){requested_stop=true;runner->stop();}});
    QTimer timer;timer.setInterval(10);
    QObject::connect(&timer,&QTimer::timeout,&window,[&]{
        if(!runner)return;
        try{
            runner->pulse();
            for(const auto& event:runner->take_events()){
                ++seen;results.appendPlainText(QString::fromStdString(serialization::encode_result(event)));
                if(smoke&&scenario!="stop"&&event.result.quality!=(scenario=="ng"?contracts::QualityVerdict::NG:contracts::QualityVerdict::OK))throw std::runtime_error("Unexpected quality");
            }
            const auto s=runner->snapshot();status.setText(QStringLiteral("Demo | 工件 %1 | 租约 %2 | PLC %3 | %4")
                .arg(s.results).arg(s.station.leases).arg(QString::fromStdString(s.physical),QString::fromStdString(s.error)));
            if(smoke&&scenario=="stop"&&s.station.active&&!requested_stop)stop.click();
            if(s.finished){
                start.setEnabled(true);
                if(smoke){
                    if(!s.error.empty()||s.station.leases||s.station.tickets)throw std::runtime_error("Outcome/cleanup");
                    if(scenario=="stop"){if(!requested_stop||s.physical=="BusinessAcked")throw std::runtime_error("Stop not isolated");}
                    else if(requested_stop||seen!=3||s.physical!="BusinessAcked")throw std::runtime_error("Result count/PLC");
                    for(const auto& worker:s.workers)if(worker.process_alive)throw std::runtime_error("Worker leak");
                    if(scenario!="stop")for(const auto& delivery:s.deliveries)if(delivery.state!="BusinessAcked")throw std::runtime_error("Output delivery");
                    if(scenario=="restart"&&++completed_runs==1){start.click();return;}
                    std::cout<<"@PROJECT_NAME@: "<<scenario<<", leases=0, production=false\n";app.exit(0);
                }
            }
            if(smoke&&elapsed.elapsed()>15000)throw std::runtime_error("Smoke deadline");
        }catch(const std::exception& e){status.setText(QString::fromUtf8(e.what()));runner->stop();if(smoke){std::cerr<<e.what();app.exit(1);}}
    });
    window.show();timer.start();if(smoke)QTimer::singleShot(0,&start,&QPushButton::click);
    const auto code=app.exec();if(runner)runner->stop();return code;
}
