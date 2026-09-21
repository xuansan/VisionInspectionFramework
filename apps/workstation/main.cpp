#include <vision/application/build_info.hpp>
#include <vision/application/demo.hpp>
#include <vision/serialization/json_codec.hpp>
#include <QApplication>
#include <QImage>
#include <QLabel>
#include <QVBoxLayout>
#include <QWidget>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QHBoxLayout>
#include <QFontDatabase>
#include <QDir>
#include <cstring>
#include <vector>
#ifdef VISION_PIPELINE_UI
#include "pipeline.hpp"
#include "model_tool.hpp"
#endif

int main(int argc, char** argv) {
    const bool smoke = argc == 2 && std::strcmp(argv[1], "--smoke-test") == 0;
    const bool pipeline_smoke=argc==3&&std::strcmp(argv[1],"--pipeline-smoke")==0;
    const bool model_ui=argc==2&&std::strcmp(argv[1],"--model-tool")==0;
    const bool model_smoke=argc==2&&std::strcmp(argv[1],"--model-smoke")==0;
    if(argc != 1 && !smoke&&!pipeline_smoke&&!model_ui&&!model_smoke) return 2;
    if(smoke||pipeline_smoke||model_smoke) qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
#ifdef _WIN32
    // The offscreen platform does not discover Windows fallback fonts itself.
    // Read the system font for test rendering; do not copy/distribute its file.
    if(smoke||pipeline_smoke||model_smoke) {
        const auto font=QDir(qEnvironmentVariable("WINDIR")).filePath(QStringLiteral("Fonts/msyh.ttc"));
        const auto font_id=QFontDatabase::addApplicationFont(font);
        const auto families=QFontDatabase::applicationFontFamilies(font_id);
        if(families.isEmpty()) return 1;
        application.setFont(QFont(families.front(),10));
    }
#endif
#ifdef VISION_PIPELINE_UI
    if(model_ui||model_smoke)return model_tool_window(application,model_smoke);
    if(!smoke)return pipeline_window(application,pipeline_smoke?QString::fromUtf8(argv[2]):QString{});
#else
    if(pipeline_smoke)return 2;
#endif
    const auto info = vision::application::build_info();
    QWidget window;
    window.setWindowTitle(QStringLiteral("工业视觉框架 · 核心基础演示"));
    window.resize(920, 640);
    auto* layout = new QVBoxLayout(&window);
    layout->addWidget(new QLabel(QStringLiteral("工业视觉检测框架"), &window));
    layout->addWidget(new QLabel(QStringLiteral("Windows 核心演示 · 合成检查数据 · 不连接相机或 PLC"), &window));
    layout->addWidget(new QLabel(QStringLiteral("版本：") +
        QString::fromUtf8(info.version.data(), static_cast<int>(info.version.size())), &window));
    auto* buttons=new QHBoxLayout();
    auto* status=new QLabel(QStringLiteral("请选择一个情景，查看双检查项汇总、计数与资源释放。"),&window);
    auto* output=new QPlainTextEdit(&window);
    output->setReadOnly(true);
    output->setMaximumBlockCount(3000);
    using S=vision::application::DemoScenario;
    const std::pair<QString,S> scenarios[]={
        {QStringLiteral("正常 OK"),S::Ok},{QStringLiteral("缺陷 NG"),S::Ng},
        {QStringLiteral("算法失败"),S::Failure},{QStringLiteral("检查超时"),S::Timeout},
        {QStringLiteral("容量不足"),S::Overload}};
    std::vector<QPushButton*> scenario_buttons;
    for(const auto& [label,scenario]:scenarios) {
        auto* button=new QPushButton(label,&window);
        scenario_buttons.push_back(button);
        buttons->addWidget(button);
        QObject::connect(button,&QPushButton::clicked,&window,[=] {
            try {
                const auto demo=vision::application::run_demo(scenario);
                const auto state=demo.event ? std::string(vision::contracts::name(demo.event->result.state)) : "Rejected";
                const auto quality=demo.event ? std::string(vision::contracts::name(demo.event->result.quality)) : "Unknown";
                status->setText(QStringLiteral("执行：%1  |  质量：%2  |  本次计数：%3  |  未释放资源票据：%4")
                    .arg(QString::fromStdString(state),QString::fromStdString(quality))
                    .arg(static_cast<qulonglong>(demo.counts.total)).arg(static_cast<qulonglong>(demo.resources.active_tickets)));
                output->setPlainText(QString::fromStdString(demo.event ?
                    vision::serialization::encode_result(*demo.event) : demo.message));
            } catch(const std::exception& e) {
                status->setText(QStringLiteral("演示执行失败，未形成有效结果"));
                output->setPlainText(QString::fromUtf8(e.what()));
            }
        });
    }
    layout->addLayout(buttons);
    layout->addWidget(status);
    layout->addWidget(output);
    layout->addWidget(new QLabel(QStringLiteral("本页只验证合成领域规则；实际采集、算法和进程隔离请使用多进程工作站入口。"),&window));
    if(smoke) {
        const QString expected_states[]={QStringLiteral("Completed"),QStringLiteral("Completed"),
            QStringLiteral("Failed"),QStringLiteral("TimedOut"),QStringLiteral("Rejected")};
        const QString expected_quality[]={QStringLiteral("OK"),QStringLiteral("NG"),
            QStringLiteral("Unknown"),QStringLiteral("Unknown"),QStringLiteral("Unknown")};
        for(std::size_t n=0;n<scenario_buttons.size();++n) {
            scenario_buttons[n]->click();
            application.processEvents();
            if(!status->text().contains(expected_states[n]) ||
               !status->text().contains(QStringLiteral("质量：")+expected_quality[n]) ||
               !status->text().contains(QStringLiteral("未释放资源票据：0")) ||
               output->toPlainText().isEmpty()) return 1;
        }
        scenario_buttons[1]->click();
        window.ensurePolished();
        layout->activate();
        QImage image(window.size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        window.render(&image);
        return image.isNull() || info.inspection_available || !image.save("workstation-smoke.png") ? 1 : 0;
    }
    window.show();
    return application.exec();
}
