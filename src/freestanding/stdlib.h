#ifndef PT_ARM_STDLIB_H
#define PT_ARM_STDLIB_H
#include <stddef.h>
void *malloc(size_t);
void *calloc(size_t, size_t);
void *realloc(void *, size_t);
void free(void *);
int abs(int);
long strtol(const char *, char **, int);
void abort(void);
void exit(int);
#endif
