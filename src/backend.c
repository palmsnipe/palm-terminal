#include "key.h"
#include <stdlib.h>
#include <string.h>
#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/hash.h>
#include <wolfssh/ssh.h>
#include <wolfssh/error.h>
#include <vterm.h>
#ifdef PT_ARMLET
#include "terminal-font.h"
#endif

#define NET_BUFFER (2 * PT_IO_CHUNK)
#define HISTORY_MAX_ROWS 192
#define AUTH_KEY_TYPE "ssh-ed25519"

typedef struct {
    unsigned cols, capacity;
    uint8_t cells[];
} HistoryRow;

typedef struct {
    uint32_t count;
    uint8_t bytes[NET_BUFFER];
} Queue;

typedef struct {
    WOLFSSH_CTX *context;
    WOLFSSH *ssh;
    VTerm *vterm;
    VTermScreen *screen;
    Queue incoming, outgoing, typed;
    PtLogin login;
    PtIdentity identity;
    uint8_t der[96];
    uint32_t der_length;
    uint8_t public_blob[51];
    uint8_t fingerprint[32];
    PtAppearance appearance;
    uint32_t dirty[2];
    HistoryRow *history[HISTORY_MAX_ROWS];
    unsigned history_limit, history_next, history_count, history_offset;
    unsigned cols, rows;
    VTermPos cursor;
    int cursor_phase, bell_pending, alt_screen;
    int cursor_visible, ready, opened, identity_loaded, host_status, host_verified, resize_pending;
    uint32_t key_command;
} Backend;

static Backend session;

static uint32_t append(Queue *q, const void *p, uint32_t n)
{
    uint32_t room = sizeof(q->bytes) - q->count;
    if (n > room) n = room;
    memcpy(q->bytes + q->count, p, n);
    q->count += n;
    return n;
}

static void remove_bytes(Queue *q, uint32_t count)
{
    q->count -= count;
    memmove(q->bytes, q->bytes + count, q->count);
}

static uint32_t take(Queue *q, void *p, uint32_t n)
{
    if (n > q->count) n = q->count;
    memcpy(p, q->bytes, n);
    remove_bytes(q, n);
    return n;
}

static int receive(WOLFSSH *ssh, void *buffer, word32 length, void *user)
{
    Backend *s = user;
    (void)ssh;
    return s->incoming.count ? (int)take(&s->incoming, buffer, length) : WS_CBIO_ERR_WANT_READ;
}

static int transmit(WOLFSSH *ssh, void *buffer, word32 length, void *user)
{
    Backend *s = user;
    (void)ssh;
    uint32_t count = append(&s->outgoing, buffer, length);
    return count ? (int)count : WS_CBIO_ERR_WANT_WRITE;
}

static int authenticate(byte type, WS_UserAuthData *data, void *user)
{
    Backend *s = user;
    if (!s->host_verified || s->host_status != PT_OK) return WOLFSSH_USERAUTH_REJECTED;
    if (type == WOLFSSH_USERAUTH_PASSWORD && !s->login.key_auth) {
        data->sf.password.password = (const byte *)s->login.password;
        data->sf.password.passwordSz = (word32)strlen(s->login.password);
        return WOLFSSH_USERAUTH_SUCCESS;
    }
    if (type == WOLFSSH_USERAUTH_PUBLICKEY && s->login.key_auth && s->der_length) {
        WS_UserAuthData_PublicKey *pub = &data->sf.publicKey;
        pub->publicKeyType = (const byte *)AUTH_KEY_TYPE;
        pub->publicKeyTypeSz = sizeof(AUTH_KEY_TYPE) - 1;
        pub->privateKey = s->der;
        pub->privateKeySz = s->der_length;
        pub->publicKey = s->public_blob;
        pub->publicKeySz = sizeof(s->public_blob);
        return WOLFSSH_USERAUTH_SUCCESS;
    }
    return WOLFSSH_USERAUTH_INVALID_AUTHTYPE;
}

static int verify_host(const byte *key, word32 size, void *user)
{
    Backend *s = user;
    if (wc_Sha256Hash(key, size, s->fingerprint)) {
        s->host_status = PT_ERROR;
        return -1;
    }
    if (!s->login.pinned) {
        s->host_status = PT_UNKNOWN_HOST;
        return -1;
    }
    if (memcmp(s->fingerprint, s->login.fingerprint, sizeof(s->fingerprint))) {
        s->host_status = PT_CHANGED_HOST;
        return -1;
    }
    s->host_verified = 1;
    return 0;
}

