/**
 * libwebos_tuner.c: tvservice プロセス内へ注入する共有ライブラリ (.so)
 * すべてのフロントエンド / SDEC / B-CAS 関数を dlsym() で動的解決し、
 * 127.0.0.1:40570 のローカル TCP 経由でチューナー1の裏選局と B-CAS APDU 中継を提供する。
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#define __USE_GNU 1
#include <dlfcn.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* tvservice 内部シンボル関数型定義 */
typedef int (*fn_fe_sig_wait_t)(int tuner, uint32_t timeout_ms);
typedef int (*fn_fe_sig_post_t)(int tuner);
typedef int (*fn_fe_tune_dtv_t)(int tuner);
typedef int (*fn_fe_ctrl_out_t)(int tuner, int on_off);
typedef int (*fn_dil_chg_media_t)(uint8_t tuner, uint8_t media);
typedef int (*fn_dil_chk_lock_t)(int tuner, int *lock_out);
typedef int (*fn_dil_sat_lnb_t)(int tuner, int on_off);
typedef int (*fn_dil_sat_22k_t)(int tuner, int on_off);
typedef int (*fn_hal_get_media_t)(int tuner, int *media_out);
typedef int (*fn_dil_sdec_in_t)(uint8_t ch, int port, int input_type, int cas_type);
typedef int (*fn_dil_sdec_rst_t)(uint8_t ch, uint32_t scope);
typedef int (*fn_dil_sdec_pid_t)(uint8_t ch, int pid_type, uint16_t pid, int pes_dest);

typedef int (*fn_pvr_open_dn_t)(uint8_t dn_ch);
typedef int (*fn_pvr_close_dn_t)(uint8_t dn_ch);
typedef int (*fn_pvr_start_dn_t)(uint8_t dn_ch);
typedef int (*fn_pvr_stop_dn_t)(uint8_t dn_ch);
typedef int (*fn_pvr_bufinfo_t)(uint8_t dn_ch, uint32_t *base_out, uint32_t *size_out);
typedef int (*fn_pvr_getbuf_t)(uint8_t dn_ch, uint32_t *rp_out, uint32_t *wp_out, void **idx_out, uint32_t *idx_sz_out);
typedef int (*fn_pvr_retbuf_t)(uint8_t dn_ch, uint32_t read_ptr, uint32_t write_ptr);
typedef int (*fn_pvr_setenc_t)(uint32_t state);

typedef int (*fn_bcas_sem_wait_t)(void *sem, int timeout);
typedef int (*fn_bcas_sem_sig_t)(void *sem);
typedef int (*fn_bcas_is_blk_t)(void);
typedef int (*fn_bcas_encode_blk_t)(uint8_t nad, uint8_t *apdu, uint16_t *len, uint8_t *out, uint8_t *seq, int proto);
typedef int (*fn_bcas_xfer_t)(uint8_t slot, uint8_t *tx, uint16_t tx_len, uint8_t *rx, uint16_t *rx_len);
typedef int (*fn_bcas_get_card_id_t)(uint8_t sc_idx, void *out_str, void *out_len);

static fn_fe_sig_wait_t     p_fe_wait     = NULL;
static fn_fe_sig_post_t     p_fe_post     = NULL;
static fn_fe_tune_dtv_t     p_fe_tunedtv  = NULL;
static fn_fe_ctrl_out_t     p_fe_ctrlout  = NULL;
static fn_dil_chg_media_t   p_dil_chgmed  = NULL;
static fn_dil_chk_lock_t    p_dil_lock    = NULL;
static fn_dil_sat_lnb_t     p_dil_lnb     = NULL;
static fn_dil_sat_22k_t     p_dil_22k     = NULL;
static fn_hal_get_media_t   p_hal_getmed  = NULL;
static fn_dil_sdec_in_t     p_sdec_in     = NULL;
static fn_dil_sdec_rst_t    p_sdec_rst    = NULL;
static fn_dil_sdec_pid_t    p_sdec_pid    = NULL;

static fn_pvr_open_dn_t     p_pvr_open    = NULL;
static fn_pvr_close_dn_t    p_pvr_close   = NULL;
static fn_pvr_start_dn_t    p_pvr_start   = NULL;
static fn_pvr_stop_dn_t     p_pvr_stop    = NULL;
static fn_pvr_bufinfo_t     p_pvr_binfo   = NULL;
static fn_pvr_getbuf_t      p_pvr_getbuf  = NULL;
static fn_pvr_retbuf_t      p_pvr_retbuf  = NULL;
static fn_pvr_setenc_t      p_pvr_setenc  = NULL;

