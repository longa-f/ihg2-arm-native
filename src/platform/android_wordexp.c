#include "wordexp.h"
#include <stdlib.h>
#include <string.h>

// HAP only normalizes the fixed, literal .hap_store path on this target. Do not
// evaluate shell syntax or expand environment variables from an untrusted path.
int wordexp(const char* words, wordexp_t* result, int flags) {
    (void)flags;
    if (!words || !result || !*words) return WRDE_SYNTAX;
    memset(result, 0, sizeof *result);
    result->we_wordv = calloc(2, sizeof(char*));
    if (!result->we_wordv) return WRDE_NOSPACE;
    result->we_wordv[0] = strdup(words);
    if (!result->we_wordv[0]) {
        free(result->we_wordv);
        memset(result, 0, sizeof *result);
        return WRDE_NOSPACE;
    }
    result->we_wordc = 1;
    return 0;
}

void wordfree(wordexp_t* result) {
    if (!result) return;
    if (result->we_wordv) {
        free(result->we_wordv[0]);
        free(result->we_wordv);
    }
    memset(result, 0, sizeof *result);
}
