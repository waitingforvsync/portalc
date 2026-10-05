// main.c - the windowed app: fixed-rate camera ticks from WASD + cursor keys,
// a rendered MODE 5 frame expanded to RGBA and uploaded to a texture, and a
// 2:1 letterboxed quad drawn through richc's gfx layer.

#include <stdbool.h>
#include <stddef.h>

#include "arith.h"
#include "camera.h"
#include "map.h"
#include "mode5.h"
#include "render.h"

#include "richc/app/app.h"
#include "richc/app/keys.h"
#include "richc/gfx/bindings.h"
#include "richc/gfx/buffer.h"
#include "richc/gfx/encoder.h"
#include "richc/gfx/gfx.h"
#include "richc/gfx/pass.h"
#include "richc/gfx/pipeline.h"
#include "richc/gfx/shader.h"
#include "richc/gfx/texture.h"

#define TICK_SECONDS (1.0 / 50.0)

// The window: the full BBC display frame, opened at an integer scale.
#define PT_WINDOW_WIDTH 320
#define PT_WINDOW_HEIGHT 256
#define PT_WINDOW_SCALE 3

typedef struct vertex {
    rc_vec2f pos;
    rc_vec2f uv;
} vertex;

// a strip quad filling clip space; v = 0 at the top scanline
static const vertex vertices[4] = {
    {.pos = {-1.0f, 1.0f}, .uv = {0.0f, 0.0f}},
    {.pos = {-1.0f, -1.0f}, .uv = {0.0f, 1.0f}},
    {.pos = {1.0f, 1.0f}, .uv = {1.0f, 0.0f}},
    {.pos = {1.0f, -1.0f}, .uv = {1.0f, 1.0f}},
};

static const char vs_src[] =
    "layout(location = 0) in vec2 a_pos;\n"
    "layout(location = 1) in vec2 a_uv;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    v_uv = a_uv;\n"
    "    gl_Position = rc_clip(vec4(a_pos, 0.0, 1.0));\n"
    "}\n";

static const char fs_src[] =
    "in vec2 v_uv;\n"
    "uniform sampler2D u_screen;\n"
    "out vec4 o_color;\n"
    "void main() {\n"
    "    o_color = texture(u_screen, v_uv);\n"
    "}\n";

static struct {
    rc_arena arena;
    rc_arena frame_arena;

    pt_map map;
    pt_render render;
    pt_camera camera;
    pt_vcache vcache;    // the camera's and the renderer's, shared
    uint8_t rgba[PT_SCREEN_WIDTH * PT_SCREEN_SCANLINES * 4];
    double tick_acc;
    bool keys[512];
    bool quit;

    rc_gfx_buffer vbuf;
    rc_gfx_texture screen;
    rc_gfx_sampler sampler;
    rc_gfx_shader shader;
    rc_gfx_simple_layout layout;
    rc_gfx_bind_group group0;
    rc_gfx_pipeline pip;
} state;

static void on_key_down(void *ctx, rc_scancode key, rc_mod mods)
{
    (void)ctx;
    (void)mods;
    if (key == RC_SCANCODE_ESCAPE)
        state.quit = true;
    if (key >= 0 && key < 512)
        state.keys[key] = true;
}

static void on_key_up(void *ctx, rc_scancode key, rc_mod mods)
{
    (void)ctx;
    (void)mods;
    if (key >= 0 && key < 512)
        state.keys[key] = false;
}

static uint8_t gather_input(void)
{
    uint8_t in = 0;
    if (state.keys[RC_SCANCODE_W] || state.keys[RC_SCANCODE_UP])
        in |= PT_IN_FWD;
    if (state.keys[RC_SCANCODE_S] || state.keys[RC_SCANCODE_DOWN])
        in |= PT_IN_BACK;
    if (state.keys[RC_SCANCODE_A])
        in |= PT_IN_LEFT;
    if (state.keys[RC_SCANCODE_D])
        in |= PT_IN_RIGHT;
    if (state.keys[RC_SCANCODE_LEFT])
        in |= PT_IN_TURNL;
    if (state.keys[RC_SCANCODE_RIGHT])
        in |= PT_IN_TURNR;
    return in;
}

