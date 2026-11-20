#include "palm.h"
#include <Chars.h>
#include <Graffiti.h>
#include <Keyboard.h>
#include <NotifyMgr.h>
#include <PenInputMgr.h>
#include "resource.h"

#define APP_FEATURE 0
#define PAGE_SIZE 5
#define SLEEP_EVENT (firstUserEvent + 1)
#define SSH_ENDED_EVENT (firstUserEvent + 2)
#define CONNECT_TIMEOUT 45UL
#define SHELL_TOOLBAR 15
#define SHELL_LINE 15

enum { InputSeed, InputImport, InputName, InputPublic };
enum { RemotePassword, KeyUnlock, ImportPassword, ProtectKey, ConfirmProtection };

typedef struct {
    PtArm arm;
    PtNet net;
    DmOpenRef db;
    PtProfile profile;
    PtLogin login;
    PtAppearance appearance, draft;
    PtKeyIO key_io;
    Char value[PT_PATH_MAX], secret[PT_PASSWORD_MAX], protection[PT_PASSWORD_MAX];
    Char name[PT_LABEL_MAX], line_draft[256];
    uint8_t detail[36];
    UInt32 rows_id[PAGE_SIZE];
    Char rows_text[PAGE_SIZE][42];
    Char *choices[PAGE_SIZE];
    UInt32 target_key, timestamp, cursor_deadline;
    UInt32 pin_version;
    Int32 ssh_end_reason;
    PtGrid grid;
    RectangleType drawn_bounds;
    UInt16 page, row_count, file_size, text_action, secret_action, key_job, cursor_interval;
    UInt16 view_width, view_height, input_area, bitmap_width, drawn_orientation;
    Boolean listing_keys, more, generating, pumping, ssh_started, shell, ctrl, typing, stopped;
    Boolean sleep_pending, deferred_sleep, cursor_on;
    Boolean dia, can_rotate, fullscreen, bar_hidden;
#ifdef PT_PROFILE
    UInt32 heap_at_launch, handshake_ticks, key_ticks;
#endif
    PtBitmap terminal, preview;
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

static RectangleType display_bounds(void)
{
    RectangleType bounds;
    WinGetBounds(WinGetDisplayWindow(), &bounds);
    return bounds;
}

static void post_display_change(void)
{
    EventType event = {.eType = winDisplayChangedEvent};
    EvtAddUniqueEventToQueue(&event, 0, true);
}

static Err on_display_change(SysNotifyParamType *parameters)
{
    (void)parameters;
    post_display_change();
    return errNone;
}

static void display_features(App *a)
{
    UInt32 flags = 0;
    if (FtrGet(pinCreator, pinFtrAPIVersion, &a->pin_version) || !a->pin_version) return;
    if (a->pin_version < pinAPIVersion1_1) {
        a->dia = true;
    } else if (!FtrGet(sysFtrCreator, sysFtrNumInputAreaFlags, &flags)) {
        a->dia = (flags & grfFtrInputAreaFlagCollapsible) != 0;
        a->can_rotate = (flags & grfFtrInputAreaFlagLandscape) != 0;
    }
}

static void configure_form(App *a, FormType *form)
{
    if (!a->dia && !a->can_rotate) return;
    FrmSetDIAPolicyAttr(form, frmDIAPolicyCustom);
    WinSetConstraintsSize(FrmGetWindowHandle(form), 160, 160, 240, 160, 160, 240);
}

static void leave_fullscreen(App *a)
{
    if (!a->fullscreen) return;
    a->fullscreen = false;
    if (a->bar_hidden) {
        StatShow();
        a->bar_hidden = false;
    }
    if (a->dia) {
        if (a->input_area <= pinInputAreaClosed) PINSetInputAreaState(a->input_area);
        PINSetInputTriggerState(pinInputTriggerEnabled);
    }
}

static void enter_fullscreen(App *a)
{
    a->fullscreen = true;
    if (!a->dia) return;
    a->input_area = PINGetInputAreaState();
    PINSetInputAreaState(pinInputAreaClosed);
    PINSetInputTriggerState(pinInputTriggerDisabled);
    if (a->pin_version >= pinAPIVersion1_1) {
        UInt32 visible;
        if (StatGetAttribute(statAttrBarVisible, &visible) == errNone && visible)
            a->bar_hidden = StatHide() == errNone;
    }
}

static UInt16 shell_height(const App *a, const RectangleType *bounds)
{
    return 2 * (bounds->extent.y - (a->fullscreen ? 0 : SHELL_TOOLBAR +
                                      (a->typing ? SHELL_LINE : 0)));
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
    leave_fullscreen(a);
    a->pumping = false;
    a->key_job = 0;
    a->ssh_started = false;
    a->shell = false;
    a->typing = false;
    a->ctrl = false;
    pt_wipe(a->line_draft, sizeof(a->line_draft));
    pt_bitmap_close(&a->preview);
    a->grid = (PtGrid){0};
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
    a->login.appearance = a->appearance;
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

static void open_settings(App *a)
{
    a->draft = a->appearance;
    if (a->shell && a->typing) get_field(ShellText, a->line_draft, sizeof(a->line_draft));
    FrmGotoForm(SettingsForm);
}

static void cursor_reset(App *a)
{
    a->cursor_deadline = TimGetTicks() + a->cursor_interval;
    if (!a->cursor_on && (a->appearance.flags & PT_CURSOR_BLINK))
        pt_call(&a->arm, PT_CURSOR_PHASE, NULL, 0, 1);
    a->cursor_on = true;
}

static void cursor_blink(App *a)
{
    if (!(a->appearance.flags & PT_CURSOR_BLINK) || a->appearance.shape == PT_CURSOR_HIDDEN)
        return;
    UInt32 now = TimGetTicks();
    if ((Int32)(now - a->cursor_deadline) < 0) return;
    a->cursor_on = !a->cursor_on;
    a->cursor_deadline = now + a->cursor_interval;
    pt_call(&a->arm, PT_CURSOR_PHASE, NULL, 0, a->cursor_on);
}

static void draw_form(App *a, UInt16 id, Boolean opening);

static Boolean apply_settings(App *a)
{
    if (!MemCmp(&a->appearance, &a->draft, sizeof(a->appearance))) return true;
    PtBitmap bitmap = {0};
    if (a->appearance.font != a->draft.font &&
        !pt_bitmap_open(&bitmap, a->bitmap_width, pt_font_height(a->draft.font))) {
        notice("There is not enough memory for this font size.");
        return false;
    }
    RectangleType bounds = display_bounds();
    PtGrid grid = pt_grid(&a->draft, 2 * bounds.extent.x, shell_height(a, &bounds));
    if (a->shell && pt_call(&a->arm, PT_APPEARANCE, &a->draft, grid.rows, grid.cols) != PT_OK) {
        pt_bitmap_close(&bitmap);
        notice("Could not apply the settings to this session.");
        return false;
    }
    if (bitmap.display) {
        pt_bitmap_close(&a->terminal);
        a->terminal = bitmap;
    }
    a->appearance = a->draft;
    a->grid = grid;
    a->cursor_on = true;
    cursor_reset(a);
    if (!pt_settings_save(&a->appearance))
        notice("Settings applied for this run. Palm OS could not save them.");
    return true;
}

static Boolean settings_event(App *a, EventType *event)
{
    if (event->eType == ctlSelectEvent) {
        UInt16 id = event->data.ctlSelect.controlID;
        if (id == SettingsSave || id == SettingsCancel) {
            if (id == SettingsSave && !apply_settings(a)) return true;
            pt_bitmap_close(&a->preview);
            FrmGotoForm(a->shell ? ShellForm : HomeForm);
            return true;
        }
    }
    if (!pt_settings_change(&a->draft, event)) return false;
    pt_settings_draw(&a->arm, &a->draft, &a->preview);
    return true;
}

static void shell_draw(App *a, Boolean all)
{
    if (!a->terminal.display) return;
    PtFrame frame = {0};
    if (pt_call(&a->arm, PT_DAMAGE, &frame, 0, 0) <= 0 && !all) return;
    if (frame.bell) SndPlaySystemSound(sndInfo);
    if (form_id() != ShellForm) return;

    UInt32 dirty[2] = {pt_le32(frame.rows), pt_le32(frame.rows + 4)};
    if (!(dirty[0] || dirty[1] || all)) return;
    UInt16 coords = WinSetCoordinateSystem(kCoordinatesNative);
    if (all) pt_paint_rgb(0, 0, a->view_width, a->view_height, a->appearance.background);
    const UInt16 height = pt_font_height(a->appearance.font);
    for (UInt16 row = 0; row < a->grid.rows; row++) {
        UInt32 raster = ((UInt32)a->view_width << 16) | row;
        if ((all || (dirty[row / 32] & (1UL << (row % 32)))) &&
            pt_call(&a->arm, PT_RASTER_ROW, a->terminal.bits, a->terminal.stride, raster) == PT_OK)
            WinDrawBitmap((BitmapType *)a->terminal.display, 0, row * height);
    }
    WinSetCoordinateSystem(coords);
}

static Int32 shell_layout(App *a, FormType *form, const RectangleType *bounds)
{
    UInt16 width = 2 * bounds->extent.x, height = shell_height(a, bounds);
    PtGrid grid = pt_grid(&a->appearance, width, height);
    if (width > a->bitmap_width || !pt_grid_valid(grid.rows, grid.cols)) return PT_ERROR;
    if (grid.rows != a->grid.rows || grid.cols != a->grid.cols) {
        Int32 result = pt_call(&a->arm, PT_RESIZE, NULL, grid.rows, grid.cols);
        if (result != PT_OK) return result;
    }
    a->grid = grid;
    a->view_width = width;
    a->view_height = height;

    const Boolean controls = !a->fullscreen;
    const Boolean typing = controls && a->typing;
    Coord toolbar = bounds->extent.y - SHELL_TOOLBAR + 1;
    static const UInt16 buttons[] = {End, ShellFull, ShellSettings, Type,
                                    Ctrl, Esc, Tab, ScrollUp, ScrollDown};
    Coord right = bounds->extent.x - 1;
    for (UInt16 i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
        UInt16 index = object_index(buttons[i]);
        RectangleType button;
        FrmGetObjectBounds(form, index, &button);
        Coord x = button.topLeft.x;
        if (i < 4) {
            x = right - button.extent.x;
            right = x - 1;
        }
        FrmSetObjectPosition(form, index, x, toolbar);
        show(buttons[i], controls);
    }

    UInt16 index = object_index(ShellText);
    RectangleType field;
    FrmGetObjectBounds(form, index, &field);
    Boolean width_changed = field.extent.x != bounds->extent.x - 45;
    field.extent.x = bounds->extent.x - 45;
    field.topLeft.y = toolbar - SHELL_LINE;
    FrmSetObjectBounds(form, index, &field);
    if (width_changed) FldRecalculateField(object(ShellText), false);
    FrmSetObjectPosition(form, object_index(Send), bounds->extent.x - 41, field.topLeft.y);
    if (!typing && FrmGetFocus(form) == index) FrmSetFocus(form, frmInvalidObjectId);
    show(ShellText, typing);
    show(Send, typing);
    if (typing && FrmGetFocus(form) != index) focus(ShellText);
    return PT_OK;
}

static void term_send(App *a, UInt32 key)
{
    UInt32 mods = a->ctrl ? PT_CTRL : 0;
    if (a->appearance.flags & PT_CURSOR_BLINK) cursor_reset(a);
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
    if (a->typing && !a->fullscreen && (!v || c == vchrRockerLeft || c == vchrRockerRight))
        return false;
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
    case ShellSettings:
        open_settings(a);
        break;
    case ShellFull:
        enter_fullscreen(a);
        draw_form(a, ShellForm, false);
        break;
    case Type:
        a->typing = !a->typing;
        set_field(ShellText, "");
        draw_form(a, ShellForm, false);
        if (a->typing) {
            SysKeyboardDialog(kbdAlpha);
            draw_form(a, ShellForm, false);
        }
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
    UInt16 active = form_id();
    if (!a->pumping || (active != BusyForm && active != ShellForm && active != SettingsForm))
        return;
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
        RectangleType bounds = display_bounds();
        a->login.grid = pt_grid(&a->appearance, 2 * bounds.extent.x, shell_height(a, &bounds));
        Int32 rc = pt_call(&a->arm, PT_CONNECT, &a->login, 0, 0);
        pt_wipe(a->login.password, sizeof(a->login.password));
        if (rc != PT_OK) {
            stop(a);
            code_notice("Could not initialize SSH", rc);
            FrmGotoForm(HomeForm);
            return;
        }
        a->grid = a->login.grid;
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
        a->cursor_on = true;
        cursor_reset(a);
        FrmGotoForm(ShellForm);
    } else if (result == PT_UNKNOWN_HOST) {
        stop(a);
        FrmGotoForm(TrustForm);
    } else if (result == PT_CHANGED_HOST) {
        stop(a);
        notice("Host key changed. Verify the new fingerprint before using Forget host.");
        FrmGotoForm(HomeForm);
    } else if (result < 0 || result == PT_CLOSED || (result == PT_IDLE && network == PT_NET_END)) {
        Int32 reason = network == PT_NET_FAIL ? a->net.error : (Int32)pt_le32(a->detail);
        stop(a);
        a->ssh_end_reason = result < 0 ? (reason ? reason : result) : 0;
        FrmGotoForm(HomeForm);
        /* Let Palm OS restore the display and draw Home before opening a dialog. */
        EventType event = {.eType = SSH_ENDED_EVENT};
        EvtAddEventToQueue(&event);
    } else if (a->shell) {
        if (active == ShellForm) cursor_blink(a);
        if (active == ShellForm || (a->appearance.flags & PT_AUDIBLE_BELL))
            shell_draw(a, false);
    }
}

static void draw_form(App *a, UInt16 id, Boolean opening)
{
    FormType *form = FrmGetActiveForm();
    RectangleType bounds = display_bounds();
    if (a->dia || a->can_rotate) {
        WinHandle window = FrmGetWindowHandle(form);
        RectangleType current_bounds;
        WinGetBounds(window, &current_bounds);
        if (MemCmp(&current_bounds, &bounds, sizeof(bounds))) WinSetBounds(window, &bounds);
    }
    a->drawn_bounds = bounds;
    if (a->can_rotate) a->drawn_orientation = SysGetOrientation();
    if (id == ShellForm) {
        if (opening && a->typing) {
            set_field(ShellText, a->line_draft);
            pt_wipe(a->line_draft, sizeof(a->line_draft));
        }
        if (opening && a->ctrl) CtlSetLabel(object(Ctrl), "Ctrl*");
        Int32 result = shell_layout(a, form, &bounds);
        if (result != PT_OK) {
            stop(a);
            code_notice("Could not resize the SSH terminal", result);
            FrmGotoForm(HomeForm);
            return;
        }
        WinEraseWindow();
        FrmDrawForm(form);
        shell_draw(a, true);
        return;
    }
    if (id == SettingsForm) {
        if (opening)
            pt_bitmap_open(&a->preview, PT_PREVIEW_WIDTH, PT_PREVIEW_HEIGHT);
        pt_settings_draw(&a->arm, &a->draft, &a->preview);
        return;
    }
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
    }
}

