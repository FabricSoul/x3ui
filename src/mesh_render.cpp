#include "mesh_render.h"

#include <SDL2/SDL.h>
#include <dlfcn.h>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <initializer_list>

namespace x3 {

// ------------------------------------------------------------------ GL loader
// We avoid pulling in a GL loader: resolve the handful of entry points we need
// through SDL_GL_GetProcAddress (works for core + ARB on the live context).
typedef unsigned GLenum; typedef int GLint; typedef unsigned GLuint;
typedef unsigned char GLboolean; typedef int GLsizei; typedef unsigned GLbitfield;

#define GL_VIEWPORT                  0x0BA2
#define GL_SCISSOR_BOX               0x0C10
#define GL_SCISSOR_TEST              0x0C11
#define GL_BLEND                     0x0BE2
#define GL_DEPTH_TEST                0x0B71
#define GL_CULL_FACE                 0x0B44
#define GL_DEPTH_WRITEMASK           0x0B72
#define GL_DEPTH_FUNC                0x0B74
#define GL_BLEND_SRC                 0x0BE1
#define GL_BLEND_DST                 0x0BE0
#define GL_BLEND_EQUATION            0x8009
#define GL_CURRENT_PROGRAM           0x8B8D
#define GL_ACTIVE_TEXTURE            0x84E0
#define GL_ARRAY_BUFFER              0x8892
#define GL_ELEMENT_ARRAY_BUFFER      0x8893
#define GL_ARRAY_BUFFER_BINDING      0x8894
#define GL_ELEMENT_ARRAY_BUFFER_BINDING 0x8895
#define GL_DEPTH_BUFFER_BIT          0x00000100
#define GL_TEXTURE0                  0x84C0
#define GL_LEQUAL                    0x0203
#define GL_FALSE                     0
#define GL_TRUE                      1

struct GL {
    void (*GetIntegerv)(GLenum, GLint*);
    GLboolean (*IsEnabled)(GLenum);
    void (*Enable)(GLenum);
    void (*Disable)(GLenum);
    void (*Viewport)(GLint, GLint, GLsizei, GLsizei);
    void (*Scissor)(GLint, GLint, GLsizei, GLsizei);
    void (*DepthMask)(GLboolean);
    void (*DepthFunc)(GLenum);
    void (*BlendFunc)(GLenum, GLenum);
    void (*BlendEquation)(GLenum);
    void (*UseProgram)(GLuint);
    void (*BindBuffer)(GLenum, GLuint);
    void (*ActiveTexture)(GLenum);
    void (*DisableVertexAttribArray)(GLuint);
    void (*Clear)(GLbitfield);
    bool ok = false;
};
static GL g;

static void load_gl() {
    if (g.ok) return;
    typedef void* (*getproc_t)(const char*);
    static getproc_t getproc = (getproc_t)dlsym(RTLD_NEXT, "SDL_GL_GetProcAddress");
    if (!getproc) return;
    auto L = [&](const char* n) { return getproc(n); };
    g.GetIntegerv = (decltype(g.GetIntegerv))L("glGetIntegerv");
    g.IsEnabled   = (decltype(g.IsEnabled))L("glIsEnabled");
    g.Enable      = (decltype(g.Enable))L("glEnable");
    g.Disable     = (decltype(g.Disable))L("glDisable");
    g.Viewport    = (decltype(g.Viewport))L("glViewport");
    g.Scissor     = (decltype(g.Scissor))L("glScissor");
    g.DepthMask   = (decltype(g.DepthMask))L("glDepthMask");
    g.DepthFunc   = (decltype(g.DepthFunc))L("glDepthFunc");
    g.BlendFunc   = (decltype(g.BlendFunc))L("glBlendFunc");
    g.BlendEquation = (decltype(g.BlendEquation))L("glBlendEquation");
    g.UseProgram  = (decltype(g.UseProgram))L("glUseProgram");
    g.BindBuffer  = (decltype(g.BindBuffer))L("glBindBuffer");
    g.ActiveTexture = (decltype(g.ActiveTexture))L("glActiveTexture");
    g.DisableVertexAttribArray = (decltype(g.DisableVertexAttribArray))L("glDisableVertexAttribArray");
    g.Clear       = (decltype(g.Clear))L("glClear");
    g.ok = g.GetIntegerv && g.Viewport && g.Scissor && g.UseProgram &&
           g.BindBuffer && g.DisableVertexAttribArray && g.Clear &&
           g.Enable && g.Disable && g.DepthMask && g.ActiveTexture;
}

// Snapshot of the GL state the engine pipeline clobbers, so ImGui + the engine's
// next frame are unaffected.
struct GLState {
    GLint vp[4], sc[4];
    GLint scissor_en, blend_en, depth_en, cull_en, depth_mask, depth_func;
    GLint blend_src, blend_dst, blend_eq;
    GLint program, active_tex, arr_buf, elem_buf;
};
static void save_gl(GLState& s) {
    g.GetIntegerv(GL_VIEWPORT, s.vp);
    g.GetIntegerv(GL_SCISSOR_BOX, s.sc);
    s.scissor_en = g.IsEnabled(GL_SCISSOR_TEST);
    s.blend_en   = g.IsEnabled(GL_BLEND);
    s.depth_en   = g.IsEnabled(GL_DEPTH_TEST);
    s.cull_en    = g.IsEnabled(GL_CULL_FACE);
    g.GetIntegerv(GL_DEPTH_WRITEMASK, &s.depth_mask);
    g.GetIntegerv(GL_DEPTH_FUNC, &s.depth_func);
    g.GetIntegerv(GL_BLEND_SRC, &s.blend_src);
    g.GetIntegerv(GL_BLEND_DST, &s.blend_dst);
    g.GetIntegerv(GL_BLEND_EQUATION, &s.blend_eq);
    g.GetIntegerv(GL_CURRENT_PROGRAM, &s.program);
    g.GetIntegerv(GL_ACTIVE_TEXTURE, &s.active_tex);
    g.GetIntegerv(GL_ARRAY_BUFFER_BINDING, &s.arr_buf);
    g.GetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &s.elem_buf);
}
static void restore_gl(const GLState& s) {
    // The engine renders with no VAO and leaves attrib arrays 0..11 enabled —
    // the most likely thing to corrupt ImGui's next draw, so disable them.
    for (GLuint i : {0u,1u,2u,3u,4u,10u,11u}) g.DisableVertexAttribArray(i);
    g.UseProgram((GLuint)s.program);
    g.ActiveTexture((GLenum)s.active_tex);
    g.BindBuffer(GL_ARRAY_BUFFER, (GLuint)s.arr_buf);
    g.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)s.elem_buf);
    g.BlendEquation((GLenum)s.blend_eq);
    g.BlendFunc((GLenum)s.blend_src, (GLenum)s.blend_dst);
    g.DepthMask((GLboolean)s.depth_mask);
    g.DepthFunc((GLenum)s.depth_func);
    s.blend_en ? g.Enable(GL_BLEND) : g.Disable(GL_BLEND);
    s.depth_en ? g.Enable(GL_DEPTH_TEST) : g.Disable(GL_DEPTH_TEST);
    s.cull_en  ? g.Enable(GL_CULL_FACE) : g.Disable(GL_CULL_FACE);
    s.scissor_en ? g.Enable(GL_SCISSOR_TEST) : g.Disable(GL_SCISSOR_TEST);
    g.Viewport(s.vp[0], s.vp[1], s.vp[2], s.vp[3]);
    g.Scissor(s.sc[0], s.sc[1], s.sc[2], s.sc[3]);
}

