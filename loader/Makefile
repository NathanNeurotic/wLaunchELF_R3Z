EE_CFLAGS  := -G0 -Os -EL -Wall -D_EE -ffunction-sections -fdata-sections
EE_ASFLAGS += -EL -G0

# Standard LaunchELF loader placement in low RAM:
# Loader body at 0x84000, stack at 0x9e000, stack size 0x2000.
LOADADDR  = 0x84000
STACKADDR = 0x9e000
STACKSIZE = 0x02000

LDPARAMS := -Wl,--defsym -Wl,_stack_size=$(STACKSIZE) -Wl,--defsym -Wl,_stack=$(STACKADDR)

EE_LDFLAGS += -Wl,-Ttext -Wl,$(LOADADDR) -s -Wl,--gc-sections $(LDPARAMS)

LOW_MEM_LIMIT = 0xa0000
LAYOUT_CHECK_SCRIPT = ../scripts/check_loader_layout.sh
EE_NM ?= $(EE_TOOL_PREFIX)nm

EE_BIN = loader.elf
EE_OBJS = loader.o

EE_LIBS =
ifeq ($(DEBUG),1)
EE_LIBS += -ldebug
endif

all: $(EE_BIN) check-layout

check-layout: $(EE_BIN)
	sh $(LAYOUT_CHECK_SCRIPT) "$(EE_BIN)" "$(EE_NM)" "$(LOADADDR)" "$(STACKADDR)" "$(STACKSIZE)" "$(LOW_MEM_LIMIT)"

clean:
	rm -f -r $(EE_OBJS) $(EE_BIN)

include $(PS2SDK)/Defs.make
include Rules.make
