/**
 * tv_db.c: LG webOS 純正 DB8 (チャンネル一覧) & EPGDB (番組表) の軽量 JSON パーサ＆変換実装
 */
#include "tv_db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <pthread.h>

/* ---- 動的文字列バッファ ---- */
typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} strbuf_t;

static void sb_init(strbuf_t *sb, size_t init_cap) {
    sb->cap = init_cap ? init_cap : 4096;
    sb->len = 0;
    sb->buf = (char *)malloc(sb->cap);
    if (sb->buf) sb->buf[0] = '\0';
}

static void sb_append_n(strbuf_t *sb, const char *s, size_t n) {
    if (!sb->buf || n == 0) return;
    if (sb->len + n + 1 > sb->cap) {
        size_t ncap = sb->cap * 2;
        while (ncap < sb->len + n + 1) ncap *= 2;
        char *nb = (char *)realloc(sb->buf, ncap);
        if (!nb) return;
        sb->buf = nb;
        sb->cap = ncap;
    }
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
}

static void sb_append(strbuf_t *sb, const char *s) {
    if (s) sb_append_n(sb, s, strlen(s));
}

static void sb_append_json_str(strbuf_t *sb, const char *s) {
    sb_append_n(sb, "\"", 1);
    if (s) {
        for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
            unsigned char c = *p;
            if (c == '"') sb_append_n(sb, "\\\"", 2);
            else if (c == '\\') sb_append_n(sb, "\\\\", 2);
            else if (c == '\n') sb_append_n(sb, "\\n", 2);
            else if (c == '\r') sb_append_n(sb, "\\r", 2);
            else if (c == '\t') sb_append_n(sb, "\\t", 2);
            else if (c < 0x20) {
                char u[8];
                snprintf(u, sizeof(u), "\\u%04x", c);
                sb_append(sb, u);
            } else {
                sb_append_n(sb, (const char *)p, 1);
            }
        }
    }
    sb_append_n(sb, "\"", 1);
}

/* ---- コマンド出力読込 ---- */
static char *run_cmd_capture(const char *cmd, size_t *out_len) {
    FILE *fp = popen(cmd, "r");
    if (!fp) return NULL;
    strbuf_t sb;
    sb_init(&sb, 65536);
    char tmp[8192];
    size_t n;
    while ((n = fread(tmp, 1, sizeof(tmp), fp)) > 0) {
        sb_append_n(&sb, tmp, n);
    }
    pclose(fp);
    if (out_len) *out_len = sb.len;
    return sb.buf;
}

/* ---- 超軽量 JSON 走査ヘルパー ---- */
static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

static int hex4(const char *p, unsigned *out) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

/** JSON 文字列リテラル ("...") を UTF-8 へデコードして読み込み、終了位置の次のポインタを返す */
static const char *parse_json_str(const char *p, char *out, size_t out_sz) {
    p = skip_ws(p);
    if (*p != '"') return NULL;
    p++;
    size_t w = 0;
    while (*p && *p != '"') {
        unsigned char c = (unsigned char)*p++;
        if (c == '\\' && *p) {
            char esc = *p++;
            if (esc == '"' || esc == '\\' || esc == '/') {
                if (w + 1 < out_sz) out[w++] = esc;
            } else if (esc == 'n') {
                if (w + 1 < out_sz) out[w++] = '\n';
            } else if (esc == 'r') {
                if (w + 1 < out_sz) out[w++] = '\r';
            } else if (esc == 't') {
                if (w + 1 < out_sz) out[w++] = '\t';
            } else if (esc == 'u' && p[0] && p[1] && p[2] && p[3]) {
                unsigned cp = 0;
                if (hex4(p, &cp)) {
                    p += 4;
                    if (cp < 0x80) {
                        if (w + 1 < out_sz) out[w++] = (char)cp;
                    } else if (cp < 0x800) {
                        if (w + 2 < out_sz) {
                            out[w++] = (char)(0xc0 | (cp >> 6));
                            out[w++] = (char)(0x80 | (cp & 0x3f));
                        }
                    } else {
                        if (w + 3 < out_sz) {
                            out[w++] = (char)(0xe0 | (cp >> 12));
                            out[w++] = (char)(0x80 | ((cp >> 6) & 0x3f));
                            out[w++] = (char)(0x80 | (cp & 0x3f));
                        }
                    }
                }
            }
        } else {
            if (w + 1 < out_sz) out[w++] = (char)c;
        }
    }
    if (*p == '"') p++;
    if (out_sz > 0) out[w] = '\0';
    return p;
}