void pt_terminal_dimensions(word32 *cols, word32 *rows, word32 *width, word32 *height)
{
    *cols = session.cols;
    *rows = session.rows;
    *width = session.cols * pt_font_width(session.appearance.font);
    *height = session.rows * pt_font_height(session.appearance.font);
}

static void damage_row(Backend *s, int row)
{
    if (row >= 0 && row < (int)s->rows)
        s->dirty[(unsigned)row / 32] |= 1u << ((unsigned)row % 32);
}

static void damage_all(Backend *s)
{
    s->dirty[0] = s->dirty[1] = UINT32_MAX;
}

static int damage(VTermRect rect, void *user)
{
    Backend *s = user;
    if (s->history_offset) return 1;
    for (int row = rect.start_row; row < rect.end_row && row < (int)s->rows; row++)
        damage_row(s, row);
    return 1;
}

static int cursor(VTermPos pos, VTermPos old, int visible, void *user)
{
    Backend *s = user;
    s->cursor = pos;
    s->cursor_visible = visible;
    if (!s->history_offset) {
        damage_row(s, old.row);
        damage_row(s, pos.row);
    }
    return 1;
}

static int term_property(VTermProp prop, VTermValue *value, void *user)
{
    Backend *s = user;
    if (prop == VTERM_PROP_CURSORVISIBLE)
        return cursor(s->cursor, s->cursor, value->boolean, s);
    if (prop == VTERM_PROP_ALTSCREEN) {
        s->alt_screen = value->boolean;
        s->history_offset = 0;
        damage_all(s);
    }
    return 1;
}

static int bell(void *user)
{
    Backend *s = user;
    s->bell_pending = (s->appearance.flags & PT_AUDIBLE_BELL) != 0;
    return 1;
}

static uint16_t rgb565(unsigned red, unsigned green, unsigned blue)
{
    return (uint16_t)((red >> 3) << 11 | (green >> 2) << 5 | blue >> 3);
}

static uint16_t cell_color(VTermScreen *screen, VTermColor color)
{
    vterm_screen_convert_color_to_rgb(screen, &color);
    return rgb565(color.rgb.red, color.rgb.green, color.rgb.blue);
}

/* Keep default-color and reverse attributes in history so old lines
 * adopt the current theme. Explicit ANSI colors are stored as RGB565. */
#define CELL_DEFAULT_FG (UINT32_C(1) << 21)
#define CELL_DEFAULT_BG (UINT32_C(1) << 22)
#define CELL_REVERSE    (UINT32_C(1) << 23)

static void encode_cell(Backend *s, const VTermScreenCell *cell, uint8_t *out)
{
    uint32_t code = cell->chars[0] == UINT32_MAX ? 0 : cell->chars[0];
    if (VTERM_COLOR_IS_DEFAULT_FG(&cell->fg)) code |= CELL_DEFAULT_FG;
    if (VTERM_COLOR_IS_DEFAULT_BG(&cell->bg)) code |= CELL_DEFAULT_BG;
    if (cell->attrs.reverse) code |= CELL_REVERSE;
    pt_put_le32(out, code);
    uint16_t fg = code & CELL_DEFAULT_FG ? 0 : cell_color(s->screen, cell->fg);
    uint16_t bg = code & CELL_DEFAULT_BG ? 0 : cell_color(s->screen, cell->bg);
    out[4] = (uint8_t)(fg >> 8);
    out[5] = (uint8_t)fg;
    out[6] = (uint8_t)(bg >> 8);
    out[7] = (uint8_t)bg;
}

static void empty_cell(uint8_t *out)
{
    pt_put_le32(out, CELL_DEFAULT_FG | CELL_DEFAULT_BG);
    memset(out + 4, 0, 4);
}

static HistoryRow *take_oldest_history(Backend *s)
{
    unsigned slot = (s->history_next + HISTORY_MAX_ROWS - s->history_count) % HISTORY_MAX_ROWS;
    HistoryRow *row = s->history[slot];
    s->history[slot] = NULL;
    s->history_count--;
    return row;
}

static void trim_history(Backend *s, unsigned limit)
{
    while (s->history_count > limit) free(take_oldest_history(s));
    if (s->history_offset > s->history_count) {
        s->history_offset = s->history_count;
        damage_all(s);
    }
}

