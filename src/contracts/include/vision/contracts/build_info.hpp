#pragma once
#include <string_view>

namespace vision::contracts {
// Build identity only. Production readiness belongs to a future application
// state machine; this descriptor must never be used as a PLC-ready signal.
struct BuildInfo {
    std::string_view version;
    std::string_view stage;
    bool inspection_available;
};
}
