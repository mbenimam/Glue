#include "scene/Mesh.h"
#include <vector>

namespace prism {

void Mesh::destroy() {
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (ebo_) glDeleteBuffers(1, &ebo_);
    vao_ = vbo_ = ebo_ = 0;
}

void Mesh::upload(const MeshData& d) {
    destroy();
    std::vector<float> vb;
    vb.reserve(d.positions.size() * 8);
    for (size_t i = 0; i < d.positions.size(); ++i) {
        const Vec3& p = d.positions[i];
        Vec3 n = i < d.normals.size() ? d.normals[i] : Vec3{0, 1, 0};
        Vec2 uv = i < d.uvs.size() ? d.uvs[i] : Vec2{};
        vb.insert(vb.end(), {p.x, p.y, p.z, n.x, n.y, n.z, uv.x, uv.y});
    }
    indexCount_ = (GLsizei)d.indices.size();
    glCreateBuffers(1, &vbo_);
    glNamedBufferStorage(vbo_, vb.size() * sizeof(float), vb.data(), 0);
    glCreateBuffers(1, &ebo_);
    glNamedBufferStorage(ebo_, d.indices.size() * sizeof(uint32_t), d.indices.data(), 0);
    glCreateVertexArrays(1, &vao_);
    glVertexArrayVertexBuffer(vao_, 0, vbo_, 0, 8 * sizeof(float));
    glVertexArrayElementBuffer(vao_, ebo_);
    glEnableVertexArrayAttrib(vao_, 0); glVertexArrayAttribFormat(vao_, 0, 3, GL_FLOAT, GL_FALSE, 0);
    glEnableVertexArrayAttrib(vao_, 1); glVertexArrayAttribFormat(vao_, 1, 3, GL_FLOAT, GL_FALSE, 12);
    glEnableVertexArrayAttrib(vao_, 2); glVertexArrayAttribFormat(vao_, 2, 2, GL_FLOAT, GL_FALSE, 24);
    for (int i = 0; i < 3; ++i) glVertexArrayAttribBinding(vao_, i, 0);
}

void Mesh::draw() const {
    glBindVertexArray(vao_);
    glDrawElements(GL_TRIANGLES, indexCount_, GL_UNSIGNED_INT, nullptr);
}

} // namespace prism
