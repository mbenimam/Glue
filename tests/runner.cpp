#include "TestFramework.h"
#include <cstring>

int main(int argc, char** argv) {
    // Optional filter: tests --only substring
    const char* only = (argc > 2 && !std::strcmp(argv[1], "--only")) ? argv[2] : nullptr;
    for (auto& t : testfw::registry()) {
        if (only && !std::strstr(t.name, only)) continue;
        int before = testfw::failures();
        t.fn();
        std::printf("%-28s %s\n", t.name, testfw::failures() == before ? "ok" : "FAILED");
    }
    std::printf("%d checks, %d failed\n", testfw::checks(), testfw::failures());
    return testfw::failures() ? 1 : 0;
}
