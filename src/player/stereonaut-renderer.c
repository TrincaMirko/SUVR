/* Continuously render decoded DMA-BUF frames (Iris, dav1d or a PC hardware
 * decoder) on the validated OpenXR quad. */
#define _POSIX_C_SOURCE 200809L
#define XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_PLATFORM_XLIB

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/dma-buf.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "browser-ui.h"
#include "controller-bindings.h"
#include "jellyfin.h"
#include "player-ui.h"
#include "settings.h"
#include "title-settings.h"
#include "trickplay.h"
#include "ui-draw.h"
#include "nv12-playback-ipc.h"
#include "player-ui-ipc.h"

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define MAX_IMAGES 8U
#define VIDEO_BUFFER_COUNT 12U
#define UBO_STRIDE 256U
#define PI_F 3.14159265358979f
#define DRM_FORMAT_MOD_LINEAR UINT64_C(0)
#define DRM_FORMAT_MOD_QCOM_COMPRESSED UINT64_C(0x0500000000000001)
#define UI_WIDTH PLAYER_UI_WIDTH
#define UI_HEIGHT PLAYER_UI_HEIGHT
#define UI_BYTES (UI_WIDTH * UI_HEIGHT * 4U)
#define UI_PANEL_WIDTH_METERS PLAYER_UI_PANEL_WIDTH_METERS
#define UI_PANEL_HEIGHT_METERS PLAYER_UI_PANEL_HEIGHT_METERS
#define GRAB_RECENTER_COS 0.5f
/* The select action is analog so the press point can be lower than the
 * trigger's full click travel. */
#define SELECT_PRESS_THRESHOLD 0.50f
#define SELECT_RELEASE_THRESHOLD 0.03f
/* Thumbstick left/right: same low threshold as the trigger, one action per
 * deflection. */
#define STICK_PRESS_THRESHOLD 0.30f
#define STICK_RELEASE_THRESHOLD 0.15f
/* Grip as a modifier of button bindings: its analog value, because the
 * Frame's grip click needs a firm squeeze that a hand also moving the stick
 * rarely keeps up. */
#define GRIP_PRESS_THRESHOLD 0.40f
#define GRIP_RELEASE_THRESHOLD 0.25f

/* Holding a stick direction or button repeats its action (volume, tilt,
 * zoom) after the delay, then at the interval. */
#define BINDING_REPEAT_DELAY 0.5
#define BINDING_REPEAT_INTERVAL 0.15

/* Buttons beyond trigger and grip, for the controller bindings. Only
 * valve/frame_controller has them all (paths checked on the Frame with
 * tools/openxr-playback/xr-input-profile-probe.c: the right controller has
 * A/B/X/Y and menu, the left the d-pad and view); the Index profile gets
 * the stick clicks and A/B. */
static const struct {
    int input;          /* enum binding_input */
    const char *action; /* OpenXR action name */
    const char *label;
    unsigned hand;      /* 0 left, 1 right */
    const char *component;
    bool index;         /* also bound for valve/index_controller */
} button_inputs[] = {
    {BINDING_LEFT_STICK_CLICK, "left_stick_click", "Stick-Klick links", 0,
     "thumbstick/click", true},
    {BINDING_RIGHT_STICK_CLICK, "right_stick_click", "Stick-Klick rechts",
     1, "thumbstick/click", true},
    {BINDING_LEFT_BUMPER, "left_bumper", "Bumper links", 0, "bumper/click",
     false},
    {BINDING_RIGHT_BUMPER, "right_bumper", "Bumper rechts", 1,
     "bumper/click", false},
    {BINDING_DPAD_UP, "dpad_up", "Kreuz hoch", 0, "dpad_up/click", false},
    {BINDING_DPAD_DOWN, "dpad_down", "Kreuz runter", 0, "dpad_down/click",
     false},
    {BINDING_DPAD_LEFT, "dpad_left", "Kreuz links", 0, "dpad_left/click",
     false},
    {BINDING_DPAD_RIGHT, "dpad_right", "Kreuz rechts", 0, "dpad_right/click",
     false},
    {BINDING_VIEW, "view", "View", 0, "view/click", false},
    {BINDING_A, "button_a", "A", 1, "a/click", true},
    {BINDING_B, "button_b", "B", 1, "b/click", true},
    {BINDING_X, "button_x", "X", 1, "x/click", false},
    {BINDING_Y, "button_y", "Y", 1, "y/click", false},
    {BINDING_MENU, "menu", "Menue", 1, "menu/click", false},
};

struct mat4 { float v[16]; };

struct view_uniform {
    struct mat4 mvp;
    struct mat4 camera_to_world;
    float fov_tangents[4];
    int32_t modes[4];
    float source_info[4];
    float video_scale[4]; /* TP10: visible / padded size of the planes */
    float levels[4];      /* black, white, chroma range (8-bit codes) */
    /* Eye position inside the video dome (radius 1 m, dome axes): head
     * motion (6DoF), zoom, height and slide. */
    float eye_offset[4];
    /* This eye's stereo offset: longitude and latitude shift (radians) and
     * the same as texture offset for the flat screen. */
    float stereo_offset[4];
    float picture[4];     /* brightness, contrast, saturation, gamma */
};

/* Autofocus measurement (stereo-disparity.comp). */
#define DISPARITY_CANDIDATES 128
#define DISPARITY_ZERO 16          /* candidates below zero disparity */
#define DISPARITY_STEP_DEGREES 0.15f
#define DISPARITY_WINDOW_DEGREES_X 4.0f
#define DISPARITY_WINDOW_DEGREES_Y 3.0f

struct disparity_push {
    float left[4];   /* window centre (texture uv), sample step */
    float right[4];  /* window centre, shift per candidate, zero index */
    float bounds[4]; /* right eye u range, TP10 plane scale */
};

struct disparity_result {
    float score[DISPARITY_CANDIDATES];
    float left_variance;
};

_Static_assert(sizeof(struct view_uniform) <= UBO_STRIDE,
               "view uniform exceeds dynamic UBO stride");

struct video_buffer {
    int dma_buf_fd;
    bool tp10;
    VkImage image;          /* NV12, or the TP10 luma plane */
    VkDeviceMemory memory;
    VkImageView view;
    VkImage image_uv;       /* TP10 chroma plane */
    VkDeviceMemory memory_uv;
    VkImageView view_uv;
    VkSemaphore producer_semaphore;
    VkDescriptorSet descriptor_set;
    /* DMA-BUF: wait for the producer's fence. Host memory: copy the new
     * frame into the image before sampling. */
    bool producer_wait_pending;
    /* NV12_BUFFER_HOST: the memfd's mapping as Vulkan host memory, copied
     * into the device-local image(s) above for every new frame. */
    bool host;
    void *host_mapping;
    size_t host_size;
    VkBuffer host_buffer;
    VkDeviceMemory host_memory;
};

/* A UI panel drawn into the eye images (PC, see panels_in_eye_images): a
 * mipmapped image the renderer samples itself. On the Frame the panels are
 * quad layers and only width and height are used. */
struct ui_texture {
    uint32_t width, height;
    uint32_t mip_levels;
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkDescriptorSet descriptor_set;
    bool ready;
};

struct app {
    XrInstance xr;
    XrSystemId system;
    XrSession session;
    XrSpace space;
    XrSpace view_space;
    XrSessionState state;
    bool running;
    bool xr_should_render;
    bool view_geometry_logged;
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    VkCommandPool command_pool;
    VkCommandBuffer command;
    VkFence fence;
    VkRenderPass render_pass;
    VkPipelineLayout pipeline_layout;
    VkPipeline pipeline;
    VkPipeline pipeline_tp10;
    /* Autofocus: disparity measurement (NV12 and TP10 variants) and its
     * host-visible result (struct disparity_result). */
    VkPipeline disparity_pipeline, disparity_pipeline_tp10;
    VkBuffer disparity_buffer;
    VkDeviceMemory disparity_memory;
    void *disparity_mapping;
    bool disparity_pending;
    uint64_t disparity_frames;
    double autofocus_log_time;
    float autofocus_offset; /* degrees the eye pictures are moved apart */
    float autofocus_speed;  /* degrees per second of the offset */
    double autofocus_time;  /* last advance of the offset */
    /* Subject the focus is set to (distance in metres and its disparity
     * in degrees); none: the video is shown unchanged. */
    bool autofocus_focused;
    float autofocus_distance, autofocus_focus_disparity;
    bool focus_threshold;   /* only subjects nearer than FOCUS_DISTANCE */
    bool av1_software;      /* settings.json, read by the next session */
    bool autofocus_gaze;    /* last window followed the eye tracking */
    /* Last measurement, for the debug display. */
    bool autofocus_valid;
    float autofocus_disparity, autofocus_score, autofocus_variance;
    /* Debug display ("Blick anzeigen"): a frame of the measurement window
     * along the gaze (green: eye tracking, orange: head direction) and a
     * text line on the panel. */
    bool gaze_debug;
    XrSwapchain gaze_marker[2]; /* eye tracking, head direction */
    XrPosef gaze_marker_pose;
    bool gaze_marker_visible;
    double debug_text_time;
    char debug_text[192];
    VkDescriptorSetLayout descriptor_layout;
    VkDescriptorPool descriptor_pool;
    VkSamplerYcbcrConversion conversion;
    VkSampler video_sampler;
    VkSampler plane_sampler; /* TP10 planes: plain linear sampling */
    struct video_buffer video_buffers[NV12_IPC_MAX_BUFFERS];
    /* Last frame kept on screen while the session restarts after a seek. */
    struct video_buffer held;
    bool has_held;
    VkDescriptorSet spare_set;
    bool awaiting_frame;
    bool first_init_done;
    uint32_t video_buffer_count;
    int current_video_index;
    bool current_video_presented;
    int ui_fd;              /* control socket to the launcher */
    bool playing;           /* a playback session is attached (ipc_fd) */
    bool quit;
    bool xr_exit;           /* the runtime ended the session */
    bool awaiting_command;  /* a selection was sent, wait for PLAY/BROWSE */
    struct browser_ui browser;
    XrSwapchain browser_swapchain;
    uint32_t browser_image_count;
    XrSwapchainImageVulkan2KHR browser_images[MAX_IMAGES];
    bool browser_image_initialized[MAX_IMAGES];
    VkBuffer browser_staging_buffer;
    VkDeviceMemory browser_staging_memory;
    uint32_t *browser_pixels; /* mapped staging memory: write-only */
    uint32_t *browser_cpu;    /* the browser draws here (cached memory) */
    VkCommandBuffer browser_command;
    /* Pointer dot as its own small quad layer, so moving the pointer does
     * not require redrawing and uploading a panel texture. */
    XrSwapchain pointer_swapchain;
    bool pointer_layer_visible;
    XrPosef pointer_pose;
    float pointer_size;
    VkFence browser_fence;
    int ipc_fd;
    bool eos_received;
    bool title_finished;  /* EOS after the last frame, not "back" */
    char location[PLAYER_UI_PATH_MAX]; /* what the session plays */
    int end_action;       /* enum player_end_action (settings.json) */
    PFN_vkImportSemaphoreFdKHR import_semaphore_fd;
    PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties;
    PFN_vkGetImageDrmFormatModifierPropertiesEXT get_modifier_properties;
    /* VK_EXT_external_memory_host, for PC decoder buffers (optional). */
    PFN_vkGetMemoryHostPointerPropertiesEXT get_host_pointer_properties;
    VkDeviceSize dma_buf_size;
    uint32_t video_width, video_height, video_stride;
    uint32_t video_format; /* enum nv12_pixel_format */
    uint32_t buffer_kind;  /* enum nv12_buffer_kind */
    uint32_t coded_width, coded_height;
    uint32_t projection_mode, stereo_layout;
    bool swap_eyes;
    uint32_t video_levels; /* enum nv12_video_levels */
    uint32_t format_source; /* enum nv12_format_source, shown on the panel */
    uint32_t fisheye_fov;   /* lens angle (degrees) for fisheye titles */
    float volume;           /* output gain (settings.json "volume") */
    XrActionSet action_set;
    XrAction aim_action;
    XrAction select_action;
    XrAction squeeze_action;
    XrAction stick_action;
    XrAction grip_value_action;
    bool grip_value_down[2];
    float grip_value[2];
    XrAction button_actions[ARRAY_COUNT(button_inputs)];
    /* Controller bindings (settings.json "binding_*") and their input
     * state: what was down last frame, whether a held button served as a
     * modifier (then it does not act on release), and the repeating action
     * + 1 per input. */
    struct binding bindings[BINDING_ACTION_COUNT];
    int learn_action; /* action waiting for its buttons, or -1 */
    bool input_held[BINDING_INPUT_COUNT];
    bool input_combined[BINDING_INPUT_COUNT];
    int input_repeat[BINDING_INPUT_COUNT];
    double input_repeat_time[BINDING_INPUT_COUNT];
    bool grip_toggle[2]; /* grip pressed on empty space: show/hide on release */
    /* "Next/previous video": the title to start once the session ended. */
    char skip_path[PLAYER_UI_PATH_MAX];
    char skip_title[256];
    /* Eye tracking (XR_EXT_eye_gaze_interaction): the gaze ray steers the
     * autofocus measurement; without it the head's forward direction. */
    bool has_eye_gaze_extension, has_eye_gaze;
    XrAction gaze_action;
    XrSpace gaze_space;
    bool gaze_logged;
    int stick_direction[2]; /* -1 left, +1 right, 0 centred */
    bool select_down[2];
    bool has_valve_frame_controller;
    bool has_pcvr_controller;
    bool has_generic_controller;
    XrPath hands[2];
    XrSpace aim_spaces[2];
    int ui_hover;
    bool ui_pointer_visible;
    int ui_pointer_x;
    int ui_pointer_y;
    bool ui_visible;
    int autohide;            /* seconds without input, 0 = never */
    double last_input_time;  /* monotonic seconds */
    bool auto_hidden;        /* hidden by the timer, any trigger shows it */
    bool ui_dirty;
    /* Panel placement in the local reference space. The panel is world-fixed
     * and upright; only the yaw rotates it towards the viewer. */
    float panel_x, panel_y, panel_z, panel_yaw;
    int grab_hand;
    XrVector3f grab_offset;
    int scrub_hand;
    int64_t scrub_us;
    bool seek_pending;
    uint32_t player_state;
    int64_t position_us;
    int64_t duration_us;
    VkDeviceSize uv_offset;
    VkBuffer uniform_buffer;
    VkDeviceMemory uniform_memory;
    void *uniform_mapping;
    VkFormat color_format;
    XrSwapchain swapchain;
    uint32_t width, height, image_count;
    XrSwapchainImageVulkan2KHR images[MAX_IMAGES];
    VkImageView image_views[MAX_IMAGES][2];
    VkFramebuffer framebuffers[MAX_IMAGES][2];
    XrSwapchain ui_swapchain;
    uint32_t ui_image_count;
    XrSwapchainImageVulkan2KHR ui_images[MAX_IMAGES];
    bool ui_image_initialized[MAX_IMAGES];
    VkFormat ui_format;
    bool ui_bgra;
    struct ui_texture ui_texture, browser_texture;
    bool panels_in_eye_images;
    VkSampler panel_sampler;
    VkDescriptorSetLayout panel_descriptor_layout;
    VkDescriptorPool panel_descriptor_pool;
    VkPipelineLayout panel_pipeline_layout;
    VkPipeline panel_pipeline;
    float panel_anisotropy; /* 1 without samplerAnisotropy */
    VkBuffer ui_staging_buffer;
    VkDeviceMemory ui_staging_memory;
    uint32_t *ui_pixels; /* mapped staging memory: write-only */
    uint32_t *ui_cpu;    /* the panel draws here */
    struct player_ui player_ui;
    bool popup_open;
    int popup_tab;                       /* enum player_ui_tab */
    float adjust[PLAYER_ADJUST_COUNT];   /* see player_adjust_info */
    char title_key[TITLE_KEY_SIZE];      /* titles.json entry, "" if none */
    /* The dome (and the flat screen) is centred on this head position and
     * faces this gaze yaw; set at the first frame and by "zentrieren". */
    bool recenter_pending;
    bool recenter_yaw;
    XrVector3f dome_center;
    float dome_yaw;                      /* radians */
    char title[256];
    /* Seek bar preview: trickplay pictures of the playing Jellyfin item. */
    struct trickplay *trickplay;
    struct ui_image *preview;
    /* Jellyfin description of the playing title (chapters, info tab). */
    struct jf_details details;
    bool details_valid;
    bool preview_visible;
    int64_t preview_us;
    bool preview_perspective; /* straighten 180/360 degree previews */
    int preview_fov;          /* degrees, one of player_ui_preview_fovs */
    VkCommandBuffer ui_command;
    VkFence ui_fence;
    uint64_t xr_frames, rendered_frames, session_changes;
    uint64_t received_frames, released_frames;
    uint64_t unique_video_frames, repeated_video_frames;
    uint64_t hidden_video_frames, pipeline_skipped_frames;
};

static void need_vk(const char *what, VkResult result);
static void need(bool condition, const char *what);
static void hold_current_frame(struct app *app);
static void send_control(struct app *app, uint32_t type, const char *path,
                         int64_t start_us);
static void discard_held_frame(struct app *app);
static void reinitialize_video(struct app *app,
                               const struct nv12_ipc_message *message,
                               const struct msghdr *received);

static uint64_t parse_u64(const char *text, const char *name)
{
    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno || !end || *end || value == 0) {
        fprintf(stderr, "invalid %s: %s\n", name, text);
        exit(2);
    }
    return (uint64_t)value;
}

static bool has_device_extension(VkPhysicalDevice physical, const char *name)
{
    uint32_t count = 0;
    need_vk("vkEnumerateDeviceExtensionProperties(count)",
            vkEnumerateDeviceExtensionProperties(physical, NULL, &count, NULL));
    VkExtensionProperties *properties = calloc(count, sizeof(*properties));
    need(properties != NULL, "allocate extension properties");
    need_vk("vkEnumerateDeviceExtensionProperties(list)",
            vkEnumerateDeviceExtensionProperties(physical, NULL, &count, properties));
    bool found = false;
    for (uint32_t i = 0; i < count; ++i)
        if (strcmp(properties[i].extensionName, name) == 0) found = true;
    free(properties);
    return found;
}

static void die_xr(const char *what, XrResult result)
{
    fprintf(stderr, "%s: XrResult=%d\n", what, result);
    exit(2);
}

static void need_xr(const char *what, XrResult result)
{
    if (XR_FAILED(result)) die_xr(what, result);
}

static void need_vk(const char *what, VkResult result)
{
    if (result != VK_SUCCESS) {
        fprintf(stderr, "%s: VkResult=%d\n", what, result);
        exit(2);
    }
}

static void need(bool condition, const char *what)
{
    if (!condition) {
        fprintf(stderr, "%s\n", what);
        exit(2);
    }
}

static XrPath xr_path(struct app *app, const char *text)
{
    XrPath path = XR_NULL_PATH;
    need_xr("xrStringToPath", xrStringToPath(app->xr, text, &path));
    return path;
}

static void create_action(XrActionSet set, XrActionType type,
                          const char *name, const char *localized,
                          XrPath hands[2], XrAction *action)
{
    XrActionCreateInfo info = {
        .type = XR_TYPE_ACTION_CREATE_INFO,
        .actionType = type,
        .countSubactionPaths = 2,
        .subactionPaths = hands,
    };
    snprintf(info.actionName, sizeof(info.actionName), "%s", name);
    snprintf(info.localizedActionName, sizeof(info.localizedActionName),
             "%s", localized);
    need_xr("xrCreateAction", xrCreateAction(set, &info, action));
}

static void initialize_actions(struct app *app)
{
    app->hands[0] = xr_path(app, "/user/hand/left");
    app->hands[1] = xr_path(app, "/user/hand/right");
    XrActionSetCreateInfo set = {
        .type = XR_TYPE_ACTION_SET_CREATE_INFO,
    };
    snprintf(set.actionSetName, sizeof(set.actionSetName), "player");
    snprintf(set.localizedActionSetName, sizeof(set.localizedActionSetName),
             "Stereonaut Player");
    need_xr("xrCreateActionSet",
            xrCreateActionSet(app->xr, &set, &app->action_set));
    create_action(app->action_set, XR_ACTION_TYPE_POSE_INPUT,
                  "aim", "Zeigen", app->hands, &app->aim_action);
    create_action(app->action_set, XR_ACTION_TYPE_FLOAT_INPUT,
                  "select", "Auswaehlen", app->hands,
                  &app->select_action);
    create_action(app->action_set, XR_ACTION_TYPE_BOOLEAN_INPUT,
                  "squeeze", "Greifen", app->hands, &app->squeeze_action);
    create_action(app->action_set, XR_ACTION_TYPE_VECTOR2F_INPUT,
                  "stick", "Blaettern und Spulen", app->hands,
                  &app->stick_action);
    create_action(app->action_set, XR_ACTION_TYPE_FLOAT_INPUT,
                  "grip_value", "Griff (Kombinationen)", app->hands,
                  &app->grip_value_action);
    for (size_t i = 0; i < ARRAY_COUNT(button_inputs); ++i)
        create_action(app->action_set, XR_ACTION_TYPE_BOOLEAN_INPUT,
                      button_inputs[i].action, button_inputs[i].label,
                      app->hands, &app->button_actions[i]);
    struct {
        const char *profile;
        const char *select_component;
        const char *squeeze_component;
        bool thumbstick;
        bool enabled;
        int buttons; /* button_inputs: 0 none, 1 Index subset, 2 all */
        bool grip_value; /* has squeeze/value */
    } profiles[] = {
        {"/interaction_profiles/valve/frame_controller",
         "trigger/value", "squeeze/click", true,
         app->has_valve_frame_controller, 2, true},
        {"/interaction_profiles/khr/generic_controller",
         "trigger/value", "squeeze/value", true, app->has_generic_controller,
         0, true},
        {"/interaction_profiles/valve/index_controller",
         "trigger/value", "squeeze/value", true, true, 1, true},
        /* The common PCVR profiles use the same core affordances as Index:
         * trigger for pointing, squeeze for panel grab and a thumbstick for
         * seek/volume. Keep face-button suggestions to Index: their names
         * and hands differ between Touch, WMR and Vive. */
        {"/interaction_profiles/oculus/touch_controller",
         "trigger/value", "squeeze/value", true,
         app->has_pcvr_controller, 0, true},
        {"/interaction_profiles/microsoft/motion_controller",
         "trigger/value", "squeeze/value", true,
         app->has_pcvr_controller, 0, true},
        {"/interaction_profiles/htc/vive_cosmos_controller",
         "trigger/value", "squeeze/click", true,
         app->has_pcvr_controller, 0, false},
        /* Original Vive wands expose a trackpad rather than a thumbstick;
         * it is still a Vector2f input and maps naturally to navigation. */
        {"/interaction_profiles/htc/vive_controller",
         "trigger/value", "squeeze/click", true,
         app->has_pcvr_controller, 0, false},
        /* No squeeze button or stick: panel grab, show/hide, page flips and
         * stick seeking are unavailable. */
        {"/interaction_profiles/khr/simple_controller",
         "select/click", NULL, false, true, 0, false},
    };
    static const char *const hand_names[2] = {"left", "right"};
    unsigned accepted = 0;
    for (size_t p = 0; p < ARRAY_COUNT(profiles); ++p) {
        if (!profiles[p].enabled)
            continue;
        XrActionSuggestedBinding bindings[10 + ARRAY_COUNT(button_inputs)];
        uint32_t count = 0;
        char text[4][2][96];
        for (unsigned hand = 0; hand < 2; ++hand) {
            snprintf(text[0][hand], sizeof(text[0][hand]),
                     "/user/hand/%s/input/aim/pose", hand_names[hand]);
            bindings[count++] = (XrActionSuggestedBinding){
                app->aim_action, xr_path(app, text[0][hand])};
            snprintf(text[1][hand], sizeof(text[1][hand]),
                     "/user/hand/%s/input/%s", hand_names[hand],
                     profiles[p].select_component);
            bindings[count++] = (XrActionSuggestedBinding){
                app->select_action, xr_path(app, text[1][hand])};
            if (profiles[p].squeeze_component) {
                snprintf(text[2][hand], sizeof(text[2][hand]),
                         "/user/hand/%s/input/%s", hand_names[hand],
                         profiles[p].squeeze_component);
                bindings[count++] = (XrActionSuggestedBinding){
                    app->squeeze_action, xr_path(app, text[2][hand])};
            }
            if (profiles[p].thumbstick) {
                const char *stick = strstr(profiles[p].profile,
                                           "vive_controller")
                                        ? "trackpad" : "thumbstick";
                snprintf(text[3][hand], sizeof(text[3][hand]),
                         "/user/hand/%s/input/%s", hand_names[hand], stick);
                bindings[count++] = (XrActionSuggestedBinding){
                    app->stick_action, xr_path(app, text[3][hand])};
            }
            if (profiles[p].grip_value) {
                char path[96];
                snprintf(path, sizeof(path),
                         "/user/hand/%s/input/squeeze/value",
                         hand_names[hand]);
                bindings[count++] = (XrActionSuggestedBinding){
                    app->grip_value_action, xr_path(app, path)};
            }
        }
        for (size_t i = 0; i < ARRAY_COUNT(button_inputs); ++i) {
            if (profiles[p].buttons == 0 ||
                (profiles[p].buttons == 1 && !button_inputs[i].index))
                continue;
            char path[96];
            snprintf(path, sizeof(path), "/user/hand/%s/input/%s",
                     hand_names[button_inputs[i].hand],
                     button_inputs[i].component);
            bindings[count++] = (XrActionSuggestedBinding){
                app->button_actions[i], xr_path(app, path)};
        }
        XrInteractionProfileSuggestedBinding suggestion = {
            .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
            .interactionProfile = xr_path(app, profiles[p].profile),
            .countSuggestedBindings = count,
            .suggestedBindings = bindings,
        };
        XrResult result =
            xrSuggestInteractionProfileBindings(app->xr, &suggestion);
        printf("controller_bindings profile=%s result=%d\n",
               profiles[p].profile, result);
        if (XR_SUCCEEDED(result))
            ++accepted;
    }
    need(accepted > 0, "no controller interaction profile was accepted");
    for (unsigned hand = 0; hand < 2; ++hand) {
        XrActionSpaceCreateInfo space = {
            .type = XR_TYPE_ACTION_SPACE_CREATE_INFO,
            .action = app->aim_action,
            .subactionPath = app->hands[hand],
            .poseInActionSpace = {{0, 0, 0, 1}, {0, 0, 0}},
        };
        need_xr("xrCreateActionSpace",
                xrCreateActionSpace(app->session, &space,
                                    &app->aim_spaces[hand]));
    }
    if (app->has_eye_gaze) {
        XrActionCreateInfo gaze = {
            .type = XR_TYPE_ACTION_CREATE_INFO,
            .actionType = XR_ACTION_TYPE_POSE_INPUT,
        };
        snprintf(gaze.actionName, sizeof(gaze.actionName), "gaze");
        snprintf(gaze.localizedActionName,
                 sizeof(gaze.localizedActionName), "Blickrichtung");
        need_xr("xrCreateAction(gaze)",
                xrCreateAction(app->action_set, &gaze, &app->gaze_action));
        XrActionSuggestedBinding binding = {
            app->gaze_action,
            xr_path(app, "/user/eyes_ext/input/gaze_ext/pose"),
        };
        XrInteractionProfileSuggestedBinding suggestion = {
            .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
            .interactionProfile =
                xr_path(app, "/interaction_profiles/ext/eye_gaze_interaction"),
            .countSuggestedBindings = 1,
            .suggestedBindings = &binding,
        };
        XrResult result =
            xrSuggestInteractionProfileBindings(app->xr, &suggestion);
        printf("eye_gaze_binding result=%d\n", result);
        XrActionSpaceCreateInfo space = {
            .type = XR_TYPE_ACTION_SPACE_CREATE_INFO,
            .action = app->gaze_action,
            .poseInActionSpace = {{0, 0, 0, 1}, {0, 0, 0}},
        };
        if (XR_FAILED(result) ||
            XR_FAILED(xrCreateActionSpace(app->session, &space,
                                          &app->gaze_space)))
            app->has_eye_gaze = false;
    }
    XrSessionActionSetsAttachInfo attach = {
        .type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO,
        .countActionSets = 1,
        .actionSets = &app->action_set,
    };
    need_xr("xrAttachSessionActionSets",
            xrAttachSessionActionSets(app->session, &attach));
}

