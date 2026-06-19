CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra

BIN := build/c_echo_tool

all: $(BIN)

$(BIN): main.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< -o $@

smoke: $(BIN)
	printf '%s' '{"message":"hello from c"}' >/tmp/c_echo_tool.json
	./$(BIN) /tmp/c_echo_tool.json

clean:
	rm -rf build
