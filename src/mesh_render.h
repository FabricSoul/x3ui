#pragma once
#include <cstdint>

// Experimental: render a real engine mesh by reusing the live player scene.
// Must be called on the main render thread with the GL context current, AFTER
// ImGui has drawn (it paints directly into the framebuffer sub-rect). Safe no-op
// if the scene/object isn't renderable.
namespace x3 {

struct MeshView {
    int   gx = 0, gy = 0, gw = 0, gh = 0; // GL viewport (bottom-left origin), pixels
    float yaw = 0.0f, pitch = 0.0f;       // orbit angles (radians)
    float dist = 1.0f;                    // orbit distance (object world units)
    bool  clear_depth = true;             // clear depth in the sub-rect first
};

// Render obj_addr's mesh (and children) with an orbit camera framing the object.
// Returns true if it issued the engine draw, false if it bailed (not renderable).
bool mesh_render_object(uint32_t obj_addr, const MeshView& mv);

} // namespace x3