static void apply_init(struct app *app, const struct nv12_ipc_message *message,
                       const struct msghdr *received)
{
    /* Iris chooses the count per stream (H.264 needs more references at
     * small sizes), so it may differ between titles and seeks. */
    need(message->buffer_count >= 1 &&
             message->buffer_count <= NV12_IPC_MAX_BUFFERS,
         "NV12 IPC buffer count out of range");
    struct cmsghdr *header = CMSG_FIRSTHDR(received);
    need(header && header->cmsg_level == SOL_SOCKET &&
             header->cmsg_type == SCM_RIGHTS,
         "NV12 IPC init lacks DMA-BUF file descriptors");
    size_t fd_bytes = header->cmsg_len - CMSG_LEN(0);
    need(fd_bytes == message->buffer_count * sizeof(int),
         "NV12 IPC descriptor count differs from buffer count");
    int *fds = (int *)CMSG_DATA(header);
    app->video_buffer_count = message->buffer_count;
    for (uint32_t i = 0; i < app->video_buffer_count; ++i) {
        app->video_buffers[i].dma_buf_fd = fds[i];
        struct stat status;
        if (fstat(fds[i], &status) < 0) {
            perror("fstat IPC DMA-BUF");
            exit(2);
        }
        need((uint64_t)status.st_size == message->size,
             "IPC DMA-BUF length differs from contract");
    }
    app->video_width = message->width;
    app->video_height = message->height;
    app->video_stride = message->stride;
    app->dma_buf_size = message->size;
    app->uv_offset = message->uv_offset;
    app->video_format = message->pixel_format;
    app->buffer_kind = message->buffer_kind;
    need(app->buffer_kind == NV12_BUFFER_DMA_BUF ||
             (app->buffer_kind == NV12_BUFFER_HOST &&
              app->get_host_pointer_properties),
         "video buffers in host memory need VK_EXT_external_memory_host");
    app->coded_width = message->coded_width;
    app->coded_height = message->coded_height;
    need(app->video_format == NV12_FORMAT_NV12 ||
             (app->video_format == NV12_FORMAT_P010 &&
              app->coded_width == app->video_width &&
              app->coded_height == app->video_height &&
              app->video_stride >= 2U * app->video_width) ||
             (app->video_format == NV12_FORMAT_TP10 &&
              app->coded_width >= app->video_width &&
              app->coded_height >= app->video_height &&
              app->coded_width % 3U == 0 &&
              app->video_stride == app->coded_width / 3U * 4U),
         "unsupported video pixel format in IPC contract");
    app->projection_mode = message->projection_mode;
    app->stereo_layout = message->stereo_layout;
    app->swap_eyes = message->swap_eyes != 0;
    app->video_levels = message->video_levels;
    app->format_source = message->format_source;
    app->fisheye_fov = message->fisheye_fov >= 120 &&
                               message->fisheye_fov <= 240
                           ? message->fisheye_fov
                           : NV12_FISHEYE_FOV_DEFAULT;
    need(app->video_width > 0 && app->video_height > 0 &&
             !(app->video_width & 1U) && !(app->video_height & 1U) &&
             (app->video_format == NV12_FORMAT_TP10 ||
              app->video_stride >= app->video_width) &&
             app->uv_offset >=
                 (uint64_t)app->video_stride * app->video_height &&
             app->dma_buf_size >= app->uv_offset +
                 (uint64_t)app->video_stride * app->video_height / 2U,
         "NV12 IPC layout is not linear 4:2:0");
    need(app->projection_mode <= NV12_PROJECTION_FISHEYE,
         "invalid projection mode in IPC contract");
    need(app->stereo_layout <= NV12_STEREO_TB,
         "invalid stereo layout in IPC contract");
    need(app->video_levels <= NV12_LEVELS_FULL,
         "invalid video levels in IPC contract");
    need(app->format_source <= NV12_FORMAT_SOURCE_USER,
         "invalid format source in IPC contract");
    /* The host owns the panel placement for its whole lifetime. */
    printf("render_config projection=%u stereo=%u swap_eyes=%u levels=%u"
           " view0=left view1=right\n",
           app->projection_mode, app->stereo_layout, app->swap_eyes,
           app->video_levels);
}

static void send_message(struct app *app, const struct nv12_ipc_message *message)
{
    ssize_t written;
    do {
        written = send(app->ipc_fd, message, sizeof(*message), MSG_NOSIGNAL);
    } while (written < 0 && errno == EINTR);
    if (written < 0 && (errno == EPIPE || errno == ECONNRESET ||
                        errno == ENOTCONN)) {
        app->eos_received = true; /* the session is gone: end playback */
        return;
    }
    if (written < 0) { perror("send NV12 IPC response"); exit(2); }
    need((size_t)written == sizeof(*message), "short NV12 IPC response");
}

static void send_ipc(struct app *app, uint32_t type, uint32_t index)
{
    struct nv12_ipc_message message = {.type = type, .index = index};
    send_message(app, &message);
}

static void send_volume(struct app *app)
{
    struct nv12_ipc_message message = {
        .type = NV12_IPC_VOLUME, .volume = app->volume,
    };
    send_message(app, &message);
}

/* Steps of 10 %; above 100 % the samples are amplified (may clip). */
static void change_volume(struct app *app, int direction)
{
    float volume = roundf((app->volume + 0.1f * (float)direction) * 10.0f) /
                   10.0f;
    app->volume = volume < 0.0f ? 0.0f : volume > 1.5f ? 1.5f : volume;
    settings_set_double("volume", app->volume);
    send_volume(app);
    app->ui_dirty = true;
}

static void send_seek_to(struct app *app, int64_t target_us)
{
    struct nv12_ipc_message message = {
        .type = NV12_IPC_ACTION, .index = NV12_ACTION_SEEK_TO,
        .position_us = target_us,
    };
    send_message(app, &message);
}

/* Applies a choice from the view popup at once and reports it to the
 * session, which stores it for this title. */
static void change_view(struct app *app, int control)
{
    if (control >= PLAYER_UI_PROJECTION &&
        control <= PLAYER_UI_PROJECTION + NV12_PROJECTION_FISHEYE) {
        uint32_t projection = (uint32_t)(control - PLAYER_UI_PROJECTION);
        /* Choosing fisheye again steps through the usual lens angles. */
        static const uint32_t lenses[] = {180, 190, 200, 220};
        if (projection == NV12_PROJECTION_FISHEYE &&
            app->projection_mode == NV12_PROJECTION_FISHEYE) {
            size_t next = 0;
            for (size_t i = 0; i < sizeof(lenses) / sizeof(lenses[0]); ++i)
                if (lenses[i] == app->fisheye_fov)
                    next = (i + 1) % (sizeof(lenses) / sizeof(lenses[0]));
            app->fisheye_fov = lenses[next];
        }
        app->projection_mode = projection;
        app->format_source = NV12_FORMAT_SOURCE_USER;
    } else if (control >= PLAYER_UI_STEREO &&
               control <= PLAYER_UI_STEREO + NV12_STEREO_TB) {
        app->stereo_layout = (uint32_t)(control - PLAYER_UI_STEREO);
        app->format_source = NV12_FORMAT_SOURCE_USER;
    } else if (control == PLAYER_UI_SWAP_EYES)
        app->swap_eyes = !app->swap_eyes;
    else if (control >= PLAYER_UI_LEVELS &&
             control <= PLAYER_UI_LEVELS + NV12_LEVELS_FULL)
        app->video_levels = (uint32_t)(control - PLAYER_UI_LEVELS);
    else
        return;
    app->ui_dirty = true;
    struct nv12_ipc_message message = {
        .type = NV12_IPC_VIEW,
        .projection_mode = app->projection_mode,
        .fisheye_fov = app->fisheye_fov,
        .stereo_layout = app->stereo_layout,
        .swap_eyes = app->swap_eyes,
        .video_levels = app->video_levels,
    };
    send_message(app, &message);
    printf("player_view projection=%u stereo=%u swap_eyes=%u levels=%u\n",
           app->projection_mode, app->stereo_layout, app->swap_eyes,
           app->video_levels);
}

/* ---- picture position and adjustments ------------------------------- */

/* Per-title values go to the title's entry in titles.json, tilt and head
 * motion to settings.json. */
static void save_adjust(const struct app *app, int index)
{
    const struct player_adjust_info *info = &player_adjust_info[index];
    if (info->global) {
        settings_set_double(info->key, app->adjust[index]);
        return;
    }
    if (!app->title_key[0])
        return;
    const char *names[PLAYER_ADJUST_COUNT];
    for (int i = 0; i < PLAYER_ADJUST_COUNT; ++i)
        names[i] = player_adjust_info[i].global ? NULL
                                                : player_adjust_info[i].key;
    if (title_settings_save_numbers(app->title_key, "adjust", names,
                                    app->adjust, PLAYER_ADJUST_COUNT) < 0)
        fprintf(stderr, "picture adjustments could not be saved\n");
}

static float clamp_adjust(int index, float value)
{
    const struct player_adjust_info *info = &player_adjust_info[index];
    return value < info->minimum ? info->minimum
           : value > info->maximum ? info->maximum : value;
}

/* Global values at startup, per-title values when a title starts. */
static void load_adjust(struct app *app, bool global)
{
    const char *names[PLAYER_ADJUST_COUNT];
    for (int i = 0; i < PLAYER_ADJUST_COUNT; ++i) {
        const struct player_adjust_info *info = &player_adjust_info[i];
        names[i] = info->global ? NULL : info->key;
        if (info->global == global)
            app->adjust[i] = info->standard;
        if (global && info->global)
            app->adjust[i] = clamp_adjust(
                i, (float)settings_get_double(info->key, info->standard));
    }
    if (!global && app->title_key[0]) {
        float values[PLAYER_ADJUST_COUNT];
        memcpy(values, app->adjust, sizeof(values));
        if (title_settings_load_numbers(app->title_key, "adjust", names,
                                        values, PLAYER_ADJUST_COUNT) > 0)
            for (int i = 0; i < PLAYER_ADJUST_COUNT; ++i)
                if (!player_adjust_info[i].global)
                    app->adjust[i] = clamp_adjust(i, values[i]);
    }
}

static void change_adjust(struct app *app, int control)
{
    int index = (control - PLAYER_UI_ADJUST) / 2;
    int direction = (control - PLAYER_UI_ADJUST) % 2 ? 1 : -1;
    app->adjust[index] = player_adjust_step((enum player_adjust)index,
                                            app->adjust[index], direction);
    save_adjust(app, index);
    app->ui_dirty = true;
    printf("player_adjust %s=%.3f\n", player_adjust_info[index].key,
           app->adjust[index]);
}

/* "Zurücksetzen" of the position or the picture tab. */
static void reset_adjust(struct app *app, bool picture)
{
    for (int i = 0; i < PLAYER_ADJUST_COUNT; ++i) {
        bool is_picture = i >= PLAYER_ADJUST_BRIGHTNESS;
        if (is_picture != picture || i == PLAYER_ADJUST_HEAD_MOTION ||
            (i >= PLAYER_ADJUST_AUTOFOCUS && i <= PLAYER_ADJUST_FOCUS_FADE))
            continue;
        app->adjust[i] = player_adjust_info[i].standard;
        save_adjust(app, i);
    }
    app->ui_dirty = true;
    printf("player_adjust_reset picture=%d\n", picture);
}

/* Scrub preview settings from the view popup; global, kept in
 * settings.json. */
static void change_preview(struct app *app, int control)
{
    if (control == PLAYER_UI_PREVIEW_MODE ||
        control == PLAYER_UI_PREVIEW_MODE + 1) {
        app->preview_perspective = control == PLAYER_UI_PREVIEW_MODE + 1;
        settings_set_bool("trickplay_perspective", app->preview_perspective);
    } else if (control >= PLAYER_UI_PREVIEW_FOV &&
               control < PLAYER_UI_PREVIEW_FOV + PLAYER_UI_PREVIEW_FOV_COUNT) {
        app->preview_fov =
            player_ui_preview_fovs[control - PLAYER_UI_PREVIEW_FOV];
        app->preview_perspective = true;
        settings_set_bool("trickplay_perspective", true);
        settings_set_int("trickplay_fov", app->preview_fov);
    } else {
        return;
    }
    app->ui_dirty = true;
    printf("player_preview perspective=%d fov=%d\n",
           app->preview_perspective, app->preview_fov);
}

static void load_preview_settings(struct app *app)
{
    app->preview_perspective = settings_get_bool("trickplay_perspective",
                                                 true);
    int fov = settings_get_int("trickplay_fov", 100);
    app->preview_fov = 100;
    for (int i = 0; i < PLAYER_UI_PREVIEW_FOV_COUNT; ++i)
        if (player_ui_preview_fovs[i] == fov)
            app->preview_fov = fov;
    int autohide = settings_get_int("panel_autohide", 5);
    app->autohide = 0;
    for (int i = 0; i < PLAYER_UI_AUTOHIDE_COUNT; ++i)
        if (player_ui_autohide[i] == autohide)
            app->autohide = autohide;
    int end_action = settings_get_int("end_action", PLAYER_END_STOP);
    app->end_action = end_action >= 0 && end_action < PLAYER_END_COUNT
                          ? end_action
                          : PLAYER_END_STOP;
    double volume = settings_get_double("volume", 1.0);
    app->volume = volume < 0.0 ? 0.0f : volume > 1.5 ? 1.5f : (float)volume;
    app->focus_threshold = settings_get_bool("focus_threshold", false);
    app->av1_software = settings_get_bool("av1_software", false);
    for (int i = 0; i < BINDING_ACTION_COUNT; ++i) {
        char key[64];
        snprintf(key, sizeof(key), "binding_%s", binding_action_info[i].key);
        app->bindings[i] = binding_decode(settings_get_int(
            key, binding_encode(binding_action_info[i].standard)));
    }
}

static void save_binding(const struct app *app, int action)
{
    char key[64];
    snprintf(key, sizeof(key), "binding_%s", binding_action_info[action].key);
    settings_set_int(key, binding_encode(app->bindings[action]));
    char text[64];
    binding_describe(app->bindings[action], text, sizeof(text));
    printf("binding %s=%s\n", binding_action_info[action].key, text);
}

static void send_panel_state(struct app *app)
{
    struct nv12_ipc_message message = {
        .type = NV12_IPC_PANEL,
        .panel_x = app->panel_x, .panel_y = app->panel_y,
        .panel_z = app->panel_z, .panel_yaw = app->panel_yaw,
        .panel_visible = app->ui_visible,
        .panel_valid = 1,
    };
    send_message(app, &message);
}

static void release_current_video(struct app *app)
{
    if (app->current_video_index < 0)
        return;
    if (!app->current_video_presented) {
        if (app->xr_should_render)
            app->pipeline_skipped_frames++;
        else
            app->hidden_video_frames++;
    }
    send_ipc(app, NV12_IPC_RELEASE, (uint32_t)app->current_video_index);
    app->released_frames++;
    app->current_video_index = -1;
}

static void receive_frame_messages(struct app *app)
{
    for (;;) {
        struct nv12_ipc_message message = {0};
        char control[CMSG_SPACE(sizeof(int) * NV12_IPC_MAX_BUFFERS)] = {0};
        struct iovec iov = {.iov_base = &message, .iov_len = sizeof(message)};
        struct msghdr received = {
            .msg_iov = &iov, .msg_iovlen = 1,
            .msg_control = control, .msg_controllen = sizeof(control),
        };
        ssize_t bytes = recvmsg(app->ipc_fd, &received, MSG_DONTWAIT);
        if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        if (bytes < 0 && errno == EINTR)
            continue;
        if (bytes == 0) {
            app->eos_received = true;
            return;
        }
        if (bytes < 0 && errno == ECONNRESET) {
            app->eos_received = true;
            return;
        }
        if (bytes < 0) { perror("recv NV12 IPC message"); exit(2); }
        need((size_t)bytes == sizeof(message), "short NV12 IPC message");
        if (message.type == NV12_IPC_FRAME) {
            need(message.index < app->video_buffer_count,
                 "NV12 IPC frame index is invalid");
            release_current_video(app);
            app->current_video_index = (int)message.index;
            app->current_video_presented = false;
            app->video_buffers[message.index].producer_wait_pending = true;
            if (app->awaiting_frame) {
                app->awaiting_frame = false;
                app->seek_pending = false;
                app->ui_dirty = true;
            }
            /* The held frame is superseded by the new stream. */
            discard_held_frame(app);
            app->received_frames++;
        } else if (message.type == NV12_IPC_FLUSH) {
            release_current_video(app);
            send_ipc(app, NV12_IPC_FLUSHED, 0);
        } else if (message.type == NV12_IPC_HOLD) {
            hold_current_frame(app);
            send_ipc(app, NV12_IPC_HELD, 0);
        } else if (message.type == NV12_IPC_SEEKING) {
            app->awaiting_frame = true;
            app->ui_dirty = true;
            printf("video_seeking\n");
        } else if (message.type == NV12_IPC_INIT) {
            reinitialize_video(app, &message, &received);
        } else if (message.type == NV12_IPC_FORMAT) {
            need(message.projection_mode <= NV12_PROJECTION_FISHEYE &&
                     message.stereo_layout <= NV12_STEREO_TB &&
                     message.format_source <= NV12_FORMAT_SOURCE_USER,
                 "invalid detected format");
            app->projection_mode = message.projection_mode;
            app->stereo_layout = message.stereo_layout;
            app->format_source = message.format_source;
            if (message.fisheye_fov >= 120 && message.fisheye_fov <= 240)
                app->fisheye_fov = message.fisheye_fov;
            app->ui_dirty = true;
            printf("player_format projection=%u stereo=%u source=%u\n",
                   app->projection_mode, app->stereo_layout,
                   app->format_source);
        } else if (message.type == NV12_IPC_EOS) {
            app->eos_received = true;
            app->title_finished = message.index == 1;
        } else if (message.type == NV12_IPC_STATUS) {
            /* Redraw only for a visible change: the shown second, the
             * seek bar knob (whole pixels) or the play state. */
            int64_t old_second = app->position_us / 1000000;
            int old_knob = app->duration_us > 0
                ? (int)(1000.0 * app->position_us / app->duration_us) : -1;
            bool changed = app->player_state != message.player_state ||
                           app->duration_us != message.duration_us;
            app->player_state = message.player_state;
            if (!app->seek_pending)
                app->position_us = message.position_us;
            app->duration_us = message.duration_us;
            int knob = app->duration_us > 0
                ? (int)(1000.0 * app->position_us / app->duration_us) : -1;
            if (changed || old_second != app->position_us / 1000000 ||
                old_knob != knob)
                app->ui_dirty = true;
        } else {
            need(false, "unknown NV12 IPC message");
        }
        /* The socket is the bounded FIFO. Consume at most one decoded frame
         * per XR iteration so a decoder burst cannot discard valid frames. */
        return;
    }
}

static uint32_t memory_type(VkPhysicalDevice physical, uint32_t bits,
                            VkMemoryPropertyFlags wanted)
{
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
        if ((bits & (1U << i)) &&
            (properties.memoryTypes[i].propertyFlags & wanted) == wanted)
            return i;
    fprintf(stderr, "no compatible Vulkan memory type\n");
    exit(2);
}

static struct mat4 multiply(struct mat4 a, struct mat4 b)
{
    struct mat4 result = {{0}};
    for (unsigned col = 0; col < 4; ++col)
        for (unsigned row = 0; row < 4; ++row)
            for (unsigned k = 0; k < 4; ++k)
                result.v[col * 4 + row] += a.v[k * 4 + row] * b.v[col * 4 + k];
    return result;
}

static struct mat4 view_from_pose(XrPosef pose)
{
    float x = pose.orientation.x, y = pose.orientation.y;
    float z = pose.orientation.z, w = pose.orientation.w;
    /* Invert the rigid OpenXR view pose.  The rotational part of the view
     * matrix is the transpose of the pose's quaternion rotation. */
    struct mat4 result = {{
        1 - 2*y*y - 2*z*z, 2*x*y - 2*z*w,     2*x*z + 2*y*w,     0,
        2*x*y + 2*z*w,     1 - 2*x*x - 2*z*z, 2*y*z - 2*x*w,     0,
        2*x*z - 2*y*w,     2*y*z + 2*x*w,     1 - 2*x*x - 2*y*y, 0,
        0,                 0,                 0,                 1,
    }};
    result.v[12] = -(result.v[0] * pose.position.x + result.v[4] * pose.position.y + result.v[8] * pose.position.z);
    result.v[13] = -(result.v[1] * pose.position.x + result.v[5] * pose.position.y + result.v[9] * pose.position.z);
    result.v[14] = -(result.v[2] * pose.position.x + result.v[6] * pose.position.y + result.v[10] * pose.position.z);
    return result;
}

static struct mat4 rotation_from_pose(XrPosef pose)
{
    float x = pose.orientation.x, y = pose.orientation.y;
    float z = pose.orientation.z, w = pose.orientation.w;
    /* Camera-to-LOCAL rotation. Position is deliberately excluded so
     * equirectangular video cannot acquire geometry parallax from head or
     * physical eye translation. */
    return (struct mat4){{
        1 - 2*y*y - 2*z*z, 2*x*y + 2*z*w,     2*x*z - 2*y*w,     0,
        2*x*y - 2*z*w,     1 - 2*x*x - 2*z*z, 2*y*z + 2*x*w,     0,
        2*x*z + 2*y*w,     2*y*z - 2*x*w,     1 - 2*x*x - 2*y*y, 0,
        0,                 0,                 0,                 1,
    }};
}