static fn_bcas_sem_wait_t   p_sem_wait    = NULL;
static fn_bcas_sem_sig_t    p_sem_sig     = NULL;
static fn_bcas_is_blk_t     p_is_blk      = NULL;
static fn_bcas_encode_blk_t p_enc_blk     = NULL;
static fn_bcas_xfer_t       p_xfer_apdu   = NULL;
static fn_bcas_get_card_id_t p_get_cardid = NULL;

static uint8_t *g_info_fe     = NULL;
static uint8_t *g_bcas_table  = NULL;
static int      g_syms_ok     = 0;
static int      g_pvr_syms_ok = 0;
static volatile int g_running = 1;

static pthread_mutex_t g_tune_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_bcas_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_pvr_mtx  = PTHREAD_MUTEX_INITIALIZER;

static volatile int      g_pvr_active   = 0;
static uint8_t           g_pvr_dn_ch    = 0;
static uint8_t           g_pvr_sdec_ch  = 1;
static uint32_t          g_pvr_base     = 0;
static uint32_t          g_pvr_size     = 0;
static uint64_t          g_pvr_bytes    = 0;
static int               g_pvr_cli_fd   = -1;

#define GINFO_FE_STRIDE 480u
#define TUNER_PORT      40570
#define PVR_STREAM_PORT 40571

/* Thumb-2 MOVW / MOVT ペア (32bit 命令) の 16bit 即値をデコードする */
static int decode_thumb2_mov16(uint16_t hw1, uint16_t hw2, int *rd_out, uint16_t *imm16_out, int *is_movt_out) {
    /* MOVW: 1111 0 i 10 0 1 0 0 imm4 | 0 imm3 Rd imm8 -> hw1 & 0xfbf0 == 0xf240 */
    /* MOVT: 1111 0 i 10 1 1 0 0 imm4 | 0 imm3 Rd imm8 -> hw1 & 0xfbf0 == 0xf2c0 */
    if ((hw1 & 0xfb70u) != 0xf240u) return 0;
    if ((hw2 & 0x8000u) != 0) return 0;
    int is_movt = ((hw1 >> 7) & 1u) != 0;
    uint16_t i_bit = (hw1 >> 10) & 1u;
    uint16_t imm4  = hw1 & 0x000fu;
    uint16_t imm3  = (hw2 >> 12) & 0x0007u;
    int rd         = (hw2 >> 8) & 0x000fu;
    uint16_t imm8  = hw2 & 0x00ffu;
    uint16_t imm16 = (uint16_t)((imm4 << 12) | (i_bit << 11) | (imm3 << 8) | imm8);
    *rd_out = rd;
    *imm16_out = imm16;
    *is_movt_out = is_movt;
    return 1;
}

/** 関数の先頭 Thumb-2 命令群から MOVW/MOVT で構築される 32bit ポインタを自動抽出する */
static uint8_t *scan_movw_movt_ptr(void *func_ptr, size_t max_bytes) {
    if (!func_ptr) return NULL;
    uintptr_t raw = (uintptr_t)func_ptr & ~(uintptr_t)1;
    const uint16_t *code = (const uint16_t *)raw;
    size_t halfwords = max_bytes / 2;

    uint16_t low16[16] = {0};
    uint8_t  has_low[16] = {0};

    for (size_t i = 0; i + 1 < halfwords; ) {
        uint16_t hw1 = code[i];
        /* 32bit Thumb-2 命令か判定 */
        if ((hw1 & 0xe000u) == 0xe000u && (hw1 & 0x1800u) != 0x0000u) {
            uint16_t hw2 = code[i + 1];
            int rd = 0, is_movt = 0;
            uint16_t imm16 = 0;
            if (decode_thumb2_mov16(hw1, hw2, &rd, &imm16, &is_movt)) {
                if (!is_movt) {
                    low16[rd] = imm16;
                    has_low[rd] = 1;
                } else if (has_low[rd]) {
                    uint32_t val = ((uint32_t)imm16 << 16) | low16[rd];
                    if (val >= 0x01000000u && val < 0x40000000u) {
                        return (uint8_t *)(uintptr_t)val;
                    }
                }
            }
            i += 2;
        } else {
            i += 1;
        }
    }
    return NULL;
}

