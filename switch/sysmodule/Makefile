.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET    := nxstream
TITLE_ID  := 4200000000004E53
VERSION   := 1.0.0
BUILD     := build
SOURCES   := source source/core source/net source/capture
DATA      :=
INCLUDES  := source
APP_JSON  := $(TOPDIR)/$(TARGET).json

ARCH     := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS   := -g -Wall -Wextra -Wno-missing-field-initializers -O2 -ffunction-sections \
            $(ARCH) $(DEFINES) $(INCLUDE) -D__SWITCH__ -DNXSTREAM_VERSION=\"$(VERSION)\" \
            $(if $(strip $(NXSTREAM_MODULE_ID)),-DNXSTREAM_CONTENT_ID=\"$(NXSTREAM_MODULE_ID)\")
CXXFLAGS := $(CFLAGS) -std=gnu++20 -fno-rtti -fno-exceptions
ASFLAGS  := -g $(ARCH)
LDFLAGS   = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map) -Wl,--gc-sections

LIBS     := -lnx
LIBDIRS  := $(PORTLIBS) $(LIBNX)

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT  := $(CURDIR)/$(TARGET)
export TOPDIR  := $(CURDIR)
export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)
export APP_JSON

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))

export LD        := $(CXX)
export OFILES    := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o)
export INCLUDE   := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                    $(foreach dir,$(LIBDIRS),-I$(dir)/include) -I$(CURDIR)/$(BUILD)
export LIBPATHS  := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

DIST := $(CURDIR)/../dist/atmosphere/contents/$(TITLE_ID)

.PHONY: $(BUILD) clean all dist

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

dist: all
	@rm -rf $(CURDIR)/../dist
	@mkdir -p $(DIST)/flags $(CURDIR)/../dist/config/nxstream
	@cp $(TARGET).nsp $(DIST)/exefs.nsp
	@touch $(DIST)/flags/boot2.flag
	@cp toolbox.json $(DIST)/toolbox.json
	@printf "%s %s\n" "$(if $(strip $(NXSTREAM_MODULE_ID)),$(strip $(NXSTREAM_MODULE_ID)),dev)" "$(VERSION)" > $(DIST)/nxstream.id
	@if [ -f $(CURDIR)/../manager/nxstream.nro ]; then 		mkdir -p $(CURDIR)/../dist/switch/NXStream && cp $(CURDIR)/../manager/nxstream.nro $(CURDIR)/../dist/switch/NXStream/nxstream.nro; 	else echo "warning: manager/nxstream.nro not built (make -C switch/manager)"; fi
	@echo "dist ready: copy the contents of switch/dist to the root of the SD card"

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(CURDIR)/../dist $(TARGET).nsp $(TARGET).nso $(TARGET).npdm $(TARGET).elf

else
.PHONY: all

DEPENDS := $(OFILES:.o=.d)

all: $(OUTPUT).nsp

$(OUTPUT).nsp: $(OUTPUT).nso $(OUTPUT).npdm

$(OUTPUT).nso: $(OUTPUT).elf

$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

endif
