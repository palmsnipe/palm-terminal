#include "palm.h"
#include <Chars.h>
#include <Keyboard.h>
#include <NotifyMgr.h>
#include "resource.h"
#include "terminal-font.h"

#define APP_FEATURE 0
#define PAGE_SIZE 5
#define SMALL_ROWS 26
#define SLEEP_EVENT (firstUserEvent + 1)
#define CONNECT_TIMEOUT 45UL

enum { InputSeed, InputImport, InputName, InputPublic };
enum { RemotePassword, KeyUnlock, ImportPassword, ProtectKey, ConfirmProtection };

typedef struct {
    PtArm arm;
    PtNet net;
    DmOpenRef db;
    PtProfile profile;
    PtLogin login;
    PtKeyIO key_io;
    Char value[PT_PATH_MAX], secret[PT_PASSWORD_MAX], protection[PT_PASSWORD_MAX];
    Char name[PT_LABEL_MAX];
    uint8_t detail[36], row[PT_GRID_COLS * PT_CELL_BYTES];
    UInt32 rows_id[PAGE_SIZE];
    Char rows_text[PAGE_SIZE][42];
    Char *choices[PAGE_SIZE];
    UInt32 target_key, timestamp;
    UInt16 page, row_count, file_size, text_action, secret_action, rows, key_job;
    Boolean listing_keys, more, generating, pumping, ssh_started, shell, ctrl, typing, stopped;
    Boolean sleep_pending, deferred_sleep;
#ifdef PT_PROFILE
    UInt32 heap_at_launch, handshake_ticks, key_ticks;
#endif
    BitmapType *row_bitmap;
    BitmapTypeV3 *row_display;
} App;

static App *current(void)
{
    UInt32 ptr = 0;
    return FtrGet(PT_CREATOR, APP_FEATURE, &ptr) == errNone ? (App *)ptr : NULL;
}

static UInt16 form_id(void)
{
    return FrmGetActiveFormID();
}

static UInt16 object_index(UInt16 id)
{
    return FrmGetObjectIndex(FrmGetActiveForm(), id);
}

static void *object(UInt16 id)
{
    UInt16 index = object_index(id);
    return index == frmInvalidObjectId ? NULL : FrmGetObjectPtr(FrmGetActiveForm(), index);
}

static void show(UInt16 id, Boolean visible)
{
    UInt16 index = object_index(id);
    if (index == frmInvalidObjectId) return;
    if (visible) FrmShowObject(FrmGetActiveForm(), index);
    else FrmHideObject(FrmGetActiveForm(), index);
}

static Boolean set_field(UInt16 id, const Char *text)
{
    FieldType *field = object(id);
    if (!field) return false;
    UInt16 length = StrLen(text);
    MemHandle h = MemHandleNew(length + 1);
    if (!h) return false;
    Char *p = MemHandleLock(h);
    if (!p) {
        MemHandleFree(h);
        return false;
    }
    MemMove(p, text, length + 1);
    MemHandleUnlock(h);
    FldFreeMemory(field);
    FldSetTextHandle(field, h);
    FldSetInsPtPosition(field, length);
    return true;
}

static void get_field(UInt16 id, Char *out, UInt16 capacity)
{
    FieldType *field = object(id);
    const Char *value = field ? FldGetTextPtr(field) : NULL;
    if (value) StrNCopy(out, value, capacity - 1);
    else out[0] = 0;
    out[capacity - 1] = 0;
}

static void focus(UInt16 id)
{
    UInt16 index = object_index(id);
    if (index != frmInvalidObjectId) FrmSetFocus(FrmGetActiveForm(), index);
}

static void notice(const Char *message)
{
    FrmCustomAlert(InfoAlert, message, "", "");
}

static void code_notice(const Char *prefix, Int32 code)
{
    Char message[86], number[18];
    StrIToA(number, code);
    StrCopy(message, prefix);
    StrCat(message, " (");
    StrCat(message, number);
    StrCat(message, ").");
    notice(message);
}

static Boolean confirm(const Char *message)
{
    return FrmCustomAlert(ConfirmAlert, message, "", "") == 0;
}

static void text(Coord x, Coord y, const Char *p)
{
    Int16 size = StrLen(p), width = 160 - x - 4;
    Boolean fit;
    FntCharsInWidth(p, &width, &size, &fit);
    WinDrawChars(p, size, x, y);
}

static void repaint(Coord x, Coord y, Coord width, Coord height)
{
    RectangleType rect;
    RctSetRectangle(&rect, x, y, width, height);
    WinEraseRectangle(&rect, 0);
}

static void home_input(App *a)
{
    get_field(Host, a->profile.host, sizeof(a->profile.host));
    get_field(User, a->profile.user, sizeof(a->profile.user));
    get_field(Port, a->profile.port, sizeof(a->profile.port));
}

