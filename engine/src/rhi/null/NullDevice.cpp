#include "rhi/null/NullDevice.h"
#include <cstring>
#include <sstream>

namespace glue::rhi {
namespace {

struct Counter {
    int live = 0;
};

class NBuffer final : public Buffer {
public:
    NBuffer(const BufferDesc& d, Counter& c) : data_(d.size), c_(c) {
        if (d.initialData) std::memcpy(data_.data(), d.initialData, d.size);
        ++c_.live;
    }
    ~NBuffer() override { --c_.live; }
    size_t size() const override { return data_.size(); }
    void write(size_t o, const void* p, size_t n) override { std::memcpy(data_.data() + o, p, n); }
    void read(size_t o, void* out, size_t n) override { std::memcpy(out, data_.data() + o, n); }
    void resizeDiscard(size_t n) override { data_.assign(n, 0); }
private:
    std::vector<uint8_t> data_;
    Counter& c_;
};

class NTexture final : public Texture {
public:
    NTexture(const TextureDesc& d, Counter& c)
        : w_(d.width), h_(d.height), fmt_(d.format), px_((size_t)d.width * d.height * bytesPerPixel(d.format)), c_(c) {
        if (d.initialData) std::memcpy(px_.data(), d.initialData, px_.size());
        ++c_.live;
    }
    ~NTexture() override { --c_.live; }
    int width() const override { return w_; }
    int height() const override { return h_; }
    Format format() const override { return fmt_; }
    void write(const void* p) override { std::memcpy(px_.data(), p, px_.size()); }
    void generateMipmaps() override {}
    uint64_t imguiId() const override { return reinterpret_cast<uint64_t>(this); }
private:
    int w_, h_;
    Format fmt_;
    std::vector<uint8_t> px_;
    Counter& c_;
};

class NRenderTarget final : public RenderTarget {
public:
    NRenderTarget(const RenderTargetDesc& d, Counter& c) : w_(d.width), h_(d.height) {
        TextureDesc t; t.width = w_; t.height = h_; t.format = d.colorFormat;
        color_ = std::make_unique<NTexture>(t, c);
        if (d.depth) { t.format = Format::Depth32F; depth_ = std::make_unique<NTexture>(t, c); }
    }
    int width() const override { return w_; }
    int height() const override { return h_; }
    Texture& color() override { return *color_; }
    Texture* depth() override { return depth_.get(); }
private:
    int w_, h_;
    std::unique_ptr<NTexture> color_, depth_;
};

class NPipeline final : public Pipeline {
public:
    NPipeline(bool compute, Counter& c) : compute_(compute), c_(c) { ++c_.live; }
    ~NPipeline() override { --c_.live; }
    bool isCompute() const override { return compute_; }
    void set(const char*, float) override {}
    void set(const char*, int) override {}
    void set(const char*, uint32_t) override {}
    void set(const char*, float, float) override {}
    void set(const char*, float, float, float) override {}
    void set(const char*, float, float, float, float) override {}
    void set(const char*, const Mat4&) override {}
private:
    bool compute_;
    Counter& c_;
};

class NTimer final : public GpuTimer {
public:
    uint64_t resultNanoseconds() override { return 0; }
};

class NContext final : public CommandContext {
public:
    std::vector<std::string> log;

