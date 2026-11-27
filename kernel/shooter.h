/* PrismOS — 3D シューティングゲーム (レイキャスティング) の純粋ロジック。
 *
 * ハードウェアに触らないため、カーネルとホストテストで同じ
 * ソースを使う。浮動小数点は使わない (起動時の CPU は SSE も
 * x87 も有効化していないため、コンパイラが浮動小数点命令を
 * 出すと #UD で死ぬ)。位置と距離は Q16.16 固定小数点
 * (int32_t、1 マス = 65536)、角度は 0〜1023 (1 周 = 1024、
 * 1 単位 = 約 0.35 度) で表す。sin/cos は 1024 項目の
 * Q16.16 テーブルから読む (cos(a) = sin(a + 256))。
 *
 * マップは 16x16 の格子。'#' が壁、'.' が空き、'E' が敵の
 * 初期位置、'P' がプレイヤーの初期位置。敵はプレイヤーに
 * 近づき、近づくと攻撃する。弾は無限。撃つと中心のレイに
 * いちばん近い敵に当たる (ヒットスキャン)。
 */
#ifndef PRISM_SHOOTER_H
#define PRISM_SHOOTER_H

#include <stddef.h>
#include <stdint.h>

#define SHOOTER_MAP_W 16
#define SHOOTER_MAP_H 16
#define SHOOTER_FX_ONE 65536 /* Q16.16 の 1.0 */
#define SHOOTER_ANGLE_FULL 1024
#define SHOOTER_FOV_STEPS 188 /* 視野角 約 66 度 */
#define SHOOTER_MAX_ENEMIES 8
#define SHOOTER_PLAYER_HEALTH 100
#define SHOOTER_ENEMY_HEALTH 100
#define SHOOTER_SHOT_DAMAGE 34 /* 3 発で倒れる */
#define SHOOTER_SCORE_PER_KILL 100

/* 固定小数点の sin/cos。angle は 0〜1023 (範囲外は 1024 で割った余り)。 */
int32_t shooter_sin(int angle);
int32_t shooter_cos(int angle);

/* 整数の平方根 (64 ビット入力、切り捨て)。 */
uint32_t shooter_isqrt(uint64_t v);

/* マップのセル。範囲外は壁として扱う。 */
int shooter_is_wall(int cell_x, int cell_y);

/* レイキャスティング (DDA)。(px, py) から angle 方向にレイを
 * 飛ばし、壁までの垂直距離 (Q16.16) を返す。side には当たった
 * 壁の向きを入れる (0 = 南北の壁、1 = 東西の壁)。 */
int32_t shooter_ray(int32_t px, int32_t py, int angle, int *side);

/* 移動の衝突判定。angle 方向に dist (Q16.16) だけ進んだとき、
 * 壁にぶつからなければ 1 を返して (x, y) を進める。軸ごとに
 * 判定するため、壁に沿って滑る。 */
int shooter_try_move(int32_t *x, int32_t *y, int angle, int32_t dist);

typedef struct {
    int32_t x; /* Q16.16 */
    int32_t y;
    int health;
    int alive;
} shooter_enemy;

typedef struct {
    int32_t px; /* プレイヤー位置 Q16.16 */
    int32_t py;
    int angle; /* 0〜1023 */
    int health;
    int score;
    int kills;
    int phase; /* 0 = 未開始、1 = プレイ中、2 = 勝利、3 = 敗北 */
    shooter_enemy enemies[SHOOTER_MAX_ENEMIES];
    int enemy_count;
    int last_tick_second; /* 敵の AI を最後に動かした秒 */
} shooter_game;

/* 新しいゲームを作る (マップから敵とプレイヤーを置く)。 */
void shooter_new_game(shooter_game *g);

/* 残っている敵の数。 */
int shooter_enemies_left(const shooter_game *g);

/* 視線方向のベクトル (Q16.16) と敵の相対位置から、画面上の
 * 位置を求める。dot/cross を使い、atan2 は使わない。
 * dot > 0 かつ |cross|/dot が視野内なら 1 を返し、screen_x
 * (0.0〜1.0 を Q16.16 で表したもの) と距離 (Q16.16) を入れる。 */
int shooter_enemy_screen(const shooter_game *g, const shooter_enemy *e,
                         int32_t *screen_x, int32_t *dist);

/* 発射する。中心のレイ上 (許容角 約 2 度) にいる敵のうち、
 * いちばん近い敵に当てる。wall_dist は中心のレイの壁までの
 * 距離 (Q16.16)。当たったら当たった敵の番号 (0〜)、
 * 外れたら -1 を返す。敵の体力が 0 になったら倒れたものとして
 * 数え、score と kills を増やす。 */
int shooter_shoot(shooter_game *g, int32_t wall_dist);

/* 敵の AI を 1 秒分進める。敵はプレイヤーに近づき、0.8 マス
 * 以内に来たら攻撃して health を 10 減らす。プレイヤーが
 * 倒れたら phase = 3 にする。 */
void shooter_enemy_tick(shooter_game *g, int second);

/* プレイヤーを回転させる。steps は角度テーブル単位 (正で右回り)。 */
void shooter_turn(shooter_game *g, int steps);

#endif /* PRISM_SHOOTER_H */
