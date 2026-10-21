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
CFLAGS  += -DPROD_BUILD
endif

ARCH    := -mthumb -mthumb-interwork

CFLAGS  := -g -Wall -O2 -march=armv5te -mtune=arm946e-s \
           -fomit-frame-pointer -ffast-math $(ARCH)

CFLAGS  += $(INCLUDE) -DARM9 -D__NDS__

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

.PHONY: $(BUILD) clean

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nds $(TARGET).elf

else

DEPENDS := $(OFILES:.o=.d)

$(OUTPUT).nds : $(OUTPUT).elf
$(OUTPUT).elf : $(OFILES)

%.o: %.c
	@echo $(notdir $<)
	@$(CC) $(CFLAGS) -MMD -MP -MF $(DEPSDIR)/$*.d -c $< -o $@

%.o: %.s
	@echo $(notdir $<)
	@$(CC) -x assembler-with-cpp $(ASFLAGS) -MMD -MP -MF $(DEPSDIR)/$*.d -c $< -o $@

-include $(DEPENDS)

endif
