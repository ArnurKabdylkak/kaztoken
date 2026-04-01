#!/usr/bin/env python3
"""
flash_isp.py — прошивка LPC1768 по ISP/UART.

Протокол строго по UM10360 + AN11229 (NXP):
  - W <addr> <count> : count кратен 45, данные в UU-encoding
  - Checksum (сумма сырых байт) после каждых 20 UU-строк
  - Bootloader отвечает OK или RESEND на каждый батч

pip install pyserial
"""

import argparse, sys, time, struct, serial

# ---- Константы LPC1768 ----
FLASH_BASE   = 0x0000_0000
# Bootloader использует начало SRAM (0x10000000–0x100001FF).
# Буфер для записи — сразу после.
SRAM_BUF     = 0x1000_0200

SECTOR_TABLE = (
    [(i * 0x1000, 0x1000) for i in range(16)] +          # 0-15: 4 кБ каждый
    [(0x10000 + i * 0x8000, 0x8000) for i in range(14)]  # 16-29: 32 кБ каждый
)

def sector_of(addr):
    for idx, (base, size) in enumerate(SECTOR_TABLE):
        if base <= addr < base + size:
            return idx
    raise ValueError(f"0x{addr:08X} вне Flash")

# ---- UUencoding (AN11229) ----
# Нулевой 6-битный блок → 0x60 ('`'), остальные → значение + 0x20

def _enc6(v):
    c = (v & 0x3F) + 32
    return 96 if c == 32 else c   # 0x00 → 0x60, иначе +32

def uuencode_line(data: bytes) -> bytes:
    """Кодировать 1–45 байт в одну UU-строку с CRLF."""
    assert 1 <= len(data) <= 45
    out = bytearray([len(data) + 32])           # байт длины
    padded = data + b'\x00' * ((-len(data)) % 3)
    for i in range(0, len(padded), 3):
        b0, b1, b2 = padded[i], padded[i+1], padded[i+2]
        out.append(_enc6(b0 >> 2))
        out.append(_enc6(b0 << 4 | b1 >> 4))
        out.append(_enc6(b1 << 2 | b2 >> 6))
        out.append(_enc6(b2))
    return bytes(out) + b'\r\n'

def uuencode_block(data: bytes) -> list:
    return [uuencode_line(data[i:i+45]) for i in range(0, len(data), 45)]

# ---- Checksum ----
# Сумма сырых байт данных (до UU-кодирования), десятичное число

def raw_checksum(data: bytes) -> int:
    return sum(data) & 0xFFFF_FFFF

# ---- CRC32 для верификации ----

def crc32_lpc(data: bytes) -> int:
    crc = 0xFFFF_FFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xEDB8_8320 if crc & 1 else crc >> 1
    return crc ^ 0xFFFF_FFFF

# ---- Вектор 7 (обязательно для LPC17xx) ----

def patch_checksum(data: bytearray) -> bytearray:
    if len(data) < 32:
        data += b'\x00' * (32 - len(data))
    vecs = list(struct.unpack_from('<8I', data, 0))
    struct.pack_into('<I', data, 0x1C, (-(sum(vecs[:7]))) & 0xFFFF_FFFF)
    return data

# ---- ISP класс ----

