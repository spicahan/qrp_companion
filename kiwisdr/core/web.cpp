#include "web.h"
#include "mongoose/mongoose.h"
#include "web_ui_html.h"
#include <cstring>
#include <cstdio>

static struct mg_mgr s_mgr;
static struct mg_connection *s_ws_clients[4];  // max 4 simultaneous clients
static int s_num_clients = 0;

static web::TouchCallback s_touch_cb = nullptr;
static web::ButtonCallback s_button_cb = nullptr;

// Track clients
static void add_client(struct mg_connection *c)
{
    for (int i = 0; i < 4; i++) {
        if (!s_ws_clients[i]) { s_ws_clients[i] = c; s_num_clients++; return; }
    }
}

static void remove_client(struct mg_connection *c)
{
    for (int i = 0; i < 4; i++) {
        if (s_ws_clients[i] == c) { s_ws_clients[i] = nullptr; s_num_clients--; return; }
    }
}

// Handle WebSocket messages from client
static void handle_ws_message(struct mg_connection *c, struct mg_ws_message *wm)
{
    const uint8_t *data = (const uint8_t *)wm->data.buf;
    size_t len = wm->data.len;
    if (len < 1) return;

    uint8_t type = data[0];

    if (type == 0x10 && len >= 9) {
        // Ping — echo back immediately for RTT measurement
        mg_ws_send(c, (const char *)data, len, WEBSOCKET_OP_BINARY);
        // Change type to pong
        uint8_t pong = 0x11;
        // Send pong with same timestamp
        uint8_t buf[9];
        buf[0] = 0x11;
        memcpy(buf + 1, data + 1, 8);
        mg_ws_send(c, (const char *)buf, 9, WEBSOCKET_OP_BINARY);
    }
    else if (type == 0x20 && len >= 7 && s_touch_cb) {
        // Touch: action(1) + norm_x(2) + norm_y(2)  (0-10000 fixed point)
        int action = data[1];
        int nx = data[2] | (data[3] << 8);  // 0-10000
        int ny = data[4] | (data[5] << 8);  // 0-10000
        s_touch_cb(action, nx, ny, 10000, 10000);
    }
    else if (type == 0x30 && len > 1 && s_button_cb) {
        // Button: name string (not null-terminated)
        char id[32];
        int slen = (int)(len - 1);
        if (slen > 31) slen = 31;
        memcpy(id, data + 1, slen);
        id[slen] = '\0';
        s_button_cb(id);
    }
}

// Mongoose event handler
static void ev_handler(struct mg_connection *c, int ev, void *ev_data)
{
    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;
        if (mg_match(hm->uri, mg_str("/ws"), NULL)) {
            mg_ws_upgrade(c, hm, NULL);
            add_client(c);
        } else {
            // Serve the embedded HTML for any request
            mg_http_reply(c, 200, "Content-Type: text/html\r\n",
                          "%s", WEB_UI_HTML);
        }
    }
    else if (ev == MG_EV_WS_MSG) {
        handle_ws_message(c, (struct mg_ws_message *)ev_data);
    }
    else if (ev == MG_EV_CLOSE) {
        remove_client(c);
    }
}

void web::init(int port)
{
    mg_mgr_init(&s_mgr);
    char url[32];
    snprintf(url, sizeof(url), "http://0.0.0.0:%d", port);
    mg_http_listen(&s_mgr, url, ev_handler, NULL);
    memset(s_ws_clients, 0, sizeof(s_ws_clients));
    s_num_clients = 0;
    printf("Web server: http://0.0.0.0:%d/\n", port);
}

void web::poll()
{
    mg_mgr_poll(&s_mgr, 0);  // non-blocking
}

void web::sendSpectrum(const uint8_t *bins, int count,
                       int span_rate, uint64_t vfo_freq, int mode, bool apf)
{
    if (s_num_clients == 0) return;

    // Frame: type(1) + nbins(2) + rate(4) + vfo(8) + mode(1) + apf(1) + pad(6) + bins
    static const int HDR = 23;
    uint8_t buf[23 + 512];
    if (count > 512) count = 512;

    buf[0] = 0x01;
    buf[1] = count & 0xFF;
    buf[2] = (count >> 8) & 0xFF;
    memcpy(buf + 3, &span_rate, 4);
    memcpy(buf + 7, &vfo_freq, 8);
    buf[15] = (uint8_t)mode;
    buf[16] = apf ? 1 : 0;
    memset(buf + 17, 0, 6);  // padding
    memcpy(buf + HDR, bins, count);

    for (int i = 0; i < 4; i++) {
        if (s_ws_clients[i]) {
            mg_ws_send(s_ws_clients[i], (const char *)buf, HDR + count,
                       WEBSOCKET_OP_BINARY);
        }
    }
}

void web::setTouchCallback(TouchCallback cb) { s_touch_cb = cb; }
void web::setButtonCallback(ButtonCallback cb) { s_button_cb = cb; }
bool web::hasClients() { return s_num_clients > 0; }
