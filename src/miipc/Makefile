# MiiPC - sample-based groovebox for Miyoo Mini / Mini Plus / Mini v4 (Onion OS)
#   make                 -> PC build (testing, SDL 1.2 / sdl12-compat)
#   make PLATFORM=miyoo  -> cross-compile for Miyoo (ARMv7, Cortex-A7)
#   make package         -> dist/MiiPC/ ready for /mnt/SDCARD/App/
#   make linux-pkg       -> dist/miipc-linux-x86_64.tar.gz test build

PLATFORM ?= desktop
TARGET   := miipc
SRC      := $(wildcard src/*.c)
OBJ      := $(patsubst src/%.c,build/$(PLATFORM)/%.o,$(SRC))

CFLAGS   += -std=c99 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation

ifeq ($(PLATFORM),miyoo)
  # Miyoo Mini: SDL 1.2 from Onion
  CROSS     ?= arm-linux-gnueabihf-
  CC        := $(CROSS)gcc
  SYSROOT   ?= $(shell $(CC) --print-sysroot)
  CFLAGS    += -marm -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -ffast-math \
               -DMIYOO -I$(SYSROOT)/usr/include/SDL
  LDLIBS    += -lSDL -lpthread -lm
else ifeq ($(PLATFORM),linux)
  # Portable Linux release: zig cc targeting glibc 2.17 + static SDL2
  # (see tools/build-linux-release.sh). Runs on any x86_64 distro since ~2014.
  ZIGCC     ?= zig cc -target x86_64-linux-gnu.2.17
  CC        := $(ZIGCC)
  SDL2_PREFIX ?= $(HOME)/sdl2static
  CFLAGS    += -I$(SDL2_PREFIX)/include/SDL2 -D_REENTRANT -O2
  LDLIBS    += $(SDL2_PREFIX)/lib/libSDL2.a -lm -ldl -lpthread -lrt
else ifeq ($(PLATFORM),desktop12)
  # PC with SDL 1.2: same code path as the Miyoo, for checking it builds
  CC        ?= cc
  CFLAGS    += $(filter-out -D_GNU_SOURCE=1,$(shell sdl-config --cflags)) -g
  LDLIBS    += $(shell sdl-config --libs) -lm
else
  # PC development build with the system SDL2
  CC        ?= cc
  CFLAGS    += $(filter-out -D_GNU_SOURCE=1,$(shell sdl2-config --cflags)) -g
  LDLIBS    += $(shell sdl2-config --libs) -lm
endif

all: build/$(PLATFORM)/$(TARGET)

build/$(PLATFORM)/$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

build/$(PLATFORM)/%.o: src/%.c src/*.h | build/$(PLATFORM)
	$(CC) $(CFLAGS) -c -o $@ $<

build/$(PLATFORM):
	mkdir -p $@

kit:
	python3 tools/gen_kit.py samples/kit01

package: build/miyoo/$(TARGET)
	rm -rf dist && mkdir -p dist/MiiPC
	cp -r package/MiiPC/* dist/MiiPC/
	cp build/miyoo/$(TARGET) dist/MiiPC/
	mkdir -p dist/MiiPC/samples && cp -r samples/* dist/MiiPC/samples/

ifneq ($(PLATFORM),miyoo)
build/miyoo/$(TARGET):
	$(MAKE) PLATFORM=miyoo
endif

linux-pkg:
	$(MAKE) PLATFORM=linux
	rm -rf dist/miipc-linux-x86_64 && mkdir -p dist/miipc-linux-x86_64
	cp build/linux/$(TARGET) package/linux/start.sh package/linux/README.txt dist/miipc-linux-x86_64/
	chmod +x dist/miipc-linux-x86_64/$(TARGET) dist/miipc-linux-x86_64/start.sh
	cp -r samples dist/miipc-linux-x86_64/
	tar -C dist -czf dist/miipc-linux-x86_64.tar.gz miipc-linux-x86_64

# Refresh the ready-built PC binary used by start-linux.sh in the project folder.
linux-bin:
	$(MAKE) PLATFORM=linux
	mkdir -p bin
	strip -o bin/miipc-linux-x86_64 build/linux/$(TARGET)
	chmod +x bin/miipc-linux-x86_64

run: all
	./build/desktop/$(TARGET) samples/kit01

clean:
	rm -rf build dist

.PHONY: all kit package linux-pkg linux-bin run clean
