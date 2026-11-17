/**
 * webos_tuner_d.c: LG webOS TV 内蔵チューナー＆B-CAS ネットワーク配信デーモン (耐障害・自動発見対応版)
 *  - TCP  :40580 (BonDriver_WebOS 用制御・チャンネル一覧・B-CAS APDU・TS ストリーム)
 *  - UDP  :40581 (LAN 内自動発見 WEBOS_TUNER_DISCOVER 応答、有線/無線 MAC 通知対応)
 *  - HTTP :40772 (Mirakurun 互換 REST API / 純正 EPG / ステータス・ログ画面 / TS 配信)
 */
#include "multi2.h"
#include "tv_db.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <dlfcn.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>

#define BRIDGE_PORT      40570
#define PVR_STREAM_PORT  40571
#define TCP_PORT         40580
#define UDP_PORT         40581
#define HTTP_PORT        40772
#define MAX_STREAM_CLIENTS 16
#define LOG_RING_LINES     64
#define NUM_TUNERS         2
#define MAX_PVR_PIDS       64

typedef struct {
    int fd;
    int decode;
    int active;
    int is_http;
    int tuner; /* 0: PVR (EXT_INPUT0), 1: dump/reader (EXT_INPUT1) */
    uint16_t filter_sid; /* 0: フルTS, !=0: 指定サービスのみ抽出 */
    uint8_t  pat_cc;
    int      priority;
    int      snd_timeouts;
    uint64_t skip_until_ms;
    uint8_t  kicked;
    uint64_t kicked_ms;
} stream_client_t;

typedef struct {
    int              id; /* 0 または 1 */
    int              is_pvr;
    pthread_mutex_t  tune_mtx;
    int              cur_media;
    uint32_t         cur_phy;
    uint32_t         cur_freq;
    uint16_t         cur_tsid;
    int              cur_lock;
    volatile int     stream_gen;

    b25_ctx_t        b25;
    pthread_mutex_t  b25_mtx;
    uint8_t          ts_filt[188u * 256u];
    volatile uint64_t bytes_total;

    pthread_t        th;
    volatile int     in_read;
    volatile int     reopen_req;
    volatile uint64_t last_pkt_ms;
    volatile uint64_t read_start_ms;

    uint8_t          pid_added[8192];
    int              pid_count;
    int              pid_full;
} tuner_state_t;

/* ---- グローバル状態 ---- */
static tv_channel_list_t g_chlist;
static pthread_mutex_t   g_ch_mtx = PTHREAD_MUTEX_INITIALIZER;

static tuner_state_t     g_tuner[NUM_TUNERS];

static pthread_mutex_t   g_cli_mtx = PTHREAD_MUTEX_INITIALIZER;
static stream_client_t   g_clients[MAX_STREAM_CLIENTS];

static volatile int      g_running = 1;

static char              g_so_path[256]  = "/tmp/libwebos_tuner.so";
static char              g_inj_path[256] = "/tmp/webos_inject";
static char              g_hostname[64]  = "LGwebOSTV";
static volatile int      g_power_mode = 1;            /* 1=always(ActiveStandby維持), 0=ondemand(スリープ), 2=shutdown(完全シャットダウン) */
static volatile int      g_pending_power_cmd = 0;     /* 1=powerOff(standby), 2=powerOn */
static char              g_tv_power_state[32] = "Active";
static char              g_tv_name[64]        = "";
static char              g_tv_model[64]       = "LG webOS TV";
static char              g_tv_webos[64]       = "";
static char              g_tv_fw[32]          = "";
static char              g_tv_release[32]     = "";

static void load_starfish_release(void) {
    FILE *f = fopen("/etc/starfish-release", "r");
    if (!f) return;
    char line[128] = {0};
    if (fgets(line, sizeof(line), f)) {
        const char *r = strstr(line, "release ");
        if (r) {
            r += 8;
            size_t w = 0;
            while (*r && *r != ' ' && *r != '\r' && *r != '\n' && w + 1 < sizeof(g_tv_release)) {
                g_tv_release[w++] = *r++;
            }
            g_tv_release[w] = '\0';
            snprintf(g_tv_webos, sizeof(g_tv_webos), "%s", g_tv_release);
        }
    }
    fclose(f);
}

static void get_power_cfg_path(char *out, size_t sz) {
    const char *sl = strrchr(g_so_path, '/');
    if (sl && (size_t)(sl - g_so_path) + 20 < sz) {
        size_t dirlen = (size_t)(sl - g_so_path);
        memcpy(out, g_so_path, dirlen);
        snprintf(out + dirlen, sz - dirlen, "/power_mode.conf");
    } else {
        snprintf(out, sz, "/tmp/webos_tuner_power.conf");
    }
}

static const char *power_mode_str(int m) {
    if (m == 0) return "ondemand";
    if (m == 2) return "shutdown";
    return "always";
}

static void load_power_mode(void) {
    char p[300];
    get_power_cfg_path(p, sizeof(p));
    FILE *f = fopen(p, "r");
    if (!f) {
        g_power_mode = 1;
        return;
    }
    char buf[32] = {0};
    if (fgets(buf, sizeof(buf), f)) {
        if (strncmp(buf, "ondemand", 8) == 0) g_power_mode = 0;
        else if (strncmp(buf, "shutdown", 8) == 0) g_power_mode = 2;
        else g_power_mode = 1;
    }
    fclose(f);
}

static void save_power_mode(int mode) {
    if (mode < 0 || mode > 2) mode = 1;
    g_power_mode = mode;
    char p[300];
    get_power_cfg_path(p, sizeof(p));
    FILE *f = fopen(p, "w");
    if (f) {
        fprintf(f, "%s\n", power_mode_str(g_power_mode));
        fclose(f);
    }
}

/* ---- リングログ ---- */
static pthread_mutex_t   g_log_mtx = PTHREAD_MUTEX_INITIALIZER;
static char              g_log_ring[LOG_RING_LINES][224];
static int               g_log_head = 0;
static int               g_log_count = 0;

static uint64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
}

static void add_log(const char *fmt, ...) {
    char msg[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char line[224];
    snprintf(line, sizeof(line), "%02d:%02d:%02d %s",
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec, msg);

    fprintf(stderr, "%s\n", line);

    pthread_mutex_lock(&g_log_mtx);
    snprintf(g_log_ring[g_log_head], sizeof(g_log_ring[0]), "%s", line);
    g_log_head = (g_log_head + 1) % LOG_RING_LINES;
    if (g_log_count < LOG_RING_LINES) g_log_count++;
    pthread_mutex_unlock(&g_log_mtx);
}

/* ---- ネットワーク IF / MAC 読み取り ---- */
static void read_sys_str(const char *path, char *out, size_t sz, const char *fallback) {
    FILE *f = fopen(path, "r");
    if (!f) { snprintf(out, sz, "%s", fallback); return; }
    if (!fgets(out, (int)sz, f)) {
        snprintf(out, sz, "%s", fallback);
        fclose(f);
        return;
    }
    fclose(f);
    size_t l = strlen(out);
    while (l > 0 && (out[l - 1] == '\n' || out[l - 1] == '\r')) out[--l] = '\0';
}

static void get_mac_info(char *active_mac, size_t a_sz, char *eth_mac, size_t e_sz, char *wlan_mac, size_t w_sz) {
    char eth_st[32], wlan_st[32];
    read_sys_str("/sys/class/net/eth0/address", eth_mac, e_sz, "00:00:00:00:00:00");
    read_sys_str("/sys/class/net/wlan0/address", wlan_mac, w_sz, "00:00:00:00:00:00");
    read_sys_str("/sys/class/net/eth0/operstate", eth_st, sizeof(eth_st), "down");
    read_sys_str("/sys/class/net/wlan0/operstate", wlan_st, sizeof(wlan_st), "down");

    if (strcmp(eth_st, "up") == 0 && strcmp(eth_mac, "00:00:00:00:00:00") != 0) {
        snprintf(active_mac, a_sz, "%s", eth_mac);
    } else if (strcmp(wlan_st, "up") == 0 && strcmp(wlan_mac, "00:00:00:00:00:00") != 0) {
        snprintf(active_mac, a_sz, "%s", wlan_mac);
    } else if (strcmp(eth_mac, "00:00:00:00:00:00") != 0) {
        snprintf(active_mac, a_sz, "%s", eth_mac);
    } else {
        snprintf(active_mac, a_sz, "%s", wlan_mac);
    }
}

/* ループバック以外のローカル IPv4 アドレスを取得する */
static void get_local_ip(char *out, size_t sz, int client_fd) {
    struct ifaddrs *ifaddr = NULL;
    char fallback[64] = {0};
    if (getifaddrs(&ifaddr) == 0) {
        for (struct ifaddrs *ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
            if (ifa->ifa_flags & IFF_LOOPBACK) continue;
            if (!(ifa->ifa_flags & IFF_UP)) continue;
            struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
            char ip[INET_ADDRSTRLEN] = {0};
            if (!inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip))) continue;
            if (strcmp(ip, "0.0.0.0") == 0) continue;
            if (strcmp(ifa->ifa_name, "eth0") == 0 || strcmp(ifa->ifa_name, "wlan0") == 0) {
                snprintf(out, sz, "%s", ip);
                freeifaddrs(ifaddr);
                return;
            }
            if (!fallback[0]) snprintf(fallback, sizeof(fallback), "%s", ip);
        }
        freeifaddrs(ifaddr);
    }
    if (fallback[0]) {
        snprintf(out, sz, "%s", fallback);
        return;
    }
    if (client_fd >= 0) {
        struct sockaddr_in laddr;
        socklen_t llen = sizeof(laddr);
        if (getsockname(client_fd, (struct sockaddr *)&laddr, &llen) == 0) {
            const char *s = inet_ntoa(laddr.sin_addr);
            if (s && strcmp(s, "0.0.0.0") != 0 && strcmp(s, "127.0.0.1") != 0) {
                snprintf(out, sz, "%s", s);
                return;
            }
        }
    }
    snprintf(out, sz, "127.0.0.1");
}