static int push_history(int cols, const VTermScreenCell *cells, void *user)
{
    Backend *s = user;
    if (!s->history_limit) return 1;
    unsigned width = (unsigned)cols;
    size_t bytes = sizeof(HistoryRow) + width * PT_CELL_BYTES;
    HistoryRow *row = s->history_count < s->history_limit ? malloc(bytes) : NULL;
    if (row) {
        row->capacity = width;
    } else if (s->history_count) {
        row = take_oldest_history(s);
        if (row->capacity < width) {
            HistoryRow *wider = realloc(row, bytes);
            if (wider) {
                row = wider;
                row->capacity = width;
            }
        }
    }
    if (!row) return 1;
    s->history[s->history_next] = row;
    s->history_next = (s->history_next + 1) % HISTORY_MAX_ROWS;
    s->history_count++;
    row->cols = width < row->capacity ? width : row->capacity;
    for (unsigned col = 0; col < row->cols; col++)
        encode_cell(s, &cells[col], row->cells + col * PT_CELL_BYTES);
    if (s->history_offset && s->history_offset < s->history_count) s->history_offset++;
    else if (s->history_offset) damage_all(s);
    return 1;
}

static int clear_history(void *user)
{
    Backend *s = user;
    trim_history(s, 0);
    s->history_next = 0;
    damage_all(s);
    return 1;
}

static const VTermScreenCallbacks screen_callbacks = {
    .damage = damage,
    .movecursor = cursor,
    .settermprop = term_property,
    .bell = bell,
    .sb_pushline = push_history,
    .sb_clear = clear_history,
};

static void terminal_output(const char *data, size_t count, void *user)
{
    Backend *s = user;
    append(&s->typed, data, (uint32_t)count);
}

static void *terminal_malloc(size_t size, void *user)
{
    Backend *s = user;
    void *buf = calloc(1, size);
    /* libvterm needs room for the old and new grids during a resize. Give
     * those grids priority over older lines when the native heap is tight. */
    while (!buf && s->history_count) {
        trim_history(s, s->history_count - 1);
        buf = calloc(1, size);
    }
    if (!buf) abort();
    return buf;
}

static void terminal_free(void *p, void *user)
{
    (void)user;
    free(p);
}

static VTermAllocatorFunctions terminal_allocator = {
    .malloc = terminal_malloc, .free = terminal_free,
};

static void terminal_colors(Backend *s)
{
    const PtAppearance *a = &s->appearance;
    VTermColor text, background;
    vterm_color_rgb(&text, a->text[0], a->text[1], a->text[2]);
    vterm_color_rgb(&background, a->background[0], a->background[1], a->background[2]);
    vterm_screen_set_default_colors(s->screen, &text, &background);
    vterm_state_set_bold_highbright(vterm_obtain_state(s->vterm),
                                  (a->flags & PT_BRIGHT_BOLD) != 0);
}

static int terminal_open(Backend *s)
{
    s->history_limit = pt_history_limit(s->appearance.history);
    s->vterm = vterm_new_with_allocator((int)s->rows, (int)s->cols,
                                        &terminal_allocator, s);
    if (!s->vterm) return -1;
    vterm_set_utf8(s->vterm, 1);
    vterm_output_set_callback(s->vterm, terminal_output, s);
    s->screen = vterm_obtain_screen(s->vterm);
    if (!s->screen) return -1;
    terminal_colors(s);
    vterm_screen_set_callbacks(s->screen, &screen_callbacks, s);
    vterm_screen_enable_altscreen(s->screen, 1);
    vterm_screen_set_damage_merge(s->screen, VTERM_DAMAGE_ROW);
    vterm_screen_reset(s->screen, 1);
    s->cursor_phase = 1;
    damage_all(s);
    return 0;
}

static int resize_terminal(Backend *s, unsigned rows, unsigned cols)
{
    if (!s->vterm || !pt_grid_valid(rows, cols))
        return PT_ERROR;
    if (rows != s->rows || cols != s->cols) {
        s->rows = rows;
        s->cols = cols;
        vterm_set_size(s->vterm, (int)rows, (int)cols);
        vterm_state_get_cursorpos(vterm_obtain_state(s->vterm), &s->cursor);
        s->resize_pending = 1;
    }
    damage_all(s);
    return PT_OK;
}

