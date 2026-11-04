// GLES 3.0 backend of the RHI.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "core/log.hpp"
#include "rhi/rhi.hpp"

namespace alia::rhi {
namespace {

struct GlBuffer {
    GLuint id;
    GLenum target;
};
struct GlTexture {
    GLuint id;
    int width, height;
    Format format;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;  // an imported dmabuf
    Wrap wrap = Wrap::Clamp;
};

// Data textures (rhi.hpp): internal format, format, type.
struct DataFormat {
    GLenum internal, format, type;
    size_t texel;
};
DataFormat data_format(Format f) {
    switch (f) {
    case Format::R8: return {GL_R8, GL_RED, GL_UNSIGNED_BYTE, 1};
    case Format::R16F: return {GL_R16F, GL_RED, GL_FLOAT, 4};
    case Format::RGBA16F: return {GL_RGBA16F, GL_RGBA, GL_FLOAT, 16};
    default: return {0, 0, 0, 0};
    }
}
struct GlTarget {
    GLuint fbo;
    int width, height;
    bool has_depth;
};
struct GlPipeline {
    GLuint program;
    PipelineDesc desc;
};

GLuint compile(GLenum type, const char* src, std::string* error) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        *error = std::string(type == GL_VERTEX_SHADER ? "vertex" : "fragment") + " shader: " + log;
        glDeleteShader(s);
        return 0;
    }
    return s;
}

class GlesDevice final : public Device {
public:
    GlesDevice() {
        glGenBuffers(1, &ubo_);
        glBindBuffer(GL_UNIFORM_BUFFER, ubo_);
        glBufferData(GL_UNIFORM_BUFFER, kUboSize, nullptr, GL_DYNAMIC_DRAW);
        glGenVertexArrays(1, &vao_);
        glBindVertexArray(vao_);
        glGenSamplers(4, samplers_);
        for (int i = 0; i < 4; i++) {  // [filter + 2 * wrap]
            GLint f = i % 2 == 0 ? GL_NEAREST : GL_LINEAR, w = i < 2 ? GL_CLAMP_TO_EDGE : GL_REPEAT;
            glSamplerParameteri(samplers_[i], GL_TEXTURE_MIN_FILTER, f);
            glSamplerParameteri(samplers_[i], GL_TEXTURE_MAG_FILTER, f);
            glSamplerParameteri(samplers_[i], GL_TEXTURE_WRAP_S, w);
            glSamplerParameteri(samplers_[i], GL_TEXTURE_WRAP_T, w);
        }
    }

    ~GlesDevice() override {
        for (auto& [k, b] : buffers_) glDeleteBuffers(1, &b.id);
        for (auto& [k, t] : textures_) {
            glDeleteTextures(1, &t.id);
            destroy_image(t.image);
        }
        for (auto& [k, f] : fences_) glDeleteSync(f);
        for (auto& [k, t] : targets_) glDeleteFramebuffers(1, &t.fbo);
        for (auto& [k, p] : pipelines_) glDeleteProgram(p.program);
        glDeleteBuffers(1, &ubo_);
        glDeleteVertexArrays(1, &vao_);
        glDeleteSamplers(4, samplers_);
    }

