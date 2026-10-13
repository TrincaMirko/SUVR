#define _POSIX_C_SOURCE 200809L
#include "player-ui.h"

#include "controller-bindings.h"
#include "i18n.h"
#include "jellyfin.h"
#include "nv12-playback-ipc.h"
#include "ui-draw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Control card. */
#define CARD_X 40
#define CARD_Y 360
#define CARD_W 1520
#define CARD_H 310

/* Seek bar. */
#define BAR_X 250
#define BAR_W 1100
#define BAR_Y (CARD_Y + 96)
#define BAR_H 12
#define BAR_HIT_TOP (CARD_Y + 68)
#define BAR_HIT_BOTTOM (CARD_Y + 136)

/* Transport row. */
#define ROW_Y (CARD_Y + 220)
#define BACK_X 116
#define SKIP_BACK_X 650
#define PLAY_X 800
#define SKIP_FORWARD_X 950
#define VIEW_X 1484
#define VOLUME_DOWN_X 1160
#define VOLUME_UP_X 1350
#define VOLUME_R 30
#define END_X 196
#define END_W 300
#define END_H 48
#define SMALL_R 38
#define SKIP_R 42
#define PLAY_R 54

/* Scrub preview above the card, centred on the previewed position. */
#define PREVIEW_IMAGE_H 196
#define PREVIEW_PAD 6
#define PREVIEW_LABEL_H 40
#define PREVIEW_BOTTOM (CARD_Y - 12)

/* Settings popup above the card: a tab row and three columns of controls
 * in up to three rows. */
#define POPUP_X 180
#define POPUP_Y 10
#define POPUP_W 1380
#define POPUP_H 334
#define COLUMN_W 416
#define COLUMN_X(column) (POPUP_X + 22 + (column) * 460)
#define TAB_Y (POPUP_Y + 18)
#define TAB_W 200
#define ROW_LABEL_Y(row) (POPUP_Y + 86 + (row) * 82)
#define ROW_CHIP_Y(row) (ROW_LABEL_Y(row) + 32)
#define STEPPER_Y(row) (POPUP_Y + 98 + (row) * 70)
#define STEPPER_LABEL_W 170
#define STEPPER_BUTTON_W 56
#define CHIP_GAP 10
#define CHIP_H 44

const int player_ui_preview_fovs[PLAYER_UI_PREVIEW_FOV_COUNT] = {
    60, 80, 100, 120,
};

const int player_ui_autohide[PLAYER_UI_AUTOHIDE_COUNT] = {0, 5, 15};

const struct player_adjust_info player_adjust_info[PLAYER_ADJUST_COUNT] = {
    [PLAYER_ADJUST_ZOOM] = {"adjust.zoom", "zoom", -0.5f, 0.8f, 0.05f, 0.0f,
                            100.0f, "%+.0f cm", false},
    [PLAYER_ADJUST_HEIGHT] = {"adjust.height", "height", -0.5f, 0.5f, 0.05f, 0.0f,
                              100.0f, "%+.0f cm", false},
    [PLAYER_ADJUST_SLIDE] = {"adjust.slide", "slide", -0.5f, 0.5f, 0.05f, 0.0f,
                             100.0f, "%+.0f cm", false},
    [PLAYER_ADJUST_TILT] = {"adjust.tilt", "tilt", -90.0f, 90.0f, 5.0f, 0.0f,
                            1.0f, "%+.0f°", true},
    [PLAYER_ADJUST_YAW] = {"adjust.yaw", "yaw", -180.0f, 180.0f, 5.0f, 0.0f,
                           1.0f, "%+.0f°", false},
    [PLAYER_ADJUST_ROLL] = {"adjust.roll", "roll", -30.0f, 30.0f, 1.0f, 0.0f,
                            1.0f, "%+.0f°", false},
    [PLAYER_ADJUST_STEREO_X] = {"adjust.stereo_x", "stereo_x", -3.0f, 3.0f,
                                0.1f, 0.0f, 1.0f, "%+.1f°", false},
    [PLAYER_ADJUST_STEREO_Y] = {"adjust.stereo_y", "stereo_y", -2.0f, 2.0f,
                                0.1f, 0.0f, 1.0f, "%+.1f°", false},
    [PLAYER_ADJUST_HEAD_MOTION] = {"label.strength", "head_motion", 0.0f, 1.5f,
                                   0.1f, 0.0f, 100.0f, "%.0f %%", true},
    [PLAYER_ADJUST_AUTOFOCUS] = {"adjust.autofocus", "autofocus", 0.0f, 1.0f, 0.1f,
                                 0.0f, 100.0f, "%.0f %%", true},
    [PLAYER_ADJUST_FOCUS_DISTANCE] = {"adjust.focus_distance", "focus_distance", 0.2f,
                                      1.0f, 0.05f, 0.6f, 100.0f, "%.0f cm",
                                      true},
    [PLAYER_ADJUST_FOCUS_HYSTERESIS] = {"adjust.focus_hysteresis", "focus_hysteresis",
                                        0.0f, 0.5f, 0.05f, 0.15f, 100.0f,
                                        "%.0f %%", true},
    [PLAYER_ADJUST_FOCUS_FADE] = {"adjust.focus_fade", "focus_fade", 0.0f, 3.0f,
                                  0.1f, 1.0f, 1.0f, "%.1f s", true},
    [PLAYER_ADJUST_BRIGHTNESS] = {"adjust.brightness", "brightness", -0.3f, 0.3f,
                                  0.02f, 0.0f, 100.0f, "%+.0f", false},
    [PLAYER_ADJUST_CONTRAST] = {"adjust.contrast", "contrast", 0.5f, 1.5f, 0.05f,
                                1.0f, 100.0f, "%.0f %%", false},
    [PLAYER_ADJUST_SATURATION] = {"adjust.saturation", "saturation", 0.0f, 2.0f,
                                  0.1f, 1.0f, 100.0f, "%.0f %%", false},
    [PLAYER_ADJUST_GAMMA] = {"adjust.gamma", "gamma", 0.5f, 2.0f, 0.05f, 1.0f, 1.0f,
                             "%.2f", false},
};

