#!/usr/bin/env python3
"""
capture_bootrom.py — принимает hex-дамп Boot ROM с UART и сохраняет в .bin

Использование:
  python3 capture_bootrom.py --port /dev/ttyUSB0 --out bootrom.bin
"""
import argparse, sys, time, serial

def capture(port, baud, out_path):
    print(f'Открываю {port} @ {baud}...')
    s = serial.Serial(port, baud, timeout=30)
    time.sleep(0.5)
    s.reset_input_buffer()

    data = bytearray(0x2000)
    received = 0
    started = False

    print('Жду BOOTROM_DUMP_START...  (нажми RESET на плате)')
    while True:
        line = s.readline().decode(errors='replace').strip()
        if not line:
            continue
        if 'BOOTROM_DUMP_START' in line:
            print('Получен старт. Читаю...')
            started = True
            continue
        if not started:
            continue
        if 'BOOTROM_DUMP_END' in line:
            print(f'\nГотово! Получено {received} байт.')
            break

        # Формат: "1FFF0000: XX XX XX ... |ascii|"
        if ':' not in line:
            continue
        try:
            addr_str, rest = line.split(':', 1)
            addr = int(addr_str.strip(), 16)
            offset = addr - 0x1FFF0000
            if offset < 0 or offset >= 0x2000:
                continue
            hex_part = rest.split('|')[0].strip()
            bytes_ = [int(h, 16) for h in hex_part.split() if h]
            for i, b in enumerate(bytes_):
                if offset + i < 0x2000:
                    data[offset + i] = b
                    received += 1
            pct = received * 100 // 0x2000
            print(f'\r  [{"#"*(pct//5):<20}] {pct:3d}%  {received}/8192', end='', flush=True)
        except Exception:
            pass

    s.close()

    with open(out_path, 'wb') as f:
        f.write(data)
    print(f'Сохранено в {out_path}')

    # Анализ
    print('\n=== Строки в Boot ROM ===')
    cur = ''
    for b in data:
        if 32 <= b < 127:
            cur += chr(b)
        else:
            if len(cur) >= 6:
                print(f'  "{cur}"')
            cur = ''

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--port', default='/dev/ttyUSB0')
    p.add_argument('--baud', type=int, default=9600)
    p.add_argument('--out',  default='bootrom.bin')
    args = p.parse_args()
    try:
        capture(args.port, args.baud, args.out)
    except Exception as e:
        print(f'\nОШИБКА: {e}', file=sys.stderr)
        sys.exit(1)

if __name__ == '__main__':
    main()