static int set_appearance(Backend *s, const PtAppearance *appearance, unsigned rows, unsigned cols)
{
    if (!s->screen || !pt_appearance_valid(appearance) || !pt_grid_valid(rows, cols))
        return PT_ERROR;
    s->history_limit = pt_history_limit(appearance->history);
    trim_history(s, s->history_limit);
    s->appearance = *appearance;
    terminal_colors(s);
    s->cursor_phase = 1;
    if (!(s->appearance.flags & PT_AUDIBLE_BELL)) s->bell_pending = 0;
    return resize_terminal(s, rows, cols);
}

static int get_row(Backend *s, unsigned row, uint8_t *out)
{
    if (!s->screen || row >= s->rows) return PT_ERROR;
    if (row < s->history_offset) {
        unsigned slot = (s->history_next + HISTORY_MAX_ROWS - s->history_offset + row) %
                        HISTORY_MAX_ROWS;
        const HistoryRow *saved = s->history[slot];
        unsigned count = saved->cols < s->cols ? saved->cols : s->cols;
        memcpy(out, saved->cells, count * PT_CELL_BYTES);
        for (unsigned col = count; col < s->cols; col++) empty_cell(out + col * PT_CELL_BYTES);
        return PT_OK;
    }
    row -= s->history_offset;
    for (unsigned col = 0; col < s->cols; col++, out += PT_CELL_BYTES) {
        VTermPos pos = {(int)row, (int)col};
        VTermScreenCell cell = {0};
        if (vterm_screen_get_cell(s->screen, pos, &cell)) encode_cell(s, &cell, out);
        else empty_cell(out);
    }
    return PT_OK;
}

#ifdef PT_ARMLET
static const uint8_t *glyph_for(unsigned font, uint32_t code)
{
    const int line = code >= 0x2500 && code <= 0x259f;
    unsigned i = line ? code - 0x2500 : code == 0 ? 0 :
                 code >= 32 && code <= 255 ? code - 32 : '?' - 32;
    switch (font) {
    case PT_FONT_SMALL: return line ? line_5x8[i] : latin_5x8[i];
    case PT_FONT_LARGE: return line ? line_8x13[i] : latin_8x13[i];
    default: return line ? line_6x10[i] : latin_6x10[i];
    }
}

static uint16_t swap565(uint16_t color)
{
    /* Store big-endian Palm RGB565 using an ARM little-endian halfword. */
    return (uint16_t)((color >> 8) | (color << 8));
}

static uint16_t rgb(const uint8_t color[3])
{
    return rgb565(color[0], color[1], color[2]);
}

static uint16_t contrasting_color(uint16_t color)
{
    unsigned light = 2 * ((color >> 11) & 31u) + 3 * ((color >> 5) & 63u) +
                     (color & 31u);
    return light > 141 ? 0 : UINT16_MAX;
}

static inline void paint_fixed_glyph(uint16_t *dest, unsigned pitch, unsigned width,
                                    unsigned height, const uint8_t *glyph, uint16_t fg, uint16_t bg)
{
    fg = swap565(fg);
    bg = swap565(bg);
    for (unsigned y = 0; y < height; y++, dest += pitch) {
        unsigned bits = glyph[y];
        for (unsigned x = 0; x < width; x++, bits <<= 1)
            dest[x] = (bits & 0x80) ? fg : bg;
    }
}

static void paint_glyph(uint16_t *dest, unsigned pitch, unsigned font,
                        const uint8_t *glyph, uint16_t fg, uint16_t bg)
{
    /* Constant dimensions let the compiler unroll the inner pixel loop. */
    switch (font) {
    case PT_FONT_SMALL: paint_fixed_glyph(dest, pitch, 5, 8, glyph, fg, bg); break;
    case PT_FONT_LARGE: paint_fixed_glyph(dest, pitch, 8, 13, glyph, fg, bg); break;
    default: paint_fixed_glyph(dest, pitch, 6, 10, glyph, fg, bg); break;
    }
}