static const struct ui_color color_card = UI_RGBA(17, 18, 22, 225);
static const struct ui_color color_border = UI_RGBA(255, 255, 255, 18);
static const struct ui_color color_button = UI_RGBA(42, 45, 54, 255);
static const struct ui_color color_button_hover = UI_RGBA(62, 67, 80, 255);
static const struct ui_color color_text = UI_RGBA(236, 239, 244, 255);
static const struct ui_color color_dim = UI_RGBA(150, 157, 170, 255);
static const struct ui_color color_accent = UI_RGBA(56, 160, 255, 255);
static const struct ui_color color_track = UI_RGBA(255, 255, 255, 50);
static const struct ui_color color_shadow = UI_RGBA(0, 0, 0, 100);

static void format_time(char text[32], int64_t microseconds)
{
    if (microseconds < 0)
        microseconds = 0;
    int64_t seconds = microseconds / 1000000;
    if (seconds >= 3600)
        snprintf(text, 32, "%d:%02d:%02d", (int)(seconds / 3600),
                 (int)(seconds / 60 % 60), (int)(seconds % 60));
    else
        snprintf(text, 32, "%d:%02d", (int)(seconds / 60),
                 (int)(seconds % 60));
}

static float time_ratio(const struct player_ui_state *state, int64_t position)
{
    if (state->duration_us <= 0 || position <= 0)
        return 0.0f;
    if (position >= state->duration_us)
        return 1.0f;
    return (float)((double)position / (double)state->duration_us);
}

static float shown_ratio(const struct player_ui_state *state)
{
    return time_ratio(state, state->scrubbing ? state->scrub_us
                                              : state->position_us);
}

static bool in_circle(float x, float y, float cx, float cy, float r)
{
    return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
}

