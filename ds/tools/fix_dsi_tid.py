# Patches the DSi title ID LOW into an .nds header (offset 0x230, 4 bytes
# little-endian) and recomputes the header CRC16 at 0x15E.
#
# ndstool derives the DSi TID from the game code and its `-f` header fix
# ZEROES the DSi TID fields, so both jobs are done here instead:
#   python fix_dsi_tid.py app.nds 00F9A003
#
# With --bump it also advances the counter file by one so every build gets
# a fresh TID (FBI announces reinstalls as new software):
#   python fix_dsi_tid.py --bump tid_counter
import sys

def crc16(data):
    # NDS header CRC16 (GBATEK): reflected poly 0x8005 -> 0xA001,
    # init 0xFFFF, LSB-first, over bytes 0x000..0x15D.
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc

def main():
    if sys.argv[1] == '--bump':
        path = sys.argv[2]
        with open(path, 'r+') as f:
            cur = int(f.read().strip(), 16)
            nxt = cur + 1
            f.seek(0)
            f.write(f"{nxt:08X}")
            f.truncate()
        print(f"{path}: 0x{cur:08X} -> 0x{nxt:08X}")
        return
    path = sys.argv[1]
    tid_hex = sys.argv[2]
    tid = int(tid_hex, 16).to_bytes(4, 'little')
    with open(path, 'r+b') as f:
        b = bytearray(f.read())
        b[0x230:0x234] = tid
        crc = crc16(b[:0x15E])
        b[0x15E] = crc & 0xFF
        b[0x15F] = (crc >> 8) & 0xFF
        f.seek(0)
        f.write(b)
    print(f"{path}: TID low -> 0x{tid_hex}  header CRC16 -> 0x{crc:04X}")

if __name__ == '__main__':
    main()
