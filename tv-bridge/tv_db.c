/**
 * tv_db.c: LG webOS 純正 DB8 (チャンネル一覧) & EPGDB (番組表) の軽量 JSON パーサ＆変換実装
 */
#include "tv_db.h"
#include "eit.h"
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

/* LG webOS EPG DB が使用する PUA (U+E0F8..U+E0FF, U+E180..U+E19C) -> ARIB 94区囲み文字テーブル */
static const char * const LG_PUA_ROW94[38] = {
    NULL,
    "[HV]", "[SD]", "[P]", "[W]", "[MV]", "[手]", "[字]", "[双]",
    "[デ]", "[S]", "[二]", "[多]", "[解]", "[SS]", "[B]", "[N]",
    "■", "●", "[天]", "[交]", "[映]", "[無]", "[料]", "[年齢制限]",
    "[前]", "[後]", "[再]", "[新]", "[初]", "[終]", "[生]", "[販]",
    "[声]", "[吹]", "[PPV]", "(秘)", "[ほか]"
};

static const char *map_lg_pua_codepoint(unsigned cp) {
    if (cp >= 0xE0F8u && cp <= 0xE0FFu) {
        return LG_PUA_ROW94[cp - 0xE0F8u + 1u];
    }
    if (cp >= 0xE180u && cp <= 0xE19Cu) {
        return LG_PUA_ROW94[cp - 0xE180u + 9u];
    }
    if (cp == 0xE2F0u) return "!?";
    if (cp == 0xE2F1u) return "!!";
    return NULL;
}