    BufferH create_buffer(const BufferDesc& d) override {
        GlBuffer b{0, d.usage == BufferUsage::Index16 ? GLenum(GL_ELEMENT_ARRAY_BUFFER) : GLenum(GL_ARRAY_BUFFER)};
        glGenBuffers(1, &b.id);
        glBindBuffer(b.target, b.id);
        glBufferData(b.target, GLsizeiptr(d.size), d.data, d.dynamic ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
        BufferH h{next_++};
        buffers_[h.id] = b;
        return h;
    }

    void update_buffer(BufferH h, const void* data, size_t size, size_t offset) override {
        auto& b = buffers_.at(h.id);
        glBindBuffer(b.target, b.id);
        glBufferSubData(b.target, GLintptr(offset), GLsizeiptr(size), data);
    }

    TextureH create_texture(const TextureDesc& d) override {
        GlTexture t{0, d.width, d.height, d.format};
        t.wrap = d.wrap;
        glGenTextures(1, &t.id);
        glBindTexture(GL_TEXTURE_2D, t.id);
        GLint filter = d.filter == Filter::Linear ? GL_LINEAR : GL_NEAREST;
        GLint wrap = d.wrap == Wrap::Repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
        switch (d.format) {
        case Format::RGBA8:
            if (d.pixels) {
                // GL's first row is the bottom one; flip so the API stays top-row-first.
                std::vector<uint8_t> flipped(size_t(d.width) * d.height * 4);
                const auto* src = static_cast<const uint8_t*>(d.pixels);
                size_t row = size_t(d.width) * 4;
                for (int y = 0; y < d.height; y++)
                    std::memcpy(&flipped[(d.height - 1 - y) * row], src + y * row, row);
                glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, d.width, d.height);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, d.width, d.height, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data());
            } else {
                glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, d.width, d.height);
            }
            break;
        case Format::Depth24:
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH_COMPONENT24, d.width, d.height);
            break;
        case Format::Depth32F:
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_DEPTH_COMPONENT32F, d.width, d.height);
            break;
        case Format::R8:
        case Format::R16F:
        case Format::RGBA16F: {
            const DataFormat f = data_format(d.format);
            glTexStorage2D(GL_TEXTURE_2D, 1, f.internal, d.width, d.height);
            if (d.pixels) {
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, d.width, d.height, f.format, f.type, d.pixels);
            }
            break;
        }
        }
        TextureH h{next_++};
        textures_[h.id] = t;
        return h;
    }

    void update_texture(TextureH h, const void* pixels) override {
        const auto& t = textures_.at(h.id);
        if (t.format != Format::RGBA8) return;
        std::vector<uint8_t> flipped(size_t(t.width) * t.height * 4);
        const auto* src = static_cast<const uint8_t*>(pixels);
        size_t row = size_t(t.width) * 4;
        for (int y = 0; y < t.height; y++) std::memcpy(&flipped[(t.height - 1 - y) * row], src + y * row, row);
        glBindTexture(GL_TEXTURE_2D, t.id);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, t.width, t.height, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data());
    }

    void update_texture_rect(TextureH h, int x, int y, int w, int hgt, const void* pixels) override {
        const auto& t = textures_.at(h.id);
        if (t.format != Format::RGBA8 || x < 0 || y < 0 || w <= 0 || hgt <= 0 || x + w > t.width || y + hgt > t.height) return;
        // GL's first row is the bottom one: the rect's rows reversed, placed from the bottom.
        std::vector<uint8_t> flipped(size_t(w) * hgt * 4);
        const auto* src = static_cast<const uint8_t*>(pixels);
        const size_t row = size_t(w) * 4;
        for (int r = 0; r < hgt; r++) std::memcpy(&flipped[(hgt - 1 - r) * row], src + r * row, row);
        glBindTexture(GL_TEXTURE_2D, t.id);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, t.height - y - hgt, w, hgt, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data());
    }

    void update_data_texture(TextureH h, const void* data, size_t row_bytes) override {
        const auto& t = textures_.at(h.id);
        const DataFormat f = data_format(t.format);
        if (!f.internal || t.image != EGL_NO_IMAGE_KHR) return;
        glBindTexture(GL_TEXTURE_2D, t.id);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, row_bytes ? GLint(row_bytes / f.texel) : 0);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, t.width, t.height, f.format, f.type, data);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }

    TextureH import_dmabuf(const DmabufDesc& d, std::string* error) override {
        EGLDisplay dpy = eglGetCurrentDisplay();
        auto create = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
        auto target = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
        const char* ext = dpy != EGL_NO_DISPLAY ? eglQueryString(dpy, EGL_EXTENSIONS) : nullptr;
        if (!create || !target || !ext || !std::strstr(ext, "EGL_EXT_image_dma_buf_import")) {
            *error = "no EGL_EXT_image_dma_buf_import";
            return {};
        }
        const bool modifiers = std::strstr(ext, "EGL_EXT_image_dma_buf_import_modifiers") != nullptr;
        constexpr EGLint kR8 = 0x20203852;  // DRM_FORMAT_R8: fourcc 'R', '8', ' ', ' '
        EGLint attrs[] = {EGL_WIDTH, d.width, EGL_HEIGHT, d.height, EGL_LINUX_DRM_FOURCC_EXT, kR8,
                          EGL_DMA_BUF_PLANE0_FD_EXT, d.fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGLint(d.offset),
                          EGL_DMA_BUF_PLANE0_PITCH_EXT, EGLint(d.stride),
                          EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGLint(uint32_t(d.modifier)),
                          EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGLint(uint32_t(d.modifier >> 32)), EGL_NONE};
        if (!modifiers) attrs[14] = EGL_NONE;  // linear only: leave the modifier out
        EGLImageKHR image = create(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
        if (image == EGL_NO_IMAGE_KHR) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "eglCreateImageKHR: 0x%x", eglGetError());
            *error = buf;
            return {};
        }
        GlTexture t{0, d.width, d.height, Format::R8, image};
        glGenTextures(1, &t.id);
        glBindTexture(GL_TEXTURE_2D, t.id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        while (glGetError() != GL_NO_ERROR) {}
        target(GL_TEXTURE_2D, image);
        if (GLenum e = glGetError(); e != GL_NO_ERROR) {
            glDeleteTextures(1, &t.id);
            destroy_image(image);
            char buf[64];
            std::snprintf(buf, sizeof buf, "glEGLImageTargetTexture2DOES: 0x%x", e);
            *error = buf;
            return {};
        }
        TextureH h{next_++};
        textures_[h.id] = t;
        return h;
    }

    FenceH fence() override {
        FenceH h{next_++};
        fences_[h.id] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        glFlush();  // so the fence reaches the GPU without waiting for the next swap
        return h;
    }
    bool signaled(FenceH h) override {
        auto it = fences_.find(h.id);
        if (it == fences_.end()) return true;
        GLint status = GL_UNSIGNALED;
        glGetSynciv(it->second, GL_SYNC_STATUS, 1, nullptr, &status);
        return status == GL_SIGNALED;
    }
    void destroy(FenceH h) override {
        if (auto it = fences_.find(h.id); it != fences_.end()) {
            glDeleteSync(it->second);
            fences_.erase(it);
        }
    }

    TargetH create_target(const TargetDesc& d) override {
        const auto& color = textures_.at(d.color.id);
        GlTarget t{0, color.width, color.height, bool(d.depth)};
        glGenFramebuffers(1, &t.fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color.id, 0);
        if (d.depth)
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, textures_.at(d.depth.id).id, 0);
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) ALIA_ERROR("framebuffer incomplete: 0x%x", status);
        TargetH h{next_++};
        targets_[h.id] = t;
        return h;
    }

    PipelineH create_pipeline(const PipelineDesc& d, std::string* error) override {
        GLuint vs = compile(GL_VERTEX_SHADER, d.vertex_source, error);
        if (!vs) return {};
        GLuint fs = compile(GL_FRAGMENT_SHADER, d.fragment_source, error);
        if (!fs) {
            glDeleteShader(vs);
            return {};
        }
        GLuint prog = glCreateProgram();
        glAttachShader(prog, vs);
        glAttachShader(prog, fs);
        for (int i = 0; i < d.attrib_count; i++) {
            char name[16];
            std::snprintf(name, sizeof name, "a_%u", d.attribs[i].location);
            glBindAttribLocation(prog, d.attribs[i].location, name);
        }
        glLinkProgram(prog);
        glDeleteShader(vs);
        glDeleteShader(fs);
        GLint ok = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[2048];
            glGetProgramInfoLog(prog, sizeof log, nullptr, log);
            *error = std::string(d.name) + ": link: " + log;
            glDeleteProgram(prog);
            return {};
        }
        // Fixed conventions (rhi.hpp): "Params" block on binding 0, u_texN on slot N.
        GLuint block = glGetUniformBlockIndex(prog, "Params");
        if (block != GL_INVALID_INDEX) glUniformBlockBinding(prog, block, 0);
        glUseProgram(prog);
        for (int i = 0; i < 4; i++) {
            char name[16];
            std::snprintf(name, sizeof name, "u_tex%d", i);
            GLint loc = glGetUniformLocation(prog, name);
            if (loc >= 0) glUniform1i(loc, i);
        }
        PipelineH h{next_++};
        pipelines_[h.id] = {prog, d};
        return h;
    }

    void destroy(BufferH h) override { if (auto it = buffers_.find(h.id); it != buffers_.end()) { glDeleteBuffers(1, &it->second.id); buffers_.erase(it); } }
    void destroy(TextureH h) override {
        if (auto it = textures_.find(h.id); it != textures_.end()) {
            glDeleteTextures(1, &it->second.id);
            destroy_image(it->second.image);
            textures_.erase(it);
        }
    }
    void destroy(TargetH h) override { if (auto it = targets_.find(h.id); it != targets_.end()) { glDeleteFramebuffers(1, &it->second.fbo); targets_.erase(it); } }
    void destroy(PipelineH h) override { if (auto it = pipelines_.find(h.id); it != pipelines_.end()) { glDeleteProgram(it->second.program); pipelines_.erase(it); } }

    void begin_pass(const PassDesc& d) override {
        pass_ = d;
        if (d.target) {
            const auto& t = targets_.at(d.target.id);
            glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
            pass_w_ = t.width;
            pass_h_ = t.height;
            pass_depth_ = t.has_depth;
        } else {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            pass_w_ = d.width;
            pass_h_ = d.height;
            pass_depth_ = false;
        }
        set_viewport({0, 0, pass_w_, pass_h_});
        // DontCare clears too: on a tile GPU a clear is free and avoids loading old tiles.
        GLbitfield clear = 0;
        if (d.color_load != LoadOp::Load) {
            glClearColor(d.clear_color[0], d.clear_color[1], d.clear_color[2], d.clear_color[3]);
            clear |= GL_COLOR_BUFFER_BIT;
        }
        if (pass_depth_ && d.depth_load != LoadOp::Load) {
            glDepthMask(GL_TRUE);
            glClearDepthf(d.clear_depth);
            clear |= GL_DEPTH_BUFFER_BIT;
        }
        glDisable(GL_SCISSOR_TEST);
        if (clear) glClear(clear);
        stats_.passes++;
    }

    void set_viewport(const Viewport& vp) override {
        // API is top-left origin; GL's window coordinates are bottom-left.
        glViewport(vp.x, pass_h_ - vp.y - vp.height, vp.width, vp.height);
    }

    void set_pipeline(PipelineH h) override {
        const auto& p = pipelines_.at(h.id);
        glUseProgram(p.program);
        const auto& d = p.desc;
        if (d.depth_test) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
        glDepthMask(d.depth_write ? GL_TRUE : GL_FALSE);
        if (d.alpha_blend) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        } else {
            glDisable(GL_BLEND);
        }
        if (d.cull_back) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
        current_ = &p;
        attribs_dirty_ = true;
    }

    void set_vertex_buffer(BufferH h) override {
        vertex_buffer_ = buffers_.at(h.id).id;
        attribs_dirty_ = true;
    }

    void set_instance_buffer(BufferH h, size_t offset) override {
        instance_buffer_ = buffers_.at(h.id).id;
        instance_offset_ = offset;
        attribs_dirty_ = true;
    }

    void set_index_buffer(BufferH h) override { glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers_.at(h.id).id); }

    void set_texture(int slot, TextureH h) override {
        glActiveTexture(GLenum(GL_TEXTURE0 + slot));
        glBindTexture(GL_TEXTURE_2D, textures_.at(h.id).id);
        glBindSampler(GLuint(slot), 0);  // the texture's own filter
    }

    void set_texture(int slot, TextureH h, Filter filter) override {
        const GlTexture& t = textures_.at(h.id);
        glActiveTexture(GLenum(GL_TEXTURE0 + slot));
        glBindTexture(GL_TEXTURE_2D, t.id);
        glBindSampler(GLuint(slot), samplers_[(filter == Filter::Linear ? 1 : 0) + (t.wrap == Wrap::Repeat ? 2 : 0)]);
    }

    void set_uniforms(const void* data, size_t size) override {
        glBindBuffer(GL_UNIFORM_BUFFER, ubo_);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, GLsizeiptr(std::min(size, kUboSize)), data);
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, ubo_);
    }

    void draw(uint32_t count, uint32_t first) override {
        apply_attribs();
        glDrawArrays(GL_TRIANGLES, GLint(first), GLsizei(count));
        stats_.draws++;
        stats_.triangles += count / 3;
    }

    void draw_instanced(uint32_t count, uint32_t instances, uint32_t first) override {
        apply_attribs();
        glDrawArraysInstanced(GL_TRIANGLES, GLint(first), GLsizei(count), GLsizei(instances));
        stats_.draws++;
        stats_.triangles += count / 3 * instances;
    }

    void draw_indexed(uint32_t count, uint32_t first) override {
        apply_attribs();
        glDrawElements(GL_TRIANGLES, GLsizei(count), GL_UNSIGNED_SHORT,
                       reinterpret_cast<const void*>(uintptr_t(first) * 2));
        stats_.draws++;
        stats_.triangles += count / 3;
    }

    void end_pass() override {
        // Tell the driver which attachments needn't be written back to memory.
        GLenum discard[2];
        int n = 0;
        bool fbo = bool(pass_.target);
        if (pass_.color_store == StoreOp::DontCare) discard[n++] = fbo ? GL_COLOR_ATTACHMENT0 : GL_COLOR;
        if (pass_depth_ && pass_.depth_store == StoreOp::DontCare) discard[n++] = fbo ? GL_DEPTH_ATTACHMENT : GL_DEPTH;
        if (n) glInvalidateFramebuffer(GL_FRAMEBUFFER, n, discard);
        current_ = nullptr;
    }

    void read_pixels(TargetH h, int w, int hgt, uint8_t* rgba) override {
        glBindFramebuffer(GL_FRAMEBUFFER, h ? targets_.at(h.id).fbo : 0);
        std::vector<uint8_t> tmp(size_t(w) * hgt * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, w, hgt, GL_RGBA, GL_UNSIGNED_BYTE, tmp.data());
        size_t row = size_t(w) * 4;
        for (int y = 0; y < hgt; y++) std::memcpy(rgba + y * row, &tmp[(hgt - 1 - y) * row], row);
    }

    FrameStats take_stats() override {
        FrameStats s = stats_;
        stats_ = {};
        return s;
    }

    std::string describe() const override {
        auto str = [](GLenum e) { auto* s = glGetString(e); return s ? reinterpret_cast<const char*>(s) : "?"; };
        return std::string(str(GL_RENDERER)) + " | " + str(GL_VERSION);
    }

