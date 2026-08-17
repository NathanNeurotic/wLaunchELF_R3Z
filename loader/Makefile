EE_CFLAGS  := -G0 -O2 -EL -Wall -D_EE
EE_ASFLAGS += -EL -G0

# Standard LaunchELF loader placement:
# Loader body at 0x90000, stack at 0xA8000, stack size 0x4000.
LOADADDR  = 0x90000
STACKADDR = 0xA8000
STACKSIZE = 0x04000

LDPARAMS := -Wl,--defsym -Wl,_stack_size=$(STACKSIZE) -Wl,--defsym -Wl,_stack=$(STACKADDR)

EE_LDFLAGS += -Wl,-Ttext -Wl,$(LOADADDR) -s $(LDPARAMS)

LOW_MEM_LIMIT = 0x100000
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
