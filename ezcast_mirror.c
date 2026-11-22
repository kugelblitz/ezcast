/*
 * ezcast_mirror.c
 * Single-threaded X11 screen mirror to EZCast / AM8269D dongle (1:1 capture & output)
 *
 * Compile:
 *   gcc -O3 -march=native ezcast_mirror.c -o ezcast_mirror -lX11 -lXext -lXfixes -lturbojpeg
 *
 * Run:
 *   ./ezcast_mirror [-f MAX_FPS] [-q QUALITY] [-d] <DONGLE_IP> <X> <Y> <W> <H>
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xfixes.h>
#include <turbojpeg.h>

/* =====================================================================
 * Protocol & Keepalive Constants
 * ===================================================================== */
#define KEEPALIVE_FRAME_NS 1000000000ULL

#define PORT_CONTROL       63630
#define PORT_STREAM        2425

#define PICO_PIC_FORMAT_CMD  0x02
#define PICO_HEARTBEAT       0x04
#define PICO_REQUEST         0x06
#define PICO_NOTIFICATION    0x07

#define FLAG_PC_TO_DPF       0
#define FLAG_DPF_TO_PC       1

#define OUT_PIX_FMT_JPEG     1
#define REQUEST_STREAM       1
#define REQUEST_RESULT_ALLOW 1

#define MAX_CURSOR_DIM       256
#define RX_BUF_CAP           16384

static volatile sig_atomic_t g_running = 1;

static void handle_sigint(int sig) {
    (void)sig;
    g_running = 0;
}

static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void sleep_ns(uint64_t ns) {
    if (ns == 0) return;
    struct timespec req;
    req.tv_sec = (time_t)(ns / 1000000000ULL);
    req.tv_nsec = (long)(ns % 1000000000ULL);
    while (nanosleep(&req, &req) == -1 && errno == EINTR && g_running) {
    }
}

static ssize_t send_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;
    while (sent < len && g_running) {
        ssize_t n = send(fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        sent += (size_t)n;
    }
    return (ssize_t)sent;
}

static inline void write_le32(uint8_t *dst, uint32_t v) {
    dst[0] = (uint8_t)(v & 0xFF);
    dst[1] = (uint8_t)((v >> 8) & 0xFF);
    dst[2] = (uint8_t)((v >> 16) & 0xFF);
    dst[3] = (uint8_t)((v >> 24) & 0xFF);
}

static inline uint32_t read_le32(const uint8_t *src) {
    return (uint32_t)src[0] |
           ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) |
           ((uint32_t)src[3] << 24);
}