/* ---- 127.0.0.1:40570 (tvservice 注入部) との通信 ---- */
static int bridge_cmd(const char *cmd, char *resp, size_t rsz) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval tv = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(BRIDGE_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    size_t clen = strlen(cmd);
    send(fd, cmd, clen, MSG_NOSIGNAL);
    if (clen == 0 || cmd[clen - 1] != '\n') send(fd, "\n", 1, MSG_NOSIGNAL);

    size_t got = 0;
    while (got + 1 < rsz) {
        ssize_t n = recv(fd, resp + got, rsz - 1 - got, 0);
        if (n <= 0) break;
        got += (size_t)n;
        if (memchr(resp, '\n', got)) break;
    }
    resp[got] = '\0';
    close(fd);
    return (got > 0) ? 0 : -1;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int b25_bcas_bridge_cb(void *user, const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t *rx_len) {
    (void)user;
    char cmd[1024];
    char resp[2048];
    int pos = snprintf(cmd, sizeof(cmd), "BCAS ");
    for (size_t i = 0; i < tx_len && pos + 3 < (int)sizeof(cmd); i++) {
        pos += snprintf(cmd + pos, sizeof(cmd) - (size_t)pos, "%02x", tx[i]);
    }
    if (bridge_cmd(cmd, resp, sizeof(resp)) != 0) return -1;
    if (strncmp(resp, "OK ", 3) != 0) return -1;

    const char *p = resp + 3;
    size_t max_rx = *rx_len;
    size_t out_n = 0;
    while (p[0] && p[1] && p[0] != '\n' && p[0] != '\r' && out_n < max_rx) {
        int hi = hex_val(p[0]), lo = hex_val(p[1]);
        if (hi < 0 || lo < 0) break;
        rx[out_n++] = (uint8_t)((hi << 4) | lo);
        p += 2;
    }
    *rx_len = out_n;
    return (out_n > 0) ? 0 : -1;
}

static int init_bcas_syskey(void) {
    const uint8_t int_cmd[5] = {0x90, 0x30, 0x00, 0x00, 0x00};
    uint8_t rx[128];
    size_t rlen = sizeof(rx);
    if (b25_bcas_bridge_cb(NULL, int_cmd, sizeof(int_cmd), rx, &rlen) != 0) return -1;
    if (rlen < 57 || rx[4] != 0x21 || rx[5] != 0x00) return -1;
    for (int t = 0; t < NUM_TUNERS; t++) {
        pthread_mutex_lock(&g_tuner[t].b25_mtx);
        b25_set_syskey(&g_tuner[t].b25, rx + 16, rx + 48);
        pthread_mutex_unlock(&g_tuner[t].b25_mtx);
    }
    add_log("[B-CAS] SystemKey & InitialCBC loaded (rx=%zuB)", rlen);
    return 0;
}

static int ensure_bridge_injected(void) {
    char resp[256];
    if (bridge_cmd("PING", resp, sizeof(resp)) == 0 && strncmp(resp, "OK ", 3) == 0) {
        return 0;
    }
    char syscmd[600];
    snprintf(syscmd, sizeof(syscmd), "%s %s", g_inj_path, g_so_path);
    int rc = system(syscmd);
    (void)rc;
    usleep(300000);
    if (bridge_cmd("PING", resp, sizeof(resp)) == 0 && strncmp(resp, "OK ", 3) == 0) {
        return 0;
    }
    return -1;
}

static int count_active_clients_locked(int tuner) {
    int c = 0;
    for (int i = 0; i < MAX_STREAM_CLIENTS; i++) {
        if (g_clients[i].active && (tuner < 0 || g_clients[i].tuner == tuner)) c++;
    }
    return c;
}

static int count_active_clients_for(int tuner) {
    pthread_mutex_lock(&g_cli_mtx);
    int c = count_active_clients_locked(tuner);
    pthread_mutex_unlock(&g_cli_mtx);
    return c;
}

static int count_active_clients(void) {
    return count_active_clients_for(-1);
}

static uint64_t total_bytes_sent(void) {
    return g_tuner[0].bytes_total + g_tuner[1].bytes_total;
}

/** 相手側が既にクローズ済み、またはキック済み timeout 経過ソケットを掃除する */
static void prune_dead_clients(void) {
    int to_close[MAX_STREAM_CLIENTS];
    int n_close = 0;
    uint64_t now = mono_ms();
    pthread_mutex_lock(&g_cli_mtx);
    for (int i = 0; i < MAX_STREAM_CLIENTS; i++) {
        /* SO_SNDTIMEO (12s) より長く猶予を取ってから fd を回収する */
        if (g_clients[i].kicked && g_clients[i].fd >= 0 && (now - g_clients[i].kicked_ms) >= 15000) {
            to_close[n_close++] = g_clients[i].fd;
            g_clients[i].fd = -1;
            g_clients[i].kicked = 0;
            g_clients[i].active = 0;
            continue;
        }
        if (!g_clients[i].active || g_clients[i].fd < 0) continue;
        char probe = 0;
        ssize_t r = recv(g_clients[i].fd, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
        if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            to_close[n_close++] = g_clients[i].fd;
            g_clients[i].active = 0;
            g_clients[i].fd = -1;
            g_clients[i].kicked = 0;
        }
    }
    pthread_mutex_unlock(&g_cli_mtx);
    for (int i = 0; i < n_close; i++) {
        close(to_close[i]);
        add_log("[TS] Pruned dead client fd=%d", to_close[i]);
    }
}

static int add_stream_client(int fd, int decode, int is_http, int tuner, uint16_t filter_sid, int priority) {
    if (tuner < 0 || tuner >= NUM_TUNERS) tuner = 1;
    struct timeval stv = {12, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &stv, sizeof(stv));

    pthread_mutex_lock(&g_cli_mtx);
    int slot = -1;
    for (int i = 0; i < MAX_STREAM_CLIENTS; i++) {
        if (!g_clients[i].active && !g_clients[i].kicked) {
            g_clients[i].fd = fd;
            g_clients[i].decode = decode;
            g_clients[i].is_http = is_http;
            g_clients[i].tuner = tuner;
            g_clients[i].filter_sid = filter_sid;
            g_clients[i].pat_cc = 0;
            g_clients[i].priority = priority;
            g_clients[i].snd_timeouts = 0;
            g_clients[i].skip_until_ms = 0;
            g_clients[i].kicked = 0;
            g_clients[i].kicked_ms = 0;
            g_clients[i].active = 1;
            slot = i;
            break;
        }
    }
    pthread_mutex_unlock(&g_cli_mtx);
    return slot;
}

static int take_client_fd_for_close(int fd) {
    int owned = 0;
    pthread_mutex_lock(&g_cli_mtx);
    for (int i = 0; i < MAX_STREAM_CLIENTS; i++) {
        if (g_clients[i].fd == fd) {
            g_clients[i].active = 0;
            g_clients[i].kicked = 0;
            g_clients[i].fd = -1;
            owned = 1;
        }
    }
    pthread_mutex_unlock(&g_cli_mtx);
    return owned;
}

static void pvr_reset_pid_table(tuner_state_t *t) {
    static const uint16_t init_pids[] = {0x0000, 0x0001, 0x0010, 0x0011, 0x0012, 0x0014, 0x0024};
    memset(t->pid_added, 0, sizeof(t->pid_added));
    for (size_t i = 0; i < sizeof(init_pids) / sizeof(init_pids[0]); i++) {
        t->pid_added[init_pids[i]] = 1;
    }
    t->pid_count = (int)(sizeof(init_pids) / sizeof(init_pids[0]));
    t->pid_full = 0;
}

static void on_pvr_pid_discovered(void *user, uint16_t pid, int pid_type) {
    tuner_state_t *t = (tuner_state_t *)user;
    if (!t || !t->is_pvr || pid >= 0x1fff) return;
    if (t->pid_added[pid] || t->pid_full) return;
    if (t->pid_count >= MAX_PVR_PIDS) {
        t->pid_full = 1;
        add_log("[PVR] Local PID cap reached (%d), ignoring further PIDs", t->pid_count);
        return;
    }
    t->pid_added[pid] = 1;
    char cmd[64], resp[128];
    snprintf(cmd, sizeof(cmd), "PVRPID 1 %d 0x%x 8", pid_type, pid);
    if (bridge_cmd(cmd, resp, sizeof(resp)) == 0 && strncmp(resp, "OK ", 3) == 0) {
        const char *rp = strstr(resp, "ret=");
        int ret = rp ? atoi(rp + 4) : -1;
        if (ret == 0) {
            t->pid_count++;
        } else {
            t->pid_full = 1;
            add_log("[PVR] HW PID filter full at count=%d (pid=0x%04x ret=%d)", t->pid_count, pid, ret);
        }
    }
}

static int is_same_transponder(const tuner_state_t *t, int media, uint32_t phy, uint32_t freq_khz, uint16_t tsid) {
    return (t->cur_lock == 0 && t->cur_media == media && t->cur_phy == phy &&
            t->cur_freq == freq_khz && (media == 1 || t->cur_tsid == tsid));
}

static int max_priority_for_tuner(int tuner) {
    int maxp = -1000;
    pthread_mutex_lock(&g_cli_mtx);
    for (int i = 0; i < MAX_STREAM_CLIENTS; i++) {
        if (g_clients[i].active && g_clients[i].tuner == tuner) {
            if (g_clients[i].priority > maxp) maxp = g_clients[i].priority;
        }
    }
    pthread_mutex_unlock(&g_cli_mtx);
    return maxp;
}

static void kick_clients_for_tuner(int tuner) {
    int kfds[MAX_STREAM_CLIENTS];
    int kcnt = 0;
    uint64_t now = mono_ms();
    pthread_mutex_lock(&g_cli_mtx);
    for (int i = 0; i < MAX_STREAM_CLIENTS; i++) {
        if (g_clients[i].active && g_clients[i].tuner == tuner && g_clients[i].fd >= 0) {
            g_clients[i].active = 0;
            g_clients[i].kicked = 1;
            g_clients[i].kicked_ms = now;
            kfds[kcnt++] = g_clients[i].fd;
        }
    }
    pthread_mutex_unlock(&g_cli_mtx);
    for (int i = 0; i < kcnt; i++) {
        shutdown(kfds[i], SHUT_RDWR);
        add_log("[TUNE%d] Kicked lower-priority client fd=%d", tuner, kfds[i]);
    }
}

/**
 * 要求されたチューナー (req_tuner: -1=auto, 0=Tuner0/PVR, 1=Tuner1/dump) へ選局する
 */
static int tune_tuner(int req_tuner, int media, uint32_t phy, uint32_t freq_khz, uint16_t tsid,
                      int force, int bypass_cache, int priority, int *tuner_out) {
    prune_dead_clients();

    int tv_lit = (strcmp(g_tv_power_state, "Active") == 0);
    int allow_t0 = (!tv_lit || priority >= 2);
    int tid = req_tuner;
    int preempt = 0;

    if (tid < 0) {
        /* 1. すでに同じトランスポンダを受信中ならそれに相乗り (優先度無関係) */
        for (int pass = 1; pass >= 0; pass--) {
            if (count_active_clients_for(pass) > 0 &&
                is_same_transponder(&g_tuner[pass], media, phy, freq_khz, tsid)) {
                tid = pass;
                break;
            }
        }
        /* 2. 空きチューナーを選択 (TV 点灯中かつ低優先度なら Tuner 0 を保護) */
        if (tid < 0) {
            if (count_active_clients_for(1) == 0) tid = 1;
            else if (allow_t0 && count_active_clients_for(0) == 0) tid = 0;
        }
        /* 3. 優先度による奪取 (priority >= 0 かつ既存最大優先度より大、または >=128) */
        if (tid < 0 && priority >= 0) {
            int p1 = max_priority_for_tuner(1);
            if (priority >= 128 || priority > p1) {
                tid = 1;
                preempt = 1;
            } else if (allow_t0) {
                int p0 = max_priority_for_tuner(0);
                if (priority >= 128 || priority > p0) {
                    tid = 0;
                    preempt = 1;
                }
            }
        }
        if (tid < 0) return -2;
    }
    if (tid != 0 && tid != 1) tid = 1;
    if (tuner_out) *tuner_out = tid;

    tuner_state_t *t = &g_tuner[tid];
    pthread_mutex_lock(&t->tune_mtx);

    int active_now = count_active_clients_for(tid);
    if (!bypass_cache && active_now > 0 && is_same_transponder(t, media, phy, freq_khz, tsid)) {
        pthread_mutex_unlock(&t->tune_mtx);
        return 0;
    }
    if (!force && active_now > 0 && !is_same_transponder(t, media, phy, freq_khz, tsid)) {
        if (!preempt && priority >= 0) {
            int maxp = max_priority_for_tuner(tid);
            if (priority >= 128 || priority > maxp) preempt = 1;
        }
        if (!preempt) {
            pthread_mutex_unlock(&t->tune_mtx);
            return -2;
        }
    }
    if (preempt) {
        kick_clients_for_tuner(tid);
    }

    char cmd[128], resp[512];
    snprintf(cmd, sizeof(cmd), "TUNE%d %d %u %u 0x%x 1 28860", tid, media, phy, freq_khz, tsid);
    if (bridge_cmd(cmd, resp, sizeof(resp)) != 0 || strncmp(resp, "OK ", 3) != 0) {
        pthread_mutex_unlock(&t->tune_mtx);
        return -1;
    }

    int lock_val = -1;
    const char *lp = strstr(resp, "lock=");
    if (!lp) lp = strstr(resp, "lock1=");
    if (lp) lock_val = atoi(strchr(lp, '=') + 1);

    t->cur_media = media;
    t->cur_phy   = phy;
    t->cur_freq  = freq_khz;
    t->cur_tsid  = tsid;
    t->cur_lock  = lock_val;
    t->stream_gen++;

    pthread_mutex_lock(&t->b25_mtx);
    b25_reset_stream(&t->b25);
    if (t->is_pvr) pvr_reset_pid_table(t);
    pthread_mutex_unlock(&t->b25_mtx);

    pthread_mutex_unlock(&t->tune_mtx);
    add_log("[TUNE%d] media=%d phy=%u freq=%u tsid=0x%04x lock=%d pri=%d",
            tid, media, phy, freq_khz, tsid, lock_val, priority);
    return (lock_val == 0) ? 0 : -3;
}

/* ---- 192B TS 同期・MULTI2 復号・配信の共通処理 ---- */
static void on_sigusr1(int s) {
    (void)s;
    /* SA_RESTART なしで登録し、read/recv のブロックを EINTR で即解除する */
}

static void write_proc_str(const char *path, const char *val) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) return;
    ssize_t w = write(fd, val, strlen(val));
    (void)w;
    close(fd);
}

