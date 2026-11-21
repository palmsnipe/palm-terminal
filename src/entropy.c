#include "native.h"
#include <string.h>
#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/hash.h>
#include <wolfssl/wolfcrypt/hmac.h>

typedef struct {
    uint8_t key[32];
    uint8_t value[32];
} Generator;

static Generator generator;
static int ready;

void pt_wipe(void *p, size_t length)
{
    volatile uint8_t *bytes = p;
    while (length--) *bytes++ = 0;
}

static int hmac(const uint8_t key[32], const uint8_t *data, uint32_t length,
                uint8_t output[32])
{
    Hmac context;
    int rc = wc_HmacInit(&context, NULL, INVALID_DEVID);
    if (!rc) rc = wc_HmacSetKey(&context, WC_SHA256, key, 32);
    if (!rc) rc = wc_HmacUpdate(&context, data, length);
    if (!rc) rc = wc_HmacFinal(&context, output);
    wc_HmacFree(&context);
    pt_wipe(&context, sizeof(context));
    return rc;
}

/* HMAC_DRBG Update as specified in SP 800-90A. */
static int update(Generator *g, const uint8_t *provided, size_t length)
{
    uint8_t input[33 + PT_SEED_SIZE];
    int rc = 0;
    if (length) memcpy(input + 33, provided, length);
    for (unsigned step = 0; !rc && step < (length ? 2u : 1u); step++) {
        memcpy(input, g->value, 32);
        input[32] = (uint8_t)step;
        rc = hmac(g->key, input, (uint32_t)(33 + length), g->key);
        if (!rc) rc = hmac(g->key, g->value, 32, g->value);
    }
    pt_wipe(input, sizeof(input));
    return rc;
}

static int generate(Generator *g, uint8_t *out, uint32_t length)
{
    int rc = 0;
    for (uint32_t offset = 0; !rc && offset < length;) {
        rc = hmac(g->key, g->value, 32, g->value);
        uint32_t count = length - offset < 32 ? length - offset : 32;
        if (!rc) memcpy(out + offset, g->value, count);
        offset += count;
    }
    if (!rc) rc = update(g, NULL, 0);
    if (rc) pt_wipe(out, length);
    return rc;
}

static int initialize(Generator *g, const uint8_t seed[PT_SEED_SIZE])
{
    memset(g->key, 0, 32);
    memset(g->value, 1, 32);
    return update(g, seed, PT_SEED_SIZE);
}

int pt_seed_init(const uint8_t seed[PT_SEED_SIZE], uint8_t state[PT_SEED_STATE])
{
    Generator g;
    int rc = initialize(&g, seed);
    if (!rc) {
        memcpy(state, &g, sizeof(g));
        /* Keep the reserved bytes covered by the PTS1 checksum for upgrades. */
        memset(state + 64, 0, 32);
        rc = wc_Sha256Hash(state, 96, state + 96);
    }
    pt_wipe(&g, sizeof(g));
    return rc;
}

int pt_seed_check(const uint8_t state[PT_SEED_STATE])
{
    uint8_t checksum[32];
    if (wc_Sha256Hash(state, 96, checksum)) return PT_ERROR;
    return memcmp(checksum, state + 96, sizeof(checksum)) ? PT_NO_RANDOM : PT_OK;
}

int pt_seed_mix(const uint8_t seed[PT_SEED_SIZE], uint8_t state[PT_SEED_STATE])
{
    int rc = pt_seed_check(state);
    if (rc) return rc;
    Generator g;
    memcpy(&g, state, sizeof(g));
    rc = update(&g, seed, PT_SEED_SIZE);
    if (!rc) memcpy(state, &g, sizeof(g));
    if (!rc) rc = wc_Sha256Hash(state, 96, state + 96);
    pt_wipe(&g, sizeof(g));
    return rc ? PT_ERROR : PT_OK;
}

/* Like PalmTLS, reject malformed or stationary captures. This validates
 * the input shape and does not measure the digitizer's physical entropy. */
int pt_stylus_condition(PtStylusSample *samples, uint32_t count)
{
    const PtStylusSample *previous = NULL;
    unsigned points = 0;
    if (!samples || count <= PT_STYLUS_POINTS || count > PT_STYLUS_MAX_EVENTS) return PT_NO_RANDOM;
    for (uint32_t i = 0; i < count; i++) {
        const PtStylusSample *sample = &samples[i];
        switch (sample->phase) {
        case PT_STYLUS_DOWN:
            if (previous) return PT_NO_RANDOM;
            previous = sample;
            break;
        case PT_STYLUS_MOVE:
            if (!previous) return PT_NO_RANDOM;
            if (memcmp(sample->tick, previous->tick, sizeof(sample->tick)) &&
                memcmp(sample->raw, previous->raw, sizeof(sample->raw))) {
                points++;
                previous = sample;
            }
            break;
        case PT_STYLUS_UP:
            if (!previous) return PT_NO_RANDOM;
            previous = NULL;
            break;
        default:
            return PT_NO_RANDOM;
        }
    }
    if (previous || points < PT_STYLUS_POINTS) return PT_NO_RANDOM;
    uint8_t digest[WC_SHA512_DIGEST_SIZE];
    int rc = wc_Sha512Hash((const uint8_t *)samples, count * sizeof(*samples), digest);
    if (!rc) memcpy(samples, digest, PT_SEED_SIZE);
    pt_wipe(digest, sizeof(digest));
    return rc ? PT_ERROR : PT_OK;
}

int pt_seed_next(const uint8_t state[PT_SEED_STATE], PtSeedIO *next)
{
    Generator g;
    int rc = pt_seed_check(state);
    memcpy(&g, state, sizeof(g));
    if (!rc) rc = generate(&g, next->working, sizeof(next->working));
    if (!rc) {
        memcpy(next->next, &g, sizeof(g));
        memcpy(next->next + 64, state + 64, 32);
        rc = wc_Sha256Hash(next->next, 96, next->next + 96);
    }
    if (rc) pt_wipe(next, sizeof(*next));
    pt_wipe(&g, sizeof(g));
    return rc;
}

int pt_seed_use(const uint8_t seed[PT_SEED_SIZE])
{
    ready = initialize(&generator, seed) == 0;
    if (!ready) pt_seed_clear();
    return ready ? 0 : -1;
}

void pt_seed_clear(void)
{
    pt_wipe(&generator, sizeof(generator));
    ready = 0;
}

int pt_random(unsigned char *output, uint32_t length)
{
    if (!ready || generate(&generator, output, length)) {
        pt_wipe(output, length);
        pt_seed_clear();
        return -1;
    }
    return 0;
}