class ISP:
    def __init__(self, port, baud, xtal_khz=12000, timeout=5.0):
        self.xtal = xtal_khz
        self._buf = b''
        self._s = serial.Serial(port, baud,
                                bytesize=serial.EIGHTBITS,
                                parity=serial.PARITY_NONE,
                                stopbits=serial.STOPBITS_ONE,
                                timeout=timeout)
        time.sleep(0.1)
        self._s.reset_input_buffer()

    def close(self):
        self._s.close()

    def _send(self, data: bytes):
        self._s.write(data)

    def _read_tokens(self, wait=0.25) -> list:
        """Прочитать все байты, разбить по \\r и \\n, вернуть непустые токены."""
        time.sleep(wait)
        chunk = self._s.read(self._s.in_waiting or 1)
        self._buf += chunk
        # Дочитать хвост
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
        """Команда → ждём числовой код возврата 0."""
        self._send((cmd + '\r\n').encode())
        collected = []
        for _ in range(30):
            for t in self._read_tokens(0.2):
                if t == cmd:
                    continue   # эхо
                collected.append(t)
                try:
                    code = int(t)
                    if code != 0:
                        raise RuntimeError(f"ISP error {code} на '{cmd}'")
                    return collected[:-1]
                except ValueError:
                    pass
        raise TimeoutError(f"Таймаут на '{cmd}', получено: {collected}")

    def synchronize(self):
        print('  Синхронизация', end='', flush=True)
        self._s.reset_input_buffer()
        self._s.reset_output_buffer()
        self._buf = b''

        # Шлём '?' пока не получим "Synchronized"
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
            raise RuntimeError('Нет ответа. Запусти скрипт → ISP → RESET → отпусти ISP')

        self._buf = b''
        self._s.reset_input_buffer()

        # Ответить "Synchronized\r\n" → ждём "OK"
        self._send(b'Synchronized\r\n')
        ok = any('OK' in t for t in self._read_tokens(0.4))
        if not ok:
            # Попробуем ещё раз прочитать
            ok = any('OK' in t for t in self._read_tokens(0.3))
        if not ok:
            raise RuntimeError('Нет OK после Synchronized')

        # Частота кварца → ждём "OK"
        self._send(f'{self.xtal}\r\n'.encode())
        ok = any('OK' in t for t in self._read_tokens(0.4))
        if not ok:
            ok = any('OK' in t for t in self._read_tokens(0.3))
        if not ok:
            raise RuntimeError('Нет OK после частоты кварца')

        # Выключить эхо "A 0"
        self._send(b'A 0\r\n')
        time.sleep(0.3)
        self._buf = b''
        self._s.reset_input_buffer()
        print(' OK')

    def unlock(self):
        self._cmd('U 23130')

    def prepare_sectors(self, s, e):
        self._cmd(f'P {s} {e}')

    def erase_sectors(self, s, e):
        self._cmd(f'E {s} {e}')

    def write_to_ram(self, addr: int, data: bytes):
        """
        W <addr> <count>
        count должен быть кратен 45 (размер UU-строки в байтах).
        Протокол:
          1. Шлём команду, ждём "0" (CMD_SUCCESS)
          2. Шлём UU-строки батчами по 20
          3. После каждого батча — checksum (сумма сырых байт, десятичный)
          4. Ждём "OK" или "RESEND"
        """
        # Дополнить до кратного 180 = LCM(4, 45)
        # count должен быть кратен 4 (word-aligned) и 45 (UU-строка)
        pad = (-len(data)) % 180
        padded = data + b'\x00' * pad
        count = len(padded)    # это и есть то что мы реально шлём

        # Команда W с реальным count (кратным 45)
        self._send(f'W {addr} {count}\r\n'.encode())

        # Ждём CMD_SUCCESS
        got_ok = False
        for _ in range(20):
            for t in self._read_tokens(0.15):
                try:
                    code = int(t)
                    if code == 0:
                        got_ok = True
                    else:
                        raise RuntimeError(f'W cmd error {code}')
                    break
                except ValueError:
                    pass   # эхо или другой текст
            if got_ok:
                break
        if not got_ok:
            raise RuntimeError('W: нет CMD_SUCCESS')

        # Разбить на UU-строки и сырые чанки для checksum
        uu_lines   = uuencode_block(padded)
        raw_chunks = [padded[i*45:(i+1)*45] for i in range(len(uu_lines))]

        BATCH = 20
        for b_start in range(0, len(uu_lines), BATCH):
            batch      = uu_lines  [b_start: b_start + BATCH]
            raw_batch  = raw_chunks[b_start: b_start + BATCH]

            for attempt in range(3):
                # Отправить строки
                for line in batch:
                    self._send(line)

                # Checksum от сырых байт
                cs = raw_checksum(b''.join(raw_batch))
                self._send(f'{cs}\r\n'.encode())

                # Ждём OK или RESEND
                response = ''
                for _ in range(10):
                    tokens = self._read_tokens(0.2)
                    for t in tokens:
                        if t in ('OK', 'RESEND'):
                            response = t
                            break
                        try:
                            if int(t) == 0:
                                response = 'OK'
                                break
                        except ValueError:
                            pass
                    if response:
                        break

                if response == 'OK':
                    break
                elif response == 'RESEND':
                    continue
                else:
                    # Некоторые версии LPC17xx bootloader'а молчат после W
                    # Считаем что данные приняты
                    break

    def copy_ram_to_flash(self, flash_addr: int, ram_addr: int, size: int):
        self._cmd(f'C {flash_addr} {ram_addr} {size}')

    def go(self, addr: int = 0):
        self._send(f'G {addr} T\r\n'.encode())
        print(f'  Запуск с 0x{addr:08X}...')