static int send_all_timeout(int fd, const uint8_t *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return -2;
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

static void dispatch_ts_chunk(tuner_state_t *t, uint8_t *raw_buf, size_t *carry_io, size_t avail,
                              int *synced_io, uint8_t *ts_raw, uint8_t *ts_dec) {
    size_t pos = 0;
    if (!*synced_io) {
        for (size_t i = 0; i + 192u * 4u < avail; i++) {
            if (raw_buf[i] == 0x47 && raw_buf[i + 192] == 0x47 &&
                raw_buf[i + 384] == 0x47 && raw_buf[i + 576] == 0x47) {
                pos = (i >= 4) ? (i - 4) : (i + 188);
                *synced_io = 1;
                break;
            }
        }
        if (!*synced_io) {
            if (avail > 1024) {
                memmove(raw_buf, raw_buf + avail - 1024, 1024);
                *carry_io = 1024;
            } else {
                *carry_io = avail;
            }
            return;
        }
    }

    size_t pkts = 0;
    while (pos + 192 <= avail && pkts < 256) {
        if (raw_buf[pos + 4] != 0x47) {
            int found = 0;
            for (size_t j = pos + 1; j + 192 < avail; j++) {
                if (raw_buf[j] == 0x47 && raw_buf[j + 192] == 0x47) {
                    pos = (j >= 4) ? (j - 4) : (j + 188);
                    found = 1;
                    break;
                }
            }
            if (!found) break;
            continue;
        }
        memcpy(ts_raw + pkts * 188u, raw_buf + pos + 4, 188u);
        pkts++;
        pos += 192u;
    }

    *carry_io = avail - pos;
    if (*carry_io > 0) memmove(raw_buf, raw_buf + pos, *carry_io);
    if (pkts == 0) return;

    size_t ts_bytes = pkts * 188u;
    t->bytes_total += ts_bytes;

    int fds[MAX_STREAM_CLIENTS];
    int decs[MAX_STREAM_CLIENTS];
    uint16_t fsids[MAX_STREAM_CLIENTS];
    uint8_t pccs[MAX_STREAM_CLIENTS];
    int cnt = 0, need_dec = 0, need_filt = 0;
    uint64_t now_ms = mono_ms();
    pthread_mutex_lock(&g_cli_mtx);
    for (int i = 0; i < MAX_STREAM_CLIENTS; i++) {
        if (g_clients[i].active && g_clients[i].fd >= 0 && g_clients[i].tuner == t->id) {
            if (g_clients[i].skip_until_ms > now_ms) continue;
            fds[cnt] = g_clients[i].fd;
            decs[cnt] = g_clients[i].decode;
            fsids[cnt] = g_clients[i].filter_sid;
            pccs[cnt] = g_clients[i].pat_cc;
            if (g_clients[i].decode) need_dec = 1;
            if (g_clients[i].filter_sid != 0) need_filt = 1;
            cnt++;
        }
    }
    pthread_mutex_unlock(&g_cli_mtx);

    if (need_dec) {
        memcpy(ts_dec, ts_raw, ts_bytes);
        pthread_mutex_lock(&t->b25_mtx);
        b25_process_packets(&t->b25, ts_dec, pkts);
        pthread_mutex_unlock(&t->b25_mtx);
    } else if (t->is_pvr || need_filt) {
        /* 暗号解除不要時でも PVR PID 抽出 / サービス絞り込みのため PAT/CAT/PMT を解析 */
        pthread_mutex_lock(&t->b25_mtx);
        b25_scan_sections(&t->b25, ts_raw, pkts);
        pthread_mutex_unlock(&t->b25_mtx);
    }

    for (int i = 0; i < cnt; i++) {
        const uint8_t *src = decs[i] ? ts_dec : ts_raw;
        const uint8_t *send_ptr = src;
        size_t send_len = ts_bytes;
        if (fsids[i] != 0) {
            pthread_mutex_lock(&t->b25_mtx);
            size_t fpkts = b25_filter_service_packets(&t->b25, src, pkts, fsids[i], &pccs[i], t->ts_filt);
            pthread_mutex_unlock(&t->b25_mtx);
            if (fpkts == 0) continue;
            send_ptr = t->ts_filt;
            send_len = fpkts * 188u;
            pthread_mutex_lock(&g_cli_mtx);
            for (int j = 0; j < MAX_STREAM_CLIENTS; j++) {
                if (g_clients[j].active && g_clients[j].fd == fds[i]) {
                    g_clients[j].pat_cc = pccs[i];
                    break;
                }
            }
            pthread_mutex_unlock(&g_cli_mtx);
        }
        int sr = send_all_timeout(fds[i], send_ptr, send_len);
        if (sr == -2) {
            int drop = 0;
            uint64_t skip_to = mono_ms() + 1000;
            pthread_mutex_lock(&g_cli_mtx);
            for (int j = 0; j < MAX_STREAM_CLIENTS; j++) {
                if (g_clients[j].active && g_clients[j].fd == fds[i]) {
                    g_clients[j].snd_timeouts++;
                    g_clients[j].skip_until_ms = skip_to;
                    if (g_clients[j].snd_timeouts >= 3) drop = 1;
                    break;
                }
            }
            pthread_mutex_unlock(&g_cli_mtx);
            if (!drop) continue;
        } else if (sr == 0) {
            pthread_mutex_lock(&g_cli_mtx);
            for (int j = 0; j < MAX_STREAM_CLIENTS; j++) {
                if (g_clients[j].active && g_clients[j].fd == fds[i]) {
                    g_clients[j].snd_timeouts = 0;
                    g_clients[j].skip_until_ms = 0;
                    break;
                }
            }
            pthread_mutex_unlock(&g_cli_mtx);
            continue;
        }
        if (take_client_fd_for_close(fds[i])) {
            add_log("[TS%d] Client fd=%d disconnected", t->id, fds[i]);
            close(fds[i]);
        }
    }
}

/** Tuner 1: /proc/lg/sdec/dump/reader (EXT_INPUT1) フル TS 受信スレッド */
static void *ts_reader_thread(void *arg) {
    (void)arg;
    tuner_state_t *t = &g_tuner[1];
    const size_t BUF_CAP = 192u * 256u;
    uint8_t *raw_buf = (uint8_t *)malloc(BUF_CAP);
    uint8_t *ts_raw  = (uint8_t *)malloc(188u * 256u);
    uint8_t *ts_dec  = (uint8_t *)malloc(188u * 256u);

    int dump_fd = -1;
    size_t carry = 0;
    int synced = 0;
    int last_gen = t->stream_gen;

    while (g_running) {
        int n_cli = count_active_clients_for(1);

        if (n_cli == 0) {
            if (dump_fd >= 0) {
                close(dump_fd);
                dump_fd = -1;
                carry = 0;
                synced = 0;
                add_log("[TS1] Reader closed (0 clients)");
            }
            t->reopen_req = 0;
            usleep(20000);
            continue;
        }

        if (t->reopen_req && dump_fd >= 0) {
            close(dump_fd);
            dump_fd = -1;
            carry = 0;
            synced = 0;
            t->reopen_req = 0;
            add_log("[RECOVER1] Reopening dump/reader & re-tuning tuner1...");
            if (t->cur_media > 0 && t->cur_freq > 0) {
                tune_tuner(1, t->cur_media, t->cur_phy, t->cur_freq, t->cur_tsid, 1, 1, 0, NULL);
            }
        }

        if (dump_fd < 0) {
            write_proc_str("/proc/lg/sdec/dump/buffer_size", "3145728");
            write_proc_str("/proc/lg/sdec/dump/input", "EXT_INPUT1");
            dump_fd = open("/proc/lg/sdec/dump/reader", O_RDONLY);
            if (dump_fd < 0) {
                add_log("[TS1] Failed to open dump/reader: %s", strerror(errno));
                usleep(100000);
                continue;
            }
            carry = 0;
            synced = 0;
            last_gen = t->stream_gen;
            t->last_pkt_ms = mono_ms();
            add_log("[TS1] Reader opened (EXT_INPUT1, clients=%d)", n_cli);
        }

        int cur_gen = t->stream_gen;
        if (cur_gen != last_gen) {
            last_gen = cur_gen;
            carry = 0;
            synced = 0;
            t->last_pkt_ms = mono_ms();
        }

        t->read_start_ms = mono_ms();
        t->in_read = 1;
        ssize_t n = read(dump_fd, raw_buf + carry, BUF_CAP - carry);
        t->in_read = 0;

        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            usleep(5000);
            continue;
        }

        t->last_pkt_ms = mono_ms();
        dispatch_ts_chunk(t, raw_buf, &carry, carry + (size_t)n, &synced, ts_raw, ts_dec);
    }

    if (dump_fd >= 0) close(dump_fd);
    free(raw_buf);
    free(ts_raw);
    free(ts_dec);
    return NULL;
}

