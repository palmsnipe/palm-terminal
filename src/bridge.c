#include "palm.h"

#define ARM_STACK_BYTES (40UL * 1024)
#define ARM_ARENA_BYTES (448UL * 1024)

extern SysAppInfoPtr SysGetAppInfo(SysAppInfoPtr *, SysAppInfoPtr *)
    SYS_TRAP(sysTrapSysGetAppInfo);

static void *raw_allocate(UInt32 size)
{
    if (size < 0x10000UL) return MemPtrNew(size);
    SysAppInfoPtr ui, action;
    SysAppInfoPtr info = SysGetAppInfo(&ui, &action);
    if (!info) return NULL;
    return MemChunkNew(0, size, (UInt16)(info->memOwnerID |
                       memNewChunkFlagNonMovable | 0x1000));
}

static uint8_t *align8(void *p)
{
    return (uint8_t *)(((UInt32)p + 7) & ~7UL);
}

static UInt32 be32(const uint8_t *p)
{
    return (UInt32)p[0] << 24 | (UInt32)p[1] << 16 | (UInt32)p[2] << 8 | p[3];
}

Int32 pt_call(const PtArm *arm, UInt32 command, void *buffer, UInt32 size, UInt32 value)
{
    if (!arm->entry) return PT_ERROR;
    uint8_t packet[20];
    pt_put_le32(packet, (UInt32)(align8(arm->stack) + ARM_STACK_BYTES));
    pt_put_le32(packet + 4, command);
    pt_put_le32(packet + 8, (UInt32)buffer);
    pt_put_le32(packet + 12, size);
    pt_put_le32(packet + 16, value);
    Int32 rc = (Int32)PceNativeCall(arm->entry, packet);
    if ((command >= PT_KEY_IMPORT && command <= PT_STOP) || command == PT_KEY_STEP ||
        (command == PT_STEP && rc != PT_IDLE && rc != PT_PROGRESS))
        MemSet(align8(arm->stack), ARM_STACK_BYTES, 0);
    return rc;
}

static Boolean load_image(PtArm *arm, const uint8_t *meta, UInt32 meta_length)
{
    if (meta_length < 12) return false;
    UInt32 length = be32(meta), parts = be32(meta + 4), relocations = be32(meta + 8);
    if (length < 4 || length > 1024UL * 1024 || !parts || parts > 18 ||
        (meta_length - 12) / 4 != relocations || meta_length % 4) return false;
    arm->image = raw_allocate(length + 7);
    if (!arm->image) return false;
    uint8_t *image = align8(arm->image);
    UInt32 used = 0;
    for (UInt16 i = 1; i <= parts; i++) {
        MemHandle h = DmGetResource('armc', i);
        if (!h) return false;
        UInt32 chunk = MemHandleSize(h);
        const void *data = MemHandleLock(h);
        Boolean good = data && chunk && chunk <= length - used;
        if (good) MemMove(image + used, data, chunk);
        if (data) MemHandleUnlock(h);
        DmReleaseResource(h);
        if (!good) return false;
        used += chunk;
    }
    if (used != length) return false;
    for (UInt32 i = 0; i < relocations; i++) {
        UInt32 offset = be32(meta + 12 + 4 * i);
        if ((offset & 3) || offset > length - 4) return false;
        uint8_t *address = image + offset;
        UInt32 target = pt_le32(address);
        if ((target & ~1UL) >= length) return false;
        pt_put_le32(address, (UInt32)image + target);
    }
    arm->entry = (NativeFuncType *)image;
    return true;
}

Boolean pt_arm_open(PtArm *arm)
{
    MemHandle h = DmGetResource('armc', 0);
    if (!h) return false;
    const uint8_t *meta = MemHandleLock(h);
    Boolean ok = meta && load_image(arm, meta, MemHandleSize(h));
    if (meta) MemHandleUnlock(h);
    DmReleaseResource(h);
    if (ok) {
        arm->stack = raw_allocate(ARM_STACK_BYTES + 7);
        arm->arena = raw_allocate(ARM_ARENA_BYTES + 7);
        ok = arm->stack && arm->arena;
    }
    if (ok) ok = pt_call(arm, PT_BOOT, align8(arm->arena), ARM_ARENA_BYTES, 0) == PT_OK;
    if (!ok) pt_arm_close(arm);
    return ok;
}

void pt_arm_close(PtArm *arm)
{
    if (arm->image) MemPtrFree(arm->image);
    if (arm->stack) {
        pt_wipe(arm->stack, ARM_STACK_BYTES + 7);
        MemPtrFree(arm->stack);
    }
    if (arm->arena) {
        pt_wipe(arm->arena, ARM_ARENA_BYTES + 7);
        MemPtrFree(arm->arena);
    }
    MemSet(arm, sizeof(*arm), 0);
}

void pt_wipe(void *p, size_t size)
{
    volatile uint8_t *bytes = p;
    while (size--) *bytes++ = 0;
}
