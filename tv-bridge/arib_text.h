/**
 * arib_text.h: ARIB STD-B24 8bit 文字コード -> UTF-8 変換
 */
#ifndef ARIB_TEXT_H
#define ARIB_TEXT_H

#include <stddef.h>
#include <stdint.h>

/**
 * ARIB 8bit 文字列を UTF-8 に変換する。
 * @param in     変換元 (NUL 終端されていない)
 * @param len    変換元の長さ
 * @param out    出力バッファ (常に NUL 終端される)
 * @param out_sz 出力バッファサイズ
 * @return 書き込んだバイト数 (NUL を除く)
 */
size_t arib_to_utf8(const uint8_t *in, size_t len, char *out, size_t out_sz);

#endif
