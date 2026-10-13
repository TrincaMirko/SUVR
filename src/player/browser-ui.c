#define _POSIX_C_SOURCE 200809L
#include "browser-ui.h"

#include "jellyfin.h"
#include "settings.h"
#include "thumbnails.h"
#include "ui-draw.h"
#include "webdav.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

/* Layout of the 2048 x 1024 panel: navigation card (left), grid card
 * (centre) with a pager below, detail card (right). */
#define GRID_COLUMNS 4U
#define GRID_ROWS 3U
#define PAGE_ENTRIES (GRID_COLUMNS * GRID_ROWS)

#define RAIL_X 24
#define CENTER_X 420
#define DETAIL_X 1652
#define CARD_Y 24
#define CARD_H 900
#define SIDE_W 372
#define CENTER_W 1208

#define GRID_X 452
#define GRID_Y 196
/* Landscape tiles (Jellyfin backdrops, else the top of the poster) with a
 * one-line title. */
#define TILE_W 270
#define THUMB_H 160
#define LABEL_SIZE 24
#define LABEL_LINE 32
#define TILE_H (THUMB_H + 6 + LABEL_LINE)
#define TILE_GAP_X 24
#define TILE_GAP_Y 18

#define PAGER_Y 940
#define PAGER_H 60
#define PAGE_BUTTONS 7

#define RAIL_ITEM_X 40
#define RAIL_ITEM_W 340
#define RAIL_ITEM_H 76
#define RAIL_FIRST_Y 172
#define RAIL_EXIT_Y 832

#define BACK_X 488
#define BACK_Y 74

/* Sort chip in the header and its menu. */
#define SORT_W 300
#define SORT_H 52
#define SORT_X (CENTER_X + CENTER_W - SORT_W - 32)
#define SORT_Y 48
#define MENU_ITEM_H 52
#define MENU_W 470
#define MENU_X (CENTER_X + CENTER_W - MENU_W - 32)
#define MENU_Y (SORT_Y + SORT_H + 8)
#define MENU_H (JF_SORT_COUNT * MENU_ITEM_H + 16)

/* Filter chip at the right end of the breadcrumb row and its menu. */
#define FILTER_W 190
#define FILTER_H 46
#define FILTER_X (CENTER_X + CENTER_W - 32 - FILTER_W)
#define FILTER_Y 126
#define FILTER_MENU_W 360
#define FILTER_MENU_X (FILTER_X + FILTER_W - FILTER_MENU_W)
#define FILTER_MENU_Y (FILTER_Y + FILTER_H + 8)
#define FILTER_MENU_H (4 * MENU_ITEM_H + 16)

/* Options overlay (language choice), opened from the rail. */
#define OPT_MENU_W 470
#define OPT_MENU_X (RAIL_ITEM_X + RAIL_ITEM_W + 16)
#define OPT_MENU_Y 300
#define OPT_HEADER_H 44
/* ponytail: hit rect bounded to 8 languages; extra files still draw and
 * select via the row math, the bound only caps the clickable area. */
#define OPT_MENU_H (OPT_HEADER_H + 8 * MENU_ITEM_H + 16)

/* On-screen keyboard tool: four rows of ten keys over a row of actions
 * (shift, symbol toggle, space, delete, cancel, confirm). Shift uppercases
 * the letter layer; the symbol layer carries punctuation and URL glyphs. */
#define KB_W 1120
#define KB_X (CENTER_X + (CENTER_W - KB_W) / 2)
#define KB_Y 180
#define KEY_W 96
#define KEY_H 66
#define KEY_GAP 10
#define KEY_TOP (KB_Y + 120)
#define KB_H (120 + 5 * (KEY_H + KEY_GAP) + 16)
#define KEY_ROWS 4
#define KEY_COLUMNS 10