static UInt16 port(App *a)
{
    if (!a->profile.port[0]) StrCopy(a->profile.port, "22");
    UInt32 n = 0;
    for (const Char *p = a->profile.port; *p; p++) {
        if (*p < '0' || *p > '9') return 0;
        n = n * 10 + (UInt32)(*p - '0');
    }
    if (!n || n > 65535) return 0;
    StrIToA(a->profile.port, n);
    return (UInt16)n;
}

static Boolean valid_host(const Char *host)
{
    if (!*host) return false;
    for (; *host; host++) {
        if ((*host >= 'a' && *host <= 'z') || (*host >= 'A' && *host <= 'Z') ||
            (*host >= '0' && *host <= '9') || *host == '-' || *host == '.') continue;
        return false;
    }
    return true;
}

static void go_list(App *a, Boolean keys)
{
    a->page = 0;
    a->listing_keys = keys;
    FrmGotoForm(ListForm);
}

static void clear_credentials(App *a)
{
    pt_wipe(&a->key_io, sizeof(a->key_io));
    pt_wipe(a->secret, sizeof(a->secret));
    pt_wipe(a->protection, sizeof(a->protection));
    pt_wipe(a->login.password, sizeof(a->login.password));
    pt_call(&a->arm, PT_CLEAR_KEY, NULL, 0, 0);
}

#ifdef PT_PROFILE
/* A profiling build records timings and allocation counts on a disposable SD card. */
static void record_profile(App *a, const Char *path, UInt32 ticks)
{
    UInt32 free_bytes = 0, largest = 0;
    UInt32 peak = (UInt32)pt_call(&a->arm, PT_STATS, NULL, 0, 0);
    if (MemHeapFreeBytes(0, &free_bytes, &largest)) return;
    UInt32 footprint = a->heap_at_launch > free_bytes ? a->heap_at_launch - free_bytes : 0;
    Char line[150];
    StrPrintF(line, "ticks=%lu\nticks_per_second=%u\nheap_delta_bytes=%lu\narm_arena_peak_bytes=%lu\n",
              ticks, SysTicksPerSecond(), footprint, peak);
    pt_sd_create(path, line, StrLen(line));
}
#endif

static void stop(App *a)
{
#ifdef PT_PROFILE
    if (a->shell) record_profile(a, "/PT-SSH.TXT", a->handshake_ticks);
#endif
    a->pumping = false;
    a->key_job = 0;
    a->ssh_started = false;
    a->shell = false;
    a->typing = false;
    a->ctrl = false;
    a->rows = PT_GRID_ROWS;
    pt_net_close(&a->net);
    pt_call(&a->arm, PT_STOP, NULL, 0, 0);
    clear_credentials(a);
}

static Boolean seed_ready(App *a)
{
    if (pt_seed_reserve(&a->arm)) return true;
    notice("Import a fresh desktop seed using Seed setup.");
    return false;
}

static void ask_text(App *a, UInt16 action, const Char *initial)
{
    a->text_action = action;
    if (a->value != initial) StrCopy(a->value, initial);
    FrmGotoForm(TextForm);
}

static void ask_secret(App *a, UInt16 action)
{
    a->secret_action = action;
    pt_wipe(a->secret, sizeof(a->secret));
    FrmGotoForm(SecretForm);
}

static void authentication(App *a)
{
    ask_secret(a, a->profile.key ? KeyUnlock : RemotePassword);
}

static void start_connection(App *a)
{
    if (!seed_ready(a)) {
        stop(a);
        FrmGotoForm(HomeForm);
        return;
    }
    MemSet(a->detail, sizeof(a->detail), 0);
    a->timestamp = TimGetTicks();
    if (!pt_net_start(&a->net)) {
        Err error = a->net.error;
        stop(a);
        code_notice("Could not open Palm networking", error);
        FrmGotoForm(HomeForm);
        return;
    }
    a->pumping = true;
    FrmGotoForm(BusyForm);
}

static void connect(App *a)
{
    home_input(a);
    if (!port(a) || !valid_host(a->profile.host) || !a->profile.user[0]) {
        notice("Enter an IPv4 address or DNS name, a port, and your SSH username.");
        return;
    }
    if (a->profile.key && !pt_key_stored(a->db, a->profile.key, NULL, NULL)) {
        notice("Select an available key or choose Use password.");
        return;
    }
    if (!pt_profile_save(a->db, &a->profile)) {
        notice("Could not save this connection profile.");
        return;
    }
    MemSet(&a->login, sizeof(a->login), 0);
    StrCopy(a->login.user, a->profile.user);
    a->login.cols = PT_GRID_COLS;
    a->login.rows = PT_GRID_ROWS;
    a->login.key_auth = a->profile.key != 0;
    a->login.pinned = pt_pin_find(a->db, &a->profile, a->login.fingerprint) != 0;
    if (a->login.pinned) authentication(a);
    else start_connection(a);
}

