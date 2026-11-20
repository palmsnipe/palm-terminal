/* Adapted from the MIT-licensed Palm Toolchain application starter. */
#include <PalmOS.h>

UInt32 PilotMain(UInt16 command, void *parameters, UInt16 launchFlags);
UInt32 __Startup__(void);
extern const char __reloc_start_offset[];
extern const char __reloc_end_offset[];

#define LINK_CODE_BASE 0x10000000UL
#define LINK_CODE_LIMIT 0x10010000UL

UInt32 __attribute__((section(".vectors"))) __Startup__(void)
{
    UInt32 runtime;
    __asm__ volatile ("lea __Startup__(%%pc),%0" : "=a" (runtime));
    UInt32 delta = runtime - LINK_CODE_BASE;
    if (delta && MemSemaphoreReserve(true) == errNone) {
        const UInt32 *offset = (const UInt32 *)(runtime + (UInt32)__reloc_start_offset);
        const UInt32 *end = (const UInt32 *)(runtime + (UInt32)__reloc_end_offset);
        for (; offset < end; ++offset) {
            UInt32 *entry = (UInt32 *)(runtime + *offset);
            if (*entry >= LINK_CODE_BASE && *entry < LINK_CODE_LIMIT) *entry += delta;
        }
        MemSemaphoreRelease(true);
    }
    SysAppInfoPtr app;
    void *previous, *globals;
    SysAppStartup(&app, &previous, &globals);
    UInt32 result = PilotMain(app->cmd, app->cmdPBP, app->launchFlags);
    SysAppExit(app, previous, globals);
    return result;
}
