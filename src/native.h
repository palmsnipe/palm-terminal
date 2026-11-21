#ifndef PALM_TERMINAL_NATIVE_H
#define PALM_TERMINAL_NATIVE_H
#include <stddef.h>
#include <stdint.h>
#include "appearance.h"

#define PT_SEED_SIZE 48
#define PT_SEED_STATE 128
#define PT_KEY_FILE_MAX 8192
#define PT_KEY_SAVE_MAX 665
#define PT_PUBLIC_MAX 100
#define PT_PASSWORD_MAX 128
#define PT_LOGIN_MAX 64
#define PT_HOST_MAX 96
#define PT_IO_CHUNK 4096
#define PT_CELL_BYTES 8
#define PT_CTRL 4
#define PT_CELL_CODE_MASK 0x1fffffUL

enum PtCommand {
    PT_BOOT = 1, PT_IMPORT_SEED, PT_NEXT_SEED, PT_USE_SEED, PT_FORGET_SEED,
    PT_KEY_IMPORT, PT_KEY_GENERATE, PT_KEY_UNLOCK, PT_KEY_SAVE, PT_CLEAR_KEY,
    PT_CONNECT, PT_STOP, PT_FEED, PT_DRAIN, PT_STEP, PT_RESIZE, PT_INPUT,
    PT_DAMAGE, PT_ROW, PT_SCROLL, PT_KEY_STEP, PT_STATS, PT_RASTER_ROW,
    PT_APPEARANCE, PT_CURSOR_PHASE, PT_RASTER_PREVIEW, PT_LIVE
};
enum PtResult {
    PT_OK = 0, PT_PROGRESS = 1, PT_CONNECTED = 2, PT_CLOSED = 3,
    PT_UNKNOWN_HOST = 4, PT_CHANGED_HOST = 5, PT_IDLE = 6,
    PT_ERROR = -1, PT_BAD_KEY = -2, PT_NO_RANDOM = -3, PT_NO_MEMORY = -4
};
enum { PT_HOST_KEY_UNKNOWN, PT_HOST_KEY_ED25519, PT_HOST_KEY_P256 };
enum PtKey {
    PT_UP = 0x110000, PT_DOWN, PT_LEFT, PT_RIGHT, PT_ENTER, PT_BACKSPACE,
    PT_TAB, PT_ESC, PT_DELETE, PT_HOME, PT_END, PT_PAGEUP, PT_PAGEDOWN
};

/* These structures contain byte arrays only. Both CPUs share their layout. */
typedef struct {
    char user[PT_LOGIN_MAX];
    char password[PT_PASSWORD_MAX];
    uint8_t fingerprint[32];
    uint8_t pinned;
    uint8_t key_auth;
    PtAppearance appearance;
    PtGrid grid;
} PtLogin;

typedef struct {
    uint8_t rows[8];
    uint8_t bell;
    uint8_t history_offset;
} PtFrame;

typedef struct {
    PtAppearance appearance;
    uint8_t pixels[4];
} PtPreview;

typedef struct {
    uint8_t file[PT_KEY_FILE_MAX];
    char password[PT_PASSWORD_MAX];
    char public_key[PT_PUBLIC_MAX];
} PtKeyIO;

typedef struct {
    uint8_t next[PT_SEED_STATE];
    uint8_t working[PT_SEED_SIZE];
} PtSeedIO;

static inline uint32_t pt_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static inline void pt_put_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

int32_t pt_dispatch(uint32_t command, void *buffer, uint32_t size, uint32_t value);
void pt_wipe(void *p, size_t length);
int pt_seed_import(const uint8_t seed[PT_SEED_SIZE], uint8_t state[PT_SEED_STATE]);
int pt_seed_next(const uint8_t state[PT_SEED_STATE], PtSeedIO *next);
int pt_seed_use(const uint8_t seed[PT_SEED_SIZE]);
void pt_seed_clear(void);
int pt_random(unsigned char *output, uint32_t length);
#endif
