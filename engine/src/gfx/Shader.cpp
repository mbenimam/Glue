#include "gfx/Shader.h"
#include <vector>

namespace glue {

GLuint Shader::compileStage(GLenum type, const std::string& src, std::string& err) {
    GLuint s = glCreateShader(type);
    const char* p = src.c_str();
    glShaderSource(s, 1, &p, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint n = 0;
        glGetShaderiv(s, GL_INFO_LOG_LENGTH, &n);
        std::vector<char> log(n > 1 ? n : 1);
        glGetShaderInfoLog(s, n, nullptr, log.data());
        err = log.data();
        glDeleteShader(s);
        return 0;
    }
    return s;
}

bool Shader::link(GLuint a, GLuint b) {
    GLuint p = glCreateProgram();
    glAttachShader(p, a);
    if (b) glAttachShader(p, b);
    glLinkProgram(p);
    glDeleteShader(a);
    if (b) glDeleteShader(b);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint n = 0;
        glGetProgramiv(p, GL_INFO_LOG_LENGTH, &n);
        std::vector<char> log(n > 1 ? n : 1);
        glGetProgramInfoLog(p, n, nullptr, log.data());
        error_ = log.data();
        glDeleteProgram(p);
        return false;
    }
    destroy();
    id_ = p;
    locs_.clear();
    error_.clear();
    return true;
}

bool Shader::compileGraphics(const std::string& vs, const std::string& fs) {
    std::string err;
    GLuint v = compileStage(GL_VERTEX_SHADER, vs, err);
    if (!v) { error_ = "vertex: " + err; return false; }
    GLuint f = compileStage(GL_FRAGMENT_SHADER, fs, err);
    if (!f) { glDeleteShader(v); error_ = err; return false; }
    return link(v, f);
}

bool Shader::compileCompute(const std::string& cs) {
    std::string err;
    GLuint c = compileStage(GL_COMPUTE_SHADER, cs, err);
    if (!c) { error_ = err; return false; }
    return link(c, 0);
}

GLint Shader::loc(const char* name) const {
    auto it = locs_.find(name);
    if (it != locs_.end()) return it->second;
    GLint l = glGetUniformLocation(id_, name);
    locs_.emplace(name, l);
    return l;
}

} // namespace glue
