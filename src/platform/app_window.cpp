#include "platform/app_window.h"

#include <GLFW/glfw3.h>

#include <glad/gl.h>
#include <stdexcept>
#include <string>

namespace nt::platform {

namespace {

// GLFW must be initialised once per process and torn down after the
// last window dies. Reference-counted so multiple windows stay possible
// (external plugin editors will not use this path, but tools might).
int& glfw_refcount() {
    static int count = 0;
    return count;
}

[[noreturn]] void throw_glfw_error(const char* what) {
    const char* description = nullptr;
    glfwGetError(&description);
    throw std::runtime_error(std::string(what) + ": " +
                             (description != nullptr ? description : "unknown error"));
}

void acquire_glfw() {
    if (glfw_refcount() == 0) {
        if (glfwInit() != GLFW_TRUE) {
            throw_glfw_error("GLFW init failed");
        }
    }
    ++glfw_refcount();
}

void release_glfw() {
    if (--glfw_refcount() == 0) {
        glfwTerminate();
    }
}

// Creates the window with an OpenGL 3.3 core context, or throws with
// the GLFW error description. Runs between acquire_glfw()/release_glfw().
GLFWwindow* create_window(const AppWindow::Config& config) {
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);

    GLFWwindow* window =
        glfwCreateWindow(config.width, config.height, config.title.c_str(), nullptr, nullptr);
    if (window == nullptr) {
        release_glfw();
        throw_glfw_error("window creation failed");
    }
    return window;
}

} // namespace

AppWindow::AppWindow(const Config& config) : window_((acquire_glfw(), create_window(config))) {
    glfwMakeContextCurrent(window_);
    glfwSwapInterval(1);

    // GL function loading is part of context bootstrap: everything
    // above the platform layer may assume a loaded GL 3.3 core context.
    if (gladLoadGL(glfwGetProcAddress) == 0) {
        glfwDestroyWindow(window_);
        release_glfw();
        throw std::runtime_error("failed to load OpenGL functions");
    }
}

AppWindow::~AppWindow() {
    if (window_ != nullptr) {
        glfwDestroyWindow(window_);
    }
    release_glfw();
}

bool AppWindow::should_close() const {
    return glfwWindowShouldClose(window_) == GLFW_TRUE;
}

void AppWindow::begin_frame(float r, float g, float b) {
    glfwPollEvents();

    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(window_, &width, &height);
    glViewport(0, 0, width, height);
    glClearColor(r, g, b, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
}

void AppWindow::end_frame() {
    glfwSwapBuffers(window_);
}

void AppWindow::framebuffer_size(int& width, int& height) const {
    glfwGetFramebufferSize(window_, &width, &height);
}

void AppWindow::window_size(int& width, int& height) const {
    glfwGetWindowSize(window_, &width, &height);
}

float AppWindow::content_scale() const {
    float xscale = 1.0F;
    float yscale = 1.0F;
    glfwGetWindowContentScale(window_, &xscale, &yscale);
    return xscale;
}

} // namespace nt::platform