static int resolve_symbols(void) {
    p_fe_wait    = (fn_fe_sig_wait_t)dlsym(RTLD_DEFAULT, "FE_AccessSignalWait");
    p_fe_post    = (fn_fe_sig_post_t)dlsym(RTLD_DEFAULT, "FE_AccessSignalPost");
    p_fe_tunedtv = (fn_fe_tune_dtv_t)dlsym(RTLD_DEFAULT, "FE_INTF_TU_TuneDTV");
    p_fe_ctrlout = (fn_fe_ctrl_out_t)dlsym(RTLD_DEFAULT, "FE_INTF_TU_ControlOutput");
    p_dil_chgmed = (fn_dil_chg_media_t)dlsym(RTLD_DEFAULT, "DIL_TU_ChangeTransMedia");
    p_dil_lock   = (fn_dil_chk_lock_t)dlsym(RTLD_DEFAULT, "DIL_TU_CheckLockState");
    p_dil_lnb    = (fn_dil_sat_lnb_t)dlsym(RTLD_DEFAULT, "DIL_TU_SATSetLNBPower");
    p_dil_22k    = (fn_dil_sat_22k_t)dlsym(RTLD_DEFAULT, "DIL_TU_SATSet22khzOnOff");
    p_hal_getmed = (fn_hal_get_media_t)dlsym(RTLD_DEFAULT, "HAL_DVBV5_GetCurrentTransMedia");
    p_sdec_in    = (fn_dil_sdec_in_t)dlsym(RTLD_DEFAULT, "DIL_SDEC_SetInputConfig");
    p_sdec_rst   = (fn_dil_sdec_rst_t)dlsym(RTLD_DEFAULT, "DIL_SDEC_ResetFilters");
    p_sdec_pid   = (fn_dil_sdec_pid_t)dlsym(RTLD_DEFAULT, "DIL_SDEC_SetPID");
    g_info_fe    = (uint8_t *)dlsym(RTLD_DEFAULT, "gInfoFE");

    p_pvr_open   = (fn_pvr_open_dn_t)dlsym(RTLD_DEFAULT, "HAL_PVR_OpenDownloadDevice");
    p_pvr_close  = (fn_pvr_close_dn_t)dlsym(RTLD_DEFAULT, "HAL_PVR_CloseDownloadDevice");
    p_pvr_start  = (fn_pvr_start_dn_t)dlsym(RTLD_DEFAULT, "HAL_PVR_StartDownload");
    p_pvr_stop   = (fn_pvr_stop_dn_t)dlsym(RTLD_DEFAULT, "HAL_PVR_StopDownload");
    p_pvr_binfo  = (fn_pvr_bufinfo_t)dlsym(RTLD_DEFAULT, "HAL_PVR_GetDownloadBufferInfo");
    p_pvr_getbuf = (fn_pvr_getbuf_t)dlsym(RTLD_DEFAULT, "HAL_PVR_GetDownloadBuffer");
    p_pvr_retbuf = (fn_pvr_retbuf_t)dlsym(RTLD_DEFAULT, "HAL_PVR_ReturnDownloadBuffer");
    p_pvr_setenc = (fn_pvr_setenc_t)dlsym(RTLD_DEFAULT, "HAL_PVR_CRYPTO_SetEncryptionState");

    p_sem_wait   = (fn_bcas_sem_wait_t)dlsym(RTLD_DEFAULT, "BCAS_SemaphoreWait");
    p_sem_sig    = (fn_bcas_sem_sig_t)dlsym(RTLD_DEFAULT, "BCAS_SemaphoreSignal");
    p_is_blk     = (fn_bcas_is_blk_t)dlsym(RTLD_DEFAULT, "BCAS_SmartCardIsBlockEncodingSupported");
    p_enc_blk    = (fn_bcas_encode_blk_t)dlsym(RTLD_DEFAULT, "BCAS_EncodeBlock");
    p_xfer_apdu  = (fn_bcas_xfer_t)dlsym(RTLD_DEFAULT, "BCAS_SmartCardTransferAPDU");
    p_get_cardid = (fn_bcas_get_card_id_t)dlsym(RTLD_DEFAULT, "BCAS_GetCardIDInfoInString");

    /* g_bcas_table を BCAS_GetCardIDInfoInString の冒頭命令から自動探索 */
    if (p_get_cardid) {
        g_bcas_table = scan_movw_movt_ptr((void *)p_get_cardid, 96);
    }

    g_syms_ok = (p_fe_wait && p_fe_post && p_fe_tunedtv && p_fe_ctrlout &&
                 p_dil_chgmed && p_dil_lock && p_dil_lnb && p_dil_22k &&
                 p_hal_getmed && p_sdec_in && g_info_fe);
    g_pvr_syms_ok = (p_sdec_rst && p_sdec_pid && p_pvr_open && p_pvr_close &&
                     p_pvr_start && p_pvr_stop && p_pvr_binfo &&
                     p_pvr_getbuf && p_pvr_retbuf && p_pvr_setenc);
    return g_syms_ok;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex_to_bytes(const char *hex, uint8_t *out, int max_out) {
    int n = 0;
    while (*hex && n < max_out) {
        while (*hex == ' ' || *hex == '\t' || *hex == '\r' || *hex == '\n') hex++;
        if (!hex[0] || !hex[1]) break;
        int hi = hex_val(hex[0]), lo = hex_val(hex[1]);
        if (hi < 0 || lo < 0) break;
        out[n++] = (uint8_t)((hi << 4) | lo);
        hex += 2;
    }
    return n;
}

static const char *parse_u32(const char *s, uint32_t *out) {
    while (*s == ' ' || *s == '\t') s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        if (hex_val(*s) < 0) return NULL;
        uint32_t v = 0;
        int d;
        while ((d = hex_val(*s)) >= 0) {
            v = (v << 4) | (uint32_t)d;
            s++;
        }
        *out = v;
        return s;
    }
    if (*s < '0' || *s > '9') return NULL;
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10u + (uint32_t)(*s - '0');
        s++;
    }
    *out = v;
    return s;
}