/* Rotation about the x (0), y (1) or z (2) axis, right-handed. */
static struct mat4 rotation_axis(int axis, float angle)
{
    float c = cosf(angle), s = sinf(angle);
    struct mat4 m = {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
    int a = (axis + 1) % 3, b = (axis + 2) % 3;
    m.v[a * 4 + a] = c;
    m.v[b * 4 + b] = c;
    m.v[a * 4 + b] = s;  /* column a, row b */
    m.v[b * 4 + a] = -s; /* column b, row a */
    return m;
}

static XrVector3f transform_direction(struct mat4 m, XrVector3f v)
{
    return (XrVector3f){
        m.v[0] * v.x + m.v[4] * v.y + m.v[8] * v.z,
        m.v[1] * v.x + m.v[5] * v.y + m.v[9] * v.z,
        m.v[2] * v.x + m.v[6] * v.y + m.v[10] * v.z,
    };
}

static struct mat4 projection_from_fov(XrFovf fov)
{
    const float near_z = 0.05f, far_z = 100.0f;
    float left = tanf(fov.angleLeft), right = tanf(fov.angleRight);
    float down = tanf(fov.angleDown), up = tanf(fov.angleUp);
    struct mat4 result = {{0}};
    result.v[0] = 2.0f / (right - left);
    /* Vulkan's framebuffer Y direction is opposite to OpenXR's view-space
     * Y direction when a positive viewport height is used. */
    result.v[5] = 2.0f / (down - up);
    result.v[8] = (right + left) / (right - left);
    result.v[9] = (up + down) / (down - up);
    result.v[10] = far_z / (near_z - far_z);
    result.v[11] = -1.0f;
    result.v[14] = -(far_z * near_z) / (far_z - near_z);
    return result;
}

/* Started from the desktop overlay inside the SteamVR dashboard, the app
 * becomes visible but keeps the dashboard in front (session state VISIBLE
 * instead of FOCUSED) until the viewer clicks "back to game". Hide it with
 * SteamVR's own tool, only during startup so a dashboard the viewer opens
 * later stays alone. */
static void hide_dashboard_at_startup(const struct app *app)
{
    enum { WINDOW_S = 20, MAX_ATTEMPTS = 4 };
    static time_t first_call_s;
    static time_t last_attempt_s;
    static int attempts;
    time_t now = time(NULL);
    if (!first_call_s)
        first_call_s = now;
    /* This workaround is specific to the Steam Frame dashboard launch.
     * Desktop SteamVR decides dashboard focus itself; hiding it there makes
     * a direct PCVR launch unexpectedly lose its system UI. */
    if (!app->has_valve_frame_controller ||
        app->state != XR_SESSION_STATE_VISIBLE ||
        now - first_call_s > WINDOW_S || attempts >= MAX_ATTEMPTS ||
        (attempts && now - last_attempt_s < 1))
        return;
    ++attempts;
    last_attempt_s = now;
    const char *tool = getenv("STEREONAUT_VRCMD");
    if (!tool || !tool[0])
        tool = "/opt/steamvr/bin/linuxarm64/vrcmd";
    if (access(tool, X_OK) < 0)
        return;
    signal(SIGCHLD, SIG_IGN);
    pid_t child = fork();
    if (child == 0) {
        int null = open("/dev/null", O_WRONLY);
        if (null >= 0) {
            dup2(null, STDOUT_FILENO);
            dup2(null, STDERR_FILENO);
        }
        execl(tool, "vrcmd", "--hidedashboard", (char *)NULL);
        _exit(127);
    }
    printf("hide_dashboard attempt=%d pid=%ld\n", attempts, (long)child);
}

static void poll_events(struct app *app)
{
    for (;;) {
        XrEventDataBuffer event = {.type = XR_TYPE_EVENT_DATA_BUFFER};
        XrResult result = xrPollEvent(app->xr, &event);
        if (result == XR_EVENT_UNAVAILABLE) return;
        need_xr("xrPollEvent", result);
        if (event.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
            static const char *const hand_names[2] = {"left", "right"};
            for (unsigned hand = 0; hand < 2; ++hand) {
                XrInteractionProfileState profile = {
                    .type = XR_TYPE_INTERACTION_PROFILE_STATE,
                };
                char text[XR_MAX_PATH_LENGTH] = "none";
                if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(
                        app->session, app->hands[hand], &profile)) &&
                    profile.interactionProfile != XR_NULL_PATH) {
                    uint32_t length = 0;
                    xrPathToString(app->xr, profile.interactionProfile,
                                   sizeof(text), &length, text);
                }
                printf("interaction_profile hand=%s profile=%s\n",
                       hand_names[hand], text);
            }
            continue;
        }
        if (event.type != XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) continue;
        XrEventDataSessionStateChanged *change =
            (XrEventDataSessionStateChanged *)&event;
        app->state = change->state;
        ++app->session_changes;
        printf("openxr_session_state=%d time=%" PRId64 "\n", app->state,
               change->time);
        if (app->state == XR_SESSION_STATE_READY) {
            XrSessionBeginInfo begin = {
                .type = XR_TYPE_SESSION_BEGIN_INFO,
                .primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            };
            need_xr("xrBeginSession", xrBeginSession(app->session, &begin));
            app->running = true;
        } else if (app->state == XR_SESSION_STATE_STOPPING) {
            need_xr("xrEndSession", xrEndSession(app->session));
            app->running = false;
        } else if (app->state == XR_SESSION_STATE_EXITING ||
                   app->state == XR_SESSION_STATE_LOSS_PENDING) {
            app->running = false;
            app->quit = true;
            app->xr_exit = true;
            return;
        }
    }
}

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    if (!file) { perror(path); exit(2); }
    if (fseek(file, 0, SEEK_END) || (*size = (size_t)ftell(file)) == 0 ||
        fseek(file, 0, SEEK_SET)) { perror("read shader size"); exit(2); }
    uint8_t *data = malloc(*size);
    if (!data || fread(data, 1, *size, file) != *size) {
        perror("read shader"); exit(2);
    }
    fclose(file);
    return data;
}

static VkShaderModule shader(VkDevice device, const char *path)
{
    size_t size = 0;
    uint8_t *data = read_file(path, &size);
    need(size % 4 == 0, "SPIR-V size is not word aligned");
    VkShaderModuleCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = size, .pCode = (const uint32_t *)data,
    };
    VkShaderModule result = VK_NULL_HANDLE;
    need_vk("vkCreateShaderModule", vkCreateShaderModule(device, &info, NULL, &result));
    free(data);
    return result;
}

static void initialize_xr_vulkan(struct app *app)
{
    /* Controller profiles with a squeeze/grip button are optional extensions;
     * khr/simple_controller has none. */
    const char *valve_extension = "XR_VALVE_frame_controller_interaction";
    const char *generic_extension = "XR_KHR_generic_controller";
    uint32_t available_count = 0;
    need_xr("xrEnumerateInstanceExtensionProperties(count)",
            xrEnumerateInstanceExtensionProperties(NULL, 0, &available_count,
                                                   NULL));
    XrExtensionProperties *available =
        calloc(available_count ? available_count : 1U, sizeof(*available));
    need(available != NULL, "allocate OpenXR extension list");
    for (uint32_t i = 0; i < available_count; ++i)
        available[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    need_xr("xrEnumerateInstanceExtensionProperties(list)",
            xrEnumerateInstanceExtensionProperties(NULL, available_count,
                                                   &available_count,
                                                   available));
    for (uint32_t i = 0; i < available_count; ++i) {
        if (strcmp(available[i].extensionName,
                   XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME) == 0)
            app->has_eye_gaze_extension = true;
        if (strcmp(available[i].extensionName, valve_extension) == 0)
            app->has_valve_frame_controller = true;
        if (strcmp(available[i].extensionName, generic_extension) == 0)
            app->has_generic_controller = true;
    }
    free(available);
    /* No extension is required for the standardized PC interaction
     * profiles. Unsupported profiles are harmlessly rejected by
     * xrSuggestInteractionProfileBindings. */
    app->has_pcvr_controller = true;
    const char *extensions[4] = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
    uint32_t extension_count = 1;
    if (app->has_eye_gaze_extension)
        extensions[extension_count++] =
            XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME;
    if (app->has_valve_frame_controller)
        extensions[extension_count++] = valve_extension;
    if (app->has_generic_controller)
        extensions[extension_count++] = generic_extension;
    XrInstanceCreateInfo info = {
        .type = XR_TYPE_INSTANCE_CREATE_INFO,
        .applicationInfo = {.applicationVersion = 1, .engineVersion = 1,
                            .apiVersion = XR_MAKE_VERSION(1, 0, 0)},
        .enabledExtensionCount = extension_count,
        .enabledExtensionNames = extensions,
    };
    snprintf(info.applicationInfo.applicationName,
             sizeof(info.applicationInfo.applicationName), "stereonaut-xr-quad");
    snprintf(info.applicationInfo.engineName, sizeof(info.applicationInfo.engineName), "stereonaut");
    need_xr("xrCreateInstance", xrCreateInstance(&info, &app->xr));
    XrSystemGetInfo system_info = {.type = XR_TYPE_SYSTEM_GET_INFO,
                                   .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
    need_xr("xrGetSystem", xrGetSystem(app->xr, &system_info, &app->system));
    if (app->has_eye_gaze_extension) {
        XrSystemEyeGazeInteractionPropertiesEXT eye_gaze = {
            .type = XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT,
        };
        XrSystemProperties properties = {
            .type = XR_TYPE_SYSTEM_PROPERTIES, .next = &eye_gaze,
        };
        if (XR_SUCCEEDED(xrGetSystemProperties(app->xr, app->system,
                                               &properties)))
            app->has_eye_gaze = eye_gaze.supportsEyeGazeInteraction;
    }
    printf("eye_gaze extension=%d supported=%d\n",
           app->has_eye_gaze_extension, app->has_eye_gaze);

    PFN_xrCreateVulkanInstanceKHR create_instance = NULL;
    PFN_xrGetVulkanGraphicsDevice2KHR get_device = NULL;
    PFN_xrCreateVulkanDeviceKHR create_device = NULL;
    PFN_xrGetVulkanGraphicsRequirements2KHR get_requirements = NULL;
    need_xr("xrGetInstanceProcAddr(create instance)", xrGetInstanceProcAddr(app->xr, "xrCreateVulkanInstanceKHR", (PFN_xrVoidFunction *)&create_instance));
    need_xr("xrGetInstanceProcAddr(get device)", xrGetInstanceProcAddr(app->xr, "xrGetVulkanGraphicsDevice2KHR", (PFN_xrVoidFunction *)&get_device));
    need_xr("xrGetInstanceProcAddr(create device)", xrGetInstanceProcAddr(app->xr, "xrCreateVulkanDeviceKHR", (PFN_xrVoidFunction *)&create_device));
    need_xr("xrGetInstanceProcAddr(requirements)", xrGetInstanceProcAddr(app->xr, "xrGetVulkanGraphicsRequirements2KHR", (PFN_xrVoidFunction *)&get_requirements));
    need(create_instance && get_device && create_device && get_requirements, "missing Vulkan-enable2 entry point");
    XrGraphicsRequirementsVulkanKHR requirements = {
        .type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR,
    };
    need_xr("xrGetVulkanGraphicsRequirements2KHR", get_requirements(app->xr, app->system, &requirements));
    printf("openxr_vulkan_requirements min=%" PRIu64 " max=%" PRIu64 "\n",
           requirements.minApiVersionSupported, requirements.maxApiVersionSupported);

    VkApplicationInfo vk_app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "stereonaut-xr-quad", .applicationVersion = 1,
        .pEngineName = "stereonaut", .engineVersion = 1, .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo vk_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &vk_app};
    XrVulkanInstanceCreateInfoKHR xr_vk_info = {
        .type = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR, .systemId = app->system,
        .pfnGetInstanceProcAddr = vkGetInstanceProcAddr, .vulkanCreateInfo = &vk_info};
    VkResult vk_result = VK_SUCCESS;
    need_xr("xrCreateVulkanInstanceKHR", create_instance(app->xr, &xr_vk_info, &app->instance, &vk_result));
    need_vk("xrCreateVulkanInstanceKHR Vulkan", vk_result);
    XrVulkanGraphicsDeviceGetInfoKHR device_info = {
        .type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR,
        .systemId = app->system, .vulkanInstance = app->instance};
    need_xr("xrGetVulkanGraphicsDevice2KHR", get_device(app->xr, &device_info, &app->physical));

    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(app->physical, &count, NULL);
    VkQueueFamilyProperties *families = calloc(count, sizeof(*families));
    need(families != NULL, "allocate queue families");
    vkGetPhysicalDeviceQueueFamilyProperties(app->physical, &count, families);
    bool found = false;
    for (uint32_t i = 0; i < count; ++i)
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            app->queue_family = i; found = true; break;
        }
    free(families);
    need(found, "OpenXR Vulkan device has no graphics queue");
    const char *device_extensions[] = {
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
        VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
        VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
    };
    for (size_t i = 0; i < ARRAY_COUNT(device_extensions); ++i)
        need(has_device_extension(app->physical, device_extensions[i]),
             "required NV12 DMA-BUF Vulkan extension is missing");
    /* PC decoder buffers arrive as memfds (no udmabuf size limit). */
    const char *enabled_extensions[ARRAY_COUNT(device_extensions) + 1];
    memcpy(enabled_extensions, device_extensions, sizeof(device_extensions));
    uint32_t enabled_count = ARRAY_COUNT(device_extensions);
    bool host_import = has_device_extension(
        app->physical, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    if (host_import)
        enabled_extensions[enabled_count++] =
            VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
    /* Panels drawn into the eye images are sampled anisotropically. */
    VkPhysicalDeviceFeatures supported, features = {0};
    vkGetPhysicalDeviceFeatures(app->physical, &supported);
    features.samplerAnisotropy = supported.samplerAnisotropy;
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = app->queue_family, .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo vk_device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = enabled_count,
        .ppEnabledExtensionNames = enabled_extensions,
        .pEnabledFeatures = &features};
    XrVulkanDeviceCreateInfoKHR xr_device_info = {
        .type = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR, .systemId = app->system,
        .pfnGetInstanceProcAddr = vkGetInstanceProcAddr, .vulkanPhysicalDevice = app->physical,
        .vulkanCreateInfo = &vk_device_info};
    need_xr("xrCreateVulkanDeviceKHR", create_device(app->xr, &xr_device_info, &app->device, &vk_result));
    need_vk("xrCreateVulkanDeviceKHR Vulkan", vk_result);
    vkGetDeviceQueue(app->device, app->queue_family, 0, &app->queue);
    need(app->queue != VK_NULL_HANDLE, "vkGetDeviceQueue returned null");
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(app->physical, &properties);
    printf("openxr_vulkan_device=%s queue_family=%u\n", properties.deviceName, app->queue_family);
    app->panel_anisotropy = 1.0f;
    if (features.samplerAnisotropy)
        app->panel_anisotropy =
            fminf(8.0f, properties.limits.maxSamplerAnisotropy);
    app->import_semaphore_fd = (PFN_vkImportSemaphoreFdKHR)
        vkGetDeviceProcAddr(app->device, "vkImportSemaphoreFdKHR");
    app->get_memory_fd_properties = (PFN_vkGetMemoryFdPropertiesKHR)
        vkGetDeviceProcAddr(app->device, "vkGetMemoryFdPropertiesKHR");
    app->get_modifier_properties = (PFN_vkGetImageDrmFormatModifierPropertiesEXT)
        vkGetDeviceProcAddr(app->device,
                            "vkGetImageDrmFormatModifierPropertiesEXT");
    need(app->import_semaphore_fd && app->get_memory_fd_properties &&
             app->get_modifier_properties,
         "required external-memory entry point is missing");
    if (host_import)
        app->get_host_pointer_properties =
            (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(
                app->device, "vkGetMemoryHostPointerPropertiesEXT");
}

static void initialize_swapchain(struct app *app)
{
    uint32_t format_count = 0;
    need_xr("xrEnumerateSwapchainFormats(count)", xrEnumerateSwapchainFormats(app->session, 0, &format_count, NULL));
    int64_t *formats = calloc(format_count, sizeof(*formats));
    need(formats != NULL, "allocate swapchain formats");
    need_xr("xrEnumerateSwapchainFormats(list)", xrEnumerateSwapchainFormats(app->session, format_count, &format_count, formats));
    app->color_format = VK_FORMAT_B8G8R8A8_SRGB;
    bool supported = false;
    for (uint32_t i = 0; i < format_count; ++i) if (formats[i] == app->color_format) supported = true;
    if (!supported) { app->color_format = (VkFormat)formats[0]; supported = true; }
    free(formats);
    XrViewConfigurationView views[2] = {
        {.type = XR_TYPE_VIEW_CONFIGURATION_VIEW}, {.type = XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    uint32_t view_count = 0;
    need_xr("xrEnumerateViewConfigurationViews", xrEnumerateViewConfigurationViews(app->xr, app->system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &view_count, views));
    need(view_count == 2, "Steam Frame did not report stereo views");
    app->width = views[0].recommendedImageRectWidth;
    app->height = views[0].recommendedImageRectHeight;
    need(views[1].recommendedImageRectWidth == app->width && views[1].recommendedImageRectHeight == app->height, "eyes have different recommended dimensions");
    XrSwapchainCreateInfo info = {.type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
        .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT,
        .format = app->color_format, .sampleCount = 1, .width = app->width,
        .height = app->height, .faceCount = 1, .arraySize = 2, .mipCount = 1};
    need_xr("xrCreateSwapchain", xrCreateSwapchain(app->session, &info, &app->swapchain));
    need_xr("xrEnumerateSwapchainImages(count)", xrEnumerateSwapchainImages(app->swapchain, 0, &app->image_count, NULL));
    need(app->image_count > 0 && app->image_count <= MAX_IMAGES, "unexpected OpenXR swapchain image count");
    for (uint32_t i = 0; i < app->image_count; ++i) app->images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
    need_xr("xrEnumerateSwapchainImages(list)", xrEnumerateSwapchainImages(app->swapchain, app->image_count, &app->image_count, (XrSwapchainImageBaseHeader *)app->images));
    printf("openxr_swapchain format=%d dimensions=%ux%u images=%u\n", app->color_format, app->width, app->height, app->image_count);
}

/* One plane of a two-plane frame as its own image, sampled by the plane
 * shader (tp10-projection.frag) instead of a YCbCr sampler.
 * TP10 (Iris Main10): TP10 packs three 10-bit samples per word, so a plane
 * has the byte geometry (and Turnip's UBWC layout) of an RGBA8 image a
 * third as wide. The patched Turnip samples such views as FMT6_TP10_Y
 * (RGBA8) or FMT6_TP10_UV (RGB10A2) when STEREONAUT_TP10 is set
 * (tools/mesa-tp10).
 * P010 (PC hardware decoders): linear R16 luma and RG16 chroma, the 10 bits
 * in the high bits, which the shader reads like TP10's normalized codes. */
static void import_plane(struct app *app, struct video_buffer *buffer,
                         VkFormat format, uint64_t drm_modifier,
                         uint32_t width, VkDeviceSize offset, uint32_t rows,
                         VkImage *image, VkDeviceMemory *memory,
                         VkImageView *view)
{
    const VkSubresourceLayout plane = {
        .offset = offset, .rowPitch = app->video_stride,
    };
    VkImageDrmFormatModifierExplicitCreateInfoEXT modifier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        .drmFormatModifier = drm_modifier,
        .drmFormatModifierPlaneCount = 1, .pPlaneLayouts = &plane,
    };
    VkExternalMemoryImageCreateInfo external = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = &modifier,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkImageCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &external,
        .imageType = VK_IMAGE_TYPE_2D, .format = format,
        .extent = {width, rows, 1},
        .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    need_vk("vkCreateImage(video plane)",
            vkCreateImage(app->device, &info, NULL, image));
    VkImageDrmFormatModifierPropertiesEXT chosen = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT,
    };
    need_vk("vkGetImageDrmFormatModifierPropertiesEXT(plane)",
            app->get_modifier_properties(app->device, *image, &chosen));
    need(chosen.drmFormatModifier == drm_modifier,
         "Vulkan substituted the video plane modifier");
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(app->device, *image, &requirements);
    need(requirements.size <= app->dma_buf_size,
         "video plane layout exceeds the DMA-BUF");
    VkMemoryFdPropertiesKHR fd_properties = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
    };
    need_vk("vkGetMemoryFdPropertiesKHR(plane)",
            app->get_memory_fd_properties(
                app->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                buffer->dma_buf_fd, &fd_properties));
    uint32_t bits = requirements.memoryTypeBits & fd_properties.memoryTypeBits;
    need(bits != 0, "plane DMA-BUF memory-type intersection is empty");
    int imported_fd = dup(buffer->dma_buf_fd);
    if (imported_fd < 0) { perror("dup plane DMA-BUF"); exit(2); }
    VkImportMemoryFdInfoKHR import = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        .fd = imported_fd,
    };
    VkMemoryDedicatedAllocateInfo dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = &import, .image = *image,
    };
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &dedicated, .allocationSize = app->dma_buf_size,
        .memoryTypeIndex = memory_type(app->physical, bits, 0),
    };
    need_vk("vkAllocateMemory(import plane)",
            vkAllocateMemory(app->device, &allocation, NULL, memory));
    need_vk("vkBindImageMemory(import plane)",
            vkBindImageMemory(app->device, *image, *memory, 0));
    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = *image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .levelCount = 1, .layerCount = 1},
    };
    need_vk("vkCreateImageView(plane)",
            vkCreateImageView(app->device, &view_info, NULL, view));
}

/* A device-local image the host buffer is copied into: NV12 through the
 * YCbCr sampler, or one P010 plane for the plane shader. */
static void create_upload_image(struct app *app, VkFormat format,
                                uint32_t width, uint32_t height, bool ycbcr,
                                VkImage *image, VkDeviceMemory *memory,
                                VkImageView *view)
{
    VkImageCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = format,
        .extent = {width, height, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    need_vk("vkCreateImage(upload)",
            vkCreateImage(app->device, &info, NULL, image));
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(app->device, *image, &requirements);
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memory_type(app->physical,
                                       requirements.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    need_vk("vkAllocateMemory(upload)",
            vkAllocateMemory(app->device, &allocation, NULL, memory));
    need_vk("vkBindImageMemory(upload)",
            vkBindImageMemory(app->device, *image, *memory, 0));
    VkSamplerYcbcrConversionInfo conversion = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
        .conversion = app->conversion,
    };
    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = ycbcr ? &conversion : NULL,
        .image = *image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .levelCount = 1, .layerCount = 1},
    };
    need_vk("vkCreateImageView(upload)",
            vkCreateImageView(app->device, &view_info, NULL, view));
}

/* NV12_BUFFER_HOST: map the memfd, import the mapping as host memory of a
 * transfer buffer and create the images it is copied into. Sampling
 * device-local images is also much cheaper than reading system memory over
 * PCIe for every eye and display frame. */
static void import_host_buffer(struct app *app, uint32_t i)
{
    struct video_buffer *buffer = &app->video_buffers[i];
    bool p010 = app->video_format == NV12_FORMAT_P010;
    buffer->host = true;
    buffer->tp10 = p010;
    buffer->host_size = (size_t)app->dma_buf_size;
    void *mapping = mmap(NULL, buffer->host_size, PROT_READ | PROT_WRITE,
                         MAP_SHARED, buffer->dma_buf_fd, 0);
    need(mapping != MAP_FAILED, "map host video buffer");
    buffer->host_mapping = mapping;
    VkExternalMemoryBufferCreateInfo external = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
    };
    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &external,
        .size = app->dma_buf_size, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    need_vk("vkCreateBuffer(host video)",
            vkCreateBuffer(app->device, &buffer_info, NULL,
                           &buffer->host_buffer));
    VkMemoryHostPointerPropertiesEXT pointer = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT,
    };
    need_vk("vkGetMemoryHostPointerPropertiesEXT",
            app->get_host_pointer_properties(
                app->device,
                VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                mapping, &pointer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(app->device, buffer->host_buffer,
                                  &requirements);
    uint32_t bits = requirements.memoryTypeBits & pointer.memoryTypeBits;
    need(bits != 0, "host video memory-type intersection is empty");
    VkImportMemoryHostPointerInfoEXT import = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
        .pHostPointer = mapping,
    };
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &import,
        .allocationSize = app->dma_buf_size,
        .memoryTypeIndex = memory_type(app->physical, bits, 0),
    };
    need_vk("vkAllocateMemory(import host video)",
            vkAllocateMemory(app->device, &allocation, NULL,
                             &buffer->host_memory));
    need_vk("vkBindBufferMemory(host video)",
            vkBindBufferMemory(app->device, buffer->host_buffer,
                               buffer->host_memory, 0));
    if (p010) {
        create_upload_image(app, VK_FORMAT_R16_UNORM, app->video_width,
                            app->video_height, false, &buffer->image,
                            &buffer->memory, &buffer->view);
        create_upload_image(app, VK_FORMAT_R16G16_UNORM,
                            app->video_width / 2U, app->video_height / 2U,
                            false, &buffer->image_uv, &buffer->memory_uv,
                            &buffer->view_uv);
    } else {
        create_upload_image(app, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
                            app->video_width, app->video_height, true,
                            &buffer->image, &buffer->memory, &buffer->view);
    }
    printf("host_import index=%u format=%s bytes=%zu\n", i,
           p010 ? "p010" : "nv12", buffer->host_size);
}

/* Records the copy of a new host-memory frame into its image(s); the
 * images stay in GENERAL like imported DMA-BUFs. */
static void record_host_upload(struct app *app,
                               const struct video_buffer *video)
{
    uint32_t count = video->tp10 ? 2U : 1U;
    VkImageMemoryBarrier barriers[2];
    for (uint32_t i = 0; i < count; ++i)
        barriers[i] = (VkImageMemoryBarrier){
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = i ? video->image_uv : video->image,
            .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                 .levelCount = 1, .layerCount = 1},
        };
    vkCmdPipelineBarrier(app->command,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                         count, barriers);
    /* bufferRowLength counts texels of the plane's format. */
    uint32_t stride = app->video_stride;
    if (video->tp10) {
        VkBufferImageCopy luma = {
            .bufferRowLength = stride / 2U,
            .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                 .layerCount = 1},
            .imageExtent = {app->video_width, app->video_height, 1},
        };
        VkBufferImageCopy chroma = {
            .bufferOffset = app->uv_offset, .bufferRowLength = stride / 4U,
            .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                 .layerCount = 1},
            .imageExtent = {app->video_width / 2U, app->video_height / 2U, 1},
        };
        vkCmdCopyBufferToImage(app->command, video->host_buffer, video->image,
                               VK_IMAGE_LAYOUT_GENERAL, 1, &luma);
        vkCmdCopyBufferToImage(app->command, video->host_buffer,
                               video->image_uv, VK_IMAGE_LAYOUT_GENERAL, 1,
                               &chroma);
    } else {
        VkBufferImageCopy planes[2] = {
            {.bufferRowLength = stride,
             .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT,
                                  .layerCount = 1},
             .imageExtent = {app->video_width, app->video_height, 1}},
            {.bufferOffset = app->uv_offset, .bufferRowLength = stride / 2U,
             .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT,
                                  .layerCount = 1},
             .imageExtent = {app->video_width / 2U, app->video_height / 2U,
                             1}},
        };
        vkCmdCopyBufferToImage(app->command, video->host_buffer, video->image,
                               VK_IMAGE_LAYOUT_GENERAL, 2, planes);
    }
    for (uint32_t i = 0; i < count; ++i) {
        barriers[i].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barriers[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    vkCmdPipelineBarrier(app->command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, NULL, 0, NULL, count, barriers);
}

static void import_video_buffer(struct app *app, uint32_t i)
{
    if (app->buffer_kind == NV12_BUFFER_HOST) {
        import_host_buffer(app, i);
        return;
    }
    struct video_buffer *tp10_buffer = &app->video_buffers[i];
    /* TP10 and P010 both go through the plane shader. */
    bool p010 = app->video_format == NV12_FORMAT_P010;
    tp10_buffer->tp10 = app->video_format == NV12_FORMAT_TP10 || p010;
    if (tp10_buffer->tp10) {
        uint64_t modifier = p010 ? DRM_FORMAT_MOD_LINEAR
                                 : DRM_FORMAT_MOD_QCOM_COMPRESSED;
        uint32_t width = p010 ? app->coded_width : app->coded_width / 3U;
        import_plane(app, tp10_buffer,
                     p010 ? VK_FORMAT_R16_UNORM : VK_FORMAT_R8G8B8A8_UNORM,
                     modifier, width, 0, app->coded_height,
                     &tp10_buffer->image, &tp10_buffer->memory,
                     &tp10_buffer->view);
        import_plane(app, tp10_buffer,
                     p010 ? VK_FORMAT_R16G16_UNORM
                          : VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                     modifier, p010 ? width / 2U : width, app->uv_offset,
                     (app->coded_height + 1U) / 2U,
                     &tp10_buffer->image_uv, &tp10_buffer->memory_uv,
                     &tp10_buffer->view_uv);
        VkSemaphoreCreateInfo semaphore = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        };
        need_vk("vkCreateSemaphore(producer)",
                vkCreateSemaphore(app->device, &semaphore, NULL,
                                  &tp10_buffer->producer_semaphore));
        printf("%s_import index=%u coded=%ux%u\n", p010 ? "p010" : "tp10",
               i, app->coded_width, app->coded_height);
        return;
    }
    VkSamplerYcbcrConversionInfo conversion_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
        .conversion = app->conversion,
    };
    const VkSubresourceLayout planes[2] = {
        {.offset = 0,
         .size = (VkDeviceSize)app->video_stride * app->video_height,
         .rowPitch = app->video_stride},
        {.offset = app->uv_offset,
         .size = (VkDeviceSize)app->video_stride * app->video_height / 2U,
         .rowPitch = app->video_stride},
    };
    struct video_buffer *buffer = &app->video_buffers[i];
    VkImageDrmFormatModifierExplicitCreateInfoEXT modifier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        .drmFormatModifier = DRM_FORMAT_MOD_LINEAR,
        .drmFormatModifierPlaneCount = 2, .pPlaneLayouts = planes,
    };
    VkExternalMemoryImageCreateInfo external = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = &modifier,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkImageCreateInfo image = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &external,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
        .extent = {app->video_width, app->video_height, 1},
        .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    need_vk("vkCreateImage(NV12 DMA-BUF)",
            vkCreateImage(app->device, &image, NULL, &buffer->image));
    VkImageDrmFormatModifierPropertiesEXT chosen = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT,
    };
    need_vk("vkGetImageDrmFormatModifierPropertiesEXT",
            app->get_modifier_properties(app->device, buffer->image,
                                         &chosen));
    need(chosen.drmFormatModifier == DRM_FORMAT_MOD_LINEAR,
         "Vulkan substituted the NV12 modifier");
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(app->device, buffer->image, &requirements);
    need(requirements.size <= app->dma_buf_size,
         "NV12 Vulkan memory requirement exceeds DMA-BUF");
    VkMemoryFdPropertiesKHR fd_properties = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
    };
    need_vk("vkGetMemoryFdPropertiesKHR",
            app->get_memory_fd_properties(
                app->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                buffer->dma_buf_fd, &fd_properties));
    uint32_t bits =
        requirements.memoryTypeBits & fd_properties.memoryTypeBits;
    need(bits != 0, "NV12 DMA-BUF memory-type intersection is empty");
    uint32_t type = memory_type(app->physical, bits, 0);
    int imported_fd = dup(buffer->dma_buf_fd);
    if (imported_fd < 0) { perror("dup NV12 DMA-BUF"); exit(2); }
    VkImportMemoryFdInfoKHR import = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        .fd = imported_fd,
    };
    VkMemoryDedicatedAllocateInfo dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = &import, .image = buffer->image,
    };
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &dedicated, .allocationSize = app->dma_buf_size,
        .memoryTypeIndex = type,
    };
    need_vk("vkAllocateMemory(import NV12)",
            vkAllocateMemory(app->device, &allocation, NULL,
                             &buffer->memory));
    need_vk("vkBindImageMemory(import NV12)",
            vkBindImageMemory(app->device, buffer->image,
                              buffer->memory, 0));
    VkImageViewCreateInfo view = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = &conversion_info, .image = buffer->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .levelCount = 1, .layerCount = 1},
    };
    need_vk("vkCreateImageView(NV12)",
            vkCreateImageView(app->device, &view, NULL, &buffer->view));
    VkSemaphoreCreateInfo semaphore = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };
    need_vk("vkCreateSemaphore(producer)",
            vkCreateSemaphore(app->device, &semaphore, NULL,
                              &buffer->producer_semaphore));
    printf("nv12_import index=%u memory_type=%u\n", i, type);
}

