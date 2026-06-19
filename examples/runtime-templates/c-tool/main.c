#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char* slurp(const char* path, long* len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = (char*)calloc((size_t)n + 1, 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (len) *len = n;
    return buf;
}

static void json_escape(const char* s) {
    for (; *s; ++s) {
        if (*s == '"' || *s == '\\') putchar('\\');
        if (*s == '\n') { fputs("\\n", stdout); continue; }
        putchar(*s);
    }
}

// Tiny demo extractor for {"message":"..."}. Swap for cJSON/jansson in real tools.
static void extract_message(const char* json, char* out, size_t cap) {
    const char* key = strstr(json, "\"message\"");
    if (!key) { snprintf(out, cap, "hello"); return; }
    const char* colon = strchr(key, ':');
    const char* start = colon ? strchr(colon, '"') : NULL;
    if (!start) { snprintf(out, cap, "hello"); return; }
    start++;
    const char* end = strchr(start, '"');
    size_t n = end ? (size_t)(end - start) : 0;
    if (n >= cap) n = cap - 1;
    memcpy(out, start, n);
    out[n] = 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        puts("{\"success\":false,\"error\":\"usage: c_echo_tool <input.json>\"}");
        return 2;
    }
    long len = 0;
    char* raw = slurp(argv[1], &len);
    if (!raw) {
        puts("{\"success\":false,\"error\":\"failed to read input json\"}");
        return 1;
    }
    char msg[1024];
    extract_message(raw, msg, sizeof(msg));
    printf("{\"success\":true,\"output\":\"");
    json_escape(msg);
    printf("\",\"input_bytes\":%ld}\n", len);
    free(raw);
    return 0;
}
