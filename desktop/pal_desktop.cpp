#include "pal.h"

#include <QWidget>
#include <QImage>
#include <QPainter>
#include <QMouseEvent>
#include <chrono>
#include <thread>
#include <queue>
#include <mutex>
#include <cstring>

static int fb_w = 0, fb_h = 0;
static uint16_t *fb_buffer = nullptr;

// Event queue (thread-safe in case we need it later)
static std::queue<pal::TouchEvent> evt_queue;
static std::mutex evt_mutex;

// ---- Qt Widget that displays the framebuffer ----

class DisplayWidget : public QWidget {
public:
    DisplayWidget(int w, int h, QWidget *parent = nullptr)
        : QWidget(parent), img_w(w), img_h(h)
    {
        setFixedSize(w, h);
        setWindowTitle("QRP Companion (Desktop)");
        setMouseTracking(false);
    }

    void commitUpdate() { update(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        if (!fb_buffer) return;

        // QImage::Format_RGB16 is native-endian RGB565
        QImage img((const uchar *)fb_buffer, img_w, img_h,
                   img_w * 2, QImage::Format_RGB16);
        QPainter p(this);
        p.drawImage(0, 0, img);
    }

    void mousePressEvent(QMouseEvent *e) override
    {
        std::lock_guard<std::mutex> lock(evt_mutex);
        evt_queue.push({(int)e->x(), (int)e->y(), pal::TouchEvent::DOWN});
    }

    void mouseReleaseEvent(QMouseEvent *e) override
    {
        std::lock_guard<std::mutex> lock(evt_mutex);
        evt_queue.push({(int)e->x(), (int)e->y(), pal::TouchEvent::UP});
    }

    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (e->buttons() & Qt::LeftButton) {
            std::lock_guard<std::mutex> lock(evt_mutex);
            evt_queue.push({(int)e->x(), (int)e->y(), pal::TouchEvent::MOVE});
        }
    }

private:
    int img_w, img_h;
};

static DisplayWidget *g_widget = nullptr;

// Monotonic clock base
static auto clock_base = std::chrono::steady_clock::now();

namespace pal {

bool init(int width, int height)
{
    fb_w = width;
    fb_h = height;
    fb_buffer = new uint16_t[width * height]();
    clock_base = std::chrono::steady_clock::now();
    return true;
}

void shutdown()
{
    delete[] fb_buffer;
    fb_buffer = nullptr;
}

DisplayInfo getDisplayInfo()
{
    return { fb_w, fb_h };
}

uint16_t* getFramebuffer()
{
    return fb_buffer;
}

void commitFrame()
{
    if (g_widget) g_widget->commitUpdate();
}

bool pollEvent(TouchEvent &evt)
{
    std::lock_guard<std::mutex> lock(evt_mutex);
    if (evt_queue.empty()) return false;
    evt = evt_queue.front();
    evt_queue.pop();
    return true;
}

int64_t micros()
{
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(now - clock_base).count();
}

void delayMs(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

int freeHeapKb() { return 0; }
int freePsramKb() { return 0; }

static const char empty[] = "";
const char* debugLine1() { return empty; }
const char* debugLine2() { return empty; }

} // namespace pal

// Expose widget creation for main
QWidget* pal_desktop_createWidget(int w, int h)
{
    g_widget = new DisplayWidget(w, h);
    return g_widget;
}
