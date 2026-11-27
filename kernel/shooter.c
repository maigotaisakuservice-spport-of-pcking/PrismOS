/* PrismOS — 3D シューティングゲームの純粋ロジック。
 *
 * 固定小数点 (Q16.16) と整数演算だけを使う。浮動小数点は
 * カーネルでは使えない (SSE/x87 未有効化のため #UD)。
 */
#include "shooter.h"
#include "shooter_sin.h"

/* マップ: '#' = 壁、'.' = 空き、'E' = 敵、'P' = プレイヤー。
 * プレイヤーは (1.5, 1.5) から東を向いて始まり、最初の敵は
 * 同じ通路の正面 (4.5, 1.5) にいる。 */
static const char SHOOTER_MAP[SHOOTER_MAP_H][SHOOTER_MAP_W + 1] = {
    "################",
    "#P..E..........#",
    "#..............#",
    "#.####.#####...#",
    "#..............#",
    "#..E....#..E...#",
    "#.......#......#",
    "#.#####.#.###..#",
    "#..............#",
    "#..E.....E.....#",
    "#..............#",
    "#.####.#####...#",
    "#..............#",
    "#..E...........#",
    "#..............#",
    "################",
};

/* 視野角の半分のタンジェント (tan 33 度 = 0.6494) を Q16.16 で。 */
#define SHOOTER_TAN_HALF_FOV 42560
/* ヒット判定の許容角 (tan 2 度 = 0.0349) を千分率で。 */
#define SHOOTER_HIT_TAN_MILLI 35

int32_t shooter_sin(int angle) {
    return (int32_t)shooter_sin_table[angle & 1023];
}

int32_t shooter_cos(int angle) {
    return (int32_t)shooter_sin_table[(angle + 256) & 1023];
}

uint32_t shooter_isqrt(uint64_t v) {
    /* ニュートン法。64 ビットの二分探索より速く、収束する。 */
    if (v == 0) {
        return 0;
    }
    uint64_t x = v;
    uint64_t y = (x + 1) >> 1;
    while (y < x) {
        x = y;
        y = (x + v / x) >> 1;
    }
    return (uint32_t)x;
}

int shooter_is_wall(int cell_x, int cell_y) {
    if (cell_x < 0 || cell_x >= SHOOTER_MAP_W || cell_y < 0 ||
        cell_y >= SHOOTER_MAP_H) {
        return 1;
    }
    return SHOOTER_MAP[cell_y][cell_x] == '#';
}

int32_t shooter_ray(int32_t px, int32_t py, int angle, int *side) {
    /* 方向ベクトル (Q16.16、テーブルから直接)。 */
    int32_t dir_x = shooter_cos(angle);
    int32_t dir_y = shooter_sin(angle);
    /* 1 マス進むのにかかるレイの長さ (Q16.16)。方向が 0 の
     * 軸はとても大きい値にして、実質その軸では進まない。 */
    int32_t delta_x = (dir_x == 0) ? (int32_t)(1 << 30)
                                   : (int32_t)((int64_t)(1LL << 32) /
                                               (dir_x < 0 ? -dir_x : dir_x));
    int32_t delta_y = (dir_y == 0) ? (int32_t)(1 << 30)
                                   : (int32_t)((int64_t)(1LL << 32) /
                                               (dir_y < 0 ? -dir_y : dir_y));
    int map_x = (int)(px >> 16);
    int map_y = (int)(py >> 16);
    int step_x;
    int step_y;
    int32_t side_x;
    int32_t side_y;
    if (dir_x < 0) {
        step_x = -1;
        side_x = (int32_t)(((int64_t)(px - map_x * SHOOTER_FX_ONE) *
                            delta_x) >>
                           16);
    } else {
        step_x = 1;
        side_x = (int32_t)(((int64_t)((map_x + 1) * SHOOTER_FX_ONE - px) *
                            delta_x) >>
                           16);
    }
    if (dir_y < 0) {
        step_y = -1;
        side_y = (int32_t)(((int64_t)(py - map_y * SHOOTER_FX_ONE) *
                            delta_y) >>
                           16);
    } else {
        step_y = 1;
        side_y = (int32_t)(((int64_t)((map_y + 1) * SHOOTER_FX_ONE - py) *
                            delta_y) >>
                           16);
    }
    int hit_side = 0;
    for (int i = 0; i < 64; i++) {
        if (side_x < side_y) {
            side_x += delta_x;
            map_x += step_x;
            hit_side = 0;
        } else {
            side_y += delta_y;
            map_y += step_y;
            hit_side = 1;
        }
        if (shooter_is_wall(map_x, map_y)) {
            break;
        }
    }
    if (side != NULL) {
        *side = hit_side;
    }
    /* 壁までの垂直距離 = 最後に進んだ軸の距離から 1 マス分引く。 */
    if (hit_side == 0) {
        return side_x - delta_x;
    }
    return side_y - delta_y;
}

