#!/usr/bin/env python3
"""
read_flash.py — чтение Flash/SRAM/регистров LPC1768 через ISP/UART.

Использование:
  python read_flash.py --port /dev/ttyUSB0 vectors
  python read_flash.py --port /dev/ttyUSB0 regs
  python read_flash.py --port /dev/ttyUSB0 dump --addr 0x0 --size 756 --out readback.bin
  python read_flash.py --port /dev/ttyUSB0 dump --addr 0x0 --size 756 --out readback.bin --elf
  python read_flash.py --port /dev/ttyUSB0 reg --addr 0x400FC0C4

  # Boot ROM через ремап (в ISP-режиме Boot ROM маппируется на 0x0):
  python read_flash.py --port /dev/ttyUSB0 dump --addr 0x0 --size 0x2000 --out bootrom.elf --elf --load-addr 0x1FFF0000 --entry 0x1FFF0081

pip install pyserial
"""

import argparse, struct, sys, time
import serial

# ───────────────────────── Карта памяти LPC1768 ─────────────────────────
# Доступно через ISP R-команду:
FLASH_BASE   = 0x0000_0000   # User Flash,    512 KB
FLASH_SIZE   = 0x0008_0000

LOCAL_SRAM   = 0x1000_0000   # Local SRAM,     32 KB (CPU)
LOCAL_SRAM_SIZE = 0x0000_8000

BOOTROM_BASE = 0x1FFF_0000   # Boot ROM,        8 KB  (0x1FFF0000–0x1FFF1FFF)
BOOTROM_SIZE = 0x0000_2000

AHB_SRAM0    = 0x2007_C000   # AHB SRAM  bank0, 16 KB (0x2007C000–0x2007FFFF)
AHB_SRAM0_SIZE = 0x0000_4000

AHB_SRAM1    = 0x2008_0000   # AHB SRAM  bank1, 16 KB (0x20080000–0x20083FFF), LPC1768/6/5
AHB_SRAM1_SIZE = 0x0000_4000

# Недоступно через ISP R (ADDR_NOT_MAPPED, ошибка 14):
#   GPIO Fast I/O:  0x2009_C000  (FIO — AHB периферия)
#   APB0 периф.:   0x4000_0000  (WDT, TIM0/1, UART0/1, PWM1, SPI, RTC, GPIO int, PINSEL, SSP1, ADC, CAN...)
#   APB1 периф.:   0x4008_0000  (SSP0, DAC, TIM2/3, UART2/3, I2C2, UART3, I2S, QEI, MCpwm, SYSCON...)
#   AHB периф.:    0x5000_0000  (Ethernet, GPDMA, USB)
#   SCB/NVIC:      0xE000_0000

# ───────────────────────── UU-кодек ─────────────────────────

def _enc6(v):
    c = (v & 0x3F) + 32
    return 96 if c == 32 else c

def uuencode_line(data: bytes) -> bytes:
    assert 1 <= len(data) <= 45
    out = bytearray([len(data) + 32])
    padded = data + b'\x00' * ((-len(data)) % 3)
    for i in range(0, len(padded), 3):
        b0, b1, b2 = padded[i], padded[i+1], padded[i+2]
        out.append(_enc6(b0 >> 2))
        out.append(_enc6(b0 << 4 | b1 >> 4))
        out.append(_enc6(b1 << 2 | b2 >> 6))
        out.append(_enc6(b2))
    return bytes(out) + b'\r\n'

def uudecode_line(line: str) -> bytes:
    """Декодировать одну UU-строку обратно в байты."""
    if not line:
        return b''
    def dec6(c):
        return (ord(c) - 32) & 0x3F
    length = dec6(line[0])
    if length == 0:
        return b''
    out = bytearray()
    i = 1
    while i + 3 <= len(line):
        a = dec6(line[i])
        b = dec6(line[i+1])
        c = dec6(line[i+2])
        d = dec6(line[i+3])
        out.append((a << 2) | (b >> 4))
        out.append(((b & 0x0F) << 4) | (c >> 2))
        out.append(((c & 0x03) << 6) | d)
        i += 4
    return bytes(out[:length])

