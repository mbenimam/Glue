#include "gfx/Texture.h"
#include "core/Log.h"
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace glue {

void Texture2D::create(int w, int h, GLenum fmt, GLenum filter, GLenum wrap) {
    destroy();
    w_ = w; h_ = h;
    glCreateTextures(GL_TEXTURE_2D, 1, &id_);
    glTextureStorage2D(id_, 1, fmt, w, h);
    glTextureParameteri(id_, GL_TEXTURE_MIN_FILTER, filter);
    glTextureParameteri(id_, GL_TEXTURE_MAG_FILTER, filter == GL_LINEAR_MIPMAP_LINEAR ? GL_LINEAR : filter);
    glTextureParameteri(id_, GL_TEXTURE_WRAP_S, wrap);
    glTextureParameteri(id_, GL_TEXTURE_WRAP_T, wrap);
}

void Texture2D::upload(int w, int h, int ch, const unsigned char* px) {
    GLenum fmt = ch == 4 ? GL_RGBA : ch == 3 ? GL_RGB : ch == 2 ? GL_RG : GL_RED;
    create(w, h, GL_RGBA8, GL_LINEAR_MIPMAP_LINEAR, GL_REPEAT);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTextureSubImage2D(id_, 0, 0, 0, w, h, fmt, GL_UNSIGNED_BYTE, px);
    glGenerateTextureMipmap(id_);
}

bool Texture2D::loadFile(const std::string& path, bool) {
    int w, h, c;
    stbi_set_flip_vertically_on_load(1);
    unsigned char* px = stbi_load(path.c_str(), &w, &h, &c, 4);
    if (!px) { Log::warn("texture not loaded: " + path); return false; }
    upload(w, h, 4, px);
    stbi_image_free(px);
    return true;
}

void Framebuffer::destroy() {
    if (fbo_) glDeleteFramebuffers(1, &fbo_);
    if (color_) glDeleteTextures(1, &color_);
    if (depth_) glDeleteTextures(1, &depth_);
    fbo_ = color_ = depth_ = 0;
}

void Framebuffer::resize(int w, int h, GLenum fmt) {
    w = std::max(w, 1); h = std::max(h, 1);
    if (fbo_ && w == w_ && h == h_ && fmt == fmt_) return;
    destroy();
    w_ = w; h_ = h; fmt_ = fmt;
    glCreateTextures(GL_TEXTURE_2D, 1, &color_);
    glTextureStorage2D(color_, 1, fmt, w, h);
    glTextureParameteri(color_, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(color_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(color_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(color_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glCreateTextures(GL_TEXTURE_2D, 1, &depth_);
    glTextureStorage2D(depth_, 1, GL_DEPTH_COMPONENT32F, w, h);
    glCreateFramebuffers(1, &fbo_);
    glNamedFramebufferTexture(fbo_, GL_COLOR_ATTACHMENT0, color_, 0);
    glNamedFramebufferTexture(fbo_, GL_DEPTH_ATTACHMENT, depth_, 0);
}

std::vector<unsigned char> Framebuffer::readback() const {
    std::vector<unsigned char> px((size_t)w_ * h_ * 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w_, h_, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    return px;
}

} // namespace glue
