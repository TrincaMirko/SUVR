#ifndef STEREONAUT_BROWSER_UI_H
#define STEREONAUT_BROWSER_UI_H

#include "file-browser.h"
#include "i18n.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Model and CPU rasterizer of the media browser panel. It knows nothing about
 * OpenXR or Vulkan: the host feeds pointer hits in and uploads the pixels. */

#define BROWSER_UI_WIDTH 2048U
#define BROWSER_UI_HEIGHT 1024U
#define BROWSER_UI_PANEL_WIDTH_METERS 2.8f
#define BROWSER_UI_PANEL_HEIGHT_METERS 1.4f

enum browser_ui_action {
    BROWSER_UI_NONE,
    BROWSER_UI_FILE_SELECTED,
    BROWSER_UI_EXIT_REQUESTED,
};

struct ui_font;
struct ui_image;
struct thumbnail_loader;

struct browser_ui {
    struct file_browser *browser;
    struct file_browser_listing listing;
    char status_message[256];
    char location[4096];  /* last requested location */
    size_t *grid;         /* listing indices shown as tiles (no parent) */
    size_t grid_count;
    int parent;           /* listing index of the ".." entry or -1 */
    struct ui_image **thumbs; /* per listing entry, may be NULL */
    bool *thumb_requested;    /* per listing entry */
    bool *streams_requested;  /* codec/size asked for, per listing entry */
    struct thumbnail_loader *loader;
    int sort;             /* enum jf_sort */
    bool sort_descending;
    int local_sort;       /* enum file_browser_local_sort */
    bool local_sort_descending;
    bool sort_menu_open;
    bool filter_menu_open;
    bool options_open;    /* options overlay (language choice) */
    struct i18n_language languages[I18N_MAX_LANGUAGES]; /* snapshot while open */
    int language_count;
    /* On-screen keyboard: a reusable text-entry tool any field can summon
     * (see keyboard_begin). It edits kb_target in place and reports commit
     * or cancel through kb_purpose. */
    bool keyboard_open;
    int kb_purpose;       /* enum kb_purpose */
    bool kb_shift;        /* uppercase letters */
    bool kb_symbols;      /* symbol layer instead of letters */
    bool kb_mask;         /* render the edited text as dots (passwords) */
    char kb_title[48];    /* prompt shown above the keys */
    char *kb_target;      /* buffer being edited (points into this struct) */
    size_t kb_capacity;
    char query[64];       /* search text (UTF-8) */
    char webdav_url[512]; /* add-server form fields */
    char webdav_user[128];
    char webdav_pass[256];
    size_t page;
    size_t selected_page; /* page of the last started title */
    char selected_title[256]; /* name of the last started title */
    int64_t selected_start_us; /* where the last started title begins */
    int resume_prompt;    /* listing index asking "resume or restart", -1 */
    size_t restore_page;  /* page to show for the next listing */
    int hover;
    int detail;           /* listing index shown in the detail card or -1 */
    bool pointer_visible;
    int pointer_x;
    int pointer_y;
    bool dirty;           /* output changed */
    bool content_dirty;   /* cached static layer must be redrawn */
    uint32_t stale_tiles; /* grid slots to redraw in the cached layer */
    uint8_t card_pixel[4];/* card background as stored in the cached layer */
    struct ui_font *font, *bold;
    uint8_t *base;        /* cached static layer, BROWSER_UI_WIDTH x HEIGHT */
};

int browser_ui_open(struct browser_ui *ui, const char *location,
                    const char *message, char *error, size_t error_size);
void browser_ui_close(struct browser_ui *ui);
/* Shows another location (directory or Jellyfin path) with an optional
 * status line. */
void browser_ui_navigate(struct browser_ui *ui, const char *location,
                         const char *message);
/* Picks up a finished listing; true if the picture changed. */
bool browser_ui_update(struct browser_ui *ui);
/* Hands a decoded thumbnail for listing entry `index` to the panel (takes
 * ownership). */
void browser_ui_set_thumbnail(struct browser_ui *ui, size_t index,
                              struct ui_image *image);
void browser_ui_render(struct browser_ui *ui, uint32_t *pixels, bool bgra);
/* Maps panel texture coordinates to a control id or -1. */
int browser_ui_hit_test(float u, float v);
void browser_ui_set_pointer(struct browser_ui *ui, bool visible, int x, int y,
                            int hover);
enum browser_ui_action browser_ui_activate(struct browser_ui *ui, char *path,
                                           size_t path_size);
/* Closes the sort menu and the resume prompt (e.g. when the browser is
 * left). */
void browser_ui_close_menus(struct browser_ui *ui);
/* The title after (direction 1) or before (-1) `path` in the current
 * listing ("play next/previous"): copies its path and remembers it as the
 * started title; false if there is none. */
bool browser_ui_following_title(struct browser_ui *ui, const char *path,
                                int direction, char *next, size_t next_size);
/* Previous (< 0) or next (> 0) page, e.g. from the thumbstick. */
void browser_ui_flip(struct browser_ui *ui, int direction);

#endif