static void fingerprint_text(const uint8_t *digest, Char out[44])
{
    static const Char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    UInt32 bits = 0;
    UInt16 remaining = 0, output = 0;
    for (UInt16 i = 0; i < 32; i++) {
        bits = (bits << 8) | digest[i];
        remaining += 8;
        while (remaining >= 6) {
            remaining -= 6;
            out[output++] = alphabet[(bits >> remaining) & 63];
        }
    }
    if (remaining) out[output++] = alphabet[(bits << (6 - remaining)) & 63];
    out[output] = 0;
}

static void trust(App *a)
{
    if (!pt_pin_save(a->db, &a->profile, a->detail + 4)) {
        notice("Could not save the verified host key.");
        FrmGotoForm(HomeForm);
        return;
    }
    MemMove(a->login.fingerprint, a->detail + 4, sizeof(a->login.fingerprint));
    a->login.pinned = true;
    authentication(a);
}

static void list_page(App *a)
{
    UInt16 index = 0, left = a->page * PAGE_SIZE;
    a->row_count = 0;
    a->more = false;
    while (true) {
        UInt32 id;
        PtProfile profile;
        Char title[PT_LABEL_MAX];
        if (a->listing_keys) {
            id = pt_record_find(a->db, PT_RECORD_KEY, &index, NULL, 0);
            if (id && !pt_key_stored(a->db, id, title, NULL)) continue;
        } else {
            id = pt_record_find(a->db, PT_RECORD_PROFILE, &index, NULL, 0);
            if (id && !pt_profile_read(a->db, id, &profile)) continue;
        }
        if (!id) break;
        if (left) {
            left--;
            continue;
        }
        if (a->row_count == PAGE_SIZE) {
            a->more = true;
            break;
        }
        UInt16 row = a->row_count++;
        a->rows_id[row] = id;
        a->choices[row] = a->rows_text[row];
        Char *label = a->rows_text[row];
        if (a->listing_keys) StrCopy(label, title);
        else {
            StrNCopy(label, profile.user, 12);
            label[12] = 0;
            StrCat(label, "@");
            UInt16 used = StrLen(label);
            StrNCopy(label + used, profile.host, sizeof(a->rows_text[row]) - used - 1);
            label[sizeof(a->rows_text[row]) - 1] = 0;
        }
    }
    if (!a->row_count && a->page) {
        a->page--;
        list_page(a);
        return;
    }
    ListType *list = object(Items);
    LstSetListChoices(list, a->choices, a->row_count);
    LstSetSelection(list, a->row_count ? 0 : noListSelection);
}

static UInt32 selected(App *a)
{
    Int16 row = LstGetSelection(object(Items));
    if (row < 0 || (UInt16)row >= a->row_count) {
        notice(a->listing_keys ? "Select a key." : "Select a connection profile.");
        return 0;
    }
    return a->rows_id[row];
}

static void list_controls(App *a, UInt16 action)
{
    if (action == Back) {
        FrmGotoForm(HomeForm);
        return;
    }
    if (action == Prev || action == Next) {
        if (action == Prev && a->page) a->page--;
        else if (action == Next && a->more) a->page++;
        else return;
        list_page(a);
        FrmDrawForm(FrmGetActiveForm());
        return;
    }
    if (action == UsePassword) {
        a->profile.key = 0;
        FrmGotoForm(HomeForm);
        return;
    }
    if (action == Generate || action == Import) {
        clear_credentials(a);
        a->generating = action == Generate;
        if (a->generating) ask_text(a, InputName, "Palm E2");
        else ask_text(a, InputImport, "/id_ed25519");
        return;
    }
    UInt32 id = selected(a);
    if (!id) return;
    if (action == Pick) {
        if (a->listing_keys) a->profile.key = id;
        else if (!pt_profile_read(a->db, id, &a->profile)) return;
        FrmGotoForm(HomeForm);
    } else if (action == Remove && confirm(a->listing_keys ? "Delete this private key?" : "Delete this profile?")) {
        if (!pt_record_remove(a->db, id, a->listing_keys ? PT_RECORD_KEY : PT_RECORD_PROFILE))
            notice("Could not delete the selected item.");
        else if (a->listing_keys && a->profile.key == id) a->profile.key = 0;
        list_page(a);
        FrmDrawForm(FrmGetActiveForm());
    } else if (action == Public) {
        a->target_key = id;
        ask_text(a, InputPublic, "/palm-ed25519.pub");
    }
}

static void key_done(App *a)
{
    clear_credentials(a);
    go_list(a, true);
}

