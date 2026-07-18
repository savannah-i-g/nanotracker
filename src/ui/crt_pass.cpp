#include "ui/crt_pass.h"

#include <array>
#include <glad/gl.h>
#include <stdexcept>
#include <string>

namespace nt::ui {

namespace {

constexpr int kGlowDivisor = 4; // glow chain runs at quarter resolution

// All passes draw one fullscreen triangle generated from gl_VertexID;
// no vertex buffers are involved.
constexpr const char* kFullscreenVs = R"(#version 330 core
out vec2 v_uv;
void main() {
    vec2 pos = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    v_uv = pos;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
})";

constexpr const char* kBlitFs = R"(#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_scene;
void main() { frag = texture(u_scene, v_uv); })";

// Bright-pass + box downsample seeding the glow chain. The soft knee
// keeps dark backgrounds out of the glow while letting phosphor text
// bloom.
constexpr const char* kDownsampleFs = R"(#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_scene;
uniform vec2 u_texel;
void main() {
    vec3 c = vec3(0.0);
    c += texture(u_scene, v_uv + u_texel * vec2(-0.5, -0.5)).rgb;
    c += texture(u_scene, v_uv + u_texel * vec2( 0.5, -0.5)).rgb;
    c += texture(u_scene, v_uv + u_texel * vec2(-0.5,  0.5)).rgb;
    c += texture(u_scene, v_uv + u_texel * vec2( 0.5,  0.5)).rgb;
    c *= 0.25;
    float luma = dot(c, vec3(0.299, 0.587, 0.114));
    frag = vec4(c * smoothstep(0.15, 0.45, luma), 1.0);
})";

// Separable 9-tap Gaussian; u_dir selects the axis.
constexpr const char* kBlurFs = R"(#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_src;
uniform vec2 u_dir;
void main() {
    const float w[5] = float[](0.227027, 0.194594, 0.121622, 0.054054, 0.016216);
    vec3 c = texture(u_src, v_uv).rgb * w[0];
    for (int i = 1; i < 5; ++i) {
        c += texture(u_src, v_uv + u_dir * float(i)).rgb * w[i];
        c += texture(u_src, v_uv - u_dir * float(i)).rgb * w[i];
    }
    frag = vec4(c, 1.0);
})";

constexpr const char* kCompositeFs = R"(#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D u_scene;
uniform sampler2D u_glow;
uniform vec2 u_resolution;
uniform float u_intensity;
void main() {
    vec3 scene = texture(u_scene, v_uv).rgb;
    vec3 glow = texture(u_glow, v_uv).rgb;

    vec3 color = scene + glow * (0.7 * u_intensity);

    // Scanlines: darken alternate physical lines, softened by intensity.
    float line = 0.5 + 0.5 * cos(v_uv.y * u_resolution.y * 3.14159265);
    color *= mix(1.0, 0.82 + 0.18 * line, u_intensity);

    // Corner vignette.
    float dist = length(v_uv - 0.5) * 1.3;
    color *= 1.0 - 0.35 * u_intensity * smoothstep(0.55, 1.0, dist);

    frag = vec4(color, 1.0);
})";

unsigned compile_shader(GLenum type, const char* source) {
    const unsigned shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    int ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok == 0) {
        std::array<char, 1024> log{};
        glGetShaderInfoLog(shader, log.size(), nullptr, log.data());
        glDeleteShader(shader);
        throw std::runtime_error(std::string("shader compile failed: ") + log.data());
    }
    return shader;
}

unsigned link_program(const char* fs_source) {
    const unsigned vs = compile_shader(GL_VERTEX_SHADER, kFullscreenVs);
    const unsigned fs = compile_shader(GL_FRAGMENT_SHADER, fs_source);
    const unsigned program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    int ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok == 0) {
        std::array<char, 1024> log{};
        glGetProgramInfoLog(program, log.size(), nullptr, log.data());
        glDeleteProgram(program);
        throw std::runtime_error(std::string("program link failed: ") + log.data());
    }
    return program;
}

void make_target(unsigned& fbo, unsigned& tex, int width, int height) {
    glGenFramebuffers(1, &fbo);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        throw std::runtime_error("CRT framebuffer incomplete");
    }
}

void draw_fullscreen() {
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

} // namespace

CrtPass::~CrtPass() {
    destroy_targets();
    if (vao_ != 0) {
        glDeleteVertexArrays(1, &vao_);
    }
    for (const unsigned prog : {prog_blit_, prog_downsample_, prog_blur_, prog_composite_}) {
        if (prog != 0) {
            glDeleteProgram(prog);
        }
    }
}