static void on_update(void *ctx, double dt)
{
    (void)ctx;

    // fixed 50 Hz ticks, BBC style; clamp a long stall to a burst of ticks
    state.tick_acc += dt;
    if (state.tick_acc > 0.25)
        state.tick_acc = 0.25;
    while (state.tick_acc >= TICK_SECONDS) {
        state.tick_acc -= TICK_SECONDS;
        pt_camera_tick(&state.camera, &state.map, &state.vcache, gather_input());
    }
}

static void on_render(void *ctx, rc_vec2i size)
{
    (void)ctx;

    pt_render_view(&state.render, state.camera.angle, state.camera.cell);
    pt_mode5_to_rgba(state.render.fb, state.rgba);

    rc_gfx_begin_frame(size);
    rc_arena_reset(&state.frame_arena);

    rc_gfx_texture_update(state.screen, 0, 0,
                          rc_box2i_make_pos_size((rc_vec2i) {0, 0},
                                                 (rc_vec2i) {PT_SCREEN_WIDTH, PT_SCREEN_SCANLINES}),
                          (rc_view_bytes) {.data = state.rgba, .num = sizeof(state.rgba)});

    rc_gfx_encoder *enc = rc_gfx_encoder_begin(&state.frame_arena);
    rc_gfx_encoder_pass_begin(enc, &(rc_gfx_pass_desc) {
        .colors = {
            {
                .clear_value = {0.0f, 0.0f, 0.0f, 1.0f},
            },
        },
        .label = RC_STR("present"),
    });

    // fit the full BBC frame to the window, then centre the MODE 5 playfield
    // within it (its pixels are double wide); the surround is the black border
    float s = (float)size.x / (float)PT_WINDOW_WIDTH;
    if ((float)size.y / (float)PT_WINDOW_HEIGHT < s)
        s = (float)size.y / (float)PT_WINDOW_HEIGHT;
    int32_t vw = (int32_t)((float)(PT_SCREEN_WIDTH * 2) * s);
    int32_t vh = (int32_t)((float)PT_SCREEN_SCANLINES * s);
    rc_gfx_encoder_set_viewport(enc,
                                rc_box2i_make_pos_size(
                                    (rc_vec2i) {(size.x - vw) / 2, (size.y - vh) / 2},
                                    (rc_vec2i) {vw, vh}),
                                0.0f, 1.0f);

    rc_gfx_encoder_set_pipeline(enc, state.pip);
    rc_gfx_encoder_set_bind_group(enc, 0, state.group0, NULL, 0);
    rc_gfx_encoder_set_vertex_buffer(enc, 0, state.vbuf, 0);
    rc_gfx_encoder_draw(enc, &(rc_gfx_draw_desc) {.vertex_count = 4});

    rc_gfx_encoder_pass_end(enc);

    rc_gfx_cmd_buffer cb = rc_gfx_encoder_finish(enc);
    rc_gfx_submit(&cb, 1);
    rc_gfx_end_frame();
}

