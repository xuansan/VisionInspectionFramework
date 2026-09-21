#include "model_tool.hpp"
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QGraphicsView>
#include <QGraphicsScene>
#include <QWheelEvent>
#include <QImageReader>
#include <QFileDialog>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QProcess>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QCryptographicHash>
#include <QTimer>
#include <QCloseEvent>
namespace {
class View:public QGraphicsView {
public:
    using QGraphicsView::QGraphicsView;
    QImage image;QLabel* info{};
    void wheelEvent(QWheelEvent* e) override {const auto f=e->angleDelta().y()>0?1.2:1/1.2;
        if(transform().m11()*f>=.05&&transform().m11()*f<=100)scale(f,f);e->accept();}
    void mouseMoveEvent(QMouseEvent* e) override {
        const auto p=mapToScene(e->pos()).toPoint();
        if(image.rect().contains(p)&&info){const auto c=image.pixelColor(p);info->setText(QString("x=%1 y=%2 RGB=%3,%4,%5").arg(p.x()).arg(p.y()).arg(c.red()).arg(c.green()).arg(c.blue()));}
        QGraphicsView::mouseMoveEvent(e);
    }
};
}
int model_tool_window(QApplication& app,bool smoke) {
    QWidget window;window.resize(1100,800);window.setWindowTitle(QStringLiteral("模型试运行 · 离线 / 无PLC输出"));
    auto* layout=new QVBoxLayout(&window);
    auto* config=new QLineEdit(QString::fromUtf8(VISION_MODEL_CONFIG),&window);
    auto* image_path=new QLineEdit(QString::fromUtf8(VISION_MODEL_IMAGE),&window);
    auto row=[&](const QString& text,QLineEdit* field,const QString& filter) {
        auto* line=new QHBoxLayout;line->addWidget(new QLabel(text,&window));line->addWidget(field);
        auto* browse=new QPushButton(QStringLiteral("选择"),&window);line->addWidget(browse);layout->addLayout(line);
        QObject::connect(browse,&QPushButton::clicked,&window,[&,field,filter]{
            const auto path=QFileDialog::getOpenFileName(&window,QStringLiteral("选择文件"),{},filter);if(!path.isEmpty())field->setText(path);
        });
    };
    row(QStringLiteral("模型配置"),config,"JSON (*.json)");row(QStringLiteral("图像"),image_path,"PNM (*.pgm *.ppm)");
    auto* parameters=new QHBoxLayout;
    auto* score=new QDoubleSpinBox(&window);score->setRange(0,1);score->setDecimals(3);score->setValue(.25);
    parameters->addWidget(new QLabel(QStringLiteral("置信度"),&window));parameters->addWidget(score);
    QSpinBox* roi[4];const QString labels[]{"ROI X","Y","W (0=全图)","H (0=全图)"};
    for(unsigned i=0;i<4;++i){roi[i]=new QSpinBox(&window);roi[i]->setRange(0,4096);parameters->addWidget(new QLabel(labels[i],&window));parameters->addWidget(roi[i]);}
    auto* start=new QPushButton(QStringLiteral("校验并试运行"),&window);auto* cancel=new QPushButton(QStringLiteral("取消"),&window);
    parameters->addWidget(start);parameters->addWidget(cancel);layout->addLayout(parameters);
    auto* info=new QLabel(QStringLiteral("滚轮缩放，拖动平移；原始数据和生产统计不被修改。"),&window);layout->addWidget(info);
    auto* scene=new QGraphicsScene(&window);auto* view=new View(scene,&window);view->info=info;view->setMouseTracking(true);
    view->setDragMode(QGraphicsView::ScrollHandDrag);layout->addWidget(view,1);
    auto* text=new QPlainTextEdit(&window);text->setReadOnly(true);text->setMaximumBlockCount(1000);text->setMaximumHeight(180);layout->addWidget(text);
    QTemporaryDir output;QProcess process;QByteArray response,error;bool cancelled=false,loading=false;
    QObject::connect(&process,&QProcess::readyReadStandardOutput,&window,[&] {
        response+=process.readAllStandardOutput();if(response.size()>(loading?16*1024*1024+4096:1024*1024)){process.kill();text->setPlainText("Result limit exceeded");}
    });
    QObject::connect(&process,&QProcess::readyReadStandardError,&window,[&] {
        error+=process.readAllStandardError();if(error.size()>16384)error=error.right(16384);
    });
    QObject::connect(start,&QPushButton::clicked,&window,[&] {
        if(process.state()!=QProcess::NotRunning)return;
        response.clear();error.clear();cancelled=false;scene->clear();
        loading=true;start->setEnabled(false);text->setPlainText(QStringLiteral("独立进程读取并校验图像…"));
        process.start(QString::fromUtf8(VISION_MODEL_TOOL),{"--preview",image_path->text()});
    });
    auto launch_inference=[&] {
        view->image=QImage::fromData(response,"PNM");const auto size=view->image.size();
        if(!output.isValid()||size.width()<=0||size.height()<=0||
           size.width()>4096||size.height()>4096||static_cast<qint64>(size.width())*size.height()>4*1024*1024) {
            start->setEnabled(true);text->setPlainText(QStringLiteral("图像尺寸或文件大小超限"));if(smoke)app.exit(1);return;
        }
        scene->addPixmap(QPixmap::fromImage(view->image));scene->setSceneRect(view->image.rect());view->fitInView(scene->sceneRect(),Qt::KeepAspectRatio);
        const auto width=roi[2]->value()?roi[2]->value():size.width(),height=roi[3]->value()?roi[3]->value():size.height();
        if(roi[0]->value()+width>size.width()||roi[1]->value()+height>size.height()) {
            start->setEnabled(true);text->setPlainText(QStringLiteral("ROI超出图像"));if(smoke)app.exit(1);return;
        }
        QPen roi_pen(Qt::yellow);roi_pen.setCosmetic(true);roi_pen.setWidth(2);
        scene->addRect(roi[0]->value(),roi[1]->value(),width,height,roi_pen);
        // PGM/P6 type is checked again by the file-camera worker.
        const auto magic=response.left(2);
        if(magic!="P5"&&magic!="P6"){text->setPlainText("Only binary P5/P6");if(smoke)app.exit(1);return;}
        QJsonArray region;for(auto* value:roi)region.append(value->value());
        const QJsonObject request{{"config",config->text()},{"image",image_path->text()},{"width",size.width()},{"height",size.height()},
            {"channels",magic=="P5"?1:3},{"score",score->value()},{"roi",region},{"output",output.path()}};
        start->setEnabled(false);text->setPlainText(QStringLiteral("独立宿主校验、预热和推理中…"));
        response.clear();error.clear();loading=false;
        process.start(QString::fromUtf8(VISION_MODEL_TOOL),{"--request",QString::fromUtf8(QJsonDocument(request).toJson(QJsonDocument::Compact))});
    };
    QObject::connect(cancel,&QPushButton::clicked,&window,[&]{cancelled=true;process.kill();});
    QObject::connect(&process,&QProcess::errorOccurred,&window,[&](QProcess::ProcessError e) {
        if(e==QProcess::FailedToStart){start->setEnabled(true);text->setPlainText(process.errorString());if(smoke)app.exit(1);}
    });
    QObject::connect(&process,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),&window,[&](int code,QProcess::ExitStatus status) {
        start->setEnabled(true);
        if(cancelled||code||status!=QProcess::NormalExit){text->setPlainText(cancelled?QStringLiteral("已取消"):QString::fromUtf8(error));if(smoke)app.exit(1);return;}
        if(loading){launch_inference();return;}
        const auto doc=QJsonDocument::fromJson(response);const auto check=doc.object()["check"].toObject();
        if(doc.isNull()||doc.object()["mode"]!="Replay"){if(smoke)app.exit(1);return;}
        text->setPlainText(QString::fromUtf8(doc.toJson()));
        for(const auto& item:check["defects"].toArray()) {
            const auto box=item.toObject()["box_xyxy"].toArray();if(box.size()!=4)continue;
            QPen box_pen(Qt::red);box_pen.setCosmetic(true);box_pen.setWidth(2);
            scene->addRect(box[0].toDouble(),box[1].toDouble(),box[2].toDouble()-box[0].toDouble(),box[3].toDouble()-box[1].toDouble(),box_pen);
        }
        for(const auto& item:check["masks"].toArray()) {
            const auto m=item.toObject();const auto hash=m["hash"].toString();
            const int w=m["width"].toInt(),h=m["height"].toInt();
            if(!hash.startsWith("sha256:")||hash.size()!=71||w<=0||h<=0||static_cast<qint64>(w)*h>1024*1024)continue;
            QFile mask(output.path()+"/masks/"+hash.mid(7)+".mask");
            if(!mask.open(QIODevice::ReadOnly)||mask.size()!=static_cast<qint64>(w)*h)continue;
            auto bytes=mask.readAll();
            if(QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex())!=hash.mid(7))continue;
            QImage overlay(w,h,QImage::Format_ARGB32);overlay.fill(Qt::transparent);
            for(int y=0;y<h;++y)for(int x=0;x<w;++x)if(bytes[y*w+x])overlay.setPixelColor(x,y,QColor(0,255,0,90));
            scene->addPixmap(QPixmap::fromImage(overlay));
        }
        if(smoke) {
            const auto picture=window.grab().toImage();
            const auto screenshot=qEnvironmentVariable("VISION_MODEL_SCREENSHOT");
            const bool saved=screenshot.isEmpty()||picture.save(screenshot);
            app.exit(saved&&!picture.isNull()&&check["quality"]=="NG"&&scene->items().size()>=3?0:1);
        }
    });
    window.show();
    if(smoke){QTimer::singleShot(0,start,&QPushButton::click);QTimer::singleShot(22000,&app,[&]{app.exit(1);});}
    const auto result=app.exec();
    if(process.state()!=QProcess::NotRunning){process.kill();process.waitForFinished(3000);}
    return result;
}
