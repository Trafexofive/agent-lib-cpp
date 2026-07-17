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

    // Incremental-wrap state. sourceSnapshot[i] is the source line captured at the
    // last wrap; sourceLineSpans[i] is how many display lines source[i] produced;
    // inCodeAfter[i] is the code-fence state AFTER processing source[i]. Together
    // these let us re-wrap only the dirty tail on a transcriptVersion bump instead
    // of the whole transcript (kills O(n^2) during streaming).
    std::vector<std::string> sourceSnapshot;
    std::vector<int> sourceLineSpans;
    std::vector<bool> inCodeAfter;

    void invalidate() {
        sourceVersion = std::numeric_limits<uint64_t>::max();
        width = -1;
        lines.clear();
        blockKinds.clear();
        blockHeaders.clear();
        blockSelected.clear();
        sourceSnapshot.clear();
        sourceLineSpans.clear();
        inCodeAfter.clear();
    }
};;

}  // namespace cortex::mk3::ui::chat