/* Shader levels (enum nv12_video_levels): black and white luma and the
 * chroma range in 8-bit code values. */
static void video_levels_uniform(uint32_t levels, float out[4])
{
    static const float table[3][4] = {
        [NV12_LEVELS_NORMAL] = {16.0f, 235.0f, 224.0f, 0.0f},
        [NV12_LEVELS_DEEP] = {0.0f, 235.0f, 224.0f, 0.0f},
        [NV12_LEVELS_FULL] = {0.0f, 255.0f, 255.0f, 0.0f},
    };
    memcpy(out, table[levels <= NV12_LEVELS_FULL ? levels : 0],
           sizeof(table[0]));
}

/* Format check and the YCbCr sampler shared by every imported frame. The
 * sampler only reconstructs chroma (RGB_IDENTITY: r = Cr, g = Y, b = Cb);
 * the shader applies the title's levels and BT.709. */
static void create_video_sampler(struct app *app)
{
    VkDrmFormatModifierPropertiesListEXT list = {
        .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
    };
    VkFormatProperties2 format_properties = {
        .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &list,
    };
    vkGetPhysicalDeviceFormatProperties2(
        app->physical, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
        &format_properties);
    need(list.drmFormatModifierCount > 0, "NV12 exposes no DRM modifiers");
    list.pDrmFormatModifierProperties =
        calloc(list.drmFormatModifierCount,
               sizeof(*list.pDrmFormatModifierProperties));
    need(list.pDrmFormatModifierProperties != NULL,
         "allocate NV12 modifier properties");
    vkGetPhysicalDeviceFormatProperties2(
        app->physical, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
        &format_properties);
    VkFormatFeatureFlags features = 0;
    for (uint32_t i = 0; i < list.drmFormatModifierCount; ++i) {
        VkDrmFormatModifierPropertiesEXT *item =
            &list.pDrmFormatModifierProperties[i];
        if (item->drmFormatModifier == DRM_FORMAT_MOD_LINEAR &&
            item->drmFormatModifierPlaneCount == 2)
            features = item->drmFormatModifierTilingFeatures;
    }
    free(list.pDrmFormatModifierProperties);
    need(features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
         "linear two-plane NV12 is not sampleable");
    need(features &
             VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT,
         "linear-filtered NV12 YCbCr conversion is unsupported");
    VkChromaLocation chroma_location;
    if (features & VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT)
        chroma_location = VK_CHROMA_LOCATION_MIDPOINT;
    else if (features & VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT)
        chroma_location = VK_CHROMA_LOCATION_COSITED_EVEN;
    else
        need(false, "NV12 exposes no supported chroma sample location");

    VkSamplerYcbcrConversionCreateInfo conversion = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,
        .format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
        .ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY,
        .ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL,
        .components = {VK_COMPONENT_SWIZZLE_IDENTITY,
                       VK_COMPONENT_SWIZZLE_IDENTITY,
                       VK_COMPONENT_SWIZZLE_IDENTITY,
                       VK_COMPONENT_SWIZZLE_IDENTITY},
        .xChromaOffset = chroma_location,
        .yChromaOffset = chroma_location,
        .chromaFilter = VK_FILTER_LINEAR,
    };
    need_vk("vkCreateSamplerYcbcrConversion",
            vkCreateSamplerYcbcrConversion(app->device, &conversion, NULL,
                                           &app->conversion));
    VkSamplerYcbcrConversionInfo conversion_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
        .conversion = app->conversion,
    };
    VkSamplerCreateInfo sampler = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext = &conversion_info,
        .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = 0.0f,
    };
    need_vk("vkCreateSampler(NV12)",
            vkCreateSampler(app->device, &sampler, NULL,
                            &app->video_sampler));
    VkSamplerCreateInfo plane_sampler = sampler;
    plane_sampler.pNext = NULL;
    need_vk("vkCreateSampler(TP10)",
            vkCreateSampler(app->device, &plane_sampler, NULL,
                            &app->plane_sampler));

    printf("nv12_sampler format=%d model=rgb_identity levels=shader"
           " chroma_location=%s\n",
           VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
           chroma_location == VK_CHROMA_LOCATION_MIDPOINT ? "midpoint"
                                                          : "cosited_even");
}

static void create_pipeline(struct app *app)
{
    VkAttachmentDescription attachment = {.format = app->color_format, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference reference = {.attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &reference};
    VkRenderPassCreateInfo render_info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &attachment, .subpassCount = 1, .pSubpasses = &subpass};
    need_vk("vkCreateRenderPass", vkCreateRenderPass(app->device, &render_info, NULL, &app->render_pass));
    for (uint32_t image = 0; image < app->image_count; ++image) for (uint32_t eye = 0; eye < 2; ++eye) {
        VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = app->images[image].image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = app->color_format,
            .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .baseArrayLayer = eye, .layerCount = 1}};
        need_vk("vkCreateImageView", vkCreateImageView(app->device, &view, NULL, &app->image_views[image][eye]));
        VkFramebufferCreateInfo fb = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = app->render_pass, .attachmentCount = 1, .pAttachments = &app->image_views[image][eye], .width = app->width, .height = app->height, .layers = 1};
        need_vk("vkCreateFramebuffer", vkCreateFramebuffer(app->device, &fb, NULL, &app->framebuffers[image][eye]));
    }
    /* Binding 1 is the NV12 frame (immutable YCbCr sampler), 2 and 3 the
     * TP10 luma and chroma planes; each pipeline uses only its own. */
    /* Binding 4 is the autofocus result; the measurement (compute) reads
     * the luma through binding 1 or 2. */
    VkDescriptorSetLayoutBinding bindings[5] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_VERTEX_BIT |
                       VK_SHADER_STAGE_FRAGMENT_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT |
                       VK_SHADER_STAGE_COMPUTE_BIT,
         .pImmutableSamplers = &app->video_sampler},
        {.binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT |
                       VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 3, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
        {.binding = 4, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
    };
    VkDescriptorSetLayoutCreateInfo layout = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 5, .pBindings = bindings};
    need_vk("vkCreateDescriptorSetLayout", vkCreateDescriptorSetLayout(app->device, &layout, NULL, &app->descriptor_layout));
    VkPushConstantRange disparity_push = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .size = sizeof(struct disparity_push),
    };
    VkPipelineLayoutCreateInfo pipeline_layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &app->descriptor_layout,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &disparity_push};
    need_vk("vkCreatePipelineLayout", vkCreatePipelineLayout(app->device, &pipeline_layout, NULL, &app->pipeline_layout));
    VkShaderModule vert = shader(app->device, "video-projection.vert.spv"), frag = shader(app->device, "nv12-projection.frag.spv");
    VkPipelineShaderStageCreateInfo stages[2] = {{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vert, .pName = "main"}, {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = frag, .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vertex = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP};
    VkViewport viewport = {.width = (float)app->width, .height = (float)app->height, .minDepth = 0, .maxDepth = 1};
    VkRect2D scissor = {.extent = {app->width, app->height}};
    VkPipelineViewportStateCreateInfo view = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1, .pScissors = &scissor};
    VkPipelineRasterizationStateCreateInfo raster = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo multisample = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState blend_attachment = {.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
    VkPipelineColorBlendStateCreateInfo blend = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &blend_attachment};
    VkGraphicsPipelineCreateInfo pipeline = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = stages, .pVertexInputState = &vertex, .pInputAssemblyState = &assembly, .pViewportState = &view, .pRasterizationState = &raster, .pMultisampleState = &multisample, .pColorBlendState = &blend, .layout = app->pipeline_layout, .renderPass = app->render_pass};
    need_vk("vkCreateGraphicsPipelines", vkCreateGraphicsPipelines(app->device, VK_NULL_HANDLE, 1, &pipeline, NULL, &app->pipeline));
    VkShaderModule tp10_frag = shader(app->device, "tp10-projection.frag.spv");
    stages[1].module = tp10_frag;
    need_vk("vkCreateGraphicsPipelines(TP10)", vkCreateGraphicsPipelines(app->device, VK_NULL_HANDLE, 1, &pipeline, NULL, &app->pipeline_tp10));
    vkDestroyShaderModule(app->device, vert, NULL); vkDestroyShaderModule(app->device, frag, NULL);
    vkDestroyShaderModule(app->device, tp10_frag, NULL);
    const char *const disparity_shaders[2] = {
        "stereo-disparity.comp.spv", "stereo-disparity-tp10.comp.spv",
    };
    VkPipeline *disparity_pipelines[2] = {
        &app->disparity_pipeline, &app->disparity_pipeline_tp10,
    };
    for (int i = 0; i < 2; ++i) {
        VkShaderModule module = shader(app->device, disparity_shaders[i]);
        VkComputePipelineCreateInfo compute = {
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                      .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                      .module = module, .pName = "main"},
            .layout = app->pipeline_layout,
        };
        need_vk("vkCreateComputePipelines(disparity)",
                vkCreateComputePipelines(app->device, VK_NULL_HANDLE, 1,
                                         &compute, NULL,
                                         disparity_pipelines[i]));
        vkDestroyShaderModule(app->device, module, NULL);
    }
}

static struct player_ui_state player_state(const struct app *app)
{
    return (struct player_ui_state){
        .title = app->title,
        .position_us = app->position_us,
        .duration_us = app->duration_us,
        .scrub_us = app->scrub_us,
        .scrubbing = app->scrub_hand >= 0 || app->seek_pending,
        .loading = app->awaiting_frame,
        .paused = app->player_state == NV12_PLAYER_PAUSED,
        .grabbed = app->grab_hand >= 0,
        .popup_open = app->popup_open,
        .popup_tab = app->popup_tab,
        .adjust = app->adjust,
        .gaze_debug = app->gaze_debug,
        .focus_threshold = app->focus_threshold,
        .av1_software = app->av1_software,
        .bindings = app->bindings,
        .learn_action = app->learn_action,
        .debug_text = app->gaze_debug ? app->debug_text : NULL,
        .projection_mode = app->projection_mode,
        .stereo_layout = app->stereo_layout,
        .swap_eyes = app->swap_eyes,
        .video_levels = app->video_levels,
        .format_source = app->format_source,
        .fisheye_fov = app->fisheye_fov,
        .volume = app->volume,
        .end_action = app->end_action,
        .autohide = app->autohide,
        .details = app->details_valid ? &app->details : NULL,
        .hover = app->ui_hover,
        .preview_visible = app->preview_visible,
        .preview_us = app->preview_us,
        .preview = app->preview,
        .preview_perspective = app->preview_perspective,
        .preview_fov = app->preview_fov,
    };
}

/* Where the panels are drawn. SteamVR on a PC composites quad layers so
 * that text and thin lines shimmer and step when the head moves, also when
 * the image has mipmaps or is filtered down beforehand; the video, which the
 * renderer draws into the eye images itself, stays clean. On a PC the
 * panels are therefore drawn into the eye images as well, from a mipmapped
 * image with trilinear and anisotropic filtering. The Frame's compositor
 * shows quad layers cleanly and sharper. STEREONAUT_UI_LAYER=quad|eye
 * overrides. */
static bool panels_in_eye_images(void)
{
    const char *forced = getenv("STEREONAUT_UI_LAYER");
    if (forced && strcmp(forced, "quad") == 0)
        return false;
    if (forced && strcmp(forced, "eye") == 0)
        return true;
    return access("/dev/video-dec0", F_OK) < 0;
}

/* A panel's quad-layer swapchain or, drawn into the eye images, its
 * mipmapped image (then no swapchain). */
static XrSwapchain create_ui_texture(struct app *app,
                                     struct ui_texture *texture,
                                     uint32_t width, uint32_t height,
                                     const char *what)
{
    *texture = (struct ui_texture){.width = width, .height = height};
    if (!app->panels_in_eye_images) {
        XrSwapchainCreateInfo info = {
            .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
            .usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                          XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT,
            .format = app->ui_format,
            .sampleCount = 1,
            .width = width,
            .height = height,
            .faceCount = 1,
            .arraySize = 1,
            .mipCount = 1,
        };
        XrSwapchain swapchain = XR_NULL_HANDLE;
        need_xr(what, xrCreateSwapchain(app->session, &info, &swapchain));
        return swapchain;
    }
    uint32_t size = width > height ? width : height;
    while (size >> texture->mip_levels)
        ++texture->mip_levels;
    VkImageCreateInfo image = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = app->ui_format,
        .extent = {width, height, 1}, .mipLevels = texture->mip_levels,
        .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    need_vk("vkCreateImage(panel)",
            vkCreateImage(app->device, &image, NULL, &texture->image));
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(app->device, texture->image, &requirements);
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memory_type(app->physical,
                                       requirements.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    need_vk("vkAllocateMemory(panel)",
            vkAllocateMemory(app->device, &allocation, NULL,
                             &texture->memory));
    need_vk("vkBindImageMemory(panel)",
            vkBindImageMemory(app->device, texture->image, texture->memory,
                              0));
    VkImageViewCreateInfo view = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = texture->image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = app->ui_format,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .levelCount = texture->mip_levels,
                             .layerCount = 1},
    };
    need_vk("vkCreateImageView(panel)",
            vkCreateImageView(app->device, &view, NULL, &texture->view));
    return XR_NULL_HANDLE;
}

static VkImageMemoryBarrier ui_barrier(VkImage image, uint32_t level,
                                       uint32_t levels, VkAccessFlags from,
                                       VkAccessFlags to,
                                       VkImageLayout old_layout,
                                       VkImageLayout new_layout)
{
    return (VkImageMemoryBarrier){
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = from, .dstAccessMask = to,
        .oldLayout = old_layout, .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .baseMipLevel = level, .levelCount = levels,
                             .layerCount = 1},
    };
}

/* Copies the staged drawing into the quad layer's swapchain image (target)
 * or into the panel image, whose smaller mip levels are then filtered down
 * from it level by level. */
static void record_ui_upload(VkCommandBuffer command,
                             struct ui_texture *texture, VkImage target,
                             VkBuffer staging, bool initialized)
{
    VkImage image = texture->image ? texture->image : target;
    uint32_t levels = texture->image ? texture->mip_levels : 1U;
    bool ready = texture->image ? texture->ready : initialized;
    VkImageMemoryBarrier to_copy = ui_barrier(
        image, 0, levels, ready ? VK_ACCESS_SHADER_READ_BIT : 0,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        ready ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
              : VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                         1, &to_copy);
    VkBufferImageCopy copy = {
        .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .layerCount = 1},
        .imageExtent = {texture->width, texture->height, 1},
    };
    vkCmdCopyBufferToImage(command, staging, image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    int32_t width = (int32_t)texture->width;
    int32_t height = (int32_t)texture->height;
    for (uint32_t level = 1; level < levels; ++level) {
        VkImageMemoryBarrier to_source = ui_barrier(
            image, level - 1, 1, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                             NULL, 1, &to_source);
        int32_t next_width = width > 1 ? width / 2 : 1;
        int32_t next_height = height > 1 ? height / 2 : 1;
        VkImageBlit blit = {
            .srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                               .mipLevel = level - 1, .layerCount = 1},
            .srcOffsets = {{0, 0, 0}, {width, height, 1}},
            .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                               .mipLevel = level, .layerCount = 1},
            .dstOffsets = {{0, 0, 0}, {next_width, next_height, 1}},
        };
        vkCmdBlitImage(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       VK_FILTER_LINEAR);
        width = next_width;
        height = next_height;
    }
    /* All but the last level are now transfer sources. */
    VkImageMemoryBarrier to_sample[2] = {
        ui_barrier(image, 0, levels - 1, VK_ACCESS_TRANSFER_READ_BIT,
                   VK_ACCESS_SHADER_READ_BIT,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        ui_barrier(image, levels - 1, 1, VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_ACCESS_SHADER_READ_BIT,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
    };
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0,
                         NULL, levels > 1 ? 2U : 1U,
                         levels > 1 ? to_sample : &to_sample[1]);
    texture->ready = true;
}

static void create_ui_swapchain(struct app *app)
{
    uint32_t count = 0;
    need_xr("xrEnumerateSwapchainFormats(UI count)",
            xrEnumerateSwapchainFormats(app->session, 0, &count, NULL));
    int64_t *formats = calloc(count, sizeof(*formats));
    need(formats != NULL, "allocate UI swapchain formats");
    need_xr("xrEnumerateSwapchainFormats(UI list)",
            xrEnumerateSwapchainFormats(app->session, count, &count, formats));
    VkFormat preferred[] = {
        VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB,
        VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM,
    };
    for (size_t candidate = 0; candidate < ARRAY_COUNT(preferred); ++candidate)
        for (uint32_t i = 0; i < count; ++i)
            if (!app->ui_format && formats[i] == preferred[candidate])
                app->ui_format = preferred[candidate];
    free(formats);
    need(app->ui_format != VK_FORMAT_UNDEFINED,
         "no supported UI swapchain format");
    app->ui_bgra = app->ui_format == VK_FORMAT_B8G8R8A8_SRGB ||
                   app->ui_format == VK_FORMAT_B8G8R8A8_UNORM;
    app->ui_swapchain = create_ui_texture(app, &app->ui_texture, UI_WIDTH,
                                          UI_HEIGHT, "xrCreateSwapchain(UI)");
    printf("ui_panel format=%d dimensions=%ux%u drawn_into=%s mips=%u\n",
           app->ui_format, UI_WIDTH, UI_HEIGHT,
           app->panels_in_eye_images ? "eye_images" : "quad_layer",
           app->ui_texture.mip_levels);
    if (!app->ui_swapchain)
        return;
    need_xr("xrEnumerateSwapchainImages(UI count)",
            xrEnumerateSwapchainImages(app->ui_swapchain, 0,
                                       &app->ui_image_count, NULL));
    need(app->ui_image_count > 0 && app->ui_image_count <= MAX_IMAGES,
         "unexpected UI swapchain image count");
    for (uint32_t i = 0; i < app->ui_image_count; ++i)
        app->ui_images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
    need_xr("xrEnumerateSwapchainImages(UI list)",
            xrEnumerateSwapchainImages(
                app->ui_swapchain, app->ui_image_count,
                &app->ui_image_count,
                (XrSwapchainImageBaseHeader *)app->ui_images));
}

static void create_ui_resources(struct app *app)
{
    VkCommandBufferAllocateInfo commands = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = app->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    need_vk("vkAllocateCommandBuffers(UI)",
            vkAllocateCommandBuffers(app->device, &commands,
                                     &app->ui_command));
    VkFenceCreateInfo fence = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    need_vk("vkCreateFence(UI)",
            vkCreateFence(app->device, &fence, NULL, &app->ui_fence));
    VkBufferCreateInfo buffer = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = UI_BYTES,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    need_vk("vkCreateBuffer(UI staging)",
            vkCreateBuffer(app->device, &buffer, NULL,
                           &app->ui_staging_buffer));
    VkMemoryRequirements memory;
    vkGetBufferMemoryRequirements(app->device,
                                  app->ui_staging_buffer, &memory);
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memory.size,
        .memoryTypeIndex = memory_type(
            app->physical, memory.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    need_vk("vkAllocateMemory(UI staging)",
            vkAllocateMemory(app->device, &allocation, NULL,
                             &app->ui_staging_memory));
    need_vk("vkBindBufferMemory(UI staging)",
            vkBindBufferMemory(app->device, app->ui_staging_buffer,
                               app->ui_staging_memory, 0));
    need_vk("vkMapMemory(UI staging)",
            vkMapMemory(app->device, app->ui_staging_memory, 0,
                        UI_BYTES, 0, (void **)&app->ui_pixels));
    app->ui_cpu = calloc(1, UI_BYTES);
    need(app->ui_cpu != NULL, "allocate panel pixels");
    app->ui_dirty = true;
}

static void upload_ui(struct app *app)
{
    struct player_ui_state state = player_state(app);
    player_ui_render(&app->player_ui, &state, app->ui_cpu, app->ui_bgra);
    XrSwapchainImageAcquireInfo acquire = {
        .type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO,
    };
    uint32_t index = 0;
    XrSwapchainImageWaitInfo wait = {
        .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,
        .timeout = XR_INFINITE_DURATION,
    };
    if (app->ui_swapchain) {
        need_xr("xrAcquireSwapchainImage(UI)",
                xrAcquireSwapchainImage(app->ui_swapchain, &acquire, &index));
        need_xr("xrWaitSwapchainImage(UI)",
                xrWaitSwapchainImage(app->ui_swapchain, &wait));
        need(index < app->ui_image_count, "invalid UI swapchain index");
    }
    need_vk("vkWaitForFences(UI)",
            vkWaitForFences(app->device, 1, &app->ui_fence,
                            VK_TRUE, UINT64_C(5000000000)));
    need_vk("vkResetFences(UI)",
            vkResetFences(app->device, 1, &app->ui_fence));
    memcpy(app->ui_pixels, app->ui_cpu, UI_BYTES);
    need_vk("vkResetCommandBuffer(UI)",
            vkResetCommandBuffer(app->ui_command, 0));
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    need_vk("vkBeginCommandBuffer(UI)",
            vkBeginCommandBuffer(app->ui_command, &begin));
    record_ui_upload(app->ui_command, &app->ui_texture,
                     app->ui_images[index].image, app->ui_staging_buffer,
                     app->ui_image_initialized[index]);
    need_vk("vkEndCommandBuffer(UI)",
            vkEndCommandBuffer(app->ui_command));
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &app->ui_command,
    };
    need_vk("vkQueueSubmit(UI)",
            vkQueueSubmit(app->queue, 1, &submit, app->ui_fence));
    need_vk("vkWaitForFences(UI completion)",
            vkWaitForFences(app->device, 1, &app->ui_fence,
                            VK_TRUE, UINT64_C(5000000000)));
    app->ui_image_initialized[index] = true;
    XrSwapchainImageReleaseInfo release = {
        .type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO,
    };
    if (app->ui_swapchain)
        need_xr("xrReleaseSwapchainImage(UI)",
                xrReleaseSwapchainImage(app->ui_swapchain, &release));
    app->ui_dirty = false;
}

static void write_video_descriptor(struct app *app, VkDescriptorSet set,
                                   const struct video_buffer *buffer)
{
    VkImageView view = buffer->view;
    VkDescriptorBufferInfo descriptor_buffer = {
        .buffer = app->uniform_buffer,
        .offset = 0,
        .range = sizeof(struct view_uniform),
    };
    VkDescriptorImageInfo descriptor_image = {
        .sampler = app->video_sampler,
        .imageView = view,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
    VkDescriptorImageInfo planes[2] = {
        {.sampler = app->plane_sampler, .imageView = buffer->view,
         .imageLayout = VK_IMAGE_LAYOUT_GENERAL},
        {.sampler = app->plane_sampler, .imageView = buffer->view_uv,
         .imageLayout = VK_IMAGE_LAYOUT_GENERAL},
    };
    VkDescriptorBufferInfo disparity = {
        .buffer = app->disparity_buffer, .offset = 0,
        .range = sizeof(struct disparity_result),
    };
    VkWriteDescriptorSet writes[4] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = set, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
         .pBufferInfo = &descriptor_buffer},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = set, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .pImageInfo = &descriptor_image},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = set, .dstBinding = 2, .descriptorCount = 2,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .pImageInfo = planes},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = set, .dstBinding = 4, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &disparity},
    };
    vkUpdateDescriptorSets(app->device, 1, &writes[3], 0, NULL);
    if (buffer->tp10) {
        vkUpdateDescriptorSets(app->device, 1, &writes[0], 0, NULL);
        vkUpdateDescriptorSets(app->device, 1, &writes[2], 0, NULL);
    } else {
        vkUpdateDescriptorSets(app->device, 2, writes, 0, NULL);
    }
}

