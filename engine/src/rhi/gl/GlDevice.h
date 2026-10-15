#pragma once
#include "rhi/Rhi.h"

namespace glue::rhi {
// OpenGL 4.5 core backend (direct state access). Created through Device::create(Backend::OpenGL, ...).
std::unique_ptr<Device> createGlDevice(platform::Window* window, std::string* error);
}
