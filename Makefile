# markee — Simulated 72x16 24-bit RGB LED Matrix Display (raylib)

TARGET      := markee
TEST_TARGET := test_api

CC          ?= cc
DEFAULT_PREFIX := $(shell \
	if [ -n "$$HOME" ]; then \
		if [ -d "$$HOME/.local/bin" ] && [ -w "$$HOME/.local/bin" ]; then echo "$$HOME/.local"; \
		elif [ ! -e "$$HOME/.local/bin" ] && [ -d "$$HOME/.local" ] && [ -w "$$HOME/.local" ]; then echo "$$HOME/.local"; \
		elif [ ! -e "$$HOME/.local" ] && [ -w "$$HOME" ]; then echo "$$HOME/.local"; \
		else echo "/usr/local"; fi; \
	else echo "/usr/local"; fi)
PREFIX      ?= $(DEFAULT_PREFIX)
BINDIR      ?= $(PREFIX)/bin

SRC_DIR     := src
INC_DIR     := include
TEST_DIR    := tests
BUILD_DIR   := build

SRCS        := $(SRC_DIR)/main.c \
               $(SRC_DIR)/audio.c \
               $(SRC_DIR)/cli.c \
               $(SRC_DIR)/display.c \
               $(SRC_DIR)/draw.c \
               $(SRC_DIR)/gif.c \
               $(SRC_DIR)/json.c \
               $(SRC_DIR)/renderer.c \
               $(SRC_DIR)/server.c \
               $(SRC_DIR)/webui.c

CORE_SRCS   := $(SRC_DIR)/audio.c \
               $(SRC_DIR)/cli.c \
               $(SRC_DIR)/display.c \
               $(SRC_DIR)/draw.c \
               $(SRC_DIR)/gif.c \
               $(SRC_DIR)/json.c \
               $(SRC_DIR)/renderer.c \
               $(SRC_DIR)/server.c \
               $(SRC_DIR)/webui.c

OBJS        := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))
CORE_OBJS   := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(CORE_SRCS))
TEST_OBJ    := $(BUILD_DIR)/test_api.o
DEPS        := $(OBJS:.o=.d) $(TEST_OBJ:.o=.d)

# Detect raylib flags via pkg-config with Homebrew/standard fallbacks
RAYLIB_CFLAGS := $(shell pkg-config --cflags raylib 2>/dev/null)
RAYLIB_LIBS   := $(shell pkg-config --libs raylib 2>/dev/null)

ifeq ($(strip $(RAYLIB_LIBS)),)
  UNAME_S := $(shell uname -s)
  ifeq ($(UNAME_S),Darwin)
    BREW_PREFIX := $(shell brew --prefix raylib 2>/dev/null || echo /opt/homebrew/opt/raylib)
    RAYLIB_CFLAGS := -I$(BREW_PREFIX)/include
    RAYLIB_LIBS   := -L$(BREW_PREFIX)/lib -lraylib -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo -framework CoreAudio -framework AudioToolbox
  else
    RAYLIB_LIBS   := -lraylib -lGL -lm -ldl -lrt -lX11
  endif
endif

WARN_FLAGS  := -Wall -Wextra -Wpedantic
OPT_FLAGS   ?= -O2
CFLAGS      := -std=c11 $(WARN_FLAGS) $(OPT_FLAGS) -I$(INC_DIR) $(RAYLIB_CFLAGS)
DEPFLAGS    := -MMD -MP
LDFLAGS     ?=
LDLIBS      := $(RAYLIB_LIBS) -lpthread -lm
SAN_FLAGS   := -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
SAN_TARGET  := test_api_san
FUZZ_DIR    := $(TEST_DIR)/fuzz
FUZZ_OUT    := $(BUILD_DIR)/fuzz

.PHONY: all debug release run test sanitize sanitize-build fuzz-smoke fuzz clean distclean install uninstall help

all: $(TARGET)

debug: OPT_FLAGS := -O0 -g -DDEBUG
debug: clean $(TARGET)

