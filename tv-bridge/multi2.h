/**
 * multi2.h: ARIB STD-B25 (MULTI2) ストリーム復号エンジン（マルチパケット PSI/ECM 再構成対応）
 */
#ifndef WEBOS_MULTI2_H
#define WEBOS_MULTI2_H

#include <stdint.h>
#include <stddef.h>

/** B-CAS APDU コールバック型: 成功時 0 を返し、rx_len に受信長を格納する */
typedef int (*b25_bcas_cb_t)(void *user, const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t *rx_len);

/** PAT/CAT/PMT から新規 PID が見つかった時の通知コールバック (pid_type: 0=Video, 1=Audio, 2=PCR, 4=Subt, 5=PSI) */
typedef void (*b25_pid_cb_t)(void *user, uint16_t pid, int pid_type);

#define B25_MAX_ECMS     16
#define B25_MAX_SEC_ASM  32
#define B25_MAX_SERVICES 32
#define B25_MAX_SVC_ES   24

typedef struct {
    uint16_t pid;
    uint8_t  collecting;
    uint8_t  buf[4098];
    size_t   len;
    size_t   expected;
} b25_sec_asm_t;

typedef struct {
    uint16_t sid;
    uint16_t pmt_pid;
    uint16_t pcr_pid;
    uint16_t ecm_pid;
    uint16_t es_pids[B25_MAX_SVC_ES];
    int      num_es;
    uint8_t  has_pmt;
} b25_service_t;

typedef struct {
    uint32_t sk[8];
    uint32_t iv_l, iv_r;
    int      has_syskey;

    struct {
        uint16_t pid;
        uint8_t  last_body[64];
        size_t   last_body_len;
        uint32_t wk_odd[8];
        uint32_t wk_even[8];
        int      valid;
    } ecms[B25_MAX_ECMS];
    int num_ecms;

    b25_sec_asm_t sec_asms[B25_MAX_SEC_ASM];
    int           num_asms;

    uint16_t      pat_tsid;
    uint8_t       pat_ver;
    uint16_t      nit_pid;
    b25_service_t services[B25_MAX_SERVICES];
    int           num_services;

    uint8_t  pid_to_ecm[8192];
    uint8_t  is_pmt[8192];
    uint8_t  is_ecm[8192];
    uint8_t  pmt_ver[8192];

    b25_bcas_cb_t bcas_cb;
    void         *bcas_user;

    b25_pid_cb_t  pid_cb;
    void         *pid_user;
} b25_ctx_t;

void b25_init(b25_ctx_t *ctx, b25_bcas_cb_t cb, void *user);
void b25_set_pid_cb(b25_ctx_t *ctx, b25_pid_cb_t cb, void *user);
void b25_set_syskey(b25_ctx_t *ctx, const uint8_t syskey[32], const uint8_t iv[8]);
void b25_reset_stream(b25_ctx_t *ctx);

/** 188B TS パケット 1 つ（または複数）をその場で復号する */
void b25_process_packets(b25_ctx_t *ctx, uint8_t *ts_buf, size_t num_packets);

/** 暗号解除を行わず、PAT/CAT/PMT 解析および PID 通知のみを実行する */
void b25_scan_sections(b25_ctx_t *ctx, const uint8_t *ts_buf, size_t num_packets);

/**
 * 指定された serviceId (sid) に属する TS パケットと共通 PSI/SI のみを抽出し、
 * PAT を該当 1 サービスのみの構成に書き換えて out_ts へ格納する。
 * 戻り値: out_ts に書き出したパケット数 (0..num_packets)
 */
size_t b25_filter_service_packets(const b25_ctx_t *ctx, const uint8_t *in_ts, size_t num_packets,
                                  uint16_t sid, uint8_t *pat_cc_io, uint8_t *out_ts);

#endif