def is_uu_line(s: str) -> bool:
    """Проверить, похоже ли на UU-строку (первый символ — байт длины 33..77, длина >= 5)."""
    # Минимальная UU-строка: 1 байт данных → '!' + 4 символа = 5 символов.
    # Исключаем короткие токены типа "0", "OK", числа ответа.
    if not s or len(s) < 5:
        return False
    return 33 <= ord(s[0]) <= 77

# ───────────────────────── ISP класс ─────────────────────────

class ISP:
    def __init__(self, port: str, baud: int = 115200, xtal_khz: int = 12000, timeout: float = 5.0):
        self.xtal = xtal_khz
        self._buf = b''
        self._s = serial.Serial(
            port, baud,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE,
            timeout=timeout,
        )
        time.sleep(0.1)
        self._s.reset_input_buffer()

    def close(self):
        self._s.close()

    def _send(self, data: bytes):
        self._s.write(data)

    def _read_tokens(self, wait: float = 0.25) -> list:
        time.sleep(wait)
        chunk = self._s.read(self._s.in_waiting or 1)
        self._buf += chunk
        while True:
            time.sleep(0.05)
            n = self._s.in_waiting
            if not n:
                break
            self._buf += self._s.read(n)
        parts = self._buf.replace(b'\r\n', b'\n').replace(b'\r', b'\n').split(b'\n')
        self._buf = parts[-1]
        return [p.decode(errors='replace').strip() for p in parts[:-1] if p.strip()]

    def _cmd(self, cmd: str) -> list:
        self._send((cmd + '\r\n').encode())
        collected = []
        for _ in range(30):
            for t in self._read_tokens(0.2):
                if t == cmd:
                    continue
                collected.append(t)
                try:
                    code = int(t)
                    if code != 0:
                        raise RuntimeError(f'ISP error {code} на «{cmd}»')
                    return collected[:-1]
                except ValueError:
                    pass
        raise TimeoutError(f'Таймаут на «{cmd}», получено: {collected}')

    def synchronize(self):
        print('  Синхронизация', end='', flush=True)
        self._s.reset_input_buffer()
        self._s.reset_output_buffer()
        self._buf = b''
        found = False
        for _ in range(100):
            self._send(b'?')
            time.sleep(0.1)
            print('.', end='', flush=True)
            chunk = self._s.read(self._s.in_waiting or 1)
            self._buf += chunk
            if b'Synchronized' in self._buf:
                found = True
                time.sleep(0.15)
                self._buf += self._s.read(self._s.in_waiting)
                break
        if not found:
            raise RuntimeError('Нет ответа. Удержи ISP → RESET → отпусти ISP')
        self._buf = b''
        self._s.reset_input_buffer()
        self._send(b'Synchronized\r\n')
        ok = any('OK' in t for t in self._read_tokens(0.4))
        if not ok:
            ok = any('OK' in t for t in self._read_tokens(0.3))
        if not ok:
            raise RuntimeError('Нет OK после Synchronized')
        self._send(f'{self.xtal}\r\n'.encode())
        ok = any('OK' in t for t in self._read_tokens(0.4))
        if not ok:
            ok = any('OK' in t for t in self._read_tokens(0.3))
        if not ok:
            raise RuntimeError('Нет OK после частоты кварца')
        self._send(b'A 0\r\n')
        time.sleep(0.3)
        self._buf = b''
        self._s.reset_input_buffer()
        print(' OK')

    def unlock(self):
        self._cmd('U 23130')

    # ───── ЧТЕНИЕ ПАМЯТИ ─────

    def read_memory(self, addr: int, count: int) -> bytes:
        """
        R <addr> <count>
        Читает count байт из памяти LPC1768.
        Работает для Flash (0x0), SRAM (0x10000000), Boot ROM (0x1FFF0000).
        count выравнивается до 4 байт.

        Протокол R (аналогичен W, но наоборот):
          Bootloader → "0\\r\\n"
          Bootloader → [до 20 UU-строк]
          Bootloader → "<checksum>\\r\\n"  (сумма сырых байт данных)
          Host       → "OK\\r\\n"          (подтверждение батча)
          (повтор для следующих батчей если count > 900 байт)
        """
        count = (count + 3) & ~3  # word-align

        self._send(f'R {addr} {count}\r\n'.encode())

        # Один общий цикл: "0" и UU-строки и checksum приходят одним burst-ом.
        # Разбивать на два отдельных цикла нельзя — первый поглощает UU-данные.
        #
        # Протокол батча (до 20 UU-строк):
        #   Boot ← "0"          CMD_SUCCESS (только перед первым батчем)
        #   Boot ← <UU line>×N
        #   Boot ← <checksum>   целое число = сумма сырых байт
        #   Host → "OK\r\n"     подтверждение; без него бутлоадер зависает

        BATCH    = 20
        expected = (count + 44) // 45
        result   = bytearray()
        received = 0
        got_ok   = False

        while received < expected:
            want         = min(BATCH, expected - received)
            batch_lines: list[str] = []
            got_checksum = False

            for _ in range((want + 5) * 10):
                for t in self._read_tokens(0.15):
                    # До получения CMD_SUCCESS "0" — только его и ищем
                    if not got_ok:
                        try:
                            code = int(t)
                            if code == 0:
                                got_ok = True
                            else:
                                raise RuntimeError(
                                    f'R error {code} (addr=0x{addr:08X})')
                        except ValueError:
                            pass  # эхо или мусор до "0"
                        continue   # не переходим к сбору UU пока не OK

                    # После "0": сначала UU-строки, затем checksum
                    if is_uu_line(t) and len(batch_lines) < want:
                        batch_lines.append(t)
                    elif len(batch_lines) >= want and not got_checksum:
                        try:
                            int(t)   # checksum — любое целое
                            got_checksum = True
                        except ValueError:
                            pass

                if got_ok and len(batch_lines) >= want and got_checksum:
                    break

            if not got_ok:
                raise RuntimeError(f'R: нет CMD_SUCCESS (addr=0x{addr:08X})')
            if len(batch_lines) < want:
                raise RuntimeError(
                    f'R: батч: получено {len(batch_lines)} UU-строк, '
                    f'ожидалось {want} (addr=0x{addr:08X})')

            for line in batch_lines:
                result += uudecode_line(line)
            received += len(batch_lines)

            # Подтверждаем батч — без этого бутлоадер зависает на следующем чтении
            self._send(b'OK\r\n')
            time.sleep(0.05)  # дать бутлоадеру время принять OK

        return bytes(result[:count])

