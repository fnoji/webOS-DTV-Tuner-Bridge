/**
 * multi2.c: ARIB STD-B25 MULTI2 ブロック暗号 & マルチパケット PAT/PMT/ECM 再構成エンジン
 */
#include "multi2.h"
#include <string.h>

static inline uint32_t load_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline void store_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
static inline uint32_t rotl32(uint32_t v, int n) {
    return (v << n) | (v >> (32 - n));
}

typedef struct { uint32_t l, r; } blk_t;

static inline blk_t pi1(blk_t p) { return (blk_t){p.l, p.r ^ p.l}; }
static inline blk_t pi2(blk_t p, uint32_t k1) {
    uint32_t y = p.r + k1;
    uint32_t z = rotl32(y, 1) + y - 1u;
    return (blk_t){p.l ^ rotl32(z, 4) ^ z, p.r};
}
static inline blk_t pi3(blk_t p, uint32_t k2, uint32_t k3) {
    uint32_t y = p.l + k2;
    uint32_t z = rotl32(y, 2) + y + 1u;
    uint32_t a = rotl32(z, 8) ^ z;
    uint32_t b = a + k3;
    uint32_t c = b + (b >> 31);
    return (blk_t){p.l, p.r ^ rotl32(c, 16) ^ (c | p.l)};
}
static inline blk_t pi4(blk_t p, uint32_t k4) {
    uint32_t y = p.r + k4;
    return (blk_t){p.l ^ (rotl32(y, 2) + y + 1u), p.r};
}

static blk_t multi2_encrypt(blk_t t, const uint32_t wk[8]) {
    for (int i = 0; i < 4; i++) {
        t = pi1(t); t = pi2(t, wk[0]); t = pi3(t, wk[1], wk[2]); t = pi4(t, wk[3]);
        t = pi1(t); t = pi2(t, wk[4]); t = pi3(t, wk[5], wk[6]); t = pi4(t, wk[7]);
    }
    return t;
}

static blk_t multi2_decrypt(blk_t t, const uint32_t wk[8]) {
    for (int i = 0; i < 4; i++) {
        t = pi4(t, wk[7]); t = pi3(t, wk[5], wk[6]); t = pi2(t, wk[4]); t = pi1(t);
        t = pi4(t, wk[3]); t = pi3(t, wk[1], wk[2]); t = pi2(t, wk[0]); t = pi1(t);
    }
    return t;
}

static void key_schedule(uint32_t wk[8], const uint8_t dk_b[8], const uint32_t sk[8]) {
    blk_t a0 = pi1((blk_t){load_be32(dk_b), load_be32(dk_b + 4)});
    blk_t a1 = pi2(a0, sk[0]);
    blk_t a2 = pi3(a1, sk[1], sk[2]);
    blk_t a3 = pi4(a2, sk[3]);
    blk_t a4 = pi1(a3);
    blk_t a5 = pi2(a4, sk[4]);
    blk_t a6 = pi3(a5, sk[5], sk[6]);
    blk_t a7 = pi4(a6, sk[7]);
    blk_t a8 = pi1(a7);
    wk[0] = a1.l; wk[1] = a2.r; wk[2] = a3.l; wk[3] = a4.r;
    wk[4] = a5.l; wk[5] = a6.r; wk[6] = a7.l; wk[7] = a8.r;
}

static void decrypt_cbc_ofb(uint8_t *buf, size_t n, uint32_t iv_l, uint32_t iv_r, const uint32_t wk[8]) {
    blk_t state = {iv_l, iv_r};
    while (n >= 8) {
        blk_t c = {load_be32(buf), load_be32(buf + 4)};
        blk_t d = multi2_decrypt(c, wk);
        store_be32(buf,     d.l ^ state.l);
        store_be32(buf + 4, d.r ^ state.r);
        state = c;
        buf += 8;
        n -= 8;
    }
    if (n > 0) {
        uint8_t t[8] = {0};
        memcpy(t, buf, n);
        blk_t ks = multi2_encrypt(state, wk);
        blk_t p = {load_be32(t) ^ ks.l, load_be32(t + 4) ^ ks.r};
        store_be32(t, p.l);
        store_be32(t + 4, p.r);
        memcpy(buf, t, n);
    }
}

