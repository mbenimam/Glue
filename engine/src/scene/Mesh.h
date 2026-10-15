#pragma once
#include "rhi/Rhi.h"
#include "scene/MeshData.h"

namespace glue {

// Vertex layout of Mesh: position (location 0), normal (1), uv (2), interleaved as 8 floats.
rhi::VertexLayout meshVertexLayout();

// GPU copy of a MeshData, made through the RHI so it works on any backend.
class Mesh {
public:
    void upload(rhi::Device& device, const MeshData& d);
    void release() { vbo_.reset(); ibo_.reset(); indexCount_ = 0; }
    // Binds the buffers and issues the draw. A pipeline created with meshVertexLayout() must be bound.
    void draw(rhi::CommandContext& ctx) const;
    size_t triangles() const { return indexCount_ / 3; }
    bool valid() const { return vbo_ != nullptr; }

private:
    std::unique_ptr<rhi::Buffer> vbo_, ibo_;
    uint32_t indexCount_ = 0;
};

} // namespace glue
