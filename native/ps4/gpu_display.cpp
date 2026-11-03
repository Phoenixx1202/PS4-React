// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "gpu_display.hpp"

#include "system.hpp"

#include <orbis/Pigletv2VSH.h>
#include <orbis/libkernel.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace ps4 {
namespace {

constexpr int kSystemShared = 128 * 1024 * 1024;
constexpr int kVideoShared = 256 * 1024 * 1024;
constexpr int kFlexibleShared = 64 * 1024 * 1024;

constexpr const char* kVertexShader = R"(
attribute vec2 position;
attribute vec2 texcoord;
varying vec2 uv;
void main() {
    gl_Position = vec4(position, 0.0, 1.0);
    uv = texcoord;
}
)";

constexpr const char* kFragmentShader = R"(
precision mediump float;
varying vec2 uv;
uniform sampler2D frame;
void main() {
    vec4 pixel = texture2D(frame, uv);
    gl_FragColor = vec4(pixel.b, pixel.g, pixel.r, pixel.a);
}
)";

unsigned compile(GLenum type, const char* source) {
    const GLuint shader = glCreateShader(type);
    if (!shader) return 0;
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint status = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[512] = {};
        glGetShaderInfoLog(shader, sizeof log, nullptr, log);
        ps4::system::log("gpu: shader compile failed: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

} // namespace

GpuDisplay::~GpuDisplay() {
    close();
}

bool GpuDisplay::load_modules() {
    char prefix[256] = {};
    const char* random = sceKernelGetFsSandboxRandomWord();
    if (!random || !*random) return false;
    std::snprintf(prefix, sizeof prefix, "/%s/common/lib/", random);
    const std::string piglet = std::string(prefix) + "libScePigletv2VSH.sprx";
    const std::string shaders = std::string(prefix) + "libScePrecompiledShaders.sprx";
    int start = 0;
    piglet_ = static_cast<int>(sceKernelLoadStartModule(piglet.c_str(), 0, nullptr, 0, nullptr, &start));
    if (piglet_ < 0) {
        ps4::system::log("gpu: load Piglet=0x%08x", static_cast<unsigned>(piglet_));
        piglet_ = -1;
        return false;
    }
    shaders_ = static_cast<int>(sceKernelLoadStartModule(shaders.c_str(), 0, nullptr, 0, nullptr, &start));
    if (shaders_ < 0) {
        ps4::system::log("gpu: load precompiled shaders=0x%08x", static_cast<unsigned>(shaders_));
        shaders_ = -1;
        return false;
    }
    return true;
}

bool GpuDisplay::create_context(int width, int height) {
    OrbisPglConfig config{};
    config.size = sizeof config;
    config.flags = ORBIS_PGL_FLAGS_USE_COMPOSITE_EXT | ORBIS_PGL_FLAGS_USE_FLEXIBLE_MEMORY;
    config.processOrder = 1;
    config.systemSharedMemorySize = kSystemShared;
    config.videoSharedMemorySize = kVideoShared;
    config.maxMappedFlexibleMemory = kFlexibleShared;
    config.drawCommandBufferSize = 1024 * 1024;
    config.lcueResourceBufferSize = 1024 * 1024;
    config.unk_0x5C = 2;
    if (!scePigletSetConfigurationVSH(&config)) {
        ps4::system::log("gpu: scePigletSetConfigurationVSH failed");
        return false;
    }

    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) {
        ps4::system::log("gpu: eglGetDisplay failed");
        return false;
    }
    EGLint major = 0, minor = 0;
    if (!eglInitialize(display, &major, &minor)) {
        ps4::system::log("gpu: eglInitialize=0x%08x", static_cast<unsigned>(eglGetError()));
        return false;
    }
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        ps4::system::log("gpu: eglBindAPI=0x%08x", static_cast<unsigned>(eglGetError()));
        return false;
    }
    (void)eglSwapInterval(display, 0);

    const EGLint attributes[] = {
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0, EGL_SAMPLE_BUFFERS, 0, EGL_SAMPLES, 0,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_NONE,
    };
    EGLConfig egl_config = nullptr;
    EGLint count = 0;
    if (!eglChooseConfig(display, attributes, &egl_config, 1, &count) || count < 1) {
        ps4::system::log("gpu: eglChooseConfig=0x%08x", static_cast<unsigned>(eglGetError()));
        return false;
    }

    OrbisPglWindow window{0, static_cast<khronos_uint32_t>(width),
                          static_cast<khronos_uint32_t>(height)};
    const EGLint surface_attributes[] = {EGL_RENDER_BUFFER, EGL_BACK_BUFFER, EGL_NONE};
    EGLSurface surface = eglCreateWindowSurface(display, egl_config, &window, surface_attributes);
    if (surface == EGL_NO_SURFACE) {
        ps4::system::log("gpu: eglCreateWindowSurface=0x%08x", static_cast<unsigned>(eglGetError()));
        return false;
    }
    const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext context = eglCreateContext(display, egl_config, EGL_NO_CONTEXT, context_attributes);
    if (context == EGL_NO_CONTEXT) {
        ps4::system::log("gpu: eglCreateContext=0x%08x", static_cast<unsigned>(eglGetError()));
        return false;
    }
    if (!eglMakeCurrent(display, surface, surface, context)) {
        ps4::system::log("gpu: eglMakeCurrent=0x%08x", static_cast<unsigned>(eglGetError()));
        return false;
    }

    display_ = display;
    surface_ = surface;
    context_ = context;
    ps4::system::log("gpu: GL_VERSION=%s", reinterpret_cast<const char*>(glGetString(GL_VERSION)));
    return true;
}