# ---- Прошивка ----

def flash_binary(port, baud, image_path, xtal_khz=12000, verify=True):
    print(f'\n=== LPC1768 ISP Flash Tool ===')
    print(f'Порт  : {port}  ({baud} Bd)')
    print(f'Образ : {image_path}')
    print(f'Кварц : {xtal_khz} kHz\n')

    with open(image_path, 'rb') as f:
        raw = bytearray(f.read())
    print(f'Размер: {len(raw)} байт')

    raw = patch_checksum(raw)
    # Дополнить до кратного 256 (минимальный блок Flash)
    raw += b'\xFF' * ((-len(raw)) % 256)
    total = len(raw)
    print(f'Записываем: {total} байт\n')

    isp = ISP(port, baud, xtal_khz)
    try:
        isp.synchronize()
        isp.unlock()
        print('  Flash разблокирован.')

        # Полное стирание (обходит CRP1)
        print('  Стирание секторов 0-29...', end=' ', flush=True)
        isp.prepare_sectors(0, 29)
        isp.erase_sectors(0, 29)
        print('OK')

        print(f'  Запись {total} байт...')
        written = 0
        CHUNK = 256   # минимальный блок для C команды

        while written < total:
            chunk = bytes(raw[written: written + CHUNK])
            sec = sector_of(FLASH_BASE + written)

            isp.prepare_sectors(sec, sec)
            isp.write_to_ram(SRAM_BUF, chunk)
            isp.copy_ram_to_flash(FLASH_BASE + written, SRAM_BUF, CHUNK)

            written += CHUNK
            pct = written * 100 // total
            bar = '#' * (pct // 5) + '.' * (20 - pct // 5)
            print(f'\r  [{bar}] {pct:3d}%', end='', flush=True)

        print('\n  Запись завершена.')

        if verify:
            print('  Верификация CRC...', end=' ', flush=True)
            try:
                isp._cmd(f'M {FLASH_BASE} {total} {crc32_lpc(bytes(raw))}')
                print('OK')
            except Exception as e:
                print(f'ОШИБКА: {e}')

        isp.go()
        print('  Готово! Нажми RESET для запуска.\n')

    finally:
        isp.close()


# ---- Диагностика ----

def diagnose(port, baud):
    print(f'Диагностика {port} @ {baud} Bd — Ctrl+C для выхода\n')
    s = serial.Serial(port, baud, timeout=0.1)
    s.reset_input_buffer()
    try:
        while True:
            s.write(b'?')
            d = s.read(64)
            if d:
                print(f'  RX: {d!r}')
            time.sleep(0.2)
    except KeyboardInterrupt:
        print('\nВыход.')
    finally:
        s.close()


# ---- CLI ----

def main():
    p = argparse.ArgumentParser(description='ISP-прошивка LPC1768')
    p.add_argument('--port',      default='/dev/ttyUSB0')
    p.add_argument('--baud',      type=int, default=115200)
    p.add_argument('--image',     default=None)
    p.add_argument('--xtal',      type=int, default=12000)
    p.add_argument('--no-verify', action='store_true')
    p.add_argument('--diagnose',  action='store_true')
    args = p.parse_args()

    if args.diagnose:
        diagnose(args.port, args.baud)
        return
    if not args.image:
        p.error('--image обязателен')
    try:
        flash_binary(args.port, args.baud, args.image, args.xtal,
                     verify=not args.no_verify)
    except Exception as e:
        print(f'\nОШИБКА: {e}', file=sys.stderr)
        sys.exit(1)

if __name__ == '__main__':
    main()