static int connect_loopback(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval tv = {3, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/** Tuner 0: PVR ch_a (127.0.0.1:40571) + 動的 PID 抽出による 2 本目受信スレッド */
static void *pvr_reader_thread(void *arg) {
    (void)arg;
    tuner_state_t *t = &g_tuner[0];
    const size_t BUF_CAP = 192u * 256u;
    uint8_t *raw_buf = (uint8_t *)malloc(BUF_CAP);
    uint8_t *ts_raw  = (uint8_t *)malloc(188u * 256u);
    uint8_t *ts_dec  = (uint8_t *)malloc(188u * 256u);

    int pvr_fd = -1;
    size_t carry = 0;
    int synced = 0;
    int last_gen = t->stream_gen;

    while (g_running) {
        int n_cli = count_active_clients_for(0);

        if (n_cli == 0) {
            if (pvr_fd >= 0) {
                close(pvr_fd);
                pvr_fd = -1;
                carry = 0;
                synced = 0;
                char resp[128];
                bridge_cmd("PVRSTOP", resp, sizeof(resp));
                add_log("[TS0] PVR stopped (0 clients)");
            }
            t->reopen_req = 0;
            usleep(20000);
            continue;
        }

        int cur_gen = t->stream_gen;
        if (pvr_fd >= 0 && (t->reopen_req || cur_gen != last_gen)) {
            close(pvr_fd);
            pvr_fd = -1;
            carry = 0;
            synced = 0;
            if (t->reopen_req) {
                t->reopen_req = 0;
                add_log("[RECOVER0] Reopening PVR & re-tuning tuner0...");
                if (t->cur_media > 0 && t->cur_freq > 0) {
                    tune_tuner(0, t->cur_media, t->cur_phy, t->cur_freq, t->cur_tsid, 1, 1, 0, NULL);
                }
            }
        }

        if (pvr_fd < 0) {
            char resp[256];
            if (bridge_cmd("PVRSTART 1 3 1", resp, sizeof(resp)) != 0 || strncmp(resp, "OK ", 3) != 0) {
                add_log("[TS0] PVRSTART failed: %s", resp);
                usleep(150000);
                continue;
            }
            pvr_fd = connect_loopback(PVR_STREAM_PORT);
            if (pvr_fd < 0) {
                add_log("[TS0] Failed to connect PVR stream port %d", PVR_STREAM_PORT);
                usleep(100000);
                continue;
            }
            carry = 0;
            synced = 0;
            last_gen = t->stream_gen;
            t->last_pkt_ms = mono_ms();
            add_log("[TS0] PVR opened (EXT_INPUT0 -> ch_a, clients=%d)", n_cli);
        }

        t->read_start_ms = mono_ms();
        t->in_read = 1;
        ssize_t n = recv(pvr_fd, raw_buf + carry, BUF_CAP - carry, 0);
        t->in_read = 0;

        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            add_log("[TS0] PVR stream EOF/err (%s), reopening...", strerror(errno));
            close(pvr_fd);
            pvr_fd = -1;
            usleep(50000);
            continue;
        }

        t->last_pkt_ms = mono_ms();
        dispatch_ts_chunk(t, raw_buf, &carry, carry + (size_t)n, &synced, ts_raw, ts_dec);
    }

    if (pvr_fd >= 0) {
        close(pvr_fd);
        char resp[128];
        bridge_cmd("PVRSTOP", resp, sizeof(resp));
    }
    free(raw_buf);
    free(ts_raw);
    free(ts_dec);
    return NULL;
}

/** 250ms 周期の死活監視・サスペンド復帰検知・詰まり解除スレッド (2チューナー対応) */
static void *watchdog_thread(void *arg) {
    (void)arg;
    time_t last_wall = time(NULL);

    while (g_running) {
        usleep(250000);
        prune_dead_clients();

        time_t now_wall = time(NULL);
        int resumed = (now_wall - last_wall > 4);
        last_wall = now_wall;

        if (resumed) {
            add_log("[POWER] System resume detected (wall clock jump)");
            for (int i = 0; i < NUM_TUNERS; i++) {
                if (count_active_clients_for(i) > 0 || g_tuner[i].in_read) {
                    g_tuner[i].reopen_req = 1;
                    pthread_kill(g_tuner[i].th, SIGUSR1);
                }
            }
            continue;
        }

        uint64_t now_m = mono_ms();
        for (int i = 0; i < NUM_TUNERS; i++) {
            if (count_active_clients_for(i) > 0 && g_tuner[i].in_read) {
                uint64_t limit = (i == 0) ? 3000ULL : 2000ULL;
                if ((now_m - g_tuner[i].read_start_ms) > limit && (now_m - g_tuner[i].last_pkt_ms) > limit) {
                    add_log("[WATCHDOG%d] reader stalled >%llums, forcing reopen", i, (unsigned long long)limit);
                    g_tuner[i].reopen_req = 1;
                    pthread_kill(g_tuner[i].th, SIGUSR1);
                }
            }
        }
    }
    return NULL;
}

/* ---- UDP :40581 (LAN 内自動発見) ---- */
static void *udp_discovery_thread(void *arg) {
    (void)arg;
    int sfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sfd < 0) return NULL;
    int one = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(sfd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(UDP_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("[UDP] bind");
        close(sfd);
        return NULL;
    }
    add_log("[UDP] Discovery listening on 0.0.0.0:%d", UDP_PORT);

    char buf[256];
    while (g_running) {
        struct sockaddr_in cli;
        socklen_t clen = sizeof(cli);
        ssize_t n = recvfrom(sfd, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&cli, &clen);
        if (n <= 0) {
            if (errno == EINTR) continue;
            break;
        }
        buf[n] = '\0';
        if (strncmp(buf, "WEBOS_TUNER_DISCOVER", 20) == 0) {
            char act_mac[32], eth_mac[32], wlan_mac[32];
            get_mac_info(act_mac, sizeof(act_mac), eth_mac, sizeof(eth_mac), wlan_mac, sizeof(wlan_mac));
            char reply[320];
            int rlen = snprintf(reply, sizeof(reply),
                                "WEBOS_TUNER_HERE\tport=%d\thttp=%d\tmac=%s\tmac_eth=%s\tmac_wlan=%s\thost=%s\tver=1.0\n",
                                TCP_PORT, HTTP_PORT, act_mac, eth_mac, wlan_mac, g_hostname);
            sendto(sfd, reply, (size_t)rlen, 0, (struct sockaddr *)&cli, clen);
        }
    }
    close(sfd);
    return NULL;
}

static int parse_tuner_arg(const char *s, int def_val) {
    if (!s || !*s) return def_val;
    if (strncasecmp(s, "auto", 4) == 0 || strcmp(s, "-1") == 0) return -1;
    if (s[0] == '0') return 0;
    if (s[0] == '1') return 1;
    return def_val;
}

/* ---- TCP :40580 (BonDriver_WebOS 用プロトコル) ---- */
static void *tcp_client_worker(void *arg) {
    int fd = *(int *)arg;
    free(arg);

    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    struct timeval tv = {3, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    char buf[1024];
    int streamed = 0;
    int assigned_tuner = 1;

    while (g_running && !streamed) {
        size_t pos = 0;
        while (pos + 1 < sizeof(buf)) {
            ssize_t n = recv(fd, buf + pos, 1, 0);
            if (n <= 0) { close(fd); return NULL; }
            if (buf[pos] == '\n') break;
            pos++;
        }
        buf[pos] = '\0';
        while (pos > 0 && buf[pos - 1] == '\r') buf[--pos] = '\0';

        if (strcmp(buf, "HELLO") == 0) {
            char act_mac[32], eth_mac[32], wlan_mac[32], resp[256];
            get_mac_info(act_mac, sizeof(act_mac), eth_mac, sizeof(eth_mac), wlan_mac, sizeof(wlan_mac));
            int rl = snprintf(resp, sizeof(resp), "OK WEBOS_TUNER 1.0 mac=%s host=%s tuners=2\n", act_mac, g_hostname);
            send(fd, resp, (size_t)rl, MSG_NOSIGNAL);
        } else if (strcmp(buf, "CHLIST") == 0) {
            pthread_mutex_lock(&g_ch_mtx);
            for (int i = 0; i < g_chlist.count; i++) {
                const tv_channel_t *c = &g_chlist.items[i];
                char line[320];
                int ll = snprintf(line, sizeof(line), "%d\t%d\t%u\t%u\t%u\t%u\t%u\t%s\t%s\n",
                                  c->space, c->media, c->phy, c->freq_khz,
                                  c->tsid, c->onid, c->sid, c->ch_num, c->name);
                send(fd, line, (size_t)ll, MSG_NOSIGNAL);
            }
            pthread_mutex_unlock(&g_ch_mtx);
            const char *end = "END\n";
            send(fd, end, strlen(end), MSG_NOSIGNAL);
        } else if (strncmp(buf, "TUNE ", 5) == 0) {
            int media = 0, force = 1;
            uint32_t phy = 0, freq = 0, tsid = 0;
            char tstr[16] = {0};
            int argc_n = sscanf(buf + 5, "%d %u %u %u %d %15s", &media, &phy, &freq, &tsid, &force, tstr);
            if (argc_n >= 4) {
                int req_t = (argc_n >= 6) ? parse_tuner_arg(tstr, 1) : 1;
                int actual_t = 1;
                int tr = tune_tuner(req_t, media, phy, freq, (uint16_t)tsid, force, 0, 0, &actual_t);
                char resp[160];
                int rl;
                if (tr == 0) {
                    assigned_tuner = actual_t;
                    rl = snprintf(resp, sizeof(resp),
                                  "OK lock=0 tuner=%d media=%d phy=%u freq=%u tsid=%u\n",
                                  actual_t, media, phy, freq, tsid);
                } else if (tr == -2) {
                    rl = snprintf(resp, sizeof(resp), "ERR busy\n");
                } else {
                    rl = snprintf(resp, sizeof(resp), "ERR tune=%d\n", tr);
                }
                send(fd, resp, (size_t)rl, MSG_NOSIGNAL);
            } else {
                const char *er = "ERR usage: TUNE <media> <phy> <freq> <tsid> [force] [tuner]\n";
                send(fd, er, strlen(er), MSG_NOSIGNAL);
            }
        } else if (strncmp(buf, "BCAS ", 5) == 0) {
            char bcmd[1024], bresp[2048];
            snprintf(bcmd, sizeof(bcmd), "BCAS %s", buf + 5);
            if (bridge_cmd(bcmd, bresp, sizeof(bresp)) == 0) {
                send(fd, bresp, strlen(bresp), MSG_NOSIGNAL);
            } else {
                const char *er = "ERR bcas_bridge\n";
                send(fd, er, strlen(er), MSG_NOSIGNAL);
            }
        } else if (strncmp(buf, "STREAM", 6) == 0) {
            int dec = 1, req_t = assigned_tuner;
            char tstr[16] = {0};
            if (buf[6] == ' ') {
                int sn = sscanf(buf + 7, "%d %15s", &dec, tstr);
                if (sn >= 2) req_t = parse_tuner_arg(tstr, assigned_tuner);
            }
            if (add_stream_client(fd, dec ? 1 : 0, 0, req_t, 0, 0) >= 0) {
                const char *ok = "OK STREAMING\n";
                send(fd, ok, strlen(ok), MSG_NOSIGNAL);
                streamed = 1;
            } else {
                const char *er = "ERR max_clients\n";
                send(fd, er, strlen(er), MSG_NOSIGNAL);
                close(fd);
                return NULL;
            }
        } else if (strcmp(buf, "STAT") == 0) {
            char st[384];
            int sl = snprintf(st, sizeof(st),
                              "OK T1(m=%d,phy=%u,freq=%u,tsid=0x%04x,lock=%d,cli=%d,bytes=%llu) "
                              "T0(m=%d,phy=%u,freq=%u,tsid=0x%04x,lock=%d,cli=%d,bytes=%llu)\n",
                              g_tuner[1].cur_media, g_tuner[1].cur_phy, g_tuner[1].cur_freq, g_tuner[1].cur_tsid,
                              g_tuner[1].cur_lock, count_active_clients_for(1), (unsigned long long)g_tuner[1].bytes_total,
                              g_tuner[0].cur_media, g_tuner[0].cur_phy, g_tuner[0].cur_freq, g_tuner[0].cur_tsid,
                              g_tuner[0].cur_lock, count_active_clients_for(0), (unsigned long long)g_tuner[0].bytes_total);
            send(fd, st, (size_t)sl, MSG_NOSIGNAL);
        } else if (strcmp(buf, "QUIT") == 0) {
            close(fd);
            return NULL;
        } else {
            const char *er = "ERR unknown\n";
            send(fd, er, strlen(er), MSG_NOSIGNAL);
        }
    }
    return NULL;
}

static void *tcp_server_thread(void *arg) {
    (void)arg;
    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) return NULL;
    int one = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(TCP_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(sfd, 16) != 0) {
        perror("[TCP] bind/listen");
        close(sfd);
        return NULL;
    }
    add_log("[TCP] Listening on 0.0.0.0:%d", TCP_PORT);

    while (g_running) {
        int cfd = accept(sfd, NULL, NULL);
        if (cfd < 0) { if (errno == EINTR) continue; break; }
        int *pfd = (int *)malloc(sizeof(int));
        *pfd = cfd;
        pthread_t th;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&th, &attr, tcp_client_worker, pfd);
        pthread_attr_destroy(&attr);
    }
    close(sfd);
    return NULL;
}

/* ---- HTTP :40772 (Mirakurun 互換 API & ステータス画面) ---- */
static void http_send_json(int fd, int code, const char *status, const char *body) {
    size_t blen = body ? strlen(body) : 0;
    char hdr[512];
    int hlen = snprintf(hdr, sizeof(hdr),
                        "HTTP/1.1 %d %s\r\n"
                        "Content-Type: application/json; charset=utf-8\r\n"
                        "Content-Length: %zu\r\n"
                        "Connection: close\r\n\r\n",
                        code, status, blen);
    send(fd, hdr, (size_t)hlen, MSG_NOSIGNAL);
    if (blen > 0) send_all_timeout(fd, (const uint8_t *)body, blen);
}

/* DNS リバインディング対策: Host ヘッダ検証 */
static int is_valid_host_header(const char *req) {
    const char *h = strstr(req, "\r\nHost: ");
    if (!h) h = strstr(req, "\nHost: ");
    if (!h) h = strcasestr(req, "Host: ");
    if (!h) return 1; // Host ヘッダなし (HTTP/1.0 等) は許可
    h = strchr(h, ':') + 1;
    while (*h == ' ' || *h == '\t') h++;
    char host[256];
    int i = 0;
    while (*h && *h != '\r' && *h != '\n' && *h != ':' && i < (int)sizeof(host) - 1) {
        host[i++] = *h++;
    }
    host[i] = '\0';
    if (host[0] == '\0') return 1;
    if (strcmp(host, "localhost") == 0 || strcmp(host, "127.0.0.1") == 0) return 1;
    if (g_hostname[0] && strcasecmp(host, g_hostname) == 0) return 1;

    // IPv4 チェック (数字とドットのみ)
    int dots = 0, digits = 0, valid_ip = 1;
    for (int j = 0; host[j]; j++) {
        if (host[j] >= '0' && host[j] <= '9') {
            digits++;
        } else if (host[j] == '.') {
            dots++;
        } else {
            valid_ip = 0;
            break;
        }
    }
    if (valid_ip && dots == 3 && digits >= 4) return 1;

    // IPv6 リテラル (コロン含む、または [...] 形式)
    if (strchr(host, ':') != NULL || (host[0] == '[' && host[strlen(host) - 1] == ']')) return 1;

    return 0; // 不正な外部ドメイン (悪意ある DNS リバインディング等) を遮断
}

/* 同一端末内 (127.0.0.1) からの要求か。TV 内アプリ / tunerctl.sh のローカル操作を許可するため */
static int is_loopback_peer(int fd) {
    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);
    if (getpeername(fd, (struct sockaddr *)&sa, &sl) != 0) return 0;
    if (sa.sin_family != AF_INET) return 0;
    return ntohl(sa.sin_addr.s_addr) == INADDR_LOOPBACK;
}

static int parse_http_priority(const char *req) {
    const char *p = strcasestr(req, "X-Mirakurun-Priority:");
    if (!p) return 0;
    p += 21;
    while (*p == ' ' || *p == '\t') p++;
    int val = atoi(p);
    if (val < -128) val = -128;
    if (val > 128) val = 128;
    return val;
}

