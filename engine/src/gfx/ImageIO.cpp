#include "gfx/ImageIO.h"
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace glue::gfx {

bool loadImageRgba8(const std::string& path, int& w, int& h, std::vector<uint8_t>& out) {
    int c;
    stbi_set_flip_vertically_on_load(1);
    unsigned char* px = stbi_load(path.c_str(), &w, &h, &c, 4);
    if (!px) return false;
    out.assign(px, px + (size_t)w * h * 4);
    stbi_image_free(px);
    return true;
}

bool savePngRgba8(const std::string& path, int w, int h, std::vector<uint8_t> px) {
    for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
    stbi_flip_vertically_on_write(1);
    return stbi_write_png(path.c_str(), w, h, 4, px.data(), w * 4) != 0;
}

} // namespace glue::gfx