static void paint_cursor(uint16_t *dest, unsigned pitch, const PtAppearance *a,
                         const uint8_t *glyph, uint16_t fg, uint16_t bg)
{
    const unsigned width = pt_font_width(a->font), height = pt_font_height(a->font);
    const int automatic = (a->flags & PT_CURSOR_AUTO) != 0;
    uint16_t color = automatic ? fg : rgb(a->cursor);
    if (automatic && color == bg) color = contrasting_color(bg);
    if (a->shape == PT_CURSOR_BLOCK) {
        paint_glyph(dest, pitch, a->font, glyph,
                    automatic && fg != bg ? bg : contrasting_color(color), color);
        return;
    }
    paint_glyph(dest, pitch, a->font, glyph, fg, bg);
    color = swap565(color);
    if (a->shape == PT_CURSOR_UNDERLINE) {
        for (unsigned x = 0; x < width; x++) dest[(height - 1) * pitch + x] = color;
    } else if (a->shape == PT_CURSOR_BAR) {
        for (unsigned y = 0; y < height; y++) dest[y * pitch] = color;
    }
}

static int bitmap_valid(const void *bitmap, uint32_t stride, unsigned width)
{
    return bitmap && stride >= width * 2 && !(((uintptr_t)bitmap | stride) & 1u);
}

static int raster_row(Backend *s, unsigned row, unsigned pixels_wide, void *bitmap, uint32_t stride)
{
    const PtAppearance *a = &s->appearance;
    const unsigned width = pt_font_width(a->font), height = pt_font_height(a->font);
    unsigned used = s->cols * width;
    if (pixels_wide < used || pixels_wide > PT_DISPLAY_LONG ||
        !bitmap_valid(bitmap, stride, pixels_wide)) return PT_ERROR;
    uint8_t cells[PT_GRID_COLS * PT_CELL_BYTES];
    int rc = get_row(s, row, cells);
    if (rc != PT_OK) return rc;

    const unsigned pitch = stride / 2;
    uint16_t *pixels = bitmap;
    const uint16_t text = rgb(a->text), background = rgb(a->background);
    int cursor_col = !s->history_offset && s->cursor_visible && s->cursor_phase &&
                     a->shape != PT_CURSOR_HIDDEN && s->cursor.row == (int)row ?
                     s->cursor.col : -1;

    for (unsigned col = 0; col < s->cols; col++) {
        const uint8_t *cell = cells + col * PT_CELL_BYTES;
        const uint32_t code = pt_le32(cell);
        uint16_t fg = code & CELL_DEFAULT_FG ? text : (uint16_t)(cell[4] << 8 | cell[5]);
        uint16_t bg = code & CELL_DEFAULT_BG ? background : (uint16_t)(cell[6] << 8 | cell[7]);
        if (code & CELL_REVERSE) {
            uint16_t swap = fg;
            fg = bg;
            bg = swap;
        }
        const uint8_t *glyph = glyph_for(a->font, code & PT_CELL_CODE_MASK);
        if ((int)col == cursor_col) paint_cursor(pixels + col * width, pitch, a, glyph, fg, bg);
        else paint_glyph(pixels + col * width, pitch, a->font, glyph, fg, bg);
    }
    for (unsigned y = 0; y < height; y++)
        for (unsigned x = used; x < pixels_wide; x++)
            pixels[y * pitch + x] = swap565(background);
    return PT_OK;
}

static int raster_preview(const PtPreview *preview, uint32_t stride)
{
    const PtAppearance *a = &preview->appearance;
    uint16_t *pixels = (uint16_t *)(uintptr_t)pt_le32(preview->pixels);
    if (!pt_appearance_valid(a) || !bitmap_valid(pixels, stride, PT_PREVIEW_WIDTH))
        return PT_ERROR;

    const uint16_t fg = rgb(a->text), bg = rgb(a->background);
    const unsigned width = pt_font_width(a->font), height = pt_font_height(a->font);
    const unsigned pitch = stride / 2;
    for (unsigned y = 0; y < PT_PREVIEW_HEIGHT; y++)
        for (unsigned x = 0; x < PT_PREVIEW_WIDTH; x++) pixels[y * pitch + x] = swap565(bg);

    static const char sample[] = "$ echo hello";
    pixels += ((PT_PREVIEW_HEIGHT - height) / 2) * pitch + 8;
    for (unsigned i = 0; i < sizeof(sample) - 1; i++)
        paint_glyph(pixels + i * width, pitch, a->font,
                    glyph_for(a->font, (uint8_t)sample[i]), fg, bg);
    paint_cursor(pixels + (sizeof(sample) - 1) * width, pitch, a, glyph_for(a->font, 0), fg, bg);
    return PT_OK;
}
#endif

static void clear_key(Backend *s)
{
    pt_wipe(&s->identity, sizeof(s->identity));
    pt_wipe(s->der, sizeof(s->der));
    s->der_length = 0;
    s->identity_loaded = 0;
    s->key_command = 0;
    pt_key_cancel();
}