static const char *const letter_keys[KEY_ROWS][KEY_COLUMNS] = {
    {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"},
    {"q", "w", "e", "r", "t", "y", "u", "i", "o", "p"},
    {"a", "s", "d", "f", "g", "h", "j", "k", "l", ";"},
    {"z", "x", "c", "v", "b", "n", "m", ",", ".", "/"},
};
static const char *const symbol_keys[KEY_ROWS][KEY_COLUMNS] = {
    {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"},
    {"!", "@", "#", "$", "%", "&", "*", "(", ")", "-"},
    {"_", "=", "+", "/", ":", ";", "\"", "'", "?", "~"},
    {",", ".", "<", ">", "[", "]", "{", "}", "|", "\\"},
};

/* "Resume or restart" prompt over the grid. */
#define PROMPT_W 680
#define PROMPT_H 300
#define PROMPT_X (CENTER_X + (CENTER_W - PROMPT_W) / 2)
#define PROMPT_Y 330
#define PROMPT_BUTTON_W 300
#define PROMPT_BUTTON_H 64
#define PROMPT_BUTTON_Y (PROMPT_Y + PROMPT_H - PROMPT_BUTTON_H - 32)
#define PROMPT_RESUME_X (PROMPT_X + 32)
#define PROMPT_RESTART_X (PROMPT_X + PROMPT_W - 32 - PROMPT_BUTTON_W)

/* Titles watched less than this are offered from the start only. */
#define RESUME_MINIMUM_US INT64_C(30000000)

/* Control ids returned by browser_ui_hit_test. */
enum {
    ID_TILE = 0, /* 0 .. PAGE_ENTRIES-1 */
    ID_PREVIOUS = 20,
    ID_NEXT = 21,
    ID_PAGE = 22, /* 22 .. 22+PAGE_BUTTONS-1 */
    ID_PAGE_AREA = 29, /* hit test only: resolved with the model */
    ID_BACK = 30,
    ID_RAIL = 40, /* 40 .. 40+RAIL_COUNT-1 */
    ID_EXIT = 49,
    ID_SORT = 50,       /* the chip */
    ID_SORT_ITEM = 60,  /* 60 .. 60+JF_SORT_COUNT-1 */
    ID_SORT_AREA = 79,  /* hit test only: resolved with the model */
    ID_RESUME = 80,
    ID_RESTART = 81,
    ID_PROMPT_AREA = 82, /* hit test only: resolved with the model */
    ID_FILTER = 83,
    ID_FILTER_ITEM = 84, /* 84 .. 87 */
    ID_FILTER_AREA = 88, /* hit test only: resolved with the model */
    ID_KEY = 100,        /* 100 .. 139: key grid row by row (4 x 10) */
    ID_KEY_SHIFT = 140,
    ID_KEY_SYMBOLS = 141,
    ID_KEY_SPACE = 142,
    ID_KEY_BACKSPACE = 143,
    ID_KEY_CANCEL = 144,
    ID_KEY_ENTER = 145,
    ID_KEYBOARD_AREA = 146, /* hit test only: resolved with the model */
    ID_OPTION_ITEM = 150,  /* 150 ..: one per language row */
    ID_OPTION_AREA = 190,  /* hit test only: resolved with the model */
};

/* What the stateless hit test reports (menus are open or not). */
enum {
    HIT_MENU = 1, HIT_PROMPT = 2, HIT_FILTER = 4, HIT_KEYBOARD = 8,
    HIT_OPTIONS = 16,
};

/* What the keyboard tool does when the user confirms (ID_KEY_ENTER). */
enum {
    KB_PURPOSE_NONE, KB_PURPOSE_SEARCH,
    KB_PURPOSE_WEBDAV_URL, KB_PURPOSE_WEBDAV_USER, KB_PURPOSE_WEBDAV_PASS,
};

/* Rectangle of keyboard control `id`; false for other ids. */
static bool key_rect(int id, int *x, int *y, int *width)
{
    *width = KEY_W;
    if (id >= ID_KEY && id < ID_KEY + KEY_ROWS * KEY_COLUMNS) {
        int index = id - ID_KEY;
        *x = KB_X + 26 + (index % KEY_COLUMNS) * (KEY_W + KEY_GAP);
        *y = KEY_TOP + (index / KEY_COLUMNS) * (KEY_H + KEY_GAP);
        return true;
    }
    static const int widths[6] = {140, 140, 300, 150, 140, 148};
    if (id < ID_KEY_SHIFT || id > ID_KEY_ENTER)
        return false;
    *x = KB_X + 26;
    for (int i = ID_KEY_SHIFT; i < id; ++i)
        *x += widths[i - ID_KEY_SHIFT] + KEY_GAP;
    *y = KEY_TOP + KEY_ROWS * (KEY_H + KEY_GAP);
    *width = widths[id - ID_KEY_SHIFT];
    return true;
}

/* The glyph a grid key types, honouring the shift and symbol layers. */
static void key_label(const struct browser_ui *ui, int id, char *out,
                      size_t size)
{
    int index = id - ID_KEY;
    const char *base = ui->kb_symbols
                           ? symbol_keys[index / KEY_COLUMNS]
                                        [index % KEY_COLUMNS]
                           : letter_keys[index / KEY_COLUMNS]
                                        [index % KEY_COLUMNS];
    snprintf(out, size, "%s", base);
    if (!ui->kb_symbols && ui->kb_shift && out[0] >= 'a' && out[0] <= 'z')
        out[0] = (char)(out[0] - 32);
}

static const char *const sort_labels[JF_SORT_COUNT] = {
    "sort.name", "sort.random", "sort.community", "sort.critic",
    "sort.added", "sort.played", "sort.agerating", "sort.playcount",
    "sort.released", "sort.runtime",
};

/* Dates, ratings and counts are most useful newest/highest first. */
static bool default_descending(int sort)
{
    return sort != JF_SORT_NAME && sort != JF_SORT_RANDOM &&
           sort != JF_SORT_OFFICIAL_RATING && sort != JF_SORT_RUNTIME;
}

static const char *const local_sort_labels[FILE_BROWSER_SORT_COUNT] = {
    "sort.name", "sort.modified", "sort.filesize",
};

static bool local_location(const struct browser_ui *ui)
{
    return ui->location[0] && strcmp(ui->location, "root:") != 0 &&
           strncmp(ui->location, "jf:", 3) != 0;
}

/* Folder listings of a Jellyfin library and local directories can be
 * sorted, each with its own criteria. */
static bool sortable(const struct browser_ui *ui)
{
    return strncmp(ui->location, "jf:view:", 8) == 0 || local_location(ui);
}

/* Library listings ("jf:view:FOLDER", optionally "/genre=ID") can be
 * filtered by genre, studio or person. */
static bool filterable(const struct browser_ui *ui)
{
    return strncmp(ui->location, "jf:view:", 8) == 0;
}

static bool filtered(const struct browser_ui *ui)
{
    return filterable(ui) && strchr(ui->location + 8, '/') != NULL;
}

static const char *const filter_labels[4] = {
    "filter.genres", "filter.studios", "filter.cast", "filter.clear",
};
static const char *const filter_keys[3] = {"genre", "studio", "person"};

static int filter_count(const struct browser_ui *ui)
{
    return filtered(ui) ? 4 : 3;
}

static int sort_count(const struct browser_ui *ui)
{
    return local_location(ui) ? FILE_BROWSER_SORT_COUNT : JF_SORT_COUNT;
}

static const char *sort_label(const struct browser_ui *ui, int sort)
{
    return tr(local_location(ui) ? local_sort_labels[sort] : sort_labels[sort]);
}

static int active_sort(const struct browser_ui *ui)
{
    return local_location(ui) ? ui->local_sort : ui->sort;
}

static bool active_descending(const struct browser_ui *ui)
{
    return local_location(ui) ? ui->local_sort_descending
                              : ui->sort_descending;
}

/* Random order has no direction. */
static bool has_direction(const struct browser_ui *ui, int sort)
{
    return local_location(ui) || sort != JF_SORT_RANDOM;
}

enum { RAIL_START, RAIL_JELLYFIN, RAIL_LOCAL, RAIL_WEBDAV, RAIL_SEARCH,
       RAIL_OPTIONS, RAIL_COUNT };
_Static_assert(ID_RAIL + RAIL_COUNT <= ID_EXIT, "rail ids overlap");

static const struct ui_color color_card = UI_RGBA(17, 18, 22, 240);
static const struct ui_color color_border = UI_RGBA(255, 255, 255, 18);
static const struct ui_color color_tile = UI_RGBA(32, 34, 41, 255);
static const struct ui_color color_tile_hover = UI_RGBA(46, 50, 60, 255);
static const struct ui_color color_text = UI_RGBA(236, 239, 244, 255);
static const struct ui_color color_label = UI_RGBA(205, 210, 220, 255);
static const struct ui_color color_dim = UI_RGBA(140, 147, 160, 255);
static const struct ui_color color_accent = UI_RGBA(56, 160, 255, 255);
static const struct ui_color color_accent_soft = UI_RGBA(56, 160, 255, 60);
static const struct ui_color color_chip = UI_RGBA(42, 45, 54, 255);
static const struct ui_color color_shadow = UI_RGBA(0, 0, 0, 110);

static size_t page_count(const struct browser_ui *ui)
{
    return ui->grid_count ? (ui->grid_count + PAGE_ENTRIES - 1U) / PAGE_ENTRIES
                          : 1U;
}

/* Listing index of grid slot `slot` on the current page, or -1. */
static int slot_entry(const struct browser_ui *ui, int slot)
{
    if (slot < 0 || slot >= (int)PAGE_ENTRIES)
        return -1;
    size_t position = ui->page * PAGE_ENTRIES + (size_t)slot;
    return position < ui->grid_count ? (int)ui->grid[position] : -1;
}

static int rail_active(const struct browser_ui *ui)
{
    if (strncmp(ui->location, "jf:search:", 10) == 0)
        return RAIL_SEARCH;
    if (strncmp(ui->location, "jf:", 3) == 0)
        return RAIL_JELLYFIN;
    if (strncmp(ui->location, "webdav:", 7) == 0)
        return RAIL_WEBDAV;
    if (!ui->location[0] || strcmp(ui->location, "root:") == 0)
        return RAIL_START;
    return RAIL_LOCAL;
}

static void tile_rect(int slot, int *x, int *y)
{
    *x = GRID_X + (slot % (int)GRID_COLUMNS) * (TILE_W + TILE_GAP_X);
    *y = GRID_Y + (slot / (int)GRID_COLUMNS) * (TILE_H + TILE_GAP_Y);
}

/* First page number button shown and how many there are. */
static void page_window(const struct browser_ui *ui, size_t *first,
                        size_t *count)
{
    size_t pages = page_count(ui);
    *count = pages < PAGE_BUTTONS ? pages : PAGE_BUTTONS;
    size_t start = ui->page >= PAGE_BUTTONS / 2 ? ui->page - PAGE_BUTTONS / 2
                                                : 0;
    if (start + *count > pages)
        start = pages - *count;
    *first = start;
}

static void page_button_rect(size_t count, size_t i, int *x, int *w)
{
    const int width = 64, gap = 10;
    int total = (int)count * width + ((int)count - 1) * gap;
    *x = CENTER_X + CENTER_W / 2 - total / 2 + (int)i * (width + gap);
    *w = width;
}

static const char *extension_badge(const struct file_browser_entry *entry,
                                   char badge[8])
{
    badge[0] = '\0';
    if (entry->kind != FILE_BROWSER_MEDIA || strncmp(entry->path, "jf:", 3) == 0)
        return badge;
    const char *dot = strrchr(entry->path, '.');
    if (!dot || strlen(dot + 1) > 4)
        return badge;
    size_t i = 0;
    for (const char *c = dot + 1; *c && i < 6; ++c)
        badge[i++] = (char)(*c >= 'a' && *c <= 'z' ? *c - 32 : *c);
    badge[i] = '\0';
    return badge;
}

static const char *format_duration(int64_t duration_us, char text[16])
{
    text[0] = '\0';
    if (duration_us <= 0)
        return text;
    int64_t seconds = duration_us / 1000000;
    if (seconds >= 3600)
        snprintf(text, 16, "%d:%02d:%02d", (int)(seconds / 3600),
                 (int)(seconds / 60 % 60), (int)(seconds % 60));
    else
        snprintf(text, 16, "%d:%02d", (int)(seconds / 60),
                 (int)(seconds % 60));
    return text;
}

/* ---- drawing ----------------------------------------------------------- */

static void draw_card(struct ui_canvas *canvas, int x, int width)
{
    ui_shadow_round_rect(canvas, x, CARD_Y + 8, width, CARD_H, 28, 28,
                         color_shadow);
    ui_fill_round_rect(canvas, x, CARD_Y, width, CARD_H, 28, color_card);
    ui_stroke_round_rect(canvas, x, CARD_Y, width, CARD_H, 28, 2,
                         color_border);
}

static void draw_rail_item(struct browser_ui *ui, struct ui_canvas *canvas,
                           int y, enum ui_icon icon, const char *label,
                           bool active, bool hover)
{
    ui_set_rect_raw(canvas, RAIL_ITEM_X, y, RAIL_ITEM_W, RAIL_ITEM_H,
                    ui->card_pixel);
    if (active || hover)
        ui_fill_round_rect(canvas, RAIL_ITEM_X, y, RAIL_ITEM_W, RAIL_ITEM_H,
                           18, hover ? color_tile_hover : color_tile);
    if (active)
        ui_fill_round_rect(canvas, RAIL_ITEM_X + 6, y + 18, 6,
                           RAIL_ITEM_H - 36, 3, color_accent);
    struct ui_color tint = active || hover ? color_text : color_dim;
    ui_draw_icon(canvas, icon, RAIL_ITEM_X + 50, y + RAIL_ITEM_H / 2.0f, 40,
                 active ? color_accent : tint);
    ui_draw_text(canvas, active ? ui->bold : ui->font, 30,
                 RAIL_ITEM_X + 88, y + 18, RAIL_ITEM_W - 100, UI_ALIGN_LEFT,
                 tint, label);
}

static void draw_rail(struct browser_ui *ui, struct ui_canvas *canvas,
                      int hover)
{
    static const struct {
        enum ui_icon icon;
        const char *label;
    } items[RAIL_COUNT] = {
        {UI_ICON_HOME, "rail.start"},
        {UI_ICON_SERVER, "rail.jellyfin"},
        {UI_ICON_DISK, "rail.local"},
        {UI_ICON_SERVER, "rail.webdav"},
        {UI_ICON_SEARCH, "rail.search"},
        {UI_ICON_INFO, "rail.options"},
    };
    int active = rail_active(ui);
    for (int i = 0; i < RAIL_COUNT; ++i)
        if (hover < 0 || hover == ID_RAIL + i)
            draw_rail_item(ui, canvas,
                           RAIL_FIRST_Y + i * (RAIL_ITEM_H + 12),
                           items[i].icon, tr(items[i].label),
                           i == active || (i == RAIL_OPTIONS &&
                                           ui->options_open),
                           hover == ID_RAIL + i);
    if (hover < 0 || hover == ID_EXIT)
        draw_rail_item(ui, canvas, RAIL_EXIT_Y, UI_ICON_POWER,
                       tr("rail.exit"), false, hover == ID_EXIT);
}

/* Options overlay: a titled card with one row per installed language. */
static void draw_options_menu(struct browser_ui *ui, struct ui_canvas *canvas,
                              int hover)
{
    int count = ui->language_count;
    int height = OPT_HEADER_H + count * MENU_ITEM_H + 16;
    ui_shadow_round_rect(canvas, OPT_MENU_X, OPT_MENU_Y + 6, OPT_MENU_W,
                         height, 22, 24, color_shadow);
    ui_fill_round_rect(canvas, OPT_MENU_X, OPT_MENU_Y, OPT_MENU_W, height, 22,
                       UI_RGBA(30, 32, 39, 255));
    ui_stroke_round_rect(canvas, OPT_MENU_X, OPT_MENU_Y, OPT_MENU_W, height,
                         22, 2, color_border);
    ui_draw_text(canvas, ui->bold, 25, OPT_MENU_X + 28, OPT_MENU_Y + 12,
                 OPT_MENU_W - 56, UI_ALIGN_LEFT, color_dim,
                 tr("options.language"));
    for (int i = 0; i < count; ++i) {
        int y = OPT_MENU_Y + OPT_HEADER_H + 8 + i * MENU_ITEM_H;
        bool current = strcmp(ui->languages[i].code, i18n_current()) == 0;
        if (hover == ID_OPTION_ITEM + i)
            ui_fill_round_rect(canvas, OPT_MENU_X + 8, y, OPT_MENU_W - 16,
                               MENU_ITEM_H, 14, color_tile_hover);
        ui_draw_text(canvas, current ? ui->bold : ui->font, 25,
                     OPT_MENU_X + 28, y + 11, OPT_MENU_W - 56, UI_ALIGN_LEFT,
                     current ? color_accent : color_text,
                     ui->languages[i].name);
    }
}

static void draw_tile(struct browser_ui *ui, struct ui_canvas *canvas,
                      int slot, bool hover)
{
    int index = slot_entry(ui, slot);
    if (index < 0)
        return;
    const struct file_browser_entry *entry = &ui->listing.entries[index];
    int x, y;
    tile_rect(slot, &x, &y);
    if (hover) /* start from the card colour so the outline is clean */
        ui_set_rect_raw(canvas, x - 8, y - 8, TILE_W + 16, TILE_H + 12,
                        ui->card_pixel);
    const struct ui_image *thumb = ui->thumbs ? ui->thumbs[index] : NULL;
    if (thumb) {
        ui_draw_image(canvas, thumb, x, y, TILE_W, THUMB_H, 14,
                      hover ? 0.0f : 0.12f);
    } else {
        ui_fill_round_rect(canvas, x, y, TILE_W, THUMB_H, 14,
                           hover ? color_tile_hover : color_tile);
        ui_draw_icon(canvas,
                     entry->kind == FILE_BROWSER_MEDIA ? UI_ICON_VIDEO
                                                       : UI_ICON_FOLDER,
                     x + TILE_W / 2.0f, y + THUMB_H / 2.0f, 64,
                     hover ? color_accent : color_dim);
    }
    char badge[8];
    if (extension_badge(entry, badge)[0]) {
        int width = ui_text_width(ui->bold, 18, badge) + 18;
        ui_fill_round_rect(canvas, x + 10, y + THUMB_H - 38, width, 28, 8,
                           UI_RGBA(0, 0, 0, 170));
        ui_draw_text(canvas, ui->bold, 18, x + 19, y + THUMB_H - 35, 0,
                     UI_ALIGN_LEFT, color_text, badge);
    }
    char duration[16];
    if (format_duration(entry->duration_us, duration)[0]) {
        int width = ui_text_width(ui->bold, 18, duration) + 18;
        ui_fill_round_rect(canvas, x + TILE_W - 10 - width, y + THUMB_H - 38,
                           width, 28, 8, UI_RGBA(0, 0, 0, 170));
        ui_draw_text(canvas, ui->bold, 18, x + TILE_W - 19, y + THUMB_H - 35,
                     0, UI_ALIGN_RIGHT, color_text, duration);
    }
    if (entry->played) {
        const char *seen = tr("badge.watched");
        int width = ui_text_width(ui->bold, 18, seen) + 18;
        ui_fill_round_rect(canvas, x + TILE_W - 10 - width, y + 10, width, 28,
                           8, UI_RGBA(0, 0, 0, 170));
        ui_draw_text(canvas, ui->bold, 18, x + TILE_W - 19, y + 13, 0,
                     UI_ALIGN_RIGHT, color_accent, seen);
    }
    if (entry->resume_us > 0 && entry->duration_us > 0) {
        float share = (float)entry->resume_us / (float)entry->duration_us;
        share = share < 0.02f ? 0.02f : share > 1.0f ? 1.0f : share;
        ui_fill_round_rect(canvas, x + 10, y + THUMB_H - 8, TILE_W - 20, 5,
                           2.5f, UI_RGBA(0, 0, 0, 170));
        ui_fill_round_rect(canvas, x + 10, y + THUMB_H - 8,
                           (TILE_W - 20) * share, 5, 2.5f, color_accent);
    }
    if (hover)
        ui_stroke_round_rect(canvas, x - 5, y - 5, TILE_W + 10, THUMB_H + 10,
                             18, 4, color_accent);
    ui_draw_text_wrapped(canvas, hover ? ui->bold : ui->font, LABEL_SIZE,
                         x - 4, y + THUMB_H + 6, TILE_W + 8, LABEL_LINE, 1,
                         UI_ALIGN_CENTER, hover ? color_text : color_label,
                         entry->name);
}

static void draw_breadcrumbs(struct browser_ui *ui, struct ui_canvas *canvas)
{
    const char *path = ui->listing.path ? ui->listing.path : "";
    char segment[256];
    int x = GRID_X;
    const char *cursor = path;
    const int limit = filterable(ui) ? FILTER_X - 16 : CENTER_X + CENTER_W - 60;
    while (*cursor && x < limit - 60) {
        const char *separator = strstr(cursor, " / ");
        size_t length = separator ? (size_t)(separator - cursor)
                                  : strlen(cursor);
        if (length >= sizeof(segment))
            length = sizeof(segment) - 1;
        memcpy(segment, cursor, length);
        segment[length] = '\0';
        bool last = !separator;
        int width = ui_text_width(last ? ui->bold : ui->font, 24, segment) +
                    44;
        if (width > 420)
            width = 420;
        if (x + width > limit)
            width = limit - x;
        ui_fill_round_rect(canvas, x, 126, width, 46, 23,
                           last ? color_accent : color_chip);
        ui_draw_text(canvas, last ? ui->bold : ui->font, 24, x + width / 2,
                     134, width - 32, UI_ALIGN_CENTER,
                     last ? UI_RGBA(255, 255, 255, 255) : color_text, segment);
        x += width + 10;
        if (!separator)
            break;
        cursor = separator + 3;
    }
}

static const char *current_title(const struct browser_ui *ui)
{
    const char *path = ui->listing.path;
    if (!path || !path[0])
        return tr("status.loading");
    const char *last = path;
    for (const char *s = strstr(path, " / "); s; s = strstr(s + 3, " / "))
        last = s + 3;
    return last;
}

static void draw_back_button(struct browser_ui *ui, struct ui_canvas *canvas,
                             bool hover)
{
    if (ui->parent < 0)
        return;
    ui_fill_circle(canvas, BACK_X, BACK_Y, 32,
                   hover ? color_accent : color_chip);
    ui_draw_icon(canvas, UI_ICON_BACK, BACK_X, BACK_Y, 34, color_text);
}

static void draw_sort_chip(struct browser_ui *ui, struct ui_canvas *canvas,
                           bool hover)
{
    if (!sortable(ui))
        return;
    ui_set_rect_raw(canvas, SORT_X - 4, SORT_Y - 4, SORT_W + 8, SORT_H + 8,
                    ui->card_pixel);
    ui_fill_round_rect(canvas, SORT_X, SORT_Y, SORT_W, SORT_H, SORT_H / 2.0f,
                       hover || ui->sort_menu_open ? color_tile_hover
                                                   : color_chip);
    ui_draw_text(canvas, ui->font, 24, SORT_X + 24, SORT_Y + 10,
                 SORT_W - 112, UI_ALIGN_LEFT, color_text,
                 sort_label(ui, active_sort(ui)));
    if (has_direction(ui, active_sort(ui)))
        ui_draw_icon(canvas,
                     active_descending(ui) ? UI_ICON_ARROW_DOWN
                                           : UI_ICON_ARROW_UP,
                     SORT_X + SORT_W - 70, SORT_Y + SORT_H / 2.0f, 28,
                     color_accent);
    ui_draw_icon(canvas, UI_ICON_CHEVRON_DOWN, SORT_X + SORT_W - 32,
                 SORT_Y + SORT_H / 2.0f, 28, color_dim);
}

static void draw_filter_chip(struct browser_ui *ui, struct ui_canvas *canvas,
                             bool hover)
{
    if (!filterable(ui))
        return;
    ui_set_rect_raw(canvas, FILTER_X - 4, FILTER_Y - 4, FILTER_W + 8,
                    FILTER_H + 8, ui->card_pixel);
    bool active = filtered(ui);
    ui_fill_round_rect(canvas, FILTER_X, FILTER_Y, FILTER_W, FILTER_H,
                       FILTER_H / 2.0f,
                       hover || ui->filter_menu_open ? color_tile_hover
                       : active                      ? color_accent_soft
                                                     : color_chip);
    ui_draw_text(canvas, active ? ui->bold : ui->font, 24, FILTER_X + 24,
                 FILTER_Y + 8, FILTER_W - 72, UI_ALIGN_LEFT,
                 active ? color_accent : color_text,
                 active ? tr("filter.active") : tr("filter.chip"));
    ui_draw_icon(canvas, UI_ICON_CHEVRON_DOWN, FILTER_X + FILTER_W - 30,
                 FILTER_Y + FILTER_H / 2.0f, 26, color_dim);
}

static void draw_filter_menu(struct browser_ui *ui, struct ui_canvas *canvas,
                             int hover)
{
    int count = filter_count(ui);
    int height = count * MENU_ITEM_H + 16;
    ui_shadow_round_rect(canvas, FILTER_MENU_X, FILTER_MENU_Y + 6,
                         FILTER_MENU_W, height, 22, 24, color_shadow);
    ui_fill_round_rect(canvas, FILTER_MENU_X, FILTER_MENU_Y, FILTER_MENU_W,
                       height, 22, UI_RGBA(30, 32, 39, 255));
    ui_stroke_round_rect(canvas, FILTER_MENU_X, FILTER_MENU_Y, FILTER_MENU_W,
                         height, 22, 2, color_border);
    for (int i = 0; i < count; ++i) {
        int y = FILTER_MENU_Y + 8 + i * MENU_ITEM_H;
        if (hover == ID_FILTER_ITEM + i)
            ui_fill_round_rect(canvas, FILTER_MENU_X + 8, y,
                               FILTER_MENU_W - 16, MENU_ITEM_H, 14,
                               color_tile_hover);
        ui_draw_text(canvas, ui->font, 25, FILTER_MENU_X + 28, y + 11,
                     FILTER_MENU_W - 56, UI_ALIGN_LEFT,
                     i == 3 ? color_accent : color_text, tr(filter_labels[i]));
    }
}

static void draw_sort_menu(struct browser_ui *ui, struct ui_canvas *canvas,
                           int hover)
{
    int count = sort_count(ui);
    int height = count * MENU_ITEM_H + 16;
    ui_shadow_round_rect(canvas, MENU_X, MENU_Y + 6, MENU_W, height, 22, 24,
                         color_shadow);
    ui_fill_round_rect(canvas, MENU_X, MENU_Y, MENU_W, height, 22,
                       UI_RGBA(30, 32, 39, 255));
    ui_stroke_round_rect(canvas, MENU_X, MENU_Y, MENU_W, height, 22, 2,
                         color_border);
    for (int i = 0; i < count; ++i) {
        int y = MENU_Y + 8 + i * MENU_ITEM_H;
        bool current = i == active_sort(ui);
        if (hover == ID_SORT_ITEM + i)
            ui_fill_round_rect(canvas, MENU_X + 8, y, MENU_W - 16,
                               MENU_ITEM_H, 14, color_tile_hover);
        ui_draw_text(canvas, current ? ui->bold : ui->font, 25, MENU_X + 28,
                     y + 11, MENU_W - 96, UI_ALIGN_LEFT,
                     current ? color_accent : color_text,
                     sort_label(ui, i));
        if (current && has_direction(ui, i))
            ui_draw_icon(canvas,
                         active_descending(ui) ? UI_ICON_ARROW_DOWN
                                               : UI_ICON_ARROW_UP,
                         MENU_X + MENU_W - 40, y + MENU_ITEM_H / 2.0f, 28,
                         color_accent);
    }
}

static void draw_pager(struct browser_ui *ui, struct ui_canvas *canvas,
                       int hover)
{
    if (page_count(ui) <= 1)
        return;
    if (hover < 0 || hover == ID_PREVIOUS) {
        bool enabled = ui->page > 0;
        ui_fill_round_rect(canvas, CENTER_X, PAGER_Y, 190, PAGER_H, 16,
                           hover == ID_PREVIOUS && enabled ? color_tile_hover
                                                           : color_card);
        ui_draw_icon(canvas, UI_ICON_CHEVRON_LEFT, CENTER_X + 34,
                     PAGER_Y + PAGER_H / 2.0f, 30,
                     enabled ? color_text : color_dim);
        ui_draw_text(canvas, ui->font, 26, CENTER_X + 58, PAGER_Y + 13, 0,
                     UI_ALIGN_LEFT, enabled ? color_text : color_dim,
                     tr("pager.previous"));
    }
    if (hover < 0 || hover == ID_NEXT) {
        bool enabled = ui->page + 1 < page_count(ui);
        int x = CENTER_X + CENTER_W - 190;
        ui_fill_round_rect(canvas, x, PAGER_Y, 190, PAGER_H, 16,
                           hover == ID_NEXT && enabled ? color_tile_hover
                                                       : color_card);
        ui_draw_text(canvas, ui->font, 26, x + 136, PAGER_Y + 13, 0,
                     UI_ALIGN_RIGHT, enabled ? color_text : color_dim,
                     tr("pager.next"));
        ui_draw_icon(canvas, UI_ICON_CHEVRON_RIGHT, x + 158,
                     PAGER_Y + PAGER_H / 2.0f, 30,
                     enabled ? color_text : color_dim);
    }
    size_t first, count;
    page_window(ui, &first, &count);
    for (size_t i = 0; i < count; ++i) {
        int id = ID_PAGE + (int)i;
        if (hover >= 0 && hover != id)
            continue;
        int x, width;
        page_button_rect(count, i, &x, &width);
        bool current = first + i == ui->page;
        ui_fill_round_rect(canvas, x, PAGER_Y, width, PAGER_H, 16,
                           current ? color_accent
                           : hover == id ? color_tile_hover
                                         : color_card);
        char number[16];
        snprintf(number, sizeof(number), "%zu", first + i + 1);
        ui_draw_text(canvas, current ? ui->bold : ui->font, 26,
                     x + width / 2, PAGER_Y + 13, 0, UI_ALIGN_CENTER,
                     color_text, number);
    }
}

static void draw_base(struct browser_ui *ui, struct ui_canvas *canvas)
{
    ui_clear(canvas, UI_RGBA(0, 0, 0, 0));
    draw_card(canvas, RAIL_X, SIDE_W);
    draw_card(canvas, CENTER_X, CENTER_W);
    draw_card(canvas, DETAIL_X, SIDE_W);
    /* Card background (over its shadow) for resetting regions later. */
    ui_get_pixel_raw(canvas, CENTER_X + 200, CARD_Y + 400, ui->card_pixel);

    ui_draw_text(canvas, ui->bold, 42, 60, 52, 0, UI_ALIGN_LEFT, color_text,
                 "Stereonaut");
    ui_draw_text(canvas, ui->font, 24, 62, 108, 0, UI_ALIGN_LEFT, color_dim,
                 tr("app.subtitle"));
    draw_rail(ui, canvas, -1);

    draw_back_button(ui, canvas, false);
    ui_draw_text(canvas, ui->bold, 40, CENTER_X + CENTER_W / 2, 46,
                 sortable(ui) ? 480 : 820, UI_ALIGN_CENTER, color_text,
                 current_title(ui));
    draw_sort_chip(ui, canvas, false);
    draw_filter_chip(ui, canvas, false);
    draw_breadcrumbs(ui, canvas);
    for (int slot = 0; slot < (int)PAGE_ENTRIES; ++slot)
        draw_tile(ui, canvas, slot, false);
    if (!ui->grid_count && ui->listing.path)
        ui_draw_text(canvas, ui->font, 30, CENTER_X + CENTER_W / 2, 480, 0,
                     UI_ALIGN_CENTER, color_dim, tr("status.empty"));

    const char *message = ui->listing.error[0] ? ui->listing.error
                                               : ui->status_message;
    if (message[0]) {
        ui_fill_round_rect(canvas, GRID_X, 856, CENTER_W - 64, 52, 16,
                           color_accent_soft);
        ui_draw_icon(canvas, UI_ICON_INFO, GRID_X + 32, 882, 32, color_text);
        ui_draw_text(canvas, ui->font, 26, GRID_X + 62, 866, CENTER_W - 140,
                     UI_ALIGN_LEFT, color_text, message);
    }
    if (page_count(ui) > 1) {
        int track = (int)GRID_ROWS * (TILE_H + TILE_GAP_Y) - TILE_GAP_Y;
        int height = track / (int)page_count(ui);
        if (height < 40)
            height = 40;
        int top = GRID_Y + (int)((size_t)(track - height) * ui->page /
                                 (page_count(ui) - 1));
        ui_fill_round_rect(canvas, CENTER_X + CENTER_W - 20, GRID_Y, 6,
                           track, 3, color_chip);
        ui_fill_round_rect(canvas, CENTER_X + CENTER_W - 20, top, 6, height,
                           3, color_dim);
    }
    draw_pager(ui, canvas, -1);
}

static void draw_detail(struct browser_ui *ui, struct ui_canvas *canvas)
{
    const int x = DETAIL_X + 24, width = SIDE_W - 48;
    ui_draw_text(canvas, ui->font, 24, x, 48, 0, UI_ALIGN_LEFT, color_dim,
                 tr("detail.title"));
    int index = ui->detail;
    if (index < 0 || (size_t)index >= ui->listing.entry_count) {
        ui_draw_text_wrapped(canvas, ui->font, 26, x, 110, width, 36, 4,
                             UI_ALIGN_LEFT, color_dim,
                             tr("detail.hint"));
        return;
    }
    const struct file_browser_entry *entry = &ui->listing.entries[index];
    const struct ui_image *thumb = ui->thumbs ? ui->thumbs[index] : NULL;
    const int image_y = 92, image_h = width * 9 / 16;
    if (thumb) {
        ui_draw_image(canvas, thumb, x, image_y, width, image_h, 16, 0.0f);
    } else {
        ui_fill_round_rect(canvas, x, image_y, width, image_h, 16,
                           color_tile);
        ui_draw_icon(canvas,
                     entry->kind == FILE_BROWSER_MEDIA ? UI_ICON_VIDEO
                                                       : UI_ICON_FOLDER,
                     x + width / 2.0f, image_y + image_h / 2.0f, 96,
                     color_dim);
    }
    const int title_y = image_y + image_h + 28;
    int lines = ui_draw_text_wrapped(canvas, ui->bold, 30, x, title_y, width,
                                     40, 4, UI_ALIGN_LEFT, color_text,
                                     entry->name);
    int y = title_y + lines * 40 + 24;
    bool jellyfin = strncmp(entry->path, "jf:", 3) == 0;
    char badge[8], duration[16], year[16] = "", size[40] = "", resume[16];
    char file_size[24] = "", modified[24] = "";
    if (entry->size > 0) {
        double megabytes = (double)entry->size / (1024.0 * 1024.0);
        if (megabytes >= 1024.0)
            snprintf(file_size, sizeof(file_size), "%.1f GB",
                     megabytes / 1024.0);
        else
            snprintf(file_size, sizeof(file_size), "%.0f MB", megabytes);
        char *point = strchr(file_size, '.');
        if (point)
            *point = ','; /* German decimal comma */
    }
    if (entry->modified > 0) {
        time_t when = (time_t)entry->modified;
        struct tm local;
        if (localtime_r(&when, &local))
            strftime(modified, sizeof(modified), "%d.%m.%Y", &local);
    }
    if (entry->year > 0)
        snprintf(year, sizeof(year), "%d", entry->year);
    if (entry->width > 0 && entry->height > 0)
        snprintf(size, sizeof(size), "%d \xc3\x97 %d", entry->width,
                 entry->height);
    struct {
        const char *label, *value;
    } rows[] = {
        {"detail.type", entry->kind == FILE_BROWSER_MEDIA ? tr("value.video")
                                                          : tr("value.folder")},
        {"detail.duration", format_duration(entry->duration_us, duration)},
        {"detail.stopped", format_duration(entry->resume_us, resume)},
        {"detail.watched", entry->played ? tr("value.yes") : ""},
        {"detail.year", year},
        {"detail.source", jellyfin ? "Jellyfin" : tr("value.local")},
        {"detail.format", extension_badge(entry, badge)},
        {"detail.codec", entry->codec},
        {"detail.size", size},
        {"detail.file", file_size},
        {"detail.modified", modified},
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        if (!rows[i].value[0])
            continue;
        ui_draw_text(canvas, ui->font, 24, x, y, 0, UI_ALIGN_LEFT, color_dim,
                     tr(rows[i].label));
        ui_draw_text(canvas, ui->bold, 24, x + 120, y, width - 120,
                     UI_ALIGN_LEFT, color_text, rows[i].value);
        y += 40;
    }
    if (entry->kind == FILE_BROWSER_MEDIA)
        ui_draw_text_wrapped(canvas, ui->font, 24, x, CARD_Y + CARD_H - 80,
                             width, 32, 2, UI_ALIGN_LEFT, color_dim,
                             tr("detail.play_hint"));
}

static void draw_keyboard(struct browser_ui *ui, struct ui_canvas *canvas,
                          int hover)
{
    ui_fill_round_rect(canvas, CENTER_X, CARD_Y, CENTER_W, CARD_H, 28,
                       UI_RGBA(0, 0, 0, 150));
    ui_shadow_round_rect(canvas, KB_X, KB_Y + 8, KB_W, KB_H, 26, 28,
                         color_shadow);
    ui_fill_round_rect(canvas, KB_X, KB_Y, KB_W, KB_H, 26,
                       UI_RGBA(30, 32, 39, 255));
    ui_stroke_round_rect(canvas, KB_X, KB_Y, KB_W, KB_H, 26, 2,
                         color_border);
    /* Title + the active field value (masked for passwords) with a caret. */
    bool search = ui->kb_purpose == KB_PURPOSE_SEARCH;
    const char *value = ui->kb_target ? ui->kb_target : "";
    char masked[64];
    if (ui->kb_mask) {
        size_t n = strlen(value);
        if (n >= sizeof(masked))
            n = sizeof(masked) - 1;
        memset(masked, '*', n);
        masked[n] = '\0';
        value = masked;
    }
    ui_fill_round_rect(canvas, KB_X + 26, KB_Y + 24, KB_W - 52, 72, 20,
                       color_tile);
    if (search) {
        ui_draw_icon(canvas, UI_ICON_SEARCH, KB_X + 66, KB_Y + 60, 34,
                     color_dim);
        char shown[96];
        snprintf(shown, sizeof(shown), "%s|", value);
        ui_draw_text(canvas, ui->font, 32, KB_X + 100, KB_Y + 40, KB_W - 160,
                     UI_ALIGN_LEFT, value[0] ? color_text : color_dim,
                     value[0] ? shown : tr("search.placeholder"));
    } else {
        ui_draw_text(canvas, ui->bold, 26, KB_X + 40, KB_Y + 36, KB_W - 320,
                     UI_ALIGN_LEFT, color_dim, ui->kb_title);
        char shown[600];
        snprintf(shown, sizeof(shown), "%s|", value);
        ui_draw_text(canvas, ui->font, 30, KB_X + 320, KB_Y + 38,
                     KB_W - 360, UI_ALIGN_LEFT, color_text, shown);
    }
    for (int id = ID_KEY; id <= ID_KEY_ENTER; ++id) {
        int x, y, width;
        if (!key_rect(id, &x, &y, &width))
            continue;
        bool focused = hover == id;
        bool primary = id == ID_KEY_ENTER;
        bool toggled = (id == ID_KEY_SHIFT && ui->kb_shift) ||
                       (id == ID_KEY_SYMBOLS && ui->kb_symbols);
        ui_fill_round_rect(canvas, x, y, width, KEY_H, 14,
                           primary ? (focused ? UI_RGBA(90, 180, 255, 255)
                                              : color_accent)
                           : toggled  ? color_accent_soft
                           : focused  ? color_tile_hover
                                      : color_chip);
        char glyph[8];
        const char *label;
        if (id < ID_KEY_SHIFT) {
            key_label(ui, id, glyph, sizeof(glyph));
            label = glyph;
        } else if (id == ID_KEY_SHIFT) {
            label = tr("key.shift");
        } else if (id == ID_KEY_SYMBOLS) {
            label = ui->kb_symbols ? tr("key.letters") : tr("key.symbols");
        } else if (id == ID_KEY_SPACE) {
            label = tr("key.space");
        } else if (id == ID_KEY_BACKSPACE) {
            label = tr("key.backspace");
        } else if (id == ID_KEY_CANCEL) {
            label = tr("key.cancel");
        } else {
            label = search ? tr("key.search")
                   : ui->kb_purpose == KB_PURPOSE_WEBDAV_PASS
                         ? tr("key.save")
                         : tr("key.next");
        }
        ui_draw_text(canvas, id < ID_KEY_SHIFT ? ui->bold : ui->font, 28,
                     x + width / 2, y + 14, width - 12, UI_ALIGN_CENTER,
                     color_text, label);
    }
}

static void draw_resume_prompt(struct browser_ui *ui,
                               struct ui_canvas *canvas, int hover)
{
    const struct file_browser_entry *entry =
        &ui->listing.entries[ui->resume_prompt];
    ui_fill_round_rect(canvas, CENTER_X, CARD_Y, CENTER_W, CARD_H, 28,
                       UI_RGBA(0, 0, 0, 150));
    ui_shadow_round_rect(canvas, PROMPT_X, PROMPT_Y + 8, PROMPT_W, PROMPT_H,
                         26, 28, color_shadow);
    ui_fill_round_rect(canvas, PROMPT_X, PROMPT_Y, PROMPT_W, PROMPT_H, 26,
                       UI_RGBA(30, 32, 39, 255));
    ui_stroke_round_rect(canvas, PROMPT_X, PROMPT_Y, PROMPT_W, PROMPT_H, 26,
                         2, color_border);
    ui_draw_text(canvas, ui->bold, 34, PROMPT_X + PROMPT_W / 2,
                 PROMPT_Y + 34, PROMPT_W - 64, UI_ALIGN_CENTER, color_text,
                 entry->name);
    char position[16], text[64];
    format_duration(entry->resume_us, position);
    ui_draw_text(canvas, ui->font, 26, PROMPT_X + PROMPT_W / 2,
                 PROMPT_Y + 96, PROMPT_W - 64, UI_ALIGN_CENTER, color_dim,
                 tr("resume.question"));
    snprintf(text, sizeof(text), tr("resume.at"), position);
    const struct {
        int id, x;
        const char *label;
        bool primary;
    } buttons[] = {
        {ID_RESUME, PROMPT_RESUME_X, text, true},
        {ID_RESTART, PROMPT_RESTART_X, tr("resume.restart"), false},
    };
    for (size_t i = 0; i < 2; ++i) {
        bool focused = hover == buttons[i].id;
        ui_fill_round_rect(canvas, buttons[i].x, PROMPT_BUTTON_Y,
                           PROMPT_BUTTON_W, PROMPT_BUTTON_H,
                           PROMPT_BUTTON_H / 2.0f,
                           buttons[i].primary
                               ? (focused ? UI_RGBA(90, 180, 255, 255)
                                          : color_accent)
                               : (focused ? color_tile_hover : color_chip));
        ui_draw_text(canvas, ui->bold, 28,
                     buttons[i].x + PROMPT_BUTTON_W / 2,
                     PROMPT_BUTTON_Y + 15, PROMPT_BUTTON_W - 32,
                     UI_ALIGN_CENTER, color_text, buttons[i].label);
    }
}

void browser_ui_render(struct browser_ui *ui, uint32_t *pixels, bool bgra)
{
    if (!ui->font)
        ui->font = ui_font_load(false);
    if (!ui->bold)
        ui->bold = ui_font_load(true);
    struct ui_canvas canvas = {(uint8_t *)pixels, BROWSER_UI_WIDTH,
                               BROWSER_UI_HEIGHT, bgra};
    if (!ui->base) {
        ui->base = malloc((size_t)BROWSER_UI_WIDTH * BROWSER_UI_HEIGHT * 4U);
        ui->content_dirty = true;
    }
    if (ui->base) {
        struct ui_canvas base = {ui->base, BROWSER_UI_WIDTH,
                                 BROWSER_UI_HEIGHT, bgra};
        if (ui->content_dirty) {
            draw_base(ui, &base);
        } else {
            for (int slot = 0; slot < (int)PAGE_ENTRIES; ++slot)
                if (ui->stale_tiles & (1U << slot)) {
                    int x, y;
                    tile_rect(slot, &x, &y);
                    ui_set_rect_raw(&base, x - 8, y - 8, TILE_W + 16,
                                    TILE_H + 12, ui->card_pixel);
                    draw_tile(ui, &base, slot, false);
                }
        }
        ui->stale_tiles = 0;
        ui_copy(&canvas, &base);
    } else {
        draw_base(ui, &canvas);
    }
    ui->content_dirty = false;
    /* Hover states are redrawn over the cached layer. */
    int hover = ui->hover;
    if (hover == ID_SORT)
        draw_sort_chip(ui, &canvas, true);
    else if (hover == ID_FILTER)
        draw_filter_chip(ui, &canvas, true);
    else if (hover >= ID_TILE && hover < ID_TILE + (int)PAGE_ENTRIES)
        draw_tile(ui, &canvas, hover - ID_TILE, true);
    else if (hover == ID_BACK)
        draw_back_button(ui, &canvas, true);
    else if ((hover >= ID_RAIL && hover < ID_RAIL + RAIL_COUNT) ||
             hover == ID_EXIT)
        draw_rail(ui, &canvas, hover);
    else if (hover >= ID_PREVIOUS && hover < ID_PAGE + PAGE_BUTTONS)
        draw_pager(ui, &canvas, hover);
    draw_detail(ui, &canvas);
    if (ui->sort_menu_open)
        draw_sort_menu(ui, &canvas, hover);
    if (ui->filter_menu_open)
        draw_filter_menu(ui, &canvas, hover);
    if (ui->options_open)
        draw_options_menu(ui, &canvas, hover);
    if (ui->resume_prompt >= 0 &&
        (size_t)ui->resume_prompt < ui->listing.entry_count)
        draw_resume_prompt(ui, &canvas, hover);
    if (ui->keyboard_open)
        draw_keyboard(ui, &canvas, hover);
    ui->dirty = false;
}

/* ---- input ------------------------------------------------------------- */

/* The stateless hit test cannot know whether the sort menu or the resume
 * prompt is open; it reports ID_PROMPT_AREA / ID_SORT_AREA over their
 * rectangles (which overlap the grid) and set_pointer re-tests without them
 * when they are closed. */
static int hit_test(int x, int y, int layers)
{
    if ((layers & HIT_KEYBOARD) && x >= KB_X && x < KB_X + KB_W &&
        y >= KB_Y && y < KB_Y + KB_H)
        return ID_KEYBOARD_AREA;
    if ((layers & HIT_PROMPT) && x >= PROMPT_X && x < PROMPT_X + PROMPT_W &&
        y >= PROMPT_Y && y < PROMPT_Y + PROMPT_H)
        return ID_PROMPT_AREA;
    if (x >= SORT_X && x < SORT_X + SORT_W && y >= SORT_Y &&
        y < SORT_Y + SORT_H)
        return ID_SORT;
    if (x >= FILTER_X && x < FILTER_X + FILTER_W && y >= FILTER_Y &&
        y < FILTER_Y + FILTER_H)
        return ID_FILTER;
    if ((layers & HIT_FILTER) && x >= FILTER_MENU_X &&
        x < FILTER_MENU_X + FILTER_MENU_W && y >= FILTER_MENU_Y &&
        y < FILTER_MENU_Y + FILTER_MENU_H)
        return ID_FILTER_AREA;
    if ((layers & HIT_MENU) && x >= MENU_X && x < MENU_X + MENU_W &&
        y >= MENU_Y && y < MENU_Y + MENU_H)
        return ID_SORT_AREA;
    if ((layers & HIT_OPTIONS) && x >= OPT_MENU_X &&
        x < OPT_MENU_X + OPT_MENU_W && y >= OPT_MENU_Y &&
        y < OPT_MENU_Y + OPT_MENU_H)
        return ID_OPTION_AREA;
    for (int slot = 0; slot < (int)PAGE_ENTRIES; ++slot) {
        int tx, ty;
        tile_rect(slot, &tx, &ty);
        if (x >= tx && x < tx + TILE_W && y >= ty && y < ty + TILE_H)
            return ID_TILE + slot;
    }
    for (int i = 0; i < RAIL_COUNT; ++i) {
        int top = RAIL_FIRST_Y + i * (RAIL_ITEM_H + 12);
        if (x >= RAIL_ITEM_X && x < RAIL_ITEM_X + RAIL_ITEM_W && y >= top &&
            y < top + RAIL_ITEM_H)
            return ID_RAIL + i;
    }
    if (x >= RAIL_ITEM_X && x < RAIL_ITEM_X + RAIL_ITEM_W &&
        y >= RAIL_EXIT_Y && y < RAIL_EXIT_Y + RAIL_ITEM_H)
        return ID_EXIT;
    if ((x - BACK_X) * (x - BACK_X) + (y - BACK_Y) * (y - BACK_Y) <= 36 * 36)
        return ID_BACK;
    if (y >= PAGER_Y && y < PAGER_Y + PAGER_H) {
        if (x >= CENTER_X && x < CENTER_X + 190)
            return ID_PREVIOUS;
        if (x >= CENTER_X + CENTER_W - 190 && x < CENTER_X + CENTER_W)
            return ID_NEXT;
        return ID_PAGE_AREA;
    }
    return -1;
}

int browser_ui_hit_test(float u, float v)
{
    return hit_test((int)(u * BROWSER_UI_WIDTH), (int)(v * BROWSER_UI_HEIGHT),
                    HIT_MENU | HIT_PROMPT | HIT_FILTER | HIT_KEYBOARD |
                        HIT_OPTIONS);
}

/* Page buttons depend on the page count, which the stateless hit test does
 * not know; resolve them against the model. */
static int resolve_page_button(const struct browser_ui *ui, int x)
{
    if (page_count(ui) <= 1)
        return -1;
    size_t first, count;
    page_window(ui, &first, &count);
    for (size_t i = 0; i < count; ++i) {
        int bx, width;
        page_button_rect(count, i, &bx, &width);
        if (x >= bx && x < bx + width)
            return ID_PAGE + (int)i;
    }
    return -1;
}

void browser_ui_set_pointer(struct browser_ui *ui, bool visible, int x, int y,
                            int hover)
{
    if (ui->keyboard_open) {
        /* Modal: keys only; a click elsewhere closes the keyboard. */
        int key = -1;
        for (int id = ID_KEY; hover == ID_KEYBOARD_AREA && id <= ID_KEY_ENTER;
             ++id) {
            int kx, ky, width;
            if (key_rect(id, &kx, &ky, &width) && x >= kx &&
                x < kx + width && y >= ky && y < ky + KEY_H)
                key = id;
        }
        hover = key >= 0 ? key
                         : hover == ID_KEYBOARD_AREA ? ID_KEYBOARD_AREA : -1;
        if (hover != ui->hover) {
            ui->hover = hover;
            ui->dirty = true;
        }
        ui->pointer_visible = visible;
        ui->pointer_x = x;
        ui->pointer_y = y;
        return;
    }
    if (hover == ID_KEYBOARD_AREA)
        hover = hit_test(x, y, HIT_MENU | HIT_PROMPT | HIT_FILTER);
    if (ui->resume_prompt >= 0) {
        /* Modal: only its buttons react; any other click closes it. */
        bool on_button = y >= PROMPT_BUTTON_Y &&
                         y < PROMPT_BUTTON_Y + PROMPT_BUTTON_H;
        if (hover == ID_PROMPT_AREA && on_button && x >= PROMPT_RESUME_X &&
            x < PROMPT_RESUME_X + PROMPT_BUTTON_W)
            hover = ID_RESUME;
        else if (hover == ID_PROMPT_AREA && on_button &&
                 x >= PROMPT_RESTART_X &&
                 x < PROMPT_RESTART_X + PROMPT_BUTTON_W)
            hover = ID_RESTART;
        else
            hover = hover == ID_PROMPT_AREA ? ID_PROMPT_AREA : -1;
        if (hover != ui->hover) {
            ui->hover = hover;
            ui->dirty = true;
        }
        ui->pointer_visible = visible;
        ui->pointer_x = x;
        ui->pointer_y = y;
        return;
    }
    if (hover == ID_PROMPT_AREA)
        hover = hit_test(x, y, HIT_MENU | HIT_FILTER);
    if (hover == ID_OPTION_AREA) {
        if (ui->options_open) {
            int row = (y - OPT_MENU_Y - OPT_HEADER_H - 8) / MENU_ITEM_H;
            hover = row >= 0 && row < ui->language_count
                        ? ID_OPTION_ITEM + row
                        : -1;
        } else {
            hover = hit_test(x, y, 0);
        }
    } else if (ui->options_open && hover != ID_RAIL + RAIL_OPTIONS) {
        hover = -1; /* clicks outside the overlay only close it */
    }
    if (hover == ID_FILTER_AREA) {
        if (ui->filter_menu_open) {
            int row = (y - FILTER_MENU_Y - 8) / MENU_ITEM_H;
            hover = row >= 0 && row < filter_count(ui) ? ID_FILTER_ITEM + row
                                                       : -1;
        } else {
            hover = hit_test(x, y, HIT_MENU);
        }
    }
    if (ui->filter_menu_open && hover != ID_FILTER &&
        (hover < ID_FILTER_ITEM || hover >= ID_FILTER_ITEM + 4))
        hover = -1; /* clicks outside the menu only close it */
    if (hover == ID_FILTER && !filterable(ui))
        hover = -1;
    if (hover == ID_SORT_AREA) {
        /* The menu hides everything below it while open; closed, the area
         * is just grid. */
        if (ui->sort_menu_open) {
            int row = (y - MENU_Y - 8) / MENU_ITEM_H;
            hover = row >= 0 && row < sort_count(ui) ? ID_SORT_ITEM + row
                                                     : -1;
        } else {
            hover = hit_test(x, y, 0);
        }
    } else if (ui->sort_menu_open) {
        /* Clicks outside the menu only close it. */
        if (hover != ID_SORT)
            hover = -1;
    }
    if (hover == ID_SORT && !sortable(ui))
        hover = -1;
    if (hover == ID_PAGE_AREA)
        hover = resolve_page_button(ui, x);
    if ((hover == ID_PREVIOUS || hover == ID_NEXT) && page_count(ui) <= 1)
        hover = -1;
    if (hover == ID_BACK && ui->parent < 0)
        hover = -1;
    if (hover >= ID_TILE && hover < ID_TILE + (int)PAGE_ENTRIES &&
        slot_entry(ui, hover) < 0)
        hover = -1;
    if (hover != ui->hover) {
        ui->hover = hover;
        if (hover >= ID_TILE && hover < ID_TILE + (int)PAGE_ENTRIES)
            ui->detail = slot_entry(ui, hover);
        ui->dirty = true;
    }
    /* The host draws the pointer as its own layer; moving it does not
     * change the panel picture. */
    ui->pointer_visible = visible;
    ui->pointer_x = x;
    ui->pointer_y = y;
}

static void request(struct browser_ui *ui, const char *location)
{
    char error[256] = {0};
    if (location != ui->location) /* snprintf must not overlap its source */
        snprintf(ui->location, sizeof(ui->location), "%s", location);
    if (file_browser_request(ui->browser, location, error,
                             sizeof(error)) < 0) {
        snprintf(ui->listing.error, sizeof(ui->listing.error), "%s", error);
        ui->content_dirty = true;
        ui->dirty = true;
    }
}

/* Asks for codec and picture size of the Jellyfin titles on the current
 * and the next page (the listing comes without them). */
static void request_streams(struct browser_ui *ui)
{
    if (!ui->streams_requested)
        return;
    size_t indices[2 * PAGE_ENTRIES];
    size_t count = 0;
    for (size_t position = ui->page * PAGE_ENTRIES;
         position < ui->grid_count && position < (ui->page + 2) * PAGE_ENTRIES;
         ++position) {
        size_t index = ui->grid[position];
        const struct file_browser_entry *entry = &ui->listing.entries[index];
        if (!ui->streams_requested[index] &&
            entry->kind == FILE_BROWSER_MEDIA && !entry->codec[0] &&
            strncmp(entry->path, "jf:item:", 8) == 0)
            indices[count++] = index;
    }
    if (!count)
        return;
    size_t taken = file_browser_request_streams(
        ui->browser, ui->listing.generation, &ui->listing, indices, count);
    for (size_t i = 0; i < taken; ++i)
        ui->streams_requested[indices[i]] = true;
}

/* Loads the thumbnails of the current and the next page and releases those
 * more than one page away, so memory stays bounded for long listings. */
static void request_thumbnails(struct browser_ui *ui)
{
    request_streams(ui);
    if (!ui->thumbs || !ui->thumb_requested)
        return;
    for (size_t position = 0; position < ui->grid_count; ++position) {
        size_t page = position / PAGE_ENTRIES;
        size_t index = ui->grid[position];
        if (page + 1 < ui->page || page > ui->page + 1) {
            ui_image_free(ui->thumbs[index]);
            ui->thumbs[index] = NULL;
            ui->thumb_requested[index] = false;
        }
    }
    for (size_t page = ui->page; page <= ui->page + 1; ++page)
        for (size_t slot = 0; slot < PAGE_ENTRIES; ++slot) {
            size_t position = page * PAGE_ENTRIES + slot;
            if (position >= ui->grid_count)
                return;
            size_t index = ui->grid[position];
            const char *image = ui->listing.entries[index].image;
            if (!image || ui->thumb_requested[index])
                continue;
            ui->thumb_requested[index] = true;
            thumbnail_loader_request(ui->loader, ui->listing.generation, index,
                                     image);
        }
}

/* ---- persisted sort choice -------------------------------------------- */

static void load_sort(struct browser_ui *ui)
{
    int sort = settings_get_int("sort", JF_SORT_NAME);
    ui->sort = sort >= 0 && sort < JF_SORT_COUNT ? sort : JF_SORT_NAME;
    ui->sort_descending = settings_get_bool("sort_descending", false);
    int local = settings_get_int("local_sort", FILE_BROWSER_SORT_NAME);
    ui->local_sort = local >= 0 && local < FILE_BROWSER_SORT_COUNT
                         ? local
                         : FILE_BROWSER_SORT_NAME;
    ui->local_sort_descending =
        settings_get_bool("local_sort_descending", false);
}

static void save_sort(const struct browser_ui *ui)
{
    settings_set_int("sort", ui->sort);
    settings_set_bool("sort_descending", ui->sort_descending);
}

/* Chooses a criterion; choosing the current one flips the direction. */
static void choose_sort(struct browser_ui *ui, int sort)
{
    if (local_location(ui)) {
        if (sort == ui->local_sort)
            ui->local_sort_descending = !ui->local_sort_descending;
        else {
            ui->local_sort = sort;
            ui->local_sort_descending = sort != FILE_BROWSER_SORT_NAME;
        }
        settings_set_int("local_sort", ui->local_sort);
        settings_set_bool("local_sort_descending",
                          ui->local_sort_descending);
        file_browser_set_local_sort(ui->browser, ui->local_sort,
                                    ui->local_sort_descending);
        ui->restore_page = 0;
        request(ui, ui->location);
        return;
    }
    if (sort == ui->sort && sort != JF_SORT_RANDOM)
        ui->sort_descending = !ui->sort_descending;
    else {
        ui->sort = sort;
        ui->sort_descending = default_descending(sort);
    }
    save_sort(ui);
    file_browser_set_sort(ui->browser, ui->sort, ui->sort_descending);
    ui->restore_page = 0;
    request(ui, ui->location);
}

static void set_page(struct browser_ui *ui, size_t page)
{
    if (page >= page_count(ui) || page == ui->page)
        return;
    ui->page = page;
    request_thumbnails(ui);
    ui->hover = -1;
    ui->content_dirty = true;
    ui->dirty = true;
}

void browser_ui_close_menus(struct browser_ui *ui)
{
    if (ui->sort_menu_open || ui->filter_menu_open ||
        ui->resume_prompt >= 0 || ui->keyboard_open || ui->options_open) {
        ui->sort_menu_open = false;
        ui->filter_menu_open = false;
        ui->keyboard_open = false;
        ui->kb_purpose = KB_PURPOSE_NONE;
        ui->kb_target = NULL;
        ui->options_open = false;
        ui->resume_prompt = -1;
        ui->dirty = true;
    }
}

/* ---- On-screen keyboard tool ---------------------------------------
 * keyboard_begin points the keyboard at any buffer; the keys edit it in
 * place and ID_KEY_ENTER routes to keyboard_commit, which acts on
 * kb_purpose (run a search, or walk the WebDAV add-server fields). */
static void keyboard_begin(struct browser_ui *ui, int purpose,
                           const char *title, char *target, size_t capacity,
                           bool mask)
{
    ui->keyboard_open = true;
    ui->kb_purpose = purpose;
    ui->kb_target = target;
    ui->kb_capacity = capacity;
    ui->kb_mask = mask;
    ui->kb_shift = false;
    ui->kb_symbols = false;
    snprintf(ui->kb_title, sizeof(ui->kb_title), "%s", title);
    ui->sort_menu_open = ui->filter_menu_open = ui->options_open = false;
    ui->hover = -1;
    ui->dirty = true;
}

static void keyboard_dismiss(struct browser_ui *ui)
{
    ui->keyboard_open = false;
    ui->kb_purpose = KB_PURPOSE_NONE;
    ui->kb_target = NULL;
    ui->hover = -1;
    ui->dirty = true;
}

static void open_webdav_form(struct browser_ui *ui)
{
    ui->webdav_url[0] = ui->webdav_user[0] = ui->webdav_pass[0] = '\0';
    ui->status_message[0] = '\0';
    keyboard_begin(ui, KB_PURPOSE_WEBDAV_URL, "Server URL", ui->webdav_url,
                   sizeof(ui->webdav_url), false);
}

static void keyboard_commit(struct browser_ui *ui)
{
    switch (ui->kb_purpose) {
    case KB_PURPOSE_SEARCH: {
        size_t length = strlen(ui->query);
        while (length && ui->query[length - 1] == ' ')
            ui->query[--length] = '\0';
        if (!length)
            return;
        char location[96];
        snprintf(location, sizeof(location), "jf:search:%s", ui->query);
        keyboard_dismiss(ui);
        ui->restore_page = 0;
        request(ui, location);
        return;
    }
    case KB_PURPOSE_WEBDAV_URL:
        if (!ui->webdav_url[0])
            return; /* a server needs an address */
        keyboard_begin(ui, KB_PURPOSE_WEBDAV_USER, "Username",
                       ui->webdav_user, sizeof(ui->webdav_user), false);
        return;
    case KB_PURPOSE_WEBDAV_USER:
        keyboard_begin(ui, KB_PURPOSE_WEBDAV_PASS, "Password",
                       ui->webdav_pass, sizeof(ui->webdav_pass), true);
        return;
    case KB_PURPOSE_WEBDAV_PASS: {
        struct wd_server server;
        memset(&server, 0, sizeof(server));
        snprintf(server.url, sizeof(server.url), "%s", ui->webdav_url);
        snprintf(server.user, sizeof(server.user), "%s", ui->webdav_user);
        snprintf(server.pass, sizeof(server.pass), "%s", ui->webdav_pass);
        char error[128] = "";
        if (wd_server_add(&server, error, sizeof(error)) < 0) {
            snprintf(ui->status_message, sizeof(ui->status_message), "%s",
                     error);
            keyboard_begin(ui, KB_PURPOSE_WEBDAV_URL, "Server URL",
                           ui->webdav_url, sizeof(ui->webdav_url), false);
            return;
        }
        keyboard_dismiss(ui);
        request(ui, "webdav:");
        return;
    }
    default:
        keyboard_dismiss(ui);
        return;
    }
}

/* A key press (or a click outside, which cancels). */
static void type_key(struct browser_ui *ui, int hover)
{
    ui->dirty = true;
    if (hover == ID_KEY_SHIFT) {
        ui->kb_shift = !ui->kb_shift;
        return;
    }
    if (hover == ID_KEY_SYMBOLS) {
        ui->kb_symbols = !ui->kb_symbols;
        return;
    }
    if (hover == ID_KEY_ENTER) {
        keyboard_commit(ui);
        return;
    }
    if (hover == ID_KEY_CANCEL) {
        keyboard_dismiss(ui);
        return;
    }
    char *buf = ui->kb_target;
    if (!buf)
        return;
    size_t capacity = ui->kb_capacity;
    size_t length = strlen(buf);
    if (hover >= ID_KEY && hover < ID_KEY + KEY_ROWS * KEY_COLUMNS) {
        char glyph[8];
        key_label(ui, hover, glyph, sizeof(glyph));
        if (length + strlen(glyph) < capacity)
            memcpy(buf + length, glyph, strlen(glyph) + 1);
    } else if (hover == ID_KEY_SPACE) {
        if (length && buf[length - 1] != ' ' && length + 1 < capacity)
            memcpy(buf + length, " ", 2);
    } else if (hover == ID_KEY_BACKSPACE) {
        /* Drop one UTF-8 character (continuation bytes are 10xxxxxx). */
        while (length && (buf[length - 1] & 0xC0) == 0x80)
            --length;
        if (length)
            --length;
        buf[length] = '\0';
    } else if (hover != ID_KEYBOARD_AREA) {
        keyboard_dismiss(ui); /* a click outside the keyboard */
    }
}

static enum browser_ui_action select_title(struct browser_ui *ui, int index,
                                           int64_t start_us, char *path,
                                           size_t path_size)
{
    const struct file_browser_entry *entry = &ui->listing.entries[index];
    ui->selected_page = ui->page;
    ui->selected_start_us = start_us;
    snprintf(ui->selected_title, sizeof(ui->selected_title), "%s",
             entry->name);
    snprintf(path, path_size, "%s", entry->path);
    return BROWSER_UI_FILE_SELECTED;
}

void browser_ui_flip(struct browser_ui *ui, int direction)
{
    if (ui->sort_menu_open || ui->filter_menu_open ||
        ui->resume_prompt >= 0 || ui->keyboard_open)
        return;
    if (direction < 0 && ui->page > 0)
        set_page(ui, ui->page - 1);
    else if (direction > 0)
        set_page(ui, ui->page + 1);
}

enum browser_ui_action browser_ui_activate(struct browser_ui *ui, char *path,
                                           size_t path_size)
{
    int hover = ui->hover;
    if (ui->keyboard_open) {
        type_key(ui, hover);
        return BROWSER_UI_NONE;
    }
    if (ui->resume_prompt >= 0) {
        int index = ui->resume_prompt;
        ui->resume_prompt = -1;
        ui->hover = -1;
        ui->dirty = true;
        if ((size_t)index >= ui->listing.entry_count ||
            (hover != ID_RESUME && hover != ID_RESTART))
            return BROWSER_UI_NONE;
        return select_title(ui, index,
                            hover == ID_RESUME
                                ? ui->listing.entries[index].resume_us
                                : 0,
                            path, path_size);
    }
    if (ui->filter_menu_open) {
        ui->filter_menu_open = false;
        ui->hover = -1;
        ui->dirty = true;
        if (hover >= ID_FILTER_ITEM &&
            hover < ID_FILTER_ITEM + filter_count(ui)) {
            /* The library folder without any filter suffix. */
            char folder[64];
            size_t length = strcspn(ui->location + 8, "/");
            snprintf(folder, sizeof(folder), "%.*s",
                     (int)(length < sizeof(folder) ? length : 0),
                     ui->location + 8);
            char location[128];
            int item = hover - ID_FILTER_ITEM;
            if (item == 3)
                snprintf(location, sizeof(location), "jf:view:%s", folder);
            else
                snprintf(location, sizeof(location), "jf:filters:%s:%s",
                         folder, filter_keys[item]);
            ui->restore_page = 0;
            request(ui, location);
        }
        return BROWSER_UI_NONE;
    }
    if (hover == ID_FILTER && filterable(ui)) {
        ui->filter_menu_open = true;
        ui->sort_menu_open = false;
        ui->dirty = true;
        return BROWSER_UI_NONE;
    }
    if (ui->sort_menu_open) {
        if (hover >= ID_SORT_ITEM && hover < ID_SORT_ITEM + sort_count(ui))
            choose_sort(ui, hover - ID_SORT_ITEM);
        ui->sort_menu_open = false;
        ui->hover = -1;
        ui->dirty = true;
        return BROWSER_UI_NONE;
    }
    if (hover == ID_SORT && sortable(ui)) {
        ui->sort_menu_open = true;
        ui->dirty = true;
        return BROWSER_UI_NONE;
    }
    if (ui->options_open) {
        if (hover >= ID_OPTION_ITEM &&
            hover < ID_OPTION_ITEM + ui->language_count) {
            const char *code = ui->languages[hover - ID_OPTION_ITEM].code;
            settings_set_string("language", code);
            i18n_load(code);
        }
        ui->options_open = false;
        ui->hover = -1;
        ui->content_dirty = true; /* every cached label must be redrawn */
        ui->dirty = true;
        return BROWSER_UI_NONE;
    }
    if (hover == ID_RAIL + RAIL_OPTIONS) {
        ui->options_open = true;
        ui->sort_menu_open = ui->filter_menu_open = false;
        ui->language_count = i18n_list(ui->languages, I18N_MAX_LANGUAGES);
        ui->dirty = true;
        return BROWSER_UI_NONE;
    }
    if (hover >= ID_TILE && hover < ID_TILE + (int)PAGE_ENTRIES) {
        int index = slot_entry(ui, hover);
        if (index < 0)
            return BROWSER_UI_NONE;
        const struct file_browser_entry *entry = &ui->listing.entries[index];
        if (entry->kind == FILE_BROWSER_MEDIA) {
            if (entry->resume_us >= RESUME_MINIMUM_US) {
                ui->resume_prompt = index;
                ui->hover = -1;
                ui->dirty = true;
                return BROWSER_UI_NONE;
            }
            return select_title(ui, index, 0, path, path_size);
        }
        if (strcmp(entry->path, "webdav:add") == 0)
            open_webdav_form(ui);
        else
            request(ui, entry->path);
    } else if (hover == ID_BACK && ui->parent >= 0) {
        request(ui, ui->listing.entries[ui->parent].path);
    } else if (hover == ID_PREVIOUS) {
        if (ui->page > 0)
            set_page(ui, ui->page - 1);
    } else if (hover == ID_NEXT) {
        set_page(ui, ui->page + 1);
    } else if (hover >= ID_PAGE && hover < ID_PAGE + PAGE_BUTTONS) {
        size_t first, count;
        page_window(ui, &first, &count);
        if ((size_t)(hover - ID_PAGE) < count)
            set_page(ui, first + (size_t)(hover - ID_PAGE));
    } else if (hover == ID_RAIL + RAIL_START) {
        request(ui, "root:");
    } else if (hover == ID_RAIL + RAIL_JELLYFIN) {
        request(ui, "jf:");
    } else if (hover == ID_RAIL + RAIL_WEBDAV) {
        request(ui, "webdav:");
    } else if (hover == ID_RAIL + RAIL_SEARCH) {
        keyboard_begin(ui, KB_PURPOSE_SEARCH, tr("rail.search"), ui->query,
                       sizeof(ui->query), false);
    } else if (hover == ID_RAIL + RAIL_LOCAL) {
        const char *home = getenv("HOME");
        request(ui, home && home[0] ? home : "/");
    } else if (hover == ID_EXIT) {
        return BROWSER_UI_EXIT_REQUESTED;
    }
    return BROWSER_UI_NONE;
}

bool browser_ui_following_title(struct browser_ui *ui, const char *path,
                                int direction, char *next, size_t next_size)
{
    size_t position = 0;
    while (position < ui->grid_count &&
           strcmp(ui->listing.entries[ui->grid[position]].path, path) != 0)
        ++position;
    if (position == ui->grid_count)
        return false;
    for (;;) {
        if (direction < 0 ? position == 0 : position + 1 >= ui->grid_count)
            return false;
        position = direction < 0 ? position - 1 : position + 1;
        const struct file_browser_entry *entry =
            &ui->listing.entries[ui->grid[position]];
        if (entry->kind != FILE_BROWSER_MEDIA)
            continue;
        ui->selected_page = position / PAGE_ENTRIES;
        ui->selected_start_us = 0;
        snprintf(ui->selected_title, sizeof(ui->selected_title), "%s",
                 entry->name);
        snprintf(next, next_size, "%s", entry->path);
        return true;
    }
}

/* ---- model ------------------------------------------------------------- */

static void free_thumbnails(struct browser_ui *ui)
{
    if (ui->thumbs)
        for (size_t i = 0; i < ui->listing.entry_count; ++i)
            ui_image_free(ui->thumbs[i]);
    free(ui->thumbs);
    ui->thumbs = NULL;
    free(ui->thumb_requested);
    ui->thumb_requested = NULL;
    free(ui->streams_requested);
    ui->streams_requested = NULL;
}

static void install_listing(struct browser_ui *ui,
                            struct file_browser_listing *listing)
{
    free_thumbnails(ui);
    free(ui->grid);
    file_browser_listing_clear(&ui->listing);
    ui->listing = *listing;
    size_t slots = ui->listing.entry_count ? ui->listing.entry_count : 1;
    ui->grid = calloc(slots, sizeof(*ui->grid));
    ui->thumbs = calloc(slots, sizeof(*ui->thumbs));
    ui->thumb_requested = calloc(slots, sizeof(*ui->thumb_requested));
    ui->streams_requested = calloc(slots, sizeof(*ui->streams_requested));
    ui->grid_count = 0;
    ui->parent = -1;
    for (size_t i = 0; i < ui->listing.entry_count; ++i) {
        if (ui->listing.entries[i].kind == FILE_BROWSER_PARENT) {
            if (ui->parent < 0)
                ui->parent = (int)i;
        } else if (ui->grid) {
            ui->grid[ui->grid_count++] = i;
        }
    }
    ui->page = ui->restore_page;
    ui->restore_page = 0;
    if (ui->page >= page_count(ui))
        ui->page = 0;
    ui->hover = -1;
    ui->detail = -1;
    ui->resume_prompt = -1;
    ui->content_dirty = true;
    ui->dirty = true;
    request_thumbnails(ui);
}

void browser_ui_set_thumbnail(struct browser_ui *ui, size_t index,
                              struct ui_image *image)
{
    if (!ui->thumbs || index >= ui->listing.entry_count) {
        ui_image_free(image);
        return;
    }
    ui_image_free(ui->thumbs[index]);
    ui->thumbs[index] = image;
    /* Only the tile showing it (if it is on this page) needs a redraw. */
    for (int slot = 0; slot < (int)PAGE_ENTRIES; ++slot)
        if (slot_entry(ui, slot) == (int)index) {
            ui->stale_tiles |= 1U << slot;
            ui->dirty = true;
        }
    if (ui->detail == (int)index)
        ui->dirty = true;
}

bool browser_ui_update(struct browser_ui *ui)
{
    bool changed = false;
    struct thumbnail_result thumbnail;
    while (thumbnail_loader_poll(ui->loader, &thumbnail)) {
        if (thumbnail.generation == ui->listing.generation &&
            thumbnail.image) {
            browser_ui_set_thumbnail(ui, thumbnail.index, thumbnail.image);
            changed = true;
        } else {
            ui_image_free(thumbnail.image);
        }
    }
    struct file_browser_streams streams;
    while (file_browser_poll_streams(ui->browser, &streams)) {
        if (streams.generation != ui->listing.generation)
            continue;
        for (size_t i = 0; i < streams.count; ++i) {
            size_t index = streams.entries[i].index;
            if (index >= ui->listing.entry_count)
                continue;
            struct file_browser_entry *entry = &ui->listing.entries[index];
            snprintf(entry->codec, sizeof(entry->codec), "%s",
                     streams.entries[i].codec);
            entry->width = streams.entries[i].width;
            entry->height = streams.entries[i].height;
            if (ui->detail == (int)index) {
                ui->dirty = true;
                changed = true;
            }
        }
    }
    struct file_browser_listing updated = {0};
    int result = file_browser_poll(ui->browser, &updated);
    if (result <= 0)
        return changed;
    install_listing(ui, &updated);
    printf("browser_listing generation=%" PRIu64 " path=%s entries=%zu"
           " error=%s\n",
           ui->listing.generation, ui->listing.path ? ui->listing.path : "",
           ui->listing.entry_count,
           ui->listing.error[0] ? ui->listing.error : "none");
    return true;
}

int browser_ui_open(struct browser_ui *ui, const char *location,
                    const char *message, char *error, size_t error_size)
{
    memset(ui, 0, sizeof(*ui));
    ui->hover = -1;
    ui->detail = -1;
    ui->resume_prompt = -1;
    ui->parent = -1;
    ui->dirty = true;
    ui->content_dirty = true;
    snprintf(ui->location, sizeof(ui->location), "%s", location);
    if (message)
        snprintf(ui->status_message, sizeof(ui->status_message), "%s",
                 message);
    ui->loader = thumbnail_loader_create();
    load_sort(ui);
    int opened = file_browser_open(&ui->browser, location, error, error_size);
    if (opened == 0) {
        file_browser_set_sort(ui->browser, ui->sort, ui->sort_descending);
        file_browser_set_local_sort(ui->browser, ui->local_sort,
                                    ui->local_sort_descending);
    }
    return opened;
}

void browser_ui_navigate(struct browser_ui *ui, const char *location,
                         const char *message)
{
    snprintf(ui->status_message, sizeof(ui->status_message), "%s",
             message ? message : "");
    /* Coming back from playback shows the page the title was picked on. */
    ui->restore_page = ui->selected_page;
    ui->selected_page = 0;
    ui->hover = -1;
    ui->content_dirty = true;
    ui->dirty = true;
    request(ui, location);
}

void browser_ui_close(struct browser_ui *ui)
{
    thumbnail_loader_destroy(ui->loader);
    ui->loader = NULL;
    free_thumbnails(ui);
    free(ui->grid);
    ui->grid = NULL;
    file_browser_listing_clear(&ui->listing);
    file_browser_close(&ui->browser);
    free(ui->base);
    ui->base = NULL;
    ui_font_free(ui->font);
    ui_font_free(ui->bold);
    ui->font = ui->bold = NULL;
}
