// OpenGL 4.5 core implementation of the RHI. This and platform/glfw are the only files that include
// glad; a Vulkan backend would be a sibling of this file.
#include "rhi/gl/GlDevice.h"
#include "platform/Window.h"
#include "core/Log.h"
#include <glad/gl.h>
#include <imgui.h>
#include <backends/imgui_impl_opengl3.h>
#include <unordered_map>

namespace glue::rhi {
namespace {

// ---- format tables --------------------------------------------------------------------------
GLenum glInternalFormat(Format f) {
    switch (f) {
        case Format::R8: return GL_R8;
        case Format::RGBA8: return GL_RGBA8;
        case Format::R32F: return GL_R32F;
        case Format::RGBA16F: return GL_RGBA16F;
        case Format::RGBA32F: return GL_RGBA32F;
        case Format::Depth32F: return GL_DEPTH_COMPONENT32F;
    }
    return GL_RGBA8;
}
void glUploadFormat(Format f, GLenum& format, GLenum& type) {
    switch (f) {
        case Format::R8: format = GL_RED; type = GL_UNSIGNED_BYTE; break;
        case Format::RGBA8: format = GL_RGBA; type = GL_UNSIGNED_BYTE; break;
        case Format::R32F: format = GL_RED; type = GL_FLOAT; break;
        case Format::RGBA16F: format = GL_RGBA; type = GL_HALF_FLOAT; break;
        case Format::RGBA32F: format = GL_RGBA; type = GL_FLOAT; break;
        case Format::Depth32F: format = GL_DEPTH_COMPONENT; type = GL_FLOAT; break;
    }
}
GLenum glFilter(Filter f, bool mips) {
    if (f == Filter::Nearest) return mips ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST;
    return mips ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR;
}
GLenum glTopology(Topology t) {
    switch (t) {
        case Topology::Triangles: return GL_TRIANGLES;
        case Topology::TriangleStrip: return GL_TRIANGLE_STRIP;
        case Topology::Lines: return GL_LINES;
        case Topology::Points: return GL_POINTS;
    }
    return GL_TRIANGLES;
}

// ---- buffer ---------------------------------------------------------------------------------
class GlBuffer final : public Buffer {
public:
    explicit GlBuffer(const BufferDesc& d) : size_(d.size) {
        glCreateBuffers(1, &id_);
        glNamedBufferData(id_, (GLsizeiptr)size_, d.initialData, GL_DYNAMIC_DRAW);
    }
    ~GlBuffer() override { if (id_) glDeleteBuffers(1, &id_); }
    size_t size() const override { return size_; }
    void write(size_t off, const void* p, size_t n) override { glNamedBufferSubData(id_, (GLintptr)off, (GLsizeiptr)n, p); }
    void read(size_t off, void* out, size_t n) override { glGetNamedBufferSubData(id_, (GLintptr)off, (GLsizeiptr)n, out); }
    void resizeDiscard(size_t n) override { size_ = n; glNamedBufferData(id_, (GLsizeiptr)n, nullptr, GL_DYNAMIC_DRAW); }
    GLuint id() const { return id_; }
private:
    GLuint id_ = 0;
    size_t size_ = 0;
};

// ---- texture --------------------------------------------------------------------------------
class GlTexture final : public Texture {
public:
    explicit GlTexture(const TextureDesc& d) : w_(d.width), h_(d.height), fmt_(d.format), mips_(d.mipmaps) {
        glCreateTextures(GL_TEXTURE_2D, 1, &id_);
        GLsizei levels = 1;
        if (mips_) { int m = std::max(w_, h_); levels = 1; while (m > 1) { m >>= 1; ++levels; } }
        glTextureStorage2D(id_, levels, glInternalFormat(fmt_), w_, h_);
        glTextureParameteri(id_, GL_TEXTURE_MIN_FILTER, (GLint)glFilter(d.minFilter, mips_));
        glTextureParameteri(id_, GL_TEXTURE_MAG_FILTER, (GLint)glFilter(d.magFilter, false));
        GLenum wrap = d.wrap == Wrap::Repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
        glTextureParameteri(id_, GL_TEXTURE_WRAP_S, (GLint)wrap);
        glTextureParameteri(id_, GL_TEXTURE_WRAP_T, (GLint)wrap);
        if (d.initialData) { write(d.initialData); }
    }
    ~GlTexture() override { if (id_) glDeleteTextures(1, &id_); }
    int width() const override { return w_; }
    int height() const override { return h_; }
    Format format() const override { return fmt_; }
    void write(const void* pixels) override {
        GLenum f = GL_RGBA, t = GL_UNSIGNED_BYTE;
        glUploadFormat(fmt_, f, t);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTextureSubImage2D(id_, 0, 0, 0, w_, h_, f, t, pixels);
        if (mips_) glGenerateTextureMipmap(id_);
    }
    void generateMipmaps() override { if (mips_) glGenerateTextureMipmap(id_); }
    uint64_t imguiId() const override { return id_; }
    GLuint id() const { return id_; }
private:
    GLuint id_ = 0;
    int w_, h_;
    Format fmt_;
    bool mips_;
};

class GlRenderTarget final : public RenderTarget {
public:
    explicit GlRenderTarget(const RenderTargetDesc& d) : w_(d.width), h_(d.height) {
        TextureDesc c; c.width = w_; c.height = h_; c.format = d.colorFormat;
        c.wrap = Wrap::ClampToEdge; c.debugName = d.debugName;
        color_ = std::make_unique<GlTexture>(c);
        if (d.depth) {
            TextureDesc z; z.width = w_; z.height = h_; z.format = Format::Depth32F;
            z.minFilter = z.magFilter = Filter::Nearest; z.wrap = Wrap::ClampToEdge;
            depth_ = std::make_unique<GlTexture>(z);
        }
        glCreateFramebuffers(1, &fbo_);
        glNamedFramebufferTexture(fbo_, GL_COLOR_ATTACHMENT0, color_->id(), 0);
        if (depth_) glNamedFramebufferTexture(fbo_, GL_DEPTH_ATTACHMENT, depth_->id(), 0);
    }
    ~GlRenderTarget() override { if (fbo_) glDeleteFramebuffers(1, &fbo_); }
    int width() const override { return w_; }
    int height() const override { return h_; }
    Texture& color() override { return *color_; }
    Texture* depth() override { return depth_.get(); }
    GLuint fbo() const { return fbo_; }
private:
    int w_, h_;
    GLuint fbo_ = 0;
    std::unique_ptr<GlTexture> color_, depth_;
};

// ---- pipeline -------------------------------------------------------------------------------
GLuint compileStage(GLenum type, const std::string& src, std::string& err) {
    GLuint s = glCreateShader(type);
    const char* p = src.c_str();
    glShaderSource(s, 1, &p, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (ok) return s;
    GLint n = 0;
    glGetShaderiv(s, GL_INFO_LOG_LENGTH, &n);
    std::vector<char> log(n > 1 ? n : 1);
    glGetShaderInfoLog(s, n, nullptr, log.data());
    err = log.data();
    glDeleteShader(s);
    return 0;
}

GLuint linkProgram(GLuint a, GLuint b, std::string& err) {
    GLuint p = glCreateProgram();
    glAttachShader(p, a);
    if (b) glAttachShader(p, b);
    glLinkProgram(p);
    glDeleteShader(a);
    if (b) glDeleteShader(b);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (ok) return p;
    GLint n = 0;
    glGetProgramiv(p, GL_INFO_LOG_LENGTH, &n);
    std::vector<char> log(n > 1 ? n : 1);
    glGetProgramInfoLog(p, n, nullptr, log.data());
    err = log.data();
    glDeleteProgram(p);
    return 0;
}

class GlPipeline final : public Pipeline {
public:
    GlPipeline(GLuint program, bool compute) : program_(program), compute_(compute) {}
    ~GlPipeline() override {
        if (program_) glDeleteProgram(program_);
        if (vao_) glDeleteVertexArrays(1, &vao_);
    }
    void initGraphics(const GraphicsPipelineDesc& d) {
        desc_ = d;
        glCreateVertexArrays(1, &vao_);
        for (const auto& a : d.vertexLayout.attributes) {
            GLint comps = a.format == VertexFormat::Float ? 1 : a.format == VertexFormat::Float2 ? 2
                        : a.format == VertexFormat::Float3 ? 3 : 4;
            glEnableVertexArrayAttrib(vao_, a.location);
            glVertexArrayAttribFormat(vao_, a.location, comps, GL_FLOAT, GL_FALSE, a.offset);
            glVertexArrayAttribBinding(vao_, a.location, 0);
        }
    }
    bool isCompute() const override { return compute_; }
    GLint loc(const char* n) {
        auto it = locs_.find(n);
        if (it != locs_.end()) return it->second;
        GLint l = glGetUniformLocation(program_, n);
        locs_.emplace(n, l);
        return l;
    }
    void set(const char* n, float v) override { glProgramUniform1f(program_, loc(n), v); }
    void set(const char* n, int v) override { glProgramUniform1i(program_, loc(n), v); }
    void set(const char* n, uint32_t v) override { glProgramUniform1ui(program_, loc(n), v); }
    void set(const char* n, float x, float y) override { glProgramUniform2f(program_, loc(n), x, y); }
    void set(const char* n, float x, float y, float z) override { glProgramUniform3f(program_, loc(n), x, y, z); }
    void set(const char* n, float x, float y, float z, float w) override { glProgramUniform4f(program_, loc(n), x, y, z, w); }
    void set(const char* n, const Mat4& m) override { glProgramUniformMatrix4fv(program_, loc(n), 1, GL_FALSE, m.data()); }

    GLuint program() const { return program_; }
    GLuint vao() const { return vao_; }
    uint32_t stride() const { return desc_.vertexLayout.stride; }
    const GraphicsPipelineDesc& desc() const { return desc_; }
private:
    GLuint program_ = 0, vao_ = 0;
    bool compute_ = false;
    GraphicsPipelineDesc desc_;
    std::unordered_map<std::string, GLint> locs_;
};

class GlTimer final : public GpuTimer {
public:
    GlTimer() { glCreateQueries(GL_TIME_ELAPSED, 1, &q_); }
    ~GlTimer() override { if (q_) glDeleteQueries(1, &q_); }
    uint64_t resultNanoseconds() override {
        GLuint64 ns = 0;
        glGetQueryObjectui64v(q_, GL_QUERY_RESULT, &ns);
        return ns;
    }
    GLuint id() const { return q_; }
private:
    GLuint q_ = 0;
};

// ---- context --------------------------------------------------------------------------------
class GlContext final : public CommandContext {
public:
    explicit GlContext(platform::Window* w) : win_(w) {}

    void beginRenderPass(RenderTarget* target, const RenderPassDesc& p) override {
        auto* t = static_cast<GlRenderTarget*>(target);
        glBindFramebuffer(GL_FRAMEBUFFER, t ? t->fbo() : 0);
        int w, h;
        if (t) { w = t->width(); h = t->height(); } else win_->framebufferSize(w, h);
        glViewport(0, 0, w, h);
        GLbitfield bits = 0;
        if (p.clearColor) {
            glClearColor(p.clearColorValue[0], p.clearColorValue[1], p.clearColorValue[2], p.clearColorValue[3]);
            bits |= GL_COLOR_BUFFER_BIT;
        }
        if (p.clearDepth) {
            glDepthMask(GL_TRUE);                // a previous pipeline may have switched depth writes off
            glClearDepth(p.clearDepthValue);
            bits |= GL_DEPTH_BUFFER_BIT;
        }
        if (bits) glClear(bits);
    }
    void endRenderPass() override { glBindFramebuffer(GL_FRAMEBUFFER, 0); pipeline_ = nullptr; }
    void setViewport(int x, int y, int w, int h) override { glViewport(x, y, w, h); }

    void bindPipeline(Pipeline& p) override {
        auto& gp = static_cast<GlPipeline&>(p);
        pipeline_ = &gp;
        glUseProgram(gp.program());
        if (gp.isCompute()) return;
        const auto& d = gp.desc();
        glBindVertexArray(gp.vao());
        if (d.depth.test) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
        glDepthMask(d.depth.write ? GL_TRUE : GL_FALSE);
        if (d.polygonOffset) { glEnable(GL_POLYGON_OFFSET_FILL); glPolygonOffset(d.polygonOffsetFactor, d.polygonOffsetUnits); }
        else glDisable(GL_POLYGON_OFFSET_FILL);
        topology_ = glTopology(d.topology);
    }
    void bindVertexBuffer(Buffer& b, size_t off) override {
        glVertexArrayVertexBuffer(pipeline_->vao(), 0, static_cast<GlBuffer&>(b).id(), (GLintptr)off, pipeline_->stride());
    }
    void bindIndexBuffer(Buffer& b, IndexType t) override {
        glVertexArrayElementBuffer(pipeline_->vao(), static_cast<GlBuffer&>(b).id());
        indexType_ = t == IndexType::U16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
    }
    void bindTexture(uint32_t slot, Texture& t) override { glBindTextureUnit(slot, static_cast<GlTexture&>(t).id()); }
    void bindStorageBuffer(uint32_t slot, Buffer& b) override {
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, slot, static_cast<GlBuffer&>(b).id());
    }
    void bindStorageImage(uint32_t slot, Texture& t, Access a) override {
        GLenum acc = a == Access::Read ? GL_READ_ONLY : a == Access::Write ? GL_WRITE_ONLY : GL_READ_WRITE;
        auto& gt = static_cast<GlTexture&>(t);
        glBindImageTexture(slot, gt.id(), 0, GL_FALSE, 0, acc, glInternalFormat(gt.format()));
    }

    void draw(uint32_t n, uint32_t inst, uint32_t first) override {
        if (inst == 1) glDrawArrays(topology_, (GLint)first, (GLsizei)n);
        else glDrawArraysInstanced(topology_, (GLint)first, (GLsizei)n, (GLsizei)inst);
    }
    void drawIndexed(uint32_t n, uint32_t inst, uint32_t first) override {
        size_t isz = indexType_ == GL_UNSIGNED_SHORT ? 2 : 4;
        const void* off = (const void*)(uintptr_t)(first * isz);
        if (inst == 1) glDrawElements(topology_, (GLsizei)n, indexType_, off);
        else glDrawElementsInstanced(topology_, (GLsizei)n, indexType_, off, (GLsizei)inst);
    }
    void dispatch(uint32_t x, uint32_t y, uint32_t z) override { glDispatchCompute(x, y, z); }
    void barrier(Barrier b) override {
        GLbitfield m = 0;
        if (uint32_t(b) & uint32_t(Barrier::StorageBuffer)) m |= GL_SHADER_STORAGE_BARRIER_BIT;
        if (uint32_t(b) & uint32_t(Barrier::StorageImage)) m |= GL_SHADER_IMAGE_ACCESS_BARRIER_BIT;
        if (uint32_t(b) & uint32_t(Barrier::TextureFetch)) m |= GL_TEXTURE_FETCH_BARRIER_BIT;
        if (uint32_t(b) & uint32_t(Barrier::BufferUpdate)) m |= GL_BUFFER_UPDATE_BARRIER_BIT;
        glMemoryBarrier(m);
    }
    void beginTimer(GpuTimer& t) override { glBeginQuery(GL_TIME_ELAPSED, static_cast<GlTimer&>(t).id()); }
    void endTimer(GpuTimer&) override { glEndQuery(GL_TIME_ELAPSED); }
    void waitIdle() override { glFinish(); }

    std::vector<uint8_t> readColorRgba8(RenderTarget* target) override {
        auto* t = static_cast<GlRenderTarget*>(target);
        int w, h;
        if (t) { w = t->width(); h = t->height(); } else win_->framebufferSize(w, h);
        std::vector<uint8_t> px((size_t)w * h * 4);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, t ? t->fbo() : 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        return px;
    }

private:
    platform::Window* win_;
    GlPipeline* pipeline_ = nullptr;
    GLenum topology_ = GL_TRIANGLES;
    GLenum indexType_ = GL_UNSIGNED_INT;
};

void glDebug(GLenum, GLenum type, GLuint, GLenum severity, GLsizei len, const char* msg, const void*) {
    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) return;
    Log::write(type == GL_DEBUG_TYPE_ERROR ? "gl-error" : "gl", std::string(msg, len));
}

platform::Window* g_loadWindow = nullptr;
GLADapiproc loadThunk(const char* name) { return (GLADapiproc)g_loadWindow->glProcAddress(name); }

// ---- device ---------------------------------------------------------------------------------
class GlDevice final : public Device {
public:
    explicit GlDevice(platform::Window* w) : win_(w), ctx_(w) {}

