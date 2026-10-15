#pragma once
#include <glad/gl.h>
#include "scene/MeshData.h"

namespace prism {

// GPU copy of a MeshData: interleaved position, normal, uv in one buffer.
class Mesh {
public:
    Mesh() = default;
    ~Mesh() { destroy(); }
    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;

    void upload(const MeshData& d);
    void draw() const;
    void destroy();
    size_t triangles() const { return indexCount_ / 3; }

private:
    GLuint vao_ = 0, vbo_ = 0, ebo_ = 0;
    GLsizei indexCount_ = 0;
};

} // namespace prism