static int shooter_can_stand(int32_t x, int32_t y) {
    return !shooter_is_wall((int)(x >> 16), (int)(y >> 16));
}

int shooter_try_move(int32_t *x, int32_t *y, int angle, int32_t dist) {
    int32_t dx = (int32_t)(((int64_t)dist * shooter_cos(angle)) >> 16);
    int32_t dy = (int32_t)(((int64_t)dist * shooter_sin(angle)) >> 16);
    int moved = 0;
    int32_t nx = *x + dx;
    if (nx != *x && shooter_can_stand(nx, *y)) {
        *x = nx;
        moved = 1;
    }
    int32_t ny = *y + dy;
    if (ny != *y && shooter_can_stand(*x, ny)) {
        *y = ny;
        moved = 1;
    }
    return moved;
}

void shooter_new_game(shooter_game *g) {
    if (g == NULL) {
        return;
    }
    g->px = 0;
    g->py = 0;
    g->angle = 0;
    g->health = SHOOTER_PLAYER_HEALTH;
    g->score = 0;
    g->kills = 0;
    g->phase = 1;
    g->enemy_count = 0;
    g->last_tick_second = -1;
    for (int i = 0; i < SHOOTER_MAX_ENEMIES; i++) {
        g->enemies[i].x = 0;
        g->enemies[i].y = 0;
        g->enemies[i].health = 0;
        g->enemies[i].alive = 0;
    }
    for (int row = 0; row < SHOOTER_MAP_H; row++) {
        for (int col = 0; col < SHOOTER_MAP_W; col++) {
            char c = SHOOTER_MAP[row][col];
            if (c == 'P') {
                g->px = (int32_t)(col * SHOOTER_FX_ONE + SHOOTER_FX_ONE / 2);
                g->py = (int32_t)(row * SHOOTER_FX_ONE + SHOOTER_FX_ONE / 2);
                g->angle = 0; /* 東向き */
            } else if (c == 'E' &&
                       g->enemy_count < SHOOTER_MAX_ENEMIES) {
                shooter_enemy *e = &g->enemies[g->enemy_count++];
                e->x = (int32_t)(col * SHOOTER_FX_ONE + SHOOTER_FX_ONE / 2);
                e->y = (int32_t)(row * SHOOTER_FX_ONE + SHOOTER_FX_ONE / 2);
                e->health = SHOOTER_ENEMY_HEALTH;
                e->alive = 1;
            }
        }
    }
}

int shooter_enemies_left(const shooter_game *g) {
    if (g == NULL) {
        return 0;
    }
    int n = 0;
    for (int i = 0; i < g->enemy_count; i++) {
        if (g->enemies[i].alive) {
            n++;
        }
    }
    return n;
}

int shooter_enemy_screen(const shooter_game *g, const shooter_enemy *e,
                         int32_t *screen_x, int32_t *dist) {
    if (g == NULL || e == NULL || !e->alive) {
        return 0;
    }
    int32_t rel_x = e->x - g->px;
    int32_t rel_y = e->y - g->py;
    int32_t vx = shooter_cos(g->angle); /* Q16.16 */
    int32_t vy = shooter_sin(g->angle);
    /* 内積と外積 (Q16.16)。 */
    int32_t dot = (int32_t)(((int64_t)rel_x * vx + (int64_t)rel_y * vy) >> 16);
    int32_t cross = (int32_t)(((int64_t)rel_x * vy - (int64_t)rel_y * vx) >> 16);
    if (dot <= 0) {
        return 0; /* 背後にいる */
    }
    /* 画面上の位置: 0.5 + (cross/dot) / tan(半視野角) * 0.5。 */
    int32_t ratio = (int32_t)(((int64_t)cross << 16) / dot);
    int32_t sx = SHOOTER_FX_ONE / 2 +
                 (int32_t)(((int64_t)ratio * (SHOOTER_FX_ONE / 2)) /
                           SHOOTER_TAN_HALF_FOV);
    if (sx < 0 || sx > SHOOTER_FX_ONE) {
        return 0;
    }
    if (screen_x != NULL) {
        *screen_x = sx;
    }
    if (dist != NULL) {
        uint64_t d2 = (uint64_t)((int64_t)rel_x * rel_x +
                                 (int64_t)rel_y * rel_y);
        *dist = (int32_t)shooter_isqrt(d2);
    }
    return 1;
}