static void disconnect(Backend *s)
{
    pt_key_cancel();
    if (s->ssh) wolfSSH_free(s->ssh);
    if (s->context) wolfSSH_CTX_free(s->context);
    if (s->vterm) vterm_free(s->vterm);
    trim_history(s, 0);
    int ready = s->ready;
    pt_wipe(s, sizeof(*s));
    s->ready = ready;
    pt_seed_clear();
}

void pt_reset_after_fault(void)
{
    pt_key_cancel();
    int ready = session.ready;
    pt_wipe(&session, sizeof(session));
    session.ready = ready;
    pt_seed_clear();
}

static int connect_start(Backend *s, const PtLogin *login)
{
    if (s->ssh || !login->user[0] || !pt_appearance_valid(&login->appearance) ||
        !pt_grid_valid(login->grid.rows, login->grid.cols)) return PT_ERROR;
    s->login = *login;
    s->appearance = login->appearance;
    s->cols = login->grid.cols;
    s->rows = login->grid.rows;
    if (login->key_auth && login->pinned) {
        if (!s->identity_loaded) return PT_BAD_KEY;
        int length = pt_key_der(&s->identity, s->der, sizeof(s->der));
        if (length <= 0) return PT_BAD_KEY;
        s->der_length = (uint32_t)length;
        static const uint8_t prefix[] = "\0\0\0\013ssh-ed25519\0\0\0\040";
        memcpy(s->public_blob, prefix, 19);
        memcpy(s->public_blob + 19, s->identity.public, 32);
        pt_wipe(&s->identity, sizeof(s->identity));
        s->identity_loaded = 0;
    }

    s->context = wolfSSH_CTX_new(WOLFSSH_ENDPOINT_CLIENT, NULL);
    if (!s->context) return PT_NO_MEMORY;
    wolfSSH_SetIORecv(s->context, receive);
    wolfSSH_SetIOSend(s->context, transmit);
    wolfSSH_CTX_SetPublicKeyCheck(s->context, verify_host);
    wolfSSH_SetUserAuth(s->context, authenticate);
    int rc = wolfSSH_CTX_SetAlgoListKex(s->context, "mlkem768x25519-sha256,curve25519-sha256");
    if (!rc) rc = wolfSSH_CTX_SetAlgoListCipher(s->context, "aes128-gcm@openssh.com,aes256-gcm@openssh.com");
    if (!rc) rc = wolfSSH_CTX_SetAlgoListKey(s->context, "ssh-ed25519,ecdsa-sha2-nistp256");
    if (!rc) rc = wolfSSH_CTX_SetAlgoListMac(s->context, "hmac-sha2-256");
    if (rc) return rc;
    s->ssh = wolfSSH_new(s->context);
    if (!s->ssh) return PT_NO_MEMORY;
    wolfSSH_SetIOReadCtx(s->ssh, s);
    wolfSSH_SetIOWriteCtx(s->ssh, s);
    wolfSSH_SetPublicKeyCheckCtx(s->ssh, s);
    wolfSSH_SetUserAuthCtx(s->ssh, s);
    rc = wolfSSH_SetUsername(s->ssh, login->user);
    if (!rc) rc = wolfSSH_SetChannelType(s->ssh, WOLFSSH_SESSION_TERMINAL, NULL, 0);
    return rc;
}

static int retryable(int rc)
{
    return rc == WS_WANT_READ || rc == WS_WANT_WRITE || rc == WS_REKEYING ||
           rc == WS_WINDOW_FULL || rc == WS_CHAN_RXD;
}