static void text_done(App *a)
{
    get_field(Value, a->value, sizeof(a->value));
    if (a->text_action == InputName) {
        if (!a->value[0] || StrLen(a->value) >= sizeof(a->name)) {
            notice("Use a name of 1 to 31 characters.");
            return;
        }
        StrCopy(a->name, a->value);
        if (a->generating) {
            if (!seed_ready(a)) return;
            Int32 result = pt_call(&a->arm, PT_KEY_GENERATE, &a->key_io, 0, 0);
            pt_call(&a->arm, PT_FORGET_SEED, NULL, 0, 0);
            if (result != PT_OK) {
                notice("Could not generate an Ed25519 key.");
                key_done(a);
                return;
            }
        }
        ask_secret(a, ProtectKey);
        return;
    }
    if (a->value[0] != '/' || !a->value[1]) {
        notice("Enter the full SD path starting with /.");
        return;
    }
    if (a->text_action == InputSeed) {
        uint8_t seed[PT_SEED_SIZE];
        UInt16 size = pt_sd_read(a->value, seed, sizeof(seed));
        Boolean ok = size == sizeof(seed) && pt_seed_install(&a->arm, seed);
        pt_wipe(seed, sizeof(seed));
        if (!ok) {
            notice("Use a new 48-byte seed from the desktop helper.");
            return;
        }
        notice("Seed installed. Remove the seed file from the SD card and desktop.");
        FrmGotoForm(HomeForm);
    } else if (a->text_action == InputImport) {
        a->file_size = pt_sd_read(a->value, a->key_io.file, sizeof(a->key_io.file));
        if (!a->file_size) {
            notice("Could not read the key from SD. Check the path and card.");
            return;
        }
        ask_secret(a, ImportPassword);
    } else if (a->text_action == InputPublic) {
        Char pub[PT_PUBLIC_MAX];
        if (!pt_key_stored(a->db, a->target_key, NULL, pub) ||
            !pt_sd_create(a->value, pub, StrLen(pub))) {
            notice("Could not create the file. Check the card and choose an unused path.");
            return;
        }
        notice("Public key saved to SD.");
        go_list(a, true);
    }
}

static void finish_key_job(App *a, UInt16 job, Int32 result)
{
    a->key_job = 0;
#ifdef PT_PROFILE
    if (result >= 0)
        record_profile(a, job == PT_KEY_SAVE ? "/PT-SAVE.TXT" :
                          job == PT_KEY_UNLOCK ? "/PT-UNLK.TXT" : "/PT-IMPT.TXT",
                       TimGetTicks() - a->key_ticks);
#endif
    if (job == PT_KEY_SAVE) {
        if (result <= 0 || result > PT_KEY_SAVE_MAX ||
            !pt_key_store(a->db, a->name, &a->key_io, (UInt16)result))
            notice("Could not save the protected private key.");
        else
            notice("Key saved. Use Export .pub to authorize it on your server.");
        key_done(a);
        return;
    }
    Boolean importing = job == PT_KEY_IMPORT;
    if (result != PT_OK) {
        notice("Wrong passphrase or unsupported Ed25519 key.");
        if (importing) ask_text(a, InputImport, a->value);
        else ask_secret(a, KeyUnlock);
    } else if (importing) {
        ask_text(a, InputName, "Imported key");
    } else {
        start_connection(a);
    }
}

static void begin_key_job(App *a, UInt16 job)
{
#ifdef PT_PROFILE
    a->key_ticks = TimGetTicks();
#endif
    Int32 result = pt_call(&a->arm, job, &a->key_io, a->file_size, 1);
    pt_wipe(a->key_io.password, sizeof(a->key_io.password));
    if (job == PT_KEY_SAVE) pt_call(&a->arm, PT_FORGET_SEED, NULL, 0, 0);
    if (result == PT_PROGRESS) {
        a->key_job = job;
        FrmGotoForm(BusyForm);
    } else {
        finish_key_job(a, job, result);
    }
}

static void secret_done(App *a)
{
    if (a->secret_action == RemotePassword) {
        StrCopy(a->login.password, a->secret);
        pt_wipe(a->secret, sizeof(a->secret));
        start_connection(a);
    } else if (a->secret_action == KeyUnlock || a->secret_action == ImportPassword) {
        Boolean importing = a->secret_action == ImportPassword;
        if (!importing) a->file_size = pt_key_load(a->db, a->profile.key, &a->key_io);
        StrCopy(a->key_io.password, a->secret);
        pt_wipe(a->secret, sizeof(a->secret));
        begin_key_job(a, importing ? PT_KEY_IMPORT : PT_KEY_UNLOCK);
    } else if (a->secret_action == ProtectKey) {
        if (!a->secret[0]) {
            notice("Enter a passphrase to protect this key.");
            return;
        }
        StrCopy(a->protection, a->secret);
        ask_secret(a, ConfirmProtection);
    } else if (a->secret_action == ConfirmProtection) {
        if (StrCompare(a->secret, a->protection)) {
            notice("The passphrases differ. Enter a new one.");
            pt_wipe(a->protection, sizeof(a->protection));
            ask_secret(a, ProtectKey);
            return;
        }
        if (!seed_ready(a)) return;
        StrCopy(a->key_io.password, a->protection);
        pt_wipe(a->protection, sizeof(a->protection));
        pt_wipe(a->secret, sizeof(a->secret));
        begin_key_job(a, PT_KEY_SAVE);
    }
}