static void build_tuner_json_one(char *buf, size_t sz, int idx) {
    int hw_tid = (idx == 0) ? 1 : 0;
    int cli = count_active_clients_for(hw_tid);
    int b = (cli > 0);
    snprintf(buf, sz,
             "{\"index\":%d,\"name\":\"%s\",\"types\":[\"GR\",\"BS\",\"CS\"],"
             "\"command\":\"\",\"pid\":null,\"users\":[],"
             "\"isAvailable\":true,\"isRemote\":false,\"isFree\":%s,\"isUsing\":%s,\"isFault\":false,"
             "\"media\":%d,\"phy\":%u,\"freq\":%u,\"lock\":%d,\"clients\":%d}",
             idx,
             (idx == 0) ? "LG-webOS-Tuner1" : "LG-webOS-Tuner0-PVR",
             b ? "false" : "true", b ? "true" : "false",
             g_tuner[hw_tid].cur_media, g_tuner[hw_tid].cur_phy,
             g_tuner[hw_tid].cur_freq, g_tuner[hw_tid].cur_lock, cli);
}

/* チャンネル選局し HTTP MPEG2-TS ストリームを開始する（成功時は fd 所有権を client 配列へ移し 1 を返す） */
static int http_start_channel_stream(int fd, const tv_channel_t *ch, const char *query, int decode, uint16_t filter_sid, int priority) {
    if (!ch) {
        http_send_json(fd, 404, "Not Found", "{\"error\":\"channel not found\"}");
        return 0;
    }
    int req_t = -1;
    const char *tq = query ? strstr(query, "tuner=") : NULL;
    if (tq) req_t = parse_tuner_arg(tq + 6, -1);
    int actual_t = 1;
    int tr = tune_tuner(req_t, ch->media, ch->phy, ch->freq_khz, ch->tsid, 0, 0, priority, &actual_t);
    if (tr == -2) {
        http_send_json(fd, 503, "Service Unavailable", "{\"error\":\"tuner busy\"}");
        return 0;
    }
    if (tr != 0) {
        http_send_json(fd, 500, "Tune Failed", "{\"error\":\"tune failed\"}");
        return 0;
    }
    if (add_stream_client(fd, decode, 1, actual_t, filter_sid, priority) >= 0) {
        const char *hdr =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: video/MP2T\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: close\r\n\r\n";
        send(fd, hdr, strlen(hdr), MSG_NOSIGNAL);
        return 1;
    }
    http_send_json(fd, 503, "Too Many Clients", "{\"error\":\"max clients\"}");
    return 0;
}

static volatile int g_event_streams = 0;

