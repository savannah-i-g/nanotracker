// ui/crt_pass — the phosphor-CRT post-process. The whole UI renders
// into an offscreen target; this pass composites it to the backbuffer
// with scanlines, vignette, and a quarter-resolution glow blur.
// Intensity is user-scalable because full-strength scanlines fight
// text legibility.
//
// Frame protocol:
//   pass.begin(fb_w, fb_h, r, g, b);   // binds+clears the scene target
//   ... render ImGui ...
//   pass.end(enabled, intensity);      // composites to the backbuffer
#pragma once

#include <array>

namespace nt::ui {

class CrtPass {
public:
    CrtPass() = default;
    ~CrtPass();

    CrtPass(const CrtPass&) = delete;
    CrtPass& operator=(const CrtPass&) = delete;
    CrtPass(CrtPass&&) = delete;
    CrtPass& operator=(CrtPass&&) = delete;

    // Binds the scene framebuffer, (re)allocating render targets when
    // the framebuffer size changed. Requires a current GL context.
    void begin(int width, int height, float clear_r, float clear_g, float clear_b);

    // Composites the scene to the default framebuffer. When disabled,
    // the scene is passed through untouched (still one draw — the UI
    // has already rendered offscreen by this point).
    void end(bool enabled, float intensity);

private:
    void ensure_targets(int width, int height);
    void ensure_programs();
    void destroy_targets();

    int width_ = 0;
    int height_ = 0;

    unsigned scene_fbo_ = 0;
    unsigned scene_tex_ = 0;
    std::array<unsigned, 2> glow_fbo_{};
    std::array<unsigned, 2> glow_tex_{};

    unsigned vao_ = 0;
    unsigned prog_blit_ = 0;
    unsigned prog_downsample_ = 0;
    unsigned prog_blur_ = 0;
    unsigned prog_composite_ = 0;
};

} // namespace nt::ui