static bool in_rect(float x, float y, float rx, float ry, float rw, float rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

/* ---- popup layout -------------------------------------------------------
 * The popup is described once as a list of widgets; drawing and hit
 * testing both walk it. */

enum widget_kind { WIDGET_CHIP, WIDGET_TEXT, WIDGET_HINT, WIDGET_VALUE };

struct widget {
    enum widget_kind kind;
    int control; /* chips only */
    int x, y, width;
    bool selected;
    bool small; /* chips: smaller text */
    char text[96];
};

#define MAX_WIDGETS 64

struct popup_layout {
    struct widget items[MAX_WIDGETS];
    int count;
};

static struct widget *add_widget(struct popup_layout *layout,
                                 enum widget_kind kind, int x, int y,
                                 int width, const char *text)
{
    if (layout->count >= MAX_WIDGETS)
        return &layout->items[MAX_WIDGETS - 1];
    struct widget *widget = &layout->items[layout->count++];
    *widget = (struct widget){.kind = kind, .x = x, .y = y, .width = width};
    /* Single translation choke point: every popup label passes through here;
     * dynamic text (values, composed labels) falls through tr() unchanged. */
    snprintf(widget->text, sizeof(widget->text), "%s", tr(text));
    return widget;
}

static void add_chip(struct popup_layout *layout, int control, int x, int y,
                     int width, const char *text, bool selected)
{
    struct widget *chip = add_widget(layout, WIDGET_CHIP, x, y, width, text);
    chip->control = control;
    chip->selected = selected;
}

/* `count` equal chips across a column. */
static void add_chip_row(struct popup_layout *layout, int column, int row,
                         int count, int first_control,
                         const char *const *labels, int selected)
{
    int width = (COLUMN_W - (count - 1) * CHIP_GAP) / count;
    for (int i = 0; i < count; ++i)
        add_chip(layout, first_control + i,
                 COLUMN_X(column) + i * (width + CHIP_GAP), ROW_CHIP_Y(row),
                 width, labels[i], i == selected);
}

static void format_adjust(char text[32], enum player_adjust adjust,
                          float value)
{
    const struct player_adjust_info *info = &player_adjust_info[adjust];
    if ((adjust == PLAYER_ADJUST_HEAD_MOTION ||
         adjust == PLAYER_ADJUST_AUTOFOCUS ||
         adjust == PLAYER_ADJUST_FOCUS_HYSTERESIS) && value <= 0.0f) {
        snprintf(text, 32, "value.off");
        return;
    }
    if (adjust == PLAYER_ADJUST_FOCUS_FADE && value <= 0.0f) {
        snprintf(text, 32, "value.instant");
        return;
    }
    float shown = value * info->display_scale;
    if (shown > -0.05f && shown < 0.05f)
        shown = 0.0f; /* no "-0" */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
    snprintf(text, 32, info->format, shown);
#pragma GCC diagnostic pop
}

/* Label, [-], value, [+] on one line. */
static void add_stepper(struct popup_layout *layout,
                        const struct player_ui_state *state, int column,
                        int y, enum player_adjust adjust, const char *label)
{
    int x = COLUMN_X(column);
    add_widget(layout, WIDGET_TEXT, x, y + 10, STEPPER_LABEL_W,
               label ? label : player_adjust_info[adjust].label);
    int minus_x = x + STEPPER_LABEL_W + 6;
    int plus_x = x + COLUMN_W - STEPPER_BUTTON_W;
    add_chip(layout, PLAYER_UI_ADJUST + 2 * (int)adjust, minus_x, y,
             STEPPER_BUTTON_W, "−", false);
    add_chip(layout, PLAYER_UI_ADJUST + 2 * (int)adjust + 1, plus_x, y,
             STEPPER_BUTTON_W, "+", false);
    char value[32];
    format_adjust(value, adjust, state->adjust ? state->adjust[adjust]
                                               : player_adjust_info[adjust]
                                                     .standard);
    int value_x = minus_x + STEPPER_BUTTON_W;
    add_widget(layout, WIDGET_VALUE, value_x, y + 8, plus_x - value_x, value);
}

/* Controller tab: pairs of actions in two columns, help or the action
 * being assigned in the third. */
static void layout_buttons(const struct player_ui_state *state,
                           struct popup_layout *layout)
{
    static const struct {
        const char *label;
        int first, second; /* enum binding_action, -1 = none */
    } rows[6] = {
        {"bind.seek", BINDING_SEEK_BACK, BINDING_SEEK_FORWARD},
        {"bind.volume", BINDING_VOLUME_DOWN, BINDING_VOLUME_UP},
        {"bind.video", BINDING_PREVIOUS, BINDING_NEXT},
        {"bind.tilt", BINDING_TILT_DOWN, BINDING_TILT_UP},
        {"bind.zoom", BINDING_ZOOM_OUT, BINDING_ZOOM_IN},
        {"bind.playpause", BINDING_PLAY_PAUSE, -1},
    };
    for (int r = 0; r < 6; ++r) {
        int column = r / 3, row = r % 3;
        add_widget(layout, WIDGET_HINT, COLUMN_X(column), ROW_LABEL_Y(row), 0,
                   rows[r].label);
        int count = rows[r].second >= 0 ? 2 : 1;
        int width = (COLUMN_W - (count - 1) * CHIP_GAP) / count;
        for (int i = 0; i < count; ++i) {
            int action = i ? rows[r].second : rows[r].first;
            char text[64];
            if (state->learn_action == action)
                snprintf(text, sizeof(text), "%s", tr("learn.press"));
            else
                binding_describe(state->bindings
                                     ? state->bindings[action]
                                     : binding_action_info[action].standard,
                                 text, sizeof(text));
            add_chip(layout, PLAYER_UI_BINDING + action,
                     COLUMN_X(column) + i * (width + CHIP_GAP),
                     ROW_CHIP_Y(row), width, text,
                     state->learn_action == action);
            layout->items[layout->count - 1].small = true;
        }
    }
    int x = COLUMN_X(2), half = (COLUMN_W - CHIP_GAP) / 2;
    if (state->learn_action >= 0 &&
        state->learn_action < BINDING_ACTION_COUNT) {
        char label[96];
        snprintf(label, sizeof(label), tr("learn.assign"),
                 tr(binding_action_info[state->learn_action].label));
        add_widget(layout, WIDGET_TEXT, x, ROW_LABEL_Y(0), 0, label);
        add_widget(layout, WIDGET_HINT, x, ROW_LABEL_Y(0) + 40, 0,
                   "learn.hint1");
        add_widget(layout, WIDGET_HINT, x, ROW_LABEL_Y(0) + 70, 0,
                   "learn.hint2");
        add_widget(layout, WIDGET_HINT, x, ROW_LABEL_Y(0) + 100, 0,
                   "learn.hint3");
        add_chip(layout, PLAYER_UI_BINDING_CLEAR, x, ROW_CHIP_Y(2), half,
                 "action.remove", false);
        add_chip(layout, PLAYER_UI_BINDING_CANCEL, x + half + CHIP_GAP,
                 ROW_CHIP_Y(2), half, "action.cancel", false);
    } else {
        add_widget(layout, WIDGET_HINT, x, ROW_LABEL_Y(0), 0,
                   "buttons.hint1");
        add_widget(layout, WIDGET_HINT, x, ROW_LABEL_Y(0) + 30, 0,
                   "buttons.hint2");
        add_widget(layout, WIDGET_HINT, x, ROW_LABEL_Y(0) + 60, 0,
                   "buttons.hint3");
        add_widget(layout, WIDGET_HINT, x, ROW_LABEL_Y(0) + 90, 0,
                   "buttons.hint4");
        add_chip(layout, PLAYER_UI_BINDING_DEFAULTS, x, ROW_CHIP_Y(2),
                 COLUMN_W, "buttons.defaults", false);
    }
}

static void layout_popup(const struct player_ui_state *state,
                         struct popup_layout *layout)
{
    layout->count = 0;
    static const char *const tabs[PLAYER_UI_TAB_COUNT] = {
        "tab.view", "tab.position", "tab.picture", "tab.focus", "tab.buttons", "tab.info",
    };
    for (int i = 0; i < PLAYER_UI_TAB_COUNT; ++i)
        add_chip(layout, PLAYER_UI_TAB + i,
                 COLUMN_X(0) + i * (TAB_W + CHIP_GAP), TAB_Y, TAB_W, tabs[i],
                 state->popup_tab == i);

    if (state->popup_tab == PLAYER_UI_TAB_VIEW) {
        static const char *const modes[2] = {"mode.original", "mode.perspective"};
        static const char *const projections[4] = {"proj.flat", "180°", "360°",
                                                   "proj.fisheye"};
        static const char *const layouts[3] = {"Mono", "SBS", "TB"};
        const char *fovs[PLAYER_UI_PREVIEW_FOV_COUNT];
        char fov_text[PLAYER_UI_PREVIEW_FOV_COUNT][16];
        int fov_selected = -1;
        for (int i = 0; i < PLAYER_UI_PREVIEW_FOV_COUNT; ++i) {
            snprintf(fov_text[i], sizeof(fov_text[i]), "%d°",
                     player_ui_preview_fovs[i]);
            fovs[i] = fov_text[i];
            if (state->preview_fov == player_ui_preview_fovs[i])
                fov_selected = i;
        }
        add_widget(layout, WIDGET_HINT, COLUMN_X(0), ROW_LABEL_Y(0), 0,
                   "view.scrub_preview");
        add_chip_row(layout, 0, 0, 2, PLAYER_UI_PREVIEW_MODE, modes,
                     state->preview_perspective ? 1 : 0);
        add_widget(layout, WIDGET_HINT, COLUMN_X(0), ROW_LABEL_Y(1), 0,
                   "view.fov");
        add_chip_row(layout, 0, 1, PLAYER_UI_PREVIEW_FOV_COUNT,
                     PLAYER_UI_PREVIEW_FOV, fovs, fov_selected);
        static const char *const autohide[PLAYER_UI_AUTOHIDE_COUNT] = {
            "autohide.never", "autohide.5s", "autohide.15s",
        };
        int autohide_selected = 0;
        for (int i = 0; i < PLAYER_UI_AUTOHIDE_COUNT; ++i)
            if (player_ui_autohide[i] == state->autohide)
                autohide_selected = i;
        add_widget(layout, WIDGET_HINT, COLUMN_X(0), ROW_LABEL_Y(2), 0,
                   "view.autohide");
        add_chip_row(layout, 0, 2, PLAYER_UI_AUTOHIDE_COUNT,
                     PLAYER_UI_AUTOHIDE, autohide, autohide_selected);

        add_widget(layout, WIDGET_HINT, COLUMN_X(1), ROW_LABEL_Y(0), 0,
                   "view.head_motion");
        add_stepper(layout, state, 1, ROW_CHIP_Y(0),
                    PLAYER_ADJUST_HEAD_MOTION, NULL);
        add_widget(layout, WIDGET_HINT, COLUMN_X(1), ROW_LABEL_Y(1), 0,
                   "view.orientation");
        add_chip(layout, PLAYER_UI_RECENTER, COLUMN_X(1), ROW_CHIP_Y(1),
                 COLUMN_W, "action.center", false);
        static const char *const off_on[2] = {"value.off", "value.on"};
        add_widget(layout, WIDGET_HINT, COLUMN_X(1), ROW_LABEL_Y(2), 0,
                   "view.av1");
        add_chip_row(layout, 1, 2, 2, PLAYER_UI_AV1_SOFTWARE, off_on,
                     state->av1_software ? 1 : 0);

        static const char *const projection_labels[] = {
            [NV12_FORMAT_SOURCE_DEFAULT] = "proj.src_default",
            [NV12_FORMAT_SOURCE_NAME] = "proj.src_name",
            [NV12_FORMAT_SOURCE_IMAGE] = "proj.src_image",
            [NV12_FORMAT_SOURCE_USER] = "proj.user",
        };
        char projection_label[96];
        snprintf(projection_label, sizeof(projection_label), "%s",
                 tr(projection_labels[state->format_source <=
                                              NV12_FORMAT_SOURCE_USER
                                          ? state->format_source
                                          : NV12_FORMAT_SOURCE_USER]));
        if (state->projection_mode == NV12_PROJECTION_FISHEYE) {
            size_t used = strlen(projection_label);
            snprintf(projection_label + used,
                     sizeof(projection_label) - used, " · %u°",
                     state->fisheye_fov);
        }
        add_widget(layout, WIDGET_HINT, COLUMN_X(2), ROW_LABEL_Y(0), 0,
                   projection_label);
        /* "proj.fisheye" needs more room than the angles. */
        const int fisheye_w = 128;
        const int short_w = (COLUMN_W - fisheye_w - 3 * CHIP_GAP) / 3;
        for (int i = 0; i < 4; ++i)
            add_chip(layout, PLAYER_UI_PROJECTION + i,
                     COLUMN_X(2) + i * (short_w + CHIP_GAP), ROW_CHIP_Y(0),
                     i == 3 ? fisheye_w : short_w, projections[i],
                     i == (int)state->projection_mode);
        add_widget(layout, WIDGET_HINT, COLUMN_X(2), ROW_LABEL_Y(1), 0,
                   "label.stereo");
        add_chip_row(layout, 2, 1, 3, PLAYER_UI_STEREO, layouts,
                     (int)state->stereo_layout);
        add_chip(layout, PLAYER_UI_SWAP_EYES, COLUMN_X(2), ROW_CHIP_Y(2),
                 COLUMN_W,
                 state->swap_eyes ? "eyes.swapped" : "eyes.swap",
                 state->swap_eyes);
    } else if (state->popup_tab == PLAYER_UI_TAB_POSITION) {
        static const enum player_adjust grid[3][3] = {
            {PLAYER_ADJUST_ZOOM, PLAYER_ADJUST_HEIGHT, PLAYER_ADJUST_SLIDE},
            {PLAYER_ADJUST_TILT, PLAYER_ADJUST_YAW, PLAYER_ADJUST_ROLL},
            {PLAYER_ADJUST_STEREO_X, PLAYER_ADJUST_STEREO_Y,
             PLAYER_ADJUST_COUNT},
        };
        for (int column = 0; column < 3; ++column)
            for (int row = 0; row < 3; ++row)
                if (grid[column][row] != PLAYER_ADJUST_COUNT)
                    add_stepper(layout, state, column, STEPPER_Y(row),
                                grid[column][row], NULL);
        add_chip(layout, PLAYER_UI_RESET_POSITION, COLUMN_X(2), STEPPER_Y(2),
                 COLUMN_W, "action.reset", false);
        add_widget(layout, WIDGET_HINT, COLUMN_X(0), STEPPER_Y(3) - 4, 0,
                   "position.note");
    } else if (state->popup_tab == PLAYER_UI_TAB_PICTURE) {
        static const char *const levels[3] = {"level.norm", "level.low", "level.full"};
        add_widget(layout, WIDGET_HINT, COLUMN_X(0), ROW_LABEL_Y(0), 0,
                   "picture.blacklevel");
        add_chip_row(layout, 0, 0, 3, PLAYER_UI_LEVELS, levels,
                     (int)state->video_levels);
        add_widget(layout, WIDGET_HINT, COLUMN_X(0), ROW_LABEL_Y(1) + 6, 0,
                   "level.norm_hint");
        add_widget(layout, WIDGET_HINT, COLUMN_X(0), ROW_LABEL_Y(1) + 36, 0,
                   "level.low_hint");
        add_widget(layout, WIDGET_HINT, COLUMN_X(0), ROW_LABEL_Y(1) + 66, 0,
                   "level.full_hint");
        add_stepper(layout, state, 1, STEPPER_Y(0),
                    PLAYER_ADJUST_BRIGHTNESS, NULL);
        add_stepper(layout, state, 1, STEPPER_Y(1), PLAYER_ADJUST_CONTRAST,
                    NULL);
        add_stepper(layout, state, 1, STEPPER_Y(2),
                    PLAYER_ADJUST_SATURATION, NULL);
        add_stepper(layout, state, 2, STEPPER_Y(0), PLAYER_ADJUST_GAMMA,
                    NULL);
        add_chip(layout, PLAYER_UI_RESET_PICTURE, COLUMN_X(2), STEPPER_Y(2),
                 COLUMN_W, "action.reset", false);
    } else if (state->popup_tab == PLAYER_UI_TAB_FOCUS) {
        add_widget(layout, WIDGET_HINT, COLUMN_X(0), ROW_LABEL_Y(0), 0,
                   "focus.autofocus");
        add_stepper(layout, state, 0, ROW_CHIP_Y(0),
                    PLAYER_ADJUST_AUTOFOCUS, "label.strength");
        add_widget(layout, WIDGET_HINT, COLUMN_X(0), ROW_LABEL_Y(1), 0,
                   "focus.fade_label");
        add_stepper(layout, state, 0, ROW_CHIP_Y(1),
                    PLAYER_ADJUST_FOCUS_FADE, NULL);
        add_widget(layout, WIDGET_HINT, COLUMN_X(0), ROW_LABEL_Y(2), 0,
                   "focus.refocus");
        add_stepper(layout, state, 0, ROW_CHIP_Y(2),
                    PLAYER_ADJUST_FOCUS_HYSTERESIS, NULL);

        static const char *const thresholds[2] = {"threshold.always", "threshold.near"};
        add_widget(layout, WIDGET_HINT, COLUMN_X(1), ROW_LABEL_Y(0), 0,
                   "focus.trigger");
        add_chip_row(layout, 1, 0, 2, PLAYER_UI_FOCUS_THRESHOLD, thresholds,
                     state->focus_threshold ? 1 : 0);
        add_widget(layout, WIDGET_HINT, COLUMN_X(1), ROW_LABEL_Y(1), 0,
                   state->focus_threshold
                       ? "focus.far_hint"
                       : "focus.limit");
        add_stepper(layout, state, 1, ROW_CHIP_Y(1),
                    PLAYER_ADJUST_FOCUS_DISTANCE, NULL);

        add_widget(layout, WIDGET_HINT, COLUMN_X(2), ROW_LABEL_Y(0), 0,
                   "focus.check");
        add_chip(layout, PLAYER_UI_GAZE_DEBUG, COLUMN_X(2), ROW_CHIP_Y(0),
                 COLUMN_W, "focus.show_gaze", state->gaze_debug);
        add_widget(layout, WIDGET_HINT, COLUMN_X(2), ROW_LABEL_Y(1) + 6, 0,
                   "focus.hint1");
        add_widget(layout, WIDGET_HINT, COLUMN_X(2), ROW_LABEL_Y(1) + 36, 0,
                   "focus.hint2");
        add_widget(layout, WIDGET_HINT, COLUMN_X(2), ROW_LABEL_Y(1) + 66, 0,
                   "focus.global_note");
    } else if (state->popup_tab == PLAYER_UI_TAB_BUTTONS) {
        layout_buttons(state, layout);
    }
}

float player_adjust_step(enum player_adjust adjust, float value,
                         int direction)
{
    const struct player_adjust_info *info = &player_adjust_info[adjust];
    /* Stay on the step grid so repeated steps do not drift. */
    float steps = (value - info->standard) / info->step;
    long index = (long)(steps + (steps < 0.0f ? -0.5f : 0.5f)) + direction;
    float result = info->standard + (float)index * info->step;
    if (result < info->minimum)
        result = info->minimum;
    if (result > info->maximum)
        result = info->maximum;
    return result;
}

/* ---- drawing ----------------------------------------------------------- */

static void round_button(struct ui_canvas *canvas, float cx, float r,
                         bool hover, bool active)
{
    ui_fill_circle(canvas, cx, ROW_Y, r,
                   active ? color_accent
                   : hover ? color_button_hover
                           : color_button);
}

static void draw_chip(struct player_ui *ui, struct ui_canvas *canvas, int x,
                      int y, int width, const char *label, bool selected,
                      bool hover, bool small)
{
    ui_fill_round_rect(canvas, x, y, width, CHIP_H, CHIP_H / 2.0f,
                       selected ? color_accent
                       : hover ? color_button_hover
                               : color_button);
    ui_draw_text(canvas, selected ? ui->bold : ui->font, small ? 20 : 24,
                 x + width / 2, y + (small ? 10 : 8), width - 16,
                 UI_ALIGN_CENTER, color_text, label);
}

/* Information tab: the description across two columns, facts in the
 * third. */
static void draw_info(struct player_ui *ui, struct ui_canvas *canvas,
                      const struct jf_details *details)
{
    const int left = COLUMN_X(0), top = ROW_LABEL_Y(0);
    const int text_w = COLUMN_X(2) - 44 - left;
    if (!details) {
        ui_draw_text_wrapped(canvas, ui->font, 24, left, top, text_w, 32, 3,
                             UI_ALIGN_LEFT, color_dim,
                             tr("info.none"));
        return;
    }
    ui_draw_text(canvas, ui->font, 22, left, top, 0, UI_ALIGN_LEFT,
                 color_dim, tr("info.description"));
    ui_draw_text_wrapped(canvas, ui->font, 23, left, top + 34, text_w, 31, 6,
                         UI_ALIGN_LEFT,
                         details->overview[0] ? color_text : color_dim,
                         details->overview[0] ? details->overview
                                              : tr("info.no_description"));
    char year[16] = "", rating[16] = "";
    if (details->year > 0)
        snprintf(year, sizeof(year), "%d", details->year);
    if (details->community_rating > 0.0f)
        snprintf(rating, sizeof(rating), "%.1f", details->community_rating);
    const struct {
        const char *label, *value;
    } rows[] = {
        {"info.year", year},          {"info.rating", rating},
        {"info.studio", details->studios}, {"info.people", details->people},
        {"info.genres", details->genres},  {"info.tags", details->tags},
    };
    const int x = COLUMN_X(2), label_w = 150, bottom = POPUP_Y + POPUP_H - 20;
    int y = top;
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        if (!rows[i].value[0])
            continue;
        if (y + 30 > bottom)
            break;
        ui_draw_text(canvas, ui->font, 22, x, y + 2, label_w, UI_ALIGN_LEFT,
                     color_dim, tr(rows[i].label));
        int lines = (bottom - y) / 30 >= 2 ? 2 : 1;
        y += 30 * ui_draw_text_wrapped(canvas, ui->font, 23, x + label_w, y,
                                       COLUMN_W - label_w, 30, lines,
                                       UI_ALIGN_LEFT, color_text,
                                       rows[i].value) +
             8;
    }
}

