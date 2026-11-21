#include "palm.h"
#include <Preferences.h>
#include <UIControls.h>
#include "resource.h"

enum { AppearancePreference = 1, AppearanceVersion = 1 };

Boolean pt_bitmap_open(PtBitmap *b, UInt16 width, UInt16 height)
{
    UInt16 error = errNone;
    b->bitmap = BmpCreate(width, height, 16, NULL, &error);
    if (!b->bitmap) return false;
    b->bits = BmpGetBits(b->bitmap);
    BmpGetDimensions(b->bitmap, NULL, NULL, &b->stride);
    if (b->bits)
        b->display = BmpCreateBitmapV3(b->bitmap, kDensityDouble, b->bits, NULL);
    if (b->display) return true;
    pt_bitmap_close(b);
    return false;
}

void pt_bitmap_close(PtBitmap *b)
{
    if (b->display) BmpDelete((BitmapType *)b->display);
    if (b->bitmap) BmpDelete(b->bitmap);
    MemSet(b, sizeof(*b), 0);
}

static Boolean read_saved(PtAppearance *appearance)
{
    UInt16 length = sizeof(*appearance);
    return PrefGetAppPreferences(PT_CREATOR, AppearancePreference, appearance, &length, true)
           == AppearanceVersion && length == sizeof(*appearance) &&
           pt_appearance_valid(appearance);
}

void pt_settings_load(PtAppearance *appearance)
{
    if (!read_saved(appearance)) *appearance = pt_appearance_default();
}

Boolean pt_settings_save(const PtAppearance *appearance)
{
    PtAppearance saved;
    PrefSetAppPreferences(PT_CREATOR, AppearancePreference, AppearanceVersion,
                          appearance, sizeof(*appearance), true);
    return read_saved(&saved) && !MemCmp(&saved, appearance, sizeof(saved));
}

static void *form_object(FormType *form, UInt16 id)
{
    return FrmGetObjectPtr(form, FrmGetObjectIndex(form, id));
}

static void set_popup(FormType *form, UInt16 trigger, UInt16 list_id, UInt8 value)
{
    ListType *list = form_object(form, list_id);
    LstSetSelection(list, value);
    CtlSetLabel(form_object(form, trigger), LstGetSelectionText(list, value));
}

void pt_paint_rgb(Coord x, Coord y, Coord width, Coord height, const uint8_t rgb[3])
{
    RGBColorType color = {0, rgb[0], rgb[1], rgb[2]};
    RectangleType rect;
    RctSetRectangle(&rect, x, y, width, height);
    WinPushDrawState();
    WinSetForeColorRGB(&color, NULL);
    WinDrawRectangle(&rect, 0);
    WinPopDrawState();
}

static void swatch(Coord x, Coord width, const uint8_t rgb[3])
{
    RectangleType rect;
    RctSetRectangle(&rect, x, 86, width, 6);
    pt_paint_rgb(x, 86, width, 6, rgb);
    WinDrawRectangleFrame(simpleFrame, &rect);
}

void pt_settings_draw(const PtArm *arm, const PtAppearance *a, PtBitmap *preview)
{
    FormType *form = FrmGetActiveForm();
    if (FrmGetFormId(form) == BehaviorForm) {
        set_popup(form, SettingsHistory, SettingsHistoryList, a->history);
        CtlSetValue(form_object(form, SettingsFollow), !!(a->flags & PT_FOLLOW_OUTPUT));
        CtlSetValue(form_object(form, SettingsBell), !!(a->flags & PT_AUDIBLE_BELL));
        CtlSetValue(form_object(form, SettingsBold), !!(a->flags & PT_BRIGHT_BOLD));
        FrmDrawForm(form);
        return;
    }
    set_popup(form, SettingsFont, SettingsFontList, a->font);
    set_popup(form, SettingsShape, SettingsShapeList, a->shape);
    CtlSetLabel(form_object(form, SettingsTheme), "Presets...");
    CtlSetValue(form_object(form, SettingsMatch), !!(a->flags & PT_CURSOR_AUTO));
    CtlSetValue(form_object(form, SettingsBlink), !!(a->flags & PT_CURSOR_BLINK));
    CtlSetEnabled(form_object(form, SettingsBlink), a->shape != PT_CURSOR_HIDDEN);
    FrmDrawForm(form);
    swatch(10, 33, a->text);
    swatch(52, 56, a->background);
    swatch(117, 35, a->flags & PT_CURSOR_AUTO ? a->text : a->cursor);

    if (!preview->display) return;
    PtPreview sample = {.appearance = *a};
    pt_put_le32(sample.pixels, (UInt32)preview->bits);
    if (pt_call(arm, PT_RASTER_PREVIEW, &sample, preview->stride, 0) == PT_OK) {
        UInt16 coords = WinSetCoordinateSystem(kCoordinatesNative);
        WinDrawBitmap((BitmapType *)preview->display, 16, 230);
        WinSetCoordinateSystem(coords);
    }
}

static Boolean pick_color(uint8_t rgb[3], const Char *title)
{
    RGBColorType color = {0, rgb[0], rgb[1], rgb[2]};
    IndexedColorType index = WinRGBToIndex(&color);
    if (!UIPickColor(&index, &color, UIPickColorStartRGB, title, NULL)) return false;
    rgb[0] = color.r;
    rgb[1] = color.g;
    rgb[2] = color.b;
    return true;
}

Boolean pt_settings_change(PtAppearance *a, const EventType *event)
{
    if (event->eType == popSelectEvent) {
        UInt16 id = event->data.popSelect.controlID;
        Int16 choice = event->data.popSelect.selection;
        if (id == SettingsFont && choice >= 0 && choice < PT_FONT_COUNT) a->font = choice;
        else if (id == SettingsShape && choice >= 0 && choice < PT_CURSOR_COUNT) a->shape = choice;
        else if (id == SettingsHistory && choice >= 0 && choice < PT_HISTORY_COUNT) a->history = choice;
        else if (id == SettingsTheme && choice >= 0 && choice < 3) {
            static const uint8_t themes[3][6] = {
                {232, 232, 232, 0, 0, 0}, {24, 24, 24, 250, 248, 240}, {255, 195, 92, 0, 0, 0}
            };
            MemMove(a->text, themes[choice], 3);
            MemMove(a->background, themes[choice] + 3, 3);
            a->flags |= PT_CURSOR_AUTO;
        }
        else return false;
        return true;
    }
    if (event->eType != ctlSelectEvent) return false;

    UInt8 flag;
    switch (event->data.ctlSelect.controlID) {
    case SettingsDefaults:
        *a = pt_appearance_default();
        return true;
    case SettingsText:
        pick_color(a->text, "Text color");
        return true;
    case SettingsBackground:
        pick_color(a->background, "Background color");
        return true;
    case SettingsCursor: {
        uint8_t color[3];
        MemMove(color, a->flags & PT_CURSOR_AUTO ? a->text : a->cursor, sizeof(color));
        if (pick_color(color, "Cursor color")) {
            MemMove(a->cursor, color, sizeof(color));
            a->flags &= ~PT_CURSOR_AUTO;
        }
        return true;
    }
    case SettingsMatch: flag = PT_CURSOR_AUTO; break;
    case SettingsBlink: flag = PT_CURSOR_BLINK; break;
    case SettingsFollow: flag = PT_FOLLOW_OUTPUT; break;
    case SettingsBell: flag = PT_AUDIBLE_BELL; break;
    case SettingsBold: flag = PT_BRIGHT_BOLD; break;
    default: return false;
    }
    if (event->data.ctlSelect.on) a->flags |= flag;
    else a->flags &= ~flag;
    return true;
}