/**
 * 指定チューナー (0 または 1) を物理周波数・TSID・放送種別へ選局する共通関数
 */
static int cmd_tune(int tuner, uint32_t media, uint32_t phy, uint32_t freq_khz, uint32_t tsid,
                    uint32_t lnb, uint32_t sr, char *resp, size_t rsz) {
    if (!g_syms_ok) return snprintf(resp, rsz, "ERR syms_not_ready\n");
    if (tuner != 0 && tuner != 1) return snprintf(resp, rsz, "ERR invalid tuner=%d\n", tuner);
    if (media != 1 && media != 4 && media != 5) {
        return snprintf(resp, rsz, "ERR invalid media=%u\n", media);
    }

    pthread_mutex_lock(&g_tune_mtx);

    /* Tuner 1 選局時: PVR が CH[1] を占有中でなければ EXT_INPUT1 を再設定してサスペンド復帰を保証 */
    if (tuner == 1 && !(g_pvr_active && g_pvr_sdec_ch == 1)) {
        p_sdec_in(1, 4, 0, 0);
    }

    int cur_m = -1;
    p_hal_getmed(tuner, &cur_m);

    int wr = p_fe_wait(tuner, 3000);
    int cm_ret = 999, tune_ret = -1, out_ret = -1, lck_target = -1, lck_other = -1;
    if (wr == 0) {
        uint8_t *fe = g_info_fe + (uint32_t)tuner * GINFO_FE_STRIDE;
        if (media == 1) {
            /* 地上デジタル ISDB-T */
            *(volatile uint16_t *)(fe + 0x10) = 6000;
            *(volatile uint32_t *)(fe + 0x58) = (0x7f000000u | (phy & 0xffffu));
            *(volatile uint32_t *)(fe + 0x5c) = 1u;
            *(volatile uint32_t *)(fe + 0x60) = 16u;
            *(volatile uint32_t *)(fe + 0x68) = 0x00130024u;
        } else {
            /* 衛星 BS (media=4) / CS110 (media=5) */
            *(volatile uint16_t *)(fe + 0x10) = (uint16_t)(sr ? sr : 28860);
            *(volatile uint32_t *)(fe + 0x58) = ((tsid & 0xffffu) << 16) | (phy & 0xffffu);
            *(volatile uint32_t *)(fe + 0x5c) = media;
            *(volatile uint32_t *)(fe + 0x60) = 16u;
            *(volatile uint32_t *)(fe + 0x68) = (media == 4) ? 0x64431eecu : 0x64431eedu;
            p_dil_lnb(tuner, (int)lnb);
            p_dil_22k(tuner, 0);
        }
        *(volatile uint32_t *)(fe + 0x6c)  = phy;
        *(volatile uint32_t *)(fe + 0x70)  = freq_khz;
        *(volatile uint32_t *)(fe + 0x74)  = freq_khz;
        *(volatile uint32_t *)(fe + 0x7c)  = 0u;
        *(volatile uint32_t *)(fe + 0x178) = 0u;
        *(volatile uint32_t *)(fe + 0x17c) = 0u;
        *(volatile uint8_t  *)(fe + 0x1c1) = 0u;
        *(volatile uint32_t *)(fe + 0x1d4) = 0u;

        if (cur_m != (int)media) cm_ret = p_dil_chgmed((uint8_t)tuner, (uint8_t)media);
        tune_ret = p_fe_tunedtv(tuner);
        if (tune_ret == 0) out_ret = p_fe_ctrlout(tuner, 1);
        p_fe_post(tuner);

        for (int i = 0; i < 50; i++) {
            usleep(50000);
            p_dil_lock(tuner, &lck_target);
            if (lck_target == 0) break;
        }
    }
    p_dil_lock(tuner ^ 1, &lck_other);
    pthread_mutex_unlock(&g_tune_mtx);

    int lck1 = (tuner == 1) ? lck_target : lck_other;
    int lck0 = (tuner == 0) ? lck_target : lck_other;
    return snprintf(resp, rsz,
                    "OK tuner=%d cached=0 wait=%d chgMedia=%d tuneDtv=%d ctrlOut=%d media=%u phy=%u freq=%u tsid=0x%04x lock1=%d lock0=%d lock=%d\n",
                    tuner, wr, cm_ret, tune_ret, out_ret, media, phy, freq_khz, tsid, lck1, lck0, lck_target);
}

