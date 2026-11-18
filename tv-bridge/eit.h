/**
 * eit.h: EIT (ARIB STD-B10) セクションから番組表を収集するモジュール
 *
 * - PID 0x12-0x26 の EIT[p/f] (0x4E/0x4F) および EIT[schedule] (0x50-0x6F) を解析
 * - MPEG-2 PSI CRC-32 検証と TS continuity_counter 追跡により破損セクションを完全排除
 * - event_group_descriptor (0xD6) を解析し Mirakurun 互換の relatedItems を出力・共有サブチャンネル重複を除外
 */
#ifndef EIT_H
#define EIT_H

#include <stddef.h>
#include <stdint.h>

/** 初期化 (起動時に 1 回) */
void eit_init(void);

/** 有効なサービス一覧をクリア・登録する (データ放送・臨時チャンネル等の不要イベント排除および親子判定用) */
void eit_clear_services(void);
void eit_register_service(uint16_t onid, uint16_t tsid, uint16_t sid, uint16_t parent_sid);

/** トランスポンダ切替時にセクション組立バッファをリセットする */
void eit_set_mux(uint16_t onid, uint16_t tsid);

/** 188 バイト単位の TS パケット列を投入する */
void eit_feed_ts(const uint8_t *ts, size_t npkts);

/** 保持している有効番組数を返す */
int eit_program_count(void);

/** EIT 由来の有効番組数を返す (LG DB フォールバック分は含まない) */
int eit_native_count(void);

/**
 * LG 純正 EPG DB の番組をフォールバックとして登録する。
 * 同じ (ONID, SID, EID) の EIT 由来データが既にある場合は上書きしない。
 */
void eit_ingest_fallback(uint16_t onid, uint16_t sid, uint16_t eid,
                         int64_t start, uint32_t duration,
                         const char *name, const char *description,
                         int genre1, int genre2);

/** Mirakurun 互換の /api/programs 配列 JSON を生成する (要 free)。filter_sid 0 で全件 */
char *eit_build_programs_json(uint64_t filter_sid);

/** 単一番組の JSON を生成する (見つからなければ NULL、要 free) */
char *eit_build_program_json(uint64_t program_id);

#endif
