/* PrismOS — トランプ (ブラックジャック) の純粋ロジック */
#include "cards.h"

void cards_new_deck(int deck[PRISM_DECK_SIZE]) {
    for (int i = 0; i < PRISM_DECK_SIZE; i++) {
        deck[i] = i;
    }
}

static uint32_t cards_next_random(uint32_t *state) {
    /* xorshift32: ゲーム用の乱数。暗号には使わない。 */
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

void cards_shuffle(int deck[PRISM_DECK_SIZE], uint32_t seed) {
    if (seed == 0) {
        seed = 1;
    }
    uint32_t state = seed;
    for (int i = PRISM_DECK_SIZE - 1; i > 0; i--) {
        int j = (int)(cards_next_random(&state) % (uint32_t)(i + 1));
        int tmp = deck[i];
        deck[i] = deck[j];
        deck[j] = tmp;
    }
}

int cards_rank(int card) {
    return card % 13 + 1;
}

int cards_suit(int card) {
    return card / 13;
}

int cards_hand_value(const int *hand, int n) {
    int total = 0;
    int aces = 0;
    for (int i = 0; i < n; i++) {
        int r = cards_rank(hand[i]);
        if (r == 1) {
            aces++;
            total += 11;
        } else if (r >= 11) {
            total += 10;
        } else {
            total += r;
        }
    }
    while (total > 21 && aces > 0) {
        total -= 10;
        aces--;
    }
    return total;
}

int cards_is_blackjack(const int *hand, int n) {
    return n == 2 && cards_hand_value(hand, n) == 21;
}

int cards_dealer_hits(int value) {
    return value < 17;
}

int cards_format(int card, char *buf, size_t cap) {
    if (buf == NULL || cap < 4) {
        return 0;
    }
    if (card == 52) {
        /* ジョーカー (ババ抜き用)。 */
        if (cap < 6) {
            return 0;
        }
        buf[0] = 'J';
        buf[1] = 'O';
        buf[2] = 'K';
        buf[3] = 'E';
        buf[4] = 'R';
        buf[5] = '\0';
        return 5;
    }
    int r = cards_rank(card);
    int s = cards_suit(card);
    int n = 0;
    if (r == 1) {
        buf[n++] = 'A';
    } else if (r == 11) {
        buf[n++] = 'J';
    } else if (r == 12) {
        buf[n++] = 'Q';
    } else if (r == 13) {
        buf[n++] = 'K';
    } else if (r == 10) {
        buf[n++] = '1';
        buf[n++] = '0';
    } else {
        buf[n++] = (char)('0' + r);
    }
    buf[n++] = (s == 0) ? 'S' : (s == 1) ? 'H' : (s == 2) ? 'D' : 'C';
    buf[n] = '\0';
    return n;
}

/* ---------- poker (5-card draw) ---------- */

/* Evaluate a 5-card poker hand. The return value orders
 * hands correctly with a plain integer compare: the
 * category occupies the high bits and tie-break ranks
 * follow. Categories: 0 high card, 1 pair, 2 two pair,
 * 3 three of a kind, 4 straight, 5 flush, 6 full house,
 * 7 four of a kind, 8 straight flush. */
int poker_eval(const int *hand) {
    int ranks[5];
    int suits[5];
    for (int i = 0; i < 5; i++) {
        ranks[i] = cards_rank(hand[i]);
        suits[i] = cards_suit(hand[i]);
    }
    /* sort ranks descending (ace counts as 14) */
    int v[5];
    for (int i = 0; i < 5; i++) {
        v[i] = ranks[i] == 1 ? 14 : ranks[i];
    }
    for (int i = 0; i < 5; i++) {
        for (int j = i + 1; j < 5; j++) {
            if (v[j] > v[i]) {
                int t = v[i];
                v[i] = v[j];
                v[j] = t;
            }
        }
    }
    int flush = 1;
    for (int i = 1; i < 5; i++) {
        if (suits[i] != suits[0]) {
            flush = 0;
        }
    }
    int straight = 0;
    int straight_top = 0;
    if (v[0] - v[4] == 4 && v[0] != v[1] && v[1] != v[2] &&
        v[2] != v[3] && v[3] != v[4]) {
        straight = 1;
        straight_top = v[0];
    }
    /* wheel: A-2-3-4-5 */
    if (v[0] == 14 && v[1] == 5 && v[2] == 4 && v[3] == 3 &&
        v[4] == 2) {
        straight = 1;
        straight_top = 5;
    }
    /* group counts */
    int cnt[15];
    for (int i = 0; i < 15; i++) {
        cnt[i] = 0;
    }
    for (int i = 0; i < 5; i++) {
        cnt[v[i]]++;
    }
    int pairs = 0, threes = 0, fours = 0;
    int pair_rank = 0, three_rank = 0, four_rank = 0;
    int second_pair = 0;
    for (int r = 14; r >= 2; r--) {
        if (cnt[r] == 4) {
            fours++;
            four_rank = r;
        } else if (cnt[r] == 3) {
            threes++;
            three_rank = r;
        } else if (cnt[r] == 2) {
            pairs++;
            if (pair_rank == 0) {
                pair_rank = r;
            } else {
                second_pair = r;
            }
        }
    }
    int cat;
    int tb[5] = { 0, 0, 0, 0, 0 };
    if (straight && flush) {
        cat = 8;
        tb[0] = straight_top;
    } else if (fours) {
        cat = 7;
        tb[0] = four_rank;
        for (int i = 0; i < 5; i++) {
            if (v[i] != four_rank) {
                tb[1] = v[i];
            }
        }
    } else if (threes && pairs) {
        cat = 6;
        tb[0] = three_rank;
        tb[1] = pair_rank;
    } else if (flush) {
        cat = 5;
        for (int i = 0; i < 5; i++) {
            tb[i] = v[i];
        }
    } else if (straight) {
        cat = 4;
        tb[0] = straight_top;
    } else if (threes) {
        cat = 3;
        tb[0] = three_rank;
        int k = 1;
        for (int i = 0; i < 5; i++) {
            if (v[i] != three_rank) {
                tb[k++] = v[i];
            }
        }
    } else if (pairs == 2) {
        cat = 2;
        tb[0] = pair_rank;
        tb[1] = second_pair;
        for (int i = 0; i < 5; i++) {
            if (v[i] != pair_rank && v[i] != second_pair) {
                tb[2] = v[i];
            }
        }
    } else if (pairs == 1) {
        cat = 1;
        tb[0] = pair_rank;
        int k = 1;
        for (int i = 0; i < 5; i++) {
            if (v[i] != pair_rank) {
                tb[k++] = v[i];
            }
        }
    } else {
        cat = 0;
        for (int i = 0; i < 5; i++) {
            tb[i] = v[i];
        }
    }
    int score = cat;
    for (int i = 0; i < 5; i++) {
        score = score * 16 + tb[i];
    }
    return score;
}

const char *poker_category(int score) {
    static const char *names[9] = {
        "High card", "Pair", "Two pair", "Three of a kind",
        "Straight", "Flush", "Full house", "Four of a kind",
        "Straight flush"
    };
    int cat = score;
    for (int i = 0; i < 5; i++) {
        cat /= 16;
    }
    if (cat < 0 || cat > 8) {
        return "Unknown";
    }
    return names[cat];
}

/* CPU discard heuristic for draw poker: keep made hands
 * (two pair or better keeps everything), keep a pair
 * (discard the other three), keep four to a flush or an
 * open straight, otherwise keep only cards jack or
 * better. Fills discard[5] with 1 = throw away. Returns
 * how many to discard. */
int poker_cpu_discards(const int *hand, int discard[5]) {
    int score = poker_eval(hand);
    int cat = score;
    for (int i = 0; i < 5; i++) {
        cat /= 16;
    }
    for (int i = 0; i < 5; i++) {
        discard[i] = 0;
    }
    if (cat >= 2) {
        return 0;
    }
    if (cat == 1) {
        /* find the pair rank and discard the rest */
        int cnt[15] = { 0 };
        for (int i = 0; i < 5; i++) {
            int r = cards_rank(hand[i]);
            cnt[r == 1 ? 14 : r]++;
        }
        int pr = 0;
        for (int r = 2; r <= 14; r++) {
            if (cnt[r] == 2) {
                pr = r;
            }
        }
        int n = 0;
        for (int i = 0; i < 5; i++) {
            int r = cards_rank(hand[i]);
            if ((r == 1 ? 14 : r) != pr) {
                discard[i] = 1;
                n++;
            }
        }
        return n;
    }
    /* four to a flush? */
    int sc[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < 5; i++) {
        sc[cards_suit(hand[i])]++;
    }
    for (int su = 0; su < 4; su++) {
        if (sc[su] == 4) {
            int n = 0;
            for (int i = 0; i < 5; i++) {
                if (cards_suit(hand[i]) != su) {
                    discard[i] = 1;
                    n++;
                }
            }
            return n;
        }
    }
    /* keep jack or better */
    int n = 0;
    for (int i = 0; i < 5; i++) {
        int r = cards_rank(hand[i]);
        if (r != 1 && r < 11) {
            discard[i] = 1;
            n++;
        }
    }
    if (n == 5) {
        /* keep the highest single card */
        int best = 0;
        for (int i = 1; i < 5; i++) {
            int ri = cards_rank(hand[i]);
            int rb = cards_rank(hand[best]);
            if ((ri == 1 ? 14 : ri) > (rb == 1 ? 14 : rb)) {
                best = i;
            }
        }
        discard[best] = 0;
        n = 4;
    }
    return n;
}

/* ---------- baccarat ---------- */

/* Card value in baccarat: 10/J/Q/K = 0, ace = 1. */
int baccarat_card_value(int card) {
    int r = cards_rank(card);
    return r >= 10 ? 0 : r;
}

/* Hand total modulo 10. */
int baccarat_value(const int *hand, int n) {
    int s = 0;
    for (int i = 0; i < n; i++) {
        s += baccarat_card_value(hand[i]);
    }
    return s % 10;
}

/* Third-card rules (the standard tableau). Player: draw
 * on 0-5, stand on 6-7 (8-9 are naturals, decided by the
 * caller). Banker: depends on its total and the player's
 * third card value (p3), or -1 when the player stood. */
int baccarat_player_draws(int two_card_total) {
    return two_card_total <= 5;
}

int baccarat_banker_draws(int banker_total, int p3) {
    if (banker_total <= 2) {
        return 1;
    }
    if (p3 < 0) {
        /* player stood: banker plays like the player */
        return banker_total <= 5;
    }
    switch (banker_total) {
    case 3:
        return p3 != 8;
    case 4:
        return p3 >= 2 && p3 <= 7;
    case 5:
        return p3 >= 4 && p3 <= 7;
    case 6:
        return p3 == 6 || p3 == 7;
    default:
        return 0;
    }
}

/* ---------- sevens (7並べ) ---------- */

/* The table state for sevens: for each suit, the lowest
 * and highest rank placed so far (the 7 starts placed).
 * lo[s] / hi[s] are ranks 1-13; 0 = nothing placed yet
 * (only possible before the 7 is out). A card is
 * playable when it is a 7, or it extends its suit's run
 * by one on either side. */
int sevens_playable(const int lo[4], const int hi[4], int card) {
    int r = cards_rank(card);
    int s = cards_suit(card);
    if (r == 7) {
        return 1;
    }
    if (lo[s] == 0) {
        return 0;
    }
    return r == lo[s] - 1 || r == hi[s] + 1;
}

void sevens_place(int lo[4], int hi[4], int card) {
    int r = cards_rank(card);
    int s = cards_suit(card);
    if (lo[s] == 0) {
        lo[s] = r;
        hi[s] = r;
    } else if (r < lo[s]) {
        lo[s] = r;
    } else if (r > hi[s]) {
        hi[s] = r;
    }
}

/* ---------- old maid (ババ抜き) ---------- */

/* Remove pairs (same rank) from a hand. Cards are 0-51
 * plus 52 for the joker (never pairs). Compacts the
 * hand in place and returns the new count. */
int oldmaid_discard_pairs(int *hand, int n) {
    int used[64];
    for (int i = 0; i < n; i++) {
        used[i] = 0;
    }
    for (int i = 0; i < n; i++) {
        if (used[i] || hand[i] == 52) {
            continue;
        }
        for (int j = i + 1; j < n; j++) {
            if (!used[j] && hand[j] != 52 &&
                cards_rank(hand[j]) == cards_rank(hand[i])) {
                used[i] = 1;
                used[j] = 1;
                break;
            }
        }
    }
    int w = 0;
    for (int i = 0; i < n; i++) {
        if (!used[i]) {
            hand[w++] = hand[i];
        }
    }
    return w;
}
