#ifndef PALM_TERMINAL_APPEARANCE_H
#define PALM_TERMINAL_APPEARANCE_H
#include <stdint.h>

#define PT_DISPLAY_LONG 480
#define PT_DISPLAY_SHORT 320
#define PT_PREVIEW_WIDTH 288
#define PT_PREVIEW_HEIGHT 39
#define PT_GRID_COLS 96
#define PT_GRID_ROWS 60
#define PT_GRID_CELLS 3840

enum { PT_FONT_NORMAL, PT_FONT_SMALL, PT_FONT_LARGE, PT_FONT_COUNT };
enum { PT_CURSOR_BLOCK, PT_CURSOR_UNDERLINE, PT_CURSOR_BAR, PT_CURSOR_HIDDEN, PT_CURSOR_COUNT };
enum { PT_HISTORY_OFF, PT_HISTORY_48, PT_HISTORY_96, PT_HISTORY_192, PT_HISTORY_COUNT };
enum {
    PT_CURSOR_AUTO = 1, PT_CURSOR_BLINK = 2, PT_FOLLOW_OUTPUT = 4,
    PT_AUDIBLE_BELL = 8, PT_BRIGHT_BOLD = 16,
    PT_STYLE_FLAGS = PT_CURSOR_AUTO | PT_CURSOR_BLINK | PT_FOLLOW_OUTPUT |
                     PT_AUDIBLE_BELL | PT_BRIGHT_BOLD
};

/* Byte fields are shared by 68K and ARM and saved as Palm preferences. */
typedef struct {
    uint8_t font, shape, history, flags;
    uint8_t text[3], background[3], cursor[3];
} PtAppearance;

static inline PtAppearance pt_appearance_default(void)
{
    return (PtAppearance){
        .font = PT_FONT_NORMAL, .shape = PT_CURSOR_BLOCK, .history = PT_HISTORY_96,
        .flags = PT_CURSOR_AUTO, .text = {232, 232, 232}, .cursor = {232, 232, 232}
    };
}

static inline int pt_appearance_valid(const PtAppearance *a)
{
    return a->font < PT_FONT_COUNT && a->shape < PT_CURSOR_COUNT &&
           a->history < PT_HISTORY_COUNT && !(a->flags & ~PT_STYLE_FLAGS);
}

static inline unsigned pt_font_width(unsigned font)
{
    return font == PT_FONT_SMALL ? 5 : font == PT_FONT_LARGE ? 8 : 6;
}

static inline unsigned pt_font_height(unsigned font)
{
    return font == PT_FONT_SMALL ? 8 : font == PT_FONT_LARGE ? 13 : 10;
}

typedef struct {
    uint8_t rows, cols;
} PtGrid;

static inline PtGrid pt_grid(const PtAppearance *a, unsigned width, unsigned height)
{
    return (PtGrid){height / pt_font_height(a->font), width / pt_font_width(a->font)};
}

static inline int pt_grid_valid(unsigned rows, unsigned cols)
{
    return rows && rows <= PT_GRID_ROWS && cols && cols <= PT_GRID_COLS &&
           rows * cols <= PT_GRID_CELLS;
}

static inline unsigned pt_history_limit(unsigned option)
{
    return option ? 48u << (option - 1) : 0;
}
#endif