static void create_render_resources(struct app *app)
{
    VkCommandPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = app->queue_family};
    need_vk("vkCreateCommandPool", vkCreateCommandPool(app->device, &pool, NULL, &app->command_pool));
    VkCommandBufferAllocateInfo commands = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = app->command_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    need_vk("vkAllocateCommandBuffers", vkAllocateCommandBuffers(app->device, &commands, &app->command));
    VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    need_vk("vkCreateFence", vkCreateFence(app->device, &fence, NULL, &app->fence));
    create_pipeline(app);
    VkBufferCreateInfo buffer = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 2 * UBO_STRIDE, .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    need_vk("vkCreateBuffer(uniform)", vkCreateBuffer(app->device, &buffer, NULL, &app->uniform_buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(app->device, app->uniform_buffer, &requirements);
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memory_type(app->physical, requirements.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    need_vk("vkAllocateMemory(uniform)", vkAllocateMemory(app->device, &allocation, NULL, &app->uniform_memory));
    need_vk("vkBindBufferMemory(uniform)", vkBindBufferMemory(app->device, app->uniform_buffer, app->uniform_memory, 0));
    need_vk("vkMapMemory(uniform)", vkMapMemory(app->device, app->uniform_memory, 0, VK_WHOLE_SIZE, 0, &app->uniform_mapping));
    VkBufferCreateInfo disparity = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = sizeof(struct disparity_result),
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    need_vk("vkCreateBuffer(disparity)", vkCreateBuffer(app->device, &disparity, NULL, &app->disparity_buffer));
    vkGetBufferMemoryRequirements(app->device, app->disparity_buffer, &requirements);
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memory_type(app->physical, requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    need_vk("vkAllocateMemory(disparity)", vkAllocateMemory(app->device, &allocation, NULL, &app->disparity_memory));
    need_vk("vkBindBufferMemory(disparity)", vkBindBufferMemory(app->device, app->disparity_buffer, app->disparity_memory, 0));
    need_vk("vkMapMemory(disparity)", vkMapMemory(app->device, app->disparity_memory, 0, VK_WHOLE_SIZE, 0, &app->disparity_mapping));
    /* A set for every possible buffer (the count varies per stream) plus
     * one extra, which keeps a frame on screen across a session restart
     * while the regular buffers are imported again. */
    const uint32_t set_count = NV12_IPC_MAX_BUFFERS + 1U;
    VkDescriptorPoolSize sizes[3] = {
        {.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
         .descriptorCount = set_count},
        {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .descriptorCount = 3U * set_count},
        {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .descriptorCount = set_count},
    };
    VkDescriptorPoolCreateInfo descriptor_pool = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = set_count, .poolSizeCount = 3,
        .pPoolSizes = sizes};
    need_vk("vkCreateDescriptorPool", vkCreateDescriptorPool(app->device, &descriptor_pool, NULL, &app->descriptor_pool));
    VkDescriptorSetLayout layouts[NV12_IPC_MAX_BUFFERS + 1U];
    VkDescriptorSet sets[NV12_IPC_MAX_BUFFERS + 1U];
    for (uint32_t i = 0; i < set_count; ++i)
        layouts[i] = app->descriptor_layout;
    VkDescriptorSetAllocateInfo descriptor_allocate = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = app->descriptor_pool,
        .descriptorSetCount = set_count,
        .pSetLayouts = layouts};
    need_vk("vkAllocateDescriptorSets",
            vkAllocateDescriptorSets(app->device, &descriptor_allocate,
                                     sets));
    for (uint32_t i = 0; i < NV12_IPC_MAX_BUFFERS; ++i)
        app->video_buffers[i].descriptor_set = sets[i];
    app->spare_set = sets[NV12_IPC_MAX_BUFFERS];
}

static void destroy_video_buffer(struct app *app, struct video_buffer *buffer)
{
    VkDescriptorSet set = buffer->descriptor_set;
    if (buffer->producer_semaphore)
        vkDestroySemaphore(app->device, buffer->producer_semaphore, NULL);
    if (buffer->view)
        vkDestroyImageView(app->device, buffer->view, NULL);
    if (buffer->image)
        vkDestroyImage(app->device, buffer->image, NULL);
    if (buffer->memory)
        vkFreeMemory(app->device, buffer->memory, NULL);
    if (buffer->view_uv)
        vkDestroyImageView(app->device, buffer->view_uv, NULL);
    if (buffer->image_uv)
        vkDestroyImage(app->device, buffer->image_uv, NULL);
    if (buffer->memory_uv)
        vkFreeMemory(app->device, buffer->memory_uv, NULL);
    if (buffer->host_buffer)
        vkDestroyBuffer(app->device, buffer->host_buffer, NULL);
    if (buffer->host_memory)
        vkFreeMemory(app->device, buffer->host_memory, NULL);
    if (buffer->host_mapping)
        munmap(buffer->host_mapping, buffer->host_size);
    if (buffer->dma_buf_fd >= 0)
        close(buffer->dma_buf_fd);
    *buffer = (struct video_buffer){.dma_buf_fd = -1, .descriptor_set = set};
}

/* Session restart (seek): keep only the frame on screen, free the other
 * imports so the new decoder can allocate its CAPTURE buffers, and keep
 * presenting the held frame until the first frame of the new stream. */
static void hold_current_frame(struct app *app)
{
    need_vk("vkDeviceWaitIdle(hold)", vkDeviceWaitIdle(app->device));
    int keep = app->current_video_index;
    if (keep >= 0) {
        discard_held_frame(app);
        app->held = app->video_buffers[keep];
        app->held.producer_wait_pending = false;
        app->video_buffers[keep] = (struct video_buffer){
            .dma_buf_fd = -1, .descriptor_set = app->spare_set,
        };
        app->spare_set = VK_NULL_HANDLE;
        app->has_held = true;
    }
    for (uint32_t i = 0; i < app->video_buffer_count; ++i)
        if ((int)i != keep)
            destroy_video_buffer(app, &app->video_buffers[i]);
    app->current_video_index = -1;
    app->awaiting_frame = true;
    app->ui_dirty = true;
    printf("video_hold kept_frame=%d\n", keep);
}

static void discard_held_frame(struct app *app)
{
    if (!app->has_held)
        return;
    need_vk("vkDeviceWaitIdle(discard held)", vkDeviceWaitIdle(app->device));
    VkDescriptorSet set = app->held.descriptor_set;
    destroy_video_buffer(app, &app->held);
    app->spare_set = set;
    app->has_held = false;
}

static void reinitialize_video(struct app *app,
                               const struct nv12_ipc_message *message,
                               const struct msghdr *received)
{
    apply_init(app, message, received);
    for (uint32_t i = 0; i < app->video_buffer_count; ++i) {
        import_video_buffer(app, i);
        write_video_descriptor(app, app->video_buffers[i].descriptor_set,
                               &app->video_buffers[i]);
    }
    app->current_video_index = -1;
    app->eos_received = false;
    send_ipc(app, NV12_IPC_READY, 0);
    send_volume(app);
    printf("video_reinit buffers=%u\n", app->video_buffer_count);
}

static XrVector3f rotate_vector(XrQuaternionf q, XrVector3f vector)
{
    XrVector3f u = {q.x, q.y, q.z};
    float dot_uv = u.x * vector.x + u.y * vector.y + u.z * vector.z;
    float dot_uu = u.x * u.x + u.y * u.y + u.z * u.z;
    XrVector3f cross = {
        u.y * vector.z - u.z * vector.y,
        u.z * vector.x - u.x * vector.z,
        u.x * vector.y - u.y * vector.x,
    };
    return (XrVector3f){
        2.0f * dot_uv * u.x + (q.w * q.w - dot_uu) * vector.x +
            2.0f * q.w * cross.x,
        2.0f * dot_uv * u.y + (q.w * q.w - dot_uu) * vector.y +
            2.0f * q.w * cross.y,
        2.0f * dot_uv * u.z + (q.w * q.w - dot_uu) * vector.z +
            2.0f * q.w * cross.z,
    };
}

static XrQuaternionf quaternion_conjugate(XrQuaternionf q)
{
    return (XrQuaternionf){-q.x, -q.y, -q.z, q.w};
}

static XrQuaternionf yaw_quaternion(float yaw)
{
    return (XrQuaternionf){0.0f, sinf(yaw * 0.5f), 0.0f, cosf(yaw * 0.5f)};
}

static float vector_dot(XrVector3f a, XrVector3f b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

struct panel_hit {
    bool plane;
    float u, v;
};

static bool panel_hit_inside(const struct panel_hit *hit)
{
    return hit->plane && hit->u >= 0.0f && hit->u < 1.0f &&
           hit->v >= 0.0f && hit->v < 1.0f;
}

/* Intersect the aim ray with the (unbounded) panel plane. u/v are panel
 * texture coordinates and may lie outside [0,1). */
static struct panel_hit panel_ray_at(XrVector3f center, float yaw,
                                     float width_meters, float height_meters,
                                     XrPosef aim)
{
    struct panel_hit hit = {0};
    XrQuaternionf orientation = yaw_quaternion(yaw);
    XrVector3f normal = rotate_vector(orientation, (XrVector3f){0, 0, 1});
    XrVector3f direction =
        rotate_vector(aim.orientation, (XrVector3f){0, 0, -1});
    float denominator = vector_dot(direction, normal);
    if (denominator > -0.0001f)
        return hit;
    XrVector3f to_center = {center.x - aim.position.x,
                            center.y - aim.position.y,
                            center.z - aim.position.z};
    float distance = vector_dot(to_center, normal) / denominator;
    if (distance <= 0.0f)
        return hit;
    XrVector3f point = {aim.position.x + direction.x * distance - center.x,
                        aim.position.y + direction.y * distance - center.y,
                        aim.position.z + direction.z * distance - center.z};
    XrVector3f local = rotate_vector(quaternion_conjugate(orientation), point);
    hit.plane = true;
    hit.u = local.x / width_meters + 0.5f;
    hit.v = 0.5f - local.y / height_meters;
    return hit;
}

/* Intersect the aim ray with the (unbounded) player panel plane. u/v are
 * panel texture coordinates and may lie outside [0,1). */
static struct panel_hit panel_ray(const struct app *app, XrPosef aim)
{
    return panel_ray_at((XrVector3f){app->panel_x, app->panel_y, app->panel_z},
                        app->panel_yaw, UI_PANEL_WIDTH_METERS,
                        UI_PANEL_HEIGHT_METERS, aim);
}

static int64_t scrub_target_us(const struct app *app, float u)
{
    return (int64_t)((double)player_ui_seek_ratio(u) *
                     (double)app->duration_us);
}

/* Hit on a visible part of the player panel (not its transparent area). */
static bool on_player_panel(const struct app *app,
                            const struct panel_hit *hit)
{
    struct player_ui_state state = player_state(app);
    return panel_hit_inside(hit) &&
           player_ui_on_panel(&state, hit->u, hit->v);
}

/* Puts the pointer layer on the panel plane at the hit point, slightly in
 * front of the panel. */
static void place_pointer(struct app *app, XrVector3f center, float yaw,
                          float width_meters, float height_meters,
                          struct panel_hit hit, float size)
{
    XrQuaternionf orientation = yaw_quaternion(yaw);
    XrVector3f local = {(hit.u - 0.5f) * width_meters,
                        (0.5f - hit.v) * height_meters, 0.006f};
    XrVector3f offset = rotate_vector(orientation, local);
    app->pointer_pose = (XrPosef){
        orientation,
        {center.x + offset.x, center.y + offset.y, center.z + offset.z}};
    app->pointer_size = size;
    app->pointer_layer_visible = true;
}

struct hand_input {
    bool aim_valid;
    XrPosef aim;
    struct panel_hit hit;
    bool select_edge;
    bool select_held;
    XrActionStateBoolean squeeze;
};

static void read_boolean(struct app *app, XrAction action, XrPath hand,
                         XrActionStateBoolean *state)
{
    XrActionStateGetInfo info = {
        .type = XR_TYPE_ACTION_STATE_GET_INFO,
        .action = action,
        .subactionPath = hand,
    };
    *state = (XrActionStateBoolean){.type = XR_TYPE_ACTION_STATE_BOOLEAN};
    need_xr("xrGetActionStateBoolean",
            xrGetActionStateBoolean(app->session, &info, state));
}

/* Horizontal thumbstick flicks: -1/+1 once per deflection past the
 * threshold (vertical movement is ignored), 0 otherwise. */
static int read_stick(struct app *app, unsigned hand)
{
    XrActionStateGetInfo info = {
        .type = XR_TYPE_ACTION_STATE_GET_INFO,
        .action = app->stick_action,
        .subactionPath = app->hands[hand],
    };
    XrActionStateVector2f state = {.type = XR_TYPE_ACTION_STATE_VECTOR2F};
    need_xr("xrGetActionStateVector2f",
            xrGetActionStateVector2f(app->session, &info, &state));
    float x = state.isActive ? state.currentState.x : 0.0f;
    float y = state.isActive ? state.currentState.y : 0.0f;
    int direction = app->stick_direction[hand];
    if (direction && direction * x < STICK_RELEASE_THRESHOLD) {
        app->stick_direction[hand] = 0;
        return 0;
    }
    if (!direction) {
        if (fabsf(x) < STICK_PRESS_THRESHOLD || fabsf(y) > fabsf(x))
            return 0;
        direction = x > 0.0f ? 1 : -1;
        app->stick_direction[hand] = direction;
        return direction;
    }
    return 0;
}

static void read_select(struct app *app, unsigned hand,
                        struct hand_input *input)
{
    XrActionStateGetInfo info = {
        .type = XR_TYPE_ACTION_STATE_GET_INFO,
        .action = app->select_action,
        .subactionPath = app->hands[hand],
    };
    XrActionStateFloat state = {.type = XR_TYPE_ACTION_STATE_FLOAT};
    need_xr("xrGetActionStateFloat",
            xrGetActionStateFloat(app->session, &info, &state));
    bool down = app->select_down[hand];
    if (!state.isActive)
        down = false;
    else if (!down && state.currentState >= SELECT_PRESS_THRESHOLD)
        down = true;
    else if (down && state.currentState < SELECT_RELEASE_THRESHOLD)
        down = false;
    input->select_edge = down && !app->select_down[hand];
    input->select_held = down;
    app->select_down[hand] = down;
}

static bool pressed_edge(const XrActionStateBoolean *state)
{
    return state->isActive && state->changedSinceLastSync &&
           state->currentState;
}

static bool held(const XrActionStateBoolean *state)
{
    return state->isActive && state->currentState;
}

/* Show the panel again. If it is far outside the gaze cone it is moved in
 * front of the viewer, otherwise it keeps its world position. */
static double monotonic_seconds(void);

static void show_panel(struct app *app, const XrSpaceLocation *head,
                       bool head_valid)
{
    app->ui_visible = true;
    app->last_input_time = monotonic_seconds();
    if (head_valid) {
        XrVector3f gaze = rotate_vector(head->pose.orientation,
                                        (XrVector3f){0, 0, -1});
        XrVector3f to_panel = {app->panel_x - head->pose.position.x,
                               app->panel_y - head->pose.position.y,
                               app->panel_z - head->pose.position.z};
        float length = sqrtf(vector_dot(to_panel, to_panel));
        if (length < 0.1f ||
            vector_dot(gaze, to_panel) / length < GRAB_RECENTER_COS) {
            float flat = sqrtf(gaze.x * gaze.x + gaze.z * gaze.z);
            if (flat > 0.01f) {
                gaze.x /= flat;
                gaze.z /= flat;
                app->panel_x = head->pose.position.x + gaze.x * 1.3f;
                app->panel_y = head->pose.position.y - 0.35f;
                app->panel_z = head->pose.position.z + gaze.z * 1.3f;
                app->panel_yaw = atan2f(-gaze.x, -gaze.z);
            }
        }
    }
    app->ui_dirty = true;
    send_panel_state(app);
    printf("player_panel=visible x=%.3f y=%.3f z=%.3f yaw=%.3f\n",
           app->panel_x, app->panel_y, app->panel_z, app->panel_yaw);
}

/* Shows the scrub preview for `position_us` (-1 hides it) and asks the
 * trickplay worker for the matching picture. */
static void update_preview(struct app *app, int64_t position_us)
{
    bool visible = position_us >= 0 && app->duration_us > 0;
    if (visible != app->preview_visible ||
        (visible && position_us / 1000000 != app->preview_us / 1000000))
        app->ui_dirty = true;
    app->preview_visible = visible;
    if (!visible)
        return;
    app->preview_us = position_us;
    trickplay_request(app->trickplay, position_us, app->stereo_layout,
                      app->projection_mode,
                      app->preview_perspective ? app->preview_fov : 0);
}

/* ---- controller bindings ---------------------------------------------- */

/* Stick directions of one hand: the dominant direction past the press
 * threshold stays held until it falls under the release threshold. */
static void read_stick_directions(struct app *app, unsigned hand,
                                  bool down[BINDING_INPUT_COUNT])
{
    XrActionStateGetInfo info = {
        .type = XR_TYPE_ACTION_STATE_GET_INFO,
        .action = app->stick_action,
        .subactionPath = app->hands[hand],
    };
    XrActionStateVector2f state = {.type = XR_TYPE_ACTION_STATE_VECTOR2F};
    need_xr("xrGetActionStateVector2f",
            xrGetActionStateVector2f(app->session, &info, &state));
    float x = state.isActive ? state.currentState.x : 0.0f;
    float y = state.isActive ? state.currentState.y : 0.0f;
    const float values[4] = {-x, x, y, -y}; /* left, right, up, down */
    for (int d = 0; d < 4; ++d)
        if (app->input_held[binding_stick(hand, d)] &&
            values[d] >= STICK_RELEASE_THRESHOLD) {
            down[binding_stick(hand, d)] = true;
            return;
        }
    int best = -1;
    float best_value = STICK_PRESS_THRESHOLD;
    for (int d = 0; d < 4; ++d)
        if (values[d] >= best_value) {
            best = d;
            best_value = values[d];
        }
    if (best >= 0)
        down[binding_stick(hand, best)] = true;
}

/* Ends the session and starts the next or previous title of the folder;
 * the BROWSE that follows picks up app->skip_path. */
static void skip_title(struct app *app, int direction)
{
    char next[PLAYER_UI_PATH_MAX];
    if (app->skip_path[0] ||
        !browser_ui_following_title(&app->browser, app->location, direction,
                                    next, sizeof(next))) {
        printf("binding_skip direction=%d none\n", direction);
        return;
    }
    snprintf(app->skip_path, sizeof(app->skip_path), "%s", next);
    snprintf(app->skip_title, sizeof(app->skip_title), "%s",
             app->browser.selected_title);
    printf("binding_skip direction=%d next=%s\n", direction, next);
    send_ipc(app, NV12_IPC_ACTION, NV12_ACTION_BACK);
    if (app->awaiting_frame)
        send_control(app, PLAYER_UI_STOP_REQUESTED, NULL, 0);
}

static void run_binding(struct app *app, int action)
{
    printf("binding_action %s\n", binding_action_info[action].key);
    switch (action) {
    case BINDING_SEEK_BACK:
    case BINDING_SEEK_FORWARD:
        if (app->scrub_hand < 0 && !app->seek_pending)
            send_ipc(app, NV12_IPC_ACTION,
                     action == BINDING_SEEK_FORWARD ? NV12_ACTION_SEEK_FORWARD
                                                    : NV12_ACTION_SEEK_BACK);
        break;
    case BINDING_VOLUME_DOWN:
    case BINDING_VOLUME_UP:
        change_volume(app, action == BINDING_VOLUME_UP ? 1 : -1);
        break;
    case BINDING_TILT_DOWN:
    case BINDING_TILT_UP:
        change_adjust(app, PLAYER_UI_ADJUST + 2 * PLAYER_ADJUST_TILT +
                               (action == BINDING_TILT_UP));
        break;
    case BINDING_ZOOM_OUT:
    case BINDING_ZOOM_IN:
        change_adjust(app, PLAYER_UI_ADJUST + 2 * PLAYER_ADJUST_ZOOM +
                               (action == BINDING_ZOOM_IN));
        break;
    case BINDING_PLAY_PAUSE:
        send_ipc(app, NV12_IPC_ACTION, NV12_ACTION_TOGGLE_PAUSE);
        break;
    case BINDING_PREVIOUS:
    case BINDING_NEXT:
        skip_title(app, action == BINDING_NEXT ? 1 : -1);
        break;
    }
}

/* Assigns the pressed buttons to the action waiting for them; another
 * action with the same buttons loses them. */
static void learn_binding(struct app *app, int input, int modifier)
{
    printf("binding_learn input=%d modifier=%d grip_value=%.2f/%.2f\n", input,
           modifier, app->grip_value[0], app->grip_value[1]);
    for (int i = 0; i < BINDING_ACTION_COUNT; ++i)
        if (i != app->learn_action && app->bindings[i].input == input &&
            app->bindings[i].modifier == modifier) {
            app->bindings[i] = (struct binding){-1, -1};
            save_binding(app, i);
        }
    app->bindings[app->learn_action] = (struct binding){input, modifier};
    save_binding(app, app->learn_action);
    app->learn_action = -1;
    app->ui_dirty = true;
}

/* A modifier counts if it was down before this frame. */
static void press_input(struct app *app, int input, double now)
{
    app->last_input_time = now;
    app->input_combined[input] = false;
    app->input_repeat[input] = 0;
    int modifier = -1;
    if (app->learn_action >= 0) {
        for (int m = BINDING_FIRST_BUTTON; m < BINDING_INPUT_COUNT; ++m)
            if (m != input && app->input_held[m]) {
                modifier = m;
                break;
            }
        if (modifier >= 0) {
            app->input_combined[modifier] = true;
            learn_binding(app, input, modifier);
        } else if (!binding_is_button(input)) {
            learn_binding(app, input, -1);
        }
        /* A button alone is assigned on release: it may still become the
         * first button of a combination. */
    } else {
        int action = binding_match(app->bindings, input, app->input_held,
                                   &modifier);
        if (modifier >= 0)
            app->input_combined[modifier] = true;
        else if (binding_is_button(input) &&
                 binding_is_modifier(app->bindings, input))
            return; /* acts on release unless used as a modifier */
        if (action < 0)
            return;
        run_binding(app, action);
        if (binding_action_info[action].repeat) {
            app->input_repeat[input] = action + 1;
            app->input_repeat_time[input] = now + BINDING_REPEAT_DELAY;
        }
    }
    /* A grip that became a modifier does not show/hide the panel. */
    if (modifier == BINDING_LEFT_GRIP || modifier == BINDING_RIGHT_GRIP)
        app->grip_toggle[modifier == BINDING_RIGHT_GRIP] = false;
}

static void release_input(struct app *app, int input)
{
    bool combined = app->input_combined[input];
    app->input_combined[input] = false;
    app->input_repeat[input] = 0;
    if (!binding_is_button(input) || combined ||
        input == BINDING_LEFT_GRIP || input == BINDING_RIGHT_GRIP)
        return;
    if (app->learn_action >= 0) {
        learn_binding(app, input, -1);
        return;
    }
    if (!binding_is_modifier(app->bindings, input))
        return; /* acted on press */
    const bool none[BINDING_INPUT_COUNT] = {false};
    int modifier;
    int action = binding_match(app->bindings, input, none, &modifier);
    if (action >= 0)
        run_binding(app, action);
}

/* Turns stick directions and buttons into actions; while an action waits
 * for its buttons, the next press is assigned to it instead. */
static void process_bindings(struct app *app, const bool grip[2])
{
    bool down[BINDING_INPUT_COUNT] = {false};
    for (unsigned hand = 0; hand < 2; ++hand) {
        read_stick_directions(app, hand, down);
        down[hand ? BINDING_RIGHT_GRIP : BINDING_LEFT_GRIP] = grip[hand];
    }
    for (size_t i = 0; i < ARRAY_COUNT(button_inputs); ++i) {
        XrActionStateBoolean state;
        read_boolean(app, app->button_actions[i],
                     app->hands[button_inputs[i].hand], &state);
        down[button_inputs[i].input] = held(&state);
    }
    double now = monotonic_seconds();
    for (int input = 0; input < BINDING_INPUT_COUNT; ++input) {
        bool was = app->input_held[input];
        if (down[input] && !was) {
            press_input(app, input, now);
        } else if (!down[input] && was) {
            release_input(app, input);
        } else if (down[input] && app->input_repeat[input] &&
                   now >= app->input_repeat_time[input]) {
            run_binding(app, app->input_repeat[input] - 1);
            app->input_repeat_time[input] = now + BINDING_REPEAT_INTERVAL;
        }
    }
    memcpy(app->input_held, down, sizeof(down));
}

static void sync_player_input(struct app *app, XrTime time)
{
    XrActiveActionSet active = {.actionSet = app->action_set};
    XrActionsSyncInfo sync = {
        .type = XR_TYPE_ACTIONS_SYNC_INFO,
        .countActiveActionSets = 1,
        .activeActionSets = &active,
    };
    need_xr("xrSyncActions", xrSyncActions(app->session, &sync));
    XrSpaceLocation head = {.type = XR_TYPE_SPACE_LOCATION};
    need_xr("xrLocateSpace(player head)",
            xrLocateSpace(app->view_space, app->space, time, &head));
    const XrSpaceLocationFlags valid =
        XR_SPACE_LOCATION_POSITION_VALID_BIT |
        XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    bool head_valid = (head.locationFlags & valid) == valid;

    struct hand_input hands[2] = {0};
    for (unsigned hand = 0; hand < 2; ++hand) {
        XrActionStateGetInfo pose_info = {
            .type = XR_TYPE_ACTION_STATE_GET_INFO,
            .action = app->aim_action,
            .subactionPath = app->hands[hand],
        };
        XrActionStatePose pose = {.type = XR_TYPE_ACTION_STATE_POSE};
        need_xr("xrGetActionStatePose",
                xrGetActionStatePose(app->session, &pose_info, &pose));
        if (pose.isActive) {
            XrSpaceLocation location = {.type = XR_TYPE_SPACE_LOCATION};
            need_xr("xrLocateSpace(player aim)",
                    xrLocateSpace(app->aim_spaces[hand], app->space,
                                  time, &location));
            if ((location.locationFlags & valid) == valid) {
                hands[hand].aim_valid = true;
                hands[hand].aim = location.pose;
                hands[hand].hit = panel_ray(app, location.pose);
            }
        }
        read_select(app, hand, &hands[hand]);
        read_boolean(app, app->squeeze_action, app->hands[hand],
                     &hands[hand].squeeze);
    }
    bool grips[2];
    for (unsigned hand = 0; hand < 2; ++hand) {
        XrActionStateGetInfo info = {
            .type = XR_TYPE_ACTION_STATE_GET_INFO,
            .action = app->grip_value_action,
            .subactionPath = app->hands[hand],
        };
        XrActionStateFloat state = {.type = XR_TYPE_ACTION_STATE_FLOAT};
        need_xr("xrGetActionStateFloat(grip)",
                xrGetActionStateFloat(app->session, &info, &state));
        float value = state.isActive ? state.currentState : 0.0f;
        app->grip_value[hand] = value;
        app->grip_value_down[hand] =
            value >= (app->grip_value_down[hand] ? GRIP_RELEASE_THRESHOLD
                                                 : GRIP_PRESS_THRESHOLD);
        grips[hand] = held(&hands[hand].squeeze) || app->grip_value_down[hand];
    }
    process_bindings(app, grips);

    /* Auto-hide: any trigger, grip or stick, or pointing at the panel,
     * counts as input; a trigger brings a hidden panel back without
     * pressing whatever lies under the pointer. */
    double now = monotonic_seconds();
    bool consumed_select = false;
    for (unsigned hand = 0; hand < 2; ++hand) {
        bool pointing = app->ui_visible && hands[hand].aim_valid &&
                        on_player_panel(app, &hands[hand].hit);
        if (hands[hand].select_edge || pressed_edge(&hands[hand].squeeze) ||
            pointing)
            app->last_input_time = now;
        if (app->auto_hidden && !app->ui_visible &&
            hands[hand].select_edge) {
            show_panel(app, &head, head_valid);
            app->auto_hidden = false;
            consumed_select = true;
        }
    }
    if (app->ui_visible && app->autohide > 0 &&
        app->player_state == NV12_PLAYER_PLAYING && !app->popup_open &&
        !app->awaiting_frame && app->grab_hand < 0 && app->scrub_hand < 0 &&
        now - app->last_input_time > app->autohide) {
        app->ui_visible = false;
        app->auto_hidden = true;
        send_panel_state(app);
        printf("player_panel=auto-hidden after_s=%d\n", app->autohide);
    }

    /* Grip: grab the panel, or show/hide it when pressed on empty space. */
    if (app->grab_hand >= 0 &&
        !held(&hands[app->grab_hand].squeeze)) {
        app->grab_hand = -1;
        app->ui_dirty = true;
        send_panel_state(app);
        printf("player_panel=released x=%.3f y=%.3f z=%.3f yaw=%.3f\n",
               app->panel_x, app->panel_y, app->panel_z, app->panel_yaw);
    }
    for (unsigned hand = 0; hand < 2; ++hand) {
        /* Show/hide on release, unless the grip served as a modifier of a
         * button binding in between. */
        if (app->grip_toggle[hand] && !held(&hands[hand].squeeze)) {
            app->grip_toggle[hand] = false;
            if (app->ui_visible) {
                app->ui_visible = false;
                app->auto_hidden = false;
                send_panel_state(app);
                printf("player_panel=hidden\n");
            } else {
                show_panel(app, &head, head_valid);
                app->auto_hidden = false;
            }
        }
        if (!pressed_edge(&hands[hand].squeeze) || app->grab_hand >= 0 ||
            app->scrub_hand >= 0)
            continue;
        if (app->ui_visible && hands[hand].aim_valid &&
            on_player_panel(app, &hands[hand].hit)) {
            XrPosef aim = hands[hand].aim;
            XrVector3f center = {app->panel_x - aim.position.x,
                                 app->panel_y - aim.position.y,
                                 app->panel_z - aim.position.z};
            app->grab_offset = rotate_vector(
                quaternion_conjugate(aim.orientation), center);
            app->grab_hand = (int)hand;
            app->ui_dirty = true;
            printf("player_panel=grabbed hand=%u\n", hand);
        } else if (app->learn_action < 0) {
            app->grip_toggle[hand] = true;
        }
    }
    if (app->grab_hand >= 0 && hands[app->grab_hand].aim_valid) {
        XrPosef aim = hands[app->grab_hand].aim;
        XrVector3f offset = rotate_vector(aim.orientation, app->grab_offset);
        app->panel_x = aim.position.x + offset.x;
        app->panel_y = aim.position.y + offset.y;
        app->panel_z = aim.position.z + offset.z;
        if (head_valid)
            app->panel_yaw = atan2f(head.pose.position.x - app->panel_x,
                                    head.pose.position.z - app->panel_z);
    }

    /* Trigger: buttons on press, seek bar while held. */
    if (app->scrub_hand >= 0) {
        struct hand_input *input = &hands[app->scrub_hand];
        if (input->aim_valid && input->hit.plane) {
            int64_t target = scrub_target_us(app, input->hit.u);
            if (target != app->scrub_us) {
                app->scrub_us = target;
                app->ui_dirty = true;
            }
        }
        if (!input->select_held) {
            printf("player_seek_bar target_us=%" PRId64 "\n", app->scrub_us);
            app->scrub_hand = -1;
            app->seek_pending = true;
            app->ui_dirty = true;
            send_seek_to(app, app->scrub_us);
        }
    }
    for (unsigned hand = 0; hand < 2; ++hand) {
        if (!app->ui_visible || consumed_select || app->grab_hand >= 0 ||
            app->scrub_hand >= 0 || app->seek_pending ||
            !hands[hand].select_edge || !hands[hand].aim_valid ||
            !on_player_panel(app, &hands[hand].hit))
            continue;
        struct player_ui_state state = player_state(app);
        int action = player_ui_hit_test(&state, hands[hand].hit.u,
                                        hands[hand].hit.v);
        if (action == PLAYER_UI_SEEK_BAR) {
            app->scrub_hand = (int)hand;
            app->scrub_us = scrub_target_us(app, hands[hand].hit.u);
            app->ui_dirty = true;
        } else if (action == PLAYER_UI_VIEW_POPUP) {
            app->popup_open = !app->popup_open;
            app->learn_action = -1;
            app->ui_dirty = true;
        } else if (action >= PLAYER_UI_BINDING &&
                   action < PLAYER_UI_BINDING + BINDING_ACTION_COUNT) {
            int chosen = action - PLAYER_UI_BINDING;
            app->learn_action = app->learn_action == chosen ? -1 : chosen;
            app->ui_dirty = true;
        } else if (action == PLAYER_UI_BINDING_CLEAR) {
            if (app->learn_action >= 0) {
                app->bindings[app->learn_action] = (struct binding){-1, -1};
                save_binding(app, app->learn_action);
            }
            app->learn_action = -1;
            app->ui_dirty = true;
        } else if (action == PLAYER_UI_BINDING_CANCEL) {
            app->learn_action = -1;
            app->ui_dirty = true;
        } else if (action == PLAYER_UI_BINDING_DEFAULTS) {
            for (int i = 0; i < BINDING_ACTION_COUNT; ++i) {
                app->bindings[i] = binding_action_info[i].standard;
                save_binding(app, i);
            }
            app->learn_action = -1;
            app->ui_dirty = true;
        } else if (action >= PLAYER_UI_ADJUST &&
                   action < PLAYER_UI_ADJUST_END) {
            change_adjust(app, action);
        } else if (action >= PLAYER_UI_TAB &&
                   action < PLAYER_UI_TAB + PLAYER_UI_TAB_COUNT) {
            app->popup_tab = action - PLAYER_UI_TAB;
            app->learn_action = -1;
            app->ui_dirty = true;
        } else if (action >= PLAYER_UI_AUTOHIDE &&
                   action < PLAYER_UI_AUTOHIDE + PLAYER_UI_AUTOHIDE_COUNT) {
            app->autohide = player_ui_autohide[action - PLAYER_UI_AUTOHIDE];
            settings_set_int("panel_autohide", app->autohide);
            app->ui_dirty = true;
        } else if (action == PLAYER_UI_END_ACTION) {
            app->end_action = (app->end_action + 1) % PLAYER_END_COUNT;
            settings_set_int("end_action", app->end_action);
            app->ui_dirty = true;
        } else if (action == PLAYER_UI_VOLUME_DOWN ||
                   action == PLAYER_UI_VOLUME_UP) {
            change_volume(app, action == PLAYER_UI_VOLUME_UP ? 1 : -1);
        } else if (action == PLAYER_UI_FOCUS_THRESHOLD ||
                   action == PLAYER_UI_FOCUS_THRESHOLD + 1) {
            app->focus_threshold = action == PLAYER_UI_FOCUS_THRESHOLD + 1;
            settings_set_bool("focus_threshold", app->focus_threshold);
            app->ui_dirty = true;
            printf("focus_threshold=%d\n", app->focus_threshold);
        } else if (action == PLAYER_UI_AV1_SOFTWARE ||
                   action == PLAYER_UI_AV1_SOFTWARE + 1) {
            app->av1_software = action == PLAYER_UI_AV1_SOFTWARE + 1;
            settings_set_bool("av1_software", app->av1_software);
            app->ui_dirty = true;
            printf("av1_software=%d\n", app->av1_software);
        } else if (action == PLAYER_UI_GAZE_DEBUG) {
            app->gaze_debug = !app->gaze_debug;
            app->debug_text[0] = '\0';
            app->ui_dirty = true;
            printf("gaze_debug=%d\n", app->gaze_debug);
        } else if (action == PLAYER_UI_RECENTER) {
            app->recenter_pending = true;
            app->recenter_yaw = true;
        } else if (action == PLAYER_UI_RESET_POSITION ||
                   action == PLAYER_UI_RESET_PICTURE) {
            reset_adjust(app, action == PLAYER_UI_RESET_PICTURE);
        } else if (action >= PLAYER_UI_PREVIEW_MODE) {
            change_preview(app, action);
        } else if (action >= PLAYER_UI_PROJECTION) {
            change_view(app, action);
        } else if (action) {
            send_ipc(app, NV12_IPC_ACTION, (uint32_t)action);
            /* Before the first frame the session may be blocked opening the
             * source and cannot read the action. */
            if (action == NV12_ACTION_BACK && app->awaiting_frame)
                send_control(app, PLAYER_UI_STOP_REQUESTED, NULL, 0);
        }
    }

    int hover = 0;
    int64_t hover_us = -1;
    bool pointer_visible = false;
    int pointer_x = 0;
    int pointer_y = 0;
    app->pointer_layer_visible = false;
    if (app->ui_visible && app->grab_hand < 0) {
        for (unsigned hand = 0; hand < 2; ++hand) {
            if (!hands[hand].aim_valid ||
                !on_player_panel(app, &hands[hand].hit))
                continue;
            pointer_visible = true;
            pointer_x = (int)(hands[hand].hit.u * UI_WIDTH);
            pointer_y = (int)(hands[hand].hit.v * UI_HEIGHT);
            struct player_ui_state state = player_state(app);
            hover = player_ui_hit_test(&state, hands[hand].hit.u,
                                       hands[hand].hit.v);
            if (hover == PLAYER_UI_SEEK_BAR)
                hover_us = scrub_target_us(app, hands[hand].hit.u);
            place_pointer(app,
                          (XrVector3f){app->panel_x, app->panel_y,
                                       app->panel_z},
                          app->panel_yaw, UI_PANEL_WIDTH_METERS,
                          UI_PANEL_HEIGHT_METERS, hands[hand].hit, 0.03f);
        }
        if (app->scrub_hand >= 0)
            hover = PLAYER_UI_SEEK_BAR;
    }
    if (hover != app->ui_hover) {
        app->ui_hover = hover;
        app->ui_dirty = true;
    }
    update_preview(app, app->scrub_hand >= 0 ? app->scrub_us : hover_us);
    if (pointer_visible != app->ui_pointer_visible ||
        (pointer_visible &&
         (pointer_x != app->ui_pointer_x ||
          pointer_y != app->ui_pointer_y))) {
        app->ui_pointer_visible = pointer_visible;
        app->ui_pointer_x = pointer_x;
        app->ui_pointer_y = pointer_y;
    }
}

static XrCompositionLayerQuad make_ui_layer(const struct app *app)
{
    return (XrCompositionLayerQuad){
        .type = XR_TYPE_COMPOSITION_LAYER_QUAD,
        .layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
        .space = app->space,
        .eyeVisibility = XR_EYE_VISIBILITY_BOTH,
        .subImage = {
            .swapchain = app->ui_swapchain,
            .imageRect = {.extent = {UI_WIDTH, UI_HEIGHT}},
        },
        .pose = {yaw_quaternion(app->panel_yaw),
                 {app->panel_x, app->panel_y, app->panel_z}},
        .size = {UI_PANEL_WIDTH_METERS, UI_PANEL_HEIGHT_METERS},
    };
}


/* ---- browser mode ---------------------------------------------------- */

#define BROWSER_PANEL_Z (-2.4f)

static void create_browser_texture(struct app *app)
{
    app->browser_swapchain = create_ui_texture(
        app, &app->browser_texture, BROWSER_UI_WIDTH, BROWSER_UI_HEIGHT,
        "xrCreateSwapchain(browser)");
    if (app->browser_swapchain) {
        need_xr("xrEnumerateSwapchainImages(browser count)",
                xrEnumerateSwapchainImages(app->browser_swapchain, 0,
                                           &app->browser_image_count, NULL));
        need(app->browser_image_count > 0 &&
                 app->browser_image_count <= MAX_IMAGES,
             "unexpected browser swapchain image count");
        for (uint32_t i = 0; i < app->browser_image_count; ++i)
            app->browser_images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
        need_xr("xrEnumerateSwapchainImages(browser list)",
                xrEnumerateSwapchainImages(
                    app->browser_swapchain, app->browser_image_count,
                    &app->browser_image_count,
                    (XrSwapchainImageBaseHeader *)app->browser_images));
    }
    VkCommandBufferAllocateInfo commands = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = app->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    need_vk("vkAllocateCommandBuffers(browser)",
            vkAllocateCommandBuffers(app->device, &commands,
                                     &app->browser_command));
    VkFenceCreateInfo fence = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    need_vk("vkCreateFence(browser)",
            vkCreateFence(app->device, &fence, NULL, &app->browser_fence));
    const VkDeviceSize bytes = (VkDeviceSize)BROWSER_UI_WIDTH *
                               BROWSER_UI_HEIGHT * 4U;
    VkBufferCreateInfo buffer = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    need_vk("vkCreateBuffer(browser staging)",
            vkCreateBuffer(app->device, &buffer, NULL,
                           &app->browser_staging_buffer));
    VkMemoryRequirements memory;
    vkGetBufferMemoryRequirements(app->device, app->browser_staging_buffer,
                                  &memory);
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memory.size,
        .memoryTypeIndex = memory_type(
            app->physical, memory.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    need_vk("vkAllocateMemory(browser staging)",
            vkAllocateMemory(app->device, &allocation, NULL,
                             &app->browser_staging_memory));
    need_vk("vkBindBufferMemory(browser staging)",
            vkBindBufferMemory(app->device, app->browser_staging_buffer,
                               app->browser_staging_memory, 0));
    need_vk("vkMapMemory(browser staging)",
            vkMapMemory(app->device, app->browser_staging_memory, 0, bytes, 0,
                        (void **)&app->browser_pixels));
    app->browser_cpu = calloc(1, (size_t)bytes);
    need(app->browser_cpu != NULL, "allocate browser pixels");
    app->browser.dirty = true;
}

/* Uploads `pixels` (width x height RGBA, canvas layout) into every image of
 * a swapchain whose content never changes. */
static void fill_static_swapchain(struct app *app, XrSwapchain swapchain,
                                  const uint8_t *pixels, uint32_t width,
                                  uint32_t height)
{
    uint32_t count = 0;
    XrSwapchainImageVulkan2KHR images[MAX_IMAGES];
    need_xr("xrEnumerateSwapchainImages(static count)",
            xrEnumerateSwapchainImages(swapchain, 0, &count, NULL));
    need(count > 0 && count <= MAX_IMAGES, "unexpected static image count");
    for (uint32_t i = 0; i < count; ++i)
        images[i] = (XrSwapchainImageVulkan2KHR){
            .type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR};
    need_xr("xrEnumerateSwapchainImages(static list)",
            xrEnumerateSwapchainImages(swapchain, count, &count,
                                       (XrSwapchainImageBaseHeader *)images));
    VkDeviceSize bytes = (VkDeviceSize)width * height * 4U;
    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer buffer;
    need_vk("vkCreateBuffer(static)",
            vkCreateBuffer(app->device, &buffer_info, NULL, &buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(app->device, buffer, &requirements);
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memory_type(
            app->physical, requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    VkDeviceMemory memory;
    need_vk("vkAllocateMemory(static)",
            vkAllocateMemory(app->device, &allocation, NULL, &memory));
    need_vk("vkBindBufferMemory(static)",
            vkBindBufferMemory(app->device, buffer, memory, 0));
    void *mapping;
    need_vk("vkMapMemory(static)",
            vkMapMemory(app->device, memory, 0, bytes, 0, &mapping));
    memcpy(mapping, pixels, (size_t)bytes);
    VkCommandBufferAllocateInfo commands = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = app->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer command;
    need_vk("vkAllocateCommandBuffers(static)",
            vkAllocateCommandBuffers(app->device, &commands, &command));
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    need_vk("vkCreateFence(static)",
            vkCreateFence(app->device, &fence_info, NULL, &fence));
    for (uint32_t n = 0; n < count; ++n) {
        XrSwapchainImageAcquireInfo acquire = {
            .type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        uint32_t index = 0;
        need_xr("xrAcquireSwapchainImage(static)",
                xrAcquireSwapchainImage(swapchain, &acquire, &index));
        XrSwapchainImageWaitInfo wait = {
            .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,
            .timeout = XR_INFINITE_DURATION};
        need_xr("xrWaitSwapchainImage(static)",
                xrWaitSwapchainImage(swapchain, &wait));
        VkCommandBufferBeginInfo begin = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        need_vk("vkBeginCommandBuffer(static)",
                vkBeginCommandBuffer(command, &begin));
        VkImageMemoryBarrier transfer = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .image = images[index].image,
            .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                 .levelCount = 1, .layerCount = 1},
        };
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                             NULL, 1, &transfer);
        VkBufferImageCopy copy = {
            .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                 .layerCount = 1},
            .imageExtent = {width, height, 1},
        };
        vkCmdCopyBufferToImage(command, buffer, images[index].image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        VkImageMemoryBarrier sample = transfer;
        sample.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        sample.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sample.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        sample.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL,
                             0, NULL, 1, &sample);
        need_vk("vkEndCommandBuffer(static)", vkEndCommandBuffer(command));
        VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                               .commandBufferCount = 1,
                               .pCommandBuffers = &command};
        need_vk("vkQueueSubmit(static)",
                vkQueueSubmit(app->queue, 1, &submit, fence));
        need_vk("vkWaitForFences(static)",
                vkWaitForFences(app->device, 1, &fence, VK_TRUE,
                                UINT64_C(5000000000)));
        need_vk("vkResetFences(static)",
                vkResetFences(app->device, 1, &fence));
        need_vk("vkResetCommandBuffer(static)",
                vkResetCommandBuffer(command, 0));
        XrSwapchainImageReleaseInfo release = {
            .type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        need_xr("xrReleaseSwapchainImage(static)",
                xrReleaseSwapchainImage(swapchain, &release));
    }
    vkDestroyFence(app->device, fence, NULL);
    vkFreeCommandBuffers(app->device, app->command_pool, 1, &command);
    vkUnmapMemory(app->device, memory);
    vkDestroyBuffer(app->device, buffer, NULL);
    vkFreeMemory(app->device, memory, NULL);
}

#define POINTER_PIXELS 64U

/* Pipeline for the panels drawn into the eye images: a textured quad,
 * blended over the video with premultiplied alpha like a quad layer. */
static void create_panel_pipeline(struct app *app)
{
    if (!app->panels_in_eye_images)
        return;
    VkSamplerCreateInfo sampler = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .anisotropyEnable = app->panel_anisotropy > 1.0f,
        .maxAnisotropy = app->panel_anisotropy,
        .maxLod = VK_LOD_CLAMP_NONE,
    };
    need_vk("vkCreateSampler(panel)",
            vkCreateSampler(app->device, &sampler, NULL,
                            &app->panel_sampler));
    VkDescriptorSetLayoutBinding binding = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .pImmutableSamplers = &app->panel_sampler,
    };
    VkDescriptorSetLayoutCreateInfo layout = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &binding,
    };
    need_vk("vkCreateDescriptorSetLayout(panel)",
            vkCreateDescriptorSetLayout(app->device, &layout, NULL,
                                        &app->panel_descriptor_layout));
    VkPushConstantRange push = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .size = sizeof(struct mat4),
    };
    VkPipelineLayoutCreateInfo pipeline_layout = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &app->panel_descriptor_layout,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &push,
    };
    need_vk("vkCreatePipelineLayout(panel)",
            vkCreatePipelineLayout(app->device, &pipeline_layout, NULL,
                                   &app->panel_pipeline_layout));
    VkDescriptorPoolSize size = {
        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 2,
    };
    VkDescriptorPoolCreateInfo pool = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 2, .poolSizeCount = 1, .pPoolSizes = &size,
    };
    need_vk("vkCreateDescriptorPool(panel)",
            vkCreateDescriptorPool(app->device, &pool, NULL,
                                   &app->panel_descriptor_pool));
    struct ui_texture *textures[2] = {&app->ui_texture, &app->browser_texture};
    for (size_t i = 0; i < ARRAY_COUNT(textures); ++i) {
        VkDescriptorSetAllocateInfo allocate = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = app->panel_descriptor_pool,
            .descriptorSetCount = 1,
            .pSetLayouts = &app->panel_descriptor_layout,
        };
        need_vk("vkAllocateDescriptorSets(panel)",
                vkAllocateDescriptorSets(app->device, &allocate,
                                         &textures[i]->descriptor_set));
        VkDescriptorImageInfo image = {
            .imageView = textures[i]->view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        VkWriteDescriptorSet write = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = textures[i]->descriptor_set, .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &image,
        };
        vkUpdateDescriptorSets(app->device, 1, &write, 0, NULL);
    }
    VkShaderModule vert = shader(app->device, "ui-panel.vert.spv");
    VkShaderModule frag = shader(app->device, "ui-panel.frag.spv");
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vert, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = frag,
         .pName = "main"},
    };
    VkPipelineVertexInputStateCreateInfo vertex = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
    };
    VkViewport viewport = {.width = (float)app->width,
                           .height = (float)app->height, .maxDepth = 1};
    VkRect2D scissor = {.extent = {app->width, app->height}};
    VkPipelineViewportStateCreateInfo view = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &viewport,
        .scissorCount = 1, .pScissors = &scissor,
    };
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1,
    };
    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineColorBlendAttachmentState blend_attachment = {
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &blend_attachment,
    };
    VkGraphicsPipelineCreateInfo pipeline = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vertex,
        .pInputAssemblyState = &assembly, .pViewportState = &view,
        .pRasterizationState = &raster, .pMultisampleState = &multisample,
        .pColorBlendState = &blend, .layout = app->panel_pipeline_layout,
        .renderPass = app->render_pass,
    };
    need_vk("vkCreateGraphicsPipelines(panel)",
            vkCreateGraphicsPipelines(app->device, VK_NULL_HANDLE, 1,
                                      &pipeline, NULL, &app->panel_pipeline));
    vkDestroyShaderModule(app->device, vert, NULL);
    vkDestroyShaderModule(app->device, frag, NULL);
}

