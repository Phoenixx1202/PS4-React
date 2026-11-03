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

constexpr int kSystemShared = 250 * 1024 * 1024;
constexpr int kVideoShared = 512 * 1024 * 1024;
constexpr int kFlexibleShared = 170 * 1024 * 1024;

struct ShaderBlob {
    char* ident;
    unsigned char hash[16];
    std::uint64_t length;
    unsigned char* code;
};

extern "C" ShaderBlob scePrecompiledShaderEntries[];

ShaderBlob* find_shader(const char* name) {
    for (ShaderBlob* entry = scePrecompiledShaderEntries; entry->ident != nullptr; ++entry)
        if (std::strcmp(entry->ident, name) == 0) return entry;
    return nullptr;
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
    ps4::system::log("gpu: Piglet module=%d start=0x%08x", piglet_, static_cast<unsigned>(start));
    if (piglet_ < 0) {
        ps4::system::log("gpu: load Piglet=0x%08x", static_cast<unsigned>(piglet_));
        piglet_ = -1;
        return false;
    }
    shaders_ = static_cast<int>(sceKernelLoadStartModule(shaders.c_str(), 0, nullptr, 0, nullptr, &start));
    ps4::system::log("gpu: shaders module=%d start=0x%08x", shaders_, static_cast<unsigned>(start));
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
    config.flags = ORBIS_PGL_FLAGS_USE_COMPOSITE_EXT | ORBIS_PGL_FLAGS_USE_FLEXIBLE_MEMORY | 0x60;
    config.processOrder = 1;
    config.systemSharedMemorySize = kSystemShared;
    config.videoSharedMemorySize = kVideoShared;
    config.maxMappedFlexibleMemory = kFlexibleShared;
    config.drawCommandBufferSize = 1024 * 1024;
    config.lcueResourceBufferSize = 1024 * 1024;
    config.dbgPosCmd_0x40 = static_cast<std::uint32_t>(width);
    config.dbgPosCmd_0x44 = static_cast<std::uint32_t>(height);
    config.dbgPosCmd_0x48 = 0;
    config.dbgPosCmd_0x4C = 0;
    config.unk_0x5C = 2;
    if (!scePigletSetConfigurationVSH(&config)) {
        ps4::system::log("gpu: scePigletSetConfigurationVSH failed");
        return false;
    }

    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) {
        ps4::system::log("gpu: eglGetDisplay failed (0x%08x)", static_cast<unsigned>(eglGetError()));
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
    // Runtime GLSL compilation is disabled in Target Manager on retail
    // consoles; use Sony's precompiled WebKit texture shaders instead.
    ShaderBlob* vertex_blob = find_shader("texmap/v_2.vert");
    ShaderBlob* fragment_blob = find_shader("texmap/f_2.frag");
    if (!vertex_blob || !fragment_blob) {
        ps4::system::log("gpu: precompiled texmap shaders are missing");
        return false;
    }
    const GLuint vertex = glCreateShader(GL_VERTEX_SHADER);
    const GLuint fragment = glCreateShader(GL_FRAGMENT_SHADER);
    if (!vertex || !fragment) return false;
    glShaderBinary(1, &vertex, 0, vertex_blob->code, static_cast<GLsizei>(vertex_blob->length));
    glShaderBinary(1, &fragment, 0, fragment_blob->code, static_cast<GLsizei>(fragment_blob->length));
    program_ = glCreateProgram();
    glAttachShader(program_, vertex);
    glAttachShader(program_, fragment);
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
    vertex_ = glGetAttribLocation(program_, "a_vertex");
    model_ = glGetUniformLocation(program_, "u_modelViewMatrix");
    projection_ = glGetUniformLocation(program_, "u_projectionMatrix");
    texture_space_ = glGetUniformLocation(program_, "u_textureSpaceMatrix");
    opacity_ = glGetUniformLocation(program_, "u_opacity");
    sampler_ = glGetUniformLocation(program_, "s_sampler");
    ps4::system::log("gpu: precompiled shaders ready (vertex=%d)", vertex_);
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
    staging_.resize(static_cast<std::size_t>(width) * height * 4);
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
    if (!pixels || !context_ || !program_ || !texture_ || staging_.empty()) return false;
    // The engine's framebuffer is 0xAARRGGBB; GL wants RGBA bytes. Piglet's
    // texmap shader presents the texture bottom-up, so upload the rows reversed.
    for (int y = 0; y < height_; ++y) {
        const std::uint32_t* row = pixels + static_cast<std::size_t>(height_ - 1 - y) * width_;
        for (int x = 0; x < width_; ++x) {
            const std::uint32_t pixel = row[x];
            const std::size_t at = (static_cast<std::size_t>(y) * width_ + x) * 4;
            staging_[at + 0] = static_cast<std::uint8_t>(pixel >> 16);
            staging_[at + 1] = static_cast<std::uint8_t>(pixel >> 8);
            staging_[at + 2] = static_cast<std::uint8_t>(pixel);
            staging_[at + 3] = static_cast<std::uint8_t>(pixel >> 24);
        }
    }
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width_, height_, GL_RGBA, GL_UNSIGNED_BYTE,
                    staging_.data());
    glClear(GL_COLOR_BUFFER_BIT);
    static const GLfloat vertices[] = {
        -1.0f,  1.0f, 0.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
        -1.0f, -1.0f, 0.0f, 0.0f,
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
    };
    static const GLfloat identity[] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1,
    };
    glUseProgram(program_);
    glUniformMatrix4fv(model_, 1, GL_FALSE, identity);
    glUniformMatrix4fv(projection_, 1, GL_FALSE, identity);
    glUniformMatrix4fv(texture_space_, 1, GL_FALSE, identity);
    glUniform1f(opacity_, 1.0f);
    glUniform1i(sampler_, 0);
    glEnableVertexAttribArray(static_cast<GLuint>(vertex_));
    glVertexAttribPointer(static_cast<GLuint>(vertex_), 4, GL_FLOAT, GL_FALSE,
                          sizeof(GLfloat) * 4, vertices);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    const bool ok = eglSwapBuffers(static_cast<EGLDisplay>(display_),
                                   static_cast<EGLSurface>(surface_));
    return ok;
}

} // namespace ps4
