/* Adapted from PalmTLS stylus capture, copyright (C) 2026 Cyril Morales.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "stylus.h"
#include "resource.h"

static const RectangleType pad_bounds = {{5, 42}, {150, 70}};

void pt_stylus_close(PtStylus *pad)
{
    if (pad->samples) {
        pt_wipe(pad->samples, PT_STYLUS_MAX_EVENTS * sizeof(*pad->samples));
        MemPtrFree(pad->samples);
    }
    if (pad->code) MemPtrFree(pad->code);
    MemSet(pad, sizeof(*pad), 0);
}

void pt_stylus_open(PtStylus *pad)
{
    pt_stylus_close(pad);
    UInt32 rom, device, processor;
    if (FtrGet(sysFtrCreator, sysFtrNumROMVersion, &rom) || (rom >> 24) != 5 ||
        FtrGet(sysFtrCreator, sysFtrNumProcessorID, &processor) ||
        !sysFtrNumProcessorIsARM(processor) ||
        FtrGet(sysFtrCreator, sysFtrNumOEMDeviceID, &device) ||
        (device != 'Zir4' && device != 'D050' && device != 'Zi72')) return;

    MemHandle handle = DmGetResource('pPen', RawPenResource);
    if (!handle) return;
    const void *source = MemHandleLock(handle);
    UInt32 size = MemHandleSize(handle);
    if (source && size) {
        pad->code = MemPtrNew(size + 3);
        if (pad->code) {
            void *entry = (void *)(((UInt32)pad->code + 3) & ~3UL);
            MemMove(entry, source, size);
            pad->read_pen = entry;
        }
    }
    if (source) MemHandleUnlock(handle);
    DmReleaseResource(handle);
    if (pad->read_pen) pad->samples = MemPtrNew(PT_STYLUS_MAX_EVENTS * sizeof(*pad->samples));
    if (!pad->samples) pt_stylus_close(pad);
    pad->poll = device != 'Zir4';
}

static void progress(const PtStylus *pad)
{
    FormType *form = FrmGetActiveForm();
    RectangleType status = {{4, 116}, {152, 23}};
    RectangleType bar = {{5, 117}, {150, 6}};
    Boolean done = pad->points == PT_STYLUS_POINTS;
    WinEraseRectangle(&status, 0);
    const Char *hint;
    if (pad->samples) {
        WinDrawRectangleFrame(simpleFrame, &bar);
        bar.extent.x = (UInt32)pad->points * 150 / PT_STYLUS_POINTS;
        if (bar.extent.x) WinDrawRectangle(&bar, 0);
        hint = done ? (pad->down ? "Lift the stylus." : "Ready to use seed.") : "Change speed and direction.";
    } else hint = "Stylus input is unavailable.";
    WinDrawChars(hint, StrLen(hint), 5, 127);
    CtlSetEnabled(FrmGetObjectPtr(form, FrmGetObjectIndex(form, UseStylusSeed)), done && !pad->down);
}

void pt_stylus_draw(const PtStylus *pad)
{
    FrmDrawForm(FrmGetActiveForm());
    static const Char instructions[] = "Draw freely in the box.";
    if (pad->samples) {
        WinDrawChars(instructions, sizeof(instructions) - 1, 5, 22);
        WinDrawRectangleFrame(simpleFrame, &pad_bounds);
    }
    progress(pad);
}

static void move_pen(PtStylus *pad, Coord x, Coord y, UInt32 tick)
{
    Boolean inside = RctPtInRectangle(x, y, &pad_bounds);
    if (inside && RctPtInRectangle(pad->x, pad->y, &pad_bounds) && (x != pad->x || y != pad->y))
        WinDrawLine(pad->x, pad->y, x, y);
    pad->x = x;
    pad->y = y;

    if (!inside || pad->points == PT_STYLUS_POINTS) return;
    const PtStylusSample *previous = pad->count > pad->stroke_start ? &pad->samples[pad->count - 1] : NULL;
    if (previous && tick == pt_le32(previous->tick)) return;
    PtStylusSample sample = {.phase = previous ? PT_STYLUS_MOVE : PT_STYLUS_DOWN};
    if (PceNativeCall(pad->read_pen, sample.raw) != errNone ||
        (previous && !MemCmp(sample.raw, previous->raw, sizeof(sample.raw)))) return;
    pt_put_le32(sample.tick, tick);
    pad->samples[pad->count++] = sample;
    if (previous && ++pad->points % 4 == 0) progress(pad);
}

Boolean pt_stylus_event(PtStylus *pad, const EventType *event)
{
    if (!pad->samples) return false;
    switch (event->eType) {
    case penDownEvent: {
        if (pad->down || pad->points == PT_STYLUS_POINTS ||
            !RctPtInRectangle(event->screenX, event->screenY, &pad_bounds)) return false;
        pad->down = true;
        pad->x = event->screenX;
        pad->y = event->screenY;
        pad->stroke_start = pad->count;
        move_pen(pad, event->screenX, event->screenY, TimGetTicks());
        return true;
    }
    case penMoveEvent:
        if (!pad->down) return false;
        move_pen(pad, event->screenX, event->screenY, TimGetTicks());
        return true;
    case nilEvent: {
        if (!pad->down || !pad->poll) return false;
        Int16 x, y;
        Boolean down;
        EvtGetPen(&x, &y, &down);
        if (down) move_pen(pad, x, y, TimGetTicks());
        return true;
    }
    case penUpEvent: {
        if (!pad->down) return false;
        pad->down = false;
        if (pad->count <= pad->stroke_start + 1) {
            if (pad->count > pad->stroke_start)
                pt_wipe(&pad->samples[--pad->count], sizeof(*pad->samples));
            return true;
        }
        PtStylusSample sample = {.phase = PT_STYLUS_UP};
        pt_put_le32(sample.tick, TimGetTicks());
        pad->samples[pad->count++] = sample;
        progress(pad);
        return true;
    }
    default:
        return false;
    }
}