static int cmd_bcas(const char *hex_in, char *resp, size_t rsz) {
    if (!p_xfer_apdu || !p_is_blk || !p_enc_blk) {
        return snprintf(resp, rsz, "ERR bcas_syms_missing\n");
    }
    uint8_t raw[300], blk_buf[320], rx_buf[1280];
    int raw_len = hex_to_bytes(hex_in, raw, sizeof(raw));
    if (raw_len <= 0) return snprintf(resp, rsz, "ERR bad_hex\n");

    pthread_mutex_lock(&g_bcas_mtx);

    /* g_bcas_table から inst と sem / seq_ptr を安全取得 */
    void *inst = NULL;
    void *sem = NULL;
    uint8_t *seq_ptr = NULL;
    if (g_bcas_table) {
        uint8_t cnt = *(volatile uint8_t *)(g_bcas_table + 0);
        if (cnt >= 1 && cnt <= 8) {
            inst = *(void * volatile *)(g_bcas_table + 16);
            if (inst) {
                sem = *(void * volatile *)((uint8_t *)inst + 20);
                seq_ptr = (uint8_t *)inst + 128 + 56;
            }
        }
    }

    uint8_t local_seq = 0;
    if (!seq_ptr) seq_ptr = &local_seq;

    uint16_t tx_len = (uint16_t)raw_len;
    uint16_t rx_len = sizeof(rx_buf);
    memset(blk_buf, 0, sizeof(blk_buf));
    memset(rx_buf, 0xff, sizeof(rx_buf));

    int sem_locked = 0;
    if (sem && p_sem_wait && p_sem_sig) {
        if (p_sem_wait(sem, -1) == 0) sem_locked = 1;
    }

    int er = 0, xr = -999;
    if (p_is_blk()) {
        er = p_enc_blk(0, raw, &tx_len, blk_buf, seq_ptr, 2);
        if (er == 0) xr = p_xfer_apdu(0, blk_buf, tx_len, rx_buf, &rx_len);
    } else {
        xr = p_xfer_apdu(0, raw, tx_len, rx_buf, &rx_len);
    }

    if (sem_locked) p_sem_sig(sem);
    pthread_mutex_unlock(&g_bcas_mtx);

    if (er != 0 || xr != 0 || rx_len == 0) {
        return snprintf(resp, rsz, "ERR enc=%d xfer=%d rxLen=%u\n", er, xr, rx_len);
    }

    int len = snprintf(resp, rsz, "OK ");
    for (int i = 0; i < rx_len && len + 4 < (int)rsz; i++) {
        len += snprintf(resp + len, rsz - (size_t)len, "%02x", rx_buf[i]);
    }
    len += snprintf(resp + len, rsz - (size_t)len, "\n");
    return len;
}