static void http_handle_events_stream(int fd) {
    if (__sync_add_and_fetch(&g_event_streams, 1) > 4) {
        __sync_sub_and_fetch(&g_event_streams, 1);
        http_send_json(fd, 503, "Service Unavailable", "{\"error\":\"too many event streams\"}");
        return;
    }
    uint64_t cursor = tv_db_get_event_seq();
    int already_sent = 0;
    const char *hdr =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n\r\n[";
    if (send(fd, hdr, strlen(hdr), MSG_NOSIGNAL) <= 0) {
        __sync_sub_and_fetch(&g_event_streams, 1);
        return;
    }
    /* Mirakurun JsonObjectFrames 互換: TV EPG DB 差分イベントを逐次送信 */
    int idle_ticks = 0;
    while (g_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv = {2, 0};
        int r = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (r > 0) {
            char tmp[64];
            ssize_t rn = recv(fd, tmp, sizeof(tmp), MSG_DONTWAIT);
            if (rn <= 0) break;
        } else if (r < 0 && errno != EINTR) {
            break;
        }

        char *evs = tv_db_get_events_since(&cursor, &already_sent);
        if (evs) {
            int fail = send_all_timeout(fd, (const uint8_t *)evs, strlen(evs));
            free(evs);
            if (fail != 0) break;
            idle_ticks = 0;
        } else if (++idle_ticks >= 15) {
            if (send(fd, " ", 1, MSG_NOSIGNAL) <= 0) break;
            idle_ticks = 0;
        }
    }
    send(fd, "]", 1, MSG_NOSIGNAL);
    __sync_sub_and_fetch(&g_event_streams, 1);
}

static void *http_client_worker(void *arg) {
    int fd = *(int *)arg;
    free(arg);

    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    struct timeval tv = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char req[4096];
    ssize_t n = recv(fd, req, sizeof(req) - 1, 0);
    if (n <= 0) { close(fd); return NULL; }
    req[n] = '\0';

    if (!is_valid_host_header(req)) {
        const char *resp = "HTTP/1.1 403 Forbidden\r\nContent-Type: text/plain; charset=utf-8\r\nConnection: close\r\n\r\n403 Forbidden: Invalid Host header\r\n";
        send(fd, resp, strlen(resp), MSG_NOSIGNAL);
        close(fd);
        return NULL;
    }

    char method[16] = {0}, raw_url[1024] = {0};
    if (sscanf(req, "%15s %1023s", method, raw_url) != 2) { close(fd); return NULL; }

    char path[512] = {0}, query[512] = {0};
    char *qm = strchr(raw_url, '?');
    if (qm) {
        size_t plen = (size_t)(qm - raw_url);
        if (plen >= sizeof(path)) plen = sizeof(path) - 1;
        memcpy(path, raw_url, plen);
        snprintf(query, sizeof(query), "%s", qm + 1);
    } else {
        snprintf(path, sizeof(path), "%s", raw_url);
    }

    int decode = (strstr(query, "decode=0") != NULL) ? 0 : 1;
    int priority = parse_http_priority(req);

    if (strcmp(path, "/") == 0) {
        char act_mac[32], eth_mac[32], wlan_mac[32], ip_str[64];
        get_mac_info(act_mac, sizeof(act_mac), eth_mac, sizeof(eth_mac), wlan_mac, sizeof(wlan_mac));
        get_local_ip(ip_str, sizeof(ip_str), fd);
        int cli1 = count_active_clients_for(1);
        int cli0 = count_active_clients_for(0);
        int is_stby = (strstr(g_tv_power_state, "Standby") != NULL);
        char *html = (char *)malloc(32768);
        if (html) {
            int pos = snprintf(html, 32768,
                "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n"
                "<!DOCTYPE html><html lang='ja'><head><meta charset='utf-8'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<title>webOS DTV Tuner Bridge</title>"
                "<style>"
                "body{font-family:system-ui,-apple-system,sans-serif;max-width:920px;margin:20px auto;padding:0 16px;background:#0f172a;color:#e2e8f0;line-height:1.5}"
                ".hdr{display:flex;flex-wrap:wrap;justify-content:space-between;align-items:center;gap:8px;margin-bottom:12px}"
                "h2{margin:0;color:#38bdf8;font-size:22px}"
                ".dev-pill{display:inline-block;background:#1e293b;border:1px solid #334155;border-radius:999px;padding:3px 12px;font-size:12px;color:#cbd5e1;margin-top:4px}"
                ".auto-box{font-size:13px;color:#94a3b8;display:flex;align-items:center;gap:6px;user-select:none}"
                "a{color:#7dd3fc;text-decoration:none}a:hover{text-decoration:underline}"
                ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(270px,1fr));gap:12px;margin:12px 0}"
                ".card{background:#1e293b;border:1px solid #334155;border-radius:10px;padding:12px 15px}"
                ".lbl{font-size:12px;color:#94a3b8;margin-bottom:4px}.val{font-size:15px;font-weight:600}"
                ".on{color:#4ade80}.idle{color:#cbd5e1}.warn{color:#fbbf24}"
                ".btn-row{display:flex;flex-wrap:wrap;gap:8px;margin-top:8px}"
                ".btn{border:1px solid #475569;background:#334155;color:#f8fafc;padding:6px 12px;border-radius:7px;font-size:13px;font-weight:600;cursor:pointer;transition:0.15s}"
                ".btn:hover{background:#475569;border-color:#7dd3fc}"
                ".btn.active{background:#15803d;border-color:#4ade80;color:#fff}"
                ".btn.sub{background:#1e293b}"
                "details{margin-top:12px;background:#1e293b;border:1px solid #334155;border-radius:8px;padding:8px 12px}"
                "summary{cursor:pointer;font-size:13px;color:#94a3b8;user-select:none}"
                "ul{display:flex;flex-wrap:wrap;gap:8px 16px;padding-left:18px;margin:8px 0 2px;font-size:13px}"
                "pre{background:#020617;color:#86efac;border:1px solid #334155;padding:12px;border-radius:8px;overflow:auto;font-size:13px;max-height:320px;margin:0}"
                "footer{margin-top:16px;padding-top:10px;border-top:1px solid #334155;font-size:13px;color:#94a3b8}"
                "</style></head><body>"
                "<div class='hdr'><div>"
                "<h2>webOS DTV Tuner Bridge</h2>"
                "<div class='dev-pill' id='dev-info'>%s %s | webOS %s | FW %s</div>"
                "</div>"
                "<label class='auto-box'><input type='checkbox' id='chk-auto' checked> 自動更新 3秒 <span id='last-up'></span></label>"
                "</div>"
                "<div class='grid'>"
                "<div class='card'><div class='lbl'>ネットワーク / 接続先</div>"
                "<div class='val' id='v-ip'>IP: %s | TCP:40580 | HTTP:40772</div>"
                "<div class='lbl' style='margin-top:4px' id='v-host'>Host: %s | MAC: %s</div></div>"
                "<div class='card'><div class='lbl'>配信サマリー</div>"
                "<div class='val' id='v-sum'>Channels: %d | Clients: %d | Sent: %.2f MB</div>"
                "<div class='lbl' style='margin-top:4px'>TV電源状態: <b id='v-tvst' class='%s'>%s</b></div></div>"
                "</div>"
                "<div class='grid'>"
                "<div class='card'><div class='lbl'>電源待機モード</div>"
                "<div class='btn-row'>"
                "<button class='btn %s' id='b-always' onclick=\"setPower('mode=always')\">ActiveStandby維持</button>"
                "<button class='btn %s' id='b-ondemand' onclick=\"setPower('mode=ondemand')\">スリープ</button>"
                "<button class='btn %s' id='b-shutdown' onclick=\"setPower('mode=shutdown')\">完全シャットダウン</button>"
                "</div>"
                "<div class='lbl' id='v-pmode-desc' style='margin-top:6px'>%s</div></div>"
                "<div class='card'><div class='lbl'>TV 画面操作</div>"
                "<div class='btn-row'>"
                "<button class='btn sub' onclick=\"setPower('action=on')\">TV ON</button>"
                "<button class='btn sub' onclick=\"setPower('action=standby')\">TV OFF</button>"
                "</div>"
                "<div class='lbl %s' id='v-tvop-note' style='margin-top:6px'>%s</div></div>"
                "</div>"
                "<div class='grid'>"
                "<div class='card'><div class='lbl'>Tuner 1 フルTS / EXT_INPUT1</div>"
                "<div class='val %s' id='v-t1'>%s</div>"
                "<div class='lbl' id='v-t1-sub'>media=%d phy=%u freq=%u lock=%d cli=%d</div></div>"
                "<div class='card'><div class='lbl'>Tuner 0 PVR / EXT_INPUT0</div>"
                "<div class='val %s' id='v-t0'>%s</div>"
                "<div class='lbl' id='v-t0-sub'>media=%d phy=%u freq=%u lock=%d cli=%d</div></div>"
                "</div>"
                "<h3 style='margin:10px 0 6px;font-size:15px'>Recent Log</h3><pre id='log-pre'>",
                g_tv_name[0] ? g_tv_name : "LG webOS TV",
                g_tv_model[0] ? g_tv_model : "-",
                g_tv_webos[0] ? g_tv_webos : "-",
                g_tv_fw[0] ? g_tv_fw : "-",
                ip_str, g_hostname, act_mac,
                g_chlist.count, count_active_clients(), (double)total_bytes_sent() / (1024.0 * 1024.0),
                is_stby ? "warn" : "on",
                is_stby ? "画面消灯中・裏待機 Active Standby" : "画面点灯中 Active",
                (g_power_mode == 1) ? "active" : "",
                (g_power_mode == 0) ? "active" : "",
                (g_power_mode == 2) ? "active" : "",
                (g_power_mode == 1) ? "裏でチューナーが常時稼働します" :
                    ((g_power_mode == 0) ? "配信終了後にチューナーOFFでスリープします" : "配信終了後に完全シャットダウンします"),
                (g_power_mode == 1) ? "" : "warn",
                (g_power_mode == 1) ? "TV OFF 時は画面だけ消灯しサーバーは稼働を続けます" :
                    ((g_power_mode == 0) ? "注意: TV OFF するとこの画面とサーバーも停止します" : "注意: TV OFF すると完全停止します（復帰にはリモコンまたはWOLが必要で起動に数十秒かかります）"),
                cli1 > 0 ? "on" : "idle", cli1 > 0 ? "配信中" : "待機中",
                g_tuner[1].cur_media, g_tuner[1].cur_phy, g_tuner[1].cur_freq, g_tuner[1].cur_lock, cli1,
                cli0 > 0 ? "on" : "idle", cli0 > 0 ? "配信中" : "待機中",
                g_tuner[0].cur_media, g_tuner[0].cur_phy, g_tuner[0].cur_freq, g_tuner[0].cur_lock, cli0);

            pthread_mutex_lock(&g_log_mtx);
            int start = (g_log_count == LOG_RING_LINES) ? g_log_head : 0;
            for (int i = 0; i < g_log_count && pos + 2048 < 32768; i++) {
                int idx = (start + i) % LOG_RING_LINES;
                pos += snprintf(html + pos, (size_t)(32768 - pos), "%s\n", g_log_ring[idx]);
            }
            pthread_mutex_unlock(&g_log_mtx);
            pos += snprintf(html + pos, (size_t)(32768 - pos),
                "</pre>"
                "<details><summary>API アクセス先一覧</summary>"
                "<ul><li><a href='/api/status'>/api/status</a></li><li><a href='/api/tuners'>/api/tuners</a></li>"
                "<li><a href='/api/channels'>/api/channels</a></li><li><a href='/api/services'>/api/services</a></li>"
                "<li><a href='/api/programs'>/api/programs</a></li><li><a href='/api/log'>/api/log</a></li>"
                "<li><a href='/api/reload'>/api/reload</a></li></ul>"
                "</details>"
                "<footer>問い合わせ先 GitHub: "
                "<a href='https://github.com/fnoji' target='_blank' rel='noopener'>https://github.com/fnoji</a>"
                "</footer>"
                "<script>"
                "var chCount=%d;"
                "function setPower(q){fetch('/api/power?'+q,{method:'POST'}).then(function(){setTimeout(refreshUI,250)});}"
                "function refreshUI(){"
                "fetch('/api/status').then(function(r){return r.json()}).then(function(s){"
                "if(s.modelName)document.getElementById('dev-info').textContent=(s.deviceName||'LG webOS TV')+' '+s.modelName+' | webOS '+(s.webosVersion||'-')+' | FW '+(s.firmwareVersion||'-');"
                "var mb=((s.bytes||0)/(1024*1024)).toFixed(2);"
                "document.getElementById('v-sum').textContent='Channels: '+chCount+' | Clients: '+(s.streamCount||0)+' | Sent: '+mb+' MB';"
                "var stby=(s.tvState||'').indexOf('Standby')>=0;"
                "var elSt=document.getElementById('v-tvst');"
                "elSt.textContent=stby?'画面消灯中・裏待機 Active Standby':'画面点灯中 '+(s.tvState||'Active');"
                "elSt.className=stby?'warn':'on';"
                "var pm=s.powerMode||'always';"
                "document.getElementById('b-always').className='btn'+(pm==='always'?' active':'');"
                "document.getElementById('b-ondemand').className='btn'+(pm==='ondemand'?' active':'');"
                "document.getElementById('b-shutdown').className='btn'+(pm==='shutdown'?' active':'');"
                "document.getElementById('v-pmode-desc').textContent=pm==='always'?'裏でチューナーが常時稼働します':(pm==='ondemand'?'配信終了後にチューナーOFFでスリープします':'配信終了後に完全シャットダウンします');"
                "var nt=document.getElementById('v-tvop-note');"
                "nt.className='lbl'+(pm==='always'?'':' warn');"
                "nt.textContent=pm==='always'?'TV OFF 時は画面だけ消灯しサーバーは稼働を続けます':(pm==='ondemand'?'注意: TV OFF するとこの画面とサーバーも停止します':'注意: TV OFF すると完全停止します（復帰にはリモコンまたはWOLが必要で起動に数十秒かかります）');"
                "var d=new Date();"
                "document.getElementById('last-up').textContent=d.toTimeString().slice(0,8);"
                "}).catch(function(){});"
                "fetch('/api/tuners').then(function(r){return r.json()}).then(function(t){"
                "if(t&&t.length>=2){"
                "var u=function(k,o){var v=document.getElementById(k),sb=document.getElementById(k+'-sub');"
                "v.textContent=o.isUsing?'配信中':'待機中';v.className='val '+(o.isUsing?'on':'idle');"
                "sb.textContent='media='+o.media+' phy='+o.phy+' freq='+o.freq+' lock='+o.lock+' cli='+o.clients;};"
                "u('v-t1',t[0]);u('v-t0',t[1]);}"
                "}).catch(function(){});"
                "fetch('/api/log').then(function(r){return r.text()}).then(function(txt){"
                "var p=document.getElementById('log-pre');if(txt){p.textContent=txt;p.scrollTop=p.scrollHeight;}"
                "}).catch(function(){});"
                "}"
                "setInterval(function(){if(document.getElementById('chk-auto').checked)refreshUI();},3000);"
                "</script></body></html>",
                g_chlist.count);
            send_all_timeout(fd, (const uint8_t *)html, (size_t)pos);
            free(html);
        }
        close(fd);
        return NULL;
    } else if (strcmp(path, "/api/log") == 0) {
        char *txt = (char *)malloc(16384);
        if (txt) {
            int pos = 0;
            pthread_mutex_lock(&g_log_mtx);
            int start = (g_log_count == LOG_RING_LINES) ? g_log_head : 0;
            for (int i = 0; i < g_log_count && pos + 256 < 16384; i++) {
                int idx = (start + i) % LOG_RING_LINES;
                pos += snprintf(txt + pos, (size_t)(16384 - pos), "%s\n", g_log_ring[idx]);
            }
            pthread_mutex_unlock(&g_log_mtx);
            char hdr[256];
            int hl = snprintf(hdr, sizeof(hdr),
                              "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", pos);
            send(fd, hdr, (size_t)hl, MSG_NOSIGNAL);
            if (pos > 0) send_all_timeout(fd, (const uint8_t *)txt, (size_t)pos);
            free(txt);
        }
    } else if (strcmp(path, "/api/version") == 0) {
        http_send_json(fd, 200, "OK", "{\"current\":\"3.9.0-webos-lite\",\"latest\":\"3.9.0-webos-lite\"}");
    } else if (strcmp(path, "/api/config/server") == 0) {
        http_send_json(fd, 200, "OK", "{\"path\":\"/var/run/mirakurun.sock\",\"port\":40772}");
    } else if (strcmp(path, "/api/events/stream") == 0) {
        http_handle_events_stream(fd);
    } else if (strcmp(path, "/api/status") == 0) {
        char act_mac[32], eth_mac[32], wlan_mac[32], ip_str[64], st[800];
        get_mac_info(act_mac, sizeof(act_mac), eth_mac, sizeof(eth_mac), wlan_mac, sizeof(wlan_mac));
        get_local_ip(ip_str, sizeof(ip_str), fd);
        snprintf(st, sizeof(st),
                 "{\"version\":\"3.9.0-webos-lite\",\"tunerCount\":2,\"streamCount\":%d,\"epgReady\":true,"
                 "\"powerMode\":\"%s\",\"tvState\":\"%s\",\"deviceName\":\"%s\",\"modelName\":\"%s\",\"webosVersion\":\"%s\",\"firmwareVersion\":\"%s\","
                 "\"ip\":\"%s\",\"mac\":\"%s\",\"host\":\"%s\",\"bytes\":%llu}",
                 count_active_clients(), power_mode_str(g_power_mode),
                 g_tv_power_state, g_tv_name, g_tv_model, g_tv_webos, g_tv_fw,
                 ip_str, act_mac, g_hostname, (unsigned long long)total_bytes_sent());
        http_send_json(fd, 200, "OK", st);
    } else if (strcmp(path, "/api/power") == 0) {
        if ((strstr(query, "mode=") != NULL || strstr(query, "action=") != NULL) &&
            strcmp(method, "POST") != 0 && !is_loopback_peer(fd)) {
            http_send_json(fd, 405, "Method Not Allowed", "{\"error\":\"POST required for power state changes\"}");
            close(fd);
            return NULL;
        }
        if (strstr(query, "mode=ondemand") != NULL) {
            save_power_mode(0);
            add_log("[POWER] Mode changed -> ondemand");
        } else if (strstr(query, "mode=shutdown") != NULL) {
            save_power_mode(2);
            add_log("[POWER] Mode changed -> shutdown");
        } else if (strstr(query, "mode=always") != NULL) {
            save_power_mode(1);
            add_log("[POWER] Mode changed -> always");
        }
        if (strstr(query, "action=standby") != NULL) {
            g_pending_power_cmd = 1;
            add_log("[POWER] Requested TV OFF (mode=%s)", power_mode_str(g_power_mode));
        } else if (strstr(query, "action=on") != NULL) {
            g_pending_power_cmd = 2;
            add_log("[POWER] Requested TV ON");
        }
        char pw[192];
        snprintf(pw, sizeof(pw), "{\"ok\":true,\"powerMode\":\"%s\",\"tvState\":\"%s\"}",
                 power_mode_str(g_power_mode), g_tv_power_state);
        http_send_json(fd, 200, "OK", pw);
    } else if (strcmp(path, "/api/tuners") == 0) {
        char t0[512], t1[512], tun[1152];
        build_tuner_json_one(t0, sizeof(t0), 0);
        build_tuner_json_one(t1, sizeof(t1), 1);
        snprintf(tun, sizeof(tun), "[%s,%s]", t0, t1);
        http_send_json(fd, 200, "OK", tun);
    } else if (strncmp(path, "/api/tuners/", 12) == 0) {
        char *ep = NULL;
        long idx = strtol(path + 12, &ep, 10);
        if ((idx == 0 || idx == 1) && (!ep || *ep == '\0' || strcmp(ep, "/") == 0)) {
            char tbuf[512];
            build_tuner_json_one(tbuf, sizeof(tbuf), (int)idx);
            http_send_json(fd, 200, "OK", tbuf);
        } else {
            http_send_json(fd, 404, "Not Found", "{\"error\":\"tuner not found\"}");
        }
    } else if (strcmp(path, "/api/channels") == 0) {
        pthread_mutex_lock(&g_ch_mtx);
        char *j = tv_db_build_channels_json(&g_chlist);
        pthread_mutex_unlock(&g_ch_mtx);
        http_send_json(fd, 200, "OK", j ? j : "[]");
        free(j);
    } else if (strcmp(path, "/api/services") == 0) {
        pthread_mutex_lock(&g_ch_mtx);
        char *j = tv_db_build_services_json(&g_chlist);
        pthread_mutex_unlock(&g_ch_mtx);
        http_send_json(fd, 200, "OK", j ? j : "[]");
        free(j);
    } else if (strcmp(path, "/api/programs") == 0) {
        int refresh = (strstr(query, "refresh=1") != NULL) ? 1 : 0;
        char *j = tv_db_get_programs_json(0, refresh);
        http_send_json(fd, 200, "OK", j ? j : "[]");
        free(j);
    } else if (strcmp(path, "/api/reload") == 0) {
        tv_channel_list_t nlist;
        if (tv_db_load_channels(&nlist) == 0) {
            pthread_mutex_lock(&g_ch_mtx);
            tv_db_free_channels(&g_chlist);
            g_chlist = nlist;
            pthread_mutex_unlock(&g_ch_mtx);
            add_log("[INIT] Reloaded %d channels from DB8", g_chlist.count);
            http_send_json(fd, 200, "OK", "{\"ok\":true}");
        } else {
            http_send_json(fd, 500, "Error", "{\"ok\":false}");
        }
    } else if (strncmp(path, "/api/programs/", 14) == 0) {
        char *endp = NULL;
        uint64_t pid = strtoull(path + 14, &endp, 10);
        if (pid == 0) {
            http_send_json(fd, 404, "Not Found", "{\"error\":\"invalid program id\"}");
        } else if (endp && (strcmp(endp, "/stream") == 0 || strcmp(endp, "/stream/") == 0)) {
            uint64_t mir_sid = pid / 100000ULL;
            tv_channel_t ch_copy;
            int found = 0;
            pthread_mutex_lock(&g_ch_mtx);
            const tv_channel_t *target = tv_db_find_by_sid(&g_chlist, mir_sid);
            if (target) { ch_copy = *target; found = 1; }
            pthread_mutex_unlock(&g_ch_mtx);
            if (http_start_channel_stream(fd, found ? &ch_copy : NULL, query, decode, found ? ch_copy.sid : 0, priority)) return NULL;
        } else if (!endp || *endp == '\0' || strcmp(endp, "/") == 0) {
            char *pj = tv_db_get_program_by_id_json(pid);
            if (pj) {
                http_send_json(fd, 200, "OK", pj);
                free(pj);
            } else {
                http_send_json(fd, 404, "Not Found", "{\"error\":\"program not found\"}");
            }
        } else {
            http_send_json(fd, 404, "Not Found", "{\"error\":\"not found\"}");
        }
    } else if (strncmp(path, "/api/services/", 14) == 0) {
        char *endp = NULL;
        uint64_t sid = strtoull(path + 14, &endp, 10);
        tv_channel_t ch_copy;
        int found = 0;
        pthread_mutex_lock(&g_ch_mtx);
        const tv_channel_t *target = (sid > 0) ? tv_db_find_by_sid(&g_chlist, sid) : NULL;
        if (target) { ch_copy = *target; found = 1; }
        pthread_mutex_unlock(&g_ch_mtx);

        if (!found) {
            http_send_json(fd, 404, "Not Found", "{\"error\":\"service not found\"}");
        } else if (endp && (strcmp(endp, "/stream") == 0 || strcmp(endp, "/stream/") == 0)) {
            if (http_start_channel_stream(fd, &ch_copy, query, decode, ch_copy.sid, priority)) return NULL;
        } else if (endp && (strcmp(endp, "/programs") == 0 || strcmp(endp, "/programs/") == 0)) {
            char *pj = tv_db_get_programs_json(sid, 0);
            http_send_json(fd, 200, "OK", pj ? pj : "[]");
            free(pj);
        } else if (endp && (strcmp(endp, "/logo") == 0 || strcmp(endp, "/logo/") == 0)) {
            http_send_json(fd, 404, "Not Found", "{\"error\":\"logo not available\"}");
        } else if (!endp || *endp == '\0' || strcmp(endp, "/") == 0) {
            char *sj = tv_db_build_service_json_one(&ch_copy);
            http_send_json(fd, 200, "OK", sj ? sj : "{}");
            free(sj);
        } else {
            http_send_json(fd, 404, "Not Found", "{\"error\":\"not found\"}");
        }
    } else if (strncmp(path, "/api/channels/", 14) == 0) {
        char tbuf[16] = {0}, cbuf[64] = {0}, rest[32] = {0};
        if (sscanf(path + 14, "%15[^/]/%63[^/]%31s", tbuf, cbuf, rest) >= 2 &&
            (rest[0] == '\0' || strcmp(rest, "/stream") == 0 || strcmp(rest, "/stream/") == 0)) {
            tv_channel_t ch_copy;
            int found = 0;
            pthread_mutex_lock(&g_ch_mtx);
            const tv_channel_t *target = tv_db_find_by_mirakurun(&g_chlist, tbuf, cbuf);
            if (target) { ch_copy = *target; found = 1; }
            pthread_mutex_unlock(&g_ch_mtx);
            if (http_start_channel_stream(fd, found ? &ch_copy : NULL, query, decode, 0, priority)) return NULL;
        } else {
            http_send_json(fd, 404, "Not Found", "{\"error\":\"channel not found\"}");
        }
    } else {
        http_send_json(fd, 404, "Not Found", "{\"error\":\"not found\"}");
    }

    close(fd);
    return NULL;
}

