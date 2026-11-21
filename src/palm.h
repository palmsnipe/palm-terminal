#ifndef PALM_TERMINAL_PALM_H
#define PALM_TERMINAL_PALM_H
#include <PalmOS.h>
#include <NetMgr.h>
#include <PceNativeCall.h>
#include "native.h"

#define PT_CREATOR 'PlTm'
#define PT_STORE_NAME "Palm Terminal Data"
#define PT_SEED_NAME "Palm Terminal Seed"
#define PT_PATH_MAX 128
#define PT_LABEL_MAX 32

typedef struct {
    void *image;
    void *stack;
    void *arena;
    UInt32 arena_size;
    NativeFuncType *entry;
} PtArm;

typedef struct {
    BitmapType *bitmap;
    BitmapTypeV3 *display;
    void *bits;
    UInt16 stride;
} PtBitmap;

typedef struct {
    Char host[PT_HOST_MAX];
    Char user[PT_LOGIN_MAX];
    Char port[6];
    UInt32 key;
} PtProfile;

typedef struct {
    UInt16 ref;
    NetSocketRef socket;
    UInt16 state;
    UInt16 pending, sent, received, fed;
    Boolean owned, eof;
    Err error;
    UInt32 deadline;
    uint8_t rx[PT_IO_CHUNK], tx[PT_IO_CHUNK];
} PtNet;

enum { PT_NET_WORK, PT_NET_READY, PT_NET_END, PT_NET_FAIL };
enum { PT_RECORD_PROFILE = 1, PT_RECORD_PIN, PT_RECORD_KEY };

Boolean pt_arm_open(PtArm *arm, Boolean large_display);
void pt_arm_close(PtArm *arm);
Int32 pt_call(const PtArm *arm, UInt32 command, void *buffer, UInt32 size, UInt32 value);
void pt_wipe(void *p, size_t size);

Boolean pt_bitmap_open(PtBitmap *bitmap, UInt16 width, UInt16 height);
void pt_bitmap_close(PtBitmap *bitmap);
void pt_settings_load(PtAppearance *appearance);
Boolean pt_settings_save(const PtAppearance *appearance);
void pt_settings_draw(const PtArm *arm, const PtAppearance *appearance, PtBitmap *preview);
Boolean pt_settings_change(PtAppearance *appearance, const EventType *event);
void pt_paint_rgb(Coord x, Coord y, Coord width, Coord height, const uint8_t rgb[3]);

DmOpenRef pt_store_open(void);
Boolean pt_record_read(DmOpenRef db, UInt32 id, UInt8 type, void *out, UInt16 length);
UInt32 pt_record_find(DmOpenRef db, UInt8 type, UInt16 *index, void *head, UInt16 length);
UInt32 pt_record_save(DmOpenRef db, UInt8 type, const void *a, UInt16 len_a,
                      const void *b, UInt16 len_b);
Boolean pt_record_remove(DmOpenRef db, UInt32 id, UInt8 type);
Boolean pt_profile_read(DmOpenRef db, UInt32 id, PtProfile *profile);
UInt32 pt_profile_save(DmOpenRef db, const PtProfile *profile);
UInt32 pt_pin_find(DmOpenRef db, const PtProfile *profile, uint8_t fingerprint[32]);
Boolean pt_pin_save(DmOpenRef db, const PtProfile *profile, const uint8_t fingerprint[32]);
UInt32 pt_key_store(DmOpenRef db, const Char *label, PtKeyIO *key, UInt16 length);
Boolean pt_key_stored(DmOpenRef db, UInt32 id, Char label[PT_LABEL_MAX], Char pub[PT_PUBLIC_MAX]);
UInt16 pt_key_load(DmOpenRef db, UInt32 id, PtKeyIO *key);
Boolean pt_seed_install_stylus(PtArm *arm, PtStylusSample *samples, UInt16 count);
Boolean pt_seed_available(PtArm *arm);
Boolean pt_seed_reserve(PtArm *arm);
UInt16 pt_sd_read(const Char *path, void *out, UInt16 capacity);
Boolean pt_sd_create(const Char *path, const void *data, UInt16 length);

Boolean pt_net_start(PtNet *net);
UInt16 pt_net_poll(PtNet *net, const Char *host, UInt16 port);
UInt16 pt_net_io(PtNet *net, PtArm *arm);
void pt_net_close(PtNet *net);
#endif
