#pragma once
// Render hardware interface (RHI): the only graphics API the rest of the engine and the apps see.
//
// Design rules, so that a Vulkan or Direct3D 12 backend fits without touching any caller:
//   * Objects are created from plain descriptions and are immutable in their state. Everything that
//     Vulkan bakes into a pipeline (shaders, vertex layout, depth state, topology) is part of the
//     pipeline description here too, never a global switch.
//   * Drawing is recorded through a CommandContext: render passes, bind, draw, dispatch, barrier.
//     The OpenGL backend executes each call at once; an explicit API can record them into a command buffer.
//   * Resources are bound by slot number, never by an API object name.
//   * No header under rhi/ (except rhi/gl/) includes an OpenGL or window-system header.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "math/Math.h"

namespace glue::platform { class Window; }

namespace glue::rhi {

enum class Backend { OpenGL, Vulkan, D3D12, Null };
const char* backendName(Backend b);

// ---- resource descriptions ------------------------------------------------------------------
enum class Format : uint8_t { R8, RGBA8, R32F, RGBA16F, RGBA32F, Depth32F };
size_t bytesPerPixel(Format f);

enum class Filter : uint8_t { Nearest, Linear };
enum class Wrap : uint8_t { Repeat, ClampToEdge };

enum class BufferUsage : uint32_t {
    Vertex = 1, Index = 2, Storage = 4, Uniform = 8,
    CpuWrite = 16,   // contents are updated from the CPU after creation
    CpuRead = 32,    // contents are read back to the CPU
};
inline BufferUsage operator|(BufferUsage a, BufferUsage b) { return BufferUsage(uint32_t(a) | uint32_t(b)); }
inline bool has(BufferUsage v, BufferUsage f) { return (uint32_t(v) & uint32_t(f)) != 0; }

struct BufferDesc {
    size_t size = 0;
    BufferUsage usage = BufferUsage::Vertex;
    const void* initialData = nullptr;   // may be null (zero-filled) or hold `size` bytes
    const char* debugName = "";
};

class Buffer {
public:
    virtual ~Buffer() = default;
    virtual size_t size() const = 0;
    virtual void write(size_t offset, const void* data, size_t bytes) = 0;
    virtual void read(size_t offset, void* out, size_t bytes) = 0;      // blocking
    virtual void resizeDiscard(size_t bytes) = 0;                       // new size, old contents lost
};

struct TextureDesc {
    int width = 1, height = 1;
    Format format = Format::RGBA8;
    Filter minFilter = Filter::Linear, magFilter = Filter::Linear;
    Wrap wrap = Wrap::Repeat;
    bool mipmaps = false;
    bool storage = false;                // can be bound as a read/write image in a compute shader
    const void* initialData = nullptr;   // tightly packed rows, bottom row first (OpenGL convention)
    const char* debugName = "";
};

class Texture {
public:
    virtual ~Texture() = default;
    virtual int width() const = 0;
    virtual int height() const = 0;
    virtual Format format() const = 0;
    virtual void write(const void* pixels) = 0;   // whole image, in the texture's own format
    virtual void generateMipmaps() = 0;
    // Value usable as ImGui's ImTextureID for the active backend.
    virtual uint64_t imguiId() const = 0;
};

struct RenderTargetDesc {
    int width = 1, height = 1;
    Format colorFormat = Format::RGBA8;
    bool depth = true;
    const char* debugName = "";
};

// A colour texture plus an optional depth texture that can be drawn into and then sampled.
class RenderTarget {
public:
    virtual ~RenderTarget() = default;
    virtual int width() const = 0;
    virtual int height() const = 0;
    virtual Texture& color() = 0;
    virtual Texture* depth() = 0;          // null when the target has no depth attachment
};

// ---- pipelines ------------------------------------------------------------------------------
enum class VertexFormat : uint8_t { Float, Float2, Float3, Float4 };
enum class Topology : uint8_t { Triangles, TriangleStrip, Lines, Points };
enum class IndexType : uint8_t { U16, U32 };

struct VertexAttribute { uint32_t location = 0; VertexFormat format = VertexFormat::Float3; uint32_t offset = 0; };
struct VertexLayout {
    uint32_t stride = 0;                   // bytes per vertex of vertex-buffer slot 0
    std::vector<VertexAttribute> attributes;
};

struct DepthState { bool test = true; bool write = true; };

struct GraphicsPipelineDesc {
    std::string vertexSource;              // GLSL 4.50 (the engine's shading language)
    std::string fragmentSource;
    VertexLayout vertexLayout;             // empty: the vertex shader reads gl_VertexID only
    Topology topology = Topology::Triangles;
    DepthState depth;
    bool polygonOffset = false;            // pushes filled geometry back so wireframe on top does not z-fight
    float polygonOffsetFactor = 1.0f, polygonOffsetUnits = 1.0f;
    const char* debugName = "";
};

struct ComputePipelineDesc {
    std::string source;
    const char* debugName = "";
};

// A compiled shader program plus its state. Uniforms are set by name; a backend without loose
// uniforms (Vulkan, D3D12) backs them with a push-constant or constant buffer found by reflection.
class Pipeline {
public:
    virtual ~Pipeline() = default;
    virtual bool isCompute() const = 0;
    virtual void set(const char* name, float v) = 0;
    virtual void set(const char* name, int v) = 0;
    virtual void set(const char* name, uint32_t v) = 0;
    virtual void set(const char* name, float x, float y) = 0;
    virtual void set(const char* name, float x, float y, float z) = 0;
    virtual void set(const char* name, float x, float y, float z, float w) = 0;
    virtual void set(const char* name, const Mat4& m) = 0;
    void set(const char* name, const Vec3& v) { set(name, v.x, v.y, v.z); }
};

// Measures GPU time between begin and end on a context.
class GpuTimer {
public:
    virtual ~GpuTimer() = default;
    virtual uint64_t resultNanoseconds() = 0;   // blocks until the GPU has finished the measured work
};

// ---- recording ------------------------------------------------------------------------------
enum class Access : uint8_t { Read, Write, ReadWrite };

enum class Barrier : uint32_t {
    StorageBuffer = 1,   // writes to storage buffers become visible to later reads
    StorageImage = 2,    // writes to storage images become visible to later image loads
    TextureFetch = 4,    // image writes become visible to later texture() sampling
    BufferUpdate = 8,    // shader writes become visible to CPU-side buffer reads
};
inline Barrier operator|(Barrier a, Barrier b) { return Barrier(uint32_t(a) | uint32_t(b)); }

struct RenderPassDesc {
    bool clearColor = true;
    float clearColorValue[4] = {0, 0, 0, 1};
    bool clearDepth = true;
    float clearDepthValue = 1.0f;
};

class CommandContext {
public:
    virtual ~CommandContext() = default;

