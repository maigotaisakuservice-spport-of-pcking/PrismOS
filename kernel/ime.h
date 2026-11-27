/* PrismOS — 日本語入力 (IME)。
 *
 * ローマ字→ひらがな変換と、コンパクトな漢字辞書に
 * よる候補変換を行う状態機械。変換・候補選択・確定の
 * 基本フローを実装する (文節分割や予測変換はしない:
 * 未変換の読み全体を 1 単位として変換する)。
 *
 * 辞書は基本語彙を手で選んだ自前の表で、外部データの
 * 流用はない。ローマ字表はヘボン式を基本に訓令式の
 * 一部 (si/ti/tu/hu/zi) も受ける。
 *
 * 使い方: ime_feed に 1 文字ずつ渡す。確定した文字列が
 * あれば戻り値 1 で out に入る。変換中の文字列は
 * ime_preedit / ime_candidate で表示用に取る。
 */
#ifndef PRISM_IME_H
#define PRISM_IME_H

#include <stddef.h>

/* 状態を初期化する (IME オフ)。 */
void ime_reset(void);
/* IME のオン/オフを切り替える。新しい状態を返す。 */
int ime_toggle(void);
int ime_on(void);
/* 未変換の入力があるか (表示中の文字列があるか)。 */
int ime_active(void);
/* 表示すべき文字列 (未変換なら読みのかな、変換済み
 * なら現在の候補)。 */
const char *ime_display(void);
/* 候補の数と現在の候補番号 (変換前は候補数 0)。 */
int ime_candidate_count(void);
int ime_candidate_index(void);
/* i 番目の候補文字列 (変換中のみ、範囲外は NULL)。 */
const char *ime_candidate_at(int i);
/* 候補を番号で選ぶ。成功したら 1。 */
int ime_select_candidate(int i);
/* 候補を前後に動かす (端で回る)。成功したら 1。 */
int ime_move_candidate(int delta);

/* 1 文字入力する。c は印字可能 ASCII、'\\b' (後退)、
 * ' ' (変換/次候補)、'\\n' (確定)、0x1B (取消)。
 * 確定文字列ができたら 1 を返し out に書く (NUL 終端、
 * cap バイトまで)。それ以外は 0。 */
int ime_feed(int c, char *out, size_t cap);

/* ローマ字列をひらがなへ変換する純粋関数 (テスト用)。
 * 変換しきれない末尾はローマ字のまま残す。 */
void ime_romaji_to_kana(const char *romaji, char *out,
                        size_t cap);
/* 読み (ひらがな) の候補を探す。見つかれば候補数を
 * 返し、candidates に候補文字列へのポインタを入れる
 * (最大 max 個)。見つからなければ 0。 */
int ime_lookup(const char *kana, const char **candidates,
               int max);

#endif
