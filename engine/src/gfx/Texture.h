#pragma once
#include <glad/gl.h>
#include <string>
#include <vector>

namespace prism {

class Texture2D {
public:
    Texture2D() = default;
    ~Texture2D() { destroy(); }
    Texture2D(const Texture2D&) = delete;
    Texture2D& operator=(const Texture2D&) = delete;

    void create(int w, int h, GLenum internalFormat, GLenum filter = GL_LINEAR, GLenum wrap = GL_REPEAT);
    // Decodes an image file (png/jpg/...) with stb_image; returns false if unreadable.
    bool loadFile(const std::string& path, bool srgb = false);
    void upload(int w, int h, int channels, const unsigned char* px);
    void bind(int unit) const { glBindTextureUnit(unit, id_); }
    GLuint id() const { return id_; }
    int width() const { return w_; }
    int height() const { return h_; }
    void destroy() { if (id_) glDeleteTextures(1, &id_); id_ = 0; }

private:
    GLuint id_ = 0;
    int w_ = 0, h_ = 0;
};

// Color + depth render target; recreated when the viewport size changes.
class Framebuffer {
public:
    ~Framebuffer() { destroy(); }
    void resize(int w, int h, GLenum colorFormat = GL_RGBA16F);
    void destroy();
    void bind() const { glBindFramebuffer(GL_FRAMEBUFFER, fbo_); glViewport(0, 0, w_, h_); }
    static void unbind() { glBindFramebuffer(GL_FRAMEBUFFER, 0); }
    GLuint colorTexture() const { return color_; }
    GLuint depthTexture() const { return depth_; }
    int width() const { return w_; }
    int height() const { return h_; }
    // Reads RGBA8 pixels of the color attachment, bottom row first as OpenGL stores them.
    std::vector<unsigned char> readback() const;

private:
    GLuint fbo_ = 0, color_ = 0, depth_ = 0;
    int w_ = 0, h_ = 0;
    GLenum fmt_ = 0;
};

} // namespace prism
