CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -I/usr/include/jsoncpp
LDFLAGS ?= -ljsoncpp

BIN := build/cpp_status_feed

all: $(BIN)

$(BIN): main.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

smoke: $(BIN)
	./$(BIN)

clean:
	rm -rf build
