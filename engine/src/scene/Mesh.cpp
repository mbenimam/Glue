#include "scene/Mesh.h"

namespace glue {

rhi::VertexLayout meshVertexLayout() {
    rhi::VertexLayout l;
    l.stride = 8 * sizeof(float);
    l.attributes = {{0, rhi::VertexFormat::Float3, 0}, {1, rhi::VertexFormat::Float3, 12}, {2, rhi::VertexFormat::Float2, 24}};
    return l;
}

void Mesh::upload(rhi::Device& dev, const MeshData& d) {
    std::vector<float> vb;
    vb.reserve(d.positions.size() * 8);
    for (size_t i = 0; i < d.positions.size(); ++i) {
        const Vec3& p = d.positions[i];
        Vec3 n = i < d.normals.size() ? d.normals[i] : Vec3{0, 1, 0};
        Vec2 uv = i < d.uvs.size() ? d.uvs[i] : Vec2{};
        vb.insert(vb.end(), {p.x, p.y, p.z, n.x, n.y, n.z, uv.x, uv.y});
    }
    rhi::BufferDesc v;
    v.size = vb.size() * sizeof(float);
    v.usage = rhi::BufferUsage::Vertex;
    v.initialData = vb.data();
    v.debugName = "mesh vertices";
    vbo_ = dev.createBuffer(v);
    rhi::BufferDesc i;
    i.size = d.indices.size() * sizeof(uint32_t);
    i.usage = rhi::BufferUsage::Index;
    i.initialData = d.indices.data();
    i.debugName = "mesh indices";
    ibo_ = dev.createBuffer(i);
    indexCount_ = (uint32_t)d.indices.size();
}

void Mesh::draw(rhi::CommandContext& ctx) const {
    ctx.bindVertexBuffer(*vbo_);
    ctx.bindIndexBuffer(*ibo_, rhi::IndexType::U32);
    ctx.drawIndexed(indexCount_);
}

} // namespace glue
