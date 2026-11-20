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
#define SCROLL_LINES 96
#define AUTH_KEY_TYPE "ssh-ed25519"

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
    uint32_t dirty;
    uint8_t history[SCROLL_LINES][PT_GRID_COLS * PT_CELL_BYTES];
    unsigned history_next, history_count, history_offset;
    unsigned cols, rows;
    VTermPos cursor;
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
    *width = session.cols * 6;
    *height = session.rows * 10;
}

static int damage(VTermRect rect, void *user)
{
    Backend *s = user;
    if (s->history_offset) return 1;
    for (int row = rect.start_row; row < rect.end_row && row < (int)s->rows; row++)
        if (row >= 0) s->dirty |= 1u << row;
    return 1;
}

static int cursor(VTermPos pos, VTermPos old, int visible, void *user)
{
    Backend *s = user;
    s->cursor = pos;
    s->cursor_visible = visible;
    if (!s->history_offset) {
        if (old.row >= 0 && old.row < (int)s->rows) s->dirty |= 1u << old.row;
        if (pos.row >= 0 && pos.row < (int)s->rows) s->dirty |= 1u << pos.row;
    }
    return 1;
}

static int term_property(VTermProp prop, VTermValue *value, void *user)
{
    Backend *s = user;
    if (prop == VTERM_PROP_CURSORVISIBLE)
        return cursor(s->cursor, s->cursor, value->boolean, s);
    if (prop == VTERM_PROP_ALTSCREEN) {
        s->history_offset = 0;
        s->dirty = (1u << s->rows) - 1;
    }
    return 1;
}

static uint16_t pixel(VTermScreen *screen, VTermColor color)
{
    vterm_screen_convert_color_to_rgb(screen, &color);
    return (uint16_t)((color.rgb.red >> 3) << 11 |
                      (color.rgb.green >> 2) << 5 | color.rgb.blue >> 3);
}

static void encode_cell(Backend *s, VTermScreenCell *cell, uint8_t *out, int at_cursor)
{
    pt_put_le32(out, cell->chars[0] == UINT32_MAX ? 0 : cell->chars[0]);
    int inverse = cell->attrs.reverse ^ at_cursor;
    uint16_t fg = pixel(s->screen, inverse ? cell->bg : cell->fg);
    uint16_t bg = pixel(s->screen, inverse ? cell->fg : cell->bg);
    out[4] = (uint8_t)(fg >> 8);
    out[5] = (uint8_t)fg;
    out[6] = (uint8_t)(bg >> 8);
    out[7] = (uint8_t)bg;
}

static int push_history(int cols, const VTermScreenCell *cells, void *user)
{
    Backend *s = user;
    uint8_t *row = s->history[s->history_next];
    memset(row, 0, sizeof(s->history[0]));
    for (int col = 0; col < cols && col < PT_GRID_COLS; col++) {
        VTermScreenCell cell = cells[col];
        encode_cell(s, &cell, row + col * PT_CELL_BYTES, 0);
    }
    s->history_next = (s->history_next + 1) % SCROLL_LINES;
    if (s->history_count < SCROLL_LINES) s->history_count++;
    if (s->history_offset && s->history_offset < s->history_count) s->history_offset++;
    return 1;
}

static int clear_history(void *user)
{
    Backend *s = user;
    s->history_next = 0;
    s->history_count = 0;
    s->history_offset = 0;
    return 1;
}