/* Model matrix of a panel quad (unit square in the shader) with the pose
 * and size of its quad layer. */
static struct mat4 panel_model(const XrCompositionLayerQuad *quad)
{
    struct mat4 model = rotation_from_pose(quad->pose);
    for (unsigned row = 0; row < 3; ++row) {
        model.v[row] *= quad->size.width;
        model.v[4 + row] *= quad->size.height;
    }
    model.v[12] = quad->pose.position.x;
    model.v[13] = quad->pose.position.y;
    model.v[14] = quad->pose.position.z;
    return model;
}

static void create_pointer_texture(struct app *app)
{
    XrSwapchainCreateInfo info = {
        .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
        .usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                      XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT,
        .format = app->ui_format,
        .sampleCount = 1,
        .width = POINTER_PIXELS,
        .height = POINTER_PIXELS,
        .faceCount = 1,
        .arraySize = 1,
        .mipCount = 1,
    };
    need_xr("xrCreateSwapchain(pointer)",
            xrCreateSwapchain(app->session, &info, &app->pointer_swapchain));
    uint8_t pixels[POINTER_PIXELS * POINTER_PIXELS * 4] = {0};
    struct ui_canvas canvas = {pixels, POINTER_PIXELS, POINTER_PIXELS,
                               app->ui_bgra};
    const float c = POINTER_PIXELS / 2.0f;
    ui_fill_circle(&canvas, c, c, c - 2, UI_RGBA(0, 0, 0, 110));
    ui_fill_circle(&canvas, c, c, c - 8, UI_RGBA(255, 255, 255, 240));
    ui_fill_circle(&canvas, c, c, c - 17, UI_RGBA(56, 160, 255, 255));
    fill_static_swapchain(app, app->pointer_swapchain, pixels,
                          POINTER_PIXELS, POINTER_PIXELS);
}

static XrCompositionLayerQuad make_pointer_layer(const struct app *app)
{
    return (XrCompositionLayerQuad){
        .type = XR_TYPE_COMPOSITION_LAYER_QUAD,
        .layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
        .space = app->space,
        .eyeVisibility = XR_EYE_VISIBILITY_BOTH,
        .subImage = {
            .swapchain = app->pointer_swapchain,
            .imageRect = {.extent = {POINTER_PIXELS, POINTER_PIXELS}},
        },
        .pose = app->pointer_pose,
        .size = {app->pointer_size, app->pointer_size},
    };
}

