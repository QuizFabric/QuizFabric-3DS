# QuizFabric DS client - libnds build.
#   make            -> dev build (QuizFabricDS.nds)
#   make PROD=1     -> production build (QuizFabricDSProd.nds)

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

include $(DEVKITARM)/ds_rules

TARGET  := QuizFabricDS
BUILD   := build
SOURCES := source

ifdef PROD
TARGET  := QuizFabricDSProd
BUILD   := build_prod
DS_RSF  := dsiware_prod.rsf
GAME_TITLE := QuizFabPROD
GAME_CODE  := QZDP
else
DS_RSF  := dsiware.rsf
GAME_TITLE := QuizFabricDS
GAME_CODE  := QZDS
endif

# ndstool banner text. The 12-char ROM header title itself (what the DSi
# home menu shows - TWL CIAs read the title from the SRL, not the TMD) is
# set with ndstool -g in the %.nds recipe below.
GAME_SUBTITLE1 := by Nekky9180
GAME_SUBTITLE2 :=
MAKER_CODE := NK

# The header CRC patcher is a python script; the MSYS login shell trims
# WindowsApps off PATH, so point straight at the interpreter.
PYTHON := /c/Users/adam/AppData/Local/Microsoft/WindowsApps/python.exe

# DSi title ID LOW, patched into the .nds header at offset 0x230 (ndstool
# derives the TID from the game code). EVERY build consumes a fresh TID
# from tools/tid_counter (bumped by fix_dsi_tid.py), so reinstalling a
# rebuilt CIA always counts as new software on the home menu. makerom
# copies the SRL TID into the CIA's TMD.
# (TOPDIR, not CURDIR: the recipe runs inside the build directory.)
TID_FILE := $(TOPDIR)/tools/tid_counter
DSI_TID  := $(shell cat $(TID_FILE))

ARCH    := -mthumb -mthumb-interwork

CFLAGS  := -g -Wall -O2 -march=armv5te -mtune=arm946e-s \
           -fomit-frame-pointer -ffast-math $(ARCH)

CFLAGS  += $(INCLUDE) -DARM9 -D__NDS__

# Production define must come AFTER the CFLAGS := above or it gets wiped.
ifdef PROD
CFLAGS  += -DPROD_BUILD
endif

# MINIMAL=1: build the minimal boot test (main shows one big number).
ifdef MINIMAL
CFLAGS  += -DMINIMAL
endif

CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions

ASFLAGS := -g $(ARCH)
LDFLAGS = -specs=ds_arm9.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS    := -lnds9 -ldswifi9

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT  := $(CURDIR)/$(TARGET)
export TOPDIR  := $(CURDIR)
export DEPSDIR := $(CURDIR)/$(BUILD)

export VPATH := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

export OFILES := $(CFILES:.c=.o) $(SFILES:.s=.o)

export LD := $(CC)
export LIBPATHS := -L$(LIBNDS)/lib -L$(CALICO)/lib

export INCLUDE := $(foreach dir,$(SOURCES),-I$(CURDIR)/$(dir)) \
                  -I$(LIBNDS)/include -I$(CALICO)/include -I$(CURDIR)/$(BUILD)

.PHONY: $(BUILD) dsiware clean

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

# DSiWare .cia wrapper around the .nds: runs straight from the DSi home
# menu (or FBI on a 3DS) without TWiLightMenu++.
# cd + relative paths: makerom splits -i specs on ':' so drive letters break it.
dsiware: $(BUILD)
	@echo making cia ...
	@cd $(CURDIR) && "$(DEVKITPRO)/tools/makerom/makerom.exe" -f cia -target t \
		-rsf $(DS_RSF) -o $(TARGET).cia -srl $(TARGET).nds

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nds $(TARGET).elf $(TARGET).cia

else

DEPENDS := $(OFILES:.o=.d)

# Explicit recipe (beats the ds_rules pattern rule) so we can pass -g to
# ndstool: game code + the 12-char ROM header title the DSi home menu shows.
# -u/-z are the DSi-mode fields (TID high + ARM7 SCFG_EXT mask) that
# real DSiWare CIAs require - same values NTM (a working DSiWare title
# manager) uses. Without them TWL mode crashes on launch.
# Keep this as the FIRST rule in the inner makefile - it is the default goal.
$(OUTPUT).nds : $(OUTPUT).elf
	@ndstool -c $@ -9 $< $(_ARM7_ELF) -g $(GAME_CODE) $(MAKER_CODE) $(GAME_TITLE) -z 80040000 -u 00030004 -b $(GAME_ICON) "$(GAME_TITLE);$(GAME_SUBTITLE1);$(GAME_SUBTITLE2)" $(_ADDFILES)
	@echo built ... $(notdir $@)
	@$(PYTHON) $(TOPDIR)/tools/fix_dsi_tid.py $@ $(DSI_TID)
	@$(PYTHON) $(TOPDIR)/tools/fix_dsi_tid.py --bump $(TOPDIR)/tools/tid_counter

$(OUTPUT).elf : $(OFILES)

%.o: %.c
	@echo $(notdir $<)
	@$(CC) $(CFLAGS) -MMD -MP -MF $(DEPSDIR)/$*.d -c $< -o $@

%.o: %.s
	@echo $(notdir $<)
	@$(CC) -x assembler-with-cpp $(ASFLAGS) -MMD -MP -MF $(DEPSDIR)/$*.d -c $< -o $@

-include $(DEPENDS)

endif