    bool init(std::string* error) {
        win_->glMakeCurrent();
        g_loadWindow = win_;
        if (!gladLoadGL(loadThunk)) { if (error) *error = "glad could not load OpenGL"; return false; }
        glEnable(GL_DEBUG_OUTPUT);
        glDebugMessageCallback(glDebug, nullptr);
        info_.backend = Backend::OpenGL;
        info_.vendor = (const char*)glGetString(GL_VENDOR);
        info_.renderer = (const char*)glGetString(GL_RENDERER);
        info_.apiVersion = (const char*)glGetString(GL_VERSION);
        info_.shadingLanguage = (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION);
        return true;
    }

    const DeviceInfo& info() const override { return info_; }
    std::unique_ptr<Buffer> createBuffer(const BufferDesc& d) override { return std::make_unique<GlBuffer>(d); }
    std::unique_ptr<Texture> createTexture(const TextureDesc& d) override { return std::make_unique<GlTexture>(d); }
    std::unique_ptr<RenderTarget> createRenderTarget(const RenderTargetDesc& d) override {
        return std::make_unique<GlRenderTarget>(d);
    }
    std::unique_ptr<Pipeline> createGraphicsPipeline(const GraphicsPipelineDesc& d, std::string* error) override {
        std::string err;
        GLuint v = compileStage(GL_VERTEX_SHADER, d.vertexSource, err);
        if (!v) { if (error) *error = "vertex: " + err; return nullptr; }
        GLuint f = compileStage(GL_FRAGMENT_SHADER, d.fragmentSource, err);
        if (!f) { glDeleteShader(v); if (error) *error = err; return nullptr; }
        GLuint p = linkProgram(v, f, err);
        if (!p) { if (error) *error = err; return nullptr; }
        auto pl = std::make_unique<GlPipeline>(p, false);
        pl->initGraphics(d);
        return pl;
    }
    std::unique_ptr<Pipeline> createComputePipeline(const ComputePipelineDesc& d, std::string* error) override {
        std::string err;
        GLuint c = compileStage(GL_COMPUTE_SHADER, d.source, err);
        if (!c) { if (error) *error = err; return nullptr; }
        GLuint p = linkProgram(c, 0, err);
        if (!p) { if (error) *error = err; return nullptr; }
        return std::make_unique<GlPipeline>(p, true);
    }
    std::unique_ptr<GpuTimer> createTimer() override { return std::make_unique<GlTimer>(); }
    CommandContext& context() override { return ctx_; }

    void present() override { win_->glSwapBuffers(); }
    void setVsync(bool on) override { win_->glSetSwapInterval(on ? 1 : 0); }

    void imguiInit() override { ImGui_ImplOpenGL3_Init("#version 450"); }
    void imguiNewFrame() override { ImGui_ImplOpenGL3_NewFrame(); }
    void imguiRender() override { ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData()); }
    void imguiShutdown() override { ImGui_ImplOpenGL3_Shutdown(); }

private:
    platform::Window* win_;
    GlContext ctx_;
    DeviceInfo info_;
};

} // namespace

std::unique_ptr<Device> createGlDevice(platform::Window* window, std::string* error) {
    auto d = std::make_unique<GlDevice>(window);
    if (!d->init(error)) return nullptr;
    return d;
}

} // namespace glue::rhi
