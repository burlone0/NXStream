.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

APP_TITLE   := NXStream
APP_AUTHOR  := NXStream
APP_VERSION := 1.0.0

TARGET   := nxstream
BUILD    := build
SOURCES  := source
DATA     :=
INCLUDES := source
ROMFS    := romfs
ICON     := icon.jpg

PKGCONF  := $(PORTLIBS)/bin/$(PREFIX)pkg-config
PKGS     := SDL2_ttf SDL2_image sdl2 libcurl

ARCH     := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS   := -g -Wall -Wextra -Wno-missing-field-initializers -O2 -ffunction-sections \
            $(ARCH) $(DEFINES) $(INCLUDE) -D__SWITCH__ -DNXSTREAM_VERSION=\"$(APP_VERSION)\" \
            $(if $(strip $(NXSTREAM_MANAGER_ID)),-DNXSTREAM_CONTENT_ID=\"$(strip $(NXSTREAM_MANAGER_ID))\") \
            $(shell $(PKGCONF) --cflags $(PKGS))
CXXFLAGS := $(CFLAGS) -std=gnu++20 -fno-rtti -fno-exceptions
ASFLAGS  := -g $(ARCH)
LDFLAGS   = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS     := $(shell $(PKGCONF) --static --libs $(PKGS)) -lnx
LIBDIRS  := $(PORTLIBS) $(LIBNX)

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT  := $(CURDIR)/$(TARGET)
export TOPDIR  := $(CURDIR)
export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))

export LD        := $(CXX)
export OFILES    := $(CPPFILES:.cpp=.o)
export INCLUDE   := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                    $(foreach dir,$(LIBDIRS),-I$(dir)/include) -I$(CURDIR)/$(BUILD)
export LIBPATHS  := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export APP_ICON  := $(TOPDIR)/$(ICON)
export NROFLAGS  += --icon=$(APP_ICON) --nacp=$(CURDIR)/$(TARGET).nacp --romfsdir=$(CURDIR)/$(ROMFS)

.PHONY: $(BUILD) clean all

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf

else
.PHONY: all

DEPENDS := $(OFILES:.o=.d)

all: $(OUTPUT).nro

$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp

$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

endif