static void *http_server_thread(void *arg) {
    (void)arg;
    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) return NULL;
    int one = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(HTTP_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(sfd, 16) != 0) {
        perror("[HTTP] bind/listen");
        close(sfd);
        return NULL;
    }
    add_log("[HTTP] Listening on 0.0.0.0:%d", HTTP_PORT);

    while (g_running) {
        int cfd = accept(sfd, NULL, NULL);
        if (cfd < 0) { if (errno == EINTR) continue; break; }
        int *pfd = (int *)malloc(sizeof(int));
        *pfd = cfd;
        pthread_t th;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&th, &attr, http_client_worker, pfd);
        pthread_attr_destroy(&attr);
    }
    close(sfd);
    return NULL;
}

static void on_sig(int s) {
    (void)s;
    g_running = 0;
}

/* ---- ActiveStandby 維持 (libluna-service2 直接接続による同一 LS2 コネクション応答) ---- */
typedef void LSHandle_t;
typedef void LSMessage_t;
typedef unsigned long LSMessageToken_t;
typedef int (*LSFilterFunc_t)(LSHandle_t *sh, LSMessage_t *reply, void *ctx);

static int (*fn_LSErrorInit)(void *lserror) = NULL;
static void (*fn_LSErrorFree)(void *lserror) = NULL;
static void (*fn_LSErrorPrint)(void *lserror, FILE *out) = NULL;
static bool (*fn_LSRegister)(const char *name, LSHandle_t **sh, void *lserror) = NULL;
static bool (*fn_LSUnregister)(LSHandle_t *sh, void *lserror) = NULL;
static bool (*fn_LSGmainAttach)(LSHandle_t *sh, void *mainLoop, void *lserror) = NULL;
static bool (*fn_LSCall)(LSHandle_t *sh, const char *uri, const char *payload,
                         LSFilterFunc_t cb, void *ctx, LSMessageToken_t *tok, void *lserror) = NULL;
static bool (*fn_LSCallOneReply)(LSHandle_t *sh, const char *uri, const char *payload,
                                 LSFilterFunc_t cb, void *ctx, LSMessageToken_t *tok, void *lserror) = NULL;
static const char *(*fn_LSMessageGetPayload)(LSMessage_t *message) = NULL;

static void *(*fn_g_main_loop_new)(void *context, int is_running) = NULL;
static void (*fn_g_main_loop_run)(void *loop) = NULL;
static void (*fn_g_main_loop_quit)(void *loop) = NULL;
static void (*fn_g_main_loop_unref)(void *loop) = NULL;
static unsigned int (*fn_g_timeout_add)(unsigned int interval, int (*func)(void *), void *data) = NULL;

static void *g_pwr_loop = NULL;
static int g_held_active_standby = 0;
static time_t g_pwr_idle_since = 0;

/**
 * JSON 文字列から指定キーの文字列値またはトークン値を取り出す
 */
static int extract_json_field(const char *json, const char *key, char *out, size_t out_sz) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *k = strstr(json, pat);
    if (!k) return 0;
    const char *c = strchr(k + strlen(pat), ':');
    if (!c) return 0;
    c++;
    while (*c == ' ' || *c == '\t') c++;
    size_t w = 0;
    if (*c == '"') {
        c++;
        while (*c && *c != '"' && w + 1 < out_sz) out[w++] = *c++;
    } else {
        while (*c && *c != ',' && *c != '}' && *c != ' ' && *c != '\r' && *c != '\n' && w + 1 < out_sz) {
            out[w++] = *c++;
        }
    }
    out[w] = '\0';
    return (w > 0);
}

static int extract_timestamp_token(const char *json, char *out, size_t out_sz) {
    return extract_json_field(json, "timestamp", out, out_sz);
}

static int on_devicename_reply(LSHandle_t *sh, LSMessage_t *reply, void *ctx) {
    (void)sh; (void)ctx;
    const char *payload = reply ? fn_LSMessageGetPayload(reply) : NULL;
    if (!payload) return 1;
    char dname[64] = {0};
    if (extract_json_field(payload, "deviceName", dname, sizeof(dname))) {
        snprintf(g_tv_name, sizeof(g_tv_name), "%s", dname);
    }
    return 1;
}

static int on_sysinfo_reply(LSHandle_t *sh, LSMessage_t *reply, void *ctx) {
    (void)sh; (void)ctx;
    const char *payload = reply ? fn_LSMessageGetPayload(reply) : NULL;
    if (!payload) return 1;
    char model[64] = {0}, fw[32] = {0}, sdk[32] = {0};
    if (extract_json_field(payload, "modelName", model, sizeof(model))) {
        snprintf(g_tv_model, sizeof(g_tv_model), "%s", model);
    }
    if (extract_json_field(payload, "firmwareVersion", fw, sizeof(fw))) {
        snprintf(g_tv_fw, sizeof(g_tv_fw), "%s", fw);
    }
    if (extract_json_field(payload, "sdkVersion", sdk, sizeof(sdk))) {
        if (g_tv_release[0]) {
            snprintf(g_tv_webos, sizeof(g_tv_webos), "%s (%s)", sdk, g_tv_release);
        } else {
            snprintf(g_tv_webos, sizeof(g_tv_webos), "%s", sdk);
        }
    }
    return 1;
}

static int on_power_state_sub(LSHandle_t *sh, LSMessage_t *reply, void *ctx) {
    (void)sh; (void)ctx;
    const char *payload = reply ? fn_LSMessageGetPayload(reply) : NULL;
    if (!payload) return 1;
    char st[32] = {0};
    if (extract_json_field(payload, "state", st, sizeof(st))) {
        if (strcmp(g_tv_power_state, st) != 0) {
            snprintf(g_tv_power_state, sizeof(g_tv_power_state), "%s", st);
            add_log("[POWER] TV state -> %s", g_tv_power_state);
        }
        if (strstr(st, "Standby") != NULL) {
            g_held_active_standby = 1;
        } else if (strcmp(st, "Active") == 0) {
            g_held_active_standby = 0;
            g_pwr_idle_since = 0;
        }
    }
    return 1;
}

static int on_ls2_ack_reply(LSHandle_t *sh, LSMessage_t *reply, void *ctx) {
    (void)sh;
    const char *tag = (const char *)ctx;
    const char *payload = reply ? fn_LSMessageGetPayload(reply) : NULL;
    if (payload) {
        add_log("[POWER] %s reply: %s", tag ? tag : "LS2", payload);
    }
    return 1;
}

static int on_get_power_state_reply(LSHandle_t *sh, LSMessage_t *reply, void *ctx) {
    (void)ctx;
    const char *payload = reply ? fn_LSMessageGetPayload(reply) : NULL;
    if (payload && strstr(payload, "Active Standby") != NULL) {
        prune_dead_clients();
        if (count_active_clients() == 0) {
            unsigned char err[256];
            memset(err, 0, sizeof(err));
            fn_LSErrorInit(err);
            add_log("[POWER] Streaming finished during ActiveStandby -> triggering powerOff");
            fn_LSCallOneReply(sh,
                              "luna://com.webos.service.tvpower/power/powerOff",
                              "{\"reason\":\"stopRecording\"}",
                              on_ls2_ack_reply, (void *)"powerOff", NULL, err);
            fn_LSErrorFree(err);
        }
    }
    return 1;
}

/**
 * tvpowerd からの registerActiveStandbyRequest 通知を受け、同一 LS2 コネクション上で応答する
 */
static int on_active_standby_req(LSHandle_t *sh, LSMessage_t *reply, void *ctx) {
    (void)ctx;
    if (!reply) return 1;
    const char *payload = fn_LSMessageGetPayload(reply);
    if (!payload) return 1;

    if (strstr(payload, "\"subscribed\"") != NULL && strstr(payload, "\"timestamp\"") == NULL) {
        add_log("[POWER] Registered ActiveStandbyRequest hook (native LS2, mode=%s).",
                power_mode_str(g_power_mode));
        return 1;
    }

    char ts[64] = {0};
    if (extract_timestamp_token(payload, ts, sizeof(ts))) {
        prune_dead_clients();
        int n_cli = count_active_clients();
        char resp_json[256];
        int want_active_standby = (g_power_mode == 1 || n_cli > 0) ? 1 : 0;
        snprintf(resp_json, sizeof(resp_json),
                 "{\"clientName\":\"webostuner\",\"ack\":%s,\"timestamp\":\"%s\"}",
                 want_active_standby ? "true" : "false", ts);

        unsigned char err[256];
        memset(err, 0, sizeof(err));
        fn_LSErrorInit(err);
        bool ok = fn_LSCallOneReply(sh,
                                    "luna://com.webos.service.tvpower/power/responseActiveStandbyRequest",
                                    resp_json,
                                    on_ls2_ack_reply,
                                    (void *)(want_active_standby ? "ActiveStandbyACK" : "ActiveStandbyNACK"),
                                    NULL, err);
        fn_LSErrorFree(err);

        if (want_active_standby) {
            add_log("[POWER] ActiveStandbyRequest -> ACK (clients=%d, mode=%s, ok=%d)",
                    n_cli, power_mode_str(g_power_mode), ok ? 1 : 0);
            g_held_active_standby = 1;
            g_pwr_idle_since = 0;
        } else {
            add_log("[POWER] ActiveStandbyRequest -> NACK (0 clients, ok=%d)", ok ? 1 : 0);
            g_held_active_standby = 0;
            g_pwr_idle_since = 0;
        }
    }
    return 1;
}

/**
 * tvpowerd からの registerSuspendRequest 通知を受け、shutdown モード時は NACK して完全シャットダウンへ進める
 */