static int get_or_add_ecm_slot(b25_ctx_t *ctx, uint16_t ecm_pid) {
    if (ecm_pid == 0 || ecm_pid >= 0x1fff) return -1;
    for (int i = 0; i < ctx->num_ecms; i++) {
        if (ctx->ecms[i].pid == ecm_pid) return i;
    }
    if (ctx->num_ecms >= B25_MAX_ECMS) return -1;
    int idx = ctx->num_ecms++;
    memset(&ctx->ecms[idx], 0, sizeof(ctx->ecms[idx]));
    ctx->ecms[idx].pid = ecm_pid;
    ctx->is_ecm[ecm_pid] = 1;
    return idx;
}

static b25_sec_asm_t *get_sec_asm(b25_ctx_t *ctx, uint16_t pid) {
    for (int i = 0; i < ctx->num_asms; i++) {
        if (ctx->sec_asms[i].pid == pid) return &ctx->sec_asms[i];
    }
    if (ctx->num_asms >= B25_MAX_SEC_ASM) return NULL;
    b25_sec_asm_t *a = &ctx->sec_asms[ctx->num_asms++];
    memset(a, 0, sizeof(*a));
    a->pid = pid;
    return a;
}

void b25_init(b25_ctx_t *ctx, b25_bcas_cb_t cb, void *user) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->bcas_cb = cb;
    ctx->bcas_user = user;
    ctx->nit_pid = 0x0010;
    memset(ctx->pmt_ver, 0xff, sizeof(ctx->pmt_ver));
}

void b25_set_pid_cb(b25_ctx_t *ctx, b25_pid_cb_t cb, void *user) {
    ctx->pid_cb = cb;
    ctx->pid_user = user;
}

void b25_set_syskey(b25_ctx_t *ctx, const uint8_t syskey[32], const uint8_t iv[8]) {
    for (int i = 0; i < 8; i++) ctx->sk[i] = load_be32(syskey + i * 4);
    ctx->iv_l = load_be32(iv);
    ctx->iv_r = load_be32(iv + 4);
    ctx->has_syskey = 1;
}

void b25_reset_stream(b25_ctx_t *ctx) {
    ctx->num_ecms = 0;
    ctx->num_asms = 0;
    ctx->pat_tsid = 0;
    ctx->pat_ver  = 0;
    ctx->nit_pid  = 0x0010;
    ctx->num_services = 0;
    memset(ctx->ecms, 0, sizeof(ctx->ecms));
    memset(ctx->sec_asms, 0, sizeof(ctx->sec_asms));
    memset(ctx->services, 0, sizeof(ctx->services));
    memset(ctx->pid_to_ecm, 0, sizeof(ctx->pid_to_ecm));
    memset(ctx->is_pmt, 0, sizeof(ctx->is_pmt));
    memset(ctx->is_ecm, 0, sizeof(ctx->is_ecm));
    memset(ctx->pmt_ver, 0xff, sizeof(ctx->pmt_ver));
}

static b25_service_t *get_or_add_service(b25_ctx_t *ctx, uint16_t sid, uint16_t pmt_pid) {
    for (int i = 0; i < ctx->num_services; i++) {
        if (ctx->services[i].sid == sid) {
            if (pmt_pid != 0) ctx->services[i].pmt_pid = pmt_pid;
            return &ctx->services[i];
        }
    }
    if (ctx->num_services >= B25_MAX_SERVICES) return NULL;
    b25_service_t *s = &ctx->services[ctx->num_services++];
    memset(s, 0, sizeof(*s));
    s->sid = sid;
    s->pmt_pid = pmt_pid;
    return s;
}

