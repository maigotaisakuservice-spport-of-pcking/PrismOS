/* PrismOS — トランプ (ブラックジャック) のホストテスト。 */
#include <stdio.h>
#include <string.h>

#include "cards.h"

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
    int deck[PRISM_DECK_SIZE];
    int seen[PRISM_DECK_SIZE];

    /* 新しいデッキは 0〜51 が 1 枚ずつ。 */
    cards_new_deck(deck);
    for (int i = 0; i < PRISM_DECK_SIZE; i++) {
        seen[i] = 0;
    }
    for (int i = 0; i < PRISM_DECK_SIZE; i++) {
        check(deck[i] >= 0 && deck[i] < PRISM_DECK_SIZE,
              "new deck card in range");
        seen[deck[i]]++;
    }
    int all_once = 1;
    for (int i = 0; i < PRISM_DECK_SIZE; i++) {
        if (seen[i] != 1) {
            all_once = 0;
        }
    }
    check(all_once, "new deck has each card once");

    /* シャッフルしても 52 枚の顔ぶれは変わらない。 */
    cards_shuffle(deck, 12345);
    for (int i = 0; i < PRISM_DECK_SIZE; i++) {
        seen[i] = 0;
    }
    for (int i = 0; i < PRISM_DECK_SIZE; i++) {
        seen[deck[i]]++;
    }
    all_once = 1;
    for (int i = 0; i < PRISM_DECK_SIZE; i++) {
        if (seen[i] != 1) {
            all_once = 0;
        }
    }
    check(all_once, "shuffled deck still has each card once");
    /* 並びは変わる (seed 12345 では先頭が変わることを確認)。 */
    check(deck[0] != 0 || deck[1] != 1, "shuffle changes the order");
    /* 同じ seed では同じ並びになる (再現性)。 */
    int deck2[PRISM_DECK_SIZE];
    cards_new_deck(deck2);
    cards_shuffle(deck2, 12345);
    int same = 1;
    for (int i = 0; i < PRISM_DECK_SIZE; i++) {
        if (deck[i] != deck2[i]) {
            same = 0;
        }
    }
    check(same, "same seed gives the same shuffle");
    /* 違う seed では並びが変わる。 */
    cards_new_deck(deck2);
    cards_shuffle(deck2, 999);
    int diff = 0;
    for (int i = 0; i < PRISM_DECK_SIZE; i++) {
        if (deck[i] != deck2[i]) {
            diff = 1;
        }
    }
    check(diff, "different seed gives a different shuffle");

    /* ランクとスート。 */
    check(cards_rank(0) == 1 && cards_suit(0) == 0, "card 0 is ace of spades");
    check(cards_rank(12) == 13 && cards_suit(12) == 0, "card 12 is king of spades");
    check(cards_rank(13) == 1 && cards_suit(13) == 1, "card 13 is ace of hearts");
    check(cards_rank(51) == 13 && cards_suit(51) == 3, "card 51 is king of clubs");

    /* 表示。 */
    char buf[4];
    check(cards_format(0, buf, sizeof(buf)) == 2 && strcmp(buf, "AS") == 0,
          "format ace of spades");
    check(cards_format(9, buf, sizeof(buf)) == 3 && strcmp(buf, "10S") == 0,
          "format 10 of spades");
    check(cards_format(24, buf, sizeof(buf)) == 2 && strcmp(buf, "QH") == 0,
          "format queen of hearts");
    check(cards_format(51, buf, sizeof(buf)) == 2 && strcmp(buf, "KC") == 0,
          "format king of clubs");

    /* 手札の合計。 */
    int hand[8];
    hand[0] = 0;  /* AS */
    hand[1] = 12; /* KS */
    check(cards_hand_value(hand, 2) == 21, "ace + king is 21");
    check(cards_is_blackjack(hand, 2) == 1, "ace + king is blackjack");
    hand[0] = 0;  /* AS */
    hand[1] = 13; /* AH */
    hand[2] = 8;  /* 9S */
    check(cards_hand_value(hand, 3) == 21, "ace + ace + 9 is 21");
    hand[0] = 12; /* KS */
    hand[1] = 11; /* QS */
    hand[2] = 1;  /* 2S */
    check(cards_hand_value(hand, 3) == 22, "king + queen + 2 is 22 (bust)");
    check(cards_is_blackjack(hand, 3) == 0, "3 cards is never blackjack");
    hand[0] = 0;  /* AS */
    hand[1] = 4;  /* 5S */
    check(cards_hand_value(hand, 2) == 16, "ace + 5 is 16");
    hand[0] = 0;  /* AS */
    hand[1] = 4;  /* 5S */
    hand[2] = 9;  /* 10S */
    check(cards_hand_value(hand, 3) == 16, "ace + 5 + 10 is 16 (ace as 1)");

    /* ディーラーのルール。 */
    check(cards_dealer_hits(16) == 1, "dealer hits on 16");
    check(cards_dealer_hits(17) == 0, "dealer stands on 17");
    check(cards_dealer_hits(21) == 0, "dealer stands on 21");

    /* ---- ポーカー ---- */
    {
        /* card = suit*13 + (rank-1); suit 0=spades */
        int sf[5] = { 4, 5, 6, 7, 8 };       /* 5-9 spades */
        int fk[5] = { 12, 25, 38, 51, 0 };   /* four kings + A */
        int fh[5] = { 12, 25, 38, 1, 14 };   /* KKK + 22 */
        int fl[5] = { 0, 2, 4, 6, 10 };      /* spade flush */
        int st[5] = { 4, 18, 32, 46, 8 };    /* 5-9 mixed */
        int tk[5] = { 12, 25, 38, 3, 7 };    /* KKK */
        int tp[5] = { 12, 25, 1, 14, 7 };    /* KK 22 */
        int pr[5] = { 12, 25, 3, 5, 7 };     /* KK */
        int hc[5] = { 12, 24, 3, 5, 7 };     /* K high */
        check(poker_eval(sf) > poker_eval(fk), "poker sf>fk");
        check(poker_eval(fk) > poker_eval(fh), "poker fk>fh");
        check(poker_eval(fh) > poker_eval(fl), "poker fh>fl");
        check(poker_eval(fl) > poker_eval(st), "poker fl>st");
        check(poker_eval(st) > poker_eval(tk), "poker st>tk");
        check(poker_eval(tk) > poker_eval(tp), "poker tk>tp");
        check(poker_eval(tp) > poker_eval(pr), "poker tp>pr");
        check(poker_eval(pr) > poker_eval(hc), "poker pr>hc");
        int wheel[5] = { 0, 14, 28, 42, 4 }; /* A 2 3 4 5 */
        int six[5] = { 1, 15, 29, 43, 5 };   /* 2 3 4 5 6 */
        check(poker_eval(wheel) > poker_eval(tk),
              "poker wheel is a straight");
        check(poker_eval(wheel) < poker_eval(six),
              "poker wheel < 6-straight");
        int pa[5] = { 0, 13, 3, 5, 7 };      /* pair of aces */
        check(poker_eval(pa) > poker_eval(pr),
              "poker aces > kings");
        check(strcmp(poker_category(poker_eval(sf)),
                     "Straight flush") == 0, "poker cat sf");
        check(strcmp(poker_category(poker_eval(pr)), "Pair") ==
                  0,
              "poker cat pair");
        int disc[5];
        check(poker_cpu_discards(tp, disc) == 0,
              "cpu keeps two pair");
        check(poker_cpu_discards(pr, disc) == 3,
              "cpu discards 3 with a pair");
        int ff[5] = { 0, 2, 4, 6, 20 };      /* 4 spades + x */
        check(poker_cpu_discards(ff, disc) == 1,
              "cpu draws to flush");
    }
    /* ---- バカラ ---- */
    {
        check(baccarat_card_value(12) == 0, "bac K=0");
        check(baccarat_card_value(0) == 1, "bac A=1");
        int h1[2] = { 8, 7 };   /* 9 + 8 = 17 -> 7 */
        check(baccarat_value(h1, 2) == 7, "bac 9+8=7");
        check(baccarat_player_draws(5) == 1, "bac P draws 5");
        check(baccarat_player_draws(6) == 0, "bac P stands 6");
        check(baccarat_banker_draws(3, 8) == 0,
              "bac B3 stands vs 8");
        check(baccarat_banker_draws(3, 9) == 1,
              "bac B3 draws vs 9");
        check(baccarat_banker_draws(4, 1) == 0,
              "bac B4 stands vs 1");
        check(baccarat_banker_draws(4, 5) == 1,
              "bac B4 draws vs 5");
        check(baccarat_banker_draws(6, 6) == 1,
              "bac B6 draws vs 6");
        check(baccarat_banker_draws(6, 5) == 0,
              "bac B6 stands vs 5");
        check(baccarat_banker_draws(5, -1) == 1,
              "bac B5 draws when P stood");
        check(baccarat_banker_draws(6, -1) == 0,
              "bac B6 stands when P stood");
        check(baccarat_banker_draws(2, 0) == 1,
              "bac B2 always draws");
        check(baccarat_banker_draws(7, 6) == 0,
              "bac B7 always stands");
    }
    /* ---- 7並べ ---- */
    {
        int lo[4] = { 0, 0, 0, 0 };
        int hi[4] = { 0, 0, 0, 0 };
        int seven_s = 6;         /* 7 of spades */
        int six_s = 5, eight_s = 7, five_s = 4;
        check(sevens_playable(lo, hi, seven_s) == 1,
              "sevens: 7 playable");
        check(sevens_playable(lo, hi, six_s) == 0,
              "sevens: 6 not yet");
        sevens_place(lo, hi, seven_s);
        check(sevens_playable(lo, hi, six_s) == 1,
              "sevens: 6 after 7");
        check(sevens_playable(lo, hi, eight_s) == 1,
              "sevens: 8 after 7");
        check(sevens_playable(lo, hi, five_s) == 0,
              "sevens: 5 still not");
        sevens_place(lo, hi, six_s);
        check(sevens_playable(lo, hi, five_s) == 1,
              "sevens: 5 after 6");
    }
    /* ---- ババ抜き ---- */
    {
        int hand[6] = { 0, 13, 4, 52, 30, 43 };
        /* A spades, A hearts, 5 spades, joker, 5 dia,
         * 5 clubs -> pairs: A and two of the 5s... three
         * 5s: one pair removed, one 5 remains. */
        int n = oldmaid_discard_pairs(hand, 6);
        check(n == 2, "oldmaid: joker + one 5 remain");
        int has_joker = 0;
        for (int i = 0; i < n; i++) {
            if (hand[i] == 52) {
                has_joker = 1;
            }
        }
        check(has_joker, "oldmaid: joker stays");
        int h2[4] = { 0, 13, 26, 39 };
        check(oldmaid_discard_pairs(h2, 4) == 0,
              "oldmaid: four of a kind -> all paired off");
    }

    printf("cards tests: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
