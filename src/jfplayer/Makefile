include ../common/config.mk

TARGET = jfplayer
FFMPEG_INCLUDE ?= ../../include/ffmpeg24
CFLAGS := $(CFLAGS) -I$(FFMPEG_INCLUDE)
ifeq ($(PLATFORM),miyoomini)
# Link against the FFmpeg 2.4 libraries Onion ships in .tmp_update/lib/parasyte
FFMPEG_LIB ?= ../../static/build/.tmp_update/lib/parasyte
endif
ifneq ($(FFMPEG_LIB),)
LDFLAGS := $(LDFLAGS) -L$(FFMPEG_LIB) -Wl,-rpath-link=$(FFMPEG_LIB)
# their own dependencies (bz2, x264, ...) live on the device, not in the toolchain
LDFLAGS := $(LDFLAGS) -Wl,--allow-shlib-undefined
endif
ifeq ($(PLATFORM),miyoomini)
# Onion only ships the versioned names
FFMPEG_LIBS = -l:libavformat.so.56 -l:libavcodec.so.56 -l:libswscale.so.3 -l:libswresample.so.1 -l:libavutil.so.54
else
FFMPEG_LIBS = -lavformat -lavcodec -lswscale -lswresample -lavutil
endif
LDFLAGS := $(LDFLAGS) -lSDL -lSDL_ttf $(FFMPEG_LIBS) -lpthread -lm

include ../common/commands.mk
include ../common/recipes.mk
