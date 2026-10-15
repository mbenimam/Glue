// The render hardware interface through the null backend: no window, no GPU, same code paths as the
// engine's own rendering helpers.
#include "TestFramework.h"
#include "gfx/ShaderToy.h"
#include "rhi/Rhi.h"
#include "rhi/null/NullDevice.h"
#include "scene/Mesh.h"
#include "scene/MeshData.h"
#include <algorithm>
#include <cstring>

using namespace glue;

static bool logHas(const rhi::NullDevice& d, const std::string& needle) {
    for (const auto& l : d.commandLog())
        if (l.find(needle) != std::string::npos) return true;
    return false;
}

TEST(rhi_formatsAndNames) {
    CHECK(rhi::bytesPerPixel(rhi::Format::RGBA8) == 4);
    CHECK(rhi::bytesPerPixel(rhi::Format::RGBA16F) == 8);
    CHECK(rhi::bytesPerPixel(rhi::Format::RGBA32F) == 16);
    CHECK(std::string(rhi::backendName(rhi::Backend::OpenGL)) == "OpenGL");
    CHECK(std::string(rhi::backendName(rhi::Backend::Vulkan)) == "Vulkan");
}

TEST(rhi_factoryRefusesBackendsThatDoNotExistYet) {
    std::string err;
    auto vk = rhi::Device::create(rhi::Backend::Vulkan, nullptr, &err);
    CHECK(vk == nullptr);
    CHECK(err.find("not implemented") != std::string::npos);
    err.clear();
    auto gl = rhi::Device::create(rhi::Backend::OpenGL, nullptr, &err);   // needs a window
    CHECK(gl == nullptr);
    CHECK(!err.empty());
    auto null = rhi::Device::create(rhi::Backend::Null, nullptr, &err);
    CHECK(null != nullptr);
    CHECK(null->backend() == rhi::Backend::Null);
}

TEST(rhi_objectsAreCountedAndReleased) {
    auto dev = rhi::createNullDevice();
    {
        rhi::BufferDesc b; b.size = 64;
        auto buf = dev->createBuffer(b);
        rhi::TextureDesc t; t.width = 4; t.height = 4;
        auto tex = dev->createTexture(t);
        rhi::RenderTargetDesc r; r.width = 8; r.height = 8;   // colour + depth = 2 textures
        auto rt = dev->createRenderTarget(r);
        CHECK(dev->liveObjects() == 4);
        CHECK(rt->depth() != nullptr);
        r.depth = false;
        auto noDepth = dev->createRenderTarget(r);
        CHECK(noDepth->depth() == nullptr);
    }
    CHECK(dev->liveObjects() == 0);
}

TEST(rhi_bufferReadWriteAndResize) {
    auto dev = rhi::createNullDevice();
    float init[4] = {1, 2, 3, 4};
    rhi::BufferDesc d; d.size = sizeof(init); d.initialData = init; d.usage = rhi::BufferUsage::Storage;
    auto b = dev->createBuffer(d);
    float out[4] = {};
    b->read(0, out, sizeof(out));
    CHECK(out[2] == 3.0f);
    float v = 9.0f;
    b->write(sizeof(float), &v, sizeof(v));
    b->read(0, out, sizeof(out));
    CHECK(out[1] == 9.0f && out[0] == 1.0f);
    b->resizeDiscard(128);
    CHECK(b->size() == 128);
}

TEST(rhi_pipelineErrorsComeBackAsText) {
    auto dev = rhi::createNullDevice();
    std::string err;
    rhi::GraphicsPipelineDesc g;   // both sources empty
    CHECK(dev->createGraphicsPipeline(g, &err) == nullptr);
    CHECK(!err.empty());
    g.vertexSource = kShadertoyVertex;
    g.fragmentSource = wrapShadertoy("void mainImage(out vec4 c, in vec2 p) { c = vec4(1.0); }");
    auto p = dev->createGraphicsPipeline(g, &err);
    CHECK(p != nullptr && !p->isCompute());
    rhi::ComputePipelineDesc c;
    c.source = "#version 450\nlayout(local_size_x = 8) in;\nvoid main() {}\n";
    auto cp = dev->createComputePipeline(c, &err);
    CHECK(cp != nullptr && cp->isCompute());
}