static void parse_pat(b25_ctx_t *ctx, const uint8_t *sec, size_t max_len) {
    if (max_len < 12 || sec[0] != 0x00) return;
    uint16_t slen = ((uint16_t)(sec[1] & 0x0fu) << 8) | sec[2];
    if ((size_t)(3 + slen) > max_len || slen < 9) return;
    ctx->pat_tsid = ((uint16_t)sec[3] << 8) | sec[4];
    ctx->pat_ver  = (sec[5] >> 1) & 0x1fu;
    size_t end = 3 + (size_t)slen - 4;
    for (size_t pos = 8; pos + 4 <= end; pos += 4) {
        uint16_t prog = ((uint16_t)sec[pos] << 8) | sec[pos + 1];
        uint16_t pid  = (((uint16_t)sec[pos + 2] & 0x1fu) << 8) | sec[pos + 3];
        if (prog == 0) {
            if (pid < 0x1fff) ctx->nit_pid = pid;
        } else if (pid < 0x1fff) {
            ctx->is_pmt[pid] = 1;
            get_or_add_service(ctx, prog, pid);
            if (ctx->pid_cb) ctx->pid_cb(ctx->pid_user, pid, 5);
        }
    }
}

static void parse_cat(b25_ctx_t *ctx, const uint8_t *sec, size_t max_len) {
    if (max_len < 12 || sec[0] != 0x01 || !ctx->pid_cb) return;
    uint16_t slen = ((uint16_t)(sec[1] & 0x0fu) << 8) | sec[2];
    if ((size_t)(3 + slen) > max_len || slen < 9) return;
    size_t end = 3 + (size_t)slen - 4;
    size_t p = 8;
    while (p + 2 <= end) {
        uint8_t tag = sec[p];
        uint8_t dlen = sec[p + 1];
        if (p + 2 + dlen > end) break;
        if (tag == 0x09 && dlen >= 4) {
            uint16_t casys = ((uint16_t)sec[p + 2] << 8) | sec[p + 3];
            uint16_t emm   = (((uint16_t)sec[p + 4] & 0x1fu) << 8) | sec[p + 5];
            if (casys == 0x0005 && emm < 0x1fff) {
                ctx->pid_cb(ctx->pid_user, emm, 5);
            }
        }
        p += 2 + dlen;
    }
}

static uint16_t find_ca_ecm_pid(const uint8_t *desc, size_t len) {
    size_t p = 0;
    while (p + 2 <= len) {
        uint8_t tag = desc[p];
        uint8_t dlen = desc[p + 1];
        if (p + 2 + dlen > len) break;
        if (tag == 0x09 && dlen >= 4) {
            uint16_t casys = ((uint16_t)desc[p + 2] << 8) | desc[p + 3];
            uint16_t epid  = (((uint16_t)desc[p + 4] & 0x1fu) << 8) | desc[p + 5];
            if (casys == 0x0005 && epid < 0x1fff) return epid;
        }
        p += 2 + dlen;
    }
    return 0;
}

