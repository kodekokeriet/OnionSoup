INCLUDE_CJSON=1
include ../common/config.mk

TARGET = jellyfin
LDFLAGS := $(LDFLAGS) -lSDL -lSDL_image -lSDL_ttf
ifeq ($(PLATFORM),miyoomini)
LDFLAGS := $(LDFLAGS) -lkbinput
endif

include ../common/commands.mk
include ../common/recipes.mk
