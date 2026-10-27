#include "longpress.hpp"

#include "log.hpp"

#include <SDL3/SDL.h>
#include <gyrolib/gyrolib.h>

#include <algorithm>
#include <string>

namespace gyro4all::longpress {
namespace {

// A tap is a press the game must see for a few of its frames (30 fps games poll every 33 ms).
constexpr uint64_t kTapNs = 80'000'000;

SDL_Gamepad* g_pad = nullptr;
uint32_t g_held = 0;        // buttons down last frame
uint32_t g_forwarded = 0;   // held buttons whose press GyroLib let through
uint32_t g_suppressed = 0;  // held buttons GyroLib holds back
uint64_t g_tap_until[32] = {};
uint32_t g_taps = 0, g_blocked = 0;  // diagnostics

uint32_t buttons_of(SDL_Gamepad* pad) {
    uint32_t bits = 0;
    const int count = std::min(32, int(SDL_GAMEPAD_BUTTON_COUNT));
    for (int n = 0; n < count; ++n)
        if (SDL_GetGamepadButton(pad, SDL_GamepadButton(n))) bits |= 1u << n;
    return bits;
}

double setting(gl_context* ctx, const std::string& key) {
    double v = 0;
    return gl_setting_get(ctx, key.c_str(), &v) == GL_OK ? v : 0;
}

// The active view's activation button (1..32 -> SDL ordinal + 1, 0 = none or a pair) and whether GyroLib can defer
// its press: Hold or Hold to disable with Block long press on.
uint32_t activation_button(gl_context* ctx, bool& blocking) {
    const std::string view = "context." + std::to_string(gl_get_active_gameplay_context(ctx)) + ".";
    const double button = setting(ctx, view + "activation.button");
    const int mode = int(setting(ctx, view + "gyro.activation"));
    blocking = (mode == GL_HOLD || mode == GL_HOLD_DISABLE) && setting(ctx, view + "activation.block_long_press") != 0;
    return button >= 1 && button <= 32 ? uint32_t(button) : 0;
}

}  // namespace

Mask frame(gl_context* ctx, SDL_Gamepad* pad, uint64_t now_ns, bool active, bool declared) {
    const uint32_t buttons = pad ? buttons_of(pad) : 0;
    if (!active || pad != g_pad) {
        // Finish the old cycles without taps; buttons already held when the game gets the controller back aren't new
        // presses (and reach the game as they are).
        for (unsigned n = 0; n < 32; ++n)
            if (g_held & (1u << n)) gl_filter_event(ctx, n, GL_RELEASE, now_ns, 0, 1);
        std::fill(std::begin(g_tap_until), std::end(g_tap_until), 0);
        g_pad = pad;
        g_held = active ? buttons : 0;
        g_forwarded = g_held;
        g_suppressed = 0;
        return {};
    }
    bool blocking = false;
    const uint32_t binding = activation_button(ctx, blocking);
    // The activation button is held back from its first report on, before this frame sees the press: the game reads
    // the controller on its own threads, and a press it saw for a moment would already be its action.
    const uint32_t armed = declared && blocking && binding ? 1u << (binding - 1) : 0;
    Mask mask;
    for (unsigned n = 0; n < 32; ++n) {
        const uint32_t bit = 1u << n;
        const bool down = (buttons & bit) != 0, was_down = (g_held & bit) != 0;
        if (down || was_down) {
            const uint32_t event = down ? (was_down ? GL_REPEAT : GL_PRESS) : GL_RELEASE;
            const int32_t r = gl_filter_event(ctx, n, event, now_ns, binding == n + 1, 0);
            if (event == GL_PRESS) {
                if (r == GL_FORWARD) g_forwarded |= bit;
                else g_suppressed |= bit;
            } else if (event == GL_REPEAT) {
                if (r != GL_FORWARD && !(g_forwarded & bit)) g_suppressed |= bit;
            } else {
                if (r == GL_EMIT_TAP) {
                    g_tap_until[n] = now_ns + kTapNs;
                    ++g_taps;
                } else if (g_suppressed & bit) {
                    ++g_blocked;
                }
                if ((g_taps + g_blocked) <= 5 && (r == GL_EMIT_TAP || (g_suppressed & bit)))
                    log("longpress: button %u %s (%u taps, %u holds blocked so far)", n,
                        r == GL_EMIT_TAP ? "tapped: short press passed to the game" : "held: its game action blocked",
                        g_taps, g_blocked);
                g_forwarded &= ~bit;
                g_suppressed &= ~bit;
            }
        }
        if (g_tap_until[n] > now_ns) mask.tap |= bit;
        else g_tap_until[n] = 0;
    }
    g_held = buttons;
    mask.hide = (armed & ~g_forwarded) | g_suppressed;
    return mask;
}

}  // namespace gyro4all::longpress