static void draw_mask(App *a)
{
    Char mask[33];
    UInt16 n = StrLen(a->secret);
    if (n > sizeof(mask) - 1) n = sizeof(mask) - 1;
    MemSet(mask, n, '*');
    mask[n] = 0;
    repaint(8, 65, 144, 15);
    text(8, 66, mask);
    WinDrawLine(8, 79, 151, 79);
}

static Boolean secret_key(App *a, const EventType *event)
{
    WChar c = event->data.keyDown.chr;
    if (event->data.keyDown.modifiers & commandKeyMask) return false;
    if (c == chrCarriageReturn || c == chrLineFeed) {
        secret_done(a);
        return true;
    }
    UInt16 size = StrLen(a->secret);
    if (c == chrBackspace) {
        if (size) a->secret[--size] = 0;
    } else if (c >= 32 && c <= 126 && size + 1 < sizeof(a->secret)) {
        a->secret[size++] = (Char)c;
        a->secret[size] = 0;
    }
    draw_mask(a);
    return true;
}

static void row_draw(App *a, UInt16 row)
{
    if (pt_call(&a->arm, PT_ROW, a->row, sizeof(a->row), row) != PT_OK) return;
    UInt16 row_bytes;
    BmpGetDimensions(a->row_bitmap, NULL, NULL, &row_bytes);
    UInt16 stride = row_bytes / 2;
    UInt16 *pixels = BmpGetBits(a->row_bitmap);
    const uint8_t *cell = a->row;
    for (UInt16 col = 0; col < PT_GRID_COLS; col++, cell += PT_CELL_BYTES) {
        UInt32 code = pt_le32(cell);
        const UInt8 *glyph = latin_glyphs[0];
        if (code >= 32 && code <= 255) glyph = latin_glyphs[code - 32];
        else if (code >= 0x2500 && code <= 0x259f) glyph = line_glyphs[code - 0x2500];
        else if (code) glyph = latin_glyphs['?' - 32];
        UInt16 fg = (UInt16)cell[4] << 8 | cell[5];
        UInt16 bg = (UInt16)cell[6] << 8 | cell[7];
        for (UInt16 y = 0; y < 10; y++) {
            UInt16 *dest = pixels + y * stride + col * 6;
            UInt8 bits = glyph[y];
            for (UInt16 x = 0; x < 6; x++, bits <<= 1)
                dest[x] = (bits & 0x80) ? fg : bg;
        }
    }
    WinDrawBitmap((BitmapType *)a->row_display, 0, row * 10);
}

static void shell_draw(App *a, Boolean all)
{
    if (!a->row_display || form_id() != ShellForm) return;
    UInt32 dirty = (UInt32)pt_call(&a->arm, PT_DAMAGE, NULL, 0, 0);
    if (all) dirty = (1UL << a->rows) - 1;
    if (!dirty) return;
    UInt16 old_coords = WinSetCoordinateSystem(kCoordinatesNative);
    for (UInt16 row = 0; row < a->rows; row++)
        if (dirty & (1UL << row)) row_draw(a, row);
    WinSetCoordinateSystem(old_coords);
}

static void term_send(App *a, UInt32 key)
{
    UInt32 mods = a->ctrl ? PT_CTRL : 0;
    if (pt_call(&a->arm, PT_INPUT, NULL, mods, key) != PT_OK) SndPlaySystemSound(sndError);
    if (a->ctrl) {
        a->ctrl = false;
        CtlSetLabel(object(Ctrl), "Ctrl");
    }
}