    // target == nullptr draws into the window's back buffer.
    virtual void beginRenderPass(RenderTarget* target, const RenderPassDesc& pass) = 0;
    virtual void endRenderPass() = 0;
    virtual void setViewport(int x, int y, int width, int height) = 0;

    virtual void bindPipeline(Pipeline& p) = 0;
    virtual void bindVertexBuffer(Buffer& b, size_t offset = 0) = 0;       // slot 0
    virtual void bindIndexBuffer(Buffer& b, IndexType type) = 0;
    virtual void bindTexture(uint32_t slot, Texture& t) = 0;               // sampled; `layout(binding = slot)`
    virtual void bindStorageBuffer(uint32_t slot, Buffer& b) = 0;          // `layout(binding = slot) buffer`
    virtual void bindStorageImage(uint32_t slot, Texture& t, Access a) = 0;

    virtual void draw(uint32_t vertexCount, uint32_t instanceCount = 1, uint32_t firstVertex = 0) = 0;
    virtual void drawIndexed(uint32_t indexCount, uint32_t instanceCount = 1, uint32_t firstIndex = 0) = 0;
    virtual void dispatch(uint32_t groupsX, uint32_t groupsY = 1, uint32_t groupsZ = 1) = 0;
    virtual void barrier(Barrier b) = 0;

    virtual void beginTimer(GpuTimer& t) = 0;
    virtual void endTimer(GpuTimer& t) = 0;
    virtual void waitIdle() = 0;

    // Reads the colour image as tightly packed RGBA8, bottom row first. null target = back buffer.
    virtual std::vector<uint8_t> readColorRgba8(RenderTarget* target) = 0;
};

// ---- device ---------------------------------------------------------------------------------
struct DeviceInfo {
    Backend backend = Backend::Null;
    std::string vendor, renderer, apiVersion, shadingLanguage;
};

class Device {
public:
    virtual ~Device() = default;

    // Opens `backend` on `window`. The window must have been created for the same graphics API.
    // Returns null and fills *error when the backend is not built in or cannot start.
    static std::unique_ptr<Device> create(Backend backend, platform::Window* window, std::string* error);

    virtual const DeviceInfo& info() const = 0;
    Backend backend() const { return info().backend; }

    virtual std::unique_ptr<Buffer> createBuffer(const BufferDesc& d) = 0;
    virtual std::unique_ptr<Texture> createTexture(const TextureDesc& d) = 0;
    virtual std::unique_ptr<RenderTarget> createRenderTarget(const RenderTargetDesc& d) = 0;
    // Both return null on a compile or link error and put the driver's log in *error.
    virtual std::unique_ptr<Pipeline> createGraphicsPipeline(const GraphicsPipelineDesc& d, std::string* error) = 0;
    virtual std::unique_ptr<Pipeline> createComputePipeline(const ComputePipelineDesc& d, std::string* error) = 0;
    virtual std::unique_ptr<GpuTimer> createTimer() = 0;

    virtual CommandContext& context() = 0;

    // Frame boundaries: present() shows the back buffer.
    virtual void present() = 0;
    virtual void setVsync(bool on) = 0;

    // Dear ImGui renderer for this backend (the platform half lives on the window).
    virtual void imguiInit() = 0;
    virtual void imguiNewFrame() = 0;
    virtual void imguiRender() = 0;
    virtual void imguiShutdown() = 0;
};

} // namespace glue::rhi
