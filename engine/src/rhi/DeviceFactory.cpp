#include "rhi/Rhi.h"
#include "rhi/gl/GlDevice.h"
#include "rhi/null/NullDevice.h"

namespace glue::rhi {

// The one place that knows which backends exist. Adding Vulkan means a new branch here and a
// directory next to rhi/gl; callers keep asking for a Backend value.
std::unique_ptr<Device> Device::create(Backend backend, platform::Window* window, std::string* error) {
    switch (backend) {
        case Backend::OpenGL:
            if (!window) { if (error) *error = "OpenGL needs a window"; return nullptr; }
            return createGlDevice(window, error);
        case Backend::Null:
            return createNullDevice();
        case Backend::Vulkan:
        case Backend::D3D12:
            if (error) *error = std::string(backendName(backend)) + " backend is not implemented yet";
            return nullptr;
    }
    return nullptr;
}

} // namespace glue::rhi