static void generate_uuid_v4(char out[37]) {
    uint8_t b[16];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t r = read(fd, b, sizeof(b));
        close(fd);
        if (r != (ssize_t)sizeof(b)) {
            for (int i = 0; i < 16; i++) b[i] = (uint8_t)rand();
        }
    } else {
        for (int i = 0; i < 16; i++) b[i] = (uint8_t)rand();
    }
    b[6] = (b[6] & 0x0f) | 0x40;
    b[8] = (b[8] & 0x3f) | 0x80;
    snprintf(out, 37,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

/* =====================================================================
 * 1. Control Channel (TCP Port 63630 - JSON-RPC, Non-Blocking RX Buffer)
 * ===================================================================== */
typedef struct {
    int     sock;
    int     msg_id;
    bool    role_assigned;
    bool    stream_allowed;
    bool    closed;
    uint8_t rx_buf[RX_BUF_CAP];
    size_t  rx_len;
} EZCastControl;

static void control_mark_closed(EZCastControl *ctrl) {
    if (ctrl->sock >= 0) {
        shutdown(ctrl->sock, SHUT_RDWR);
        close(ctrl->sock);
        ctrl->sock = -1;
    }
    ctrl->closed = true;
}

static bool control_send_raw(EZCastControl *ctrl, const char *text) {
    if (ctrl->closed || ctrl->sock < 0) return false;
    uint32_t len = (uint32_t)strlen(text);
    uint8_t hdr[4];
    write_le32(hdr, len);
    if (send_all(ctrl->sock, hdr, 4) != 4) return false;
    if (send_all(ctrl->sock, text, len) != (ssize_t)len) return false;
    return true;
}

static void control_handle_packet(EZCastControl *ctrl, const char *json, uint32_t len) {
    (void)len;
    if (strncmp(json, "JSONRPC", 7) != 0) return;

    const char *method_pos = strstr(json, "\"method\"");
    if (method_pos != NULL) {
        int rpc_id = -1;
        const char *id_pos = strstr(json, "\"id\"");
        if (id_pos) {
            const char *colon = strchr(id_pos, ':');
            if (colon) rpc_id = atoi(colon + 1);
        }

        if (strstr(json, "\"common.assign_role\"") != NULL) {
            if (rpc_id >= 0) {
                char reply[128];
                snprintf(reply, sizeof(reply),
                         "6:{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":0}", rpc_id);
                control_send_raw(ctrl, reply);
            }
            ctrl->role_assigned = true;
            printf("[Control] Assigned role acknowledged.\n");
        } else if (rpc_id >= 0) {
            char reply[128];
            snprintf(reply, sizeof(reply),
                     "6:{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":0}", rpc_id);
            control_send_raw(ctrl, reply);
        }
    }

    if (strstr(json, "\"allow\"") != NULL) {
        ctrl->stream_allowed = true;
        printf("[Control] JSON-RPC request_stream ALLOWED.\n");
    }
}

static bool control_poll(EZCastControl *ctrl, int timeout_ms) {
    if (ctrl->closed || ctrl->sock < 0) return true;

    if (timeout_ms > 0) {
        struct pollfd pfd;
        pfd.fd = ctrl->sock;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr < 0) {
            if (errno == EINTR) return true;
            fprintf(stderr, "\n[Control] poll error: %s\n", strerror(errno));
            return false;
        }
        if (pr == 0) return true;
    }

    while (g_running && !ctrl->closed) {
        if (ctrl->rx_len >= sizeof(ctrl->rx_buf) - 1) {
            fprintf(stderr, "\n[Control] RX buffer overflow.\n");
            return false;
        }

        ssize_t n = recv(ctrl->sock,
                         ctrl->rx_buf + ctrl->rx_len,
                         sizeof(ctrl->rx_buf) - 1 - ctrl->rx_len,
                         MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            if (ctrl->stream_allowed && (errno == ECONNRESET || errno == EPIPE)) {
                printf("\n[Control] Peer closed control channel (normal once streaming).\n");
                control_mark_closed(ctrl);
                return true;
            }
            fprintf(stderr, "\n[Control] recv error: %s\n", strerror(errno));
            return false;
        }
        if (n == 0) {
            printf("\n[Control] Peer closed control channel (normal once streaming).\n");
            control_mark_closed(ctrl);
            return true;
        }
        ctrl->rx_len += (size_t)n;
    }

    size_t offset = 0;
    while (ctrl->rx_len - offset >= 4) {
        uint32_t pkt_len = read_le32(ctrl->rx_buf + offset);
        if (pkt_len > sizeof(ctrl->rx_buf) - 5) {
            fprintf(stderr, "\n[Control] Invalid packet length: %u\n", pkt_len);
            return false;
        }
        if (ctrl->rx_len - offset < 4 + (size_t)pkt_len) {
            break;
        }

        char saved = (char)ctrl->rx_buf[offset + 4 + pkt_len];
        ctrl->rx_buf[offset + 4 + pkt_len] = '\0';
        control_handle_packet(ctrl, (const char *)(ctrl->rx_buf + offset + 4), pkt_len);
        ctrl->rx_buf[offset + 4 + pkt_len] = saved;

        offset += 4 + (size_t)pkt_len;
    }

    if (offset > 0) {
        if (offset < ctrl->rx_len) {
            memmove(ctrl->rx_buf, ctrl->rx_buf + offset, ctrl->rx_len - offset);
        }
        ctrl->rx_len -= offset;
    }

    return true;
}

static bool control_connect(EZCastControl *ctrl, const char *ip) {
    memset(ctrl, 0, sizeof(*ctrl));
    ctrl->sock = socket(AF_INET, SOCK_STREAM, 0);
    if (ctrl->sock < 0) {
        perror("[Control] socket");
        ctrl->closed = true;
        return false;
    }
    int flag = 1;
    setsockopt(ctrl->sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT_CONTROL);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        fprintf(stderr, "[Control] Invalid IP: %s\n", ip);
        control_mark_closed(ctrl);
        return false;
    }

    printf("[Control] Connecting to %s:%d...\n", ip, PORT_CONTROL);
    if (connect(ctrl->sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[Control] connect");
        control_mark_closed(ctrl);
        return false;
    }

    char uuid_str[37];
    generate_uuid_v4(uuid_str);

    char msg[512];
    ctrl->msg_id++;
    snprintf(msg, sizeof(msg),
             "6:{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"common.set_device_description\","
             "\"params\":{\"capability\":{\"host_control\":{\"Version\":2}},"
             "\"hostname\":\"CMirrorClient\",\"unique_identifier\":\"%s\"}}",
             ctrl->msg_id, uuid_str);
    if (!control_send_raw(ctrl, msg)) return false;

    uint64_t deadline = now_ns() + 2000000000ULL;
    while (!ctrl->role_assigned && !ctrl->closed && g_running && now_ns() < deadline) {
        if (!control_poll(ctrl, 50)) return false;
    }

    if (!ctrl->closed) {
        if (!control_send_raw(ctrl, "1:100")) return false;
        sleep_ns(300000000ULL);
        if (!control_poll(ctrl, 0)) return false;
    }

    if (!ctrl->closed) {
        ctrl->msg_id++;
        snprintf(msg, sizeof(msg),
                 "6:{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"common.request_stream\"}",
                 ctrl->msg_id);
        if (!control_send_raw(ctrl, msg)) return false;
        if (!control_poll(ctrl, 50)) return false;
    }

    return true;
}

