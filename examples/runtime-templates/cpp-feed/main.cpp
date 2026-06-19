#include <json/json.h>

#include <chrono>
#include <iostream>

int main() {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();

    Json::Value r;
    r["status"] = "ok";
    r["source"] = "cpp_status_feed";
    r["timestamp_ms"] = static_cast<Json::Int64>(ms);

    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    std::cout << Json::writeString(w, r) << std::endl;
    return 0;
}
