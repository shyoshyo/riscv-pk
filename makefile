MAKE = make

all: build/bbl build/bbl.asm Bin2Mem ../modelsim/inst_rom.data build/bbl.data build/bbl.bin

RV32 = 1
SPIKE_FLAGS = 

ifndef CROSS_COMPILE
ifeq ($(RV32),1)
	CROSS_COMPILE = riscv32-unknown-linux-gnu
	SPIKE_FLAGS += --isa=RV32IMA
else
	CROSS_COMPILE = riscv64-unknown-linux-gnu
	SPIKE_FLAGS += --isa=RV64IMA
endif
endif

CC = $(CROSS_COMPILE)-as
LD = $(CROSS_COMPILE)-ld
OBJCOPY = $(CROSS_COMPILE)-objcopy
OBJDUMP = $(CROSS_COMPILE)-objdump
GDB = $(CROSS_COMPILE)-gdb

OBJECTS = build/bbl

export	CROSS_COMPILE


build/Makefile:
	cd build && rm -f ./*
	cd build && ../configure --prefix=$$RISCV --host=$(CROSS_COMPILE) --with-payload=../ucore/bin/kernel #--with-payload=../linux/bin/vmlinux

build/bbl: build/Makefile
	cd build && $(MAKE)

clean:
	rm -f ./build/* Bin2Mem ../modelsim/inst_rom.data

%.om: %
	cp $< $@
%.bin: %.om
	$(OBJCOPY) -O binary $<  $@
%.asm: %.om
	$(OBJDUMP) -D $< > $@
%.data: %.bin Bin2Mem
	./Bin2Mem -f $< -o $@
Bin2Mem: Bin2Mem.c
	gcc $< -o $@
../modelsim/inst_rom.data: build/bbl.data
	cp $< $@

.PHONY: build/bbl
