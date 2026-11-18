/**
 * arib_text.c: ARIB STD-B24 8bit 文字コード -> UTF-8 変換 (EPG の EIT 記述子用)
 *
 * ARIB STD-B24 第1編 第2部 第7章 に準拠:
 *   初期状態:
 *     G0 = 漢字 (JIS X 0208 + ARIB 追加漢字/記号, 2 バイト)
 *     G1 = 英数 (1 バイト)
 *     G2 = 平仮名 (1 バイト)
 *     G3 = 片仮名 / マクロ (1 バイト)
 *     GL = G0, GR = G2
 *   C0 (0x00-0x1F) および C1 (0x80-0x9F: MSZ 0x89 / NSZ 0x8A / COL 0x90 等) の
 *   制御符号とパラメータ長を正しく処理し、2 バイト漢字の境界ずれを防止する。
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "arib_text.h"
#include "arib_jis0208.h"

enum {
    SET_ASCII    = 0,
    SET_JIS0208  = 1,
    SET_KATAKANA = 2,
    SET_HIRAGANA = 3,
    SET_JIS0212  = 4,
    SET_DRCS1    = 5,
    SET_DRCS2    = 6,
    SET_MACRO    = 7,
};

/* ひらがな/カタカナ 0x77-0x7E の ARIB 拡張 (JIS X 0208 の 1 区の点) */
static const uint8_t HIRAGANA_EXT[8] = {
    0x35, 0x36, 0x3C, 0x23, 0x56, 0x57, 0x22, 0x26
};
static const uint8_t KATAKANA_EXT[8] = {
    0x33, 0x34, 0x3C, 0x23, 0x56, 0x57, 0x22, 0x26
};

/* ARIB 94区 (番組表の囲み文字シンボル: 点 1..37) */
static const char * const ARIB_ROW94_TABLE[38] = {
    NULL,
    "[HV]", "[SD]", "[P]", "[W]", "[MV]", "[手]", "[字]", "[双]",
    "[デ]", "[S]", "[二]", "[多]", "[解]", "[SS]", "[B]", "[N]",
    "■", "●", "[天]", "[交]", "[映]", "[無]", "[料]", "[年齢制限]",
    "[前]", "[後]", "[再]", "[新]", "[初]", "[終]", "[生]", "[販]",
    "[声]", "[吹]", "[PPV]", "(秘)", "[ほか]"
};