// ------------------------------------------------------------- engine bindings
static const uint32_t G_WORLD_ROOT = 0x082a0640;
typedef int  (*pfn_setcam)(void* view, void* scene);
typedef void (*pfn_render_object)(void* obj, void* view, void* scratch, void* scene);
typedef void (*pfn_euler)(int* out3x3, uint16_t yaw, uint16_t pitch, uint16_t roll);
static pfn_setcam        SetCamera    = (pfn_setcam)0x08161730;
static pfn_render_object RenderObject = (pfn_render_object)0x080762f0;
static pfn_euler         BuildEuler   = (pfn_euler)0x080be130;

static inline uint32_t rd32(uint32_t a) { return *(volatile uint32_t*)(uintptr_t)a; }
static inline void     wr32(uint32_t a, uint32_t v) { *(volatile uint32_t*)(uintptr_t)a = v; }

bool mesh_render_object(uint32_t obj, const MeshView& mv) {
    load_gl();
    if (!g.ok || !obj) return false;

    uint32_t scene = rd32(G_WORLD_ROOT + 0x64);
    if (!scene) return false;
    uint32_t view = rd32(scene + 0x1c);
    if (!view) return false;
    uint32_t vp = rd32(scene + 0x24c);
    if (!vp) return false;

    // Orbit target = object's world position (the values the renderer reads).
    int32_t ox = (int32_t)rd32(obj + 0x9c);
    int32_t oy = (int32_t)rd32(obj + 0xa0);
    int32_t oz = (int32_t)rd32(obj + 0xa4);

    // --- save scene fields we clobber ---
    int sv_pos[3];  memcpy(sv_pos,  (void*)(uintptr_t)(scene + 0x2c), 12);
    int sv_or[11];  memcpy(sv_or,   (void*)(uintptr_t)(scene + 0x3c), 44);
    int sv_roll  = (int)rd32(scene + 0x26c);
    int sv_vp[4];   memcpy(sv_vp,   (void*)(uintptr_t)vp, 16);
    int sv_rect[4]; memcpy(sv_rect, (void*)(uintptr_t)(scene + 0x25c), 16);

    // --- build orbit camera orientation, then derive eye = target - fwd*dist ---
    uint16_t yaw   = (uint16_t)lroundf(mv.yaw   / 6.2831853f * 65536.0f);
    uint16_t pitch = (uint16_t)lroundf(mv.pitch / 6.2831853f * 65536.0f);
    BuildEuler((int*)(uintptr_t)(scene + 0x3c), yaw, pitch, 0);
    float fx = (int32_t)rd32(scene + 0x44) / 65536.0f;   // column 2 = forward (+Z nose)
    float fy = (int32_t)rd32(scene + 0x54) / 65536.0f;
    float fz = (int32_t)rd32(scene + 0x64) / 65536.0f;
    wr32(scene + 0x2c, (uint32_t)(int32_t)lroundf(ox - fx * mv.dist));
    wr32(scene + 0x30, (uint32_t)(int32_t)lroundf(oy - fy * mv.dist));
    wr32(scene + 0x34, (uint32_t)(int32_t)lroundf(oz - fz * mv.dist));
    wr32(scene + 0x26c, 0);

    // --- viewport + projection rect for the sub-rect ---
    wr32(vp + 0,  (uint32_t)mv.gx); wr32(vp + 4,  (uint32_t)mv.gy);
    wr32(vp + 8,  (uint32_t)mv.gw); wr32(vp + 12, (uint32_t)mv.gh);
    wr32(scene + 0x25c, 0);            wr32(scene + 0x260, (uint32_t)mv.gh);
    wr32(scene + 0x264, 0);            wr32(scene + 0x268, (uint32_t)mv.gw);

    // --- draw ---
    GLState s; save_gl(s);
    SetCamera((void*)(uintptr_t)view, (void*)(uintptr_t)scene);
    if (mv.clear_depth) {
        g.Enable(GL_SCISSOR_TEST);
        g.Scissor(mv.gx, mv.gy, mv.gw, mv.gh);
        g.Clear(GL_DEPTH_BUFFER_BIT);
    }
    RenderObject((void*)(uintptr_t)obj, (void*)(uintptr_t)view,
                 (void*)(uintptr_t)(G_WORLD_ROOT + 0x4c), (void*)(uintptr_t)scene);
    restore_gl(s);

    // --- restore scene fields (so the engine's next real frame is unaffected) ---
    memcpy((void*)(uintptr_t)(scene + 0x2c),  sv_pos,  12);
    memcpy((void*)(uintptr_t)(scene + 0x3c),  sv_or,   44);
    wr32(scene + 0x26c, (uint32_t)sv_roll);
    memcpy((void*)(uintptr_t)vp,              sv_vp,   16);
    memcpy((void*)(uintptr_t)(scene + 0x25c), sv_rect, 16);
    return true;
}

} // namespace x3