static int step(Backend *s, uint8_t *details)
{
    if (!s->ssh) return PT_ERROR;
    if (!s->opened) {
        int rc = wolfSSH_connect(s->ssh);
        if (s->host_status != PT_OK) {
            memcpy(details + 4, s->fingerprint, 32);
            return s->host_status;
        }
        if (rc == WS_SUCCESS) {
            clear_key(s);
            pt_wipe(s->login.password, sizeof(s->login.password));
            if (terminal_open(s)) return PT_NO_MEMORY;
            s->opened = 1;
            return PT_CONNECTED;
        }
        int cause = wolfSSH_get_error(s->ssh);
        if (retryable(rc) || retryable(cause)) return PT_IDLE;
        pt_put_le32(details, (uint32_t)(cause ? cause : rc));
        return PT_ERROR;
    }

    if (wolfSSH_RekeyPending(s->ssh) || (wolfSSH_OutputPending(s->ssh) && !s->typed.count)) {
        int rc = wolfSSH_worker(s->ssh, NULL);
        int cause = wolfSSH_get_error(s->ssh);
        if (rc == WS_EOF || rc == WS_CHANNEL_CLOSED || cause == WS_DISCONNECT) return PT_CLOSED;
        if (rc != WS_SUCCESS && rc != WS_EXTDATA && !retryable(rc) && !retryable(cause)) {
            pt_put_le32(details, (uint32_t)(cause ? cause : rc));
            return PT_ERROR;
        }
        if (wolfSSH_RekeyPending(s->ssh)) return PT_IDLE;
    }

    if (s->resize_pending) {
        /* CHANGE-TERMINAL-SIZE bundles the whole request before the send. */
        const unsigned font = s->appearance.font;
        int rc = wolfSSH_ChangeTerminalSize(s->ssh, s->cols, s->rows,
                                            s->cols * pt_font_width(font),
                                            s->rows * pt_font_height(font));
        if (rc == WS_SUCCESS || rc == WS_WANT_WRITE) s->resize_pending = 0;
    }
    if (s->typed.count) {
        uint32_t count = s->typed.count > 512 ? 512 : s->typed.count;
        int rc = wolfSSH_stream_send(s->ssh, s->typed.bytes, count);
        if (rc > 0) remove_bytes(&s->typed, (uint32_t)rc);
        else if (!retryable(rc) && !retryable(wolfSSH_get_error(s->ssh))) {
            pt_put_le32(details, (uint32_t)wolfSSH_get_error(s->ssh));
            return PT_ERROR;
        }
    }

    uint8_t text[2048];
    int rc = wolfSSH_stream_read(s->ssh, text, sizeof(text));
    if (rc == WS_EXTDATA) rc = wolfSSH_extended_data_read(s->ssh, text, sizeof(text));
    else if (rc <= 0 && wolfSSH_get_error(s->ssh) == WS_WANT_READ) {
        int stderr_count = wolfSSH_extended_data_read(s->ssh, text, sizeof(text));
        if (stderr_count > 0) rc = stderr_count;
    }
    if (rc > 0) {
        if (s->history_offset && (s->appearance.flags & PT_FOLLOW_OUTPUT)) {
            s->history_offset = 0;
            damage_all(s);
        }
        vterm_input_write(s->vterm, (const char *)text, (size_t)rc);
        vterm_screen_flush_damage(s->screen);
        return PT_PROGRESS;
    }
    if (rc == WS_EOF || rc == WS_CHANNEL_CLOSED || rc == WS_DISCONNECT) return PT_CLOSED;
    int cause = wolfSSH_get_error(s->ssh);
    if (!rc || retryable(rc) || retryable(cause)) return PT_IDLE;
    if (cause == WS_EOF || cause == WS_CHANNEL_CLOSED || cause == WS_DISCONNECT) return PT_CLOSED;
    pt_put_le32(details, (uint32_t)(cause ? cause : rc));
    return PT_ERROR;
}

static int keyboard(Backend *s, uint32_t key, uint32_t modifiers)
{
    if (!s->vterm || s->typed.count > sizeof(s->typed.bytes) - 32) return PT_ERROR;
    static const VTermKey special[] = {
        VTERM_KEY_UP, VTERM_KEY_DOWN, VTERM_KEY_LEFT, VTERM_KEY_RIGHT, VTERM_KEY_ENTER,
        VTERM_KEY_BACKSPACE, VTERM_KEY_TAB, VTERM_KEY_ESCAPE, VTERM_KEY_DEL,
        VTERM_KEY_HOME, VTERM_KEY_END, VTERM_KEY_PAGEUP, VTERM_KEY_PAGEDOWN,
    };
    if (s->history_offset) {
        s->history_offset = 0;
        damage_all(s);
    }
    if (key < PT_UP) vterm_keyboard_unichar(s->vterm, key, (VTermModifier)modifiers);
    else if (key - PT_UP < sizeof(special) / sizeof(special[0]))
        vterm_keyboard_key(s->vterm, special[key - PT_UP], (VTermModifier)modifiers);
    return PT_OK;
}