TEST(rhi_meshDrawGoesThroughTheContext) {
    auto dev = rhi::createNullDevice();
    MeshData m = makeUvSphere(8, 12);
    Mesh mesh;
    mesh.upload(*dev, m);
    CHECK(mesh.valid());
    CHECK(mesh.triangles() == m.triangleCount());

    rhi::GraphicsPipelineDesc g;
    g.vertexSource = "v"; g.fragmentSource = "f";
    g.vertexLayout = meshVertexLayout();
    CHECK(g.vertexLayout.stride == 32);
    CHECK(g.vertexLayout.attributes.size() == 3);
    auto pl = dev->createGraphicsPipeline(g, nullptr);
    auto& ctx = dev->context();
    ctx.bindPipeline(*pl);
    mesh.draw(ctx);
    CHECK(logHas(*dev, "bindVertexBuffer size=" + std::to_string(m.positions.size() * 32)));
    CHECK(logHas(*dev, "bindIndexBuffer size=" + std::to_string(m.indices.size() * 4) + " type=u32"));
    CHECK(logHas(*dev, "drawIndexed " + std::to_string(m.indices.size())));
}

// The command sequence of one GlueLab-style frame: scene pass into a target, effect pass that samples
// it, then the back buffer. The test pins the order, which is what a Vulkan backend has to honour too.
TEST(rhi_twoPassFrameHasTheExpectedOrder) {
    auto dev = rhi::createNullDevice();
    rhi::RenderTargetDesc rd; rd.width = 64; rd.height = 32; rd.colorFormat = rhi::Format::RGBA16F;
    auto scene = dev->createRenderTarget(rd);
    rd.depth = false; rd.colorFormat = rhi::Format::RGBA8;
    auto out = dev->createRenderTarget(rd);
    rhi::GraphicsPipelineDesc g; g.vertexSource = "v"; g.fragmentSource = "f";
    auto fx = dev->createGraphicsPipeline(g, nullptr);

    auto& ctx = dev->context();
    ctx.beginRenderPass(scene.get(), {});
    ctx.endRenderPass();
    ctx.beginRenderPass(out.get(), {});
    ctx.bindPipeline(*fx);
    ctx.bindTexture(0, scene->color());
    ctx.bindTexture(2, *scene->depth());
    ctx.draw(3);
    ctx.endRenderPass();
    dev->present();

    const auto& log = dev->commandLog();
    std::vector<std::string> kinds;
    for (const auto& l : log) kinds.push_back(l.substr(0, l.find(' ')));
    std::vector<std::string> want = {"beginRenderPass", "endRenderPass", "beginRenderPass", "bindPipeline",
                                     "bindTexture", "bindTexture", "draw", "endRenderPass", "present"};
    CHECK(kinds == want);
    auto px = ctx.readColorRgba8(out.get());
    CHECK(px.size() == 64u * 32u * 4u);
}

TEST(rhi_computeDispatchAndBarriers) {
    auto dev = rhi::createNullDevice();
    rhi::ComputePipelineDesc c; c.source = "x";
    auto cp = dev->createComputePipeline(c, nullptr);
    rhi::BufferDesc b; b.size = 16; b.usage = rhi::BufferUsage::Storage;
    auto buf = dev->createBuffer(b);
    auto& ctx = dev->context();
    ctx.bindPipeline(*cp);
    ctx.bindStorageBuffer(3, *buf);
    ctx.dispatch(160, 90, 1);
    ctx.barrier(rhi::Barrier::StorageBuffer | rhi::Barrier::BufferUpdate);
    CHECK(logHas(*dev, "bindStorageBuffer 3"));
    CHECK(logHas(*dev, "dispatch 160 90 1"));
    CHECK(logHas(*dev, "barrier 9"));
}