void CrtPass::destroy_targets() {
    if (scene_fbo_ != 0) {
        glDeleteFramebuffers(1, &scene_fbo_);
        glDeleteTextures(1, &scene_tex_);
        glDeleteFramebuffers(2, glow_fbo_.data());
        glDeleteTextures(2, glow_tex_.data());
        scene_fbo_ = scene_tex_ = 0;
        glow_fbo_[0] = glow_fbo_[1] = glow_tex_[0] = glow_tex_[1] = 0;
    }
}

void CrtPass::ensure_programs() {
    if (prog_blit_ != 0) {
        return;
    }
    glGenVertexArrays(1, &vao_);
    prog_blit_ = link_program(kBlitFs);
    prog_downsample_ = link_program(kDownsampleFs);
    prog_blur_ = link_program(kBlurFs);
    prog_composite_ = link_program(kCompositeFs);
}

void CrtPass::ensure_targets(int width, int height) {
    if (width == width_ && height == height_) {
        return;
    }
    destroy_targets();
    width_ = width;
    height_ = height;
    make_target(scene_fbo_, scene_tex_, width, height);
    const int gw = width / kGlowDivisor > 0 ? width / kGlowDivisor : 1;
    const int gh = height / kGlowDivisor > 0 ? height / kGlowDivisor : 1;
    make_target(glow_fbo_[0], glow_tex_[0], gw, gh);
    make_target(glow_fbo_[1], glow_tex_[1], gw, gh);
}

void CrtPass::begin(int width, int height, float clear_r, float clear_g, float clear_b) {
    ensure_programs();
    ensure_targets(width, height);

    glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo_);
    glViewport(0, 0, width_, height_);
    glClearColor(clear_r, clear_g, clear_b, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
}

void CrtPass::end(bool enabled, float intensity) {
    glBindVertexArray(vao_);
    glDisable(GL_BLEND);
    glActiveTexture(GL_TEXTURE0);

    if (enabled && intensity > 0.0F) {
        const int gw = width_ / kGlowDivisor > 0 ? width_ / kGlowDivisor : 1;
        const int gh = height_ / kGlowDivisor > 0 ? height_ / kGlowDivisor : 1;

        // Bright-pass downsample into the glow chain.
        glBindFramebuffer(GL_FRAMEBUFFER, glow_fbo_[0]);
        glViewport(0, 0, gw, gh);
        glUseProgram(prog_downsample_);
        glUniform1i(glGetUniformLocation(prog_downsample_, "u_scene"), 0);
        glUniform2f(glGetUniformLocation(prog_downsample_, "u_texel"),
                    1.0F / static_cast<float>(width_), 1.0F / static_cast<float>(height_));
        glBindTexture(GL_TEXTURE_2D, scene_tex_);
        draw_fullscreen();

        // Separable blur: horizontal then vertical.
        glUseProgram(prog_blur_);
        glUniform1i(glGetUniformLocation(prog_blur_, "u_src"), 0);

        glBindFramebuffer(GL_FRAMEBUFFER, glow_fbo_[1]);
        glUniform2f(glGetUniformLocation(prog_blur_, "u_dir"), 1.0F / static_cast<float>(gw), 0.0F);
        glBindTexture(GL_TEXTURE_2D, glow_tex_[0]);
        draw_fullscreen();

        glBindFramebuffer(GL_FRAMEBUFFER, glow_fbo_[0]);
        glUniform2f(glGetUniformLocation(prog_blur_, "u_dir"), 0.0F, 1.0F / static_cast<float>(gh));
        glBindTexture(GL_TEXTURE_2D, glow_tex_[1]);
        draw_fullscreen();

        // Composite to the backbuffer.
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, width_, height_);
        glUseProgram(prog_composite_);
        glUniform1i(glGetUniformLocation(prog_composite_, "u_scene"), 0);
        glUniform1i(glGetUniformLocation(prog_composite_, "u_glow"), 1);
        glUniform2f(glGetUniformLocation(prog_composite_, "u_resolution"),
                    static_cast<float>(width_), static_cast<float>(height_));
        glUniform1f(glGetUniformLocation(prog_composite_, "u_intensity"), intensity);
        glBindTexture(GL_TEXTURE_2D, scene_tex_);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, glow_tex_[0]);
        glActiveTexture(GL_TEXTURE0);
        draw_fullscreen();
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, width_, height_);
        glUseProgram(prog_blit_);
        glUniform1i(glGetUniformLocation(prog_blit_, "u_scene"), 0);
        glBindTexture(GL_TEXTURE_2D, scene_tex_);
        draw_fullscreen();
    }

    glBindVertexArray(0);
    glUseProgram(0);
}

} // namespace nt::ui
