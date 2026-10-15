#include "rhi/Rhi.h"

namespace glue::rhi {

const char* backendName(Backend b) {
    switch (b) {
        case Backend::OpenGL: return "OpenGL";
        case Backend::Vulkan: return "Vulkan";
        case Backend::D3D12: return "Direct3D 12";
        case Backend::Null: return "Null";
    }
    return "?";
}

size_t bytesPerPixel(Format f) {
    switch (f) {
        case Format::R8: return 1;
        case Format::RGBA8: return 4;
        case Format::R32F: return 4;
        case Format::RGBA16F: return 8;
        case Format::RGBA32F: return 16;
        case Format::Depth32F: return 4;
    }
    return 0;
}

} // namespace glue::rhi
