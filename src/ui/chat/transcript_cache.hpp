#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace cortex::mk3::ui::chat {

// Above this source-line count, drawTranscript keeps a full span map but only
// materializes display lines for the visible viewport (± overscan). Small
// transcripts still fully materialize (tests + simple path).
inline constexpr size_t kViewportVirtualizeSourceThreshold = 120;

struct TranscriptWrapCache {
    uint64_t sourceVersion = std::numeric_limits<uint64_t>::max();
    int width = -1;
    // Fully-materialized display lines (small transcripts / tests).
    // For virtualized large transcripts this may be empty or hold only the
    // last painted viewport window (not the full document).
    std::vector<std::string> lines;
    std::vector<uint8_t> blockKinds;
    std::vector<bool> blockHeaders;
    std::vector<bool> blockSelected;

    // Incremental-wrap state. sourceSnapshot[i] is the source line captured at the
    // last wrap; sourceLineSpans[i] is how many display lines source[i] produced;
    // inCodeAfter[i] is the code-fence state AFTER processing source[i].
    std::vector<std::string> sourceSnapshot;
    std::vector<int> sourceLineSpans;
    std::vector<bool> inCodeAfter;

    // Sum of sourceLineSpans — total virtual display height for scroll math.
    int totalDisplayLines = 0;

    // Last painted viewport window (virtualized path only).
    int viewportOffset = -1;
    int viewportH = -1;
    std::vector<std::string> viewportLines;
    std::vector<uint8_t> viewportKinds;
    std::vector<bool> viewportHeaders;
    std::vector<bool> viewportSelected;

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
        totalDisplayLines = 0;
        viewportOffset = -1;
        viewportH = -1;
        viewportLines.clear();
        viewportKinds.clear();
        viewportHeaders.clear();
        viewportSelected.clear();
    }
};

}  // namespace cortex::mk3::ui::chat