static void draw_popup(struct player_ui *ui, struct ui_canvas *canvas,
                       const struct player_ui_state *state)
{
    ui_shadow_round_rect(canvas, POPUP_X, POPUP_Y + 6, POPUP_W, POPUP_H, 26,
                         22, color_shadow);
    ui_fill_round_rect(canvas, POPUP_X, POPUP_Y, POPUP_W, POPUP_H, 26,
                       UI_RGBA(17, 18, 22, 240));
    ui_stroke_round_rect(canvas, POPUP_X, POPUP_Y, POPUP_W, POPUP_H, 26, 2,
                         color_border);
    ui_fill_rect(canvas, POPUP_X + 20, TAB_Y + CHIP_H + 10, POPUP_W - 40, 2,
                 color_border);
    for (int column = state->popup_tab == PLAYER_UI_TAB_INFO ? 2 : 1;
         column < 3; ++column)
        ui_fill_rect(canvas, COLUMN_X(column) - 22, TAB_Y + CHIP_H + 26, 2,
                     POPUP_H - CHIP_H - 64, color_border);
    if (state->popup_tab == PLAYER_UI_TAB_INFO)
        draw_info(ui, canvas, state->details);
    struct popup_layout layout;
    layout_popup(state, &layout);
    for (int i = 0; i < layout.count; ++i) {
        const struct widget *widget = &layout.items[i];
        switch (widget->kind) {
        case WIDGET_CHIP:
            draw_chip(ui, canvas, widget->x, widget->y, widget->width,
                      widget->text, widget->selected,
                      state->hover == widget->control, widget->small);
            break;
        case WIDGET_TEXT:
            ui_draw_text(canvas, ui->font, 24, widget->x, widget->y,
                         widget->width, UI_ALIGN_LEFT, color_text,
                         widget->text);
            break;
        case WIDGET_HINT:
            ui_draw_text(canvas, ui->font, 22, widget->x, widget->y, 0,
                         UI_ALIGN_LEFT, color_dim, widget->text);
            break;
        case WIDGET_VALUE:
            ui_draw_text(canvas, ui->bold, 24, widget->x + widget->width / 2,
                         widget->y, widget->width - 8, UI_ALIGN_CENTER,
                         color_text, widget->text);
            break;
        }
    }
}

