// =============================================================================
// Docker relic dispatcher test runner
// =============================================================================
#include "../testing/docker_relic_test.hpp"

int main() {
    std::cout << "docker-relic-test\n";
    cortex::mk3::tests::DockerRelicTest test;
    return test.run() ? 0 : 1;
}