static void control_close(EZCastControl *ctrl) {
    control_mark_closed(ctrl);
}

/* =====================================================================
 * 2. PICO Streaming Channel (TCP Port 2425 - Non-Blocking RX Buffer)
 * ===================================================================== */
typedef struct {
    int      sock;
    uint32_t seq;
    uint64_t last_hb_ns;
    bool     allowed;
    uint8_t  rx_buf[RX_BUF_CAP];
    size_t   rx_len;
} EZDisplayStream;

static bool stream_send_pico(EZDisplayStream *st, uint32_t cmd,
                             const uint8_t *cdb, uint8_t cdb_len,
                             const uint8_t *payload, uint32_t payload_len) {
    uint8_t hdr[32];
    memset(hdr, 0, sizeof(hdr));

    st->seq++;
    uint32_t pkt_len = 24 + payload_len;

    write_le32(hdr + 0, st->seq);
    write_le32(hdr + 4, pkt_len);
    write_le32(hdr + 8, cmd);
    hdr[12] = FLAG_PC_TO_DPF;
    hdr[13] = cdb_len;
    if (cdb && cdb_len > 0) {
        memcpy(hdr + 16, cdb, cdb_len > 16 ? 16 : cdb_len);
    }

    if (send_all(st->sock, hdr, 32) != 32) return false;
    if (payload && payload_len > 0) {
        if (send_all(st->sock, payload, payload_len) != (ssize_t)payload_len) return false;
    }
    return true;
}

static bool stream_send_jpeg(EZDisplayStream *st, uint32_t w, uint32_t h,
                             const uint8_t *jpg, uint32_t jpg_len) {
    uint8_t cdb[16];
    write_le32(cdb + 0, OUT_PIX_FMT_JPEG);
    write_le32(cdb + 4, w);
    write_le32(cdb + 8, h);
    write_le32(cdb + 12, jpg_len);
    return stream_send_pico(st, PICO_PIC_FORMAT_CMD, cdb, 16, jpg, jpg_len);
}

static bool stream_handle_packet(EZDisplayStream *st, const uint8_t *body, uint32_t pkt_len) {
    if (pkt_len < 24) return true;

    uint32_t cmd = read_le32(body + 0);
    uint8_t flag = body[4];
    uint32_t d1 = read_le32(body + 8);
    uint32_t d2 = read_le32(body + 12);
    uint32_t d3 = read_le32(body + 16);
    uint32_t d4 = read_le32(body + 20);

    if (flag == FLAG_DPF_TO_PC) {
        if (cmd == PICO_REQUEST) {
            if (d4 == REQUEST_RESULT_ALLOW) {
                printf("[Stream] PICO_REQUEST allowed (count=%u, pos=%u)!\n", d2, d3);
                st->allowed = true;
            } else {
                fprintf(stderr, "\n[Stream] PICO_REQUEST denied (code %u).\n", d4);
                return false;
            }
        } else if (cmd == PICO_NOTIFICATION && d1 == 3) {
            uint8_t cdb[16];
            write_le32(cdb + 0, REQUEST_STREAM);
            write_le32(cdb + 4, d2);
            write_le32(cdb + 8, d3);
            write_le32(cdb + 12, 0);
            if (!stream_send_pico(st, PICO_REQUEST, cdb, 16, NULL, 0)) return false;
        }
    }
    return true;
}

