#ifndef PT_BCRYPT_INCLUDES_H
#define PT_BCRYPT_INCLUDES_H
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include "native.h"
#define HAVE_STDLIB_H
#define HAVE_BLF_H
#define explicit_bzero pt_wipe
/* The upstream error path overwrites with random bytes. This port clears it
   instead, and callers always check the failure result. */
#define arc4random_buf pt_wipe
#define freezero pt_freezero
static inline void pt_freezero(void *ptr, size_t len)
{
    if (ptr) pt_wipe(ptr, len);
    free(ptr);
}
#endif