release: OPT_FLAGS := -O3 -DNDEBUG
release: clean $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(TEST_TARGET): $(CORE_OBJS) $(TEST_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

$(BUILD_DIR)/test_api.o: $(TEST_DIR)/test_api.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

run: $(TARGET)
	./$(TARGET)

test: $(TEST_TARGET)
	./$(TEST_TARGET)

$(SAN_TARGET): $(CORE_SRCS) $(TEST_DIR)/test_api.c
	$(CC) -std=c11 $(WARN_FLAGS) $(SAN_FLAGS) -I$(INC_DIR) $(RAYLIB_CFLAGS) -o $@ $(CORE_SRCS) $(TEST_DIR)/test_api.c $(LDLIBS)

sanitize-build: $(SAN_TARGET)

sanitize: $(SAN_TARGET)
	./$(SAN_TARGET)

fuzz-smoke:
	@mkdir -p $(FUZZ_OUT)
	$(CC) -std=c11 $(WARN_FLAGS) $(SAN_FLAGS) -DFUZZ_STANDALONE -I$(INC_DIR) -I$(FUZZ_DIR) -o $(FUZZ_OUT)/fuzz_gif $(FUZZ_DIR)/fuzz_gif.c $(SRC_DIR)/gif.c -lm
	$(CC) -std=c11 $(WARN_FLAGS) $(SAN_FLAGS) -DFUZZ_STANDALONE -I$(INC_DIR) -I$(FUZZ_DIR) -o $(FUZZ_OUT)/fuzz_json $(FUZZ_DIR)/fuzz_json.c $(SRC_DIR)/json.c -lm
	$(CC) -std=c11 $(WARN_FLAGS) $(SAN_FLAGS) -DFUZZ_STANDALONE -I$(INC_DIR) -I$(FUZZ_DIR) $(RAYLIB_CFLAGS) -o $(FUZZ_OUT)/fuzz_draw $(FUZZ_DIR)/fuzz_draw.c $(CORE_SRCS) $(LDLIBS)
	$(CC) -std=c11 $(WARN_FLAGS) $(SAN_FLAGS) -DFUZZ_STANDALONE -I$(INC_DIR) -I$(FUZZ_DIR) $(RAYLIB_CFLAGS) -o $(FUZZ_OUT)/fuzz_http $(FUZZ_DIR)/fuzz_http.c $(CORE_SRCS) $(LDLIBS)
	./$(FUZZ_OUT)/fuzz_gif examples/*.gif
	./$(FUZZ_OUT)/fuzz_json
	./$(FUZZ_OUT)/fuzz_draw
	./$(FUZZ_OUT)/fuzz_http

fuzz:
	@mkdir -p $(FUZZ_OUT)
	$(CC) -std=c11 $(WARN_FLAGS) -O1 -g -fsanitize=fuzzer,address,undefined -I$(INC_DIR) -I$(FUZZ_DIR) -o $(FUZZ_OUT)/libfuzz_gif $(FUZZ_DIR)/fuzz_gif.c $(SRC_DIR)/gif.c -lm
	$(CC) -std=c11 $(WARN_FLAGS) -O1 -g -fsanitize=fuzzer,address,undefined -I$(INC_DIR) -I$(FUZZ_DIR) -o $(FUZZ_OUT)/libfuzz_json $(FUZZ_DIR)/fuzz_json.c $(SRC_DIR)/json.c -lm
	$(CC) -std=c11 $(WARN_FLAGS) -O1 -g -fsanitize=fuzzer,address,undefined -I$(INC_DIR) -I$(FUZZ_DIR) $(RAYLIB_CFLAGS) -o $(FUZZ_OUT)/libfuzz_draw $(FUZZ_DIR)/fuzz_draw.c $(CORE_SRCS) $(LDLIBS)
	$(CC) -std=c11 $(WARN_FLAGS) -O1 -g -fsanitize=fuzzer,address,undefined -I$(INC_DIR) -I$(FUZZ_DIR) $(RAYLIB_CFLAGS) -o $(FUZZ_OUT)/libfuzz_http $(FUZZ_DIR)/fuzz_http.c $(CORE_SRCS) $(LDLIBS)

install: $(TARGET)
	install -d "$(DESTDIR)$(BINDIR)"
	install -m 0755 $(TARGET) "$(DESTDIR)$(BINDIR)/$(TARGET)"
	@if [ -z "$(DESTDIR)" ]; then \
		case ":$$PATH:" in \
			*":$(BINDIR):"*) ;; \
			*) echo "Note: $(BINDIR) is not in your PATH" ;; \
		esac; \
	fi

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/$(TARGET)"

clean:
	rm -rf $(BUILD_DIR) $(TARGET) $(TEST_TARGET) $(SAN_TARGET)

distclean: clean
	rm -f snapshot.png recording.gif

help:
	@echo "markee build targets:"
	@echo "  make            Build $(TARGET) (default)"
	@echo "  make debug      Rebuild with debug symbols (-O0 -g)"
	@echo "  make release    Rebuild with full optimization (-O3)"
	@echo "  make run        Build and launch ./$(TARGET)"
	@echo "  make test       Build and run the C unit & socket API test suite"
	@echo "  make sanitize   Build and run tests with ASan + UBSan"
	@echo "  make fuzz-smoke Build and run standalone ASan/UBSan parser fuzzers"
	@echo "  make fuzz       Build libFuzzer binaries (requires LLVM clang)"
	@echo "  make install    Install $(TARGET) to $(BINDIR)"
	@echo "  make uninstall  Remove $(TARGET) from $(BINDIR)"
	@echo "  make clean      Remove build artifacts and binaries"
	@echo "  make distclean  Clean everything including snapshot.png and recording.gif"

-include $(DEPS)
