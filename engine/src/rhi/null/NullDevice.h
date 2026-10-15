#pragma once
#include "rhi/Rhi.h"

namespace glue::rhi {

// A device that draws nothing and records every call as text. It exists for two reasons: unit tests
// can run the engine's rendering code with no GPU, and it proves that the interface can be
// implemented by something that is not OpenGL.
class NullDevice : public Device {
public:
    // Every context call appends one line here, e.g. "beginRenderPass target=1 clearColor=1".
    virtual const std::vector<std::string>& commandLog() const = 0;
    virtual void clearCommandLog() = 0;
    // Number of live objects, for leak checks in tests.
    virtual int liveObjects() const = 0;
};

std::unique_ptr<NullDevice> createNullDevice();

} // namespace glue::rhi
