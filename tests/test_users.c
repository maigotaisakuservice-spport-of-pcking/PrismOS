/* PrismOS — multi-user records: verify, wrap/unwrap of
 * the master key, upsert/remove, TOTP secret lookup. */
#include "../kernel/users.h"

#include <stdio.h>
#include <string.h>

static int checks;
static int failures;

#define CHECK(cond)                                        \
    do {                                                   \
        checks++;                                          \
        if (!(cond)) {                                     \
            failures++;                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                   #cond);                                 \
        }                                                  \
    } while (0)

int main(void) {
    uint8_t salt[16];
    for (int i = 0; i < 16; i++) {
        salt[i] = (uint8_t)(i * 7 + 1);
    }
    char line[256];
    CHECK(users_make_line("alice", salt, 1000, "1357", "user", line,
                          sizeof(line)) == 0);
    CHECK(strncmp(line, "user alice ", 11) == 0);

    char text[PRISM_USERS_TEXT_MAX];
    text[0] = '\0';
    char out[PRISM_USERS_TEXT_MAX];
    CHECK(users_upsert(text, 0, line, out, sizeof(out)) == 0);
    strcpy(text, out);
    CHECK(users_count(text, strlen(text)) == 1);
    CHECK(users_exists(text, strlen(text), "alice") == 1);
    CHECK(users_exists(text, strlen(text), "bob") == 0);
    CHECK(users_verify(text, strlen(text), "alice", "1357") == 1);
    CHECK(users_verify(text, strlen(text), "alice", "9999") == 0);
    CHECK(users_verify(text, strlen(text), "bob", "1357") == 0);
    char role[16];
    CHECK(users_role(text, strlen(text), "alice", role,
                     sizeof(role)) == 0 &&
          strcmp(role, "user") == 0);

    /* wrap/unwrap with alice's key */
    uint8_t mk[32], uk[32];
    for (int i = 0; i < 32; i++) {
        mk[i] = (uint8_t)(i * 3 + 5);
        uk[i] = (uint8_t)(i * 11 + 2);
    }
    char wline[256];
    CHECK(users_make_wrap_line("alice", mk, uk, wline,
                               sizeof(wline)) == 0);
    CHECK(users_upsert(text, strlen(text), wline, out, sizeof(out)) ==
          0);
    strcpy(text, out);
    uint8_t wrap[32], checkb[8], got[32];
    CHECK(users_get_wrap(text, strlen(text), "alice", wrap, checkb) ==
          0);
    CHECK(users_unwrap(wrap, checkb, uk, got) == 0);
    CHECK(memcmp(got, mk, 32) == 0);
    /* wrong key cannot unwrap */
    uint8_t bad[32];
    memset(bad, 0xAA, sizeof(bad));
    CHECK(users_unwrap(wrap, checkb, bad, got) == -1);
    /* tampered wrap cannot unwrap */
    wrap[0] ^= 1;
    CHECK(users_unwrap(wrap, checkb, uk, got) == -1);

    /* rewrap: the primary PIN changes, so the master key
     * changes from mk to mk2. Without knowing alice's key,
     * users_rewrap_all must produce a text whose wrap alice
     * can still open -- yielding mk2, not mk. */
    uint8_t mk2[32];
    for (int i = 0; i < 32; i++) {
        mk2[i] = (uint8_t)(i * 7 + 1);
    }
    /* (the tamper above touched only the local copy; the
     * text still holds the original wrap) */
    {
        char rw[4096];
        int nwrap = 0;
        CHECK(users_rewrap_all(text, strlen(text), mk, mk2, rw,
                               sizeof(rw), &nwrap) == 0);
        CHECK(nwrap == 1);
        uint8_t wrap2[32], check2[8];
        CHECK(users_get_wrap(rw, strlen(rw), "alice", wrap2,
                             check2) == 0);
        CHECK(users_unwrap(wrap2, check2, uk, got) == 0);
        CHECK(memcmp(got, mk2, 32) == 0);
        /* the old master key must NOT come out anymore */
        CHECK(memcmp(got, mk, 32) != 0);
        /* wrong user key still cannot open the rewrapped wrap */
        CHECK(users_unwrap(wrap2, check2, bad, got) == -1);
        /* the user record itself survived the rewrite */
        CHECK(users_verify(rw, strlen(rw), "alice", "1357") == 1);
    }

    /* totp line */
    CHECK(users_upsert(text, strlen(text),
                       "totp alice JBSWY3DPEHPK3PXP", out,
                       sizeof(out)) == 0);
    strcpy(text, out);
    char sec[64];
    CHECK(users_totp(text, strlen(text), "alice", sec, sizeof(sec)) ==
              0 &&
          strcmp(sec, "JBSWY3DPEHPK3PXP") == 0);
    CHECK(users_totp(text, strlen(text), "bob", sec, sizeof(sec)) ==
          -1);

    /* second user, then removal */
    CHECK(users_make_line("bob", salt, 1000, "2468", "admin", line,
                          sizeof(line)) == 0);
    CHECK(users_upsert(text, strlen(text), line, out, sizeof(out)) ==
          0);
    strcpy(text, out);
    CHECK(users_count(text, strlen(text)) == 2);
    CHECK(users_verify(text, strlen(text), "bob", "2468") == 1);
    CHECK(users_remove_user(text, strlen(text), "alice", out,
                            sizeof(out)) == 0);
    strcpy(text, out);
    CHECK(users_count(text, strlen(text)) == 1);
    CHECK(users_exists(text, strlen(text), "alice") == 0);
    CHECK(users_get_wrap(text, strlen(text), "alice", wrap, checkb) ==
          -1);
    CHECK(users_totp(text, strlen(text), "alice", sec, sizeof(sec)) ==
          -1);
    CHECK(users_verify(text, strlen(text), "bob", "2468") == 1);

    /* name rules */
    CHECK(users_name_ok("alice") == 1);
    CHECK(users_name_ok("a-1_x") == 1);
    CHECK(users_name_ok("admin") == 0);
    CHECK(users_name_ok("guest") == 0);
    CHECK(users_name_ok("") == 0);
    CHECK(users_name_ok("a b") == 0);
    CHECK(users_name_ok("0123456789abcdefg") == 0);

    /* upsert replaces the PIN record */
    CHECK(users_make_line("bob", salt, 1000, "9999", "user", line,
                          sizeof(line)) == 0);
    CHECK(users_upsert(text, strlen(text), line, out, sizeof(out)) ==
          0);
    strcpy(text, out);
    CHECK(users_count(text, strlen(text)) == 1);
    CHECK(users_verify(text, strlen(text), "bob", "9999") == 1);
    CHECK(users_verify(text, strlen(text), "bob", "2468") == 0);

    printf("users tests: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