static int put_utf8(char *out, size_t sz, size_t *pos, unsigned cp) {
    if (cp == 0) return 0;
    if (*pos + 4 >= sz) return -1;
    if (cp < 0x80) {
        out[(*pos)++] = (char)cp;
    } else if (cp < 0x800) {
        out[(*pos)++] = (char)(0xC0u | (cp >> 6));
        out[(*pos)++] = (char)(0x80u | (cp & 0x3Fu));
    } else if (cp < 0x10000) {
        out[(*pos)++] = (char)(0xE0u | (cp >> 12));
        out[(*pos)++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[(*pos)++] = (char)(0x80u | (cp & 0x3Fu));
    } else {
        out[(*pos)++] = (char)(0xF0u | (cp >> 18));
        out[(*pos)++] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
        out[(*pos)++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[(*pos)++] = (char)(0x80u | (cp & 0x3Fu));
    }
    return 0;
}

static int put_str(char *out, size_t sz, size_t *pos, const char *s) {
    if (!s) return 0;
    size_t n = strlen(s);
    if (*pos + n >= sz) return -1;
    memcpy(out + *pos, s, n);
    *pos += n;
    return 0;
}

static unsigned map_jis0208(int b1, int b2) {
    int row = (b1 & 0x7F) - 0x21;
    int cell = (b2 & 0x7F) - 0x21;
    if (row < 0 || row > 93 || cell < 0 || cell > 93) return 0;
    return JIS0208_TO_UNICODE[row][cell];
}

/** ARIB 追加記号 (90区〜94区) を出力する。処理した場合は 1、通常の漢字テーブルを使う場合は 0 を返す */
static int emit_arib_ext_symbol(int b1, int b2, char *out, size_t sz, size_t *pos) {
    int ku  = (b1 & 0x7F) - 0x20; /* 1..94 */
    int ten = (b2 & 0x7F) - 0x20; /* 1..94 */
    if (ku < 90 || ku > 94 || ten < 1 || ten > 94) return 0;

    if (ku == 94) {
        if (ten >= 1 && ten <= 37 && ARIB_ROW94_TABLE[ten]) {
            put_str(out, sz, pos, ARIB_ROW94_TABLE[ten]);
        }
        return 1;
    }
    if (ku == 93) {
        if (ten >= 1 && ten <= 20) { /* ①..⑳ */
            put_utf8(out, sz, pos, 0x2460u + (unsigned)(ten - 1));
        } else if (ten >= 21 && ten <= 30) { /* ❶..❿ */
            put_utf8(out, sz, pos, 0x2776u + (unsigned)(ten - 21));
        } else if (ten >= 72 && ten <= 83) { /* Ⅰ..Ⅻ */
            put_utf8(out, sz, pos, 0x2160u + (unsigned)(ten - 72));
        } else if (ten == 84) {
            put_str(out, sz, pos, "!?");
        } else if (ten == 85) {
            put_str(out, sz, pos, "!!");
        }
        return 1;
    }
    if (ku == 92) {
        if (ten >= 1 && ten <= 20) { /* ⑴..⒇ */
            put_utf8(out, sz, pos, 0x2474u + (unsigned)(ten - 1));
        } else if (ten >= 21 && ten <= 32) { /* ⅰ..xii */
            put_utf8(out, sz, pos, 0x2170u + (unsigned)(ten - 21));
        }
        return 1;
    }
    if (ku == 90) {
        if (ten == 16) put_str(out, sz, pos, "!?");
        else if (ten == 17) put_str(out, sz, pos, "!!");
        else if (ten == 49) put_utf8(out, sz, pos, 0x3231u); /* ㈱ */
        else if (ten == 50) put_utf8(out, sz, pos, 0x3232u); /* ㈲ */
        else if (ten == 51) put_utf8(out, sz, pos, 0x3233u); /* ㈳ */
        else if (ten == 52) put_utf8(out, sz, pos, 0x3236u); /* ㈶ */
        else if (ten == 53) put_utf8(out, sz, pos, 0x3239u); /* ㈹ */
        else if (ten == 66) put_utf8(out, sz, pos, 0x26BEu); /* ⚾ */
        return 1;
    }
    return 0;
}

static unsigned map_hiragana(int v) {
    if (v >= 0x77 && v <= 0x7E) {
        return map_jis0208(0x21, HIRAGANA_EXT[v - 0x77]);
    }
    if (v >= 0x21 && v <= 0x76) return 0x3041u + (unsigned)(v - 0x21);
    return 0;
}

static unsigned map_katakana(int v) {
    if (v >= 0x77 && v <= 0x7E) {
        return map_jis0208(0x21, KATAKANA_EXT[v - 0x77]);
    }
    if (v >= 0x21 && v <= 0x76) return 0x30A1u + (unsigned)(v - 0x21);
    return 0;
}

static int map_1byte_gset(uint8_t f) {
    switch (f) {
        case 0x42: /* ASCII */
        case 0x4A: /* Alphanumeric (英数) */
        case 0x36: /* Prop. Alphanumeric */
            return SET_ASCII;
        case 0x30: /* Hiragana (平仮名) */
        case 0x37: /* Prop. Hiragana */
            return SET_HIRAGANA;
        case 0x31: /* Katakana (片仮名) */
        case 0x38: /* Prop. Katakana */
        case 0x49: /* JIS X 0201 Katakana */
            return SET_KATAKANA;
        default:
            return SET_DRCS1;
    }
}

size_t arib_to_utf8(const uint8_t *in, size_t len, char *out, size_t out_sz) {
    if (out_sz == 0) return 0;
    size_t pos = 0;
    int g[4] = { SET_JIS0208, SET_ASCII, SET_HIRAGANA, SET_KATAKANA };
    int gl = 0;   /* GL (0x21-0x7E) が参照するバッファ (初期値 G0 = 漢字) */
    int gr = 2;   /* GR (0xA1-0xFE) が参照するバッファ (初期値 G2 = 平仮名) */
    int ss = -1;
    out[0] = '\0';

    for (size_t i = 0; i < len; i++) {
        uint8_t b = in[i];

        /* ESC シーケンス (G 集合の指示・呼び出し) */
        if (b == 0x1B) {
            if (i + 1 >= len) break;
            uint8_t n1 = in[i + 1];

            /* 2 バイト G 集合の指示: ESC $ ... */
            if (n1 == 0x24) {
                if (i + 2 >= len) break;
                uint8_t n2 = in[i + 2];
                if (n2 >= 0x28 && n2 <= 0x2B) {
                    int target = n2 - 0x28;
                    if (i + 3 >= len) break;
                    uint8_t n3 = in[i + 3];
                    if (n3 == 0x20) { /* ESC $ ( 0x20 <F>: 2バイト DRCS */
                        g[target] = SET_DRCS2;
                        i += 4;
                    } else if (n3 == 0x44) {
                        g[target] = SET_JIS0212;
                        i += 3;
                    } else {
                        g[target] = SET_JIS0208;
                        i += 3;
                    }
                    continue;
                }
                /* ESC $ <F>: G0 に 2 バイト集合を指示 (0x42, 0x40, 0x39, 0x3A, 0x3B) */
                g[0] = (n2 == 0x44) ? SET_JIS0212 : SET_JIS0208;
                i += 2;
                continue;
            }

            /* 1 バイト G 集合の指示: ESC (28..2B) ... */
            if (n1 >= 0x28 && n1 <= 0x2B) {
                int target = n1 - 0x28;
                if (i + 2 >= len) break;
                uint8_t n2 = in[i + 2];
                if (n2 == 0x20) { /* ESC ( 0x20 <F>: 1バイト DRCS または マクロ (0x70) */
                    if (i + 3 >= len) break;
                    uint8_t n3 = in[i + 3];
                    if (n3 == 0x70) {
                        g[target] = SET_MACRO;
                    } else {
                        g[target] = SET_DRCS1;
                    }
                    i += 3;
                } else {
                    g[target] = map_1byte_gset(n2);
                    i += 2;
                }
                continue;
            }

            if (n1 == 0x6E) { gl = 2; i += 1; continue; } /* LS2 */
            if (n1 == 0x6F) { gl = 3; i += 1; continue; } /* LS3 */
            if (n1 == 0x7E) { gr = 1; i += 1; continue; } /* LS1R */
            if (n1 == 0x7D) { gr = 2; i += 1; continue; } /* LS2R */
            if (n1 == 0x7C) { gr = 3; i += 1; continue; } /* LS3R */
            i += 1;
            continue;
        }

        /* C0 制御文字 (0x00-0x1F) */
        if (b < 0x20) {
            if (b == 0x0A || b == 0x0D) {
                if (pos + 1 < out_sz) out[pos++] = '\n';
            } else if (b == 0x0E) {
                gl = 1; /* LS1 */
            } else if (b == 0x0F) {
                gl = 0; /* LS0 */
            } else if (b == 0x16) {
                i += 1; /* PAPF (1バイトパラメータ) */
            } else if (b == 0x19) {
                ss = 2; /* SS2 */
            } else if (b == 0x1C) {
                i += 2; /* APS (2バイトパラメータ) */
            } else if (b == 0x1D) {
                ss = 3; /* SS3 */
            }
            continue;
        }

        if (b == 0x20) {
            if (pos + 1 < out_sz) out[pos++] = ' ';
            ss = -1;
            continue;
        }
        if (b == 0x7F || b == 0xA0 || b == 0xFF) {
            ss = -1;
            continue;
        }

        /* C1 制御文字 (0x80-0x9F: ARIB STD-B24 第7章 表7-1) */
        if (b >= 0x80 && b <= 0x9F) {
            if (b == 0x8E) {
                ss = 2; /* SS2 */
            } else if (b == 0x8F) {
                ss = 3; /* SS3 */
            } else if (b == 0x8B) {
                i += 1; /* SZX (1バイトパラメータ) */
            } else if (b == 0x90 || b == 0x92) {
                /* COL / CDC: 0x20 が続く場合は 2 バイト、それ以外は 1 バイト */
                if (i + 1 < len && in[i + 1] == 0x20) i += 2;
                else i += 1;
            } else if (b == 0x91 || b == 0x93 || b == 0x94 || b == 0x95 || b == 0x97 || b == 0x98) {
                i += 1; /* FLC / POL / WMM / MACRO / HLC / RPC */
            } else if (b == 0x9B) {
                /* CSI: パラメータ (0x20-0x3F) + 終端文字 (0x40-0x7E) までスキップ */
                while (i + 1 < len) {
                    uint8_t csi_b = in[++i];
                    if (csi_b >= 0x40 && csi_b <= 0x7E) break;
                }
            } else if (b == 0x9D) {
                i += 2; /* TIME (0x20 + パラメータ) */
            }
            /* 0x88 (SSZ), 0x89 (MSZ), 0x8A (NSZ) 等はパラメータ無しの 1 バイト制御符号 */
            continue;
        }

        int high = (b >= 0xA1);
        int gidx = (ss >= 0) ? ss : (high ? gr : gl);
        ss = -1;
        int set = g[gidx];
        int v = b & 0x7F;

        /* ARIB デフォルトマクロ (G3 = 0x70、SS3 経由などで 0x60..0x6F が呼ばれる場合) */
        if (set == SET_MACRO || (gidx == 3 && set == SET_KATAKANA && v >= 0x60 && v <= 0x6F && !high)) {
            if (v == 0x60) { g[0] = SET_JIS0208; g[1] = SET_ASCII; g[2] = SET_HIRAGANA; g[3] = SET_MACRO; gl = 0; gr = 2; }
            else if (v == 0x61) { g[0] = SET_JIS0208; g[1] = SET_KATAKANA; g[2] = SET_HIRAGANA; g[3] = SET_MACRO; gl = 0; gr = 2; }
            continue;
        }

        if (set == SET_DRCS2 || set == SET_JIS0212) {
            if (i + 1 >= len) break;
            i++; /* 2 バイト分消費してスキップ */
            continue;
        }
        if (set == SET_DRCS1) {
            continue;
        }

        unsigned cp = 0;
        if (set == SET_JIS0208) {
            if (i + 1 >= len) break;
            uint8_t b2 = in[++i];
            if (emit_arib_ext_symbol(b, b2, out, out_sz, &pos)) {
                continue;
            }
            cp = map_jis0208(b, b2);
        } else if (set == SET_HIRAGANA) {
            cp = map_hiragana(v);
        } else if (set == SET_KATAKANA) {
            cp = map_katakana(v);
        } else {
            if (v >= 0x20 && v <= 0x7E) cp = (unsigned)v;
        }

        if (cp != 0 && put_utf8(out, out_sz, &pos, cp) != 0) break;
    }

    out[pos < out_sz ? pos : out_sz - 1] = '\0';
    return pos;
}
