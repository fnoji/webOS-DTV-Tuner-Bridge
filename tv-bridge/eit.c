/**
 * eit.c: EIT (ARIB STD-B10) セクションから番組表を収集するモジュール
 *
 * - 188 バイト TS から PID 0x12-0x26 の EIT セクションを continuity_counter 追跡付きで組み立てる
 * - MPEG-2 PSI CRC-32 を検証し、パケット欠落やトランスポンダ切替時の破損セクションを完全排除する
 * - EIT[p/f] (0x4E/0x4F) と EIT[schedule actual/other] (0x50-0x6F) を解析する
 * - セクションヘッダ内の original_network_id (s[10..11]) / transport_stream_id (s[8..9]) を直接参照する
 * - short_event (0x4D), extended_event (0x4E), component (0x50), content (0x54),
 *   audio_component (0xC4), event_group (0xD6) を解析し、Mirakurun 互換 JSON を生成する
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "eit.h"
#include "arib_text.h"

#define EIT_MAX_PROGRAMS   32000
#define EIT_HASH_SIZE      65536
#define EIT_SECTION_MAX    4200
#define EIT_PID_FIRST      0x12
#define EIT_PID_LAST       0x26
#define EIT_MAX_SERVICES   512

typedef struct {
    uint16_t onid;
    uint16_t tsid;
    uint16_t sid;
    uint16_t parent_sid;
} svc_entry_t;

typedef struct {
    uint16_t onid, sid, eid;
    uint16_t ref_sid, ref_eid; /* 0xD6 event_group (shared) の参照先 */
    uint8_t  shared_sub;       /* 1 = 他サービスのイベント共有サブチャンネル */
    uint8_t  native;           /* 1 = EIT 由来, 0 = LG DB フォールバック */
    uint8_t  is_free;          /* 1 = 無料放送, 0 = 有料 */
    uint8_t  g1, g2, un1, un2;
    uint8_t  video_type;       /* component_type (既定 0xB3 = 179 = 1080i) */
    uint8_t  audio_type;       /* component_type (既定 0x03 = ステレオ) */
    int64_t  start;            /* unix 秒 */
    uint32_t dur;              /* 秒 */
    char     name[160];
    char     desc[384];
    char     ext_json[768];    /* "見出し":"本文",... の JSON オブジェクト中身 */
} prog_t;

static prog_t      *g_progs = NULL;
static int          g_count = 0;
static int          g_cap   = 0;
static int32_t      g_hash[EIT_HASH_SIZE];
static svc_entry_t  g_svcs[EIT_MAX_SERVICES];
static int          g_svc_count = 0;
static pthread_mutex_t g_mtx     = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_sec_mtx = PTHREAD_MUTEX_INITIALIZER;

/* ---- セクション組み立てバッファ (EIT は PID 0x12-0x26) ---- */
typedef struct {
    uint8_t buf[EIT_SECTION_MAX];
    size_t  len;
    size_t  need;
    int     active;
    int     last_cc;
} sec_t;

static sec_t g_sec[EIT_PID_LAST - EIT_PID_FIRST + 1];

/* ---- MPEG-2 PSI CRC-32 (poly=0x04C11DB7, init=0xFFFFFFFF) ---- */
static uint32_t g_crc_table[256];
static int      g_crc_init_done = 0;

static void init_crc32_table(void) {
    if (g_crc_init_done) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i << 24;
        for (int j = 0; j < 8; j++) {
            if (c & 0x80000000u) c = (c << 1) ^ 0x04C11DB7u;
            else c <<= 1;
        }
        g_crc_table[i] = c;
    }
    g_crc_init_done = 1;
}

static uint32_t crc32_mpeg2(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc = (crc << 8) ^ g_crc_table[((crc >> 24) ^ data[i]) & 0xFFu];
    }
    return crc;
}

/* ---- 小さな文字列バッファ ---- */
typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} sb_t;

static void sb_init(sb_t *sb, size_t cap) {
    sb->buf = (char *)malloc(cap);
    sb->len = 0;
    sb->cap = sb->buf ? cap : 0;
    if (sb->buf) sb->buf[0] = '\0';
}