int32_t pt_dispatch(uint32_t command, void *buffer, uint32_t size, uint32_t value)
{
    Backend *s = &session;
    PtKeyIO *key = buffer;
    switch (command) {
    case PT_BOOT:
        if (!s->ready) s->ready = wolfSSH_Init() == WS_SUCCESS;
        return s->ready ? PT_OK : PT_ERROR;
    case PT_IMPORT_SEED:
        return pt_seed_import(buffer, (uint8_t *)buffer + PT_SEED_SIZE);
    case PT_NEXT_SEED:
        return pt_seed_next((uint8_t *)buffer + sizeof(PtSeedIO), buffer);
    case PT_USE_SEED:
        return pt_seed_use(buffer);
    case PT_FORGET_SEED:
        pt_seed_clear();
        return PT_OK;
    case PT_KEY_IMPORT:
    case PT_KEY_UNLOCK: {
        clear_key(s);
        s->key_command = command;
        int rc = pt_key_read(&s->identity, key, size);
        if (rc == PT_PROGRESS && value) return rc;
        while (rc == PT_PROGRESS) rc = pt_key_step(&s->identity, key);
        if (rc == PT_OK) {
            s->identity_loaded = 1;
            if (command == PT_KEY_IMPORT) pt_key_public(&s->identity, key);
        }
        s->key_command = 0;
        return rc;
    }
    case PT_KEY_GENERATE:
        clear_key(s);
        if (pt_key_generate(&s->identity)) return PT_NO_RANDOM;
        s->identity_loaded = 1;
        pt_key_public(&s->identity, key);
        return PT_OK;
    case PT_KEY_SAVE: {
        if (!s->identity_loaded) return PT_BAD_KEY;
        s->key_command = command;
        int rc = pt_key_save(&s->identity, key);
        if (rc == PT_PROGRESS && value) return rc;
        while (rc == PT_PROGRESS) rc = pt_key_step(&s->identity, key);
        s->key_command = 0;
        return rc;
    }
    case PT_KEY_STEP: {
        int rc = pt_key_step(&s->identity, key);
        if (rc == PT_PROGRESS) return rc;
        if (rc == PT_OK) {
            s->identity_loaded = 1;
            if (s->key_command == PT_KEY_IMPORT) pt_key_public(&s->identity, key);
        }
        s->key_command = 0;
        return rc;
    }
    case PT_CLEAR_KEY:
        clear_key(s);
        return PT_OK;
    case PT_CONNECT:
        return connect_start(s, buffer);
    case PT_STOP:
        disconnect(s);
        return PT_OK;
    case PT_FEED:
        return (int32_t)append(&s->incoming, buffer, size);
    case PT_DRAIN:
        return (int32_t)take(&s->outgoing, buffer, size);
    case PT_STEP:
        return step(s, buffer);
    case PT_RESIZE:
        return resize_terminal(s, size, value);
    case PT_APPEARANCE:
        return set_appearance(s, buffer, size, value);
    case PT_CURSOR_PHASE:
        if (s->cursor_phase != (value != 0)) {
            s->cursor_phase = value != 0;
            if (s->cursor_visible && !s->history_offset && s->appearance.shape != PT_CURSOR_HIDDEN)
                damage_row(s, s->cursor.row);
        }
        return PT_OK;
    case PT_INPUT:
        return keyboard(s, value, size);
    case PT_DAMAGE: {
        PtFrame *frame = buffer;
        pt_put_le32(frame->rows, s->dirty[0]);
        pt_put_le32(frame->rows + 4, s->dirty[1]);
        frame->bell = (uint8_t)s->bell_pending;
        int ready = s->dirty[0] || s->dirty[1] || s->bell_pending;
        s->dirty[0] = s->dirty[1] = 0;
        s->bell_pending = 0;
        return ready;
    }
    case PT_ROW:
        return get_row(s, value, buffer);
#ifdef PT_ARMLET
    case PT_RASTER_ROW:
        return raster_row(s, value & 0xffff, value >> 16, buffer, size);
    case PT_RASTER_PREVIEW:
        return raster_preview(buffer, size);
#endif
    case PT_SCROLL: {
        if (s->alt_screen) return 0;
        int offset = (int)s->history_offset + (int32_t)value;
        if (offset < 0) offset = 0;
        if (offset > (int)s->history_count) offset = (int)s->history_count;
        s->history_offset = (unsigned)offset;
        damage_all(s);
        return offset;
    }
    default:
        return PT_ERROR;
    }
}