static void append_codepoint_filtered(char *out, size_t out_sz, size_t *w, unsigned cp) {
    if (cp >= 0xE000u && cp <= 0xF8FFu) {
        const char *rep = map_lg_pua_codepoint(cp);
        if (rep) {
            size_t rl = strlen(rep);
            if (*w + rl < out_sz) {
                memcpy(out + *w, rep, rl);
                *w += rl;
            }
        }
        return;
    }
    if (cp < 0x80u) {
        if (*w + 1 < out_sz) out[(*w)++] = (char)cp;
    } else if (cp < 0x800u) {
        if (*w + 2 < out_sz) {
            out[(*w)++] = (char)(0xC0u | (cp >> 6));
            out[(*w)++] = (char)(0x80u | (cp & 0x3Fu));
        }
    } else {
        if (*w + 3 < out_sz) {
            out[(*w)++] = (char)(0xE0u | (cp >> 12));
            out[(*w)++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            out[(*w)++] = (char)(0x80u | (cp & 0x3Fu));
        }
    }
}

/** JSON 文字列リテラル ("...") を UTF-8 へデコードして読み込み (LG PUA 外字変換付き)、終了位置の次のポインタを返す */
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
                    append_codepoint_filtered(out, out_sz, &w, cp);
                }
            }
        } else if (c >= 0xE0u && c <= 0xEFu &&
                   ((unsigned char)p[0] & 0xC0u) == 0x80u &&
                   ((unsigned char)p[1] & 0xC0u) == 0x80u) {
            unsigned b1 = (unsigned char)p[0];
            unsigned b2 = (unsigned char)p[1];
            unsigned cp = ((c & 0x0Fu) << 12) | ((b1 & 0x3Fu) << 6) | (b2 & 0x3Fu);
            p += 2;
            append_codepoint_filtered(out, out_sz, &w, cp);
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

static int is_dummy_channel_name(const char *name) {
    if (!name) return 1;
    while (*name == ' ' || *name == '\t') name++;
    if (*name == '\0') return 1;
    if (strcmp(name, "-") == 0 ||
        strcmp(name, "−") == 0 || /* U+2212 */
        strcmp(name, "ー") == 0 || /* U+30FC */
        strcmp(name, "―") == 0 || /* U+2015 */
        strcmp(name, "－") == 0) { /* U+FF0D */
        return 1;
    }
    return 0;
}

static int cmp_channel(const void *a, const void *b) {
    const tv_channel_t *ca = (const tv_channel_t *)a;
    const tv_channel_t *cb = (const tv_channel_t *)b;
    if (ca->space != cb->space) return ca->space - cb->space;
    /* 地デジはリモコン番号順 -> ONID順 -> サービスID順 (011 -> 012 -> 013) */
    if (ca->media == 1) {
        int rka = ca->remote_key > 0 ? ca->remote_key : 99;
        int rkb = cb->remote_key > 0 ? cb->remote_key : 99;
        if (rka != rkb) return rka - rkb;
        if (ca->onid != cb->onid) return (int)ca->onid - (int)cb->onid;
    }
    if (ca->sid != cb->sid) return (int)ca->sid - (int)cb->sid;
    int na = atoi(ca->ch_num), nb = atoi(cb->ch_num);
    if (na != nb) return na - nb;
    return (ca->phy < cb->phy) ? -1 : (ca->phy > cb->phy ? 1 : 0);
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
        int shortcut = 255;
        int special_svc = 0;
        long long num_val = 0;
        char key[64];

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
            } else if (strcmp(key, "shortCut") == 0 && parse_json_int(rp, &num_val, &np)) {
                shortcut = (int)num_val; rp = np;
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
            } else if (strcmp(key, "specialService") == 0) {
                special_svc = (strncmp(rp, "true", 4) == 0) ? 1 : 0;
                rp = skip_json_val(rp);
            } else {
                rp = skip_json_val(rp);
            }
            if (!rp) break;
        }

        /* デジタルTV (1) またはデジタル音声 (2: 放送大学ラジオ等) のみ対象とし、
           臨時サービス (161 / specialService)、データ放送 (192)、エンジニアリング (164)、
           プロモーション (165)、旧BSプレミアム跡地 (BS 103)、ダミー局名 ("−") を除外 */
        if (ch.freq_khz > 0 && ch.sid > 0 && !ch.skipped && !ch.invisible &&
            (ch.service_type == 1 || ch.service_type == 2) && !special_svc &&
            !(ch.onid == 4 && ch.sid == 103) &&
            !is_dummy_channel_name(ch.name)) {
            if (mode_id == 0) {
                ch.media = 1; ch.space = 0; snprintf(ch.type_str, sizeof(ch.type_str), "GR");
                if (shortcut >= 1 && shortcut <= 12) {
                    ch.remote_key = shortcut;
                } else {
                    int cn = atoi(ch.ch_num);
                    if (cn >= 11 && cn <= 129) ch.remote_key = cn / 10;
                }
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

    /* 同一 (media, onid, tsid) 内で最小 SID (プライマリサービス) とリモコン番号を特定し、サブチャンネル判定を設定 */
    eit_clear_services();
    for (int i = 0; i < list->count; i++) {
        tv_channel_t *c = &list->items[i];
        uint16_t min_sid = c->sid;
        int rkey = c->remote_key;
        for (int j = 0; j < list->count; j++) {
            const tv_channel_t *o = &list->items[j];
            if (o->media == c->media && o->onid == c->onid && o->tsid == c->tsid) {
                if (o->sid < min_sid) min_sid = o->sid;
                if (rkey == 0 && o->remote_key > 0) rkey = o->remote_key;
            }
        }
        if (c->media == 1 && c->remote_key == 0 && rkey > 0) {
            c->remote_key = rkey;
        }
        if (c->media == 1) {
            /* 地デジ: ARIB TR-B14 のビットマスク (sid & 0x0187) または同一 TS 内の 2 番目以降をサブチャンネルと判定 */
            c->is_sub = ((c->sid & 0x0187) != 0 || c->sid > min_sid) ? 1 : 0;
            eit_register_service(c->onid, c->tsid, c->sid, min_sid);
        } else if (c->media == 4) {
            /* BS: 同一 (onid, tsid) 内の最小 SID (101, 141, 151, 161, 171, 181, 231 等) をメイン、それ以外をサブと判定 */
            c->is_sub = (c->sid > min_sid) ? 1 : 0;
            eit_register_service(c->onid, c->tsid, c->sid, min_sid);
        } else {
            /* CS: 1 トランスポンダに複数の独立チャンネルが相乗りするため全局をメイン扱い */
            c->is_sub = 0;
            eit_register_service(c->onid, c->tsid, c->sid, c->sid);
        }
    }

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
    char head[384];
    if (c->remote_key > 0) {
        snprintf(head, sizeof(head),
                 "{\"id\":%llu,\"serviceId\":%u,\"networkId\":%u,\"type\":%u,"
                 "\"remoteControlKeyId\":%d,\"hasLogoData\":false,\"epgReady\":true,"
                 "\"channel\":{\"type\":\"%s\",\"channel\":\"%u\"},\"name\":",
                 (unsigned long long)c->mirakurun_sid, c->sid, c->onid, c->service_type ? c->service_type : 1,
                 c->remote_key, c->type_str, c->phy);
    } else {
        snprintf(head, sizeof(head),
                 "{\"id\":%llu,\"serviceId\":%u,\"networkId\":%u,\"type\":%u,"
                 "\"hasLogoData\":false,\"epgReady\":true,"
                 "\"channel\":{\"type\":\"%s\",\"channel\":\"%u\"},\"name\":",
                 (unsigned long long)c->mirakurun_sid, c->sid, c->onid, c->service_type ? c->service_type : 1,
                 c->type_str, c->phy);
    }
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
    int first = 1;
    for (int i = 0; i < list->count; i++) {
        if (!first) sb_append(&sb, ",");
        first = 0;
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
    /* 同一 (media, phy, tsid) ごとにグルーピング (Mirakurun 準拠) */
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
#define EPG_EVENT_RING_CAP 8192

typedef struct {
    uint64_t id;
    uint64_t hash;
} epg_hash_entry_t;

typedef struct {
    uint64_t seq;
    char    *json;
} epg_event_item_t;

static pthread_mutex_t  g_fetch_mtx = PTHREAD_MUTEX_INITIALIZER;
static char             g_epg_raw_sample[8192];
static int              g_epgdb_polled_once = 0;

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

/**
 * EIT + フォールバック統合後の番組表 JSON を前回ハッシュと比較し、
 * 新規/更新/削除イベントを /api/events/stream のリングバッファへ積む (要 g_fetch_mtx 保持)
 */
static int sync_eit_events_locked(void) {
    char *json = eit_build_programs_json(0);
    if (!json) return 0;

    size_t h_cap = 8192, h_cnt = 0;
    epg_hash_entry_t *new_hashes = (epg_hash_entry_t *)malloc(h_cap * sizeof(epg_hash_entry_t));
    int changed_count = 0;
    int64_t now_ms = (int64_t)time(NULL) * 1000LL;

    const char *p = json;
    while (*p && *p != '[') p++;
    if (*p == '[') p++;

    while (*p) {
        p = skip_ws(p);
        if (*p == ']' || *p == '\0') break;
        if (*p == ',') { p++; continue; }
        if (*p != '{') break;

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
        unsigned long long mir_pid = 0;
        if (sscanf(obj_start, "{\"id\":%llu,", &mir_pid) == 1 && mir_pid > 0) {
            uint64_t h = fnv1a_update(14695981039346656037ULL, obj_start, obj_len);
            if (new_hashes) {
                if (h_cnt >= h_cap) {
                    size_t ncap = h_cap * 2;
                    epg_hash_entry_t *nh = (epg_hash_entry_t *)realloc(new_hashes, ncap * sizeof(epg_hash_entry_t));
                    if (nh) { new_hashes = nh; h_cap = ncap; }
                }
                if (h_cnt < h_cap) {
                    new_hashes[h_cnt].id   = (uint64_t)mir_pid;
                    new_hashes[h_cnt].hash = h;
                    h_cnt++;
                }
            }
            if (g_prev_hashes) {
                epg_hash_entry_t key_entry = { (uint64_t)mir_pid, 0 };
                epg_hash_entry_t *found = (epg_hash_entry_t *)bsearch(
                    &key_entry, g_prev_hashes, g_prev_hash_cnt, sizeof(epg_hash_entry_t), cmp_hash_entry);
                if (!found) {
                    push_epg_event("create", obj_start, obj_len, now_ms);
                    changed_count++;
                } else if (found->hash != h) {
                    push_epg_event("update", obj_start, obj_len, now_ms);
                    changed_count++;
                }
            }
        }
    }
    free(json);

    if (new_hashes && h_cnt > 0) {
        qsort(new_hashes, h_cnt, sizeof(epg_hash_entry_t), cmp_hash_entry);
        /* 重複 ID があれば圧縮 */
        size_t uw = 0;
        for (size_t i = 0; i < h_cnt; i++) {
            if (uw > 0 && new_hashes[uw - 1].id == new_hashes[i].id) {
                new_hashes[uw - 1].hash = new_hashes[i].hash;
            } else {
                new_hashes[uw++] = new_hashes[i];
            }
        }
        h_cnt = uw;

        if (g_prev_hashes && g_prev_hash_cnt > 0) {
            size_t pi = 0, ni = 0;
            int rem_cnt = 0;
            while (pi < g_prev_hash_cnt && rem_cnt < 1024) {
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

    return changed_count;
}

/**
 * LG 純正 EPG DB (com.webos.epgdb) からプライマリ番組のみをフォールバック登録する (要 g_fetch_mtx 保持)
 */
static void poll_webos_epgdb_locked(void) {
    char next_tok[256] = {0};
    for (int page = 0; page < 40; page++) {
        char cmd[1024];
        if (next_tok[0]) {
            snprintf(cmd, sizeof(cmd),
                     "luna-send -n 1 -w 4000 luna://com.webos.epgdb/find "
                     "'{\"query\":{\"from\":\"com.webos.service.tv.programSCH:4\","
                     "\"select\":[\"programId\",\"startTime\",\"duration\",\"programName\",\"description\",\"genre\",\"genreList\",\"additionalInfo\"],"
                     "\"limit\":250,\"page\":\"%s\"}}' </dev/null",
                     next_tok);
        } else {
            snprintf(cmd, sizeof(cmd),
                     "luna-send -n 1 -w 4000 luna://com.webos.epgdb/find "
                     "'{\"query\":{\"from\":\"com.webos.service.tv.programSCH:4\","
                     "\"select\":[\"programId\",\"startTime\",\"duration\",\"programName\",\"description\",\"genre\",\"genreList\",\"additionalInfo\"],"
                     "\"limit\":250}}' </dev/null");
        }
        size_t rlen = 0;
        char *raw = run_cmd_capture(cmd, &rlen);
        if (!raw || rlen == 0) { free(raw); break; }

        if (page == 0) {
            size_t n = rlen < (sizeof(g_epg_raw_sample) - 1) ? rlen : (sizeof(g_epg_raw_sample) - 1);
            memcpy(g_epg_raw_sample, raw, n);
            g_epg_raw_sample[n] = '\0';
        }

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
            int genre_lv2 = 15;
            int is_primary = 1;
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
                } else if (strcmp(key, "genreList") == 0 && *rp == '[') {
                    const char *gend = skip_json_val(rp);
                    if (parse_json_int(rp + 1, &nv, NULL)) {
                        genre_lv2 = (int)nv;
                    }
                    rp = gend;
                } else if (strcmp(key, "additionalInfo") == 0 && *rp == '{') {
                    const char *aend = skip_json_val(rp);
                    if (aend) {
                        const char *ip = strstr(rp, "\"isPrimary\"");
                        if (ip && ip < aend) {
                            const char *c = strchr(ip, ':');
                            if (c && c < aend) {
                                c = skip_ws(c + 1);
                                if (strncmp(c, "false", 5) == 0) is_primary = 0;
                            }
                        }
                    }
                    rp = aend;
                } else {
                    rp = skip_json_val(rp);
                }
                if (!rp) break;
            }

            /* isPrimary: false のサブチャンネル共有複製レコードは取り込まない */
            if (!is_primary) continue;

            unsigned onid = 0, tsid = 0, sid = 0, evid = 0;
            if (sscanf(pid_str, "%u_%u_%u_%u", &onid, &tsid, &sid, &evid) == 4 && pname[0]) {
                int64_t start_ms = parse_webos_time_ms(stime_str);
                if (start_ms > 0 && dur_sec > 0) {
                    eit_ingest_fallback((uint16_t)onid, (uint16_t)sid, (uint16_t)evid,
                                        (int64_t)(start_ms / 1000), (uint32_t)dur_sec,
                                        pname, pdesc, genre_lv1, genre_lv1 >= 0 ? genre_lv2 : -1);
                }
            }
        }
        free(raw);
        if (!new_next[0]) break;
        snprintf(next_tok, sizeof(next_tok), "%s", new_next);
    }
    g_epgdb_polled_once = 1;
}

int tv_db_poll_epg_changes(void) {
    pthread_mutex_lock(&g_fetch_mtx);
    poll_webos_epgdb_locked();
    int chg = sync_eit_events_locked();
    pthread_mutex_unlock(&g_fetch_mtx);
    return chg;
}

int tv_db_sync_eit_events(void) {
    pthread_mutex_lock(&g_fetch_mtx);
    int chg = sync_eit_events_locked();
    pthread_mutex_unlock(&g_fetch_mtx);
    return chg;
}

char *tv_db_get_programs_json(uint64_t filter_sid, int force_refresh) {
    if (force_refresh || !g_epgdb_polled_once) {
        tv_db_poll_epg_changes();
    }
    return eit_build_programs_json(filter_sid);
}

char *tv_db_get_program_by_id_json(uint64_t program_id) {
    if (!g_epgdb_polled_once) {
        tv_db_poll_epg_changes();
    }
    return eit_build_program_json(program_id);
}

const char *tv_db_get_epg_raw_sample(void) {
    return g_epg_raw_sample;
}
