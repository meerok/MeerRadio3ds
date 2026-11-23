#---------------------------------------------------------------------------------
# Meer Radio - 3DS AM/FM internet radio
#   make        -> MeerRadio.3dsx
#   make cia    -> MeerRadio.cia   (needs makerom + bannertool on PATH)
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

TARGET          :=  MeerRadio
BUILD           :=  build
SOURCES         :=  source
DATA            :=
INCLUDES        :=  source

APP_TITLE       :=  Meer Radio
APP_DESCRIPTION :=  AM/FM radio for 3DS
APP_AUTHOR      :=  Meer
ICON            :=  assets/icon.png

VERSION_MAJOR   :=  1
VERSION_MINOR   :=  0
VERSION_MICRO   :=  0

#---------------------------------------------------------------------------------
ARCH    :=  -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS  :=  -g -Wall -O2 -mword-relocations -ffunction-sections $(ARCH)
CFLAGS  +=  $(INCLUDE) -D__3DS__
CXXFLAGS:=  $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11
ASFLAGS :=  -g $(ARCH)
LDFLAGS  =  -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS    :=  -lcitro2d -lcitro3d -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lmpg123 -lz -lctru -lm
LIBDIRS :=  $(PORTLIBS) $(CTRULIB)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT   :=  $(CURDIR)/$(TARGET)
export TOPDIR   :=  $(CURDIR)
export VPATH    :=  $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR  :=  $(CURDIR)/$(BUILD)

CFILES      :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))

export LD   :=  $(CC)
export OFILES_SOURCES := $(CFILES:.c=.o)
export OFILES   :=  $(OFILES_SOURCES)
export HFILES   :=

export INCLUDE  :=  $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                    $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                    -I$(CURDIR)/$(BUILD)
export LIBPATHS :=  $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export APP_ICON := $(TOPDIR)/$(ICON)
export _3DSXDEPS := $(OUTPUT).smdh
export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh

.PHONY: all clean cia

all: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(BUILD):
	@mkdir -p $@

cia: all
	@echo building banner, icon and cia ...
	@bannertool makebanner -i assets/banner.png -a assets/banner.wav -o $(BUILD)/banner.bnr
	@bannertool makesmdh -s "$(APP_TITLE)" -l "$(APP_DESCRIPTION)" -p "$(APP_AUTHOR)" -i $(ICON) -o $(BUILD)/icon.icn
	@makerom -f cia -o $(TARGET).cia -target t -exefslogo -elf $(TARGET).elf -rsf assets/cia.rsf \
		-icon $(BUILD)/icon.icn -banner $(BUILD)/banner.bnr \
		-major $(VERSION_MAJOR) -minor $(VERSION_MINOR) -micro $(VERSION_MICRO)
	@echo built ... $(TARGET).cia

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(TARGET).smdh $(TARGET).elf $(TARGET).cia

#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------

DEPENDS :=  $(OFILES:.o=.d)

$(OUTPUT).3dsx  :   $(OUTPUT).elf $(_3DSXDEPS)
$(OFILES_SOURCES) : $(HFILES)
$(OUTPUT).elf   :   $(OFILES)

-include $(DEPENDS)

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