static Boolean shell_key(App *a, const EventType *e)
{
    WChar c = e->data.keyDown.chr;
    Boolean v = (e->data.keyDown.modifiers & commandKeyMask) != 0;
    if (a->typing && (!v || c == vchrRockerLeft || c == vchrRockerRight)) return false;
    UInt32 key = 0;
    if (v) {
        switch (c) {
        case vchrRockerUp: case vchrPageUp: key = PT_UP; break;
        case vchrRockerDown: case vchrPageDown: key = PT_DOWN; break;
        case vchrRockerLeft: key = PT_LEFT; break;
        case vchrRockerRight: key = PT_RIGHT; break;
        case vchrRockerCenter: case vchrHardRockerCenter: key = PT_ENTER; break;
        default: return false;
        }
    } else {
        switch (c) {
        case chrCarriageReturn: case chrLineFeed: key = PT_ENTER; break;
        case chrBackspace: key = PT_BACKSPACE; break;
        case chrTab: key = PT_TAB; break;
        case 27: key = PT_ESC; break;
        case 127: key = PT_DELETE; break;
        default:
            if (c < 32) return false;
            key = c;
            break;
        }
    }
    term_send(a, key);
    return true;
}

static void shell_controls(App *a, UInt16 action)
{
    switch (action) {
    case Ctrl:
        a->ctrl = !a->ctrl;
        CtlSetLabel(object(Ctrl), a->ctrl ? "Ctrl*" : "Ctrl");
        break;
    case Esc: term_send(a, PT_ESC); break;
    case Tab: term_send(a, PT_TAB); break;
    case ScrollUp: case ScrollDown:
        pt_call(&a->arm, PT_SCROLL, NULL, 0, action == ScrollUp ? 8UL : (UInt32)-8);
        shell_draw(a, false);
        break;
    case Type:
        a->typing = !a->typing;
        a->rows = a->typing ? SMALL_ROWS : PT_GRID_ROWS;
        if (a->typing) {
            set_field(ShellText, "");
            repaint(0, 130, 160, 16);
        } else {
            get_field(ShellText, a->value, sizeof(a->value));
            pt_wipe(a->value, sizeof(a->value));
        }
        show(ShellText, a->typing);
        show(Send, a->typing);
        pt_call(&a->arm, PT_RESIZE, NULL, a->rows, PT_GRID_COLS);
        if (a->typing) {
            focus(ShellText);
            SysKeyboardDialog(kbdAlpha);
        }
        shell_draw(a, true);
        break;
    case Send: {
        Char line[256];
        get_field(ShellText, line, sizeof(line));
        for (const unsigned char *p = (const unsigned char *)line; *p; p++) term_send(a, *p);
        term_send(a, PT_ENTER);
        pt_wipe(line, sizeof(line));
        set_field(ShellText, "");
        FldDrawField(object(ShellText));
        break;
    }
    case End:
        stop(a);
        FrmGotoForm(HomeForm);
        break;
    }
}

static void pump(App *a)
{
    if (a->key_job && form_id() == BusyForm) {
        Int32 result = pt_call(&a->arm, PT_KEY_STEP, &a->key_io, 0, 0);
        if (result != PT_PROGRESS) finish_key_job(a, a->key_job, result);
        return;
    }
    if (!a->pumping || (form_id() != BusyForm && form_id() != ShellForm)) return;
    if (!a->shell && TimGetTicks() - a->timestamp > SysTicksPerSecond() * CONNECT_TIMEOUT) {
        stop(a);
        notice("The SSH connection timed out.");
        FrmGotoForm(HomeForm);
        return;
    }
    if (!a->ssh_started) {
        UInt16 state = pt_net_poll(&a->net, a->profile.host, port(a));
        if (state == PT_NET_WORK) return;
        if (state != PT_NET_READY) {
            Err error = a->net.error;
            stop(a);
            code_notice("Network connection failed", error);
            FrmGotoForm(HomeForm);
            return;
        }
        Int32 rc = pt_call(&a->arm, PT_CONNECT, &a->login, 0, 0);
        pt_wipe(a->login.password, sizeof(a->login.password));
        if (rc != PT_OK) {
            stop(a);
            code_notice("Could not initialize SSH", rc);
            FrmGotoForm(HomeForm);
            return;
        }
        a->ssh_started = true;
    }

    UInt16 network = pt_net_io(&a->net, &a->arm);
    Int32 result = network == PT_NET_FAIL ? PT_ERROR :
                                               pt_call(&a->arm, PT_STEP, a->detail, 0, 0);
    if (result == PT_CONNECTED) {
#ifdef PT_PROFILE
        a->handshake_ticks = TimGetTicks() - a->timestamp;
#endif
        a->shell = true;
        FrmGotoForm(ShellForm);
    } else if (result == PT_UNKNOWN_HOST) {
        stop(a);
        FrmGotoForm(TrustForm);
    } else if (result == PT_CHANGED_HOST) {
        stop(a);
        notice("Host key changed. Verify the new fingerprint before using Forget host key.");
        FrmGotoForm(HomeForm);
    } else if (result < 0 || result == PT_CLOSED || (result == PT_IDLE && network == PT_NET_END)) {
        Int32 reason = network == PT_NET_FAIL ? a->net.error : (Int32)pt_le32(a->detail);
        stop(a);
        if (result < 0) code_notice("SSH connection ended", reason ? reason : result);
        else notice("The SSH connection closed.");
        FrmGotoForm(HomeForm);
    } else if (a->shell) {
        shell_draw(a, false);
    }
}