static const VTermScreenCallbacks screen_callbacks = {
    .damage = damage,
    .movecursor = cursor,
    .settermprop = term_property,
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
    (void)user;
    void *buf = calloc(1, size);
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

static int terminal_open(Backend *s)
{
    s->vterm = vterm_new_with_allocator((int)s->rows, (int)s->cols,
                                        &terminal_allocator, NULL);
    if (!s->vterm) return -1;
    vterm_set_utf8(s->vterm, 1);
    vterm_output_set_callback(s->vterm, terminal_output, s);
    s->screen = vterm_obtain_screen(s->vterm);
    if (!s->screen) return -1;
    VTermColor text, background;
    vterm_color_rgb(&text, 232, 232, 232);
    vterm_color_rgb(&background, 0, 0, 0);
    vterm_state_set_default_colors(vterm_obtain_state(s->vterm), &text, &background);
    vterm_screen_set_callbacks(s->screen, &screen_callbacks, s);
    vterm_screen_enable_altscreen(s->screen, 1);
    vterm_screen_set_damage_merge(s->screen, VTERM_DAMAGE_ROW);
    vterm_screen_reset(s->screen, 1);
    s->dirty = (1u << s->rows) - 1;
    return 0;
}

static int get_row(Backend *s, unsigned row, uint8_t *out)
{
    if (!s->screen || row >= s->rows) return PT_ERROR;
    if (row < s->history_offset) {
        unsigned slot = (s->history_next + SCROLL_LINES - s->history_offset + row) % SCROLL_LINES;
        memcpy(out, s->history[slot], s->cols * PT_CELL_BYTES);
        return PT_OK;
    }
    row -= s->history_offset;
    for (unsigned col = 0; col < s->cols; col++) {
        VTermPos pos = {(int)row, (int)col};
        VTermScreenCell cell;
        memset(&cell, 0, sizeof(cell));
        if (vterm_screen_get_cell(s->screen, pos, &cell))
            encode_cell(s, &cell, out + col * PT_CELL_BYTES,
                        !s->history_offset && s->cursor_visible &&
                        pos.row == s->cursor.row && pos.col == s->cursor.col);
        else
            memset(out + col * PT_CELL_BYTES, 0, PT_CELL_BYTES);
    }
    return PT_OK;
}

#ifdef PT_ARMLET
/* Read cells and draw into the Palm row bitmap in the same native call. */
static int raster_row(Backend *s, unsigned row, void *bitmap, uint32_t stride)
{
    if (!bitmap || stride < PT_GRID_COLS * 6 * 2 || ((uintptr_t)bitmap | stride) & 1)
        return PT_ERROR;
    uint8_t cells[PT_GRID_COLS * PT_CELL_BYTES] = {0};
    int rc = get_row(s, row, cells);
    if (rc != PT_OK) return rc;

    for (unsigned col = 0; col < PT_GRID_COLS; col++) {
        const uint8_t *cell = cells + col * PT_CELL_BYTES;
        uint32_t code = pt_le32(cell);
        const uint8_t *glyph = latin_glyphs[0];
        if (code >= 32 && code <= 255) glyph = latin_glyphs[code - 32];
        else if (code >= 0x2500 && code <= 0x259f) glyph = line_glyphs[code - 0x2500];
        else if (code) glyph = latin_glyphs['?' - 32];

        /* Palm's bitmap stores big-endian RGB565. The ARM stores each
         * halfword little-endian, so build the swapped values once. */
        uint16_t fg = (uint16_t)(cell[5] << 8 | cell[4]);
        uint16_t bg = (uint16_t)(cell[7] << 8 | cell[6]);
        for (unsigned y = 0; y < 10; y++) {
            uint16_t *pixels = (uint16_t *)((uint8_t *)bitmap + y * stride) + col * 6;
            uint8_t bits = glyph[y];
            for (unsigned x = 0; x < 6; x++, bits <<= 1)
                pixels[x] = (bits & 0x80) ? fg : bg;
        }
    }
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
    if (s->ssh || !login->user[0] || !login->rows || !login->cols ||
        login->rows > PT_GRID_ROWS || login->cols > PT_GRID_COLS) return PT_ERROR;
    s->login = *login;
    s->cols = login->cols;
    s->rows = login->rows;
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
        int rc = wolfSSH_ChangeTerminalSize(s->ssh, s->cols, s->rows, s->cols * 6, s->rows * 10);
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
        s->dirty = (1u << s->rows) - 1;
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
        if (!s->vterm || !size || size > PT_GRID_ROWS || !value || value > PT_GRID_COLS)
            return PT_ERROR;
        s->rows = size;
        s->cols = value;
        vterm_set_size(s->vterm, (int)size, (int)value);
        s->resize_pending = 1;
        s->dirty = (1u << s->rows) - 1;
        return PT_OK;
    case PT_INPUT:
        return keyboard(s, value, size);
    case PT_DAMAGE: {
        uint32_t result = s->dirty;
        s->dirty = 0;
        return (int32_t)result;
    }
    case PT_ROW:
        return get_row(s, value, buffer);
#ifdef PT_ARMLET
    case PT_RASTER_ROW:
        return raster_row(s, value, buffer, size);
#endif
    case PT_SCROLL: {
        int offset = (int)s->history_offset + (int32_t)value;
        if (offset < 0) offset = 0;
        if (offset > (int)s->history_count) offset = (int)s->history_count;
        s->history_offset = (unsigned)offset;
        s->dirty = (1u << s->rows) - 1;
        return offset;
    }
    default:
        return PT_ERROR;
    }
}
