# ============================================================
#  Makefile — LPC1768 bare-metal LED sequencer
#  Требует: arm-none-eabi-gcc (пакет arm-none-eabi-gcc / gcc-arm-none-eabi)
#
#  Использование:
#    make          — собрать прошивку
#    make flash    — залить через lpc21isp (ISP/UART)
#    make clean    — удалить артефакты
# ============================================================

# ---------- Инструменты ----------
PREFIX  := arm-none-eabi-
CC      := $(PREFIX)gcc
OBJCOPY := $(PREFIX)objcopy
SIZE    := $(PREFIX)size

# ---------- Исходники ----------
SRCS := startup.c main.c

# ---------- Выходные файлы ----------
TARGET  := firmware
ELF     := $(TARGET).elf
BIN     := $(TARGET).bin
HEX     := $(TARGET).hex
MAP     := $(TARGET).map

# ---------- Флаги компилятора ----------
CPU_FLAGS := \
    -mcpu=cortex-m3 \
    -mthumb

OPT := -O2

CFLAGS := \
    $(CPU_FLAGS)          \
    $(OPT)                \
    -std=c11              \
    -Wall -Wextra         \
    -ffunction-sections   \
    -fdata-sections       \
    -ffreestanding        \
    -nostdlib             \
    -g

# ---------- Флаги компоновщика ----------
LDFLAGS := \
    $(CPU_FLAGS)                     \
    -T linker.ld                     \
    -Wl,--gc-sections                \
    -Wl,-Map=$(MAP)                  \
    -nostdlib                        \
    -nostartfiles

# ---------- Объектные файлы ----------
OBJS := $(SRCS:.c=.o)

# ============================================================
#  Цели
# ============================================================
.PHONY: all flash clean size

all: $(BIN) $(HEX) size

# Компиляция
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Линковка
$(ELF): $(OBJS) linker.ld
	$(CC) $(LDFLAGS) $(OBJS) -o $@

# Бинарный образ (для lpc21isp / pyOCD)
$(BIN): $(ELF)
	$(OBJCOPY) -O binary $< $@

# Intel HEX (для Flash Magic и других утилит)
$(HEX): $(ELF)
	$(OBJCOPY) -O ihex $< $@

# Показать размер секций
size: $(ELF)
	$(SIZE) $<

# ---------- Прошивка через ISP (lpc21isp) ----------
# Настройте UART_PORT и BAUD под вашу систему.
# Fedora: dnf install lpc21isp  ИЛИ собрать из исходников.
UART_PORT ?= /dev/ttyUSB0
BAUD      ?= 115200
XTAL_KHZ  ?= 12000        # частота кварца на плате в кГц

flash: $(HEX)
	lpc21isp -verify $(HEX) $(UART_PORT) $(BAUD) $(XTAL_KHZ)

# ---------- Прошивка через Python ISP скрипт (см. flash_isp.py) ----------
pyflash: $(BIN)
	python3 flash_isp.py --port $(UART_PORT) --baud $(BAUD) --image $(BIN)

clean:
	rm -f $(OBJS) $(ELF) $(BIN) $(HEX) $(MAP)