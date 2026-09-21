#pragma once
#include <vision/application/qt/demo_runner.hpp>
#include <QCoreApplication>
inline vision::application::qt::DemoPaths demo_paths() {
    return {QString::fromUtf8(VISION_DEMO_HOST),QString::fromUtf8(VISION_DEMO_CAMERA),
        QString::fromUtf8(VISION_DEMO_ALGORITHM),QString::fromUtf8(VISION_DEMO_DEVICE),
        QString::fromUtf8(VISION_DEMO_OUTPUT),QCoreApplication::applicationDirPath()+"/dual-camera.json"};
}
