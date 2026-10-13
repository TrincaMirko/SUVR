#include "controller-bindings.h"

#include "i18n.h"

#include <stdio.h>

#define BIND(input) {(input), -1}
#define COMBO(modifier, input) {(input), (modifier)}

const struct binding_action_info binding_action_info[BINDING_ACTION_COUNT] = {
    [BINDING_SEEK_BACK] = {"−10 s", "seek_back", false,
                           BIND(BINDING_ANY_STICK_LEFT)},
    [BINDING_SEEK_FORWARD] = {"+10 s", "seek_forward", false,
                              BIND(BINDING_ANY_STICK_RIGHT)},
    [BINDING_VOLUME_DOWN] = {"bindaction.volume_down", "volume_down", true,
                             BIND(BINDING_ANY_STICK_DOWN)},
    [BINDING_VOLUME_UP] = {"bindaction.volume_up", "volume_up", true,
                           BIND(BINDING_ANY_STICK_UP)},
    [BINDING_PREVIOUS] = {"bindaction.previous", "previous", false,
                          COMBO(BINDING_RIGHT_GRIP, BINDING_RIGHT_STICK_LEFT)},
    [BINDING_NEXT] = {"bindaction.next", "next", false,
                      COMBO(BINDING_RIGHT_GRIP, BINDING_RIGHT_STICK_RIGHT)},
    [BINDING_TILT_DOWN] = {"bindaction.tilt_down", "tilt_down", true,
                           BIND(BINDING_DPAD_DOWN)},
    [BINDING_TILT_UP] = {"bindaction.tilt_up", "tilt_up", true,
                         BIND(BINDING_DPAD_UP)},
    [BINDING_ZOOM_OUT] = {"bindaction.zoom_out", "zoom_out", true,
                          BIND(BINDING_DPAD_LEFT)},
    [BINDING_ZOOM_IN] = {"bindaction.zoom_in", "zoom_in", true,
                         BIND(BINDING_DPAD_RIGHT)},
    [BINDING_PLAY_PAUSE] = {"bindaction.playpause", "play_pause", false,
                            BIND(BINDING_A)},
};

/* Names without the hand; hand_of() gives it. */
static const char *const input_names[BINDING_INPUT_COUNT] = {
    [BINDING_LEFT_STICK_LEFT] = "btn.stick_left",
    [BINDING_LEFT_STICK_RIGHT] = "btn.stick_right",
    [BINDING_LEFT_STICK_UP] = "btn.stick_up",
    [BINDING_LEFT_STICK_DOWN] = "btn.stick_down",
    [BINDING_RIGHT_STICK_LEFT] = "btn.stick_left",
    [BINDING_RIGHT_STICK_RIGHT] = "btn.stick_right",
    [BINDING_RIGHT_STICK_UP] = "btn.stick_up",
    [BINDING_RIGHT_STICK_DOWN] = "btn.stick_down",
    [BINDING_ANY_STICK_LEFT] = "btn.stick_left",
    [BINDING_ANY_STICK_RIGHT] = "btn.stick_right",
    [BINDING_ANY_STICK_UP] = "btn.stick_up",
    [BINDING_ANY_STICK_DOWN] = "btn.stick_down",
    [BINDING_LEFT_STICK_CLICK] = "btn.stick_click",
    [BINDING_RIGHT_STICK_CLICK] = "btn.stick_click",
    [BINDING_LEFT_GRIP] = "btn.grip",
    [BINDING_RIGHT_GRIP] = "btn.grip",
    [BINDING_LEFT_BUMPER] = "btn.bumper",
    [BINDING_RIGHT_BUMPER] = "btn.bumper",
    [BINDING_DPAD_UP] = "btn.dpad_up",
    [BINDING_DPAD_DOWN] = "btn.dpad_down",
    [BINDING_DPAD_LEFT] = "btn.dpad_left",
    [BINDING_DPAD_RIGHT] = "btn.dpad_right",
    [BINDING_VIEW] = "btn.view",
    [BINDING_A] = "A",
    [BINDING_B] = "B",
    [BINDING_X] = "X",
    [BINDING_Y] = "Y",
    [BINDING_MENU] = "btn.menu",
};

