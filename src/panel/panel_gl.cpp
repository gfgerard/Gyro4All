// The panel in OpenGL games: ImGui's OpenGL 3 backend drawing into the game's back buffer inside its SwapBuffers /
// wglSwapBuffers (winmm.dll's panel_gl.cpp hands each swap over), with the game's own GL context, current on the
// swapping thread. The backend loads its functions with its own loader (opengl32.dll, loaded by the game already);
// this file gets the few it needs itself from opengl32.dll and wglGetProcAddress (no OpenGL import of our own).
//
// State: the backend saves and restores what it changes (program, textures, sampler, buffers, vertex array, viewport,
// scissor, blend, polygon mode, the enables it touches, unpack row length and alignment). Around it this file sets and
// restores what the backend assumes: the default framebuffer as draw framebuffer and its back buffer as draw buffer,
// no pixel unpack buffer and no unpack skips, a full colour mask, and sRGB conversion, rasterizer discard, logic op,
// alpha-to-coverage and clip distances off.
//
// Nothing here waits for the GPU: no glFinish, no fences, no reads back. Every GL call runs under a structured
// exception guard: a fault (a driver without a function, a context the game broke) turns the panel off for the session
// instead of crashing the game, and the saved state is put back as far as it can be.
//
// The backend's objects belong to one context. When the game swaps with another one, the backend is shut down in the
// old context (made current for that, or a throwaway context of the same pixel format when the old one is gone), and
// started again in the new one; the game's context is then current again.
#include "panel.hpp"

#include <imgui.h>
#include <imgui_impl_opengl3.h>