int shooter_shoot(shooter_game *g, int32_t wall_dist) {
    if (g == NULL || g->phase != 1) {
        return -1;
    }
    int best = -1;
    int32_t best_dist = 0;
    for (int i = 0; i < g->enemy_count; i++) {
        shooter_enemy *e = &g->enemies[i];
        if (!e->alive) {
            continue;
        }
        int32_t rel_x = e->x - g->px;
        int32_t rel_y = e->y - g->py;
        int32_t vx = shooter_cos(g->angle);
        int32_t vy = shooter_sin(g->angle);
        int32_t dot =
            (int32_t)(((int64_t)rel_x * vx + (int64_t)rel_y * vy) >> 16);
        int32_t cross =
            (int32_t)(((int64_t)rel_x * vy - (int64_t)rel_y * vx) >> 16);
        if (dot <= 0) {
            continue;
        }
        /* 許容角: |cross| / dot < tan(2 度)。 */
        int64_t ac = cross < 0 ? -(int64_t)cross : (int64_t)cross;
        if (ac * 1000 >= (int64_t)dot * SHOOTER_HIT_TAN_MILLI) {
            continue;
        }
        uint64_t d2 = (uint64_t)((int64_t)rel_x * rel_x +
                                 (int64_t)rel_y * rel_y);
        int32_t dist = (int32_t)shooter_isqrt(d2);
        if (dist >= wall_dist) {
            continue; /* 壁の向こう側 */
        }
        if (best < 0 || dist < best_dist) {
            best = i;
            best_dist = dist;
        }
    }
    if (best < 0) {
        return -1;
    }
    shooter_enemy *e = &g->enemies[best];
    e->health -= SHOOTER_SHOT_DAMAGE;
    if (e->health <= 0) {
        e->alive = 0;
        g->kills++;
        g->score += SHOOTER_SCORE_PER_KILL;
        if (shooter_enemies_left(g) == 0) {
            g->phase = 2; /* 勝利 */
        }
    }
    return best;
}

void shooter_enemy_tick(shooter_game *g, int second) {
    if (g == NULL || g->phase != 1) {
        return;
    }
    if (second == g->last_tick_second) {
        return; /* 同じ秒に二度動かさない */
    }
    g->last_tick_second = second;
    const int32_t step = SHOOTER_FX_ONE * 12 / 100; /* 0.12 マス/秒 */
    for (int i = 0; i < g->enemy_count; i++) {
        shooter_enemy *e = &g->enemies[i];
        if (!e->alive) {
            continue;
        }
        int32_t dx = g->px - e->x;
        int32_t dy = g->py - e->y;
        uint64_t d2 = (uint64_t)((int64_t)dx * dx + (int64_t)dy * dy);
        int32_t dist = (int32_t)shooter_isqrt(d2);
        if (dist < SHOOTER_FX_ONE * 8 / 10) {
            /* 攻撃範囲: 体力を 10 減らす。 */
            g->health -= 10;
            if (g->health <= 0) {
                g->health = 0;
                g->phase = 3; /* 敗北 */
                return;
            }
            continue;
        }
        if (dist == 0) {
            continue;
        }
        /* プレイヤーの方向に 1 歩進む。距離の大きい軸から進み、
         * 壁があればもう一方の軸を試す (軸ごとに判定)。 */
        int32_t ratio = (int32_t)(((int64_t)step << 16) / dist);
        int32_t sx = (int32_t)(((int64_t)dx * ratio) >> 16);
        int32_t sy = (int32_t)(((int64_t)dy * ratio) >> 16);
        int adx = dx < 0 ? -dx : dx;
        int ady = dy < 0 ? -dy : dy;
        if (adx >= ady) {
            if (shooter_can_stand(e->x + sx, e->y)) {
                e->x += sx;
            } else if (shooter_can_stand(e->x, e->y + sy)) {
                e->y += sy;
            }
        } else {
            if (shooter_can_stand(e->x, e->y + sy)) {
                e->y += sy;
            } else if (shooter_can_stand(e->x + sx, e->y)) {
                e->x += sx;
            }
        }
    }
}

void shooter_turn(shooter_game *g, int steps) {
    if (g == NULL) {
        return;
    }
    int a = (g->angle + steps) % SHOOTER_ANGLE_FULL;
    if (a < 0) {
        a += SHOOTER_ANGLE_FULL;
    }
    g->angle = a;
}