static void upload_browser(struct app *app)
{
    /* Draw in cached memory: blending reads pixels back, which is very slow
     * on the uncached staging mapping. */
    browser_ui_render(&app->browser, app->browser_cpu, app->ui_bgra);
    XrSwapchainImageAcquireInfo acquire = {
        .type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO,
    };
    uint32_t index = 0;
    XrSwapchainImageWaitInfo wait = {
        .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,
        .timeout = XR_INFINITE_DURATION,
    };
    if (app->browser_swapchain) {
        need_xr("xrAcquireSwapchainImage(browser)",
                xrAcquireSwapchainImage(app->browser_swapchain, &acquire, &index));
        need_xr("xrWaitSwapchainImage(browser)",
                xrWaitSwapchainImage(app->browser_swapchain, &wait));
        need(index < app->browser_image_count, "invalid browser swapchain index");
    }
    need_vk("vkWaitForFences(browser)",
            vkWaitForFences(app->device, 1, &app->browser_fence, VK_TRUE,
                            UINT64_C(5000000000)));
    need_vk("vkResetFences(browser)",
            vkResetFences(app->device, 1, &app->browser_fence));
    memcpy(app->browser_pixels, app->browser_cpu,
           (size_t)BROWSER_UI_WIDTH * BROWSER_UI_HEIGHT * 4U);
    need_vk("vkResetCommandBuffer(browser)",
            vkResetCommandBuffer(app->browser_command, 0));
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    need_vk("vkBeginCommandBuffer(browser)",
            vkBeginCommandBuffer(app->browser_command, &begin));
    record_ui_upload(app->browser_command, &app->browser_texture,
                     app->browser_images[index].image,
                     app->browser_staging_buffer,
                     app->browser_image_initialized[index]);
    need_vk("vkEndCommandBuffer(browser)",
            vkEndCommandBuffer(app->browser_command));
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &app->browser_command,
    };
    /* SteamVR samples the image as soon as it is released, so the copy must
     * be complete by then (otherwise it shows stale or empty content). */
    need_vk("vkQueueSubmit(browser)",
            vkQueueSubmit(app->queue, 1, &submit, app->browser_fence));
    need_vk("vkWaitForFences(browser completion)",
            vkWaitForFences(app->device, 1, &app->browser_fence, VK_TRUE,
                            UINT64_C(5000000000)));
    app->browser_image_initialized[index] = true;
    XrSwapchainImageReleaseInfo release = {
        .type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO,
    };
    if (app->browser_swapchain)
        need_xr("xrReleaseSwapchainImage(browser)",
                xrReleaseSwapchainImage(app->browser_swapchain, &release));
}

static XrCompositionLayerQuad make_browser_layer(const struct app *app)
{
    return (XrCompositionLayerQuad){
        .type = XR_TYPE_COMPOSITION_LAYER_QUAD,
        .layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
        .space = app->space,
        .eyeVisibility = XR_EYE_VISIBILITY_BOTH,
        .subImage = {
            .swapchain = app->browser_swapchain,
            .imageRect = {.extent = {BROWSER_UI_WIDTH, BROWSER_UI_HEIGHT}},
        },
        .pose = {{0, 0, 0, 1}, {0, 0, BROWSER_PANEL_Z}},
        .size = {BROWSER_UI_PANEL_WIDTH_METERS,
                 BROWSER_UI_PANEL_HEIGHT_METERS},
    };
}

/* ---- launcher control socket ---------------------------------------- */

static void send_control(struct app *app, uint32_t type, const char *path,
                         int64_t start_us)
{
    struct player_ui_result result = {.type = type, .start_us = start_us};
    if (path)
        snprintf(result.path, sizeof(result.path), "%s", path);
    ssize_t written;
    do {
        written = send(app->ui_fd, &result, sizeof(result), MSG_NOSIGNAL);
    } while (written < 0 && errno == EINTR);
    if (written != (ssize_t)sizeof(result))
        app->quit = true; /* the launcher is gone */
}

/* Back to the browser: drop every video import and reset player state. */
static void end_playback(struct app *app)
{
    app->popup_open = false;
    need_vk("vkDeviceWaitIdle(end playback)", vkDeviceWaitIdle(app->device));
    discard_held_frame(app);
    for (uint32_t i = 0; i < app->video_buffer_count; ++i)
        destroy_video_buffer(app, &app->video_buffers[i]);
    if (app->ipc_fd >= 0)
        close(app->ipc_fd);
    app->ipc_fd = -1;
    app->playing = false;
    app->eos_received = false;
    app->current_video_index = -1;
    app->current_video_presented = false;
    app->awaiting_frame = true;
    app->seek_pending = false;
    app->grab_hand = -1;
    app->scrub_hand = -1;
    app->learn_action = -1;
    memset(app->input_held, 0, sizeof(app->input_held));
    memset(app->input_combined, 0, sizeof(app->input_combined));
    memset(app->input_repeat, 0, sizeof(app->input_repeat));
    app->grip_toggle[0] = app->grip_toggle[1] = false;
    app->ui_hover = 0;
    app->ui_pointer_visible = false;
    app->player_state = NV12_PLAYER_PLAYING;
    printf("openxr_summary xr_frames=%" PRIu64
           " rendered_frames=%" PRIu64 " received_frames=%" PRIu64
           " unique_video_frames=%" PRIu64
           " repeated_video_frames=%" PRIu64
           " hidden_video_frames=%" PRIu64
           " pipeline_skipped_frames=%" PRIu64
           " released_frames=%" PRIu64
           " session_changes=%" PRIu64 "\n",
           app->xr_frames, app->rendered_frames, app->received_frames,
           app->unique_video_frames, app->repeated_video_frames,
           app->hidden_video_frames, app->pipeline_skipped_frames,
           app->released_frames, app->session_changes);
    app->xr_frames = app->rendered_frames = app->received_frames = 0;
    app->unique_video_frames = app->repeated_video_frames = 0;
    app->hidden_video_frames = app->pipeline_skipped_frames = 0;
    app->released_frames = 0;
    app->position_us = 0;
    app->duration_us = 0;
    app->video_width = app->video_height = 0;
    /* The trigger that pressed "back" is usually still held: treat it as
     * held so the browser only reacts after a release (it could otherwise
     * land on "Beenden" and close the program). */
    app->select_down[0] = app->select_down[1] = true;
    trickplay_open(app->trickplay, NULL);
    app->details_valid = false;
    ui_image_free(app->preview);
    app->preview = NULL;
    app->preview_visible = false;
    app->ui_dirty = true;
    app->browser.dirty = true;
    printf("playback_ended\n");
}

/* `location` is what the session plays; "jf:item:ID[:PARENT]" has
 * trickplay previews. */
static void begin_playback(struct app *app, int fd, const char *location)
{
    if (app->playing)
        end_playback(app);
    snprintf(app->location, sizeof(app->location), "%s", location);
    app->title_finished = false;
    app->last_input_time = monotonic_seconds();
    app->auto_hidden = false;
    if (strncmp(location, "jf:item:", 8) == 0) {
        char item[64] = "";
        size_t length = strcspn(location + 8, ":");
        if (length < sizeof(item))
            memcpy(item, location + 8, length);
        item[length < sizeof(item) ? length : 0] = '\0';
        trickplay_open(app->trickplay, item);
    }
    if (location[0])
        title_settings_key(location, app->title_key, sizeof(app->title_key));
    else
        app->title_key[0] = '\0';
    load_adjust(app, false);
    /* Centre the dome on the head and the gaze direction at the first
     * rendered frame, same as "zentrieren": the video then faces wherever
     * the viewer is looking at playback start, regardless of where the
     * SteamVR seated origin happens to point. */
    app->recenter_pending = true;
    app->recenter_yaw = true;
    app->dome_yaw = 0.0f;
    app->autofocus_offset = 0.0f;
    app->autofocus_speed = 0.0f;
    app->autofocus_time = 0.0;
    app->autofocus_focused = false;
    app->autofocus_focus_disparity = app->autofocus_distance = 0.0f;
    app->ipc_fd = fd;
    app->playing = true;
    app->awaiting_command = false;
    app->eos_received = false;
    app->ui_dirty = true;
    app->select_down[0] = app->select_down[1] = true; /* see end_playback */
    printf("playback_attached\n");
}

static void handle_control(struct app *app)
{
    for (;;) {
        struct player_ui_command command = {0};
        char control[CMSG_SPACE(sizeof(int))] = {0};
        struct iovec iov = {.iov_base = &command, .iov_len = sizeof(command)};
        struct msghdr received = {
            .msg_iov = &iov, .msg_iovlen = 1,
            .msg_control = control, .msg_controllen = sizeof(control),
        };
        ssize_t bytes = recvmsg(app->ui_fd, &received, MSG_DONTWAIT);
        if (bytes < 0 && errno == EINTR)
            continue;
        if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        if (bytes <= 0) {
            app->quit = true; /* the launcher closed the socket */
            return;
        }
        if ((size_t)bytes != sizeof(command))
            continue;
        if (command.type == PLAYER_UI_PLAY) {
            struct cmsghdr *header = CMSG_FIRSTHDR(&received);
            if (header && header->cmsg_level == SOL_SOCKET &&
                header->cmsg_type == SCM_RIGHTS &&
                header->cmsg_len == CMSG_LEN(sizeof(int))) {
                int fd;
                memcpy(&fd, CMSG_DATA(header), sizeof(fd));
                command.path[sizeof(command.path) - 1] = '\0';
                begin_playback(app, fd, command.path);
            }
        } else if (command.type == PLAYER_UI_BROWSE) {
            bool finished = app->playing && app->title_finished;
            if (app->playing)
                end_playback(app);
            command.path[sizeof(command.path) - 1] = '\0';
            command.message[sizeof(command.message) - 1] = '\0';
            /* A title that played to its end may lead straight into the
             * next one (or itself) without showing the browser. */
            char next[PLAYER_UI_PATH_MAX];
            bool follow = false;
            if (app->skip_path[0] && !command.message[0]) {
                /* "Next/previous video" ended the session. */
                snprintf(next, sizeof(next), "%s", app->skip_path);
                snprintf(app->title, sizeof(app->title), "%s",
                         app->skip_title);
                follow = true;
            } else if (finished && !command.message[0] &&
                       app->end_action == PLAYER_END_REPEAT) {
                snprintf(next, sizeof(next), "%s", app->location);
                follow = next[0] != '\0';
            } else if (finished && !command.message[0] &&
                       app->end_action == PLAYER_END_NEXT) {
                follow = browser_ui_following_title(&app->browser,
                                                    app->location, 1, next,
                                                    sizeof(next));
                if (follow)
                    snprintf(app->title, sizeof(app->title), "%s",
                             app->browser.selected_title);
            }
            app->skip_path[0] = '\0';
            if (follow) {
                printf("player_end_action=%d next=%s\n", app->end_action,
                       next);
                app->awaiting_command = true;
                send_control(app, PLAYER_UI_FILE_SELECTED, next, 0);
                continue;
            }
            browser_ui_navigate(&app->browser, command.path, command.message);
            app->awaiting_command = false;
        }
    }
}

static void sync_browser_input(struct app *app, XrTime time)
{
    XrActiveActionSet active = {.actionSet = app->action_set};
    XrActionsSyncInfo sync = {
        .type = XR_TYPE_ACTIONS_SYNC_INFO,
        .countActiveActionSets = 1,
        .activeActionSets = &active,
    };
    need_xr("xrSyncActions(browser)", xrSyncActions(app->session, &sync));
    const XrSpaceLocationFlags valid =
        XR_SPACE_LOCATION_POSITION_VALID_BIT |
        XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    bool pointer_visible = false, pressed = false;
    app->pointer_layer_visible = false;
    int pointer_x = 0, pointer_y = 0, hover = -1;
    for (unsigned hand = 0; hand < 2; ++hand) {
        XrActionStateGetInfo pose_info = {
            .type = XR_TYPE_ACTION_STATE_GET_INFO,
            .action = app->aim_action,
            .subactionPath = app->hands[hand],
        };
        XrActionStatePose pose = {.type = XR_TYPE_ACTION_STATE_POSE};
        need_xr("xrGetActionStatePose(browser)",
                xrGetActionStatePose(app->session, &pose_info, &pose));
        struct hand_input input = {0};
        read_select(app, hand, &input);
        if (input.select_edge)
            pressed = true;
        int flick = read_stick(app, hand);
        if (flick)
            browser_ui_flip(&app->browser, flick);
        if (!pose.isActive)
            continue;
        XrSpaceLocation location = {.type = XR_TYPE_SPACE_LOCATION};
        need_xr("xrLocateSpace(browser aim)",
                xrLocateSpace(app->aim_spaces[hand], app->space, time,
                              &location));
        if ((location.locationFlags & valid) != valid)
            continue;
        struct panel_hit hit = panel_ray_at(
            (XrVector3f){0, 0, BROWSER_PANEL_Z}, 0.0f,
            BROWSER_UI_PANEL_WIDTH_METERS, BROWSER_UI_PANEL_HEIGHT_METERS,
            location.pose);
        if (panel_hit_inside(&hit)) {
            pointer_visible = true;
            pointer_x = (int)(hit.u * BROWSER_UI_WIDTH);
            pointer_y = (int)(hit.v * BROWSER_UI_HEIGHT);
            hover = browser_ui_hit_test(hit.u, hit.v);
            place_pointer(app, (XrVector3f){0, 0, BROWSER_PANEL_Z}, 0.0f,
                          BROWSER_UI_PANEL_WIDTH_METERS,
                          BROWSER_UI_PANEL_HEIGHT_METERS, hit, 0.04f);
        }
    }
    browser_ui_set_pointer(&app->browser, pointer_visible, pointer_x,
                           pointer_y, hover);
    if (!pressed || app->awaiting_command)
        return;
    char path[PLAYER_UI_PATH_MAX];
    switch (browser_ui_activate(&app->browser, path, sizeof(path))) {
    case BROWSER_UI_FILE_SELECTED:
        app->awaiting_command = true;
        snprintf(app->title, sizeof(app->title), "%s",
                 app->browser.selected_title);
        send_control(app, PLAYER_UI_FILE_SELECTED, path,
                     app->browser.selected_start_us);
        break;
    case BROWSER_UI_EXIT_REQUESTED:
        app->awaiting_command = true;
        send_control(app, PLAYER_UI_EXIT_REQUESTED, NULL, 0);
        app->quit = true;
        break;
    case BROWSER_UI_NONE:
        break;
    }
}

/* ---- autofocus -------------------------------------------------------
 * Close subjects in stereo video need strong convergence and easily split
 * into double images. The measurement finds the disparity of the picture
 * where the viewer looks; the eye pictures are then moved apart so that
 * near subjects keep at most the disparity of AUTOFOCUS_TARGET_DEGREES.
 * The focus is set to a subject distance and only changes when the
 * distance changes by more than the hysteresis; with the threshold on,
 * subjects farther than FOCUS_DISTANCE leave the video unchanged. The
 * offset moves to a new focus over the fade time. */
#define AUTOFOCUS_TARGET_DEGREES 3.6f /* about 1 m at 63 mm baseline */
#define AUTOFOCUS_BASELINE_METERS 0.063f
#define AUTOFOCUS_MAX_DEGREES 12.0f
#define AUTOFOCUS_MIN_SCORE 0.6f
#define AUTOFOCUS_MIN_VARIANCE 2e-4f
#define AUTOFOCUS_EVERY_FRAMES 3U

static double monotonic_seconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

/* Gaze ray (eye tracking) or, without it, the head's forward ray. */
static XrVector3f view_direction(struct app *app, XrPosef head, XrTime time,
                                 bool *gaze)
{
    *gaze = false;
    if (app->has_eye_gaze) {
        XrSpaceLocation location = {.type = XR_TYPE_SPACE_LOCATION};
        const XrSpaceLocationFlags tracked =
            XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
            XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
        if (XR_SUCCEEDED(xrLocateSpace(app->gaze_space, app->space, time,
                                       &location)) &&
            (location.locationFlags & tracked) == tracked) {
            *gaze = true;
            if (!app->gaze_logged) {
                printf("eye_gaze tracking=1\n");
                app->gaze_logged = true;
            }
            return rotate_vector(location.pose.orientation,
                                 (XrVector3f){0, 0, -1});
        }
    }
    return rotate_vector(head.orientation, (XrVector3f){0, 0, -1});
}

/* Measurement window in both eye pictures around where the viewer looks
 * (equirectangular projections only); false if nothing can be measured. */
static bool disparity_window(struct app *app, XrVector3f direction,
                             struct mat4 world_to_dome, XrVector3f eye,
                             float stereo_x, struct disparity_push *push)
{
    if (app->projection_mode == NV12_PROJECTION_FLAT ||
        app->projection_mode == NV12_PROJECTION_FISHEYE ||
        app->stereo_layout == NV12_STEREO_MONO)
        return false;
    XrVector3f d = transform_direction(world_to_dome, direction);
    float length = sqrtf(vector_dot(d, d));
    if (length <= 0.0f)
        return false;
    d.x /= length;
    d.y /= length;
    d.z /= length;
    float b = vector_dot(eye, d);
    float t = -b + sqrtf(b * b - vector_dot(eye, eye) + 1.0f);
    XrVector3f q = {eye.x + t * d.x, eye.y + t * d.y, eye.z + t * d.z};
    const float degrees = PI_F / 180.0f;
    bool full = app->projection_mode == NV12_PROJECTION_EQUIRECT_360;
    float span = full ? 2.0f * PI_F : PI_F;
    /* The left picture is shown moved by half the stereo offset. */
    float longitude = atan2f(q.x, -q.z) + 0.5f * stereo_x * degrees;
    float latitude = asinf(fmaxf(-1.0f, fminf(1.0f, q.y)));
    float stereo_y = app->adjust[PLAYER_ADJUST_STEREO_Y] * 0.5f * degrees;
    if (!full && fabsf(longitude) > 0.5f * PI_F - 3.0f * degrees)
        return false;
    float u = longitude / span + 0.5f;
    if (full)
        u -= floorf(u);
    float v_left = 0.5f - (latitude + stereo_y) / PI_F;
    float v_right = 0.5f - (latitude - stereo_y) / PI_F;
    bool sbs = app->stereo_layout == NV12_STEREO_SBS;
    float u_scale = sbs ? 0.5f : 1.0f, v_scale = sbs ? 1.0f : 0.5f;
    int left_eye = app->swap_eyes ? 1 : 0, right_eye = 1 - left_eye;
    float u_per_degree = degrees / span * u_scale;
    float v_per_degree = degrees / PI_F * v_scale;
    *push = (struct disparity_push){
        .left = {sbs ? 0.5f * left_eye + u * u_scale : u,
                 sbs ? v_left : 0.5f * left_eye + v_left * v_scale,
                 DISPARITY_WINDOW_DEGREES_X / 24.0f * u_per_degree,
                 DISPARITY_WINDOW_DEGREES_Y / 16.0f * v_per_degree},
        .right = {sbs ? 0.5f * right_eye + u * u_scale : u,
                  sbs ? v_right : 0.5f * right_eye + v_right * v_scale,
                  DISPARITY_STEP_DEGREES * u_per_degree,
                  (float)DISPARITY_ZERO},
        .bounds = {sbs ? 0.5f * right_eye : 0.0f,
                   sbs ? 0.5f * right_eye + 0.5f : 1.0f,
                   app->coded_width ? (float)app->video_width /
                                          (float)app->coded_width : 1.0f,
                   app->coded_height ? (float)app->video_height /
                                           (float)app->coded_height : 1.0f},
    };
    return true;
}

/* Subject distance of a disparity (degrees). */
static float focus_distance(float disparity)
{
    const float degrees = PI_F / 180.0f;
    return AUTOFOCUS_BASELINE_METERS /
           (2.0f * tanf(0.5f * fmaxf(disparity, 0.05f) * degrees));
}

/* Sets the focus to the subject at `disparity`, or none. */
static void set_focus(struct app *app, bool focused, float disparity)
{
    app->autofocus_focused = focused;
    app->autofocus_focus_disparity = focused ? disparity : 0.0f;
    app->autofocus_distance = focused ? focus_distance(disparity) : 0.0f;
}

/* Reads the last measurement and decides whether to refocus. */
static void update_autofocus(struct app *app)
{
    const struct disparity_result *result = app->disparity_mapping;
    double now = monotonic_seconds();
    int best = 0;
    for (int i = 1; i < DISPARITY_CANDIDATES; ++i)
        if (result->score[i] > result->score[best])
            best = i;
    float score = result->score[best];
    bool valid = best > 0 && best < DISPARITY_CANDIDATES - 1 &&
                 score >= AUTOFOCUS_MIN_SCORE &&
                 result->left_variance >= AUTOFOCUS_MIN_VARIANCE;
    app->autofocus_valid = valid;
    app->autofocus_score = score;
    app->autofocus_variance = result->left_variance;
    float disparity = 0.0f;
    if (valid) {
        /* Parabola through the peak and its neighbours. */
        float a = result->score[best - 1], c = result->score[best + 1];
        float curvature = a - 2.0f * score + c;
        float refine = curvature < 0.0f ? 0.5f * (a - c) / curvature : 0.0f;
        disparity = ((float)best + refine - (float)DISPARITY_ZERO) *
                    DISPARITY_STEP_DEGREES;
        app->autofocus_disparity = disparity;
        float distance = focus_distance(disparity);
        float limit = app->adjust[PLAYER_ADJUST_FOCUS_DISTANCE];
        float hysteresis = app->adjust[PLAYER_ADJUST_FOCUS_HYSTERESIS];
        if (!app->autofocus_focused) {
            if (!app->focus_threshold || distance <= limit)
                set_focus(app, true, disparity);
        } else if (app->focus_threshold &&
                   distance > limit * (1.0f + hysteresis)) {
            /* Released a little beyond the limit so that a subject right at
             * it does not switch back and forth. */
            set_focus(app, false, 0.0f);
        } else if (fabsf(distance - app->autofocus_distance) >
                   hysteresis * app->autofocus_distance) {
            set_focus(app, true, disparity);
        }
    }
    if (now - app->autofocus_log_time >= 1.0) {
        app->autofocus_log_time = now;
        printf("autofocus gaze=%d valid=%d disparity=%.2f score=%.2f"
               " variance=%.5f focus=%.2f offset=%.2f\n",
               app->autofocus_gaze, valid, disparity, score,
               result->left_variance, app->autofocus_distance,
               app->autofocus_offset);
    }
}

/* Moves the offset towards the current focus, once per frame. A critically
 * damped spring: it starts and ends smoothly, is about 95 % there after
 * the fade time, and a refocus on the way keeps its speed. */
static void advance_autofocus(struct app *app)
{
    double now = monotonic_seconds();
    float elapsed = app->autofocus_time > 0.0
                        ? (float)(now - app->autofocus_time)
                        : 0.0f;
    app->autofocus_time = now;
    float strength = app->adjust[PLAYER_ADJUST_AUTOFOCUS];
    if (strength <= 0.0f) {
        app->autofocus_offset = app->autofocus_speed = 0.0f;
        set_focus(app, false, 0.0f);
        return;
    }
    float target = fminf(strength * fmaxf(0.0f,
                                          app->autofocus_focus_disparity -
                                              AUTOFOCUS_TARGET_DEGREES),
                         AUTOFOCUS_MAX_DEGREES);
    float fade = app->adjust[PLAYER_ADJUST_FOCUS_FADE];
    if (fade <= 0.0f) {
        app->autofocus_offset = target;
        app->autofocus_speed = 0.0f;
        return;
    }
    float omega = 4.74f / fade; /* (1 + w t) e^(-w t) = 0.05 at w t = 4.74 */
    float away = app->autofocus_offset - target;
    float push = (app->autofocus_speed + omega * away) * elapsed;
    float decay = expf(-omega * elapsed);
    app->autofocus_offset = target + (away + push) * decay;
    app->autofocus_speed = (app->autofocus_speed - omega * push) * decay;
}

/* Gaze marker textures: the measurement window (4 x 3 degrees) with a
 * centre dot. */
#define GAZE_MARKER_WIDTH 128U
#define GAZE_MARKER_HEIGHT 96U
#define GAZE_MARKER_DISTANCE 1.5f

static void create_gaze_markers(struct app *app)
{
    static const struct ui_color colors[2] = {
        UI_RGBA(80, 230, 120, 255), UI_RGBA(255, 170, 50, 255),
    };
    for (int i = 0; i < 2; ++i) {
        XrSwapchainCreateInfo info = {
            .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
            .usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                          XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT,
            .format = app->ui_format, .sampleCount = 1,
            .width = GAZE_MARKER_WIDTH, .height = GAZE_MARKER_HEIGHT,
            .faceCount = 1, .arraySize = 1, .mipCount = 1,
        };
        need_xr("xrCreateSwapchain(gaze marker)",
                xrCreateSwapchain(app->session, &info,
                                  &app->gaze_marker[i]));
        static uint8_t pixels[GAZE_MARKER_WIDTH * GAZE_MARKER_HEIGHT * 4];
        memset(pixels, 0, sizeof(pixels));
        struct ui_canvas canvas = {pixels, GAZE_MARKER_WIDTH,
                                   GAZE_MARKER_HEIGHT, app->ui_bgra};
        ui_stroke_round_rect(&canvas, 3, 3, GAZE_MARKER_WIDTH - 6,
                             GAZE_MARKER_HEIGHT - 6, 8, 4, colors[i]);
        ui_fill_circle(&canvas, GAZE_MARKER_WIDTH / 2.0f,
                       GAZE_MARKER_HEIGHT / 2.0f, 6, UI_RGBA(0, 0, 0, 140));
        ui_fill_circle(&canvas, GAZE_MARKER_WIDTH / 2.0f,
                       GAZE_MARKER_HEIGHT / 2.0f, 4, colors[i]);
        fill_static_swapchain(app, app->gaze_marker[i], pixels,
                              GAZE_MARKER_WIDTH, GAZE_MARKER_HEIGHT);
    }
}