namespace g4a::gl {
namespace {

using GLenum = unsigned int;
using GLint = int;
using GLuint = unsigned int;
using GLboolean = unsigned char;

constexpr GLenum kBack = 0x0405, kBackLeft = 0x0402, kDrawBuffer = 0x0C01, kColorWritemask = 0x0C23;
constexpr GLenum kDrawFramebuffer = 0x8CA9, kDrawFramebufferBinding = 0x8CA6;
constexpr GLenum kPixelUnpackBuffer = 0x88EC, kPixelUnpackBufferBinding = 0x88EF;
constexpr GLenum kUnpackSkipRows = 0x0CF3, kUnpackSkipPixels = 0x0CF4;
constexpr GLenum kMajorVersion = 0x821B, kMinorVersion = 0x821C, kVersion = 0x1F02, kRenderer = 0x1F01;
constexpr GLenum kFramebufferSrgb = 0x8DB9, kRasterizerDiscard = 0x8C89, kColorLogicOp = 0x0BF2,
                 kSampleAlphaToCoverage = 0x809E, kClipDistance0 = 0x3000;
constexpr int kClipDistances = 8;
// Turned off around the panel's draw, then back on if they were.
constexpr GLenum kOffCaps[] = {kFramebufferSrgb, kRasterizerDiscard, kColorLogicOp, kSampleAlphaToCoverage};

struct Fns {
    HGLRC(WINAPI* get_current_context)() = nullptr;
    HDC(WINAPI* get_current_dc)() = nullptr;
    BOOL(WINAPI* make_current)(HDC, HGLRC) = nullptr;
    HGLRC(WINAPI* create_context)(HDC) = nullptr;
    BOOL(WINAPI* delete_context)(HGLRC) = nullptr;
    PROC(WINAPI* get_proc_address)(LPCSTR) = nullptr;
    void(WINAPI* get_integerv)(GLenum, GLint*) = nullptr;
    const unsigned char*(WINAPI* get_string)(GLenum) = nullptr;
    GLboolean(WINAPI* is_enabled)(GLenum) = nullptr;
    void(WINAPI* enable)(GLenum) = nullptr;
    void(WINAPI* disable)(GLenum) = nullptr;
    void(WINAPI* get_booleanv)(GLenum, GLboolean*) = nullptr;
    void(WINAPI* color_mask)(GLboolean, GLboolean, GLboolean, GLboolean) = nullptr;
    void(WINAPI* draw_buffer)(GLenum) = nullptr;
    void(WINAPI* pixel_storei)(GLenum, GLint) = nullptr;
    // Per context (wglGetProcAddress), reloaded on every start.
    void(WINAPI* bind_framebuffer)(GLenum, GLuint) = nullptr;
    void(WINAPI* bind_buffer)(GLenum, GLuint) = nullptr;
};

// The game's state around the panel's draw.
struct Saved {
    GLint framebuffer = 0, draw_buffer = 0, unpack_buffer = 0, skip_rows = 0, skip_pixels = 0;
    GLboolean color_mask[4] = {1, 1, 1, 1};
    GLboolean caps[sizeof(kOffCaps) / sizeof(kOffCaps[0])] = {};
    GLboolean clip[kClipDistances] = {};
    bool valid = false;
};

// Under g4a::g_lock.
Fns f;
bool g_loaded = false;
bool g_backend = false;
bool g_failed = false;  // the panel can't draw in this game (logged once)
HGLRC g_context = nullptr;
HDC g_context_dc = nullptr;
bool g_logged_no_context = false, g_logged_other_dc = false;

template <class T>
bool export_of(HMODULE m, T& out, const char* name) {
    out = reinterpret_cast<T>(GetProcAddress(m, name));
    if (!out) log("OpenGL: opengl32.dll has no %s: panel unavailable", name);
    return out != nullptr;
}

bool load_exports() {
    HMODULE m = GetModuleHandleW(L"opengl32.dll");
    if (!m) {
        log("OpenGL: opengl32.dll not loaded: panel unavailable");
        return false;
    }
    return export_of(m, f.get_current_context, "wglGetCurrentContext") &&
           export_of(m, f.get_current_dc, "wglGetCurrentDC") && export_of(m, f.make_current, "wglMakeCurrent") &&
           export_of(m, f.create_context, "wglCreateContext") && export_of(m, f.delete_context, "wglDeleteContext") &&
           export_of(m, f.get_proc_address, "wglGetProcAddress") && export_of(m, f.get_integerv, "glGetIntegerv") &&
           export_of(m, f.get_string, "glGetString") && export_of(m, f.is_enabled, "glIsEnabled") &&
           export_of(m, f.enable, "glEnable") && export_of(m, f.disable, "glDisable") &&
           export_of(m, f.get_booleanv, "glGetBooleanv") && export_of(m, f.color_mask, "glColorMask") &&
           export_of(m, f.draw_buffer, "glDrawBuffer") && export_of(m, f.pixel_storei, "glPixelStorei");
}

template <class T>
bool context_proc(T& out, const char* name) {
    out = reinterpret_cast<T>(f.get_proc_address(name));
    // wglGetProcAddress may answer 1, 2, 3 or -1 instead of null for a missing function.
    const auto v = reinterpret_cast<intptr_t>(out);
    if (v >= -1 && v <= 3) out = nullptr;
    if (!out) log("OpenGL: the game's context has no %s: panel unavailable", name);
    return out != nullptr;
}

// SEH can't share a function with C++ objects that need unwinding: the guarded bodies are plain functions.
bool guarded(void (*fn)(void*), void* arg, const char* what) {
    DWORD code = 0;
    __try {
        fn(arg);
        return true;
    } __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
    }
    log("OpenGL: exception 0x%08lX in %s: panel turned off", static_cast<unsigned long>(code), what);
    return false;
}

struct InitArgs {
    bool ok = false;
};

void init_body(void* p) {
    auto& a = *static_cast<InitArgs*>(p);
    GLint major = 0, minor = 0;
    f.get_integerv(kMajorVersion, &major);
    f.get_integerv(kMinorVersion, &minor);
    const char* version = reinterpret_cast<const char*>(f.get_string(kVersion));
    const char* renderer = reinterpret_cast<const char*>(f.get_string(kRenderer));
    log("OpenGL context %p: %s on %s", static_cast<void*>(g_context), version ? version : "?", renderer ? renderer : "?");
    if (major < 3) {
        log("OpenGL: version %d.%d: the panel needs OpenGL 3.0 or later", major, minor);
        return;
    }
    if (!context_proc(f.bind_framebuffer, "glBindFramebuffer") || !context_proc(f.bind_buffer, "glBindBuffer")) return;
    if (!ImGui_ImplOpenGL3_Init(nullptr)) {
        log("ImGui's OpenGL 3 backend did not start: panel unavailable");
        return;
    }
    g_backend = true;
    a.ok = true;
}

void shutdown_body(void*) { ImGui_ImplOpenGL3_Shutdown(); }

// The backend's objects are deleted in their own context when it still exists, else in a throwaway one (they went
// with the old context); never in the game's current one, where the same names are the game's objects.
void shutdown(HDC hdc, HGLRC current, HDC current_dc) {
    if (!g_backend) return;
    HGLRC temporary = nullptr;
    bool made = g_context_dc && f.make_current(g_context_dc, g_context);
    if (!made) {
        temporary = f.create_context(hdc);
        made = temporary && f.make_current(hdc, temporary);
    }
    if (made) {
        guarded(&shutdown_body, nullptr, "the backend's shutdown");
        g_backend = false;
    } else {
        log("OpenGL: no context to shut the backend down in: panel turned off");
        g_failed = true;
    }
    f.make_current(current_dc, current);
    if (temporary) f.delete_context(temporary);
    renderer_ready(false);
}

void save_and_set(void* p) {
    auto& s = *static_cast<Saved*>(p);
    f.get_integerv(kDrawFramebufferBinding, &s.framebuffer);
    f.get_integerv(kPixelUnpackBufferBinding, &s.unpack_buffer);
    f.get_integerv(kUnpackSkipRows, &s.skip_rows);
    f.get_integerv(kUnpackSkipPixels, &s.skip_pixels);
    f.get_booleanv(kColorWritemask, s.color_mask);
    for (size_t i = 0; i < sizeof(kOffCaps) / sizeof(kOffCaps[0]); ++i) s.caps[i] = f.is_enabled(kOffCaps[i]);
    for (int i = 0; i < kClipDistances; ++i) s.clip[i] = f.is_enabled(kClipDistance0 + i);
    s.valid = true;
    f.bind_framebuffer(kDrawFramebuffer, 0);
    f.get_integerv(kDrawBuffer, &s.draw_buffer);  // the default framebuffer's
    if (s.draw_buffer != GLint(kBack) && s.draw_buffer != GLint(kBackLeft)) f.draw_buffer(kBack);
    f.bind_buffer(kPixelUnpackBuffer, 0);
    f.pixel_storei(kUnpackSkipRows, 0);
    f.pixel_storei(kUnpackSkipPixels, 0);
    f.color_mask(1, 1, 1, 1);
    for (GLenum cap : kOffCaps) f.disable(cap);
    for (int i = 0; i < kClipDistances; ++i)
        if (s.clip[i]) f.disable(kClipDistance0 + i);
}

void restore(void* p) {
    auto& s = *static_cast<Saved*>(p);
    if (!s.valid) return;
    for (size_t i = 0; i < sizeof(kOffCaps) / sizeof(kOffCaps[0]); ++i)
        if (s.caps[i]) f.enable(kOffCaps[i]);
    for (int i = 0; i < kClipDistances; ++i)
        if (s.clip[i]) f.enable(kClipDistance0 + i);
    f.color_mask(s.color_mask[0], s.color_mask[1], s.color_mask[2], s.color_mask[3]);
    f.pixel_storei(kUnpackSkipRows, s.skip_rows);
    f.pixel_storei(kUnpackSkipPixels, s.skip_pixels);
    f.bind_buffer(kPixelUnpackBuffer, GLuint(s.unpack_buffer));
    if (s.draw_buffer != GLint(kBack) && s.draw_buffer != GLint(kBackLeft)) f.draw_buffer(GLenum(s.draw_buffer));
    f.bind_framebuffer(kDrawFramebuffer, GLuint(s.framebuffer));
}

void render(void* data) {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplOpenGL3_RenderDrawData(static_cast<ImDrawData*>(data));
}

void fail() {
    g_failed = true;
    renderer_ready(false);
}

}  // namespace