/* 'L', 'R', or 0 for inputs that exist once or on either hand. */
static char hand_of(int input)
{
    switch (input) {
    case BINDING_LEFT_STICK_LEFT: case BINDING_LEFT_STICK_RIGHT:
    case BINDING_LEFT_STICK_UP: case BINDING_LEFT_STICK_DOWN:
    case BINDING_LEFT_STICK_CLICK: case BINDING_LEFT_GRIP:
    case BINDING_LEFT_BUMPER:
        return 'L';
    case BINDING_RIGHT_STICK_LEFT: case BINDING_RIGHT_STICK_RIGHT:
    case BINDING_RIGHT_STICK_UP: case BINDING_RIGHT_STICK_DOWN:
    case BINDING_RIGHT_STICK_CLICK: case BINDING_RIGHT_GRIP:
    case BINDING_RIGHT_BUMPER:
        return 'R';
    default:
        return 0;
    }
}

static bool valid_input(int input)
{
    return input >= 0 && input < BINDING_INPUT_COUNT;
}

int binding_encode(struct binding binding)
{
    if (!valid_input(binding.input))
        return 0;
    int modifier = binding_is_button(binding.modifier) ? binding.modifier : -1;
    return (modifier + 1) * 64 + binding.input + 1;
}

struct binding binding_decode(int value)
{
    struct binding binding = {-1, -1};
    if (value <= 0)
        return binding;
    int input = value % 64 - 1, modifier = value / 64 - 1;
    if (valid_input(input) && modifier != input &&
        (modifier == -1 || binding_is_button(modifier))) {
        binding.input = input;
        binding.modifier = modifier;
    }
    return binding;
}

static void append_name(char *text, size_t size, int input, bool hand)
{
    size_t used = 0;
    while (used < size && text[used])
        ++used;
    char side = hand ? hand_of(input) : 0;
    if (side)
        snprintf(text + used, size - used, "%c %s", side,
                 tr(input_names[input]));
    else
        snprintf(text + used, size - used, "%s", tr(input_names[input]));
}

void binding_describe(struct binding binding, char *text, size_t size)
{
    if (size == 0)
        return;
    text[0] = '\0';
    if (!valid_input(binding.input)) {
        snprintf(text, size, "%s", tr("bind.unassigned"));
        return;
    }
    if (binding_is_button(binding.modifier)) {
        append_name(text, size, binding.modifier, true);
        size_t used = 0;
        while (used < size && text[used])
            ++used;
        snprintf(text + used, size - used, " + ");
        /* "R Griff + Stick →": the hand once if both are on it. */
        append_name(text, size, binding.input,
                    hand_of(binding.input) != hand_of(binding.modifier));
        return;
    }
    append_name(text, size, binding.input, true);
}

bool binding_is_modifier(const struct binding bindings[BINDING_ACTION_COUNT],
                         int button)
{
    for (int i = 0; i < BINDING_ACTION_COUNT; ++i)
        if (valid_input(bindings[i].input) && bindings[i].modifier == button)
            return true;
    return false;
}

/* The either-hand variant of a stick direction, or -1. */
static int any_hand(int input)
{
    if (input >= BINDING_LEFT_STICK_LEFT && input <= BINDING_RIGHT_STICK_DOWN)
        return BINDING_ANY_STICK_LEFT + input % 4;
    return -1;
}

int binding_match(const struct binding bindings[BINDING_ACTION_COUNT],
                  int input, const bool held[BINDING_INPUT_COUNT],
                  int *modifier)
{
    const int candidates[2] = {input, any_hand(input)};
    *modifier = -1;
    for (int with_modifier = 1; with_modifier >= 0; --with_modifier)
        for (int c = 0; c < 2; ++c) {
            if (candidates[c] < 0)
                continue;
            for (int i = 0; i < BINDING_ACTION_COUNT; ++i) {
                const struct binding *binding = &bindings[i];
                if (binding->input != candidates[c])
                    continue;
                if (with_modifier && binding_is_button(binding->modifier) &&
                    held[binding->modifier]) {
                    *modifier = binding->modifier;
                    return i;
                }
                if (!with_modifier && binding->modifier < 0)
                    return i;
            }
        }
    return -1;
}
