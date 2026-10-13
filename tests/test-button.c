/* Self-check for the trigger edge detector in stereonaut-renderer.c.
 * button_update() is static there, so the logic is mirrored here verbatim;
 * if you change one, change both. Build & run:
 *   cc -std=c11 -Wall -Wextra -Werror tests/test-button.c -o /tmp/tb && /tmp/tb
 */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

struct button { bool down, suppressed; };

static bool button_update(struct button *b, bool active, float value,
                          float press, float release, bool *held)
{
    bool was = b->down;
    if (active) {
        if (!b->down && value >= press)
            b->down = true;
        else if (b->down && value < release)
            b->down = false;
        if (!b->down)
            b->suppressed = false;
    }
    if (held)
        *held = b->down;
    return active && b->down && !was && !b->suppressed;
}

static void button_suppress(struct button *b) { b->suppressed = true; }

#define PRESS 0.50f
#define RELEASE 0.03f
#define up(b, act, v) button_update((b), (act), (v), PRESS, RELEASE, NULL)

int main(void)
{
    /* Normal: rising edge once, not while held, again after release+press. */
    struct button b = {0};
    assert(up(&b, true, 0.0f) == false);
    assert(up(&b, true, 1.0f) == true);   /* press edge */
    assert(up(&b, true, 1.0f) == false);  /* held, no repeat */
    assert(up(&b, true, 0.0f) == false);  /* release, no edge */
    assert(up(&b, true, 1.0f) == true);   /* new press */

    /* Suppress (the bug): trigger held from "back" must NOT fire until the
     * user genuinely releases and presses again. */
    b = (struct button){0};
    up(&b, true, 1.0f);                    /* holding "back" */
    button_suppress(&b);
    assert(up(&b, true, 1.0f) == false);   /* still held -> swallowed */
    assert(up(&b, true, 1.0f) == false);
    assert(up(&b, true, 0.0f) == false);   /* release -> re-arm */
    assert(up(&b, true, 1.0f) == true);    /* fresh press now counts */

    /* Focus blip while suppressed+held (the hang): inactive frames must not
     * fabricate a press nor drop suppression; held trigger stays swallowed. */
    b = (struct button){0};
    up(&b, true, 1.0f);
    button_suppress(&b);
    assert(up(&b, false, 0.0f) == false);  /* VISIBLE (inactive) */
    assert(up(&b, true, 1.0f) == false);   /* FOCUSED, still held -> no phantom */
    assert(up(&b, false, 0.0f) == false);
    assert(up(&b, true, 1.0f) == false);   /* still no phantom */
    assert(up(&b, true, 0.0f) == false);   /* finally released -> re-arm */
    assert(up(&b, true, 1.0f) == true);

    /* held state survives an inactive frame (scrubbing across a focus blip). */
    b = (struct button){0};
    bool held = false;
    button_update(&b, true, 1.0f, PRESS, RELEASE, &held);
    assert(held == true);
    button_update(&b, false, 0.0f, PRESS, RELEASE, &held);
    assert(held == true);                  /* not dropped while unfocused */

    printf("test-button: all passed\n");
    return 0;
}