/* Name of the chapter `position_us` lies in, or NULL. */
static const char *chapter_at(const struct player_ui_state *state,
                              int64_t position_us)
{
    const struct jf_details *details = state->details;
    const char *name = NULL;
    for (int i = 0; details && i < details->chapter_count; ++i)
        if (details->chapter_us[i] <= position_us &&
            details->chapter_names[i][0])
            name = details->chapter_names[i];
    return name;
}

/* Picture (if any) with the time (and chapter) below it; without a
 * picture just the label. */
static void draw_preview(struct player_ui *ui, struct ui_canvas *canvas,
                         const struct player_ui_state *state)
{
    const struct ui_image *image = state->preview;
    int image_w = 0, image_h = 0;
    if (image && image->width > 0 && image->height > 0) {
        image_h = PREVIEW_IMAGE_H;
        image_w = image->width * PREVIEW_IMAGE_H / image->height;
        if (image_w > 2 * PREVIEW_IMAGE_H)
            image_w = 2 * PREVIEW_IMAGE_H;
    }
    char text[96];
    format_time(text, state->preview_us);
    const char *chapter = chapter_at(state, state->preview_us);
    if (chapter) {
        size_t used = strlen(text);
        snprintf(text + used, sizeof(text) - used, " · %s", chapter);
    }
    int width = image_w ? image_w + 2 * PREVIEW_PAD : 140;
    if (chapter && width < 360)
        width = 360;
    int height = (image_h ? image_h + PREVIEW_PAD : 0) + PREVIEW_LABEL_H;
    float center = BAR_X + BAR_W * time_ratio(state, state->preview_us);
    float x = center - width / 2.0f;
    if (x < CARD_X)
        x = CARD_X;
    if (x > CARD_X + CARD_W - width)
        x = CARD_X + CARD_W - width;
    float y = PREVIEW_BOTTOM - height;
    ui_shadow_round_rect(canvas, x, y + 6, width, height, 18, 18,
                         color_shadow);
    ui_fill_round_rect(canvas, x, y, width, height, 18,
                       UI_RGBA(17, 18, 22, 240));
    ui_stroke_round_rect(canvas, x, y, width, height, 18, 2, color_border);
    if (image_w)
        ui_draw_image(canvas, image, x + (width - image_w) / 2.0f,
                      y + PREVIEW_PAD, image_w, image_h, 12, 0.0f);
    ui_draw_text(canvas, ui->bold, 26, (int)(x + width / 2.0f),
                 (int)(y + height - PREVIEW_LABEL_H + 5), width - 12,
                 UI_ALIGN_CENTER, color_text, text);
}