static bool stream_poll_and_heartbeat(EZDisplayStream *st, int timeout_ms) {
    if (timeout_ms > 0) {
        struct pollfd pfd;
        pfd.fd = st->sock;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr < 0) {
            if (errno == EINTR) return true;
            fprintf(stderr, "\n[Stream] poll error: %s\n", strerror(errno));
            return false;
        }
    }

    while (g_running) {
        if (st->rx_len >= sizeof(st->rx_buf)) {
            fprintf(stderr, "\n[Stream] RX buffer overflow.\n");
            return false;
        }

        ssize_t n = recv(st->sock,
                         st->rx_buf + st->rx_len,
                         sizeof(st->rx_buf) - st->rx_len,
                         MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            fprintf(stderr, "\n[Stream] recv error: %s\n", strerror(errno));
            return false;
        }
        if (n == 0) {
            fprintf(stderr, "\n[Stream] Peer closed stream connection (EOF).\n");
            return false;
        }
        st->rx_len += (size_t)n;
    }

    size_t offset = 0;
    while (st->rx_len - offset >= 8) {
        uint32_t pkt_len = read_le32(st->rx_buf + offset + 4);
        if (pkt_len > sizeof(st->rx_buf) - 8) {
            fprintf(stderr, "\n[Stream] Invalid packet length: %u\n", pkt_len);
            return false;
        }
        if (st->rx_len - offset < 8 + (size_t)pkt_len) {
            break;
        }

        if (!stream_handle_packet(st, st->rx_buf + offset + 8, pkt_len)) {
            return false;
        }
        offset += 8 + (size_t)pkt_len;
    }

    if (offset > 0) {
        if (offset < st->rx_len) {
            memmove(st->rx_buf, st->rx_buf + offset, st->rx_len - offset);
        }
        st->rx_len -= offset;
    }

    uint64_t now = now_ns();
    if (now - st->last_hb_ns >= 3000000000ULL) {
        if (!stream_send_pico(st, PICO_HEARTBEAT, NULL, 0, NULL, 0)) {
            fprintf(stderr, "\n[Stream] Failed to send heartbeat.\n");
            return false;
        }
        st->last_hb_ns = now;
    }
    return true;
}