static Boolean on_form(EventType *event)
{
    App *a = current();
    if (!a) return false;
    UInt16 id = form_id();
    if (event->eType == winEnterEvent) {
        if ((a->dia || a->can_rotate) &&
            event->data.winEnter.enterWindow == FrmGetWindowHandle(FrmGetActiveForm())) {
            if (a->dia && !a->fullscreen) PINSetInputTriggerState(pinInputTriggerEnabled);
            if (a->can_rotate) SysSetOrientationTriggerState(sysOrientationTriggerEnabled);
            post_display_change();
        }
        return false;
    }
    if (event->eType == winDisplayChangedEvent) {
        if (WinGetActiveWindow() != FrmGetWindowHandle(FrmGetActiveForm())) return false;
        RectangleType bounds = display_bounds();
        if (MemCmp(&a->drawn_bounds, &bounds, sizeof(bounds)) ||
            (a->can_rotate && a->drawn_orientation != SysGetOrientation()))
            draw_form(a, id, false);
        return true;
    }
    if (event->eType == frmOpenEvent || event->eType == frmUpdateEvent) {
        draw_form(a, id, event->eType == frmOpenEvent);
        return true;
    }
    if (id == ShellForm && a->fullscreen && event->eType == penDownEvent) {
        leave_fullscreen(a);
        draw_form(a, ShellForm, false);
        return true;
    }
    if (id == SettingsForm) return settings_event(a, event);
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
            else if (action == HomeSettings) open_settings(a);
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
    pt_settings_load(&a->appearance);
    display_features(a);
    Boolean large_display = a->dia || a->can_rotate;
    a->bitmap_width = large_display ? PT_DISPLAY_LONG : PT_DISPLAY_SHORT;
    a->cursor_interval = SysTicksPerSecond() / 2;
    StrCopy(a->profile.port, "22");

    Err result = errNone;
    pt_bitmap_open(&a->terminal, a->bitmap_width, pt_font_height(a->appearance.font));
    a->db = pt_store_open();
    Boolean sleep_registered = false, display_registered = false, feature = false;
    UInt16 card = 0;
    LocalID db = 0;
    if (!a->db || !a->terminal.display || !pt_arm_open(&a->arm, large_display) ||
        FtrSet(PT_CREATOR, APP_FEATURE, (UInt32)a)) {
        notice("Palm Terminal could not initialize its native engine or database.");
        result = memErrNotEnoughSpace;
    } else {
        feature = true;
        if (!SysCurAppDatabase(&card, &db)) {
            sleep_registered = !SysNotifyRegister(card, db, sysNotifySleepRequestEvent,
                                                  on_sleep, sysNotifyNormalPriority, a);
            if (large_display)
                display_registered = !SysNotifyRegister(card, db, sysNotifyDisplayResizedEvent,
                                                         on_display_change, sysNotifyNormalPriority, a);
        }
        FrmGotoForm(HomeForm);
        do {
            EventType event;
            UInt16 menu_error = errNone;
            /* A queued key job can always do another round. Poll for Cancel
             * or sleep between rounds without sleeping for a tick each time. */
            Int32 wait = a->key_job ? 0 : a->pumping ? 1 : evtWaitForever;
            EvtGetEvent(&event, wait);
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
            if (event.eType == SSH_ENDED_EVENT) {
                if (a->ssh_end_reason) code_notice("SSH connection ended", a->ssh_end_reason);
                else notice("The SSH connection closed.");
                continue;
            }
            if (!SysHandleEvent(&event) && !MenuHandleEvent(NULL, &event, &menu_error)) {
                if (event.eType == frmLoadEvent) {
                    FormType *form = FrmInitForm(event.data.frmLoad.formID);
                    if (!form) break;
                    configure_form(a, form);
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
    if (display_registered)
        SysNotifyUnregister(card, db, sysNotifyDisplayResizedEvent, sysNotifyNormalPriority);
    if (sleep_registered)
        SysNotifyUnregister(card, db, sysNotifySleepRequestEvent, sysNotifyNormalPriority);
    stop(a);
    if (feature) {
        FrmCloseAllForms();
        FtrUnregister(PT_CREATOR, APP_FEATURE);
    }
    if (a->db) DmCloseDatabase(a->db);
    pt_bitmap_close(&a->terminal);
    pt_arm_close(&a->arm);
    pt_wipe(a, sizeof(*a));
    MemPtrFree(a);
    return result;
}