private:
    static void destroy_image(EGLImageKHR image) {
        if (image == EGL_NO_IMAGE_KHR) return;
        auto destroy = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
        if (destroy) destroy(eglGetCurrentDisplay(), image);
    }

    void apply_attribs() {
        if (!attribs_dirty_ || !current_) return;
        const auto& d = current_->desc;
        for (int i = 0; i < kMaxAttribs; i++) glDisableVertexAttribArray(GLuint(i));
        for (int i = 0; i < d.attrib_count; i++) {
            const auto& a = d.attribs[i];
            glBindBuffer(GL_ARRAY_BUFFER, a.per_instance ? instance_buffer_ : vertex_buffer_);
            glEnableVertexAttribArray(a.location);
            glVertexAttribDivisor(a.location, a.per_instance ? 1 : 0);
            const GLsizei stride = GLsizei(a.per_instance ? d.instance_stride : d.stride);
            const void* off = reinterpret_cast<const void*>(uintptr_t(a.offset + (a.per_instance ? instance_offset_ : 0)));
            switch (a.type) {
            case AttribType::Float2: glVertexAttribPointer(a.location, 2, GL_FLOAT, GL_FALSE, stride, off); break;
            case AttribType::Float3: glVertexAttribPointer(a.location, 3, GL_FLOAT, GL_FALSE, stride, off); break;
            case AttribType::Float4: glVertexAttribPointer(a.location, 4, GL_FLOAT, GL_FALSE, stride, off); break;
            case AttribType::UNorm8x4: glVertexAttribPointer(a.location, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, off); break;
            }
        }
        attribs_dirty_ = false;
    }

    static constexpr size_t kUboSize = 1024;
    uint32_t next_ = 1;
    std::unordered_map<uint32_t, GlBuffer> buffers_;
    std::unordered_map<uint32_t, GlTexture> textures_;
    std::unordered_map<uint32_t, GLsync> fences_;
    std::unordered_map<uint32_t, GlTarget> targets_;
    std::unordered_map<uint32_t, GlPipeline> pipelines_;
    GLuint ubo_ = 0, vao_ = 0;
    GLuint samplers_[4] = {};  // nearest, linear
    PassDesc pass_;
    int pass_w_ = 0, pass_h_ = 0;
    bool pass_depth_ = false;
    const GlPipeline* current_ = nullptr;
    GLuint vertex_buffer_ = 0, instance_buffer_ = 0;
    size_t instance_offset_ = 0;
    bool attribs_dirty_ = true;
    FrameStats stats_;
};

}  // namespace

std::unique_ptr<Device> create_gles_device() { return std::make_unique<GlesDevice>(); }

}  // namespace alia::rhi
