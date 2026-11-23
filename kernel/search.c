/* PrismOS — 横断検索の一致判定 */
#include "search.h"

#include <stddef.h>

static int lower_char(int c) {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A' + 'a';
    }
    return c;
}

int search_match(const char *query, const char *name) {
    if (query == NULL || name == NULL) {
        return 0;
    }
    if (query[0] == '\0') {
        return 1;
    }
    for (const char *p = name; *p != '\0'; p++) {
        const char *q = query;
        const char *n = p;
        while (*q != '\0' && *n != '\0' &&
               lower_char((unsigned char)*q) ==
                   lower_char((unsigned char)*n)) {
            q++;
            n++;
        }
        if (*q == '\0') {
            return 1;
        }
    }
    return 0;
}
