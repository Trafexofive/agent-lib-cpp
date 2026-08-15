CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2 -g
# inkcell sibling repo. Override INKCELL_DIR if installed elsewhere.
INKCELL_DIR ?= ../../../inkcell

CPPFLAGS := -I$(INKCELL_DIR)/include -Isrc -I/usr/include/jsoncpp
LDFLAGS  := $(INKCELL_DIR)/build/libinkcell.a -ljsoncpp -lpthread

BUILD := build
BIN   := $(BUILD)/voice-console
SRC   := src/main.cpp
HEADERS := $(wildcard src/*.hpp)

.PHONY: all run live snapshot clean inkcell

all: $(BIN)

inkcell:
	$(MAKE) -C $(INKCELL_DIR) lib

$(BIN): $(SRC) $(HEADERS) inkcell
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(SRC) $(LDFLAGS) -o $@

run: snapshot
live: $(BIN)
	@$(BIN) --live
snapshot: $(BIN)
	@$(BIN) --snapshot

clean:
	rm -rf $(BUILD)
