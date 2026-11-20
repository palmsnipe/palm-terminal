#ifndef PALM_TERMINAL_KEY_H
#define PALM_TERMINAL_KEY_H
#include "native.h"

typedef struct {
    uint8_t seed[32];
    uint8_t public[32];
} PtIdentity;

int pt_key_read(PtIdentity *key, PtKeyIO *io, uint32_t length);
int pt_key_step(PtIdentity *key, PtKeyIO *io);
void pt_key_cancel(void);
int pt_key_generate(PtIdentity *key);
int pt_key_save(const PtIdentity *key, PtKeyIO *io);
int pt_key_der(const PtIdentity *key, uint8_t *der, uint32_t capacity);
void pt_key_public(const PtIdentity *key, PtKeyIO *io);
#endif
