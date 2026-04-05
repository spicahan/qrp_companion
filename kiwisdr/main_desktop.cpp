#include <QApplication>
#include <QTimer>

#include "pal.h"
#include "app.h"

int main(int argc, char **argv)
{
    QApplication qapp(argc, argv);

    const char *server = "palomar-1.proxy.kiwisdr.com:8073";
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') { server = argv[i]; break; }
    }
    printf("KiwiSDR server: %s\n", server);

    constexpr int W = 1280;
    constexpr int H = 720;

    pal::init(W, H, server);
    app::init();

    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, [&]() {
        app::tick();
    });
    timer.start(16);

    return qapp.exec();
}
