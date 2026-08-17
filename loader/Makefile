EE_CFLAGS  := -G0 -O2 -EL -Wall -D_EE -DNEWLIB_PORT_AWARE
EE_ASFLAGS += -EL -G0

LOADADDR  = 0x90000
STACKADDR = 0xA8000
STACKSIZE = 0x04000

LDPARAMS := -Wl,--defsym -Wl,_stack_size=$(STACKSIZE) -Wl,--defsym -Wl,_stack=$(STACKADDR)

EE_LDFLAGS += -Wl,-Ttext -Wl,$(LOADADDR) -s $(LDPARAMS)

EE_BIN = loader.elf
EE_OBJS = loader.o

EE_LIBS =
ifeq ($(DEBUG),1)
EE_LIBS += -ldebug
endif

all: $(EE_BIN)

check-layout: $(EE_BIN)
	@echo "Checking loader.elf layout..."
	@size $(EE_BIN)

clean:
	rm -f -r $(EE_OBJS) $(EE_BIN)

include $(PS2SDK)/Defs.make
include Rules.make