# ───────────────────────── Инструменты анализа ─────────────────────────

def read_reg32(isp: ISP, addr: int) -> int:
    """Читает один 32-битный регистр по адресу."""
    data = isp.read_memory(addr, 4)
    return struct.unpack_from('<I', data)[0]

def print_reg(isp: ISP, addr: int, name: str, fields: list | None = None):
    """
    Читает регистр, печатает hex/bin и именованные битовые поля.
    fields = [('имя', msb, lsb), ...]
    """
    val = read_reg32(isp, addr)
    print(f'\n{name} (0x{addr:08X}) = 0x{val:08X}')
    print(f'  bin: {val:032b}')
    if fields:
        for fname, msb, lsb in fields:
            mask = (1 << (msb - lsb + 1)) - 1
            field_val = (val >> lsb) & mask
            bits = f'[{msb}:{lsb}]' if msb != lsb else f'[{lsb}]'
            print(f'  {bits:8s}  {fname} = {field_val} (0x{field_val:X})')
    return val

# ───────────────────────── Команды ─────────────────────────

def cmd_vectors(isp: ISP):
    """Дамп таблицы векторов прерываний из Flash (0x00000000, 8 слов)."""
    print('\n=== Таблица векторов прерываний ===')
    data = isp.read_memory(0x00000000, 32)
    vecs = struct.unpack_from('<8I', data)
    labels = [
        'Initial SP',
        'Reset_Handler',
        'NMI_Handler',
        'HardFault_Handler',
        'MemManage_Handler',
        'BusFault_Handler',
        'UsageFault_Handler',
        'Checksum (vec7)',
    ]
    for i, (v, label) in enumerate(zip(vecs, labels)):
        print(f'  [{i}] 0x{v:08X}  {label}')

    # Верификация контрольной суммы (обязательно для LPC17xx)
    expected = (-(sum(vecs[:7]))) & 0xFFFF_FFFF
    if vecs[7] == expected:
        print('  Checksum: OK')
    else:
        print(f'  Checksum: MISMATCH (ожидался 0x{expected:08X})')

