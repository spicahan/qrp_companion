#include "pal.h"

#include <portaudio.h>
#include <QWidget>
#include <QImage>
#include <QPainter>
#include <QMouseEvent>
#include <chrono>
#include <thread>
#include <queue>
#include <mutex>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <atomic>

// POSIX serial for CAT
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <glob.h>

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

// --- Desktop CAT forward declarations (defined below getVfoFreq) ---
static int s_cat_fd = -1;
static std::thread s_cat_thread;
static std::atomic<bool> s_cat_running{false};
static void cat_thread_func();

bool init(int width, int height)
{
    fb_w = width;
    fb_h = height;
    fb_buffer = new uint16_t[width * height]();
    clock_base = std::chrono::steady_clock::now();

    // Start CAT serial thread
    s_cat_running = true;
    s_cat_thread = std::thread(cat_thread_func);
    s_cat_thread.detach();

    return true;
}

void shutdown()
{
    s_cat_running = false;
    if (s_cat_fd >= 0) { close(s_cat_fd); s_cat_fd = -1; }
    delete[] fb_buffer;
    fb_buffer = nullptr;
}

DisplayInfo getDisplayInfo()
{
    return { fb_w, fb_h };
}

uint16_t* getFramebuffer() { return fb_buffer; }
int getFramebufferStride() { return fb_w; }
bool isRotated() { return false; }

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

// --- Desktop CAT via POSIX serial (implementation) ---

static int try_open_cat_port(const char *path)
{
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;

    // Clear non-blocking for subsequent I/O
    int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    struct termios tty = {};
    tcgetattr(fd, &tty);
    cfsetispeed(&tty, B115200);
    cfsetospeed(&tty, B115200);
    tty.c_cflag = CS8 | CLOCAL | CREAD;
    tty.c_iflag = 0;
    tty.c_oflag = 0;
    tty.c_lflag = 0;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 5;  // 500ms read timeout
    tcsetattr(fd, TCSANOW, &tty);
    tcflush(fd, TCIOFLUSH);

    // Probe: send FA; and check for response
    write(fd, "FA;", 3);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    char buf[64];
    int n = read(fd, buf, sizeof(buf) - 1);
    if (n > 4 && buf[0] == 'F' && buf[1] == 'A') {
        buf[n] = '\0';
        fprintf(stderr, "CAT: found on %s: %s\n", path, buf);
        return fd;
    }

    close(fd);
    return -1;
}

