#include <QApplication>
#include <QTimer>
#include <QWidget>

#include "pal.h"
#include "app.h"

// Defined in pal_desktop.cpp
QWidget* pal_desktop_createWidget(int w, int h);

int main(int argc, char **argv)
{
    QApplication qapp(argc, argv);

    constexpr int W = 1280;
    constexpr int H = 720;

    pal::init(W, H);
    app::init();

    auto *widget = pal_desktop_createWidget(W, H);
    widget->show();

    // Drive the app loop from a Qt timer (~60 fps target)
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, [&]() {
        app::tick();
    });
    timer.start(16);

    return qapp.exec();
}
