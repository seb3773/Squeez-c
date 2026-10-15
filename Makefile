CC ?= gcc
CFLAGS ?= -O3 -flto -fomit-frame-pointer -Wall -Wextra -std=c99 -Isrc
LDFLAGS ?= -lm
BUILD_DIR ?= build
LINUX_DIR = $(BUILD_DIR)/linux
WIN64_DIR = $(BUILD_DIR)/win64

TARGET_LINUX = $(LINUX_DIR)/sqx
TARGET_WIN64 = $(WIN64_DIR)/sqx.exe
TARGET = $(TARGET_LINUX)

CROSS_WIN64_CC ?= x86_64-w64-mingw32-gcc

PREFIX ?= /usr/local
DESTDIR ?=

SRC = src/main.c src/sqx.c src/lz.c src/huff.c src/henc.c src/enc.c src/ir.c src/create.c src/rc.c src/delta.c src/audio.c src/ppm8.c
HDR = $(wildcard src/*.h)

.PHONY: all linux win64 all-platforms clean install uninstall test help debug

all: linux

linux: $(TARGET_LINUX)

win64: $(TARGET_WIN64)

all-platforms: linux win64

$(LINUX_DIR):
	mkdir -p $(LINUX_DIR)

$(WIN64_DIR):
	mkdir -p $(WIN64_DIR)

$(TARGET_LINUX): $(SRC) $(HDR) | $(LINUX_DIR)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)
	@ln -sf linux/sqx $(BUILD_DIR)/sqx
	@mkdir -p bin && ln -sf ../build/linux/sqx bin/sqx

$(TARGET_WIN64): $(SRC) $(HDR) | $(WIN64_DIR)
	$(CROSS_WIN64_CC) $(CFLAGS) -static -o $@ $(SRC) $(LDFLAGS)

debug: CFLAGS = -g -O0 -Wall -Wextra -std=c99 -Isrc
debug: clean $(TARGET_LINUX)

test: $(TARGET_LINUX)
	@echo "Running SQX validation test suite..."
	sh tests/test_container.sh
	sh tests/test_extract.sh
	sh tests/test_audio.sh
	sh tests/test_bitexact.sh
	sh tests/test_encoder.sh
	sh tests/test_large.sh
	sh tests/test_ppm6.sh
	@echo "All SQX tests passed successfully."

install: $(TARGET_LINUX)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(TARGET_LINUX) $(DESTDIR)$(PREFIX)/bin/sqx

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/sqx

clean:
	rm -rf $(BUILD_DIR) bin *.o src/*.o .tmp_*

help:
	@echo "SQX Archiver build targets:"
	@echo "  make            - Build Linux binary (build/linux/sqx)"
	@echo "  make win64      - Build Windows 64-bit binary (build/win64/sqx.exe)"
	@echo "  make all-platforms - Build both Linux and Windows binaries"
	@echo "  make debug      - Build debug binary with symbols"
	@echo "  make test       - Run test suite against reference archives"
	@echo "  make install    - Install binary to PREFIX (default: /usr/local)"
	@echo "  make clean      - Clean build artifacts"
