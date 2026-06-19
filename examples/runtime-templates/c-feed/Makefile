CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra

BIN := build/c_status_feed

all: $(BIN)

$(BIN): main.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< -o $@

smoke: $(BIN)
	./$(BIN)

clean:
	rm -rf build
