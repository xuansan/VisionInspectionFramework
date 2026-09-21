#include "pipeline.hpp"
#include <vision/application/readiness.hpp>
#include "../demo_paths.hpp"
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
#include <QPlainTextEdit>
#include <QTableWidget>
#include <QHeaderView>
#include <QImage>
#include <QCloseEvent>
#include <QTimer>
#include <QProcess>
#include <QCheckBox>
#include <QDir>
#include <QTemporaryDir>
#include <QUuid>
#include <iostream>
using namespace vision;
namespace {
QString state(application::ProductionState s) {
    switch(s) {
    case application::ProductionState::Stopped:return QStringLiteral("已停止");
    case application::ProductionState::Starting:return QStringLiteral("初始化");
    case application::ProductionState::Running:return QStringLiteral("运行中");
    case application::ProductionState::Paused:return QStringLiteral("已暂停");
    case application::ProductionState::Draining:return QStringLiteral("停止清算中");
    case application::ProductionState::Faulted:return QStringLiteral("故障");
    }
    return "?";
}
class Window final : public QWidget {
public:
    std::function<bool()> closing;
protected:
    void closeEvent(QCloseEvent* event) override {
        if(closing&&!closing())event->ignore();else event->accept();
    }
};
}
int pipeline_window(QApplication& app,const QString& smoke_scenario) {
    Window window;window.resize(1180,900);
    window.setWindowTitle(QStringLiteral("工业视觉框架 · 多进程检测工作站"));
    auto* layout=new QVBoxLayout(&window);
    auto* title=new QLabel(QStringLiteral("双相机检测工作站"),&window);
    title->setStyleSheet("font-size:25px;font-weight:600;color:#173b56;padding:8px 0;");
    layout->addWidget(title);
    auto* mode_label=new QLabel(QStringLiteral("Demo 模式  ·  模拟相机 / PLC  ·  基础亮度算法  ·  分发账本 Volatile（内存）  ·  Production 禁用"),&window);
    layout->addWidget(mode_label);
    application::Readiness production_readiness("workstation-demo","unvalidated-recipe",1);
    const auto readiness=production_readiness.evaluate(0);
    QString blocked;
    for(const auto& reason:readiness.blockers) {
        if(!blocked.isEmpty())blocked+=QStringLiteral(" / ");
        blocked+=QString::fromStdString(reason);
    }
    auto* production_status=new QLabel(QStringLiteral("生产联锁：未通过。未取得有效证据：")+blocked+
        QStringLiteral("\n可选持久模拟已接通结果落库与文件输出；图像持久追溯、真实硬件及完整生产恢复仍未验收。"),&window);
    production_status->setWordWrap(true);
    production_status->setObjectName("production-readiness");
    layout->addWidget(production_status);
    if(!smoke_scenario.isEmpty()&&(readiness.prerequisites_ready||application::production_integration_validated))return 1;
    auto* flow=new QLabel(QStringLiteral("模拟到位 → 相机 A / B → 共享图像池 → 算法 A / B → 工件汇总 → PLC ACK + 显示输出 / 审计输出"),&window);
    flow->setStyleSheet("padding:12px;background:#e9f2fa;color:#234b6a;border-radius:6px;");layout->addWidget(flow);
    auto* controls=new QHBoxLayout();
    auto* scenario=new QComboBox(&window);
    scenario->addItem(QStringLiteral("正常检测（3件）"),"normal");
    scenario->addItem(QStringLiteral("亮度 NG（3件）"),"ng");
    scenario->addItem(QStringLiteral("算法进程崩溃"),"algorithm-crash");
    scenario->addItem(QStringLiteral("一路输出挂死（另一路继续）"),"output-hang");
    scenario->addItem(QStringLiteral("相机缺帧 / 超时"),"camera-missing");
    auto* start=new QPushButton(QStringLiteral("启动演示"),&window);
    auto* pause=new QPushButton(QStringLiteral("暂停接纳"),&window);
    auto* resume=new QPushButton(QStringLiteral("继续接纳"),&window);
    auto* stop=new QPushButton(QStringLiteral("停止并清算"),&window);
    controls->addWidget(scenario);controls->addWidget(start);controls->addWidget(pause);controls->addWidget(resume);controls->addWidget(stop);
    controls->addStretch();layout->addLayout(controls);
    auto* persistent=new QCheckBox(QStringLiteral("持久模拟：SQLite 确认后回报模拟 PLC，结束时生成 JSONL 文件"),&window);
    persistent->setObjectName("persistent-demo");layout->addWidget(persistent);
    auto* persistence_status=new QLabel(QStringLiteral("持久模拟未启用。"),&window);
    persistence_status->setObjectName("persistence-status");persistence_status->setWordWrap(true);layout->addWidget(persistence_status);
    auto* archive_status=new QLabel(QStringLiteral("原图归档未启用。"),&window);
    archive_status->setWordWrap(true);layout->addWidget(archive_status);
    auto* model_tool=new QPushButton(QStringLiteral("打开模型试运行 / 图像工具"),&window);layout->addWidget(model_tool);
    QObject::connect(model_tool,&QPushButton::clicked,&window,[&] {
        QProcess::startDetached(QCoreApplication::applicationFilePath(),{"--model-tool"});
    });
    auto* summary=new QLabel(QStringLiteral("尚未启动。选择情景后启动，观察实际工作进程和结果。"),&window);
    summary->setWordWrap(true);summary->setMinimumHeight(46);layout->addWidget(summary);
    auto* workers=new QTableWidget(7,5,&window);
    workers->setHorizontalHeaderLabels({QStringLiteral("独立工作进程"),QStringLiteral("存活"),QStringLiteral("epoch"),QStringLiteral("忙碌"),QStringLiteral("诊断原因")});
    workers->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    workers->verticalHeader()->hide();workers->setEditTriggers(QAbstractItemView::NoEditTriggers);
    const QString names[]={QStringLiteral("显示输出"),QStringLiteral("审计输出"),QStringLiteral("模拟 PLC"),
        QStringLiteral("相机 A"),QStringLiteral("相机 B"),QStringLiteral("算法 A"),QStringLiteral("算法 B")};
    for(int row=0;row<7;++row)for(int col=0;col<5;++col)workers->setItem(row,col,new QTableWidgetItem(col==0?names[row]:"—"));
    workers->setMinimumHeight(255);layout->addWidget(workers);
    auto* deliveries=new QPlainTextEdit(&window);deliveries->setReadOnly(true);deliveries->setMaximumBlockCount(150);deliveries->setMaximumHeight(150);
    layout->addWidget(new QLabel(QStringLiteral("输出交付状态（与检测质量分开）"),&window));layout->addWidget(deliveries);
    auto* result=new QPlainTextEdit(&window);result->setReadOnly(true);result->setMaximumBlockCount(1000);
    layout->addWidget(new QLabel(QStringLiteral("最近工件的标准结果"),&window));layout->addWidget(result);
    layout->addWidget(new QLabel(QStringLiteral("实际使用独立子进程和共享图像；本页不代表真实硬件、ONNX或生产持久化已完成。"),&window));
    std::unique_ptr<application::qt::DemoRunner> runner;
    bool close_requested=false;unsigned seen=0;bool saw_paused=false,resumed=false,requested_stop=false;
    const bool durable_smoke=smoke_scenario.startsWith("durable");
    const auto effective_smoke=durable_smoke?(smoke_scenario=="durable-ng"?QString("ng"):
        smoke_scenario=="durable-stop"?QString("stop"):smoke_scenario=="durable-close"?QString("close"):QString("normal")):smoke_scenario;
    const bool lifecycle_smoke=effective_smoke=="close"||effective_smoke=="stop";
    QTemporaryDir smoke_directory;
    unsigned completed_runs=0;
    QObject::connect(start,&QPushButton::clicked,&window,[&] {
        try {
            if(runner&&!runner->snapshot().finished)return;
            runner.reset();seen=0;saw_paused=false;resumed=false;result->clear();deliveries->clear();
            std::optional<application::qt::DurableDemoConfig> config;
            if(persistent->isChecked()){
                const auto base=smoke_scenario.isEmpty()?QCoreApplication::applicationDirPath()+"/demo-runs":smoke_directory.path();
                const auto directory=base+"/"+QUuid::createUuid().toString(QUuid::WithoutBraces);
                if(!QDir().mkpath(directory+"/metadata")||!QDir().mkpath(directory+"/files"))throw std::runtime_error("Cannot create durable Demo directories");
                config=application::qt::DurableDemoConfig{directory+"/metadata",directory+"/files",
                    QString::fromUtf8(VISION_SQLITE_MANIFEST),QString::fromUtf8(VISION_FILE_MANIFEST),QString::fromUtf8(VISION_DURABLE_AGENT),QString::fromUtf8(VISION_FRAME_ARCHIVE)};
            }
            mode_label->setText(config?QStringLiteral("Demo 模式 · 模拟相机 / PLC · 检测结果持久落库 · 原图本地归档（未自动上传） · Production 禁用"):
                QStringLiteral("Demo 模式 · 模拟相机 / PLC · 分发账本 Volatile（内存） · Production 禁用"));
            flow->setText(config?QStringLiteral("模拟到位 → 双相机 → 独立原图归档 → 算法 → SQLite 结果确认 → 模拟 PLC ACK → 文件清算"):
                QStringLiteral("模拟到位 → 双相机 / 算法 → 工件结果 → 模拟 PLC ACK + 内存分发"));
            workers->setRowCount(config?8:7);
            if(config)for(int col=0;col<5;++col)workers->setItem(7,col,new QTableWidgetItem(col==0?QStringLiteral("SQLite 存储"):"—"));
            runner=std::make_unique<application::qt::DemoRunner>(demo_paths(),scenario->currentData().toString().toStdString(),3,std::move(config));
            if(!runner->start())throw std::runtime_error("Demo start failed");
            start->setEnabled(false);scenario->setEnabled(false);persistent->setEnabled(false);
        }catch(const std::exception& e) {summary->setText(QStringLiteral("启动失败：")+QString::fromUtf8(e.what()));if(!smoke_scenario.isEmpty())app.exit(1);}
    });
    QObject::connect(pause,&QPushButton::clicked,&window,[&]{if(runner)runner->pause();});
    QObject::connect(resume,&QPushButton::clicked,&window,[&]{if(runner)runner->resume();});
    QObject::connect(stop,&QPushButton::clicked,&window,[&]{if(runner)runner->stop();});
    window.closing=[&] {
        if(!runner||runner->snapshot().finished)return true;
        close_requested=true;runner->stop();return false;
    };
    QElapsedTimer time;time.start();QTimer loop;loop.setInterval(20);
    QObject::connect(&loop,&QTimer::timeout,&window,[&] {
        if(!runner)return;
        try {
            runner->pulse();const auto s=runner->snapshot();
            for(const auto& event:runner->take_events()) {
                ++seen;
                if(!smoke_scenario.isEmpty()) {
                    const auto expected=effective_smoke=="algorithm-crash"||effective_smoke=="camera-missing"?contracts::QualityVerdict::Unknown:
                        effective_smoke=="ng"?contracts::QualityVerdict::NG:contracts::QualityVerdict::OK;
                    if(event.result.quality!=expected)throw std::runtime_error("UI result mismatch");
                }
            }
            summary->setText(QStringLiteral("工位：%1  |  工件：%2  OK：%3  NG：%4  Unknown：%5  |  图像租约：%6  票据：%7\nPLC：%8  |  结果 ACK：%9  |  %10")
                .arg(state(s.station.state)).arg(s.station.counts.total).arg(s.station.counts.ok).arg(s.station.counts.ng).arg(s.station.counts.unknown)
                .arg(s.station.leases).arg(s.station.tickets).arg(s.device.online?QStringLiteral("在线"):QStringLiteral("未就绪 / 已停止"))
                .arg(QString::fromStdString(s.physical),QString::fromStdString(s.error)));
            persistence_status->setText(s.durable.persistence=="Disabled"?QStringLiteral("持久模拟未启用；当前结果仅在内存演示。"):
                QStringLiteral("结果存储：%1  |  已确认：%2  |  文件输出：%3  |  清算进程：%4  |  %5\n数据目录：%6\n文件目录：%7")
                .arg(QString::fromStdString(s.durable.persistence)).arg(s.durable.committed)
                .arg(QString::fromStdString(s.durable.file_output),s.durable.agent_alive?QStringLiteral("运行中"):QStringLiteral("未运行"),
                    QString::fromStdString(s.durable.error),s.durable.root,s.durable.files));
            archive_status->setText(persistent->isChecked()?QStringLiteral("原图本地归档：已确认 %1 张 | 独立归档进程：%2 | %3\n本地暂存不等于远端上传完成；分割掩码尚未联接。")
                .arg(s.station.archived_frames).arg(s.station.archive_alive?QStringLiteral("运行中"):QStringLiteral("未运行"),QString::fromStdString(s.station.archive_error)):
                QStringLiteral("原图归档未启用。"));
            for(int row=0;row<static_cast<int>(s.workers.size());++row) {
                const auto& w=s.workers[static_cast<std::size_t>(row)];
                workers->item(row,1)->setText(w.process_alive?QStringLiteral("是"):QStringLiteral("否"));
                workers->item(row,2)->setText(QString::number(w.epoch));
                workers->item(row,3)->setText(w.busy?QStringLiteral("是"):QStringLiteral("否"));
                workers->item(row,4)->setText(QString::fromStdString(w.reason));
            }
            QString reports;for(const auto& d:s.deliveries)reports+=QString::fromStdString(d.event_id+" | "+d.output_id+" | "+d.state+" | "+d.error)+"\n";
            if(deliveries->toPlainText()!=reports)deliveries->setPlainText(reports);
            if(result->toPlainText()!=QString::fromStdString(s.last_result))result->setPlainText(QString::fromStdString(s.last_result));
            pause->setEnabled(s.station.state==application::ProductionState::Running);
            resume->setEnabled(s.station.state==application::ProductionState::Paused&&!s.station.active);
            stop->setEnabled(!s.finished);
            if(!smoke_scenario.isEmpty()&&effective_smoke=="normal") {
                if(!saw_paused&&s.station.state==application::ProductionState::Running) {pause->click();saw_paused=true;}
                else if(saw_paused&&!resumed&&resume->isEnabled()) {resume->click();resumed=true;}
            }
            if(lifecycle_smoke&&!requested_stop&&s.station.active) {
                requested_stop=true;
                if(effective_smoke=="close")window.close();else stop->click();
            }
            if(s.finished) {
                start->setEnabled(true);scenario->setEnabled(true);persistent->setEnabled(true);
                if(close_requested)window.close();
                if(!smoke_scenario.isEmpty()) {
                    const unsigned expected=effective_smoke=="algorithm-crash"||effective_smoke=="camera-missing"?1U:3U;
                    if(!s.error.empty()||(!lifecycle_smoke&&seen!=expected)||s.station.leases||s.station.tickets||
                        (!lifecycle_smoke&&(result->toPlainText().isEmpty()||deliveries->toPlainText().isEmpty())))throw std::runtime_error("UI cleanup/outcome");
                    if(s.station.archive_alive)throw std::runtime_error("UI leaked archive process");
                    if(durable_smoke&&!lifecycle_smoke&&s.station.archived_frames!=expected*2)throw std::runtime_error("UI image archive count");
                    for(const auto& worker:s.workers)if(worker.process_alive)throw std::runtime_error("UI leaked worker");
                    if(lifecycle_smoke&&!requested_stop)throw std::runtime_error("UI stop not exercised");
                    if(effective_smoke=="normal"&&!resumed)throw std::runtime_error("UI pause/resume not exercised");
                    if(durable_smoke&&!lifecycle_smoke&&(s.durable.persistence!="Durable"||s.durable.committed!=expected||
                        s.durable.file_output!="BusinessAcked"||s.durable.agent_alive))throw std::runtime_error("UI durable outcome");
                    if(smoke_scenario=="durable-restart"&&completed_runs++==0){start->click();return;}
                    QImage image(window.size(),QImage::Format_ARGB32_Premultiplied);image.fill(Qt::transparent);window.render(&image);
                    if(!image.save("pipeline-"+smoke_scenario+".png"))throw std::runtime_error("UI rendering");
                    std::cout<<"pipeline UI "<<smoke_scenario.toStdString()<<": passed\n";app.exit(0);
                }
            }
            if(!smoke_scenario.isEmpty()&&time.elapsed()>15000)throw std::runtime_error("UI deadline");
        }catch(const std::exception& e) {summary->setText(QString::fromUtf8(e.what()));runner->stop();if(!smoke_scenario.isEmpty())app.exit(1);}
    });
    loop.start();window.show();
    if(!smoke_scenario.isEmpty()) {
        persistent->setChecked(durable_smoke);
        const auto index=scenario->findData(lifecycle_smoke?QStringLiteral("normal"):effective_smoke);if(index<0)return 2;scenario->setCurrentIndex(index);
        QTimer::singleShot(0,start,&QPushButton::click);
    }
    return app.exec();
}
