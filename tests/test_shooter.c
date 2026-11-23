/* PrismOS — 3D シューティングゲーム (レイキャスティング) のホストテスト。 */
#include <stdio.h>

#include "shooter.h"

static int checks = 0;
static int failures = 0;

static void check(int cond, const char *name) {
    checks++;
    if (!cond) {
        failures++;
        printf("FAIL: %s\n", name);
    }
}

int main(void) {
    /* sin/cos テーブル。 */
    check(shooter_sin(0) == 0, "sin(0) == 0");
    check(shooter_sin(256) == 65536, "sin(90deg) == max");
    check(shooter_sin(512) == 0, "sin(180deg) == 0");
    check(shooter_sin(768) == -65536, "sin(270deg) == min");
    check(shooter_cos(0) == 65536, "cos(0) == max");
    check(shooter_cos(256) == 0, "cos(90deg) == 0");
    check(shooter_cos(512) == -65536, "cos(180deg) == min");
    check(shooter_sin(1024) == shooter_sin(0), "sin wraps at 1024");
    check(shooter_cos(-1) == shooter_cos(1023), "cos wraps below 0");

    /* 整数の平方根。 */
    check(shooter_isqrt(0) == 0, "isqrt(0) == 0");
    check(shooter_isqrt(1) == 1, "isqrt(1) == 1");
    check(shooter_isqrt(2) == 1, "isqrt(2) == 1");
    check(shooter_isqrt(16) == 4, "isqrt(16) == 4");
    check(shooter_isqrt(65536) == 256, "isqrt(65536) == 256");
    check(shooter_isqrt(1000000) == 1000, "isqrt(1000000) == 1000");
    check(shooter_isqrt(1000001) == 1000, "isqrt(1000001) == 1000");

    /* 壁の判定。 */
    check(shooter_is_wall(0, 0) == 1, "corner is a wall");
    check(shooter_is_wall(1, 1) == 0, "player start is open");
    check(shooter_is_wall(15, 15) == 1, "far corner is a wall");
    check(shooter_is_wall(16, 0) == 1, "outside x is a wall");
    check(shooter_is_wall(0, -1) == 1, "outside y is a wall");
    check(shooter_is_wall(8, 5) == 1, "inner wall at (8,5)");

    /* レイキャスティング: (1.5, 1.5) から東へは 13.5 マス。
     * 13.5 * 65536 = 884736。 */
    int side = -1;
    int32_t d = shooter_ray(3 * SHOOTER_FX_ONE / 2, 3 * SHOOTER_FX_ONE / 2,
                            0, &side);
    check(d == 13 * SHOOTER_FX_ONE + SHOOTER_FX_ONE / 2,
          "ray east from start is 13.5 cells");
    check(side == 0, "east ray hits a north-south wall");
    /* 北へは壁の手前の面 (y=1.0) まで 0.5 マス。 */
    d = shooter_ray(3 * SHOOTER_FX_ONE / 2, 3 * SHOOTER_FX_ONE / 2, 768,
                    &side);
    check(d == SHOOTER_FX_ONE / 2, "ray north from start is 0.5 cells");
    check(side == 1, "north ray hits an east-west wall");
    /* (1.5, 5.5) から東へは壁 (8, 5) まで 6.5 マス。 */
    d = shooter_ray(3 * SHOOTER_FX_ONE / 2, 11 * SHOOTER_FX_ONE / 2, 0,
                    &side);
    check(d == 13 * SHOOTER_FX_ONE / 2, "ray east in row 5 is 6.5 cells");
    /* 南へは壁の手前の面 (y=15.0) まで 13.5 マス。 */
    d = shooter_ray(3 * SHOOTER_FX_ONE / 2, 3 * SHOOTER_FX_ONE / 2, 256,
                    &side);
    check(d == 27 * SHOOTER_FX_ONE / 2, "ray south from start is 13.5 cells");

    /* 移動と衝突。 */
    shooter_game g;
    shooter_new_game(&g);
    check(g.px == 3 * SHOOTER_FX_ONE / 2 &&
              g.py == 3 * SHOOTER_FX_ONE / 2,
          "player starts at (1.5, 1.5)");
    check(g.angle == 0, "player starts facing east");
    check(g.health == SHOOTER_PLAYER_HEALTH, "player starts at full health");
    check(g.phase == 1, "game starts in play phase");
    check(g.enemy_count == 6, "six enemies on the map");
    check(shooter_enemies_left(&g) == 6, "six enemies alive");
    /* 東へ 0.12 マス進む。 */
    int32_t ox = g.px;
    int moved = shooter_try_move(&g.px, &g.py, 0, SHOOTER_FX_ONE * 12 / 100);
    check(moved == 1, "moving east in the corridor works");
    check(g.px > ox && g.py == 3 * SHOOTER_FX_ONE / 2,
          "move east changes x only");
    /* 壁に向かって進んでも止まる: (14.5, 1.5) から東へ 1 マスは
     * 壁 (col 15) なので進めない。(0.12 マスならまだ空きマス。) */
    g.px = 29 * SHOOTER_FX_ONE / 2;
    g.py = 3 * SHOOTER_FX_ONE / 2;
    moved = shooter_try_move(&g.px, &g.py, 0, SHOOTER_FX_ONE);
    check(moved == 0, "moving 1 cell east into the wall is blocked");
    check(g.px == 29 * SHOOTER_FX_ONE / 2, "blocked move keeps x");
    moved = shooter_try_move(&g.px, &g.py, 0, SHOOTER_FX_ONE * 12 / 100);
    check(moved == 1, "moving 0.12 east from 14.5 is still open");
    /* 北へ 1 マスは壁 (row 0) で止まる。 */
    g.px = 3 * SHOOTER_FX_ONE / 2;
    g.py = 3 * SHOOTER_FX_ONE / 2;
    moved = shooter_try_move(&g.px, &g.py, 768, SHOOTER_FX_ONE);
    check(moved == 0, "moving 1 cell north into the wall is blocked");

    /* 画面上の敵の位置: 最初の敵は正面の真ん中。 */
    shooter_new_game(&g);
    int32_t sx = 0;
    int32_t dist = 0;
    int vis = shooter_enemy_screen(&g, &g.enemies[0], &sx, &dist);
    check(vis == 1, "first enemy is visible from the start");
    check(sx == SHOOTER_FX_ONE / 2, "first enemy is at screen centre");
    check(dist == 3 * SHOOTER_FX_ONE, "first enemy is 3 cells away");
    /* 背後の敵は見えない (180 度回転)。 */
    g.angle = 512;
    vis = shooter_enemy_screen(&g, &g.enemies[0], &sx, &dist);
    check(vis == 0, "enemy behind is not visible");
    g.angle = 0;

    /* 発射: 正面の敵に 3 発で倒れる。 */
    int32_t wall = shooter_ray(g.px, g.py, g.angle, NULL);
    int hit = shooter_shoot(&g, wall);
    check(hit == 0, "first shot hits the first enemy");
    check(g.enemies[0].health == SHOOTER_ENEMY_HEALTH - SHOOTER_SHOT_DAMAGE,
          "first shot deals damage");
    check(g.enemies[0].alive == 1, "enemy survives one shot");
    (void)shooter_shoot(&g, wall);
    hit = shooter_shoot(&g, wall);
    check(hit == 0, "third shot hits the first enemy");
    check(g.enemies[0].alive == 0, "enemy dies after three shots");
    check(g.kills == 1, "one kill counted");
    check(g.score == SHOOTER_SCORE_PER_KILL, "score is 100 after one kill");
    check(shooter_enemies_left(&g) == 5, "five enemies left");
    /* 壁より遠い敵には当たらない (壁までの距離を短くする)。 */
    shooter_new_game(&g);
    hit = shooter_shoot(&g, SHOOTER_FX_ONE);
    check(hit == -1, "shot through a near wall misses");
    /* 後ろを向いたら当たらない。 */
    g.angle = 512;
    wall = shooter_ray(g.px, g.py, g.angle, NULL);
    hit = shooter_shoot(&g, wall);
    check(hit == -1, "shot backwards misses");

    /* 勝利: 全ての敵を正面から倒す。 */
    shooter_new_game(&g);
    for (int i = 0; i < g.enemy_count; i++) {
        /* 敵の 1 マス西に立って東を向く。 */
        g.px = g.enemies[i].x - SHOOTER_FX_ONE;
        g.py = g.enemies[i].y;
        g.angle = 0;
        wall = shooter_ray(g.px, g.py, g.angle, NULL);
        check(wall > SHOOTER_FX_ONE, "clear line to enemy for win test");
        for (int s = 0; s < 3; s++) {
            (void)shooter_shoot(&g, wall);
        }
    }
    check(shooter_enemies_left(&g) == 0, "all enemies dead for the win");
    check(g.phase == 2, "game is won when all enemies are dead");
    check(g.score == 6 * SHOOTER_SCORE_PER_KILL,
          "score is 600 after six kills");

    /* 敵の AI: 近づいてくる。 */
    shooter_new_game(&g);
    int32_t ex0 = g.enemies[0].x;
    shooter_enemy_tick(&g, 1);
    check(g.enemies[0].x < ex0, "enemy moves towards the player");
    check(g.enemies[0].y == 3 * SHOOTER_FX_ONE / 2,
          "enemy moves along the corridor row");
    /* 同じ秒に二度呼んでも動かない。 */
    int32_t ex1 = g.enemies[0].x;
    shooter_enemy_tick(&g, 1);
    check(g.enemies[0].x == ex1, "enemy does not move twice in a second");
    /* 敵が隣に来たら攻撃されて体力が減る: 東へ歩いて近づく。 */
    shooter_new_game(&g);
    for (int i = 0; i < 24; i++) {
        (void)shooter_try_move(&g.px, &g.py, 0,
                               SHOOTER_FX_ONE * 12 / 100);
    }
    shooter_enemy_tick(&g, 2);
    check(g.health == SHOOTER_PLAYER_HEALTH - 10,
          "adjacent enemy attacks for 10 damage");
    check(g.phase == 1, "game continues after one attack");

    /* 敗北: 体力 10 で隣の敵に攻撃されると倒れる。 */
    shooter_new_game(&g);
    g.health = 10;
    g.px = g.enemies[0].x - SHOOTER_FX_ONE / 2;
    g.py = g.enemies[0].y;
    shooter_enemy_tick(&g, 3);
    check(g.health == 0, "health reaches 0 when attacked at 10");
    check(g.phase == 3, "game is lost at 0 health");

    /* 回転は 1024 で一周する。 */
    shooter_new_game(&g);
    shooter_turn(&g, 1024);
    check(g.angle == 0, "turning 1024 steps is a full circle");
    shooter_turn(&g, -1);
    check(g.angle == 1023, "turning -1 from 0 wraps to 1023");
    shooter_turn(&g, 2);
    check(g.angle == 1, "turning +2 from 1023 wraps to 1");

    printf("shooter tests: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