bool GpuDisplay::create_program() {
    const GLuint vertex = compile(GL_VERTEX_SHADER, kVertexShader);
    const GLuint fragment = compile(GL_FRAGMENT_SHADER, kFragmentShader);
    if (!vertex || !fragment) return false;
    program_ = glCreateProgram();
    glAttachShader(program_, vertex);
    glAttachShader(program_, fragment);
    glBindAttribLocation(program_, 0, "position");
    glBindAttribLocation(program_, 1, "texcoord");
    glLinkProgram(program_);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint status = 0;
    glGetProgramiv(program_, GL_LINK_STATUS, &status);
    if (!status) {
        char log[512] = {};
        glGetProgramInfoLog(program_, sizeof log, nullptr, log);
        ps4::system::log("gpu: program link failed: %s", log);
        return false;
    }
    position_ = 0;
    uv_ = 1;
    sampler_ = glGetUniformLocation(program_, "frame");
    return true;
}

bool GpuDisplay::create_texture() {
    glGenTextures(1, &texture_);
    if (!texture_) return false;
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width_, height_, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    return glGetError() == GL_NO_ERROR;
}

bool GpuDisplay::open(int width, int height) {
    if (width <= 0 || height <= 0) return false;
    width_ = width;
    height_ = height;
    if (!load_modules() || !create_context(width, height) || !create_program() || !create_texture()) {
        close();
        return false;
    }
    glViewport(0, 0, width, height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glClearColor(0, 0, 0, 1);
    ps4::system::log("gpu: presentation ready at %dx%d", width, height);
    return true;
}

void GpuDisplay::close() {
    if (context_) {
        eglMakeCurrent(static_cast<EGLDisplay>(display_), EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
        if (texture_) glDeleteTextures(1, &texture_);
        if (program_) glDeleteProgram(program_);
        texture_ = 0;
        program_ = 0;
        if (surface_) eglDestroySurface(static_cast<EGLDisplay>(display_),
                                        static_cast<EGLSurface>(surface_));
        if (context_) eglDestroyContext(static_cast<EGLDisplay>(display_),
                                        static_cast<EGLContext>(context_));
        eglTerminate(static_cast<EGLDisplay>(display_));
    }
    display_ = nullptr;
    surface_ = nullptr;
    context_ = nullptr;
    if (shaders_ >= 0) sceKernelStopUnloadModule(shaders_, 0, nullptr, 0, nullptr, nullptr);
    if (piglet_ >= 0) sceKernelStopUnloadModule(piglet_, 0, nullptr, 0, nullptr, nullptr);
    shaders_ = -1;
    piglet_ = -1;
}

bool GpuDisplay::present(const std::uint32_t* pixels) {
    if (!pixels || !context_ || !program_ || !texture_) return false;
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width_, height_, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glClear(GL_COLOR_BUFFER_BIT);
    static const GLfloat vertices[] = {
        -1.0f, 1.0f, 0.0f, 0.0f,
         1.0f, 1.0f, 1.0f, 0.0f,
        -1.0f, -1.0f, 0.0f, 1.0f,
         1.0f, -1.0f, 1.0f, 1.0f,
    };
    glUseProgram(program_);
    glEnableVertexAttribArray(static_cast<GLuint>(position_));
    glEnableVertexAttribArray(static_cast<GLuint>(uv_));
    glVertexAttribPointer(static_cast<GLuint>(position_), 2, GL_FLOAT, GL_FALSE,
                          sizeof(GLfloat) * 4, vertices);
    glVertexAttribPointer(static_cast<GLuint>(uv_), 2, GL_FLOAT, GL_FALSE,
                          sizeof(GLfloat) * 4, vertices + 2);
    glUniform1i(sampler_, 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    const bool ok = eglSwapBuffers(static_cast<EGLDisplay>(display_),
                                   static_cast<EGLSurface>(surface_));
    return ok;
}

} // namespace ps4
