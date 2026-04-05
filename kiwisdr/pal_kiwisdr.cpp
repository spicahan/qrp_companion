#include "pal.h"
#include "core/mongoose/mongoose.h"

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
#include <string>
#include <cmath>

static int fb_w = 0, fb_h = 0;
static uint16_t *fb_buffer = nullptr;

static std::queue<pal::TouchEvent> evt_queue;
static std::mutex evt_mutex;

// ── Qt Widget for display ──
class FbWidget : public QWidget {
public:
    FbWidget(int w, int h) : img(w, h, QImage::Format_RGB16) {
        setFixedSize(w, h);
        setWindowTitle("QRP Companion — KiwiSDR");
    }
    void commitUpdate() {
        memcpy(img.bits(), fb_buffer, fb_w * fb_h * 2);
        update();
    }
protected:
    void paintEvent(QPaintEvent *) override { QPainter(this).drawImage(0, 0, img); }
    void mousePressEvent(QMouseEvent *e) override { pushEvt(e, pal::TouchEvent::DOWN); }
    void mouseMoveEvent(QMouseEvent *e) override { if (e->buttons()) pushEvt(e, pal::TouchEvent::MOVE); }
    void mouseReleaseEvent(QMouseEvent *e) override { pushEvt(e, pal::TouchEvent::UP); }
private:
    QImage img;
    void pushEvt(QMouseEvent *e, pal::TouchEvent::Action a) {
        std::lock_guard<std::mutex> lock(evt_mutex);
        evt_queue.push({e->x(), e->y(), a});
    }
};
static FbWidget *g_widget = nullptr;

// ── KiwiSDR WebSocket client ──
static struct mg_mgr s_kiwi_mgr;
static struct mg_connection *s_kiwi_conn = nullptr;
static std::string s_kiwi_url;
static float s_kiwi_sample_rate = 12000.0f;
static bool s_kiwi_connected = false;
static bool s_kiwi_streaming = false;
static bool s_kiwi_auth_ok = false;
static bool s_kiwi_got_rate = false;
static pal::AudioInputCallback s_audio_cb = nullptr;

// KiwiSDR I/Q ring buffer (int16 BE → float I/Q)
static constexpr int KIWI_IQ_RING = 16384;
static float s_iq_ring[KIWI_IQ_RING * 2]; // interleaved I/Q
static std::atomic<int> s_iq_head{0};
static std::atomic<int> s_iq_tail{0};

static void kiwi_send(struct mg_connection *c, const char *msg) {
    mg_ws_send(c, msg, strlen(msg), WEBSOCKET_OP_TEXT);
}

static void kiwi_handle_msg(const char *msg) {
    // Parse server text messages
    if (strstr(msg, "sample_rate=")) {
        float sr;
        if (sscanf(strstr(msg, "sample_rate="), "sample_rate=%f", &sr) == 1) {
            s_kiwi_sample_rate = sr;
            printf("[kiwi] sample_rate=%.1f\n", sr);
        }
    }
    if (strstr(msg, "badp=0")) {
        printf("[kiwi] auth OK\n");
        s_kiwi_auth_ok = true;
    }
    if (strstr(msg, "badp=1") || strstr(msg, "too_busy")) {
        printf("[kiwi] rejected: %s\n", msg);
    }
}

static void kiwi_handle_snd(const uint8_t *data, size_t len) {
    // SND binary frame: 3-byte "SND" tag already stripped by caller
    // Header: flags(1) + seq(4) + smeter(2) + gps(10) = 17 bytes
    if (len < 17) return;
    // uint8_t flags = data[0];
    const uint8_t *samples = data + 17;
    size_t sample_bytes = len - 17;
    int num_samples = (int)(sample_bytes / 4);  // 2 bytes I + 2 bytes Q per sample

    // Convert big-endian int16 I/Q to float pairs
    int head = s_iq_head.load();
    for (int i = 0; i < num_samples; i++) {
        int off = i * 4;
        int16_t raw_i = (int16_t)((samples[off] << 8) | samples[off + 1]);
        int16_t raw_q = (int16_t)((samples[off + 2] << 8) | samples[off + 3]);
        s_iq_ring[head * 2]     = raw_i / 32768.0f;
        s_iq_ring[head * 2 + 1] = raw_q / 32768.0f;
        head = (head + 1) % KIWI_IQ_RING;
    }
    s_iq_head.store(head);
}