static void draw_form(App *a, UInt16 id, Boolean opening)
{
    FormType *form = FrmGetActiveForm();
    if (opening && id == HomeForm) {
        set_field(Host, a->profile.host);
        set_field(Port, a->profile.port);
        set_field(User, a->profile.user);
        focus(Host);
    }
    if (opening && id == ListForm) {
        list_page(a);
        if (!a->listing_keys) {
            show(Import, false);
            show(Generate, false);
            show(Public, false);
            show(UsePassword, false);
        }
    }
    if (opening && id == TextForm) {
        set_field(Value, a->value);
        focus(Value);
        FldSetSelection(object(Value), 0, StrLen(a->value));
    }
    FrmDrawForm(form);
    if (id == HomeForm) {
        Char name[PT_LABEL_MAX];
        if (a->profile.key && pt_key_stored(a->db, a->profile.key, name, NULL)) {
            text(8, 86, "SSH key");
            text(49, 86, name);
        } else {
            text(8, 86, "Password authentication");
        }
    } else if (id == ListForm) {
        text(77, 142, a->listing_keys ? "SSH keys" : "Profiles");
    } else if (id == TextForm) {
        static const Char *const prompt[] = {
            "Seed file on SD", "OpenSSH key file on SD", "Name for this key", "Public key destination on SD"
        };
        text(8, 40, prompt[a->text_action]);
    } else if (id == SecretForm) {
        static const Char *const prompt[] = {
            "SSH password", "Saved key passphrase", "OpenSSH key passphrase",
            "Choose a key passphrase", "Repeat the new passphrase"
        };
        text(8, 39, prompt[a->secret_action]);
        if (a->secret_action == ImportPassword) text(8, 52, "Leave blank for an unencrypted key.");
        else text(8, 52, "Enter with Graffiti or a keyboard.");
        draw_mask(a);
    } else if (id == TrustForm) {
        Char digest[44], first[25];
        fingerprint_text(a->detail + 4, digest);
        MemMove(first, digest, 24);
        first[24] = 0;
        text(8, 26, a->profile.host);
        text(8, 43, "Compare this SHA-256 fingerprint");
        text(8, 55, "with the server's trusted value.");
        text(8, 75, first);
        text(8, 88, digest + 24);
    } else if (id == BusyForm) {
        if (a->key_job) {
            text(8, 41, a->key_job == PT_KEY_SAVE ? "Protecting private key..." : "Unlocking private key...");
        } else {
            text(8, 41, a->profile.host);
            text(8, 56, a->login.pinned ? "Opening an SSH shell..." : "Reading the server host key...");
        }
    } else if (id == ShellForm) {
        shell_draw(a, true);
    }
}

static Boolean on_form(EventType *event)
{
    App *a = current();
    if (!a) return false;
    UInt16 id = form_id();
    if (event->eType == frmOpenEvent || event->eType == frmUpdateEvent) {
        draw_form(a, id, event->eType == frmOpenEvent);
        return true;
    }
    if (event->eType == keyDownEvent) {
        if (id == SecretForm) return secret_key(a, event);
        if (id == ShellForm) return shell_key(a, event);
        if (id == TextForm && event->data.keyDown.chr == chrCarriageReturn) {
            text_done(a);
            return true;
        }
        return false;
    }
    if (event->eType != ctlSelectEvent) return false;

    UInt16 action = event->data.ctlSelect.controlID;
    if (id == HomeForm) {
        if (action == Connect) connect(a);
        else {
            home_input(a);
            if (action == Profiles || action == Keys) go_list(a, action == Keys);
            else if (action == Seed) ask_text(a, InputSeed, "/PALMTERM.SEED");
            else if (action == ForgetHost) {
                UInt32 pin = port(a) ? pt_pin_find(a->db, &a->profile, NULL) : 0;
                if (!pin) notice("No saved host key for this host and port.");
                else if (confirm("Forget this host key? Verify its replacement before connecting.") &&
                         !pt_record_remove(a->db, pin, PT_RECORD_PIN))
                    notice("Could not remove the host key.");
            }
        }
    } else if (id == ListForm) {
        list_controls(a, action);
    } else if (id == ShellForm) {
        shell_controls(a, action);
    } else if (action == Accept) {
        if (id == TextForm) text_done(a);
        else if (id == SecretForm) secret_done(a);
        else if (id == TrustForm) trust(a);
    } else if (action == Cancel) {
        if (id == BusyForm && a->key_job) {
            UInt16 job = a->key_job;
            stop(a);
            if (job == PT_KEY_UNLOCK) FrmGotoForm(HomeForm);
            else go_list(a, true);
        } else if ((id == TextForm && a->text_action != InputSeed) ||
            (id == SecretForm && a->secret_action >= ImportPassword)) key_done(a);
        else {
            stop(a);
            FrmGotoForm(HomeForm);
        }
    }
    return true;
}