static XrCompositionLayerQuad make_gaze_marker_layer(const struct app *app)
{
    const float degrees = PI_F / 180.0f;
    return (XrCompositionLayerQuad){
        .type = XR_TYPE_COMPOSITION_LAYER_QUAD,
        .layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
        .space = app->space,
        .eyeVisibility = XR_EYE_VISIBILITY_BOTH,
        .subImage = {
            .swapchain = app->gaze_marker[app->autofocus_gaze ? 0 : 1],
            .imageRect = {.extent = {GAZE_MARKER_WIDTH, GAZE_MARKER_HEIGHT}},
        },
        .pose = app->gaze_marker_pose,
        .size = {2.0f * GAZE_MARKER_DISTANCE *
                     tanf(0.5f * DISPARITY_WINDOW_DEGREES_X * degrees),
                 2.0f * GAZE_MARKER_DISTANCE *
                     tanf(0.5f * DISPARITY_WINDOW_DEGREES_Y * degrees)},
    };
}

/* Debug line on the panel, refreshed four times a second. */
static void update_debug_text(struct app *app)
{
    double now = monotonic_seconds();
    if (!app->gaze_debug || now - app->debug_text_time < 0.25)
        return;
    app->debug_text_time = now;
    char text[sizeof(app->debug_text)];
    const char *source = app->autofocus_gaze ? "Auge"
                         : app->has_eye_gaze ? "Kopf (kein Blick)"
                                             : "Kopf (kein Eye Tracking)";
    char focus[24] = "aus";
    if (app->autofocus_focused)
        snprintf(focus, sizeof(focus), "%.2f m", app->autofocus_distance);
    if (app->projection_mode == NV12_PROJECTION_FLAT ||
        app->projection_mode == NV12_PROJECTION_FISHEYE ||
        app->stereo_layout == NV12_STEREO_MONO)
        snprintf(text, sizeof(text), "Blick: %s · Messung nur bei 180/360 "
                 "mit SBS/TB", source);
    else
        snprintf(text, sizeof(text),
                 "Blick: %s · Disparität %+.2f° · Score %.2f · Struktur %.4f"
                 " · %s · Fokus %s · Versatz %.2f°",
                 source, app->autofocus_disparity, app->autofocus_score,
                 app->autofocus_variance,
                 app->autofocus_valid ? "gültig" : "verworfen", focus,
                 app->autofocus_offset);
    if (strcmp(text, app->debug_text) != 0) {
        snprintf(app->debug_text, sizeof(app->debug_text), "%s", text);
        app->ui_dirty = true;
    }
}

static void render_frame(struct app *app)
{
    XrFrameState frame_state = {.type = XR_TYPE_FRAME_STATE};
    XrFrameWaitInfo wait = {.type = XR_TYPE_FRAME_WAIT_INFO};
    need_xr("xrWaitFrame", xrWaitFrame(app->session, &wait, &frame_state));
    XrFrameBeginInfo begin = {.type = XR_TYPE_FRAME_BEGIN_INFO};
    need_xr("xrBeginFrame", xrBeginFrame(app->session, &begin));
    ++app->xr_frames;
    app->xr_should_render = frame_state.shouldRender;
    if (app->playing) {
        sync_player_input(app, frame_state.predictedDisplayTime);
        if (app->ui_dirty)
            upload_ui(app);
    } else {
        sync_browser_input(app, frame_state.predictedDisplayTime);
        if (app->browser.dirty)
            upload_browser(app);
    }
    if (!frame_state.shouldRender) {
        XrFrameEndInfo end = {.type = XR_TYPE_FRAME_END_INFO, .displayTime = frame_state.predictedDisplayTime,
            .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE};
        need_xr("xrEndFrame(empty)", xrEndFrame(app->session, &end));
        return;
    }
    /* Without a decoded frame (browser, start, seek preroll) only the clear
     * colour and the panel are shown, so the headset never falls back to the
     * Steam environment. */
    struct video_buffer *video = NULL;
    if (app->playing && app->current_video_index >= 0)
        video = &app->video_buffers[app->current_video_index];
    else if (app->playing && app->has_held)
        video = &app->held;
    XrView views[2] = {{.type = XR_TYPE_VIEW}, {.type = XR_TYPE_VIEW}};
    XrViewState view_state = {.type = XR_TYPE_VIEW_STATE};
    uint32_t count = 0;
    XrViewLocateInfo locate = {.type = XR_TYPE_VIEW_LOCATE_INFO,
        .viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
        .displayTime = frame_state.predictedDisplayTime, .space = app->space};
    need_xr("xrLocateViews", xrLocateViews(app->session, &locate, &view_state, 2, &count, views));
    need(count == 2 && (view_state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
             (view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT), "OpenXR returned no valid stereo pose");
    XrSpaceLocation center = {.type = XR_TYPE_SPACE_LOCATION};
    need_xr("xrLocateSpace(VIEW)",
            xrLocateSpace(app->view_space, app->space,
                          frame_state.predictedDisplayTime, &center));
    need((center.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0,
         "OpenXR returned no valid VIEW orientation");
    if (!app->view_geometry_logged) {
        printf("openxr_view_geometry"
               " left_pose=(%.9f,%.9f,%.9f;%.9f,%.9f,%.9f,%.9f)"
               " left_fov=(%.9f,%.9f,%.9f,%.9f)"
               " right_pose=(%.9f,%.9f,%.9f;%.9f,%.9f,%.9f,%.9f)"
               " right_fov=(%.9f,%.9f,%.9f,%.9f)"
               " center_pose=(%.9f,%.9f,%.9f;%.9f,%.9f,%.9f,%.9f)\n",
               views[0].pose.position.x, views[0].pose.position.y,
               views[0].pose.position.z, views[0].pose.orientation.x,
               views[0].pose.orientation.y, views[0].pose.orientation.z,
               views[0].pose.orientation.w, views[0].fov.angleLeft,
               views[0].fov.angleRight, views[0].fov.angleDown,
               views[0].fov.angleUp, views[1].pose.position.x,
               views[1].pose.position.y, views[1].pose.position.z,
               views[1].pose.orientation.x, views[1].pose.orientation.y,
               views[1].pose.orientation.z, views[1].pose.orientation.w,
               views[1].fov.angleLeft, views[1].fov.angleRight,
               views[1].fov.angleDown, views[1].fov.angleUp,
               center.pose.position.x, center.pose.position.y,
               center.pose.position.z, center.pose.orientation.x,
               center.pose.orientation.y, center.pose.orientation.z,
               center.pose.orientation.w);
        app->view_geometry_logged = true;
    }
    XrSwapchainImageAcquireInfo acquire = {.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    uint32_t image_index = 0;
    need_xr("xrAcquireSwapchainImage", xrAcquireSwapchainImage(app->swapchain, &acquire, &image_index));
    XrSwapchainImageWaitInfo image_wait = {.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = XR_INFINITE_DURATION};
    need_xr("xrWaitSwapchainImage", xrWaitSwapchainImage(app->swapchain, &image_wait));
    need(image_index < app->image_count, "invalid OpenXR swapchain index");
    /* Dome placement: centred on the head of the first frame (or of
     * "zentrieren"), turned by the gaze yaw and the viewer's rotation.
     * world_to_dome undoes yaw, then tilt, then roll. */
    if (app->recenter_pending &&
        (center.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
        app->dome_center = center.pose.position;
        if (app->recenter_yaw) {
            XrVector3f gaze = rotate_vector(center.pose.orientation,
                                            (XrVector3f){0, 0, -1});
            app->dome_yaw = atan2f(gaze.x, -gaze.z);
        }
        app->recenter_pending = false;
        printf("player_recenter x=%.3f y=%.3f z=%.3f yaw=%.1f\n",
               app->dome_center.x, app->dome_center.y, app->dome_center.z,
               app->dome_yaw * 180.0f / PI_F);
    }
    const float degrees = PI_F / 180.0f;
    float yaw = app->dome_yaw + app->adjust[PLAYER_ADJUST_YAW] * degrees;
    float tilt = app->adjust[PLAYER_ADJUST_TILT] * degrees;
    float roll = app->adjust[PLAYER_ADJUST_ROLL] * degrees;
    struct mat4 world_to_dome = multiply(
        rotation_axis(2, roll),
        multiply(rotation_axis(0, -tilt), rotation_axis(1, yaw)));
    struct mat4 dome_to_world = multiply(
        rotation_axis(1, -yaw),
        multiply(rotation_axis(0, tilt), rotation_axis(2, -roll)));
    /* Eye inside the dome (radius 1 m): head motion, then zoom, height and
     * slide along the dome axes. */
    float motion = app->adjust[PLAYER_ADJUST_HEAD_MOTION];
    XrVector3f head = {
        (center.pose.position.x - app->dome_center.x) * motion,
        (center.pose.position.y - app->dome_center.y) * motion,
        (center.pose.position.z - app->dome_center.z) * motion,
    };
    XrVector3f eye_position = transform_direction(world_to_dome, head);
    eye_position.x -= app->adjust[PLAYER_ADJUST_SLIDE];
    eye_position.y -= app->adjust[PLAYER_ADJUST_HEIGHT];
    eye_position.z -= app->adjust[PLAYER_ADJUST_ZOOM];
    float eye_distance = sqrtf(vector_dot(eye_position, eye_position));
    if (eye_distance > 0.9f) {
        eye_position.x *= 0.9f / eye_distance;
        eye_position.y *= 0.9f / eye_distance;
        eye_position.z *= 0.9f / eye_distance;
    }
    /* Flat screen: 2 m wide, 2 m ahead of the dome centre, moved and
     * turned like the dome. */
    float eye_width = (float)app->video_width /
                      (app->stereo_layout == NV12_STEREO_SBS ? 2.0f : 1.0f);
    float eye_height = (float)app->video_height /
                       (app->stereo_layout == NV12_STEREO_TB ? 2.0f : 1.0f);
    float screen_height = eye_width > 0.0f && eye_height > 0.0f
                              ? 2.0f * eye_height / eye_width : 1.0f;
    XrVector3f screen = transform_direction(
        dome_to_world,
        (XrVector3f){app->adjust[PLAYER_ADJUST_SLIDE],
                     app->adjust[PLAYER_ADJUST_HEIGHT],
                     -(2.0f - app->adjust[PLAYER_ADJUST_ZOOM])});
    struct mat4 screen_model = dome_to_world;
    screen_model.v[12] = app->dome_center.x + screen.x;
    screen_model.v[13] = app->dome_center.y + screen.y;
    screen_model.v[14] = app->dome_center.z + screen.z;
    struct mat4 screen_scale = {{2.0f, 0, 0, 0, 0, screen_height, 0, 0,
                                 0, 0, 1, 0, 0, 0, 0, 1}};
    screen_model = multiply(screen_model, screen_scale);
    /* Autofocus: measure every few frames where the viewer looks. */
    advance_autofocus(app);
    bool gaze = false;
    XrVector3f look = view_direction(app, center.pose,
                                     frame_state.predictedDisplayTime, &gaze);
    app->autofocus_gaze = gaze;
    app->gaze_marker_visible = app->gaze_debug && video;
    if (app->gaze_marker_visible) {
        app->gaze_marker_pose = (XrPosef){
            center.pose.orientation,
            {center.pose.position.x + look.x * GAZE_MARKER_DISTANCE,
             center.pose.position.y + look.y * GAZE_MARKER_DISTANCE,
             center.pose.position.z + look.z * GAZE_MARKER_DISTANCE}};
    }
    update_debug_text(app);
    struct disparity_push disparity_push;
    bool measure =
        video &&
        (app->adjust[PLAYER_ADJUST_AUTOFOCUS] > 0.0f || app->gaze_debug) &&
        ++app->disparity_frames % AUTOFOCUS_EVERY_FRAMES == 0U &&
        disparity_window(app, look, world_to_dome, eye_position,
                         app->adjust[PLAYER_ADJUST_STEREO_X] +
                             app->autofocus_offset,
                         &disparity_push);
    for (uint32_t eye = 0; eye < 2; ++eye) {
        struct mat4 projection = projection_from_fov(views[eye].fov);
        struct mat4 view = view_from_pose(views[eye].pose);
        float side = eye == 0 ? -1.0f : 1.0f;
        float stereo_x = (app->adjust[PLAYER_ADJUST_STEREO_X] +
                          app->autofocus_offset) * 0.5f;
        float stereo_y = app->adjust[PLAYER_ADJUST_STEREO_Y] * 0.5f;
        struct view_uniform uniform = {
            .mvp = multiply(projection, multiply(view, screen_model)),
            .camera_to_world = multiply(world_to_dome,
                                        rotation_from_pose(views[eye].pose)),
            /* w: half the fisheye lens angle (radians). */
            .eye_offset = {eye_position.x, eye_position.y, eye_position.z,
                           0.5f * (float)app->fisheye_fov * degrees},
            .stereo_offset = {-side * stereo_x * degrees,
                              -side * stereo_y * degrees,
                              -side * stereo_x / 60.0f,
                              side * stereo_y / 60.0f},
            .picture = {app->adjust[PLAYER_ADJUST_BRIGHTNESS],
                        app->adjust[PLAYER_ADJUST_CONTRAST],
                        app->adjust[PLAYER_ADJUST_SATURATION],
                        app->adjust[PLAYER_ADJUST_GAMMA]},
            .fov_tangents = {
                tanf(views[eye].fov.angleLeft),
                tanf(views[eye].fov.angleRight),
                tanf(views[eye].fov.angleDown),
                tanf(views[eye].fov.angleUp),
            },
            .modes = {
                (int32_t)app->projection_mode,
                (int32_t)app->stereo_layout,
                (int32_t)eye,
                app->swap_eyes,
            },
            .source_info = {
                (float)app->video_width,
                (float)app->video_height,
                app->video_width ? 1.0f / (float)app->video_width : 0.0f,
                app->video_height ? 1.0f / (float)app->video_height : 0.0f,
            },
            .video_scale = {
                app->coded_width ? (float)app->video_width /
                                       (float)app->coded_width : 1.0f,
                app->coded_height ? (float)app->video_height /
                                        (float)app->coded_height : 1.0f,
                0.0f, 0.0f,
            },
        };
        video_levels_uniform(app->video_levels, uniform.levels);
        memcpy((uint8_t *)app->uniform_mapping + eye * UBO_STRIDE,
               &uniform, sizeof(uniform));
    }
    need_vk("vkWaitForFences(render)", vkWaitForFences(app->device, 1, &app->fence, VK_TRUE, UINT64_C(5000000000)));
    need_vk("vkResetFences(render)", vkResetFences(app->device, 1, &app->fence));
    if (app->disparity_pending) {
        app->disparity_pending = false;
        update_autofocus(app);
    }
    need_vk("vkResetCommandPool(render)", vkResetCommandPool(app->device, app->command_pool, 0));
    VkCommandBufferBeginInfo command_begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    need_vk("vkBeginCommandBuffer(render)", vkBeginCommandBuffer(app->command, &command_begin));
    VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .image = app->images[image_index].image, .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 2}};
    vkCmdPipelineBarrier(app->command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
    VkImageMemoryBarrier video_acquire = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
        .dstQueueFamilyIndex = app->queue_family,
        .image = video ? video->image : VK_NULL_HANDLE,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .levelCount = 1, .layerCount = 1},
    };
    if (video && video->host) {
        /* Own images, not shared with a foreign producer. */
        video_acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        video_acquire.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        if (video->producer_wait_pending) {
            record_host_upload(app, video);
            video->producer_wait_pending = false;
        }
    }
    VkImageMemoryBarrier video_acquires[2] = {video_acquire, video_acquire};
    uint32_t video_images = video && video->tp10 ? 2U : 1U;
    if (video_images == 2U)
        video_acquires[1].image = video->image_uv;
    if (video)
        vkCmdPipelineBarrier(app->command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, NULL, 0, NULL, video_images,
                             video_acquires);
    if (measure) {
        vkCmdBindPipeline(app->command, VK_PIPELINE_BIND_POINT_COMPUTE,
                          video->tp10 ? app->disparity_pipeline_tp10
                                      : app->disparity_pipeline);
        uint32_t offset = 0;
        vkCmdBindDescriptorSets(app->command, VK_PIPELINE_BIND_POINT_COMPUTE,
                                app->pipeline_layout, 0, 1,
                                &video->descriptor_set, 1, &offset);
        vkCmdPushConstants(app->command, app->pipeline_layout,
                           VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(disparity_push), &disparity_push);
        vkCmdDispatch(app->command, 1, 1, 1);
        VkMemoryBarrier to_host = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        };
        vkCmdPipelineBarrier(app->command,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &to_host, 0,
                             NULL, 0, NULL);
        app->disparity_pending = true;
    }
    XrCompositionLayerQuad ui_layer = app->playing
                                          ? make_ui_layer(app)
                                          : make_browser_layer(app);
    bool show_ui = app->playing ? app->ui_visible : true;
    const struct ui_texture *panel = NULL;
    if (app->panels_in_eye_images && show_ui)
        panel = app->playing ? &app->ui_texture : &app->browser_texture;
    struct mat4 panel_matrix = panel_model(&ui_layer);
    for (uint32_t eye = 0; eye < 2; ++eye) {
        VkClearValue clear = {.color = {{0.005f, 0.005f, 0.008f, 1.0f}}};
        VkRenderPassBeginInfo pass = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = app->render_pass, .framebuffer = app->framebuffers[image_index][eye],
            .renderArea = {.extent = {app->width, app->height}}, .clearValueCount = 1, .pClearValues = &clear};
        vkCmdBeginRenderPass(app->command, &pass, VK_SUBPASS_CONTENTS_INLINE);
        if (video) {
            vkCmdBindPipeline(app->command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              video->tp10 ? app->pipeline_tp10 : app->pipeline);
            uint32_t offset = eye * UBO_STRIDE;
            vkCmdBindDescriptorSets(app->command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    app->pipeline_layout, 0, 1,
                                    &video->descriptor_set, 1, &offset);
            vkCmdDraw(app->command, 4, 1, 0, 0);
        }
        if (panel && panel->ready) {
            struct mat4 mvp = multiply(
                projection_from_fov(views[eye].fov),
                multiply(view_from_pose(views[eye].pose), panel_matrix));
            vkCmdBindPipeline(app->command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              app->panel_pipeline);
            vkCmdBindDescriptorSets(app->command,
                                    VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    app->panel_pipeline_layout, 0, 1,
                                    &panel->descriptor_set, 0, NULL);
            vkCmdPushConstants(app->command, app->panel_pipeline_layout,
                               VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(mvp),
                               &mvp);
            vkCmdDraw(app->command, 4, 1, 0, 0);
        }
        vkCmdEndRenderPass(app->command);
    }
    VkImageMemoryBarrier video_release = video_acquire;
    video_release.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    video_release.dstAccessMask = 0;
    if (!video || !video->host) {
        video_release.srcQueueFamilyIndex = app->queue_family;
        video_release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
    }
    VkImageMemoryBarrier video_releases[2] = {video_release, video_release};
    if (video_images == 2U)
        video_releases[1].image = video->image_uv;
    if (video)
        vkCmdPipelineBarrier(app->command,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL,
                             0, NULL, video_images, video_releases);
    need_vk("vkEndCommandBuffer(render)", vkEndCommandBuffer(app->command));
    VkPipelineStageFlags producer_stage =
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &app->command};
    if (video && video->producer_wait_pending) {
        struct dma_buf_export_sync_file sync_export = {
            .flags = DMA_BUF_SYNC_READ, .fd = -1,
        };
        if (ioctl(video->dma_buf_fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE,
                  &sync_export) < 0) {
            perror("DMA_BUF_IOCTL_EXPORT_SYNC_FILE");
            exit(2);
        }
        VkImportSemaphoreFdInfoKHR semaphore_import = {
            .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
            .semaphore = video->producer_semaphore,
            .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
            .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
            .fd = sync_export.fd,
        };
        need_vk("vkImportSemaphoreFdKHR(producer)",
                app->import_semaphore_fd(app->device, &semaphore_import));
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &video->producer_semaphore;
        submit.pWaitDstStageMask = &producer_stage;
        video->producer_wait_pending = false;
    }
    need_vk("vkQueueSubmit(render)", vkQueueSubmit(app->queue, 1, &submit, app->fence));
    need_vk("vkWaitForFences(render completion)", vkWaitForFences(app->device, 1, &app->fence, VK_TRUE, UINT64_C(5000000000)));
    XrSwapchainImageReleaseInfo release = {.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    need_xr("xrReleaseSwapchainImage", xrReleaseSwapchainImage(app->swapchain, &release));
    XrCompositionLayerProjectionView projection_views[2] = {{.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    for (uint32_t eye = 0; eye < 2; ++eye) {
        projection_views[eye].pose = views[eye].pose; projection_views[eye].fov = views[eye].fov;
        projection_views[eye].subImage = (XrSwapchainSubImage){.swapchain = app->swapchain, .imageRect = {.extent = {(int32_t)app->width, (int32_t)app->height}}, .imageArrayIndex = eye};
    }
    XrCompositionLayerProjection layer = {.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION, .space = app->space, .viewCount = 2, .views = projection_views};
    XrCompositionLayerQuad pointer_layer = make_pointer_layer(app);
    XrCompositionLayerQuad gaze_layer = make_gaze_marker_layer(app);
    const XrCompositionLayerBaseHeader *layers[4] = {
        (const XrCompositionLayerBaseHeader *)&layer,
    };
    uint32_t layer_count = 1;
    if (app->playing && app->gaze_marker_visible)
        layers[layer_count++] = (const XrCompositionLayerBaseHeader *)&gaze_layer;
    if (show_ui) {
        if (!app->panels_in_eye_images)
            layers[layer_count++] =
                (const XrCompositionLayerBaseHeader *)&ui_layer;
        if (app->pointer_layer_visible)
            layers[layer_count++] =
                (const XrCompositionLayerBaseHeader *)&pointer_layer;
    }
    XrFrameEndInfo end = {.type = XR_TYPE_FRAME_END_INFO, .displayTime = frame_state.predictedDisplayTime,
        .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
        .layerCount = layer_count,
        .layers = layers};
    need_xr("xrEndFrame", xrEndFrame(app->session, &end));
    ++app->rendered_frames;
    if (!video)
        return;
    if (app->current_video_presented)
        ++app->repeated_video_frames;
    else {
        ++app->unique_video_frames;
        app->current_video_presented = true;
    }
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc != 2) {
        fprintf(stderr, "usage: %s UI_FD\n", argv[0]);
        return 2;
    }
    struct app app = {0};
    app.ui_fd = (int)parse_u64(argv[1], "UI fd");
    app.ipc_fd = -1;
    app.video_buffer_count = VIDEO_BUFFER_COUNT;
    for (uint32_t i = 0; i < NV12_IPC_MAX_BUFFERS; ++i)
        app.video_buffers[i].dma_buf_fd = -1;
    app.held.dma_buf_fd = -1;
    app.current_video_index = -1;
    app.player_state = NV12_PLAYER_PLAYING;
    app.ui_visible = true;
    app.ui_dirty = true;
    app.panel_y = -0.35f;
    app.panel_z = -1.3f;
    app.grab_hand = -1;
    app.scrub_hand = -1;
    app.learn_action = -1;
    app.awaiting_frame = true;
    char language[16];
    i18n_load(settings_get_string("language", language, sizeof(language))
                  ? language
                  : "eng");
    char browser_error[256] = {0};
    if (browser_ui_open(&app.browser, "root:", NULL, browser_error,
                        sizeof(browser_error)) < 0) {
        fprintf(stderr, "browser_error=%s\n", browser_error);
        return 2;
    }
    app.trickplay = trickplay_create();
    load_preview_settings(&app);
    load_adjust(&app, true);
    initialize_xr_vulkan(&app);
    XrGraphicsBindingVulkan2KHR binding = {.type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR,
        .instance = app.instance, .physicalDevice = app.physical, .device = app.device, .queueFamilyIndex = app.queue_family, .queueIndex = 0};
    XrSessionCreateInfo session_info = {.type = XR_TYPE_SESSION_CREATE_INFO, .next = &binding, .systemId = app.system};
    need_xr("xrCreateSession", xrCreateSession(app.xr, &session_info, &app.session));
    initialize_actions(&app);
    /* SteamVR on Frame requires READY/xrBeginSession before it permits a
     * reference space or swapchain to be created. */
    struct timespec pause = {.tv_nsec = 10000000};
    struct timespec started, now;
    clock_gettime(CLOCK_MONOTONIC, &started);
    while (!app.running) {
        poll_events(&app);
        clock_gettime(CLOCK_MONOTONIC, &now);
        need(now.tv_sec - started.tv_sec < 10,
             "OpenXR session did not enter READY within ten seconds");
        if (!app.running) nanosleep(&pause, NULL);
    }
    XrReferenceSpaceCreateInfo space_info = {.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO, .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL, .poseInReferenceSpace = {{0,0,0,1},{0,0,0}}};
    need_xr("xrCreateReferenceSpace", xrCreateReferenceSpace(app.session, &space_info, &app.space));
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    need_xr("xrCreateReferenceSpace(VIEW)",
            xrCreateReferenceSpace(app.session, &space_info, &app.view_space));
    initialize_swapchain(&app);
    app.panels_in_eye_images = panels_in_eye_images();
    create_ui_swapchain(&app);
    create_video_sampler(&app);
    create_render_resources(&app);
    create_ui_resources(&app);
    create_browser_texture(&app);
    create_panel_pipeline(&app);
    create_pointer_texture(&app);
    create_gaze_markers(&app);
    printf("host_ready\n");
    while (!app.quit) {
        handle_control(&app);
        if (app.playing) {
            receive_frame_messages(&app);
            struct ui_image *preview = NULL;
            if (trickplay_poll(app.trickplay, &preview)) {
                ui_image_free(app.preview);
                app.preview = preview;
                if (app.preview_visible)
                    app.ui_dirty = true;
            }
            if (trickplay_details(app.trickplay, &app.details)) {
                app.details_valid = true;
                app.ui_dirty = true;
            }
        }
        browser_ui_update(&app.browser);
        poll_events(&app);
        hide_dashboard_at_startup(&app);
        if (app.quit)
            break;
        if (app.running) render_frame(&app); else nanosleep(&pause, NULL);
        if (app.playing && app.eos_received)
            end_playback(&app);
    }
    if (app.xr_exit)
        send_control(&app, PLAYER_UI_EXIT_REQUESTED, NULL, 0);
    if (app.playing)
        end_playback(&app);
    browser_ui_close(&app.browser);
    player_ui_close(&app.player_ui);
    trickplay_destroy(app.trickplay);
    ui_image_free(app.preview);
    return 0;
}