static bool stream_connect(EZDisplayStream *st, const char *ip) {
    memset(st, 0, sizeof(*st));
    st->sock = socket(AF_INET, SOCK_STREAM, 0);
    if (st->sock < 0) {
        perror("[Stream] socket");
        return false;
    }

    int flag = 1;
    setsockopt(st->sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    int sndbuf = 512 * 1024;
    setsockopt(st->sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT_STREAM);
    inet_pton(AF_INET, ip, &addr.sin_addr);

    printf("[Stream] Connecting to %s:%d...\n", ip, PORT_STREAM);
    if (connect(st->sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[Stream] connect");
        return false;
    }

    st->last_hb_ns = now_ns();

    uint8_t cdb[16];
    write_le32(cdb + 0, REQUEST_STREAM);
    write_le32(cdb + 4, 0);
    write_le32(cdb + 8, 0);
    write_le32(cdb + 12, 0);
    if (!stream_send_pico(st, PICO_REQUEST, cdb, 16, NULL, 0)) return false;

    uint64_t deadline = now_ns() + 5000000000ULL;
    while (!st->allowed && g_running && now_ns() < deadline) {
        if (!stream_poll_and_heartbeat(st, 50)) return false;
    }
    return st->allowed;
}

static void stream_close(EZDisplayStream *st) {
    if (st->sock >= 0) {
        shutdown(st->sock, SHUT_RDWR);
        close(st->sock);
        st->sock = -1;
    }
}

/* =====================================================================
 * 3. Event-Driven XFixes Cursor Cache & Compositor
 * ===================================================================== */
typedef struct {
    bool     valid;
    int      width;
    int      height;
    int      xhot;
    int      yhot;
    uint32_t pixels[MAX_CURSOR_DIM * MAX_CURSOR_DIM];
    uint32_t backup[MAX_CURSOR_DIM * MAX_CURSOR_DIM];
    int      saved_x;
    int      saved_y;
    int      saved_w;
    int      saved_h;
    bool     has_backup;
} CachedCursor;

static void cursor_update_image(Display *dpy, CachedCursor *cc) {
    XFixesCursorImage *img = XFixesGetCursorImage(dpy);
    if (!img) {
        cc->valid = false;
        return;
    }
    int w = img->width > MAX_CURSOR_DIM ? MAX_CURSOR_DIM : img->width;
    int h = img->height > MAX_CURSOR_DIM ? MAX_CURSOR_DIM : img->height;
    cc->width = w;
    cc->height = h;
    cc->xhot = img->xhot;
    cc->yhot = img->yhot;

    for (int y = 0; y < h; y++) {
        const unsigned long *src_row = img->pixels + (size_t)y * (size_t)img->width;
        uint32_t *dst_row = cc->pixels + (size_t)y * (size_t)w;
        for (int x = 0; x < w; x++) {
            dst_row[x] = (uint32_t)src_row[x];
        }
    }
    XFree(img);
    cc->valid = true;
}

static void cursor_composite_to_shm(CachedCursor *cc, uint8_t *shm_data,
                                    int buf_w, int buf_h, int stride_bytes,
                                    int cur_x, int cur_y) {
    cc->has_backup = false;
    if (!cc->valid || cc->width <= 0 || cc->height <= 0) return;

    int dst_x0 = cur_x - cc->xhot;
    int dst_y0 = cur_y - cc->yhot;

    int start_x = dst_x0 < 0 ? -dst_x0 : 0;
    int start_y = dst_y0 < 0 ? -dst_y0 : 0;
    int end_x = (dst_x0 + cc->width > buf_w) ? (buf_w - dst_x0) : cc->width;
    int end_y = (dst_y0 + cc->height > buf_h) ? (buf_h - dst_y0) : cc->height;

    if (start_x >= end_x || start_y >= end_y) return;

    int patch_w = end_x - start_x;
    int patch_h = end_y - start_y;
    cc->saved_x = dst_x0 + start_x;
    cc->saved_y = dst_y0 + start_y;
    cc->saved_w = patch_w;
    cc->saved_h = patch_h;
    cc->has_backup = true;

    for (int py = 0; py < patch_h; py++) {
        int cy = start_y + py;
        int sy = cc->saved_y + py;
        uint32_t *fb_row = (uint32_t *)(shm_data + (size_t)sy * (size_t)stride_bytes) + cc->saved_x;
        uint32_t *bk_row = cc->backup + (size_t)py * (size_t)patch_w;
        const uint32_t *cur_row = cc->pixels + (size_t)cy * (size_t)cc->width + start_x;

        memcpy(bk_row, fb_row, (size_t)patch_w * sizeof(uint32_t));

        for (int px = 0; px < patch_w; px++) {
            uint32_t c = cur_row[px];
            uint32_t a = c >> 24;
            if (a == 0) continue;
            if (a == 255) {
                fb_row[px] = c;
                continue;
            }
            uint32_t inv_a = 255 - a;
            uint32_t dst = fb_row[px];

            uint32_t sb = (c >>  0) & 0xFF;
            uint32_t sg = (c >>  8) & 0xFF;
            uint32_t sr = (c >> 16) & 0xFF;

            uint32_t db = (dst >>  0) & 0xFF;
            uint32_t dg = (dst >>  8) & 0xFF;
            uint32_t dr = (dst >> 16) & 0xFF;

            uint32_t ob = sb + ((db * inv_a + 127) >> 8);
            uint32_t og = sg + ((dg * inv_a + 127) >> 8);
            uint32_t or_ = sr + ((dr * inv_a + 127) >> 8);

            if (ob > 255) ob = 255;
            if (og > 255) og = 255;
            if (or_ > 255) or_ = 255;

            fb_row[px] = ob | (og << 8) | (or_ << 16) | 0xFF000000U;
        }
    }
}

static inline void cursor_restore_shm(CachedCursor *cc, uint8_t *shm_data, int stride_bytes) {
    if (!cc->has_backup) return;
    for (int py = 0; py < cc->saved_h; py++) {
        int sy = cc->saved_y + py;
        uint32_t *fb_row = (uint32_t *)(shm_data + (size_t)sy * (size_t)stride_bytes) + cc->saved_x;
        const uint32_t *bk_row = cc->backup + (size_t)py * (size_t)cc->saved_w;
        memcpy(fb_row, bk_row, (size_t)cc->saved_w * sizeof(uint32_t));
    }
    cc->has_backup = false;
}

static bool frame_differs_from_prev(const uint8_t *src, int stride,
                                    uint8_t *prev_packed, int w, int h,
                                    bool has_prev) {
    size_t row_bytes = (size_t)w * 4;
    if (!has_prev) return true;

    for (int y = 0; y < h; y++) {
        const uint8_t *srow = src + (size_t)y * (size_t)stride;
        uint8_t *prow = prev_packed + (size_t)y * row_bytes;
        if (memcmp(srow, prow, row_bytes) != 0) {
            return true;
        }
    }
    return false;
}

static void frame_save_prev(const uint8_t *src, int stride,
                            uint8_t *prev_packed, int w, int h) {
    size_t row_bytes = (size_t)w * 4;
    for (int y = 0; y < h; y++) {
        memcpy(prev_packed + (size_t)y * row_bytes,
               src + (size_t)y * (size_t)stride,
               row_bytes);
    }
}

/* =====================================================================
 * 4. Main Pipeline (Single-Threaded, 1:1 Capture & Output)
 * ===================================================================== */
static void print_usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [-f MAX_FPS] [-q QUALITY] [-d] <DONGLE_IP> <X> <Y> <W> <H>\n"
            "  -f MAX_FPS  Max frames per second (default: 0 = uncapped)\n"
            "  -q QUALITY  JPEG quality 1..100 (default: 75)\n"
            "  -d          Skip unchanged frames (compare against previous frame)\n"
            "  DONGLE_IP   Target EZCast dongle IPv4 address\n"
            "  X Y W H     X11 capture & output region (1:1, no scaling)\n",
            prog);
}