static void sb_append_n(sb_t *sb, const char *s, size_t n) {
    if (!sb->buf || !s || n == 0) return;
    if (sb->len + n + 1 > sb->cap) {
        size_t ncap = sb->cap ? sb->cap * 2 : 4096;
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

static void sb_append(sb_t *sb, const char *s) {
    if (s) sb_append_n(sb, s, strlen(s));
}

static void sb_append_json(sb_t *sb, const char *s) {
    if (!sb->buf || !s) return;
    sb_append_n(sb, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        if (c == '"') sb_append_n(sb, "\\\"", 2);
        else if (c == '\\') sb_append_n(sb, "\\\\", 2);
        else if (c == '\n') sb_append_n(sb, "\\n", 2);
        else if (c == '\r') sb_append_n(sb, "\\r", 2);
        else if (c == '\t') sb_append_n(sb, "\\t", 2);
        else if (c < 0x20) {
            char esc[8];
            snprintf(esc, sizeof(esc), "\\u%04x", c);
            sb_append(sb, esc);
        } else {
            sb_append_n(sb, (const char *)p, 1);
        }
    }
    sb_append_n(sb, "\"", 1);
}

/* ---- サービス登録 & 検索 ---- */
void eit_clear_services(void) {
    pthread_mutex_lock(&g_mtx);
    g_svc_count = 0;
    pthread_mutex_unlock(&g_mtx);
}

void eit_register_service(uint16_t onid, uint16_t tsid, uint16_t sid, uint16_t parent_sid) {
    pthread_mutex_lock(&g_mtx);
    for (int i = 0; i < g_svc_count; i++) {
        if (g_svcs[i].onid == onid && g_svcs[i].sid == sid) {
            g_svcs[i].tsid = tsid;
            g_svcs[i].parent_sid = parent_sid;
            pthread_mutex_unlock(&g_mtx);
            return;
        }
    }
    if (g_svc_count < EIT_MAX_SERVICES) {
        g_svcs[g_svc_count].onid = onid;
        g_svcs[g_svc_count].tsid = tsid;
        g_svcs[g_svc_count].sid  = sid;
        g_svcs[g_svc_count].parent_sid = parent_sid;
        g_svc_count++;
    }
    pthread_mutex_unlock(&g_mtx);
}

static const svc_entry_t *find_service_locked(uint16_t onid, uint16_t sid) {
    for (int i = 0; i < g_svc_count; i++) {
        if (g_svcs[i].onid == onid && g_svcs[i].sid == sid) {
            return &g_svcs[i];
        }
    }
    return NULL;
}

/* ---- ハッシュ ---- */
static uint32_t prog_hash(uint16_t onid, uint16_t sid, uint16_t eid) {
    uint32_t h = 2166136261u;
    h = (h ^ onid) * 16777619u;
    h = (h ^ sid) * 16777619u;
    h = (h ^ eid) * 16777619u;
    return h;
}

static void hash_rebuild_locked(void) {
    memset(g_hash, 0xFF, sizeof(g_hash));
    for (int i = 0; i < g_count; i++) {
        uint32_t h = prog_hash(g_progs[i].onid, g_progs[i].sid, g_progs[i].eid) % EIT_HASH_SIZE;
        while (g_hash[h] >= 0) h = (h + 1) % EIT_HASH_SIZE;
        g_hash[h] = i;
    }
}

static int find_locked(uint16_t onid, uint16_t sid, uint16_t eid) {
    uint32_t h = prog_hash(onid, sid, eid) % EIT_HASH_SIZE;
    for (int probe = 0; probe < EIT_HASH_SIZE; probe++) {
        int32_t idx = g_hash[h];
        if (idx < 0) return -1;
        prog_t *p = &g_progs[idx];
        if (p->onid == onid && p->sid == sid && p->eid == eid) return idx;
        h = (h + 1) % EIT_HASH_SIZE;
    }
    return -1;
}

static void prune_locked(int64_t now) {
    int w = 0;
    for (int i = 0; i < g_count; i++) {
        if ((int64_t)g_progs[i].start + (int64_t)g_progs[i].dur < now - 6 * 3600) continue;
        if (w != i) g_progs[w] = g_progs[i];
        w++;
    }
    g_count = w;
    hash_rebuild_locked();
}

static void ensure_capacity_locked(void) {
    if (g_count < g_cap) return;
    if (g_cap >= EIT_MAX_PROGRAMS) {
        prune_locked((int64_t)time(NULL));
        if (g_count >= g_cap) {
            int drop = g_count / 8 + 1;
            memmove(g_progs, g_progs + drop, (size_t)(g_count - drop) * sizeof(prog_t));
            g_count -= drop;
            hash_rebuild_locked();
        }
        return;
    }
    int ncap = g_cap ? g_cap * 2 : 4096;
    if (ncap > EIT_MAX_PROGRAMS) ncap = EIT_MAX_PROGRAMS;
    prog_t *np = (prog_t *)realloc(g_progs, (size_t)ncap * sizeof(prog_t));
    if (!np) return;
    g_progs = np;
    g_cap = ncap;
}

/* UTF-8 の文字境界を壊さずに切り詰めてコピーする */
static void copy_utf8(char *dst, size_t dst_sz, const char *src) {
    if (!dst || dst_sz == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n >= dst_sz) {
        n = dst_sz - 1;
        while (n > 0 && (((unsigned char)src[n]) & 0xC0) == 0x80) n--;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void store_locked(uint16_t onid, uint16_t sid, uint16_t eid,
                         int64_t start, uint32_t dur, int is_free, int native,
                         int is_ext_table, int has_short,
                         const char *name, const char *desc, const char *ext_json,
                         int g1, int g2, int un1, int un2,
                         int video_type, int audio_type,
                         int has_group, uint16_t ref_sid, uint16_t ref_eid, int shared_sub) {
    if (g_svc_count > 0) {
        const svc_entry_t *sv = find_service_locked(onid, sid);
        if (!sv) return;
        /* LG DB フォールバック (native == 0) はメインサービスのみ取り込む */
        if (!native && sv->parent_sid != 0 && sv->parent_sid != sid) return;
    }

    int idx = find_locked(onid, sid, eid);
    if (idx >= 0) {
        prog_t *p = &g_progs[idx];
        if (!native && p->native) return;

        if (!is_ext_table) {
            p->start = start;
            p->dur = dur;
            p->is_free = (uint8_t)is_free;
        }
        if (native) p->native = 1;
        if (g1 >= 0) {
            p->g1  = (uint8_t)g1;
            p->g2  = (uint8_t)(g2 < 0 ? 15 : g2);
            p->un1 = (uint8_t)(un1 < 0 ? 0 : un1);
            p->un2 = (uint8_t)(un2 < 0 ? 0 : un2);
        }
        if (video_type > 0) p->video_type = (uint8_t)video_type;
        if (audio_type > 0) p->audio_type = (uint8_t)audio_type;

        if (has_short) {
            copy_utf8(p->name, sizeof(p->name), name ? name : "");
            copy_utf8(p->desc, sizeof(p->desc), desc ? desc : "");
        } else if (native && !is_ext_table && !p->native) {
            /* LG DB フォールバックの名前を、0x4D 無しの実 EIT セクションでクリア */
            p->name[0] = '\0';
            p->desc[0] = '\0';
        }
        if (ext_json && ext_json[0]) {
            copy_utf8(p->ext_json, sizeof(p->ext_json), ext_json);
        }
        if (has_group) {
            p->ref_sid = ref_sid;
            p->ref_eid = ref_eid;
            p->shared_sub = (uint8_t)shared_sub;
        }
        return;
    }

    ensure_capacity_locked();
    if (g_count >= g_cap) return;
    prog_t *p = &g_progs[g_count];
    memset(p, 0, sizeof(*p));
    p->onid = onid;
    p->sid  = sid;
    p->eid  = eid;
    p->start = start;
    p->dur   = dur;
    p->is_free = (uint8_t)is_free;
    p->native  = (uint8_t)native;
    p->g1  = (uint8_t)(g1 < 0 ? 0xFF : g1);
    p->g2  = (uint8_t)(g2 < 0 ? 0xFF : g2);
    p->un1 = (uint8_t)(un1 < 0 ? 0 : un1);
    p->un2 = (uint8_t)(un2 < 0 ? 0 : un2);
    p->video_type = (uint8_t)(video_type > 0 ? video_type : 0xB3);
    p->audio_type = (uint8_t)(audio_type > 0 ? audio_type : 3);
    if (has_short && name) copy_utf8(p->name, sizeof(p->name), name);
    if (has_short && desc) copy_utf8(p->desc, sizeof(p->desc), desc);
    if (ext_json && ext_json[0]) copy_utf8(p->ext_json, sizeof(p->ext_json), ext_json);
    if (has_group) {
        p->ref_sid = ref_sid;
        p->ref_eid = ref_eid;
        p->shared_sub = (uint8_t)shared_sub;
    }

    uint32_t h = prog_hash(onid, sid, eid) % EIT_HASH_SIZE;
    while (g_hash[h] >= 0) h = (h + 1) % EIT_HASH_SIZE;
    g_hash[h] = g_count;
    g_count++;
}

/* ---- 時間 (MJD + BCD) ---- */
static int bcd2(const uint8_t b) {
    int hi = b >> 4, lo = b & 0x0F;
    if (hi > 9 || lo > 9) return -1;
    return hi * 10 + lo;
}

static int64_t decode_start_time(const uint8_t *b) {
    int mjd = ((int)b[0] << 8) | b[1];
    int hh = bcd2(b[2]), mm = bcd2(b[3]), ss = bcd2(b[4]);
    if (mjd == 0xFFFF || hh < 0 || mm < 0 || ss < 0) return -1;
    int64_t days = (int64_t)mjd - 40587; /* MJD 40587 = 1970-01-01 */
    return days * 86400 + (int64_t)hh * 3600 + (int64_t)mm * 60 + ss - 9 * 3600;
}

static uint32_t decode_duration(const uint8_t *b) {
    if (b[0] == 0xFF && b[1] == 0xFF && b[2] == 0xFF) return 1; /* 終了未定 */
    int hh = bcd2(b[0]), mm = bcd2(b[1]), ss = bcd2(b[2]);
    if (hh < 0 || mm < 0 || ss < 0) return 0;
    return (uint32_t)(hh * 3600 + mm * 60 + ss);
}

/* ---- 記述子解析 ---- */
#define MAX_EXT_ITEMS 8

typedef struct {
    char    head[64];
    uint8_t raw[1024];
    size_t  raw_len;
} ext_item_accum_t;

static void parse_descriptors(uint16_t sid, uint16_t eid,
                              const uint8_t *d, size_t len,
                              int *has_short, char *name, size_t name_sz,
                              char *desc, size_t desc_sz,
                              char *ext_json, size_t ext_json_sz,
                              int *g1, int *g2, int *un1, int *un2,
                              int *video_type, int *audio_type,
                              int *has_group, uint16_t *ref_sid, uint16_t *ref_eid, int *shared_sub) {
    size_t p = 0;
    ext_item_accum_t ext_items[MAX_EXT_ITEMS];
    int ext_cnt = 0;
    memset(ext_items, 0, sizeof(ext_items));

    while (p + 2 <= len) {
        uint8_t tag = d[p];
        uint8_t dlen = d[p + 1];
        const uint8_t *b = d + p + 2;
        if (p + 2 + dlen > len) break;

        if (tag == 0x4D && dlen >= 5) { /* short_event_descriptor */
            *has_short = 1;
            size_t ep = 3; /* ISO_639_language_code (3 bytes) */
            if (ep < dlen) {
                size_t nlen = b[ep++];
                if (ep + nlen <= dlen) {
                    char tmp[512];
                    arib_to_utf8(b + ep, nlen, tmp, sizeof(tmp));
                    copy_utf8(name, name_sz, tmp);
                    ep += nlen;
                }
                if (ep < dlen) {
                    size_t tlen = b[ep++];
                    if (ep + tlen <= dlen) {
                        char tmp[1024];
                        arib_to_utf8(b + ep, tlen, tmp, sizeof(tmp));
                        copy_utf8(desc, desc_sz, tmp);
                    }
                }
            }
        } else if (tag == 0x4E && dlen >= 5) { /* extended_event_descriptor */
            /* b[0]: descriptor_number(4) | last_descriptor_number(4)
               b[1..3]: ISO_639_language_code(3)
               b[4]: length_of_items(1) */
            size_t items_len = b[4];
            if (5 + items_len <= dlen) {
                size_t ip = 5;
                size_t iend = 5 + items_len;
                while (ip < iend) {
                    size_t id_len = b[ip++];
                    if (ip + id_len > iend) break;
                    const uint8_t *id_ptr = b + ip;
                    ip += id_len;

                    if (ip >= iend) break;
                    size_t it_len = b[ip++];
                    if (ip + it_len > iend) break;
                    const uint8_t *it_ptr = b + ip;
                    ip += it_len;

                    if (id_len > 0) {
                        char hbuf[64] = {0};
                        arib_to_utf8(id_ptr, id_len, hbuf, sizeof(hbuf));
                        if (!hbuf[0]) snprintf(hbuf, sizeof(hbuf), "番組内容");
                        /* 既存見出しがあれば追記、無ければ新規スロット */
                        int slot = -1;
                        for (int k = 0; k < ext_cnt; k++) {
                            if (strcmp(ext_items[k].head, hbuf) == 0) { slot = k; break; }
                        }
                        if (slot < 0 && ext_cnt < MAX_EXT_ITEMS) {
                            slot = ext_cnt++;
                            copy_utf8(ext_items[slot].head, sizeof(ext_items[slot].head), hbuf);
                            ext_items[slot].raw_len = 0;
                        }
                        if (slot >= 0 && it_len > 0) {
                            size_t room = sizeof(ext_items[slot].raw) - ext_items[slot].raw_len;
                            size_t cp_n = it_len < room ? it_len : room;
                            memcpy(ext_items[slot].raw + ext_items[slot].raw_len, it_ptr, cp_n);
                            ext_items[slot].raw_len += cp_n;
                        }
                    } else if (it_len > 0) {
                        /* id_len == 0: 直前の項目の続き (2バイト漢字が分割されている場合があるため生バイトで結合) */
                        if (ext_cnt == 0) {
                            copy_utf8(ext_items[0].head, sizeof(ext_items[0].head), "番組内容");
                            ext_items[0].raw_len = 0;
                            ext_cnt = 1;
                        }
                        int slot = ext_cnt - 1;
                        size_t room = sizeof(ext_items[slot].raw) - ext_items[slot].raw_len;
                        size_t cp_n = it_len < room ? it_len : room;
                        memcpy(ext_items[slot].raw + ext_items[slot].raw_len, it_ptr, cp_n);
                        ext_items[slot].raw_len += cp_n;
                    }
                }
            }
        } else if (tag == 0x54 && dlen >= 2) { /* content_descriptor */
            if (*g1 < 0) {
                *g1  = (b[0] >> 4) & 0x0F;
                *g2  = b[0] & 0x0F;
                *un1 = (b[1] >> 4) & 0x0F;
                *un2 = b[1] & 0x0F;
            }
        } else if (tag == 0x50 && dlen >= 2) { /* component_descriptor */
            int stream_content = b[0] & 0x0F;
            int component_type = b[1];
            if ((stream_content == 0x01 || stream_content == 0x05) && *video_type <= 0) {
                *video_type = component_type;
            } else if (stream_content == 0x02 && *audio_type <= 0) {
                *audio_type = component_type;
            }
        } else if (tag == 0xC4 && dlen >= 3) { /* audio_component_descriptor */
            if (*audio_type <= 0) *audio_type = b[2];
        } else if (tag == 0xD6 && dlen >= 1) { /* event_group_descriptor */
            int gtype = (b[0] >> 4) & 0x0F;
            int ecnt  = b[0] & 0x0F;
            if (gtype == 1 && ecnt > 0 && 1 + ecnt * 4 <= (int)dlen) {
                *has_group = 1;
                uint16_t best_sid = (uint16_t)((b[1] << 8) | b[2]);
                uint16_t best_eid = (uint16_t)((b[3] << 8) | b[4]);
                for (int k = 1; k < ecnt; k++) {
                    size_t off = 1 + (size_t)k * 4;
                    uint16_t esid = (uint16_t)((b[off] << 8) | b[off + 1]);
                    uint16_t eeid = (uint16_t)((b[off + 2] << 8) | b[off + 3]);
                    if (esid < best_sid) {
                        best_sid = esid;
                        best_eid = eeid;
                    }
                }
                *ref_sid = best_sid;
                *ref_eid = best_eid;
                *shared_sub = (best_sid != sid || best_eid != eid) ? 1 : 0;
            }
        }

        p += 2 + dlen;
    }

    if (ext_cnt > 0 && ext_json && ext_json_sz > 0) {
        sb_t sb;
        sb_init(&sb, 1024);
        if (sb.buf) {
            int first = 1;
            for (int k = 0; k < ext_cnt; k++) {
                char body[1536] = {0};
                arib_to_utf8(ext_items[k].raw, ext_items[k].raw_len, body, sizeof(body));
                if (!body[0]) continue;

                /* ext_json_sz に収まるよう、JSON エスケープ前に UTF-8 境界で body を切り詰める */
                size_t head_len = strlen(ext_items[k].head);
                if (sb.len + head_len + 16 >= ext_json_sz) break;
                size_t max_body = (ext_json_sz - sb.len - head_len - 16);
                if (max_body > 600) max_body = 600;
                char trimmed_body[640];
                copy_utf8(trimmed_body, max_body < sizeof(trimmed_body) ? max_body : sizeof(trimmed_body), body);
                if (!trimmed_body[0]) break;

                sb_t item_sb;
                sb_init(&item_sb, 768);
                if (!item_sb.buf) break;
                if (!first) sb_append(&item_sb, ",");
                sb_append_json(&item_sb, ext_items[k].head);
                sb_append(&item_sb, ":");
                sb_append_json(&item_sb, trimmed_body);

                if (sb.len + item_sb.len + 1 >= ext_json_sz) {
                    free(item_sb.buf);
                    break;
                }
                sb_append_n(&sb, item_sb.buf, item_sb.len);
                free(item_sb.buf);
                first = 0;
            }
            if (sb.len > 0 && sb.len < ext_json_sz) {
                memcpy(ext_json, sb.buf, sb.len + 1);
            } else {
                ext_json[0] = '\0';
            }
            free(sb.buf);
        }
    }
}

static void parse_eit_section(const uint8_t *s, size_t n) {
    if (n < 18) return; /* 14B header + 4B CRC */
    uint8_t tid = s[0];
    /* 0x4E/0x4F (p/f actual/other), 0x50-0x5F (schedule actual), 0x60-0x6F (schedule other) */
    if (tid < 0x4E || tid > 0x6F) return;
    if ((s[1] & 0x80) == 0) return; /* section_syntax_indicator */

    int section_length = ((s[1] & 0x0F) << 8) | s[2];
    if (section_length < 15) return;
    size_t total = 3 + (size_t)section_length;
    if (total > n) return;

    /* MPEG-2 PSI CRC-32 検証 (破損・パケット欠落セクションを完全排除) */
    if (crc32_mpeg2(s, total) != 0) return;

    uint16_t sid  = (uint16_t)((s[3] << 8) | s[4]);
    uint16_t tsid = (uint16_t)((s[8] << 8) | s[9]);
    uint16_t onid = (uint16_t)((s[10] << 8) | s[11]);
    (void)tsid;

    int is_ext_table = ((tid >= 0x58 && tid <= 0x5F) || (tid >= 0x68 && tid <= 0x6F)) ? 1 : 0;

    size_t end = total - 4; /* CRC 4 バイトを除く */
    size_t p = 14;

    while (p + 12 <= end) {
        uint16_t eid = (uint16_t)((s[p] << 8) | s[p + 1]);
        int64_t start = decode_start_time(s + p + 2);
        uint32_t dur = decode_duration(s + p + 7);
        int is_free = ((s[p + 10] & 0x10) == 0) ? 1 : 0; /* free_CA_mode == 0 */
        int dlen = ((s[p + 10] & 0x0F) << 8) | s[p + 11];
        size_t dp = p + 12;
        if (dp + (size_t)dlen > end) break;
        if (!is_ext_table && (start < 0 || dur == 0)) {
            p = dp + (size_t)dlen;
            continue;
        }

        int has_short = 0, has_group = 0, shared_sub = 0;
        uint16_t ref_sid = 0, ref_eid = 0;
        char name[160] = {0};
        char desc[384] = {0};
        char ext_json[768] = {0};
        int g1 = -1, g2 = -1, un1 = 0, un2 = 0, video_type = 0, audio_type = 0;

        parse_descriptors(sid, eid, s + dp, (size_t)dlen,
                          &has_short, name, sizeof(name),
                          desc, sizeof(desc),
                          ext_json, sizeof(ext_json),
                          &g1, &g2, &un1, &un2,
                          &video_type, &audio_type,
                          &has_group, &ref_sid, &ref_eid, &shared_sub);

        pthread_mutex_lock(&g_mtx);
        store_locked(onid, sid, eid, start, dur, is_free, 1,
                     is_ext_table, has_short,
                     name, desc, ext_json,
                     g1, g2, un1, un2,
                     video_type, audio_type,
                     has_group, ref_sid, ref_eid, shared_sub);
        pthread_mutex_unlock(&g_mtx);

        p = dp + (size_t)dlen;
    }
}

/* ---- 公開 API ---- */
void eit_init(void) {
    init_crc32_table();
    memset(g_hash, 0xFF, sizeof(g_hash));
    memset(g_sec, 0, sizeof(g_sec));
    for (int i = 0; i <= EIT_PID_LAST - EIT_PID_FIRST; i++) {
        g_sec[i].last_cc = -1;
    }
}

void eit_set_mux(uint16_t onid, uint16_t tsid) {
    (void)onid;
    (void)tsid;
    pthread_mutex_lock(&g_sec_mtx);
    memset(g_sec, 0, sizeof(g_sec));
    for (int i = 0; i <= EIT_PID_LAST - EIT_PID_FIRST; i++) {
        g_sec[i].last_cc = -1;
    }
    pthread_mutex_unlock(&g_sec_mtx);
}

static void feed_section_bytes(sec_t *a, const uint8_t *q, size_t avail) {
    while (avail > 0) {
        if (a->need == 0) {
            if (avail < 3) {
                /* 3 バイト未満のヘッダ断片を保持 */
                if (a->len + avail <= 3) {
                    memcpy(a->buf + a->len, q, avail);
                    a->len += avail;
                    if (a->buf[0] == 0xFF) {
                        a->active = 0;
                        a->len = 0;
                        break;
                    }
                    if (a->len >= 3) {
                        size_t slen = ((size_t)(a->buf[1] & 0x0F) << 8) | a->buf[2];
                        a->need = 3 + slen;
                        if (a->need > EIT_SECTION_MAX || a->need < 18) {
                            a->active = 0;
                            a->len = 0;
                            a->need = 0;
                        }
                    }
                }
                break;
            }
            if (q[0] == 0xFF) {
                a->active = 0;
                a->len = 0;
                break;
            }
            size_t slen = ((size_t)(q[1] & 0x0F) << 8) | q[2];
            a->need = 3 + slen;
            if (a->need > EIT_SECTION_MAX || a->need < 18) {
                a->active = 0;
                a->len = 0;
                a->need = 0;
                break;
            }
        }
        size_t take = a->need - a->len;
        if (take > avail) take = avail;
        memcpy(a->buf + a->len, q, take);
        a->len += take;
        q += take;
        avail -= take;
        if (a->len == a->need) {
            parse_eit_section(a->buf, a->len);
            a->len = 0;
            a->need = 0;
        }
    }
}

void eit_feed_ts(const uint8_t *ts, size_t npkts) {
    pthread_mutex_lock(&g_sec_mtx);
    for (size_t i = 0; i < npkts; i++) {
        const uint8_t *p = ts + i * 188;
        if (p[0] != 0x47) continue;
        if (p[1] & 0x80) continue; /* transport_error_indicator */
        int pid = ((p[1] & 0x1F) << 8) | p[2];
        if (pid < EIT_PID_FIRST || pid > EIT_PID_LAST) continue;
        if ((p[3] & 0x10) == 0) continue; /* ペイロード無し */

        sec_t *a = &g_sec[pid - EIT_PID_FIRST];
        int pusi = (p[1] & 0x40) != 0;
        int cc   = p[3] & 0x0F;

        size_t off = 4;
        if (p[3] & 0x20) { /* adaptation_field あり */
            size_t alen = p[4];
            off = 5 + alen;
            if (off >= 188) continue;
        }

        if (pusi) {
            size_t ptr = p[off];
            if (off + 1 + ptr > 188) {
                a->active = 0;
                a->len = 0;
                a->need = 0;
                a->last_cc = -1;
                continue;
            }
            /* pointer_field より前のバイト列は前セクションの末尾 */
            if (a->active && ptr > 0 && a->last_cc >= 0 && ((a->last_cc + 1) & 0x0F) == cc) {
                feed_section_bytes(a, p + off + 1, ptr);
            }
            off += 1 + ptr;
            a->len = 0;
            a->need = 0;
            a->active = 1;
            a->last_cc = cc;
            if (off < 188) {
                feed_section_bytes(a, p + off, 188 - off);
            }
        } else {
            if (!a->active) continue;
            if (a->last_cc < 0 || ((a->last_cc + 1) & 0x0F) != cc) {
                /* パケット欠落による不連続: 組み立て中のセクションを破棄 */
                a->active = 0;
                a->len = 0;
                a->need = 0;
                a->last_cc = -1;
                continue;
            }
            a->last_cc = cc;
            feed_section_bytes(a, p + off, 188 - off);
        }
    }
    pthread_mutex_unlock(&g_sec_mtx);
}

void eit_ingest_fallback(uint16_t onid, uint16_t sid, uint16_t eid,
                         int64_t start, uint32_t duration,
                         const char *name, const char *description,
                         int genre1, int genre2) {
    if (!name || !name[0]) return;
    pthread_mutex_lock(&g_mtx);
    store_locked(onid, sid, eid, start, duration, 1, 0,
                 0, 1, name, description, NULL,
                 genre1, genre2, 0, 0,
                 0xB3, 3,
                 0, 0, 0, 0);
    pthread_mutex_unlock(&g_mtx);
}

static int is_subchannel_filler_or_same_as_parent_locked(const prog_t *p, uint16_t parent_sid) {
    /* BS サブチャンネル常時フィラー・通販・休止枠 */
    if (strstr(p->name, "放送休止") != NULL ||
        strstr(p->name, "Chill out Time") != NULL ||
        strstr(p->name, "Sound Shower") != NULL ||
        strstr(p->name, "BS日テレShopping") != NULL ||
        strstr(p->name, "ＢＳ日テレＳｈｏｐｐｉｎｇ") != NULL ||
        strstr(p->name, "麗しの宝石ショッピング") != NULL ||
        strstr(p->name, "マルチチャンネル編成") != NULL) {
        return 1;
    }
    /* 親チャンネルの同時間帯に同名番組があれば共有番組とみなす */
    for (int i = 0; i < g_count; i++) {
        const prog_t *m = &g_progs[i];
        if (m->onid == p->onid && m->sid == parent_sid && m->start == p->start) {
            if (m->name[0] && strcmp(m->name, p->name) == 0) return 1;
            break;
        }
    }
    return 0;
}

static int is_valid_main_program_locked(const prog_t *p) {
    if (!p->name[0]) return 0;
    if (p->shared_sub) return 0;
    if (p->start <= 0 || p->dur == 0) return 0;
    if (g_svc_count > 0) {
        const svc_entry_t *sv = find_service_locked(p->onid, p->sid);
        if (!sv) return 0;
        if (sv->parent_sid != 0 && sv->parent_sid != p->sid) {
            if (!p->native) return 0;
            if (is_subchannel_filler_or_same_as_parent_locked(p, sv->parent_sid)) return 0;
        }
    }
    return 1;
}

int eit_program_count(void) {
    int n = 0;
    pthread_mutex_lock(&g_mtx);
    for (int i = 0; i < g_count; i++) {
        if (is_valid_main_program_locked(&g_progs[i])) n++;
    }
    pthread_mutex_unlock(&g_mtx);
    return n;
}

int eit_native_count(void) {
    int n = 0;
    pthread_mutex_lock(&g_mtx);
    for (int i = 0; i < g_count; i++) {
        if (g_progs[i].native && is_valid_main_program_locked(&g_progs[i])) n++;
    }
    pthread_mutex_unlock(&g_mtx);
    return n;
}

static const char *video_res_str(uint8_t vtype) {
    switch (vtype) {
        case 0x01: case 0x02: case 0x03: case 0x04: return "480i";
        case 0xA1: case 0xA2: case 0xA3: case 0xA4: return "480p";
        case 0xB1: case 0xB2: case 0xB3: case 0xB4: return "1080i";
        case 0xC1: case 0xC2: case 0xC3: case 0xC4: return "720p";
        case 0xE1: case 0xE2: case 0xE3: case 0xE4: return "1080p";
        default: return "1080i";
    }
}

static void append_program_json(sb_t *sb, const prog_t *p) {
    uint64_t id = ((uint64_t)p->onid * 100000ULL + (uint64_t)p->sid) * 100000ULL + (uint64_t)p->eid;
    char head[256];
    snprintf(head, sizeof(head),
             "{\"id\":%llu,\"eventId\":%u,\"serviceId\":%u,\"networkId\":%u,"
             "\"startAt\":%lld,\"duration\":%lld,\"isFree\":%s,\"name\":",
             (unsigned long long)id, p->eid, p->sid, p->onid,
             (long long)p->start * 1000LL, (long long)p->dur * 1000LL,
             p->is_free ? "true" : "false");
    sb_append(sb, head);
    sb_append_json(sb, p->name);
    sb_append(sb, ",\"description\":");
    sb_append_json(sb, p->desc);

    if (p->ext_json[0]) {
        sb_append(sb, ",\"extended\":{");
        sb_append(sb, p->ext_json);
        sb_append(sb, "}");
    }

    if (p->g1 != 0xFF) {
        char g[96];
        snprintf(g, sizeof(g), ",\"genres\":[{\"lv1\":%d,\"lv2\":%d,\"un1\":%d,\"un2\":%d}]",
                 p->g1, p->g2 == 0xFF ? 15 : p->g2, p->un1, p->un2);
        sb_append(sb, g);
    }

    uint8_t vt = p->video_type ? p->video_type : 0xB3;
    int at = p->audio_type > 0 ? p->audio_type : 3;
    uint16_t r_sid = p->ref_sid ? p->ref_sid : p->sid;
    uint16_t r_eid = p->ref_eid ? p->ref_eid : p->eid;

    char tail[384];
    snprintf(tail, sizeof(tail),
             ",\"video\":{\"type\":\"mpeg2\",\"resolution\":\"%s\",\"streamContent\":1,\"componentType\":%u}"
             ",\"audio\":{\"componentType\":%d,\"isMain\":true,\"samplingRate\":48000,\"langs\":[\"jpn\"]}"
             ",\"audios\":[{\"componentType\":%d,\"isMain\":true,\"samplingRate\":48000,\"langs\":[\"jpn\"]}]"
             ",\"relatedItems\":[{\"type\":\"shared\",\"networkId\":%u,\"serviceId\":%u,\"eventId\":%u}]}",
             video_res_str(vt), vt, at, at, p->onid, r_sid, r_eid);
    sb_append(sb, tail);
}

static int matches_filter(const prog_t *p, uint64_t filter_sid) {
    if (filter_sid == 0) return 1;
    uint64_t mir_sid = (uint64_t)p->onid * 100000ULL + (uint64_t)p->sid;
    if (filter_sid >= 100000ULL) return mir_sid == filter_sid;
    return p->sid == (uint16_t)filter_sid;
}

char *eit_build_programs_json(uint64_t filter_sid) {
    sb_t sb;
    sb_init(&sb, 262144);
    if (!sb.buf) return NULL;
    sb_append(&sb, "[");
    int first = 1;
    int64_t now = (int64_t)time(NULL);

    pthread_mutex_lock(&g_mtx);
    for (int i = 0; i < g_count; i++) {
        const prog_t *p = &g_progs[i];
        if ((int64_t)p->start + (int64_t)p->dur < now - 12 * 3600) continue;
        if (!is_valid_main_program_locked(p)) continue;
        if (!matches_filter(p, filter_sid)) continue;
        if (!first) sb_append(&sb, ",");
        first = 0;
        append_program_json(&sb, p);
    }
    pthread_mutex_unlock(&g_mtx);

    sb_append(&sb, "]");
    return sb.buf;
}

char *eit_build_program_json(uint64_t program_id) {
    uint16_t eid = (uint16_t)(program_id % 100000ULL);
    uint64_t svc = program_id / 100000ULL;
    uint16_t sid = (uint16_t)(svc % 100000ULL);
    uint16_t onid = (uint16_t)(svc / 100000ULL);

    sb_t sb;
    sb_init(&sb, 1024);
    if (!sb.buf) return NULL;

    char *result = NULL;
    pthread_mutex_lock(&g_mtx);
    int idx = find_locked(onid, sid, eid);
    if (idx >= 0 && is_valid_main_program_locked(&g_progs[idx])) {
        sb.len = 0;
        if (sb.buf) sb.buf[0] = '\0';
        append_program_json(&sb, &g_progs[idx]);
        result = sb.buf;
    }
    pthread_mutex_unlock(&g_mtx);

    if (!result) free(sb.buf);
    return result;
}
