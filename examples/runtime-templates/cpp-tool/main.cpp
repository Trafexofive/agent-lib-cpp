#include <json/json.h>

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

static std::string slurp(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

static void emit_error(const std::string& msg) {
    Json::Value r;
    r["success"] = false;
    r["error"] = msg;
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    std::cout << Json::writeString(w, r) << std::endl;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        emit_error("usage: cpp_echo_tool <input.json>");
        return 2;
    }

    std::string raw = slurp(argv[1]);
    Json::Value input;
    Json::CharReaderBuilder reader;
    std::string errs;
    std::istringstream ss(raw);
    if (!Json::parseFromStream(reader, ss, &input, &errs)) {
        emit_error("invalid json: " + errs);
        return 1;
    }

    Json::Value r;
    r["success"] = true;
    r["output"] = input.get("message", "hello").asString();
    r["input_bytes"] = static_cast<Json::UInt64>(raw.size());

    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    std::cout << Json::writeString(w, r) << std::endl;
    return 0;
}