void player_ui_render(struct player_ui *ui, const struct player_ui_state *state,
                      uint32_t *pixels, bool bgra)
{
    if (!ui->font)
        ui->font = ui_font_load(false);
    if (!ui->bold)
        ui->bold = ui_font_load(true);
    struct ui_canvas canvas = {(uint8_t *)pixels, PLAYER_UI_WIDTH,
                               PLAYER_UI_HEIGHT, bgra};
    ui_clear(&canvas, UI_RGBA(0, 0, 0, 0));
    if (state->popup_open)
        draw_popup(ui, &canvas, state);

    ui_shadow_round_rect(&canvas, CARD_X, CARD_Y + 8, CARD_W, CARD_H, 36, 26,
                         color_shadow);
    ui_fill_round_rect(&canvas, CARD_X, CARD_Y, CARD_W, CARD_H, 36,
                       color_card);
    ui_stroke_round_rect(&canvas, CARD_X, CARD_Y, CARD_W, CARD_H, 36,
                         state->grabbed ? 4 : 2,
                         state->grabbed ? color_accent : color_border);

    ui_draw_text(&canvas, ui->bold, 32, PLAYER_UI_WIDTH / 2, CARD_Y + 20,
                 1100, UI_ALIGN_CENTER, color_text,
                 state->title && state->title[0] ? state->title : tr("value.video"));

    char position[32], duration[32];
    format_time(position, state->scrubbing ? state->scrub_us
                                           : state->position_us);
    format_time(duration, state->duration_us);
    if (state->loading && !state->scrubbing)
        ui_draw_text(&canvas, ui->bold, 26, CARD_X + 40, BAR_Y - 16, 0,
                     UI_ALIGN_LEFT, color_accent, tr("status.loading"));
    else
        ui_draw_text(&canvas, ui->font, 26, CARD_X + 40, BAR_Y - 16, 0,
                     UI_ALIGN_LEFT, color_text, position);
    ui_draw_text(&canvas, ui->font, 26, CARD_X + CARD_W - 40, BAR_Y - 16, 0,
                 UI_ALIGN_RIGHT, color_dim, duration);
    float ratio = shown_ratio(state);
    bool bar_hot = state->scrubbing || state->hover == PLAYER_UI_SEEK_BAR;
    ui_fill_round_rect(&canvas, BAR_X, BAR_Y, BAR_W, BAR_H, BAR_H / 2.0f,
                       color_track);
    if (ratio > 0.0f)
        ui_fill_round_rect(&canvas, BAR_X, BAR_Y, BAR_W * ratio, BAR_H,
                           BAR_H / 2.0f, color_accent);
    for (int i = 0; state->details && i < state->details->chapter_count;
         ++i) {
        float mark = time_ratio(state, state->details->chapter_us[i]);
        if (mark > 0.0f && mark < 1.0f)
            ui_fill_rect(&canvas, BAR_X + BAR_W * mark - 2, BAR_Y - 6, 4,
                         BAR_H + 12, UI_RGBA(255, 255, 255, 170));
    }
    float knob_x = BAR_X + BAR_W * ratio;
    ui_fill_circle(&canvas, knob_x, BAR_Y + BAR_H / 2.0f, bar_hot ? 20 : 15,
                   UI_RGBA(0, 0, 0, 80));
    ui_fill_circle(&canvas, knob_x, BAR_Y + BAR_H / 2.0f, bar_hot ? 17 : 12,
                   UI_RGBA(255, 255, 255, 255));

    int hover = state->hover;
    round_button(&canvas, BACK_X, SMALL_R, hover == PLAYER_UI_BACK, false);
    ui_draw_icon(&canvas, UI_ICON_EXIT, BACK_X, ROW_Y, 38, color_text);

    round_button(&canvas, SKIP_BACK_X, SKIP_R, hover == PLAYER_UI_SEEK_BACK,
                 false);
    ui_draw_text(&canvas, ui->bold, 26, SKIP_BACK_X, ROW_Y - 18, 0,
                 UI_ALIGN_CENTER, color_text, "−10");
    round_button(&canvas, SKIP_FORWARD_X, SKIP_R,
                 hover == PLAYER_UI_SEEK_FORWARD, false);
    ui_draw_text(&canvas, ui->bold, 26, SKIP_FORWARD_X, ROW_Y - 18, 0,
                 UI_ALIGN_CENTER, color_text, "+10");

    bool play_hover = hover == PLAYER_UI_TOGGLE_PAUSE;
    ui_fill_circle(&canvas, PLAY_X, ROW_Y, PLAY_R,
                   play_hover ? color_accent : UI_RGBA(240, 243, 248, 255));
    ui_draw_icon(&canvas, state->paused ? UI_ICON_PLAY : UI_ICON_PAUSE,
                 PLAY_X + (state->paused ? 4 : 0), ROW_Y, 46,
                 play_hover ? UI_RGBA(255, 255, 255, 255)
                            : UI_RGBA(20, 22, 28, 255));

    static const char *const end_labels[PLAYER_END_COUNT] = {
        "end.stop", "end.repeat", "end.next",
    };
    int end_action = state->end_action >= 0 &&
                             state->end_action < PLAYER_END_COUNT
                         ? state->end_action
                         : PLAYER_END_STOP;
    ui_fill_round_rect(&canvas, END_X, ROW_Y - END_H / 2.0f, END_W, END_H,
                       END_H / 2.0f,
                       hover == PLAYER_UI_END_ACTION ? color_button_hover
                                                     : color_button);
    ui_draw_text(&canvas, end_action == PLAYER_END_STOP ? ui->font : ui->bold,
                 22, END_X + END_W / 2, ROW_Y - 15, END_W - 24,
                 UI_ALIGN_CENTER,
                 end_action == PLAYER_END_STOP ? color_dim : color_accent,
                 tr(end_labels[end_action]));

    round_button(&canvas, VOLUME_DOWN_X, VOLUME_R,
                 hover == PLAYER_UI_VOLUME_DOWN, false);
    ui_draw_text(&canvas, ui->bold, 28, VOLUME_DOWN_X, ROW_Y - 20, 0,
                 UI_ALIGN_CENTER, color_text, "−");
    round_button(&canvas, VOLUME_UP_X, VOLUME_R, hover == PLAYER_UI_VOLUME_UP,
                 false);
    ui_draw_text(&canvas, ui->bold, 28, VOLUME_UP_X, ROW_Y - 20, 0,
                 UI_ALIGN_CENTER, color_text, "+");
    char volume[24];
    snprintf(volume, sizeof(volume), "%d %%",
             (int)(state->volume * 100.0f + 0.5f));
    ui_draw_text(&canvas, ui->font, 20, (VOLUME_DOWN_X + VOLUME_UP_X) / 2,
                 ROW_Y - 34, 0, UI_ALIGN_CENTER, color_dim, tr("label.volume"));
    ui_draw_text(&canvas, ui->bold, 26, (VOLUME_DOWN_X + VOLUME_UP_X) / 2,
                 ROW_Y - 8, 0, UI_ALIGN_CENTER,
                 state->volume <= 0.0f ? color_dim : color_text,
                 state->volume <= 0.0f ? tr("value.muted") : volume);

    round_button(&canvas, VIEW_X, SMALL_R, hover == PLAYER_UI_VIEW_POPUP,
                 state->popup_open);
    ui_draw_icon(&canvas, UI_ICON_CUBE, VIEW_X, ROW_Y, 38, color_text);

    if (state->debug_text && state->debug_text[0])
        ui_draw_text(&canvas, ui->font, 20, CARD_X + 40, CARD_Y + CARD_H - 34,
                     CARD_W - 80, UI_ALIGN_LEFT, color_dim, state->debug_text);

    if (state->preview_visible && state->duration_us > 0)
        draw_preview(ui, &canvas, state);
}

