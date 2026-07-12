#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace cortex::mk3::ui::chat {

struct TranscriptWrapCache {
    uint64_t sourceVersion = std::numeric_limits<uint64_t>::max();
    int width = -1;
    std::vector<std::string> lines;
    std::vector<uint8_t> blockKinds;
    std::vector<bool> blockHeaders;
    std::vector<bool> blockSelected;

    void invalidate() {
        sourceVersion = std::numeric_limits<uint64_t>::max();
        width = -1;
        lines.clear();
        blockKinds.clear();
        blockHeaders.clear();
        blockSelected.clear();
    }
};

}  // namespace cortex::mk3::ui::chat