/** 任意の JSON 値 (オブジェクト・配列・文字列・数値・真偽・null) をスキップする */
static const char *skip_json_val(const char *p) {
    p = skip_ws(p);
    if (*p == '"') {
        char dummy[8];
        return parse_json_str(p, dummy, 1);
    } else if (*p == '{' || *p == '[') {
        char open = *p, close_c = (open == '{') ? '}' : ']';
        int depth = 1;
        p++;
        while (*p && depth > 0) {
            if (*p == '"') {
                char dummy[8];
                p = parse_json_str(p, dummy, 1);
                if (!p) return NULL;
            } else {
                if (*p == open) depth++;
                else if (*p == close_c) depth--;
                p++;
            }
        }
        return p;
    } else {
        while (*p && *p != ',' && *p != '}' && *p != ']' &&
               *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
        return p;
    }
}

static int parse_json_int(const char *p, long long *val_out, const char **next_out) {
    p = skip_ws(p);
    if ((*p < '0' || *p > '9') && *p != '-') return 0;
    char *endp = NULL;
    long long v = strtoll(p, &endp, 10);
    if (endp == p) return 0;
    *val_out = v;
    if (next_out) *next_out = endp;
    return 1;
}

static int cmp_channel(const void *a, const void *b) {
    const tv_channel_t *ca = (const tv_channel_t *)a;
    const tv_channel_t *cb = (const tv_channel_t *)b;
    if (ca->space != cb->space) return ca->space - cb->space;
    int na = atoi(ca->ch_num), nb = atoi(cb->ch_num);
    if (na != nb) return na - nb;
    if (ca->phy != cb->phy) return (ca->phy < cb->phy) ? -1 : 1;
    return (int)ca->sid - (int)cb->sid;
}

int tv_db_load_channels(tv_channel_list_t *list) {
    memset(list, 0, sizeof(*list));
    size_t raw_len = 0;
    char *raw = run_cmd_capture(
        "luna-send -n 1 -w 4000 luna://com.palm.db/find '{\"query\":{\"from\":\"com.webos.service.tv.channel.dblist:1\",\"limit\":500}}' </dev/null",
        &raw_len);
    if (!raw || raw_len == 0) { free(raw); return -1; }

    const char *rp = strstr(raw, "\"results\"");
    if (!rp) { free(raw); return -1; }
    rp = strchr(rp, '[');
    if (!rp) { free(raw); return -1; }
    rp++;

    int cap = 256;
    list->items = (tv_channel_t *)calloc((size_t)cap, sizeof(tv_channel_t));
    if (!list->items) { free(raw); return -1; }

    while (*rp) {
        rp = skip_ws(rp);
        if (*rp == ']') break;
        if (*rp == ',') { rp++; continue; }
        if (*rp != '{') break;
        rp++;

        tv_channel_t ch;
        memset(&ch, 0, sizeof(ch));
        int mode_id = -1;
        long long num_val = 0;
        char key[64], sval[256];

        while (*rp) {
            rp = skip_ws(rp);
            if (*rp == '}') { rp++; break; }
            if (*rp == ',') { rp++; continue; }
            const char *np = parse_json_str(rp, key, sizeof(key));
            if (!np) break;
            rp = skip_ws(np);
            if (*rp == ':') rp++;
            rp = skip_ws(rp);

            if (strcmp(key, "channelModeId") == 0 && parse_json_int(rp, &num_val, &np)) {
                mode_id = (int)num_val; rp = np;
            } else if (strcmp(key, "physicalNumber") == 0 && parse_json_int(rp, &num_val, &np)) {
                ch.phy = (uint32_t)num_val; rp = np;
            } else if (strcmp(key, "Frequency") == 0 && parse_json_int(rp, &num_val, &np)) {
                ch.freq_khz = (uint32_t)num_val; rp = np;
            } else if (strcmp(key, "TSID") == 0 && parse_json_int(rp, &num_val, &np)) {
                ch.tsid = (uint16_t)num_val; rp = np;
            } else if (strcmp(key, "ONID") == 0 && parse_json_int(rp, &num_val, &np)) {
                ch.onid = (uint16_t)num_val; rp = np;
            } else if (strcmp(key, "SVCID") == 0 && parse_json_int(rp, &num_val, &np)) {
                ch.sid = (uint16_t)num_val; rp = np;
            } else if (strcmp(key, "serviceType") == 0 && parse_json_int(rp, &num_val, &np)) {
                ch.service_type = (uint16_t)num_val; rp = np;
            } else if (strcmp(key, "channelNumber") == 0 && *rp == '"') {
                rp = parse_json_str(rp, ch.ch_num, sizeof(ch.ch_num));
            } else if (strcmp(key, "channelName") == 0 && *rp == '"') {
                rp = parse_json_str(rp, ch.name, sizeof(ch.name));
            } else if (strcmp(key, "signalChannelId") == 0 && *rp == '"') {
                rp = parse_json_str(rp, ch.sig_id, sizeof(ch.sig_id));
            } else if (strcmp(key, "skipped") == 0) {
                ch.skipped = (strncmp(rp, "true", 4) == 0) ? 1 : 0;
                rp = skip_json_val(rp);
            } else if (strcmp(key, "Invisible") == 0) {
                ch.invisible = (strncmp(rp, "true", 4) == 0) ? 1 : 0;
                rp = skip_json_val(rp);
            } else {
                (void)sval;
                rp = skip_json_val(rp);
            }
            if (!rp) break;
        }

        if (ch.freq_khz > 0 && ch.sid > 0 && !ch.skipped && !ch.invisible && ch.service_type != 164) {
            if (mode_id == 0) {
                ch.media = 1; ch.space = 0; snprintf(ch.type_str, sizeof(ch.type_str), "GR");
            } else if (mode_id == 4) {
                ch.media = 4; ch.space = 1; snprintf(ch.type_str, sizeof(ch.type_str), "BS");
            } else if (mode_id == 5 || mode_id == 6) {
                ch.media = 5; ch.space = 2; snprintf(ch.type_str, sizeof(ch.type_str), "CS");
            } else {
                continue;
            }
            ch.mirakurun_sid = (uint64_t)ch.onid * 100000ULL + (uint64_t)ch.sid;
            if (list->count >= cap) {
                cap *= 2;
                tv_channel_t *nb = (tv_channel_t *)realloc(list->items, (size_t)cap * sizeof(tv_channel_t));
                if (!nb) break;
                list->items = nb;
            }
            list->items[list->count++] = ch;
        }
    }
    free(raw);
    if (list->count > 1) qsort(list->items, (size_t)list->count, sizeof(tv_channel_t), cmp_channel);
    return 0;
}

void tv_db_free_channels(tv_channel_list_t *list) {
    free(list->items);
    list->items = NULL;
    list->count = 0;
}

const tv_channel_t *tv_db_find_by_mirakurun(const tv_channel_list_t *list, const char *type_str, const char *ch_spec) {
    if (!list || !type_str || !ch_spec) return NULL;
    uint32_t num_spec = (uint32_t)strtoul(ch_spec, NULL, 10);
    for (int i = 0; i < list->count; i++) {
        const tv_channel_t *c = &list->items[i];
        if (strcmp(c->type_str, type_str) != 0) continue;
        if (strcmp(c->ch_num, ch_spec) == 0) return c;
        if (num_spec > 0 && c->phy == num_spec) return c;
        if (num_spec > 0 && (uint32_t)atoi(c->ch_num) == num_spec) return c;
        char bs_name[24];
        snprintf(bs_name, sizeof(bs_name), "%s%u_0", c->type_str, c->phy);
        if (strcmp(bs_name, ch_spec) == 0) return c;
        snprintf(bs_name, sizeof(bs_name), "%s%u", c->type_str, c->phy);
        if (strcmp(bs_name, ch_spec) == 0) return c;
    }
    return NULL;
}

const tv_channel_t *tv_db_find_by_sid(const tv_channel_list_t *list, uint64_t mirakurun_sid) {
    if (!list) return NULL;
    for (int i = 0; i < list->count; i++) {
        if (list->items[i].mirakurun_sid == mirakurun_sid) return &list->items[i];
    }
    return NULL;
}

static void sb_append_service_obj(strbuf_t *sb, const tv_channel_t *c) {
    char head[320];
    snprintf(head, sizeof(head),
             "{\"id\":%llu,\"serviceId\":%u,\"networkId\":%u,\"type\":%u,"
             "\"hasLogoData\":false,\"epgReady\":true,"
             "\"channel\":{\"type\":\"%s\",\"channel\":\"%u\"},\"name\":",
             (unsigned long long)c->mirakurun_sid, c->sid, c->onid, c->service_type ? c->service_type : 1,
             c->type_str, c->phy);
    sb_append(sb, head);
    sb_append_json_str(sb, c->name);
    sb_append(sb, "}");
}

char *tv_db_build_service_json_one(const tv_channel_t *c) {
    if (!c) return NULL;
    strbuf_t sb;
    sb_init(&sb, 512);
    sb_append_service_obj(&sb, c);
    return sb.buf;
}

char *tv_db_build_services_json(const tv_channel_list_t *list) {
    strbuf_t sb;
    sb_init(&sb, 32768);
    sb_append(&sb, "[");
    for (int i = 0; i < list->count; i++) {
        if (i > 0) sb_append(&sb, ",");
        sb_append_service_obj(&sb, &list->items[i]);
    }
    sb_append(&sb, "]");
    return sb.buf;
}

char *tv_db_build_channels_json(const tv_channel_list_t *list) {
    strbuf_t sb;
    sb_init(&sb, 32768);
    sb_append(&sb, "[");
    int first_ch = 1;
    /* 同一 (media, phy, tsid) ごとにグルーピング */
    uint8_t *done = (uint8_t *)calloc((size_t)(list->count + 1), 1);
    for (int i = 0; i < list->count; i++) {
        if (done[i]) continue;
        const tv_channel_t *base = &list->items[i];
        if (!first_ch) sb_append(&sb, ",");
        first_ch = 0;

        char ch_id[24];
        snprintf(ch_id, sizeof(ch_id), "%u", base->phy);
        char head[256];
        snprintf(head, sizeof(head), "{\"type\":\"%s\",\"channel\":\"%s\",\"name\":", base->type_str, ch_id);
        sb_append(&sb, head);
        sb_append_json_str(&sb, base->name);
        sb_append(&sb, ",\"services\":[");

        int first_sv = 1;
        for (int j = i; j < list->count; j++) {
            const tv_channel_t *c = &list->items[j];
            if (c->media == base->media && c->phy == base->phy && c->tsid == base->tsid) {
                done[j] = 1;
                if (!first_sv) sb_append(&sb, ",");
                first_sv = 0;
                char sv[160];
                snprintf(sv, sizeof(sv), "{\"id\":%llu,\"serviceId\":%u,\"networkId\":%u,\"name\":",
                         (unsigned long long)c->mirakurun_sid, c->sid, c->onid);
                sb_append(&sb, sv);
                sb_append_json_str(&sb, c->name);
                sb_append(&sb, "}");
            }
        }
        sb_append(&sb, "]}");
    }
    free(done);
    sb_append(&sb, "]");
    return sb.buf;
}

/* ---- EPG キャッシュ＆差分イベント配信 ---- */
#define EPG_EVENT_RING_CAP 1024

typedef struct {
    uint64_t id;
    uint64_t hash;
} epg_hash_entry_t;

typedef struct {
    uint64_t seq;
    char    *json;
} epg_event_item_t;

static pthread_mutex_t  g_fetch_mtx = PTHREAD_MUTEX_INITIALIZER; /* EPG DB 取得専用（長時間ロック可） */
static pthread_mutex_t  g_epg_mtx   = PTHREAD_MUTEX_INITIALIZER; /* キャッシュポインタ差し替え専用（瞬間ロック） */
static char            *g_epg_cache_json = NULL;
static time_t           g_epg_cache_ts   = 0;

static epg_hash_entry_t *g_prev_hashes   = NULL; /* g_fetch_mtx 配下で管理 */
static size_t            g_prev_hash_cnt = 0;

static pthread_mutex_t  g_ev_mtx = PTHREAD_MUTEX_INITIALIZER;
static epg_event_item_t g_ev_ring[EPG_EVENT_RING_CAP];
static uint64_t         g_ev_seq = 0;

static uint64_t fnv1a_update(uint64_t h, const void *data, size_t len) {
    const unsigned char *p = (const unsigned char *)data;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static int cmp_hash_entry(const void *a, const void *b) {
    uint64_t ia = ((const epg_hash_entry_t *)a)->id;
    uint64_t ib = ((const epg_hash_entry_t *)b)->id;
    return (ia > ib) - (ia < ib);
}

static void push_epg_event_raw(char *json_owned) {
    if (!json_owned) return;
    pthread_mutex_lock(&g_ev_mtx);
    uint64_t seq = ++g_ev_seq;
    size_t slot = (size_t)(seq % EPG_EVENT_RING_CAP);
    free(g_ev_ring[slot].json);
    g_ev_ring[slot].seq  = seq;
    g_ev_ring[slot].json = json_owned;
    pthread_mutex_unlock(&g_ev_mtx);
}

static void push_epg_event(const char *type, const char *prog_json, size_t prog_len, int64_t now_ms) {
    strbuf_t sb;
    sb_init(&sb, prog_len + 96);
    char hdr[80];
    snprintf(hdr, sizeof(hdr), "{\"resource\":\"program\",\"type\":\"%s\",\"data\":", type);
    sb_append(&sb, hdr);
    sb_append_n(&sb, prog_json, prog_len);
    char tail[48];
    snprintf(tail, sizeof(tail), ",\"time\":%lld}", (long long)now_ms);
    sb_append(&sb, tail);
    push_epg_event_raw(sb.buf);
}

static void push_epg_remove_event(uint64_t program_id, int64_t now_ms) {
    char *buf = (char *)malloc(160);
    if (!buf) return;
    snprintf(buf, 160,
             "{\"resource\":\"program\",\"type\":\"remove\",\"data\":{\"id\":%llu},\"time\":%lld}",
             (unsigned long long)program_id, (long long)now_ms);
    push_epg_event_raw(buf);
}

uint64_t tv_db_get_event_seq(void) {
    pthread_mutex_lock(&g_ev_mtx);
    uint64_t s = g_ev_seq;
    pthread_mutex_unlock(&g_ev_mtx);
    return s;
}

char *tv_db_get_events_since(uint64_t *cursor_seq_io, int *already_sent_io) {
    pthread_mutex_lock(&g_ev_mtx);
    uint64_t cur = *cursor_seq_io;
    if (cur >= g_ev_seq) {
        pthread_mutex_unlock(&g_ev_mtx);
        return NULL;
    }
    if (g_ev_seq - cur > EPG_EVENT_RING_CAP) {
        cur = g_ev_seq - EPG_EVENT_RING_CAP;
    }
    strbuf_t sb;
    sb_init(&sb, 4096);
    int emitted = 0;
    for (uint64_t s = cur + 1; s <= g_ev_seq; s++) {
        size_t slot = (size_t)(s % EPG_EVENT_RING_CAP);
        if (g_ev_ring[slot].seq == s && g_ev_ring[slot].json) {
            if (*already_sent_io || emitted > 0) sb_append(&sb, ",");
            sb_append(&sb, g_ev_ring[slot].json);
            *already_sent_io = 1;
            emitted++;
        }
    }
    *cursor_seq_io = g_ev_seq;
    pthread_mutex_unlock(&g_ev_mtx);
    if (emitted == 0) {
        free(sb.buf);
        return NULL;
    }
    return sb.buf;
}

static int64_t parse_webos_time_ms(const char *s) {
    int Y = 0, M = 0, D = 0, h = 0, m = 0, sec = 0;
    if (sscanf(s, "%d,%d,%d,%d,%d,%d", &Y, &M, &D, &h, &m, &sec) != 6) return 0;
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = Y - 1900;
    tmv.tm_mon  = M - 1;
    tmv.tm_mday = D;
    tmv.tm_hour = h;
    tmv.tm_min  = m;
    tmv.tm_sec  = sec;
    tmv.tm_isdst = -1;
    time_t tt = mktime(&tmv);
    if (tt < 0) return 0;
    return (int64_t)tt * 1000LL;
}

static int fetch_and_build_epg_json(void) {
    strbuf_t out;
    sb_init(&out, 262144);
    sb_append(&out, "[");
    int first = 1;

    size_t h_cap = 8192, h_cnt = 0;
    epg_hash_entry_t *new_hashes = (epg_hash_entry_t *)malloc(h_cap * sizeof(epg_hash_entry_t));
    int changed_count = 0;
    int64_t now_ms = (int64_t)time(NULL) * 1000LL;

    char next_tok[256] = {0};
    for (int page = 0; page < 40; page++) {
        char cmd[1024];
        if (next_tok[0]) {
            snprintf(cmd, sizeof(cmd),
                     "luna-send -n 1 -w 4000 luna://com.webos.epgdb/find "
                     "'{\"query\":{\"from\":\"com.webos.service.tv.programSCH:4\","
                     "\"select\":[\"programId\",\"startTime\",\"duration\",\"programName\",\"description\",\"genre\",\"genreList\"],"
                     "\"limit\":250,\"page\":\"%s\"}}' </dev/null",
                     next_tok);
        } else {
            snprintf(cmd, sizeof(cmd),
                     "luna-send -n 1 -w 4000 luna://com.webos.epgdb/find "
                     "'{\"query\":{\"from\":\"com.webos.service.tv.programSCH:4\","
                     "\"select\":[\"programId\",\"startTime\",\"duration\",\"programName\",\"description\",\"genre\",\"genreList\"],"
                     "\"limit\":250}}' </dev/null");
        }
        size_t rlen = 0;
        char *raw = run_cmd_capture(cmd, &rlen);
        if (!raw || rlen == 0) { free(raw); break; }

        char new_next[256] = {0};
        const char *np_key = strstr(raw, "\"next\"");
        if (np_key) {
            const char *colon = strchr(np_key, ':');
            if (colon) parse_json_str(colon + 1, new_next, sizeof(new_next));
        }

        const char *rp = strstr(raw, "\"results\"");
        if (!rp || !(rp = strchr(rp, '['))) { free(raw); break; }
        rp++;

        while (*rp) {
            rp = skip_ws(rp);
            if (*rp == ']') break;
            if (*rp == ',') { rp++; continue; }
            if (*rp != '{') break;
            rp++;

            char pid_str[64] = {0}, stime_str[48] = {0};
            char pname[256] = {0}, pdesc[1024] = {0};
            long long dur_sec = 0;
            int genre_lv1 = -1;
            char key[64];

            while (*rp) {
                rp = skip_ws(rp);
                if (*rp == '}') { rp++; break; }
                if (*rp == ',') { rp++; continue; }
                const char *nx = parse_json_str(rp, key, sizeof(key));
                if (!nx) break;
                rp = skip_ws(nx);
                if (*rp == ':') rp++;
                rp = skip_ws(rp);

                long long nv = 0;
                if (strcmp(key, "programId") == 0 && *rp == '"') {
                    rp = parse_json_str(rp, pid_str, sizeof(pid_str));
                } else if (strcmp(key, "startTime") == 0 && *rp == '"') {
                    rp = parse_json_str(rp, stime_str, sizeof(stime_str));
                } else if (strcmp(key, "programName") == 0 && *rp == '"') {
                    rp = parse_json_str(rp, pname, sizeof(pname));
                } else if (strcmp(key, "description") == 0 && *rp == '"') {
                    rp = parse_json_str(rp, pdesc, sizeof(pdesc));
                } else if (strcmp(key, "duration") == 0 && parse_json_int(rp, &nv, &nx)) {
                    dur_sec = nv; rp = nx;
                } else if (strcmp(key, "genre") == 0 && parse_json_int(rp, &nv, &nx)) {
                    genre_lv1 = (int)nv; rp = nx;
                } else {
                    rp = skip_json_val(rp);
                }
                if (!rp) break;
            }

            unsigned onid = 0, tsid = 0, sid = 0, evid = 0;
            if (sscanf(pid_str, "%u_%u_%u_%u", &onid, &tsid, &sid, &evid) == 4 && pname[0]) {
                int64_t start_ms = parse_webos_time_ms(stime_str);
                if (start_ms > 0 && dur_sec > 0) {
                    uint64_t mir_sid = (uint64_t)onid * 100000ULL + (uint64_t)sid;
                    uint64_t mir_pid = mir_sid * 100000ULL + (uint64_t)evid;
                    if (!first) sb_append(&out, ",");
                    first = 0;

                    size_t obj_pos = out.len;
                    char hdr[256];
                    snprintf(hdr, sizeof(hdr),
                             "{\"id\":%llu,\"eventId\":%u,\"serviceId\":%u,\"networkId\":%u,"
                             "\"startAt\":%lld,\"duration\":%lld,\"isFree\":true,\"name\":",
                             (unsigned long long)mir_pid, evid, sid, onid,
                             (long long)start_ms, dur_sec * 1000LL);
                    sb_append(&out, hdr);
                    sb_append_json_str(&out, pname);
                    sb_append(&out, ",\"description\":");
                    sb_append_json_str(&out, pdesc);
                    if (genre_lv1 >= 0) {
                        char gbuf[48];
                        snprintf(gbuf, sizeof(gbuf), ",\"genres\":[{\"lv1\":%d,\"lv2\":15}]", genre_lv1);
                        sb_append(&out, gbuf);
                    }
                    sb_append(&out, "}");

                    uint64_t h = 14695981039346656037ULL;
                    h = fnv1a_update(h, &start_ms, sizeof(start_ms));
                    h = fnv1a_update(h, &dur_sec, sizeof(dur_sec));
                    h = fnv1a_update(h, &genre_lv1, sizeof(genre_lv1));
                    h = fnv1a_update(h, pname, strlen(pname));
                    h = fnv1a_update(h, pdesc, strlen(pdesc));

                    if (new_hashes) {
                        if (h_cnt >= h_cap) {
                            size_t ncap = h_cap * 2;
                            epg_hash_entry_t *nh = (epg_hash_entry_t *)realloc(new_hashes, ncap * sizeof(epg_hash_entry_t));
                            if (nh) { new_hashes = nh; h_cap = ncap; }
                        }
                        if (h_cnt < h_cap) {
                            new_hashes[h_cnt].id   = mir_pid;
                            new_hashes[h_cnt].hash = h;
                            h_cnt++;
                        }
                    }

                    if (g_prev_hashes && out.buf && out.len > obj_pos) {
                        epg_hash_entry_t key_entry = { mir_pid, 0 };
                        epg_hash_entry_t *found = (epg_hash_entry_t *)bsearch(
                            &key_entry, g_prev_hashes, g_prev_hash_cnt, sizeof(epg_hash_entry_t), cmp_hash_entry);
                        if (!found) {
                            push_epg_event("create", out.buf + obj_pos, out.len - obj_pos, now_ms);
                            changed_count++;
                        } else if (found->hash != h) {
                            push_epg_event("update", out.buf + obj_pos, out.len - obj_pos, now_ms);
                            changed_count++;
                        }
                    }
                }
            }
        }
        free(raw);
        if (!new_next[0]) break;
        snprintf(next_tok, sizeof(next_tok), "%s", new_next);
    }
    sb_append(&out, "]");

    if (new_hashes && h_cnt > 0) {
        qsort(new_hashes, h_cnt, sizeof(epg_hash_entry_t), cmp_hash_entry);
        /* 前回にあって今回消えた番組 ID を O(n) マージ走査で検出して remove イベント発行 */
        if (g_prev_hashes && g_prev_hash_cnt > 0) {
            size_t pi = 0, ni = 0;
            int rem_cnt = 0;
            while (pi < g_prev_hash_cnt && rem_cnt < 256) {
                if (ni >= h_cnt || g_prev_hashes[pi].id < new_hashes[ni].id) {
                    push_epg_remove_event(g_prev_hashes[pi].id, now_ms);
                    changed_count++;
                    rem_cnt++;
                    pi++;
                } else if (g_prev_hashes[pi].id == new_hashes[ni].id) {
                    pi++;
                    ni++;
                } else {
                    ni++;
                }
            }
        }
        free(g_prev_hashes);
        g_prev_hashes   = new_hashes;
        g_prev_hash_cnt = h_cnt;
    } else {
        free(new_hashes);
    }

    pthread_mutex_lock(&g_epg_mtx);
    free(g_epg_cache_json);
    g_epg_cache_json = out.buf;
    g_epg_cache_ts = time(NULL);
    pthread_mutex_unlock(&g_epg_mtx);
    return changed_count;
}

int tv_db_poll_epg_changes(void) {
    pthread_mutex_lock(&g_fetch_mtx);
    int chg = fetch_and_build_epg_json();
    pthread_mutex_unlock(&g_fetch_mtx);
    return chg;
}

/**
 * キャッシュ済み EPG JSON 配列から条件に合う要素を抽出する。
 * target_id != 0 の場合は "id":target_id の単一オブジェクト（配列括弧なし）を返し、
 * filter_sid != 0 の場合は該当サービスの配列 "[...]" を返す。
 */
static char *extract_epg_matching(const char *json, uint64_t filter_sid, uint64_t target_id) {
    if (!json) return target_id ? NULL : strdup("[]");
    strbuf_t sb;
    if (target_id == 0) {
        sb_init(&sb, 16384);
        sb_append(&sb, "[");
    } else {
        memset(&sb, 0, sizeof(sb));
    }

    int count = 0;
    const char *p = json;
    while (*p && *p != '[') p++;
    if (*p == '[') p++;

    while (*p) {
        p = skip_ws(p);
        if (*p == ']' || *p == '\0') break;
        if (*p == ',') { p++; continue; }
        if (*p != '{') break;

        /* 文字列リテラルを考慮してオブジェクト境界を探す */
        const char *obj_start = p;
        int depth = 0, in_str = 0, esc = 0;
        while (*p) {
            char c = *p++;
            if (in_str) {
                if (esc) esc = 0;
                else if (c == '\\') esc = 1;
                else if (c == '"') in_str = 0;
            } else {
                if (c == '"') in_str = 1;
                else if (c == '{') depth++;
                else if (c == '}') {
                    depth--;
                    if (depth == 0) break;
                }
            }
        }
        size_t obj_len = (size_t)(p - obj_start);

        uint64_t id_val = 0, sid_val = 0, nid_val = 0;
        if (sscanf(obj_start, "{\"id\":%llu,\"eventId\":%*u,\"serviceId\":%llu,\"networkId\":%llu,",
                   (unsigned long long *)&id_val,
                   (unsigned long long *)&sid_val,
                   (unsigned long long *)&nid_val) == 3) {
            if (target_id != 0) {
                if (id_val == target_id) {
                    char *res = (char *)malloc(obj_len + 1);
                    if (res) {
                        memcpy(res, obj_start, obj_len);
                        res[obj_len] = '\0';
                    }
                    return res;
                }
            } else if (filter_sid != 0) {
                uint64_t mir_sid = nid_val * 100000ULL + sid_val;
                int hit = (filter_sid >= 100000ULL) ? (mir_sid == filter_sid) : (sid_val == filter_sid);
                if (hit) {
                    if (count > 0) sb_append(&sb, ",");
                    sb_append_n(&sb, obj_start, obj_len);
                    count++;
                }
            }
        }
    }

    if (target_id != 0) return NULL;
    sb_append(&sb, "]");
    return sb.buf;
}

char *tv_db_get_programs_json(uint64_t filter_sid, int force_refresh) {
    pthread_mutex_lock(&g_epg_mtx);
    int need_init = (!g_epg_cache_json);
    pthread_mutex_unlock(&g_epg_mtx);

    if (force_refresh || need_init) {
        tv_db_poll_epg_changes();
    }

    pthread_mutex_lock(&g_epg_mtx);
    char *res;
    if (filter_sid == 0) {
        res = g_epg_cache_json ? strdup(g_epg_cache_json) : strdup("[]");
    } else {
        res = extract_epg_matching(g_epg_cache_json, filter_sid, 0);
    }
    pthread_mutex_unlock(&g_epg_mtx);
    return res;
}

char *tv_db_get_program_by_id_json(uint64_t program_id) {
    pthread_mutex_lock(&g_epg_mtx);
    int need_init = (!g_epg_cache_json);
    pthread_mutex_unlock(&g_epg_mtx);

    if (need_init) {
        tv_db_poll_epg_changes();
    }

    pthread_mutex_lock(&g_epg_mtx);
    char *res = extract_epg_matching(g_epg_cache_json, 0, program_id);
    pthread_mutex_unlock(&g_epg_mtx);
    return res;
}
