#include <QApplication>
#include <QTimer>

#include "pal.h"
#include "app.h"

int main(int argc, char **argv)
{
    QApplication qapp(argc, argv);

    constexpr int W = 1280;
    constexpr int H = 720;

    pal::init(W, H);
    app::init();

    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, [&]() {
        app::tick();
    });
    timer.start(16);

    return qapp.exec();
}