def _hexdump(data: bytes, base_addr: int, rows: int = 0):
    """Hex-дамп данных. rows=0 — все строки."""
    total = len(data)
    limit = rows * 16 if rows else total
    for i in range(0, min(total, limit), 16):
        row = data[i:i+16]
        hex_part = ' '.join(f'{b:02X}' for b in row)
        asc_part = ''.join(chr(b) if 32 <= b < 127 else '.' for b in row)
        print(f'  0x{base_addr+i:08X}  {hex_part:<48}  {asc_part}')
    if rows and total > limit:
        print(f'  ... ({total - limit} байт скрыто)')

def cmd_regs(isp: ISP):
    """
    Дамп регионов памяти LPC1768, доступных через ISP R-команду.

    Доступно через ISP R:
      User Flash  0x0000_0000 – 0x0007_FFFF  (512 KB) ✓
      Local SRAM  0x1000_0000 – 0x1000_7FFF   (32 KB) ✓

    Недоступно (ошибка 14 ADDR_NOT_MAPPED):
      Boot ROM    0x1FFF_0000 – 0x1FFF_1FFF    (8 KB)  — ISP закрывает
      AHB SRAM0   0x2007_C000 – 0x2007_FFFF   (16 KB)  — проверить
      AHB SRAM1   0x2008_0000 – 0x2008_3FFF   (16 KB)  — проверить
      GPIO FIO    0x2009_C000+
      APB0        0x4000_0000+  (UART, SPI, I2C, TIM, PWM, ADC, CAN, PINSEL...)
      APB1        0x4008_0000+  (SSP0, DAC, TIM2/3, UART2/3, I2S, SYSCON...)
      AHB         0x5000_0000+  (Ethernet, GPDMA, USB)
    """

    # ── 1. Векторная таблица User Flash ──
    print('\n=== Векторная таблица User Flash (0x00000000) ===')
    fvecs_raw = isp.read_memory(FLASH_BASE, 32)
    fvecs = struct.unpack_from('<8I', fvecs_raw)
    labels = ['Initial SP', 'Reset_Handler', 'NMI', 'HardFault',
              'MemManage',  'BusFault',  'UsageFault', 'Checksum']
    for i, (v, lbl) in enumerate(zip(fvecs, labels)):
        print(f'  [{i}] 0x{v:08X}  {lbl}')
    expected_cs = (-(sum(fvecs[:7]))) & 0xFFFF_FFFF
    cs_ok = fvecs[7] == expected_cs
    print(f'  Checksum: {"OK" if cs_ok else f"MISMATCH (ожидался 0x{expected_cs:08X})"}')
    sp = fvecs[0]
    sp_ok = LOCAL_SRAM <= sp <= LOCAL_SRAM + LOCAL_SRAM_SIZE
    print(f'  Initial SP 0x{sp:08X}: {"в Local SRAM — OK" if sp_ok else "не в Local SRAM"}')
    rh = fvecs[1]
    rh_ok = FLASH_BASE <= (rh & ~1) < FLASH_BASE + FLASH_SIZE
    print(f'  Reset_Handler 0x{rh:08X}: {"в User Flash — OK" if rh_ok else "не в User Flash (не прошито?)"}')

    # ── 2. Код стартапа (Flash сразу после векторов) ──
    print('\n=== Flash: стартап-код 0x20–0x5F ===')
    _hexdump(isp.read_memory(FLASH_BASE + 0x20, 64), FLASH_BASE + 0x20)

    # ── 3. Local SRAM (первые 64 байта — буфер ISP) ──
    print(f'\n=== Local SRAM 0x{LOCAL_SRAM:08X} (первые 64 байта) ===')
    _hexdump(isp.read_memory(LOCAL_SRAM, 64), LOCAL_SRAM)

    def _try_read(label: str, addr: int, size: int):
        print(f'\n=== {label} 0x{addr:08X} (первые {size} байт) ===')
        try:
            _hexdump(isp.read_memory(addr, size), addr)
        except RuntimeError as e:
            print(f'  [недоступно] {e}')

    # ── 4. Boot ROM (ISP намеренно закрывает его от чтения) ──
    _try_read(f'Boot ROM', BOOTROM_BASE, 32)

    # ── 5. AHB SRAM bank0 ──
    _try_read(f'AHB SRAM0', AHB_SRAM0, 32)

    # ── 6. AHB SRAM bank1 (LPC1768/6/5) ──
    _try_read(f'AHB SRAM1', AHB_SRAM1, 32)

    print('\n=== Итог доступности регионов ===')
    regions = [
        ('User Flash',  FLASH_BASE,   FLASH_SIZE),
        ('Local SRAM',  LOCAL_SRAM,   LOCAL_SRAM_SIZE),
        ('Boot ROM',    BOOTROM_BASE, BOOTROM_SIZE),
        ('AHB SRAM0',   AHB_SRAM0,    AHB_SRAM0_SIZE),
        ('AHB SRAM1',   AHB_SRAM1,    AHB_SRAM1_SIZE),
    ]
    for name, addr, size in regions:
        try:
            isp.read_memory(addr, 4)
            print(f'  [OK ] 0x{addr:08X}  {name}')
        except RuntimeError as e:
            code = str(e).split('error')[-1].split('(')[0].strip() if 'error' in str(e) else '?'
            print(f'  [E{code:>2}] 0x{addr:08X}  {name}')

    print('\n[!] APB/AHB периферия (GPIO, UART, Clock) недоступна через ISP R.')
    print('    Для их чтения нужен SWD/JTAG или вывод по UART из прошивки.')