static void parse_pmt(b25_ctx_t *ctx, uint16_t pmt_pid, const uint8_t *sec, size_t max_len) {
    if (max_len < 16 || sec[0] != 0x02) return;
    uint16_t prog = ((uint16_t)sec[3] << 8) | sec[4];
    uint8_t ver = (sec[5] >> 1) & 0x1fu;
    b25_service_t *svc = get_or_add_service(ctx, prog, pmt_pid);
    if (ctx->pmt_ver[pmt_pid] == ver && (!svc || svc->has_pmt)) return;

    uint16_t slen = ((uint16_t)(sec[1] & 0x0fu) << 8) | sec[2];
    if ((size_t)(3 + slen) > max_len || slen < 13) return;
    size_t end = 3 + (size_t)slen - 4;

    uint16_t pcr_pid = (((uint16_t)sec[8] & 0x1fu) << 8) | sec[9];
    if (ctx->pid_cb && pcr_pid > 0 && pcr_pid < 0x1fff) {
        ctx->pid_cb(ctx->pid_user, pcr_pid, 2);
    }

    uint16_t pilen = (((uint16_t)sec[10] & 0x0fu) << 8) | sec[11];
    size_t pos = 12;
    if (pos + pilen > end) return;

    uint16_t common_ecm = find_ca_ecm_pid(sec + pos, pilen);
    int common_idx = get_or_add_ecm_slot(ctx, common_ecm);
    if (ctx->pid_cb && common_ecm > 0 && common_ecm < 0x1fff) {
        ctx->pid_cb(ctx->pid_user, common_ecm, 5);
    }
    if (svc) {
        svc->pcr_pid = pcr_pid;
        svc->ecm_pid = common_ecm;
        svc->num_es  = 0;
    }
    pos += pilen;

    while (pos + 5 <= end) {
        uint8_t stype   = sec[pos];
        uint16_t es_pid = (((uint16_t)sec[pos + 1] & 0x1fu) << 8) | sec[pos + 2];
        uint16_t es_ilen = (((uint16_t)sec[pos + 3] & 0x0fu) << 8) | sec[pos + 4];
        pos += 5;
        if (pos + es_ilen > end) break;
        uint16_t es_ecm = find_ca_ecm_pid(sec + pos, es_ilen);
        int idx = (es_ecm != 0) ? get_or_add_ecm_slot(ctx, es_ecm) : common_idx;
        if (idx >= 0 && es_pid < 0x1fff) {
            ctx->pid_to_ecm[es_pid] = (uint8_t)(idx + 1);
        }
        if (svc && es_pid > 0 && es_pid < 0x1fff && svc->num_es < B25_MAX_SVC_ES) {
            svc->es_pids[svc->num_es++] = es_pid;
            if (es_ecm > 0 && es_ecm < 0x1fff && svc->ecm_pid == 0) {
                svc->ecm_pid = es_ecm;
            }
        }
        if (ctx->pid_cb && es_ecm > 0 && es_ecm < 0x1fff) {
            ctx->pid_cb(ctx->pid_user, es_ecm, 5);
        }
        if (ctx->pid_cb && es_pid > 0 && es_pid < 0x1fff) {
            if (stype == 0x01 || stype == 0x02 || stype == 0x1b || stype == 0x24) {
                ctx->pid_cb(ctx->pid_user, es_pid, 0); /* Video */
            } else if (stype == 0x03 || stype == 0x04 || stype == 0x0f || stype == 0x11) {
                ctx->pid_cb(ctx->pid_user, es_pid, 1); /* Audio */
            } else if (stype == 0x06) {
                ctx->pid_cb(ctx->pid_user, es_pid, 4); /* 字幕・文字スーパー */
            }
        }
        pos += es_ilen;
    }
    if (svc) svc->has_pmt = 1;
    ctx->pmt_ver[pmt_pid] = ver;
}