/* ---- PVR Download TS ポンピング & 127.0.0.1:40571 ストリーム送信 ---- */
static int pvr_send_chunk(int fd, const uint8_t *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

static void *pvr_stream_thread(void *arg) {
    (void)arg;
    int sfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (sfd < 0) return NULL;
    int one = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PVR_STREAM_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(sfd, 4) != 0) {
        close(sfd);
        return NULL;
    }

    while (g_running) {
        int nfd = accept(sfd, NULL, NULL);
        if (nfd >= 0) {
            if (g_pvr_cli_fd >= 0) close(g_pvr_cli_fd);
            g_pvr_cli_fd = nfd;
        }

        if (!g_pvr_active || !p_pvr_getbuf || !p_pvr_retbuf) {
            usleep(30000);
            continue;
        }

        pthread_mutex_lock(&g_pvr_mtx);
        if (!g_pvr_active) {
            pthread_mutex_unlock(&g_pvr_mtx);
            usleep(20000);
            continue;
        }
        uint32_t rp = 0, wp = 0, idxsz = 0;
        void *idxp = NULL;
        int rg = p_pvr_getbuf(g_pvr_dn_ch, &rp, &wp, &idxp, &idxsz);
        if (rg == 0 && rp != 0 && wp != 0 && rp != wp) {
            uint32_t end_addr = (g_pvr_base && g_pvr_size) ? (g_pvr_base + g_pvr_size) : 0;
            if (wp > rp) {
                size_t len = (size_t)(wp - rp);
                g_pvr_bytes += len;
                if (g_pvr_cli_fd >= 0) {
                    if (pvr_send_chunk(g_pvr_cli_fd, (const uint8_t *)(uintptr_t)rp, len) != 0) {
                        close(g_pvr_cli_fd);
                        g_pvr_cli_fd = -1;
                    }
                }
            } else if (end_addr > rp && wp >= g_pvr_base) {
                size_t len1 = (size_t)(end_addr - rp);
                size_t len2 = (size_t)(wp - g_pvr_base);
                g_pvr_bytes += (len1 + len2);
                if (g_pvr_cli_fd >= 0) {
                    if (len1 > 0 && pvr_send_chunk(g_pvr_cli_fd, (const uint8_t *)(uintptr_t)rp, len1) != 0) {
                        close(g_pvr_cli_fd);
                        g_pvr_cli_fd = -1;
                    } else if (len2 > 0 && pvr_send_chunk(g_pvr_cli_fd, (const uint8_t *)(uintptr_t)g_pvr_base, len2) != 0) {
                        close(g_pvr_cli_fd);
                        g_pvr_cli_fd = -1;
                    }
                }
            }
            p_pvr_retbuf(g_pvr_dn_ch, wp, wp);
        }
        pthread_mutex_unlock(&g_pvr_mtx);
        usleep(10000);
    }

    if (g_pvr_cli_fd >= 0) { close(g_pvr_cli_fd); g_pvr_cli_fd = -1; }
    close(sfd);
    return NULL;
}

