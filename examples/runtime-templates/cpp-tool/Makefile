CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -I/usr/include/jsoncpp
LDFLAGS ?= -ljsoncpp

BIN := build/cpp_echo_tool

all: $(BIN)

$(BIN): main.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

smoke: $(BIN)
	printf '%s' '{"message":"hello from c++"}' >/tmp/cpp_echo_tool.json
	./$(BIN) /tmp/cpp_echo_tool.json

clean:
	rm -rf build
