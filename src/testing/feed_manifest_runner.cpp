// =============================================================================
// Feed manifest test runner
// =============================================================================
#include "../feeds/feed_engine.hpp"
#include "../testing/feed_manifest_test.hpp"

int main() {
    std::cout << "feed-manifest-test\n";
    cortex::mk3::feeds::registerFeeds();
    cortex::mk3::tests::FeedManifestTest test;
    return test.run() ? 0 : 1;
}