static Err on_sleep(SysNotifyParamType *parameters)
{
    App *a = parameters->userDataP;
    if (!a || a->deferred_sleep || a->stopped) return errNone;
    if (!a->pumping && !a->key_job && form_id() != SecretForm && form_id() != TextForm) return errNone;
    SleepEventParamType *sleep = parameters->notifyDetailsP;
    if (sleep) {
        ++sleep->deferSleep;
        a->deferred_sleep = true;
        EventType event;
        MemSet(&event, sizeof(event), 0);
        event.eType = SLEEP_EVENT;
        EvtAddEventToQueue(&event);
    }
    a->sleep_pending = true;
    return errNone;
}

UInt32 PilotMain(UInt16 command, MemPtr parameters, UInt16 launch_flags)
{
    (void)parameters;
    (void)launch_flags;
    if (command != sysAppLaunchCmdNormalLaunch) return errNone;
    UInt32 rom;
    if (FtrGet(sysFtrCreator, sysFtrNumROMVersion, &rom) ||
        rom < sysMakeROMVersion(5, 0, 0, 0, 0)) return sysErrRomIncompatible;
#ifdef PT_PROFILE
    UInt32 baseline = 0, largest = 0;
    MemHeapFreeBytes(0, &baseline, &largest);
#endif
    App *a = MemPtrNew(sizeof(*a));
    if (!a) return memErrNotEnoughSpace;
    MemSet(a, sizeof(*a), 0);
#ifdef PT_PROFILE
    a->heap_at_launch = baseline;
#endif
    a->net.socket = -1;
    a->rows = PT_GRID_ROWS;
    StrCopy(a->profile.port, "22");

    Err result = errNone;
    UInt16 bitmap_error = errNone;
    a->row_bitmap = BmpCreate(PT_GRID_COLS * 6, 10, 16, NULL, &bitmap_error);
    if (a->row_bitmap)
        a->row_display = BmpCreateBitmapV3(a->row_bitmap, kDensityDouble,
                                         BmpGetBits(a->row_bitmap), NULL);
    a->db = pt_store_open();
    Boolean registered = false, feature = false;
    UInt16 card = 0;
    LocalID db = 0;
    if (!a->db || !a->row_display || !pt_arm_open(&a->arm) ||
        FtrSet(PT_CREATOR, APP_FEATURE, (UInt32)a)) {
        notice("Palm Terminal could not initialize its native engine or database.");
        result = memErrNotEnoughSpace;
    } else {
        feature = true;
        registered = !SysCurAppDatabase(&card, &db) &&
                     !SysNotifyRegister(card, db, sysNotifySleepRequestEvent,
                                        on_sleep, sysNotifyNormalPriority, a);
        FrmGotoForm(HomeForm);
        do {
            EventType event;
            UInt16 menu_error = errNone;
            EvtGetEvent(&event, a->pumping || a->key_job ? 1 : evtWaitForever);
            if (event.eType == appStopEvent) break;
            if (a->sleep_pending) {
                a->sleep_pending = false;
                stop(a);
                FrmGotoForm(HomeForm);
                if (a->deferred_sleep) {
                    a->deferred_sleep = false;
                    EvtEnqueueKey(vchrResumeSleep, 0, commandKeyMask);
                }
                continue;
            }
            if (!SysHandleEvent(&event) && !MenuHandleEvent(NULL, &event, &menu_error)) {
                if (event.eType == frmLoadEvent) {
                    FormType *form = FrmInitForm(event.data.frmLoad.formID);
                    if (!form) break;
                    FrmSetActiveForm(form);
                    FrmSetEventHandler(form, on_form);
                } else {
                    FrmDispatchEvent(&event);
                }
            }
            pump(a);
        } while (true);
    }
    a->stopped = true;
    if (registered) SysNotifyUnregister(card, db, sysNotifySleepRequestEvent, sysNotifyNormalPriority);
    stop(a);
    if (feature) {
        FrmCloseAllForms();
        FtrUnregister(PT_CREATOR, APP_FEATURE);
    }
    if (a->db) DmCloseDatabase(a->db);
    if (a->row_display) BmpDelete((BitmapType *)a->row_display);
    if (a->row_bitmap) BmpDelete(a->row_bitmap);
    pt_arm_close(&a->arm);
    pt_wipe(a, sizeof(*a));
    MemPtrFree(a);
    return result;
}