static void handle_ecm(b25_ctx_t *ctx, uint16_t ecm_pid, const uint8_t *sec, size_t max_len) {
    if (!ctx->has_syskey || !ctx->bcas_cb) return;
    if (max_len < 16 || (sec[0] != 0x82 && sec[0] != 0x83)) return;
    uint16_t slen = ((uint16_t)(sec[1] & 0x0fu) << 8) | sec[2];
    if ((size_t)(3 + slen) > max_len || slen < 13) return;

    const uint8_t *body = sec + 8;
    size_t body_len = (size_t)(3 + slen) - 12;
    if (body_len == 0 || body_len > 64) return;

    int idx = get_or_add_ecm_slot(ctx, ecm_pid);
    if (idx < 0) return;
    if (ctx->ecms[idx].valid && ctx->ecms[idx].last_body_len == body_len &&
        memcmp(ctx->ecms[idx].last_body, body, body_len) == 0) {
        return;
    }

    uint8_t tx[72];
    tx[0] = 0x90; tx[1] = 0x34; tx[2] = 0x00; tx[3] = 0x00;
    tx[4] = (uint8_t)body_len;
    memcpy(tx + 5, body, body_len);
    tx[5 + body_len] = 0x00;

    uint8_t rx[128];
    size_t rx_len = sizeof(rx);
    if (ctx->bcas_cb(ctx->bcas_user, tx, 6 + body_len, rx, &rx_len) == 0 && rx_len >= 24) {
        uint16_t rc = ((uint16_t)rx[4] << 8) | rx[5];
        if (rc == 0x0800) {
            key_schedule(ctx->ecms[idx].wk_odd,  rx + 6,  ctx->sk);
            key_schedule(ctx->ecms[idx].wk_even, rx + 14, ctx->sk);
            memcpy(ctx->ecms[idx].last_body, body, body_len);
            ctx->ecms[idx].last_body_len = body_len;
            ctx->ecms[idx].valid = 1;
        }
    }
}

static void dispatch_section(b25_ctx_t *ctx, uint16_t pid, const uint8_t *sec, size_t len) {
    if (pid == 0x0000) parse_pat(ctx, sec, len);
    else if (pid == 0x0001) parse_cat(ctx, sec, len);
    else if (ctx->is_pmt[pid]) parse_pmt(ctx, pid, sec, len);
    else if (ctx->is_ecm[pid]) handle_ecm(ctx, pid, sec, len);
}

static void feed_section_packet(b25_ctx_t *ctx, uint16_t pid, int pusi, const uint8_t *pay, size_t pay_len) {
    b25_sec_asm_t *a = get_sec_asm(ctx, pid);
    if (!a || pay_len == 0) return;

    size_t pos = 0;
    if (pusi) {
        uint8_t ptr = pay[0];
        pos = 1;
        if (ptr > 0 && a->collecting && pos + ptr <= pay_len) {
            size_t copy = ptr;
            if (a->len + copy <= sizeof(a->buf)) {
                memcpy(a->buf + a->len, pay + pos, copy);
                a->len += copy;
                if (a->expected == 0 && a->len >= 3) {
                    a->expected = 3u + ((((size_t)a->buf[1] & 0x0fu) << 8) | a->buf[2]);
                }
                if (a->expected > 0 && a->len >= a->expected) {
                    dispatch_section(ctx, pid, a->buf, a->expected);
                }
            }
        }
        a->collecting = 0;
        a->len = 0;
        a->expected = 0;
        pos += ptr;

        while (pos < pay_len) {
            if (pay[pos] == 0xff) break; /* stuffing */
            if (pos + 3 <= pay_len) {
                size_t slen = 3u + ((((size_t)pay[pos + 1] & 0x0fu) << 8) | pay[pos + 2]);
                if (slen > 4096 || slen < 3) break;
                if (pos + slen <= pay_len) {
                    dispatch_section(ctx, pid, pay + pos, slen);
                    pos += slen;
                    continue;
                }
            }
            /* 後続パケットへ続く不完全セクションを開始 */
            size_t rem = pay_len - pos;
            if (rem <= sizeof(a->buf)) {
                memcpy(a->buf, pay + pos, rem);
                a->len = rem;
                a->expected = (rem >= 3) ? (3u + ((((size_t)a->buf[1] & 0x0fu) << 8) | a->buf[2])) : 0;
                if (a->expected <= 4096) a->collecting = 1;
            }
            break;
        }
    } else if (a->collecting) {
        size_t copy = pay_len;
        if (a->len + copy > sizeof(a->buf)) {
            a->collecting = 0;
            return;
        }
        memcpy(a->buf + a->len, pay, copy);
        a->len += copy;
        if (a->expected == 0 && a->len >= 3) {
            a->expected = 3u + ((((size_t)a->buf[1] & 0x0fu) << 8) | a->buf[2]);
            if (a->expected > 4096) { a->collecting = 0; return; }
        }
        if (a->expected > 0 && a->len >= a->expected) {
            dispatch_section(ctx, pid, a->buf, a->expected);
            a->collecting = 0;
            a->len = 0;
            a->expected = 0;
        }
    }
}