bool present(HDC hdc) {
    Lock lock;
    if (g_failed) return false;
    if (!g_loaded) {
        if (!load_exports()) {
            fail();
            return false;
        }
        g_loaded = true;
    }
    const HGLRC context = f.get_current_context();
    if (!context) {
        if (!g_logged_no_context) log("OpenGL: no GL context current while the game swaps: nothing drawn");
        g_logged_no_context = true;
        return false;
    }
    const HDC current_dc = f.get_current_dc();
    if (current_dc != hdc && WindowFromDC(current_dc) != WindowFromDC(hdc)) {
        if (!g_logged_other_dc) log("OpenGL: the game swaps a window its current context doesn't draw on: nothing drawn");
        g_logged_other_dc = true;
        return false;
    }
    if (context != g_context || !g_backend) {
        if (g_backend) {
            log("OpenGL: the game swaps with another context now: renderer restarted");
            shutdown(hdc, context, current_dc);
            if (g_failed) return false;
        }
        g_context = context;
        g_context_dc = current_dc;
        InitArgs args;
        if (!guarded(&init_body, &args, "the backend's start") || !args.ok) {
            fail();
            return false;
        }
        renderer_ready(true);
        log("OpenGL renderer ready on context %p", static_cast<void*>(context));
    }
    present_seen();
    RECT rc{};
    if (GetClientRect(WindowFromDC(hdc), &rc) && rc.right > 0 && rc.bottom > 0)
        backbuffer_size(uint32_t(rc.right), uint32_t(rc.bottom), true);

    ImDrawData* data = draw_data();
    if (!data) return true;  // panel closed: the swap is left alone
    Saved saved;
    const bool drawn = guarded(&save_and_set, &saved, "saving the game's GL state") &&
                       guarded(&render, data, "the panel's draw");
    guarded(&restore, &saved, "restoring the game's GL state");
    if (!drawn) {
        fail();
        return false;
    }
    return true;
}

}  // namespace g4a::gl