    void beginRenderPass(RenderTarget* t, const RenderPassDesc& p) override {
        std::ostringstream s;
        s << "beginRenderPass target=" << (t ? "offscreen" : "backbuffer") << " clearColor=" << p.clearColor
          << " clearDepth=" << p.clearDepth;
        log.push_back(s.str());
    }
    void endRenderPass() override { log.push_back("endRenderPass"); }
    void setViewport(int x, int y, int w, int h) override {
        std::ostringstream s; s << "setViewport " << x << " " << y << " " << w << " " << h; log.push_back(s.str());
    }
    void bindPipeline(Pipeline& p) override { log.push_back(p.isCompute() ? "bindPipeline compute" : "bindPipeline graphics"); }
    void bindVertexBuffer(Buffer& b, size_t o) override {
        std::ostringstream s; s << "bindVertexBuffer size=" << b.size() << " offset=" << o; log.push_back(s.str());
    }
    void bindIndexBuffer(Buffer& b, IndexType t) override {
        std::ostringstream s; s << "bindIndexBuffer size=" << b.size() << " type=" << (t == IndexType::U16 ? "u16" : "u32");
        log.push_back(s.str());
    }
    void bindTexture(uint32_t slot, Texture&) override { log.push_back("bindTexture " + std::to_string(slot)); }
    void bindStorageBuffer(uint32_t slot, Buffer&) override { log.push_back("bindStorageBuffer " + std::to_string(slot)); }
    void bindStorageImage(uint32_t slot, Texture&, Access) override { log.push_back("bindStorageImage " + std::to_string(slot)); }
    void draw(uint32_t n, uint32_t i, uint32_t f) override {
        std::ostringstream s; s << "draw " << n << " instances=" << i << " first=" << f; log.push_back(s.str());
    }
    void drawIndexed(uint32_t n, uint32_t i, uint32_t f) override {
        std::ostringstream s; s << "drawIndexed " << n << " instances=" << i << " first=" << f; log.push_back(s.str());
    }
    void dispatch(uint32_t x, uint32_t y, uint32_t z) override {
        std::ostringstream s; s << "dispatch " << x << " " << y << " " << z; log.push_back(s.str());
    }
    void barrier(Barrier b) override { log.push_back("barrier " + std::to_string(uint32_t(b))); }
    void beginTimer(GpuTimer&) override { log.push_back("beginTimer"); }
    void endTimer(GpuTimer&) override { log.push_back("endTimer"); }
    void waitIdle() override { log.push_back("waitIdle"); }
    std::vector<uint8_t> readColorRgba8(RenderTarget* t) override {
        log.push_back("readColorRgba8");
        size_t w = t ? t->width() : 1, h = t ? t->height() : 1;
        return std::vector<uint8_t>(w * h * 4, 0);
    }
};

class NDevice final : public NullDevice {
public:
    NDevice() { info_.backend = Backend::Null; info_.vendor = "glue"; info_.renderer = "null device";
                info_.apiVersion = "0"; info_.shadingLanguage = "none"; }
    const DeviceInfo& info() const override { return info_; }
    std::unique_ptr<Buffer> createBuffer(const BufferDesc& d) override { return std::make_unique<NBuffer>(d, c_); }
    std::unique_ptr<Texture> createTexture(const TextureDesc& d) override { return std::make_unique<NTexture>(d, c_); }
    std::unique_ptr<RenderTarget> createRenderTarget(const RenderTargetDesc& d) override {
        return std::make_unique<NRenderTarget>(d, c_);
    }
    std::unique_ptr<Pipeline> createGraphicsPipeline(const GraphicsPipelineDesc& d, std::string* e) override {
        if (d.vertexSource.empty() || d.fragmentSource.empty()) { if (e) *e = "empty shader source"; return nullptr; }
        return std::make_unique<NPipeline>(false, c_);
    }
    std::unique_ptr<Pipeline> createComputePipeline(const ComputePipelineDesc& d, std::string* e) override {
        if (d.source.empty()) { if (e) *e = "empty shader source"; return nullptr; }
        return std::make_unique<NPipeline>(true, c_);
    }
    std::unique_ptr<GpuTimer> createTimer() override { return std::make_unique<NTimer>(); }
    CommandContext& context() override { return ctx_; }
    void present() override { ctx_.log.push_back("present"); }
    void setVsync(bool) override {}
    void imguiInit() override {}
    void imguiNewFrame() override {}
    void imguiRender() override {}
    void imguiShutdown() override {}
    const std::vector<std::string>& commandLog() const override { return ctx_.log; }
    void clearCommandLog() override { ctx_.log.clear(); }
    int liveObjects() const override { return c_.live; }
private:
    DeviceInfo info_;
    Counter c_;
    NContext ctx_;
};

} // namespace

std::unique_ptr<NullDevice> createNullDevice() { return std::make_unique<NDevice>(); }

} // namespace glue::rhi