int main(int argc, char **argv) {
    int max_fps = 0;
    int jpeg_quality = 75;
    bool diff_only = false;

    int opt;
    while ((opt = getopt(argc, argv, "f:q:dh")) != -1) {
        switch (opt) {
            case 'f':
                max_fps = atoi(optarg);
                if (max_fps < 0) {
                    fprintf(stderr, "Error: MAX_FPS must be >= 0.\n");
                    return 1;
                }
                break;
            case 'q':
                jpeg_quality = atoi(optarg);
                if (jpeg_quality < 1 || jpeg_quality > 100) {
                    fprintf(stderr, "Error: QUALITY must be between 1 and 100.\n");
                    return 1;
                }
                break;
            case 'd':
                diff_only = true;
                break;
            case 'h':
            default:
                print_usage(argv[0]);
                return (opt == 'h') ? 0 : 1;
        }
    }

    if (argc - optind != 5) {
        print_usage(argv[0]);
        return 1;
    }

    const char *dongle_ip = argv[optind + 0];
    int cap_x = atoi(argv[optind + 1]);
    int cap_y = atoi(argv[optind + 2]);
    int dim_w = atoi(argv[optind + 3]);
    int dim_h = atoi(argv[optind + 4]);

    if (cap_x < 0 || cap_y < 0 || dim_w <= 0 || dim_h <= 0) {
        fprintf(stderr, "Error: X, Y must be >= 0 and W, H must be > 0.\n");
        return 1;
    }

    uint64_t frame_interval_ns = (max_fps > 0) ? (1000000000ULL / (uint64_t)max_fps) : 0;

    srand((unsigned int)time(NULL));
    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);

    /* 1. Open X11 Display & Validate Region + XShm + XFixes */
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "[X11] Cannot open display.\n");
        return 1;
    }
    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);
    int root_w = DisplayWidth(dpy, screen);
    int root_h = DisplayHeight(dpy, screen);

    if (cap_x + dim_w > root_w || cap_y + dim_h > root_h) {
        fprintf(stderr, "[X11] Region (%d,%d %dx%d) exceeds root display (%dx%d).\n",
                cap_x, cap_y, dim_w, dim_h, root_w, root_h);
        XCloseDisplay(dpy);
        return 1;
    }

    if (!XShmQueryExtension(dpy)) {
        fprintf(stderr, "[X11] MIT-SHM extension not available.\n");
        XCloseDisplay(dpy);
        return 1;
    }

    int xfixes_event_base = 0, xfixes_error_base = 0;
    bool has_xfixes = XFixesQueryExtension(dpy, &xfixes_event_base, &xfixes_error_base);
    CachedCursor cached_cursor;
    memset(&cached_cursor, 0, sizeof(cached_cursor));

    if (has_xfixes) {
        XFixesSelectCursorInput(dpy, root, XFixesDisplayCursorNotifyMask);
        cursor_update_image(dpy, &cached_cursor);
        printf("[Cursor] Event-driven XFixes cursor caching enabled (%dx%d).\n",
               cached_cursor.width, cached_cursor.height);
    } else {
        fprintf(stderr, "[Cursor] XFixes not available; cursor overlay disabled.\n");
    }

    /* 2. Allocate Persistent XShm Image sized strictly to W x H */
    XShmSegmentInfo shminfo;
    memset(&shminfo, 0, sizeof(shminfo));
    XImage *ximg = XShmCreateImage(dpy, DefaultVisual(dpy, screen),
                                   (unsigned int)DefaultDepth(dpy, screen),
                                   ZPixmap, NULL, &shminfo,
                                   (unsigned int)dim_w, (unsigned int)dim_h);
    if (!ximg) {
        fprintf(stderr, "[X11] XShmCreateImage failed.\n");
        XCloseDisplay(dpy);
        return 1;
    }

    shminfo.shmid = shmget(IPC_PRIVATE,
                           (size_t)ximg->bytes_per_line * (size_t)ximg->height,
                           IPC_CREAT | 0600);
    if (shminfo.shmid < 0) {
        perror("[X11] shmget");
        XDestroyImage(ximg);
        XCloseDisplay(dpy);
        return 1;
    }
    shminfo.shmaddr = (char *)shmat(shminfo.shmid, NULL, 0);
    ximg->data = shminfo.shmaddr;
    shminfo.readOnly = False;
    XShmAttach(dpy, &shminfo);
    XSync(dpy, False);
    shmctl(shminfo.shmid, IPC_RMID, NULL);

    /* 3. Pre-allocate Optional Diff Buffer & TurboJPEG */
    uint8_t *prev_frame_buf = NULL;
    if (diff_only) {
        size_t frame_bytes = (size_t)dim_w * (size_t)dim_h * 4;
        prev_frame_buf = (uint8_t *)malloc(frame_bytes);
        if (!prev_frame_buf) {
            fprintf(stderr, "[Mem] Failed to allocate diff buffer.\n");
            return 1;
        }
    }

    tjhandle tj = tjInitCompress();
    if (!tj) {
        fprintf(stderr, "[JPEG] tjInitCompress failed: %s\n", tjGetErrorStr());
        return 1;
    }

    size_t max_jpg_size = tjBufSize(dim_w, dim_h, TJSAMP_420);
    uint8_t *jpg_buf = tjAlloc((int)max_jpg_size);
    if (!jpg_buf) {
        fprintf(stderr, "[JPEG] tjAlloc failed.\n");
        return 1;
    }

    /* 4. Connect to EZCast Dongle */
    EZCastControl ctrl;
    EZDisplayStream stream;
    if (!control_connect(&ctrl, dongle_ip)) {
        fprintf(stderr, "[Main] Control connection failed.\n");
        return 1;
    }
    if (!stream_connect(&stream, dongle_ip)) {
        fprintf(stderr, "[Main] Stream handshake failed or timed out.\n");
        control_close(&ctrl);
        return 1;
    }

    if (max_fps > 0) {
        printf("[Main] Streaming 1:1 %dx%d at (%d,%d) (Q=%d, MaxFPS=%d, DiffOnly=%s). Press Ctrl+C to stop.\n",
               dim_w, dim_h, cap_x, cap_y, jpeg_quality, max_fps, diff_only ? "on" : "off");
    } else {
        printf("[Main] Streaming 1:1 %dx%d at (%d,%d) (Q=%d, MaxFPS=uncapped, DiffOnly=%s). Press Ctrl+C to stop.\n",
               dim_w, dim_h, cap_x, cap_y, jpeg_quality, diff_only ? "on" : "off");
    }

    uint64_t stat_t0 = now_ns();
    uint64_t last_sent_ns = 0;
    uint64_t cap_frames = 0;
    uint64_t sent_frames = 0;
    uint64_t skipped_frames = 0;
    uint64_t sent_bytes = 0;
    bool     has_prev_frame = false;

    /* 5. Main Single-Threaded Capture -> [Optional Diff] -> Encode -> Send Loop */
    while (g_running) {
        uint64_t frame_start_ns = now_ns();

        if (!control_poll(&ctrl, 0)) {
            fprintf(stderr, "\n[Control] Connection lost.\n");
            break;
        }
        if (!stream_poll_and_heartbeat(&stream, 0)) {
            fprintf(stderr, "\n[Stream] Connection lost.\n");
            break;
        }

        if (has_xfixes) {
            while (XPending(dpy) > 0) {
                XEvent ev;
                XNextEvent(dpy, &ev);
                if (ev.type == xfixes_event_base + XFixesCursorNotify) {
                    cursor_update_image(dpy, &cached_cursor);
                }
            }
        }

        if (!XShmGetImage(dpy, root, ximg, cap_x, cap_y, AllPlanes)) {
            fprintf(stderr, "\n[X11] XShmGetImage failed.\n");
            break;
        }

        if (has_xfixes && cached_cursor.valid) {
            Window root_ret, child_ret;
            int root_x = 0, root_y = 0, win_x = 0, win_y = 0;
            unsigned int mask = 0;
            if (XQueryPointer(dpy, root, &root_ret, &child_ret,
                              &root_x, &root_y, &win_x, &win_y, &mask)) {
                cursor_composite_to_shm(&cached_cursor, (uint8_t *)ximg->data,
                                        dim_w, dim_h, ximg->bytes_per_line,
                                        root_x - cap_x, root_y - cap_y);
            }
        }

        cap_frames++;

        bool should_send = true;
        if (diff_only) {
            uint64_t now = now_ns();
            bool keepalive_due = !has_prev_frame || ((now - last_sent_ns) >= KEEPALIVE_FRAME_NS);
            should_send = keepalive_due ||
                          frame_differs_from_prev((const uint8_t *)ximg->data,
                                                  ximg->bytes_per_line,
                                                  prev_frame_buf,
                                                  dim_w, dim_h,
                                                  has_prev_frame);
        }

        if (should_send) {
            unsigned char *dst_jpg = jpg_buf;
            unsigned long jpg_size = (unsigned long)max_jpg_size;

            int rc = tjCompress2(tj,
                                 (const unsigned char *)ximg->data,
                                 dim_w,
                                 ximg->bytes_per_line,
                                 dim_h,
                                 TJPF_BGRX,
                                 &dst_jpg,
                                 &jpg_size,
                                 TJSAMP_420,
                                 jpeg_quality,
                                 TJFLAG_FASTDCT | TJFLAG_NOREALLOC);

            cursor_restore_shm(&cached_cursor, (uint8_t *)ximg->data, ximg->bytes_per_line);

            if (rc != 0) {
                fprintf(stderr, "\n[JPEG] tjCompress2 error: %s\n", tjGetErrorStr());
                break;
            }

            if (!stream_send_jpeg(&stream, (uint32_t)dim_w, (uint32_t)dim_h, jpg_buf, (uint32_t)jpg_size)) {
                fprintf(stderr, "\n[Stream] Failed to send JPEG frame.\n");
                break;
            }

            if (diff_only) {
                frame_save_prev((const uint8_t *)ximg->data,
                                ximg->bytes_per_line,
                                prev_frame_buf,
                                dim_w, dim_h);
                has_prev_frame = true;
                last_sent_ns = now_ns();
            }

            sent_frames++;
            sent_bytes += jpg_size;
        } else {
            cursor_restore_shm(&cached_cursor, (uint8_t *)ximg->data, ximg->bytes_per_line);
            skipped_frames++;
        }

        /* Print live throughput every 1.0 second */
        uint64_t now = now_ns();
        uint64_t dt_ns = now - stat_t0;
        if (dt_ns >= 1000000000ULL) {
            double dt = (double)dt_ns / 1e9;
            double cap_fps = (double)cap_frames / dt;
            double net_fps = (double)sent_frames / dt;
            double mbps = ((double)sent_bytes * 8.0) / (dt * 1e6);

            if (diff_only) {
                double skip_fps = (double)skipped_frames / dt;
                printf("\r[Stats] Cap: %5.1f FPS | Sent: %5.1f FPS | Skip: %5.1f FPS | Bitrate: %5.2f Mbps   ",
                       cap_fps, net_fps, skip_fps, mbps);
            } else {
                printf("\r[Stats] Cap: %5.1f FPS | Sent: %5.1f FPS | Bitrate: %5.2f Mbps   ",
                       cap_fps, net_fps, mbps);
            }
            fflush(stdout);

            stat_t0 = now;
            cap_frames = 0;
            sent_frames = 0;
            skipped_frames = 0;
            sent_bytes = 0;
        }

        /* Socket-aware FPS pacing via poll() when max_fps > 0 */
        if (frame_interval_ns > 0) {
            while (g_running) {
                uint64_t elapsed_ns = now_ns() - frame_start_ns;
                if (elapsed_ns >= frame_interval_ns) break;

                int wait_ms = (int)((frame_interval_ns - elapsed_ns) / 1000000ULL);
                if (wait_ms == 0) {
                    sleep_ns(frame_interval_ns - elapsed_ns);
                    break;
                }

                struct pollfd pfds[2];
                nfds_t nfds = 0;
                int ctrl_idx = -1;
                int stream_idx = -1;

                if (!ctrl.closed && ctrl.sock >= 0) {
                    ctrl_idx = (int)nfds;
                    pfds[nfds].fd = ctrl.sock;
                    pfds[nfds].events = POLLIN;
                    pfds[nfds].revents = 0;
                    nfds++;
                }

                if (stream.sock >= 0) {
                    stream_idx = (int)nfds;
                    pfds[nfds].fd = stream.sock;
                    pfds[nfds].events = POLLIN;
                    pfds[nfds].revents = 0;
                    nfds++;
                }

                int pr = poll(pfds, nfds, wait_ms);
                if (pr < 0) {
                    if (errno == EINTR) continue;
                    g_running = 0;
                    break;
                }
                if (pr > 0) {
                    if (ctrl_idx >= 0 && pfds[ctrl_idx].revents) {
                        if ((pfds[ctrl_idx].revents & POLLNVAL) || !control_poll(&ctrl, 0)) {
                            g_running = 0;
                            break;
                        }
                    }
                    if (stream_idx >= 0 && pfds[stream_idx].revents) {
                        if ((pfds[stream_idx].revents & POLLNVAL) || !stream_poll_and_heartbeat(&stream, 0)) {
                            g_running = 0;
                            break;
                        }
                    }
                }
            }
        }
    }

    printf("\n[Main] Shutting down...\n");
    stream_close(&stream);
    control_close(&ctrl);

    tjFree(jpg_buf);
    tjDestroy(tj);
    free(prev_frame_buf);

    XShmDetach(dpy, &shminfo);
    XDestroyImage(ximg);
    shmdt(shminfo.shmaddr);
    XCloseDisplay(dpy);

    return 0;
}