static int on_suspend_req(LSHandle_t *sh, LSMessage_t *reply, void *ctx) {
    (void)ctx;
    if (!reply) return 1;
    const char *payload = fn_LSMessageGetPayload(reply);
    if (!payload) return 1;
    if (strstr(payload, "\"subscribed\"") != NULL && strstr(payload, "\"timestamp\"") == NULL) {
        return 1;
    }
    char ts[64] = {0};
    if (extract_timestamp_token(payload, ts, sizeof(ts))) {
        prune_dead_clients();
        int n_cli = count_active_clients();
        int allow_suspend = (g_power_mode == 2 && n_cli == 0) ? 0 : 1;
        char resp_json[256];
        snprintf(resp_json, sizeof(resp_json),
                 "{\"clientName\":\"webostuner\",\"ack\":%s,\"timestamp\":\"%s\"}",
                 allow_suspend ? "true" : "false", ts);

        unsigned char err[256];
        memset(err, 0, sizeof(err));
        fn_LSErrorInit(err);
        fn_LSCallOneReply(sh,
                          "luna://com.webos.service.tvpower/power/responseSuspendRequest",
                          resp_json,
                          on_ls2_ack_reply,
                          (void *)(allow_suspend ? "SuspendACK" : "SuspendNACK-ColdOff"),
                          NULL, err);
        fn_LSErrorFree(err);
        add_log("[POWER] SuspendRequest -> %s (mode=%s, clients=%d)",
                allow_suspend ? "ACK" : "NACK (Cold Shutdown)", power_mode_str(g_power_mode), n_cli);
    }
    return 1;
}

static int on_power_timer(void *data) {
    LSHandle_t *sh = (LSHandle_t *)data;
    if (!g_running) {
        if (g_pwr_loop) fn_g_main_loop_quit(g_pwr_loop);
        return 0;
    }
    int pcmd = g_pending_power_cmd;
    if (pcmd != 0) {
        g_pending_power_cmd = 0;
        unsigned char err[256];
        memset(err, 0, sizeof(err));
        fn_LSErrorInit(err);
        if (pcmd == 1) {
            fn_LSCallOneReply(sh,
                              "luna://com.webos.service.tvpower/power/powerOff",
                              "{\"reason\":\"remoteKey\"}",
                              on_ls2_ack_reply, (void *)"powerOff", NULL, err);
        } else if (pcmd == 2) {
            fn_LSCallOneReply(sh,
                              "luna://com.webos.service.tvpower/power/powerOn",
                              "{\"reason\":\"remoteKey\"}",
                              on_ls2_ack_reply, (void *)"powerOn", NULL, err);
        }
        fn_LSErrorFree(err);
    }
    if (g_power_mode != 1 && g_held_active_standby) {
        prune_dead_clients();
        int n_cli = count_active_clients();
        time_t now = time(NULL);
        if (n_cli > 0) {
            g_pwr_idle_since = 0;
        } else if (g_pwr_idle_since == 0) {
            g_pwr_idle_since = now;
        } else if ((now - g_pwr_idle_since) >= 5) {
            g_held_active_standby = 0;
            g_pwr_idle_since = 0;
            unsigned char err[256];
            memset(err, 0, sizeof(err));
            fn_LSErrorInit(err);
            fn_LSCallOneReply(sh,
                              "luna://com.webos.service.tvpower/power/getPowerState",
                              "{}",
                              on_get_power_state_reply, NULL, NULL, err);
            fn_LSErrorFree(err);
        }
    }
    return 1;
}

static int load_ls2_and_glib(void) {
    void *h_glib = dlopen("/usr/lib/libglib-2.0.so.0", RTLD_NOW | RTLD_GLOBAL);
    void *h_ls2  = dlopen("/usr/lib/libluna-service2.so.3", RTLD_NOW);
    if (!h_glib || !h_ls2) {
        add_log("[POWER] dlopen failed: glib=%p ls2=%p (%s)", h_glib, h_ls2, dlerror());
        return -1;
    }
    fn_g_main_loop_new   = dlsym(h_glib, "g_main_loop_new");
    fn_g_main_loop_run   = dlsym(h_glib, "g_main_loop_run");
    fn_g_main_loop_quit  = dlsym(h_glib, "g_main_loop_quit");
    fn_g_main_loop_unref = dlsym(h_glib, "g_main_loop_unref");
    fn_g_timeout_add     = dlsym(h_glib, "g_timeout_add");

    fn_LSErrorInit          = dlsym(h_ls2, "LSErrorInit");
    fn_LSErrorFree          = dlsym(h_ls2, "LSErrorFree");
    fn_LSErrorPrint         = dlsym(h_ls2, "LSErrorPrint");
    fn_LSRegister           = dlsym(h_ls2, "LSRegister");
    fn_LSUnregister         = dlsym(h_ls2, "LSUnregister");
    fn_LSGmainAttach        = dlsym(h_ls2, "LSGmainAttach");
    fn_LSCall               = dlsym(h_ls2, "LSCall");
    fn_LSCallOneReply       = dlsym(h_ls2, "LSCallOneReply");
    fn_LSMessageGetPayload  = dlsym(h_ls2, "LSMessageGetPayload");

    if (!fn_g_main_loop_new || !fn_g_main_loop_run || !fn_g_main_loop_quit || !fn_g_timeout_add ||
        !fn_LSErrorInit || !fn_LSErrorFree || !fn_LSRegister || !fn_LSUnregister ||
        !fn_LSGmainAttach || !fn_LSCall || !fn_LSCallOneReply || !fn_LSMessageGetPayload) {
        add_log("[POWER] Failed to resolve LS2/GLib symbols");
        return -1;
    }
    return 0;
}

static void *power_standby_thread(void *arg) {
    (void)arg;
    if (load_ls2_and_glib() != 0) return NULL;

    while (g_running) {
        unsigned char err[256];
        memset(err, 0, sizeof(err));
        fn_LSErrorInit(err);

        LSHandle_t *sh = NULL;
        if (!fn_LSRegister("io.github.webostuner.daemon", &sh, err) || !sh) {
            add_log("[POWER] LSRegister failed, retrying...");
            if (fn_LSErrorPrint) { fn_LSErrorPrint(err, stderr); fflush(stderr); }
            fn_LSErrorFree(err);
            sleep(3);
            continue;
        }

        void *loop = fn_g_main_loop_new(NULL, 0);
        g_pwr_loop = loop;
        if (!fn_LSGmainAttach(sh, loop, err)) {
            add_log("[POWER] LSGmainAttach failed");
            if (fn_LSErrorPrint) { fn_LSErrorPrint(err, stderr); fflush(stderr); }
            fn_LSErrorFree(err);
            fn_LSUnregister(sh, err);
            fn_g_main_loop_unref(loop);
            g_pwr_loop = NULL;
            sleep(3);
            continue;
        }

        LSMessageToken_t tok = 0;
        if (!fn_LSCall(sh,
                       "luna://com.webos.service.tvpower/power/registerActiveStandbyRequest",
                       "{\"clientName\":\"webostuner\",\"subscribe\":true}",
                       on_active_standby_req, NULL, &tok, err)) {
            add_log("[POWER] LSCall registerActiveStandbyRequest failed");
            if (fn_LSErrorPrint) { fn_LSErrorPrint(err, stderr); fflush(stderr); }
            fn_LSErrorFree(err);
            fn_LSUnregister(sh, err);
            fn_g_main_loop_unref(loop);
            g_pwr_loop = NULL;
            sleep(3);
            continue;
        }
        fn_LSErrorFree(err);

        memset(err, 0, sizeof(err));
        fn_LSErrorInit(err);
        LSMessageToken_t tok_sus = 0;
        fn_LSCall(sh,
                  "luna://com.webos.service.tvpower/power/registerSuspendRequest",
                  "{\"clientName\":\"webostuner\",\"subscribe\":true}",
                  on_suspend_req, NULL, &tok_sus, err);
        fn_LSErrorFree(err);

        memset(err, 0, sizeof(err));
        fn_LSErrorInit(err);
        LSMessageToken_t tok_st = 0;
        fn_LSCall(sh,
                  "luna://com.webos.service.tvpower/power/getPowerState",
                  "{\"subscribe\":true}",
                  on_power_state_sub, NULL, &tok_st, err);
        fn_LSErrorFree(err);

        memset(err, 0, sizeof(err));
        fn_LSErrorInit(err);
        fn_LSCallOneReply(sh,
                          "luna://com.webos.service.tv.systemproperty/getSystemInfo",
                          "{\"keys\":[\"modelName\",\"firmwareVersion\",\"sdkVersion\"]}",
                          on_sysinfo_reply, NULL, NULL, err);
        fn_LSErrorFree(err);

        memset(err, 0, sizeof(err));
        fn_LSErrorInit(err);
        fn_LSCallOneReply(sh,
                          "luna://com.webos.settingsservice/getSystemSettings",
                          "{\"category\":\"network\",\"keys\":[\"deviceName\"]}",
                          on_devicename_reply, NULL, NULL, err);
        fn_LSErrorFree(err);

        fn_g_timeout_add(300, on_power_timer, sh);
        fn_g_main_loop_run(loop);

        memset(err, 0, sizeof(err));
        fn_LSErrorInit(err);
        fn_LSUnregister(sh, err);
        fn_LSErrorFree(err);
        g_pwr_loop = NULL;
        fn_g_main_loop_unref(loop);
        if (g_running) sleep(2);
    }
    return NULL;
}

static void *epg_poller_thread(void *arg) {
    (void)arg;
    sleep(3);
    while (g_running) {
        int chg = tv_db_poll_epg_changes();
        if (chg > 0) {
            add_log("[EPG] Poll detected %d program event(s)", chg);
        }
        for (int i = 0; i < 300 && g_running; i++) sleep(1);
    }
    return NULL;
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--so") == 0 && i + 1 < argc)
            snprintf(g_so_path, sizeof(g_so_path), "%s", argv[++i]);
        else if (strcmp(argv[i], "--inj") == 0 && i + 1 < argc)
            snprintf(g_inj_path, sizeof(g_inj_path), "%s", argv[++i]);
    }
    load_power_mode();
    load_starfish_release();

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigusr1;
    sa.sa_flags = 0; /* SA_RESTART を付けない */
    sigaction(SIGUSR1, &sa, NULL);

    if (gethostname(g_hostname, sizeof(g_hostname) - 1) != 0) {
        snprintf(g_hostname, sizeof(g_hostname), "LGwebOSTV");
    }

    for (int i = 0; i < MAX_STREAM_CLIENTS; i++) {
        g_clients[i].fd = -1;
        g_clients[i].active = 0;
        g_clients[i].tuner = 1;
    }

    for (int t = 0; t < NUM_TUNERS; t++) {
        memset(&g_tuner[t], 0, sizeof(g_tuner[t]));
        g_tuner[t].id = t;
        g_tuner[t].is_pvr = (t == 0) ? 1 : 0;
        g_tuner[t].cur_lock = -1;
        pthread_mutex_init(&g_tuner[t].tune_mtx, NULL);
        pthread_mutex_init(&g_tuner[t].b25_mtx, NULL);
        b25_init(&g_tuner[t].b25, b25_bcas_bridge_cb, NULL);
    }
    b25_set_pid_cb(&g_tuner[0].b25, on_pvr_pid_discovered, &g_tuner[0]);

    if (ensure_bridge_injected() != 0) {
        add_log("ERR: failed to connect/inject %s", g_so_path);
        return 1;
    }
    add_log("[INIT] tvservice bridge connected.");

    if (init_bcas_syskey() != 0) {
        add_log("[WARN] B-CAS SystemKey load failed");
    }

    if (tv_db_load_channels(&g_chlist) == 0) {
        add_log("[INIT] Loaded %d channels from webOS DB8.", g_chlist.count);
    } else {
        add_log("[WARN] Failed to load DB8 channel list.");
    }

    pthread_t th_wd, th_udp, th_tcp, th_http, th_pwr, th_epg;
    pthread_create(&g_tuner[1].th, NULL, ts_reader_thread, NULL);
    pthread_create(&g_tuner[0].th, NULL, pvr_reader_thread, NULL);
    pthread_create(&th_wd,   NULL, watchdog_thread, NULL);
    pthread_create(&th_udp,  NULL, udp_discovery_thread, NULL);
    pthread_create(&th_tcp,  NULL, tcp_server_thread, NULL);
    pthread_create(&th_http, NULL, http_server_thread, NULL);
    pthread_create(&th_pwr,  NULL, power_standby_thread, NULL);
    pthread_create(&th_epg,  NULL, epg_poller_thread, NULL);

    while (g_running) {
        sleep(1);
    }
    add_log("[EXIT] Shutting down webos_tuner_d...");
    if (g_pwr_loop && fn_g_main_loop_quit) fn_g_main_loop_quit(g_pwr_loop);
    return 0;
}
