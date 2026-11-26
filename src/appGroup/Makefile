INCLUDE_SHMVAR=1
INCLUDE_CJSON=1
include ../common/config.mk

TARGET = appGroup
CFLAGS := $(CFLAGS) -DHAS_AUDIO
LDFLAGS := $(LDFLAGS) -lSDL -lSDL_ttf -lSDL_image -lSDL_mixer -lSDL_rotozoom -lpng -pthread

include ../common/commands.mk
include ../common/recipes.mk
