#ifndef PALM_TERMINAL_STYLUS_H
#define PALM_TERMINAL_STYLUS_H
#include "palm.h"

typedef struct {
    PtStylusSample *samples;
    void *code;
    NativeFuncType *read_pen;
    UInt16 count, points, stroke_start;
    Coord x, y;
    Boolean down, poll;
} PtStylus;

void pt_stylus_open(PtStylus *pad);
void pt_stylus_close(PtStylus *pad);
void pt_stylus_draw(const PtStylus *pad);
Boolean pt_stylus_event(PtStylus *pad, const EventType *event);
#endif
