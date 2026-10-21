#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

#---------------------------------------------------------------------------------
# TARGET       is the name of the output
# BUILD        is the directory where object files & intermediates are placed
# SOURCES      is a list of directories containing source code
# INCLUDES     is a list of directories containing extra header files
#---------------------------------------------------------------------------------
TARGET      := QuizFabric3DS
BUILD       := build
SOURCES     := source
DATA        := data
INCLUDES    := source

APP_TITLE       := QuizFabric
APP_DESCRIPTION := Join live QuizFabric quizzes
APP_AUTHOR      := adam

ICON            := assets/icon.png
BANNER_IMAGE    := assets/banner.png
BANNER_AUDIO    := assets/audio.wav
RSF             := assets/app.rsf

VERSION_MAJOR := 1
VERSION_MINOR := 0
VERSION_MICRO := 0

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH    := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS  := -g -Wall -O2 -mword-relocations \
           -ffunction-sections \
           $(ARCH)

CFLAGS  += $(INCLUDE) -D__3DS__

CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11

ASFLAGS := -g $(ARCH)
LDFLAGS  = -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS    := -lcitro2d -lcitro3d -lctru -lm

#---------------------------------------------------------------------------------
# list of directories containing libraries
#---------------------------------------------------------------------------------
LIBDIRS := $(CTRULIB)

#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR := $(CURDIR)

export VPATH := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                $(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
#---------------------------------------------------------------------------------
	export LD := $(CC)
#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
	export LD := $(CXX)
#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------

export OFILES_BIN     := $(addsuffix .o,$(BINFILES))
export OFILES_SOURCES := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES         := $(OFILES_BIN) $(OFILES_SOURCES)

export HFILES     := $(addsuffix .h,$(subst .,_,$(BINFILES)))
export HFILES_BIN := $(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(CURDIR)/$(BUILD)

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export APP_ICON := $(TOPDIR)/$(ICON)

export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh

# Exported so the inner make can reach the CIA inputs and metadata.
export APP_TITLE
export APP_DESCRIPTION
export APP_AUTHOR
export BANNER_IMAGE
export BANNER_AUDIO
export RSF
export VERSION_MAJOR
export VERSION_MINOR
export VERSION_MICRO

.PHONY: all 3dsx cia clean

#---------------------------------------------------------------------------------
all: 3dsx cia

3dsx: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile 3dsx-build

cia: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile cia-build

$(BUILD):
	@[ -d $@ ] || mkdir -p $@

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(OUTPUT).smdh $(TARGET).elf $(TARGET).cia

#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
# Inner make: $(CURDIR) is the build directory.
#---------------------------------------------------------------------------------

DEPENDS := $(OFILES:.o=.d)

.PHONY: 3dsx-build cia-build

3dsx-build: $(OUTPUT).3dsx
cia-build:  $(OUTPUT).cia

$(OUTPUT).3dsx : $(OUTPUT).elf $(OUTPUT).smdh
$(OUTPUT).elf  : $(OFILES)

$(OFILES_SOURCES) : $(HFILES)

#---------------------------------------------------------------------------------
# CIA packaging. bannertool builds the banner + SMDH-style icon, makerom wraps
# the ELF into an installable title.
#---------------------------------------------------------------------------------
$(OUTPUT).cia : $(OUTPUT).elf banner.bnr icon.icn
	@echo packaging cia ...
	@makerom -f cia -target t -exefslogo \
		-o $@ \
		-elf $(OUTPUT).elf \
		-rsf $(TOPDIR)/$(RSF) \
		-icon icon.icn \
		-banner banner.bnr \
		-major $(VERSION_MAJOR) -minor $(VERSION_MINOR) -micro $(VERSION_MICRO)
	@echo built ... $(notdir $@)

banner.bnr : $(TOPDIR)/$(BANNER_IMAGE) $(TOPDIR)/$(BANNER_AUDIO)
	@echo building banner ...
	@bannertool makebanner -i $(TOPDIR)/$(BANNER_IMAGE) -a $(TOPDIR)/$(BANNER_AUDIO) -o $@

icon.icn : $(TOPDIR)/$(ICON)
	@echo building icon ...
	@bannertool makesmdh -s "$(APP_TITLE)" -l "$(APP_DESCRIPTION)" -p "$(APP_AUTHOR)" -i $(TOPDIR)/$(ICON) -o $@

#---------------------------------------------------------------------------------
%.bin.o %_bin.h : %.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