static void process_packets_internal(b25_ctx_t *ctx, uint8_t *ts_buf, size_t num_packets, int do_decrypt) {
    for (size_t i = 0; i < num_packets; i++) {
        uint8_t *p = ts_buf + i * 188;
        if (p[0] != 0x47) continue;

        uint16_t pid = (((uint16_t)p[1] & 0x1fu) << 8) | p[2];
        uint8_t tsc  = (p[3] >> 6) & 3u;
        uint8_t afc  = (p[3] >> 4) & 3u;
        size_t off = 4;
        if (afc == 2 || afc == 3) off += 1u + p[4];
        if (off >= 188) continue;

        /* 平文セクションの解析 (PAT / CAT / PMT / ECM) */
        if (tsc == 0 && (afc == 1 || afc == 3)) {
            if (pid == 0x0000 || pid == 0x0001 || ctx->is_pmt[pid] || (do_decrypt && ctx->is_ecm[pid])) {
                feed_section_packet(ctx, pid, (p[1] & 0x40u) != 0, p + off, 188 - off);
            }
        }

        /* スクランブルされた ES の MULTI2 復号 */
        if (do_decrypt && (tsc == 2 || tsc == 3) && ctx->has_syskey && (afc == 1 || afc == 3)) {
            int eidx = (int)ctx->pid_to_ecm[pid] - 1;
            if (eidx < 0 && ctx->num_ecms > 0) eidx = 0;
            if (eidx >= 0 && ctx->ecms[eidx].valid) {
                const uint32_t *wk = (tsc == 3) ? ctx->ecms[eidx].wk_odd : ctx->ecms[eidx].wk_even;
                decrypt_cbc_ofb(p + off, 188 - off, ctx->iv_l, ctx->iv_r, wk);
                p[3] &= 0x3fu;
            }
        }
    }
}

void b25_process_packets(b25_ctx_t *ctx, uint8_t *ts_buf, size_t num_packets) {
    process_packets_internal(ctx, ts_buf, num_packets, 1);
}

void b25_scan_sections(b25_ctx_t *ctx, const uint8_t *ts_buf, size_t num_packets) {
    process_packets_internal(ctx, (uint8_t *)ts_buf, num_packets, 0);
}

static uint32_t mpeg2_crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint32_t)data[i] << 24;
        for (int b = 0; b < 8; b++) {
            if (crc & 0x80000000u) crc = (crc << 1) ^ 0x04c11db7u;
            else crc <<= 1;
        }
    }
    return crc;
}