def cmd_dump(isp: ISP, addr: int, size: int, out_file: str | None,
             as_elf: bool = False, load_addr: int | None = None,
             entry: int | None = None):
    """
    Читает диапазон памяти, выводит hex-дамп и сохраняет в файл.

    as_elf=True  — сохранить как ELF32 ARM (для Ghidra/IDA/objdump)
    load_addr    — VMA сегмента в ELF (по умолчанию = addr чтения)
    entry        — точка входа в ELF (по умолчанию = load_addr | 1 для Thumb)
    """
    lma   = load_addr if load_addr is not None else addr
    ep    = entry     if entry     is not None else (lma | 1)

    print(f'\nЧтение 0x{addr:08X}..0x{addr+size-1:08X} ({size} байт)')
    if as_elf:
        print(f'  ELF: load=0x{lma:08X}  entry=0x{ep:08X}')
    CHUNK = 256
    result = bytearray()

    for offset in range(0, size, CHUNK):
        chunk_size = min(CHUNK, size - offset)
        chunk = isp.read_memory(addr + offset, chunk_size)
        result += chunk
        pct = (offset + chunk_size) * 100 // size
        bar = '#' * (pct // 5) + '.' * (20 - pct // 5)
        print(f'\r  [{bar}] {pct:3d}%', end='', flush=True)

    print()
    print()
    for i in range(0, len(result), 16):
        row = result[i:i+16]
        hex_part = ' '.join(f'{b:02X}' for b in row)
        asc_part = ''.join(chr(b) if 32 <= b < 127 else '.' for b in row)
        print(f'  0x{addr+i:08X}  {hex_part:<48}  {asc_part}')

    if out_file:
        raw = bytes(result)
        payload = make_elf(raw, lma, ep) if as_elf else raw
        with open(out_file, 'wb') as f:
            f.write(payload)
        kind = 'ELF' if as_elf else 'binary'
        print(f'\n  Сохранено ({kind}): {out_file}')

    return bytes(result)

def make_elf(data: bytes, load_addr: int, entry: int) -> bytes:
    """
    Создаёт минимальный ELF32 (ARM Cortex-M3, little-endian) из бинарных данных.

    Структура файла:
      [0x00] ELF header   52 байта
      [0x34] Program hdr  32 байта   PT_LOAD
      [0x54] <data>        сырые байты

    load_addr — VMA/LMA сегмента (куда грузить в память)
    entry     — точка входа; для Thumb = адрес | 1 (напр. 0x1FFF0081)
    """
    EI_CLASS32  = 1
    EI_DATA2LSB = 1   # little-endian
    EM_ARM      = 40
    ET_EXEC     = 2
    EV_CURRENT  = 1
    PT_LOAD     = 1
    PF_R_X      = 0x5   # Read + Execute
    EF_ARM      = 0x05000200  # EABI v5

    ehdr_size = 52
    phdr_size = 32
    data_off  = ehdr_size + phdr_size  # 0x54 = 84

    # e_ident[16]
    e_ident = (b'\x7fELF'
               + bytes([EI_CLASS32, EI_DATA2LSB, EV_CURRENT, 0])
               + b'\x00' * 8)  # padding

    # ELF header: e_type..e_shstrndx  (36 байт, итого с e_ident = 52)
    ehdr = e_ident + struct.pack('<HHIIIIIHHHHHH',
        ET_EXEC,    # e_type
        EM_ARM,     # e_machine
        EV_CURRENT, # e_version
        entry,      # e_entry
        ehdr_size,  # e_phoff
        0,          # e_shoff  (секций нет)
        EF_ARM,     # e_flags
        ehdr_size,  # e_ehsize
        phdr_size,  # e_phentsize
        1,          # e_phnum
        40,         # e_shentsize
        0,          # e_shnum
        0,          # e_shstrndx
    )
    assert len(ehdr) == ehdr_size

    # Program header (32 байта): p_type..p_align
    phdr = struct.pack('<IIIIIIII',
        PT_LOAD,        # p_type
        data_off,       # p_offset  (данные в файле)
        load_addr,      # p_vaddr
        load_addr,      # p_paddr
        len(data),      # p_filesz
        len(data),      # p_memsz
        PF_R_X,         # p_flags
        4,              # p_align
    )
    assert len(phdr) == phdr_size

    return ehdr + phdr + data

def cmd_reg(isp: ISP, addr: int):
    """Читает один регистр и печатает все биты."""
    val = read_reg32(isp, addr)
    print(f'\n0x{addr:08X} = 0x{val:08X}  ({val})')
    print(f'  binary: {val:032b}')
    print(f'  bytes:  {val & 0xFF:#04x}  {(val>>8) & 0xFF:#04x}  {(val>>16) & 0xFF:#04x}  {(val>>24) & 0xFF:#04x}')
    print()
    for bit in range(31, -1, -1):
        if (val >> bit) & 1:
            print(f'  бит {bit:2d} [1 << {bit:2d}] установлен')

# ───────────────────────── main ─────────────────────────

def main():
    p = argparse.ArgumentParser(description='Чтение Flash/SRAM/регистров LPC1768 через ISP')
    p.add_argument('--port',  default='/dev/ttyUSB0', help='Серийный порт')
    p.add_argument('--baud',  type=int, default=115200)
    p.add_argument('--xtal',  type=int, default=12000, help='Частота кварца, кГц')
    sub = p.add_subparsers(dest='cmd', required=True)

    sub.add_parser('vectors', help='Таблица векторов прерываний (0x0, 32 байта)')
    sub.add_parser('regs',    help='Основные регистры GPIO/UART/Clock')

    dp = sub.add_parser('dump', help='Дамп произвольного диапазона памяти')
    dp.add_argument('--addr',      type=lambda x: int(x,0), default=0x0,   help='Начальный адрес чтения (hex)')
    dp.add_argument('--size',      type=lambda x: int(x,0), default=0x100, help='Размер в байтах (hex/dec)')
    dp.add_argument('--out',       default=None, help='Сохранить в файл (.bin или .elf)')
    dp.add_argument('--elf',       action='store_true', help='Сохранить как ELF32 ARM')
    dp.add_argument('--load-addr', type=lambda x: int(x,0), default=None,
                    help='VMA сегмента в ELF (по умолчанию = --addr)')
    dp.add_argument('--entry',     type=lambda x: int(x,0), default=None,
                    help='Точка входа в ELF (по умолчанию = load-addr|1)')

    rp = sub.add_parser('reg', help='Один 32-битный регистр')
    rp.add_argument('--addr', type=lambda x: int(x,0), required=True, help='Адрес регистра (hex)')

    args = p.parse_args()

    isp = ISP(args.port, args.baud, args.xtal)
    try:
        isp.synchronize()
        isp.unlock()
        print('  Flash разблокирован.\n')

        if args.cmd == 'vectors':
            cmd_vectors(isp)
        elif args.cmd == 'regs':
            cmd_regs(isp)
        elif args.cmd == 'dump':
            cmd_dump(isp, args.addr, args.size, args.out,
                     as_elf=args.elf,
                     load_addr=args.load_addr,
                     entry=args.entry)
        elif args.cmd == 'reg':
            cmd_reg(isp, args.addr)

    except Exception as e:
        print(f'\nОШИБКА: {e}', file=sys.stderr)
        sys.exit(1)
    finally:
        isp.close()

if __name__ == '__main__':
    main()
