#pragma once
#include <stddef.h>
typedef struct {
    size_t we_wordc;
    char** we_wordv;
    size_t we_offs;
} wordexp_t;
#define WRDE_NOCMD 1
#define WRDE_UNDEF 2
#define WRDE_APPEND 4
#define WRDE_DOOFFS 8
#define WRDE_BADCHAR 1
#define WRDE_BADVAL 2
#define WRDE_CMDSUB 3
#define WRDE_NOSPACE 4
#define WRDE_SYNTAX 5
int wordexp(const char* words, wordexp_t* result, int flags);
void wordfree(wordexp_t* result);