static void build_single_service_pat_pkt(uint8_t *pkt, uint16_t tsid, uint8_t ver,
                                         uint16_t nit_pid, uint16_t sid, uint16_t pmt_pid, uint8_t *cc_io) {
    memset(pkt, 0xff, 188);
    uint8_t cc = (*cc_io) & 0x0fu;
    *cc_io = (uint8_t)((cc + 1u) & 0x0fu);

    pkt[0] = 0x47;
    pkt[1] = 0x40; /* PUSI=1, PID=0x0000 */
    pkt[2] = 0x00;
    pkt[3] = (uint8_t)(0x10u | cc);
    pkt[4] = 0x00; /* pointer_field */

    uint8_t *sec = pkt + 5;
    /* section_length = 5(header) + 4(NIT) + 4(Service) + 4(CRC) = 17 (0x011) */
    sec[0] = 0x00;
    sec[1] = 0xb0;
    sec[2] = 17;
    sec[3] = (uint8_t)(tsid >> 8);
    sec[4] = (uint8_t)(tsid & 0xffu);
    sec[5] = (uint8_t)(0xc1u | ((ver & 0x1fu) << 1));
    sec[6] = 0x00;
    sec[7] = 0x00;
    /* NIT (program 0) */
    sec[8]  = 0x00;
    sec[9]  = 0x00;
    sec[10] = (uint8_t)(0xe0u | ((nit_pid >> 8) & 0x1fu));
    sec[11] = (uint8_t)(nit_pid & 0xffu);
    /* Target Service */
    sec[12] = (uint8_t)(sid >> 8);
    sec[13] = (uint8_t)(sid & 0xffu);
    sec[14] = (uint8_t)(0xe0u | ((pmt_pid >> 8) & 0x1fu));
    sec[15] = (uint8_t)(pmt_pid & 0xffu);

    uint32_t crc = mpeg2_crc32(sec, 16);
    sec[16] = (uint8_t)(crc >> 24);
    sec[17] = (uint8_t)(crc >> 16);
    sec[18] = (uint8_t)(crc >> 8);
    sec[19] = (uint8_t)(crc & 0xffu);
}

static int is_common_psi_si_pid(uint16_t pid) {
    return (pid == 0x0001 || pid == 0x0010 || pid == 0x0011 || pid == 0x0012 ||
            pid == 0x0014 || pid == 0x0023 || pid == 0x0024 || pid == 0x0026 ||
            pid == 0x0027 || pid == 0x0028 || pid == 0x0029);
}

size_t b25_filter_service_packets(const b25_ctx_t *ctx, const uint8_t *in_ts, size_t num_packets,
                                  uint16_t sid, uint8_t *pat_cc_io, uint8_t *out_ts) {
    if (!ctx || ctx->num_services == 0) return 0;

    const b25_service_t *svc = NULL;
    for (int i = 0; i < ctx->num_services; i++) {
        if (ctx->services[i].sid == sid) {
            svc = &ctx->services[i];
            break;
        }
    }
    /* マルチ編成サブch休止時などは同一系列のメインサービスまたは先頭サービスへフォールバック */
    if (!svc) {
        for (int i = 0; i < ctx->num_services; i++) {
            if ((ctx->services[i].sid & ~0x0007u) == (sid & ~0x0007u)) {
                svc = &ctx->services[i];
                break;
            }
        }
        if (!svc) svc = &ctx->services[0];
    }

    size_t out_cnt = 0;
    for (size_t i = 0; i < num_packets; i++) {
        const uint8_t *p = in_ts + i * 188u;
        if (p[0] != 0x47) continue;
        uint16_t pid = (((uint16_t)p[1] & 0x1fu) << 8) | p[2];
        if (pid == 0x1fff) continue;

        if (pid == 0x0000) {
            if ((p[1] & 0x40u) && svc->pmt_pid > 0) {
                build_single_service_pat_pkt(out_ts + out_cnt * 188u,
                                             ctx->pat_tsid, ctx->pat_ver,
                                             ctx->nit_pid ? ctx->nit_pid : 0x0010,
                                             svc->sid, svc->pmt_pid, pat_cc_io);
                out_cnt++;
            }
            continue;
        }

        int keep = 0;
        if (pid == svc->pmt_pid || is_common_psi_si_pid(pid)) {
            keep = 1;
        } else if (svc->has_pmt) {
            if (pid == svc->pcr_pid || (svc->ecm_pid && pid == svc->ecm_pid)) {
                keep = 1;
            } else {
                for (int k = 0; k < svc->num_es; k++) {
                    if (svc->es_pids[k] == pid) {
                        keep = 1;
                        break;
                    }
                }
            }
        }

        if (keep) {
            memcpy(out_ts + out_cnt * 188u, p, 188u);
            out_cnt++;
        }
    }
    return out_cnt;
}
