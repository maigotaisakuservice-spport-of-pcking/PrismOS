/* PrismOS — トランプ (ブラックジャック) の純粋ロジック。
 *
 * ハードウェアに触らないため、カーネルとホストテストで同じ
 * ソースを使う。カードは 0〜51 の整数で、rank = card % 13 + 1
 * (1 = A、11 = J、12 = Q、13 = K)、suit = card / 13
 * (0 = スペード、1 = ハート、2 = ダイヤ、3 = クラブ)。
 * シャッフルの乱数はゲーム用の xorshift で、暗号には使わない。
 */
#ifndef PRISM_CARDS_H
#define PRISM_CARDS_H

#include <stddef.h>
#include <stdint.h>

#define PRISM_DECK_SIZE 52

/* デッキを 0〜51 の順に並べる。 */
void cards_new_deck(int deck[PRISM_DECK_SIZE]);

/* Fisher-Yates でシャッフルする。seed が 0 のときは 1 にする。 */
void cards_shuffle(int deck[PRISM_DECK_SIZE], uint32_t seed);

/* カードのランク (1〜13) とスート (0〜3)。 */
int cards_rank(int card);
int cards_suit(int card);

/* 手札の合計。A は 11 として数え、21 を超えたら 1 として
 * 数え直す (A が複数ある場合も 1 枚ずつ下げる)。 */
int cards_hand_value(const int *hand, int n);

/* 2 枚で 21 (ブラックジャック) か。 */
int cards_is_blackjack(const int *hand, int n);

/* ディーラーは 16 以下で引き、17 以上で止まる。 */
int cards_dealer_hits(int value);

/* カードを "AS" "10H" "KD" の形で buf に書く (buf は 4 バイト
 * 以上)。戻り値は書いた文字数 (NUL を除く)。 */
int cards_format(int card, char *buf, size_t cap);

/* ---- ポーカー (5 カードドロー) ---- */
/* 5 枚の役を評価する。整数の大小がそのまま強さになる
 * (カテゴリが高位ビット、タイブレークが続く)。 */
int poker_eval(const int *hand);
/* 評価値からカテゴリ名 ("Pair" 等) を返す。 */
const char *poker_category(int score);
/* CPU の捨て札ヒューリスティクス。discard[5] に
 * 1 = 捨てる を入れ、捨てる枚数を返す。 */
int poker_cpu_discards(const int *hand, int discard[5]);

/* ---- バカラ ---- */
int baccarat_card_value(int card);
int baccarat_value(const int *hand, int n);
/* 2 枚の合計 (mod 10 の前) でプレイヤーが引くか。 */
int baccarat_player_draws(int two_card_total);
/* バンカーが引くか。p3 はプレイヤーの 3 枚目の値、
 * プレイヤーが止まったときは -1。 */
int baccarat_banker_draws(int banker_total, int p3);

/* ---- 7並べ ---- */
/* lo/hi はスートごとの配置済み最小・最大ランク
 * (0 = 未配置)。出せるカードかどうかと、配置の更新。 */
int sevens_playable(const int lo[4], const int hi[4], int card);
void sevens_place(int lo[4], int hi[4], int card);

/* ---- ババ抜き ---- */
/* 同じランクのペアを取り除く。52 はジョーカー (ペアに
 * ならない)。新しい枚数を返す。 */
int oldmaid_discard_pairs(int *hand, int n);

#endif /* PRISM_CARDS_H */
