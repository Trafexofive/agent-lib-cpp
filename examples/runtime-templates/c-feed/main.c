#include <stdio.h>
#include <time.h>

int main(void) {
    printf("{\"status\":\"ok\",\"source\":\"c_status_feed\",\"timestamp\":%ld}\n", (long)time(NULL));
    return 0;
}
