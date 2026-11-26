# The wup-homebrew kernel: build/kernel.bin, started by the loader (wup-menu boot/loader.c) in place of Nintendo's
# firmware. Put it on a pad with wup-menu's tools (mkflash.py --system / gamepad_flash.py install-cfw --system).
LLVM    ?= $(firstword $(wildcard /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin /usr/lib/llvm-18/bin) /usr/bin)
LLD     ?= $(firstword $(wildcard /opt/homebrew/opt/lld/bin/ld.lld $(LLVM)/ld.lld) ld.lld)
CC_PAD   = $(LLVM)/clang
OBJCOPY  = $(LLVM)/llvm-objcopy
B        = build
TINF     = ../libwup/third_party/tinf/src
CFLAGS   = --target=armv5te-none-eabi -mcpu=arm926ej-s -marm -mfloat-abi=soft -O2 -g -ffreestanding -fno-common \
           -ffunction-sections -fdata-sections -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
           -std=gnu11 -Isrc -Isrc/compat -I$(TINF)
SRC      = $(wildcard src/*.c) $(TINF)/tinflate.c $(TINF)/tinfzlib.c
OBJ      = $(B)/start.o $(patsubst %.c,$(B)/%.o,$(notdir $(SRC)))
vpath %.c src $(TINF)

all: $(B)/kernel.bin
$(B)/%.o: %.c src/*.h ../libwup/include/wupk.h | $(B)
	$(CC_PAD) $(CFLAGS) -c $< -o $@
$(B)/start.o: src/start.S | $(B)
	$(CC_PAD) $(CFLAGS) -c $< -o $@
$(B)/kernel.elf: $(OBJ) src/link.ld
	$(LLD) -T src/link.ld --gc-sections -o $@ $(OBJ)
$(B)/kernel.bin: $(B)/kernel.elf
	$(OBJCOPY) -O binary $< $@
	@python3 -c "import os; print('kernel.bin: %d KB' % (os.path.getsize('$@') >> 10))"
$(B):
	mkdir -p $@
clean:
	rm -rf $(B)
.PHONY: all clean