static void gfx_setup(void)
{
    rc_gfx_init(&(rc_gfx_desc) {.arena = &state.arena});

    state.vbuf = rc_gfx_buffer_make(&(rc_gfx_buffer_desc) {
        .size = sizeof(vertices),
        .usage = RC_GFX_BUFFER_USAGE_VERTEX,
        .data = {.data = (const uint8_t *)vertices, .num = sizeof(vertices)},
        .label = RC_STR("present quad"),
    });

    state.screen = rc_gfx_texture_make(&(rc_gfx_texture_desc) {
        .format = RC_GFX_TEXTURE_FORMAT_RGBA8_SRGB,
        .size = {PT_SCREEN_WIDTH, PT_SCREEN_SCANLINES},
        .usage = RC_GFX_TEXTURE_USAGE_SAMPLED | RC_GFX_TEXTURE_USAGE_COPY_DST,
        .label = RC_STR("mode5 screen"),
    });

    state.sampler = rc_gfx_sampler_make(&(rc_gfx_sampler_desc) {
        .label = RC_STR("nearest"),
    });

    state.shader = rc_gfx_shader_make(&(rc_gfx_shader_desc) {
        .vs_source = RC_STR(vs_src),
        .fs_source = RC_STR(fs_src),
        .texture_samplers = (const rc_gfx_shader_texture_sampler_pair[]) {
            {
                .glsl_name = RC_STR("u_screen"),
                .texture_group = 0,
                .texture_binding = 0,
                .sampler_group = 0,
                .sampler_binding = 1,
            },
        },
        .texture_sampler_count = 1,
        .label = RC_STR("present shader"),
    });

    state.layout = rc_gfx_simple_layout_make(
        (const rc_gfx_bind_group_layout_entry[]) {
            {
                .binding = 0,
                .visibility = RC_GFX_STAGE_FRAGMENT,
                .type = RC_GFX_BINDING_TEXTURE,
            },
            {
                .binding = 1,
                .visibility = RC_GFX_STAGE_FRAGMENT,
                .type = RC_GFX_BINDING_SAMPLER,
            },
        },
        2, RC_STR("present layout"));

    state.group0 = rc_gfx_bind_group_make(&(rc_gfx_bind_group_desc) {
        .layout = state.layout.group0,
        .entries = {
            {.binding = 0, .texture = state.screen},
            {.binding = 1, .sampler = state.sampler},
        },
        .entry_count = 2,
        .label = RC_STR("present bindings"),
    });

    state.pip = rc_gfx_pipeline_make(&(rc_gfx_pipeline_desc) {
        .shader = state.shader,
        .layout = state.layout.layout,
        .vertex_layout = {
            .buffers = {{.stride = sizeof(vertex)}},
            .attributes = {
                {
                    .location = 0,
                    .buffer_index = 0,
                    .offset = offsetof(vertex, pos),
                    .format = RC_GFX_VERTEX_FORMAT_F32X2,
                },
                {
                    .location = 1,
                    .buffer_index = 0,
                    .offset = offsetof(vertex, uv),
                    .format = RC_GFX_VERTEX_FORMAT_F32X2,
                },
            },
        },
        .primitive = RC_GFX_PRIMITIVE_TRIANGLE_STRIP,
        .colors = {{.format = rc_gfx_swapchain_format()}},
        .color_count = 1,
        .label = RC_STR("present pipeline"),
    });
}

int main(void)
{
    state.arena = rc_arena_make_default();
    state.frame_arena = rc_arena_make_default();

    pt_arith_init();
    pt_scene_init(&state.map);
    pt_vcache_init(&state.vcache);
    pt_render_init(&state.render, &state.map, &state.vcache);
    pt_camera_init(&state.camera, &state.map, &state.vcache, PT_CAM_START_X, PT_CAM_START_Y,
                   PT_CAM_START_ANGLE);

    rc_app_init(&(rc_app_desc) {
        .title = RC_STR("portalc"),
        .size = {PT_WINDOW_WIDTH * PT_WINDOW_SCALE, PT_WINDOW_HEIGHT * PT_WINDOW_SCALE},
        .resizable = true,
        .callbacks = {
            .on_key_down = on_key_down,
            .on_key_up = on_key_up,
            .on_update = on_update,
            .on_render = on_render,
        },
    });

    gfx_setup();

    while (rc_app_is_running() && !state.quit) {
        rc_app_poll();
        rc_app_request_update();
        rc_app_request_render();
    }

    rc_gfx_shutdown();
    rc_app_destroy();
    return 0;
}