static int cmd_pvr_start(uint32_t sdec_ch, uint32_t sdec_port, uint32_t dn_ch, char *resp, size_t rsz) {
    if (!g_pvr_syms_ok || !p_sdec_in) return snprintf(resp, rsz, "ERR pvr_syms_missing\n");

    pthread_mutex_lock(&g_pvr_mtx);
    if (g_pvr_active) {
        p_pvr_stop(g_pvr_dn_ch);
        p_pvr_close(g_pvr_dn_ch);
        p_sdec_rst(g_pvr_sdec_ch, 8);
        g_pvr_active = 0;
    }

    int rs = p_sdec_in((uint8_t)sdec_ch, (int)sdec_port, 0, 0);
    int rr = p_sdec_rst((uint8_t)sdec_ch, 8);
    /* SI/PSI 必須テーブルを先に登録 (PAT/CAT/NIT/SDT/EIT/TOT/BIT -> RECORD_A=8) */
    static const uint16_t init_pids[] = {0x0000, 0x0001, 0x0010, 0x0011, 0x0012, 0x0014, 0x0024};
    int pok = 0;
    for (size_t i = 0; i < sizeof(init_pids) / sizeof(init_pids[0]); i++) {
        if (p_sdec_pid((uint8_t)sdec_ch, 5 /* PSI */, init_pids[i], 8 /* RECORD_A */) == 0) pok++;
    }

    int re = p_pvr_setenc(0);
    int ro = p_pvr_open((uint8_t)dn_ch);
    int rt = p_pvr_start((uint8_t)dn_ch);
    uint32_t base = 0, sz = 0;
    int rb = p_pvr_binfo((uint8_t)dn_ch, &base, &sz);

    if (ro == 0 && rt == 0 && base != 0 && sz != 0) {
        g_pvr_dn_ch   = (uint8_t)dn_ch;
        g_pvr_sdec_ch = (uint8_t)sdec_ch;
        g_pvr_base    = base;
        g_pvr_size    = sz;
        g_pvr_bytes   = 0;
        g_pvr_active  = 1;
        pthread_mutex_unlock(&g_pvr_mtx);
        return snprintf(resp, rsz,
                        "OK sdec_in=%d rst=%d pids=%d enc0=%d open=%d start=%d binfo=%d base=0x%08x sz=0x%x\n",
                        rs, rr, pok, re, ro, rt, rb, base, sz);
    }

    if (rt == 0) p_pvr_stop((uint8_t)dn_ch);
    if (ro == 0) p_pvr_close((uint8_t)dn_ch);
    pthread_mutex_unlock(&g_pvr_mtx);
    return snprintf(resp, rsz,
                    "ERR pvr_start_failed sdec_in=%d rst=%d pids=%d enc0=%d open=%d start=%d binfo=%d base=0x%08x sz=0x%x\n",
                    rs, rr, pok, re, ro, rt, rb, base, sz);
}

static int cmd_pvr_stop(char *resp, size_t rsz) {
    if (!g_pvr_syms_ok) return snprintf(resp, rsz, "ERR pvr_syms_missing\n");
    pthread_mutex_lock(&g_pvr_mtx);
    if (g_pvr_active) {
        p_pvr_stop(g_pvr_dn_ch);
        p_pvr_close(g_pvr_dn_ch);
        p_sdec_rst(g_pvr_sdec_ch, 8);
        g_pvr_active = 0;
    }
    pthread_mutex_unlock(&g_pvr_mtx);
    return snprintf(resp, rsz, "OK pvr_stopped bytes=%llu\n", (unsigned long long)g_pvr_bytes);
}

static int handle_tune_cmd(int tuner, const char *args, char *resp, size_t rsz) {
    uint32_t media = 0, phy = 0, freq = 0, tsid = 0, lnb = 1, sr = 28860;
    const char *p = parse_u32(args, &media);
    if (p) p = parse_u32(p, &phy);
    if (p) p = parse_u32(p, &freq);
    if (p) p = parse_u32(p, &tsid);
    if (p && *p) {
        const char *p2 = parse_u32(p, &lnb);
        if (p2) {
            p = p2;
            if (*p) {
                const char *p3 = parse_u32(p, &sr);
                if (p3) p = p3;
            }
        }
    }
    if (!p || freq < 90000 || freq > 2200000) {
        return snprintf(resp, rsz, "ERR usage: TUNE%d <1|4|5> <phy> <freq_khz> <tsid> [lnb] [sr]\n", tuner);
    }
    return cmd_tune(tuner, media, phy, freq, tsid, lnb, sr, resp, rsz);
}

