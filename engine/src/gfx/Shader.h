#pragma once
#include <glad/gl.h>
#include <string>
#include <unordered_map>
#include "math/Math.h"

namespace glue {

// A linked GLSL program. Compile errors are kept as text so the editor can show them
// instead of crashing; a failed compile leaves the previous program untouched.
class Shader {
public:
    Shader() = default;
    ~Shader() { destroy(); }
    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;

    // Each returns true on success. On failure `error()` has the driver's log.
    bool compileGraphics(const std::string& vs, const std::string& fs);
    bool compileCompute(const std::string& cs);

    void bind() const { glUseProgram(id_); }
    GLuint id() const { return id_; }
    void destroy() { if (id_) glDeleteProgram(id_); id_ = 0; }
    bool valid() const { return id_ != 0; }
    const std::string& error() const { return error_; }

    GLint loc(const char* name) const;
    void set(const char* n, float v) const { glProgramUniform1f(id_, loc(n), v); }
    void set(const char* n, int v) const { glProgramUniform1i(id_, loc(n), v); }
    void set(const char* n, const Vec3& v) const { glProgramUniform3f(id_, loc(n), v.x, v.y, v.z); }
    void set(const char* n, float x, float y) const { glProgramUniform2f(id_, loc(n), x, y); }
    void set(const char* n, float x, float y, float z) const { glProgramUniform3f(id_, loc(n), x, y, z); }
    void set(const char* n, float x, float y, float z, float w) const { glProgramUniform4f(id_, loc(n), x, y, z, w); }
    void set(const char* n, const Mat4& m) const { glProgramUniformMatrix4fv(id_, loc(n), 1, GL_FALSE, m.data()); }

private:
    bool link(GLuint a, GLuint b);
    static GLuint compileStage(GLenum type, const std::string& src, std::string& err);

    GLuint id_ = 0;
    std::string error_;
    mutable std::unordered_map<std::string, GLint> locs_;
};

} // namespace glue
