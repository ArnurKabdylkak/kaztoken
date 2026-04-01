# Makefile — STM32F411 Bare-Metal Voltage Glitcher
# Toolchain: arm-none-eabi-gcc

PREFIX  = arm-none-eabi-
CC      = $(PREFIX)gcc
AS      = $(PREFIX)gcc
LD      = $(PREFIX)gcc
OBJCOPY = $(PREFIX)objcopy
OBJDUMP = $(PREFIX)objdump
SIZE    = $(PREFIX)size

TARGET  = glitcher

# Directories
SRC_DIR = src
INC_DIR = inc
BUILD   = build

# Source files
C_SRCS  = $(wildcard $(SRC_DIR)/*.c)
S_SRCS  = $(wildcard $(SRC_DIR)/*.s)
OBJS    = $(patsubst $(SRC_DIR)/%.c,$(BUILD)/%.o,$(C_SRCS)) \
          $(patsubst $(SRC_DIR)/%.s,$(BUILD)/%.o,$(S_SRCS))

# MCU flags
CPU     = -mcpu=cortex-m4 -mthumb -mfloat-abi=soft
DEFS    = -DSTM32F411xE

# Compiler flags
CFLAGS  = $(CPU) $(DEFS) -I$(INC_DIR) \
          -Wall -Wextra -Og -g3 \
          -ffunction-sections -fdata-sections \
          -fno-builtin -nostdlib

ASFLAGS = $(CPU) -g3

# Linker flags
LDSCRIPT = stm32f411.ld
LDFLAGS  = $(CPU) -T$(LDSCRIPT) -nostdlib -nostartfiles \
           -Wl,--gc-sections -Wl,-Map=$(BUILD)/$(TARGET).map

# ---- Rules ----

all: $(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).bin $(BUILD)/$(TARGET).hex
	@$(SIZE) $(BUILD)/$(TARGET).elf

$(BUILD)/$(TARGET).elf: $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $^

$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/$(TARGET).hex: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O ihex $< $@

$(BUILD)/%.o: $(SRC_DIR)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/%.o: $(SRC_DIR)/%.s | $(BUILD)
	$(AS) $(ASFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)

# Flash via ST-Link
flash: $(BUILD)/$(TARGET).bin
	st-flash write $< 0x08000000

# Flash via DFU (Black Pill has USB DFU bootloader)
dfu: $(BUILD)/$(TARGET).bin
	dfu-util -a 0 -s 0x08000000:leave -D $<

# Disassembly
disasm: $(BUILD)/$(TARGET).elf
	$(OBJDUMP) -d -S $< > $(BUILD)/$(TARGET).dis

.PHONY: all clean flash dfu disasm