void player_ui_close(struct player_ui *ui)
{
    ui_font_free(ui->font);
    ui_font_free(ui->bold);
    ui->font = ui->bold = NULL;
}

/* ---- input ------------------------------------------------------------- */

int player_ui_hit_test(const struct player_ui_state *state, float u, float v)
{
    float x = u * PLAYER_UI_WIDTH, y = v * PLAYER_UI_HEIGHT;
    if (state->popup_open && in_rect(x, y, POPUP_X, POPUP_Y, POPUP_W,
                                     POPUP_H)) {
        struct popup_layout layout;
        layout_popup(state, &layout);
        for (int i = 0; i < layout.count; ++i) {
            const struct widget *widget = &layout.items[i];
            if (widget->kind == WIDGET_CHIP &&
                in_rect(x, y, widget->x, widget->y, widget->width, CHIP_H))
                return widget->control;
        }
        return PLAYER_UI_NONE;
    }
    if (state->duration_us > 0 && y >= BAR_HIT_TOP && y < BAR_HIT_BOTTOM &&
        x >= BAR_X - 16 && x < BAR_X + BAR_W + 16)
        return PLAYER_UI_SEEK_BAR;
    if (in_circle(x, y, BACK_X, ROW_Y, SMALL_R + 6))
        return PLAYER_UI_BACK;
    if (in_circle(x, y, SKIP_BACK_X, ROW_Y, SKIP_R + 6))
        return PLAYER_UI_SEEK_BACK;
    if (in_circle(x, y, PLAY_X, ROW_Y, PLAY_R + 6))
        return PLAYER_UI_TOGGLE_PAUSE;
    if (in_circle(x, y, SKIP_FORWARD_X, ROW_Y, SKIP_R + 6))
        return PLAYER_UI_SEEK_FORWARD;
    if (in_circle(x, y, VIEW_X, ROW_Y, SMALL_R + 6))
        return PLAYER_UI_VIEW_POPUP;
    if (in_rect(x, y, END_X, ROW_Y - END_H / 2.0f, END_W, END_H))
        return PLAYER_UI_END_ACTION;
    if (in_circle(x, y, VOLUME_DOWN_X, ROW_Y, VOLUME_R + 6))
        return PLAYER_UI_VOLUME_DOWN;
    if (in_circle(x, y, VOLUME_UP_X, ROW_Y, VOLUME_R + 6))
        return PLAYER_UI_VOLUME_UP;
    return PLAYER_UI_NONE;
}

bool player_ui_on_panel(const struct player_ui_state *state, float u, float v)
{
    float x = u * PLAYER_UI_WIDTH, y = v * PLAYER_UI_HEIGHT;
    return in_rect(x, y, CARD_X, CARD_Y, CARD_W, CARD_H) ||
           (state->popup_open &&
            in_rect(x, y, POPUP_X, POPUP_Y, POPUP_W, POPUP_H));
}

float player_ui_seek_ratio(float u)
{
    float ratio = (u * PLAYER_UI_WIDTH - BAR_X) / BAR_W;
    return ratio < 0.0f ? 0.0f : ratio > 1.0f ? 1.0f : ratio;
}