static void kiwi_ev_handler(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_WS_OPEN) {
        printf("[kiwi] WebSocket connected\n");
        s_kiwi_connected = true;
        kiwi_send(c, "SET auth t=kiwi p=");
    }
    else if (ev == MG_EV_WS_MSG) {
        struct mg_ws_message *wm = (struct mg_ws_message *)ev_data;
        const char *d = wm->data.buf;
        size_t len = wm->data.len;

        if (len >= 3 && d[0] == 'M' && d[1] == 'S' && d[2] == 'G') {
            // Text message
            std::string msg(d, len);
            kiwi_handle_msg(msg.c_str());

            if (msg.find("sample_rate=") != std::string::npos)
                s_kiwi_got_rate = true;

            // Start streaming after both auth OK and sample_rate received
            if (!s_kiwi_streaming && s_kiwi_auth_ok && s_kiwi_got_rate) {
                s_kiwi_streaming = true;
                kiwi_send(c, "SET AR OK in=12000 out=44100");
                kiwi_send(c, "SET squelch=0 max=0");
                kiwi_send(c, "SET genattn=0");
                kiwi_send(c, "SET gen=0 mix=-1");
                kiwi_send(c, "SET mod=iq low_cut=-5000 high_cut=5000 freq=14074.000");
                kiwi_send(c, "SET agc=1 hang=0 thresh=-100 slope=6 decay=1000 manGain=50");
                kiwi_send(c, "SET compression=0");
                kiwi_send(c, "SET ident_user=QRP-Companion");
                printf("[kiwi] streaming started, freq=14074.000 kHz\n");
            }
        }
        else if (len >= 3 && d[0] == 'S' && d[1] == 'N' && d[2] == 'D') {
            // Binary SND frame
            kiwi_handle_snd((const uint8_t *)d + 3, len - 3);
        }
    }
    else if (ev == MG_EV_CLOSE) {
        printf("[kiwi] disconnected\n");
        s_kiwi_conn = nullptr;
        s_kiwi_connected = false;
        s_kiwi_streaming = false;
    }
    else if (ev == MG_EV_ERROR) {
        printf("[kiwi] error: %s\n", (char *)ev_data);
    }
}

static void kiwi_connect(const char *host, int port) {
    char url[256];
    long ts = (long)time(nullptr);
    snprintf(url, sizeof(url), "ws://%s:%d/%ld/SND", host, port, ts);
    s_kiwi_url = url;
    printf("[kiwi] connecting to %s\n", url);
    s_kiwi_conn = mg_ws_connect(&s_kiwi_mgr, url, kiwi_ev_handler, nullptr, nullptr);
}

// Drain I/Q ring → push to DSP
static void kiwi_drain_iq() {
    int head = s_iq_head.load();
    int tail = s_iq_tail.load();
    if (head == tail) return;

    // Compute available samples
    int avail = (head - tail + KIWI_IQ_RING) % KIWI_IQ_RING;
    if (avail <= 0) return;

    // Process in chunks
    float buf[1024];
    while (avail > 0) {
        int chunk = avail > 512 ? 512 : avail;
        for (int i = 0; i < chunk; i++) {
            buf[i * 2]     = s_iq_ring[tail * 2];
            buf[i * 2 + 1] = s_iq_ring[tail * 2 + 1];
            tail = (tail + 1) % KIWI_IQ_RING;
        }
        if (s_audio_cb) s_audio_cb(buf, chunk);
        avail -= chunk;
    }
    s_iq_tail.store(tail);
}

// Keepalive timer
static int64_t s_last_keepalive = 0;

static void kiwi_poll() {
    mg_mgr_poll(&s_kiwi_mgr, 0);
    kiwi_drain_iq();

    // Keepalive every second
    if (s_kiwi_conn && s_kiwi_streaming) {
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
        if (ms - s_last_keepalive > 1000) {
            kiwi_send(s_kiwi_conn, "SET keepalive");
            s_last_keepalive = ms;
        }
    }
}

// ── KiwiSDR frequency control ──
static float s_kiwi_freq_khz = 14074.0f;

static void kiwi_set_freq(uint64_t freq_hz) {
    s_kiwi_freq_khz = freq_hz / 1000.0f;
    if (s_kiwi_conn && s_kiwi_streaming) {
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "SET mod=iq low_cut=-5000 high_cut=5000 freq=%.3f", s_kiwi_freq_khz);
        kiwi_send(s_kiwi_conn, cmd);
    }
}

// ═══════════════════════════════════════════════════════════════
// PAL implementation
// ═══════════════════════════════════════════════════════════════

namespace pal {

bool init(int width, int height) {
    fb_w = width;
    fb_h = height;
    fb_buffer = new uint16_t[width * height]();
    g_widget = new FbWidget(width, height);
    g_widget->show();

    // Init KiwiSDR Mongoose client
    mg_mgr_init(&s_kiwi_mgr);
    kiwi_connect("palomar-1.proxy.kiwisdr.com", 8073);

    return true;
}

void shutdown() {}

DisplayInfo getDisplayInfo() { return {fb_w, fb_h}; }
uint16_t *getFramebuffer() { return fb_buffer; }
int getFramebufferStride() { return fb_w; }
bool isRotated() { return false; }
void commitFrame() {
    if (g_widget) g_widget->commitUpdate();
    kiwi_poll();
}

int getExtendedHeight() { return fb_h; }
void setVisibleOffset(int) {}
void blitBlock(const uint16_t *src, int src_stride, int src_x, int src_y,
               uint16_t *dst, int dst_stride, int dst_x, int dst_y,
               int width, int height, bool) {
    for (int row = 0; row < height; row++)
        memcpy(&dst[(dst_y + row) * dst_stride + dst_x],
               &src[(src_y + row) * src_stride + src_x], width * 2);
}

bool pollEvent(TouchEvent &evt) {
    std::lock_guard<std::mutex> lock(evt_mutex);
    if (evt_queue.empty()) return false;
    evt = evt_queue.front();
    evt_queue.pop();
    return true;
}

void injectEvent(const TouchEvent &evt) {
    std::lock_guard<std::mutex> lock(evt_mutex);
    evt_queue.push(evt);
}

int64_t micros() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::microseconds>(now).count();
}

void delayMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
int freeHeapKb() { return 0; }
int freePsramKb() { return 0; }

// Audio output via PortAudio (linear interpolation upsample to 48kHz)
#include <portaudio.h>

static constexpr int PA_OUT_RATE = 48000;
static constexpr int AOUT_RING_SIZE = 8192;
static float s_aout_ring[AOUT_RING_SIZE];
static std::atomic<int> s_aout_head{0};
static std::atomic<int> s_aout_tail{0};
static int s_audio_out_rate = 0;
static PaStream *s_pa_out = nullptr;

static int pa_output_callback(const void *, void *output, unsigned long frameCount,
                              const PaStreamCallbackTimeInfo *, PaStreamCallbackFlags, void *)
{
    float *out = (float *)output;
    static float prev_sample = 0, cur_sample = 0;
    static int phase = 0;
    int upsample = (s_audio_out_rate > 0) ? (PA_OUT_RATE / s_audio_out_rate) : 4;
    int head = s_aout_head.load(std::memory_order_acquire);
    int tail = s_aout_tail.load(std::memory_order_relaxed);
    for (unsigned long i = 0; i < frameCount; i++) {
        if (phase == 0) {
            prev_sample = cur_sample;
            if (head != tail) {
                cur_sample = s_aout_ring[tail];
                tail = (tail + 1) % AOUT_RING_SIZE;
            }
        }
        float t = (float)phase / (float)upsample;
        out[i] = prev_sample + (cur_sample - prev_sample) * t;
        phase = (phase + 1) % upsample;
    }
    s_aout_tail.store(tail, std::memory_order_release);
    return paContinue;
}

bool audioOutputOpen(int sample_rate)
{
    s_audio_out_rate = sample_rate;
    Pa_Initialize();
    PaStreamParameters outParams{};
    outParams.device = Pa_GetDefaultOutputDevice();
    outParams.channelCount = 1;
    outParams.sampleFormat = paFloat32;
    outParams.suggestedLatency = 0.05;
    Pa_OpenStream(&s_pa_out, nullptr, &outParams, PA_OUT_RATE, 256, 0, pa_output_callback, nullptr);
    Pa_StartStream(s_pa_out);
    return true;
}

void audioOutputWrite(const float *samples, int num_frames)
{
    int head = s_aout_head.load(std::memory_order_relaxed);
    for (int i = 0; i < num_frames; i++) {
        int next = (head + 1) % AOUT_RING_SIZE;
        if (next == s_aout_tail.load(std::memory_order_acquire)) break;
        s_aout_ring[head] = samples[i];
        head = next;
    }
    s_aout_head.store(head, std::memory_order_release);
}

void audioOutputClose()
{
    if (s_pa_out) { Pa_StopStream(s_pa_out); Pa_CloseStream(s_pa_out); s_pa_out = nullptr; }
}

// Audio input — KiwiSDR I/Q stream
bool audioInputOpen(AudioInputCallback cb) {
    s_audio_cb = cb;
    return true;
}
void audioInputClose() { s_audio_cb = nullptr; }

// CAT — translate to KiwiSDR WebSocket commands
bool catIsConnected() { return s_kiwi_streaming; }
int catSend(const char *data, int len) {
    // Parse FA command → set KiwiSDR frequency
    if (len >= 4 && data[0] == 'F' && data[1] == 'A') {
        // FA00014074000; → extract frequency
        char buf[32];
        int flen = 0;
        for (int i = 2; i < len && data[i] != ';'; i++) {
            if (data[i] >= '0' && data[i] <= '9') buf[flen++] = data[i];
        }
        buf[flen] = '\0';
        if (flen > 0) {
            uint64_t freq_hz = strtoull(buf, nullptr, 10);
            if (freq_hz > 0) kiwi_set_freq(freq_hz);
        }
    }
    return len;
}
int catRecv(char *buf, int buflen) {
    // Fake FA/MD responses from KiwiSDR state
    static int64_t last_resp = 0;
    int64_t now = micros();
    if (now - last_resp < 500000) return 0;  // throttle to 2Hz
    last_resp = now;

    // Return frequency + mode
    uint64_t freq_hz = (uint64_t)(s_kiwi_freq_khz * 1000);
    int n = snprintf(buf, buflen, "FA%011llu;MD6;", (unsigned long long)freq_hz);
    return n;
}

} // namespace pal