static void cat_thread_func()
{
    // Auto-detect and maintain CAT serial port connection
    while (s_cat_running) {
        if (s_cat_fd < 0) {
            glob_t gl;
            if (glob("/dev/cu.usbmodem*", 0, nullptr, &gl) == 0) {
                for (size_t i = 0; i < gl.gl_pathc && s_cat_fd < 0; i++)
                    s_cat_fd = try_open_cat_port(gl.gl_pathv[i]);
                globfree(&gl);
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

// --- PAL CAT serial transport ---
static std::mutex s_cat_rx_mutex;
static char s_cat_rx_ring[256];
static int s_cat_rx_head = 0, s_cat_rx_tail = 0;

bool catIsConnected() { return s_cat_fd >= 0; }

int catSend(const char *data, int len)
{
    if (s_cat_fd < 0) return -1;
    int n = (int)write(s_cat_fd, data, len);
    return n > 0 ? n : -1;
}

int catRecv(char *buf, int max_len)
{
    // Drain from serial port into ring buffer
    if (s_cat_fd >= 0) {
        char tmp[128];
        int n = (int)read(s_cat_fd, tmp, sizeof(tmp));
        if (n > 0) {
            std::lock_guard<std::mutex> lock(s_cat_rx_mutex);
            for (int i = 0; i < n; i++) {
                int next = (s_cat_rx_head + 1) % 256;
                if (next != s_cat_rx_tail) {
                    s_cat_rx_ring[s_cat_rx_head] = tmp[i];
                    s_cat_rx_head = next;
                }
            }
        } else if (n < 0) {
            // Port error
            close(s_cat_fd);
            s_cat_fd = -1;
        }
    }

    // Return data from ring buffer
    std::lock_guard<std::mutex> lock(s_cat_rx_mutex);
    int count = 0;
    while (count < max_len && s_cat_rx_head != s_cat_rx_tail) {
        buf[count++] = s_cat_rx_ring[s_cat_rx_tail];
        s_cat_rx_tail = (s_cat_rx_tail + 1) % 256;
    }
    return count;
}

void blitBlock(const uint16_t *src, int src_stride, int src_x, int src_y,
               uint16_t *dst, int dst_stride, int dst_x, int dst_y,
               int width, int height, bool mirror_y)
{
    for (int y = 0; y < height; y++) {
        int sy = mirror_y ? (src_y + height - 1 - y) : (src_y + y);
        memcpy(&dst[(dst_y + y) * dst_stride + dst_x],
               &src[sy * src_stride + src_x],
               width * sizeof(uint16_t));
    }
}

// --- Audio input via PortAudio ---
static PaStream *s_pa_stream = nullptr;
static AudioInputCallback s_audio_cb = nullptr;

// PortAudio callback: receives interleaved stereo float, passes through as I/Q
static int pa_input_callback(const void *input, void * /*output*/,
                             unsigned long frameCount,
                             const PaStreamCallbackTimeInfo * /*timeInfo*/,
                             PaStreamCallbackFlags /*statusFlags*/,
                             void * /*userData*/)
{
    if (!s_audio_cb || !input) return paContinue;
    // Input is interleaved stereo float [L0,R0,L1,R1,...] = [I0,Q0,I1,Q1,...]
    s_audio_cb((const float *)input, (int)frameCount);
    return paContinue;
}

bool audioInputOpen(AudioInputCallback cb)
{
    PaError err = Pa_Initialize();
    if (err != paNoError) {
        fprintf(stderr, "PortAudio init failed: %s\n", Pa_GetErrorText(err));
        return false;
    }

    // Find default input device
    PaDeviceIndex dev = Pa_GetDefaultInputDevice();
    if (dev == paNoDevice) {
        fprintf(stderr, "No audio input device found\n");
        Pa_Terminate();
        return false;
    }

    const PaDeviceInfo *info = Pa_GetDeviceInfo(dev);
    fprintf(stderr, "Audio input: %s (%.0f Hz max, %d ch)\n",
            info->name, info->defaultSampleRate, info->maxInputChannels);

    PaStreamParameters params = {};
    params.device = dev;
    params.channelCount = 2;       // stereo
    params.sampleFormat = paFloat32; // let PortAudio convert from device native format
    params.suggestedLatency = info->defaultLowInputLatency;

    s_audio_cb = cb;

    err = Pa_OpenStream(&s_pa_stream, &params, nullptr,
                        48000.0, 1024, paClipOff,
                        pa_input_callback, nullptr);
    if (err != paNoError) {
        fprintf(stderr, "Pa_OpenStream failed: %s\n", Pa_GetErrorText(err));
        s_audio_cb = nullptr;
        Pa_Terminate();
        return false;
    }

    err = Pa_StartStream(s_pa_stream);
    if (err != paNoError) {
        fprintf(stderr, "Pa_StartStream failed: %s\n", Pa_GetErrorText(err));
        Pa_CloseStream(s_pa_stream);
        s_pa_stream = nullptr;
        s_audio_cb = nullptr;
        Pa_Terminate();
        return false;
    }

    fprintf(stderr, "Audio input started: 48kHz stereo float32\n");
    return true;
}

void audioInputClose()
{
    if (s_pa_stream) {
        Pa_StopStream(s_pa_stream);
        Pa_CloseStream(s_pa_stream);
        s_pa_stream = nullptr;
    }
    s_audio_cb = nullptr;
    Pa_Terminate();
}

} // namespace pal

// Expose widget creation for main
QWidget* pal_desktop_createWidget(int w, int h)
{
    g_widget = new DisplayWidget(w, h);
    return g_widget;
}
