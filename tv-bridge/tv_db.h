/**
 * tv_db.h: LG webOS 純正 DB8 (チャンネル一覧) および EPGDB (番組表) の取得・変換モジュール
 */
#ifndef WEBOS_TV_DB_H
#define WEBOS_TV_DB_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    int      media;            /* 1=GR(地デジ), 4=BS, 5=CS */
    int      space;            /* BonDriver用スペース番号: 0=GR, 1=BS, 2=CS */
    char     type_str[8];      /* "GR", "BS", "CS" */
    uint32_t phy;
    uint32_t freq_khz;
    uint16_t tsid;
    uint16_t onid;
    uint16_t sid;
    uint16_t service_type;
    int      remote_key;       /* リモコンキーID (GR: 1..12, その他: 0) */
    uint8_t  is_sub;           /* サブチャンネル判定 (1=サブ, 0=メイン) */
    uint8_t  skipped;
    uint8_t  invisible;
    char     ch_num[16];
    char     name[128];
    char     sig_id[48];       /* "<TSID>_<SID>_<ONID>" (epgdb紐付用) */
    char     logo_path[256];   /* TV内蔵受信済み ARIB 局ロゴ PNG パス (/mnt/lg/user/jlogo/...) */
    uint8_t  has_logo;         /* 1=ロゴPNGあり, 0=なし */
    uint64_t mirakurun_sid;    /* (uint64_t)onid * 100000ULL + sid */
} tv_channel_t;

typedef struct {
    tv_channel_t *items;
    int           count;
} tv_channel_list_t;

/** luna-send 経由で TV 本体のスキャン済みチャンネル一覧を読み込む */
int tv_db_load_channels(tv_channel_list_t *list);
void tv_db_free_channels(tv_channel_list_t *list);

/** 指定条件からチャンネルを検索する（見つからない場合は NULL） */
const tv_channel_t *tv_db_find_by_mirakurun(const tv_channel_list_t *list, const char *type_str, const char *ch_spec);
const tv_channel_t *tv_db_find_by_sid(const tv_channel_list_t *list, uint64_t mirakurun_sid);

/** Mirakurun 互換の /api/channels および /api/services JSON 文字列を生成する（要 free） */
char *tv_db_build_channels_json(const tv_channel_list_t *list);
char *tv_db_build_services_json(const tv_channel_list_t *list);
char *tv_db_build_service_json_one(const tv_channel_t *c);

/**
 * TV 純正 EPG DB (com.webos.epgdb) + EIT から番組表を取得し、Mirakurun /api/programs JSON を生成する。
 * filter_sid != 0 の場合は該当サービス (16bit serviceId または Mirakurun ServiceItemId) のみ返す（要 free）。
 */
char *tv_db_get_programs_json(uint64_t filter_sid, int force_refresh);

/** 指定 ProgramId の単一 Program JSON を返す（見つからない場合は NULL、要 free）。 */
char *tv_db_get_program_by_id_json(uint64_t program_id);

/** EPG DB を再スキャンしてキャッシュを更新し、新規/変更番組数を返す（差分はイベントキューへ積まれる） */
int tv_db_poll_epg_changes(void);

/** EIT 収集済みの番組表から前回との差分 (create/update/remove) を検出してイベントキューへ積む */
int tv_db_sync_eit_events(void);

/** 現在のイベントシーケンス番号を取得する */
uint64_t tv_db_get_event_seq(void);

/** 直近の EPG DB 応答 (1 ページ目の生テキスト) を取得する (デバッグ用) */
const char *tv_db_get_epg_raw_sample(void);

/**
 * cursor_seq_io 以降の未読イベント JSON 群を取り出す（無ければ NULL、要 free）。
 * already_sent_io が 0 でない場合は先頭に ',' を付与し、出力後に 1 へ更新する。
 */
char *tv_db_get_events_since(uint64_t *cursor_seq_io, int *already_sent_io);

#endif