static void handle_client(int cfd) {
    char cmd[1024];
    char resp[4096];
    memset(cmd, 0, sizeof(cmd));
    ssize_t n = recv(cfd, cmd, sizeof(cmd) - 1, 0);
    if (n <= 0) { close(cfd); return; }
    while (n > 0 && (cmd[n - 1] == '\n' || cmd[n - 1] == '\r')) cmd[--n] = '\0';

    int rlen = 0;
    if (strcmp(cmd, "PING") == 0) {
        rlen = snprintf(resp, sizeof(resp), "OK pid=%d syms=%d pvr_syms=%d bcas_tbl=%p fe=%p\n",
                        getpid(), g_syms_ok, g_pvr_syms_ok, (void *)g_bcas_table, (void *)g_info_fe);
    } else if (strcmp(cmd, "QUIT") == 0) {
        pthread_mutex_lock(&g_pvr_mtx);
        if (g_pvr_active) {
            p_pvr_stop(g_pvr_dn_ch);
            p_pvr_close(g_pvr_dn_ch);
            p_sdec_rst(g_pvr_sdec_ch, 8);
            g_pvr_active = 0;
        }
        pthread_mutex_unlock(&g_pvr_mtx);
        rlen = snprintf(resp, sizeof(resp), "OK bye\n");
        g_running = 0;
    } else if (strcmp(cmd, "STATUS") == 0) {
        if (!g_syms_ok) {
            rlen = snprintf(resp, sizeof(resp), "ERR syms_not_ready\n");
        } else {
            rlen = snprintf(resp, sizeof(resp), "OK ");
            for (int t = 0; t < 2; t++) {
                int med = -1, lck = -1;
                p_hal_getmed(t, &med);
                p_dil_lock(t, &lck);
                const uint32_t *fe = (const uint32_t *)(g_info_fe + (uint32_t)t * GINFO_FE_STRIDE);
                rlen += snprintf(resp + rlen, sizeof(resp) - (size_t)rlen,
                                 "T%d(m=%d,l=%d,phy=%u,freq=%u,tsid=0x%04x,out=%u)%s",
                                 t, med, lck, fe[0x6c / 4], fe[0x70 / 4],
                                 (fe[0x58 / 4] >> 16) & 0xffffu, fe[0x2c / 4],
                                 (t == 0) ? " " : "\n");
            }
        }
    } else if (strncmp(cmd, "TUNE1 ", 6) == 0) {
        rlen = handle_tune_cmd(1, cmd + 6, resp, sizeof(resp));
    } else if (strncmp(cmd, "TUNE0 ", 6) == 0) {
        rlen = handle_tune_cmd(0, cmd + 6, resp, sizeof(resp));
    } else if (strncmp(cmd, "PVRSTART", 8) == 0) {
        uint32_t sch = 1, sport = 3, dn = 0;
        const char *p = cmd + 8;
        if (*p) {
            const char *p1 = parse_u32(p, &sch);
            if (p1) { p = p1; if (*p) { const char *p2 = parse_u32(p, &sport); if (p2) { p = p2; if (*p) parse_u32(p, &dn); } } }
        }
        rlen = cmd_pvr_start(sch, sport, dn, resp, sizeof(resp));
    } else if (strncmp(cmd, "PVRPID ", 7) == 0) {
        uint32_t sch = 1, ptype = 0, pid = 0, dest = 8;
        const char *p = parse_u32(cmd + 7, &sch);
        if (p) p = parse_u32(p, &ptype);
        if (p) p = parse_u32(p, &pid);
        if (p && *p) parse_u32(p, &dest);
        if (!p || !g_pvr_syms_ok) {
            rlen = snprintf(resp, sizeof(resp), "ERR pvrpid_usage\n");
        } else {
            int ret = p_sdec_pid((uint8_t)sch, (int)ptype, (uint16_t)pid, (int)dest);
            rlen = snprintf(resp, sizeof(resp), "OK pid=0x%04x ret=%d\n", pid, ret);
        }
    } else if (strcmp(cmd, "PVRSTOP") == 0) {
        rlen = cmd_pvr_stop(resp, sizeof(resp));
    } else if (strcmp(cmd, "PVRSTAT") == 0) {
        rlen = snprintf(resp, sizeof(resp), "OK active=%d dn=%u sdec=%u bytes=%llu\n",
                        g_pvr_active, g_pvr_dn_ch, g_pvr_sdec_ch, (unsigned long long)g_pvr_bytes);
    } else if (strncmp(cmd, "BCAS ", 5) == 0) {
        rlen = cmd_bcas(cmd + 5, resp, sizeof(resp));
    } else {
        rlen = snprintf(resp, sizeof(resp), "ERR unknown_cmd\n");
    }

    send(cfd, resp, (size_t)rlen, 0);
    close(cfd);
}

static void *server_thread(void *arg) {
    (void)arg;
    resolve_symbols();

    pthread_t th_pvr;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&th_pvr, &attr, pvr_stream_thread, NULL);
    pthread_attr_destroy(&attr);

    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) return NULL;
    int one = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(TUNER_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(sfd);
        return NULL;
    }
    if (listen(sfd, 8) != 0) {
        close(sfd);
        return NULL;
    }

    while (g_running) {
        int cfd = accept(sfd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        handle_client(cfd);
    }
    close(sfd);
    return NULL;
}

__attribute__((constructor)) static void on_load(void) {
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&th, &attr, server_thread, NULL);
    pthread_attr_destroy(&attr);
}
