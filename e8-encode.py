import sys
import struct

BASE = 0x2401000
code = bytearray(sys.stdin.buffer.read())

pos = code.find(0xe8, 21) + 1

start = BASE + pos
count = 0

while True:
    offset, = struct.unpack("<L", code[pos:pos + 4])
    addr = (offset + BASE + pos) % (2 ** 32)
    code[pos:pos + 4] = struct.pack("<L", addr)
    next_pos = code.find(0xe8, pos + 4) + 1
    if next_pos == 0:
        break
    count += next_pos - (pos + 4)
    pos = next_pos

start_index = code.find(b"\x78\x56\x34\x12")
count_index = code.find(b"\x78\x56\x34\x12", start_index + 1)
code[start_index:start_index + 4] = struct.pack("<L", start)
code[count_index:count_index + 4] = struct.pack("<L", count)

sys.stdout.buffer.write(code)
