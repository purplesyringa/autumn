import re
import struct

with open("interp.c") as f:
    s = f.read()

s = re.sub(r"//.*", "", s)
s = re.sub(r"/\*.*?\*/", "", s)

table = ["0, 0"] * 256
handler_index = 0
handlers = []

for match in re.findall(r"DEF\(([\s\S]*?)\)", s):
    name, *opcodes = [entry.strip() for entry in match.split(",")]
    if name == "name":
        continue
    handlers.append(f"op_{name} - base_sym")
    for opcode in opcodes:
        if opcode:
            opcode, _, arg = opcode.partition(" = ")
            opcode = int(opcode, 0)
            arg = arg or "0"
            table[opcode] = f"{handler_index}, {arg}"
    handler_index += 1

def crc32_u32(a: int, b: int) -> int:
    x = a ^ b
    for _ in range(32):
        if x & 1:
            x ^= 0x105ec76f1
        x >>= 1
    return x

def crc32_u64(a: int, b: int) -> int:
    a = crc32_u32(a, b & ((1 << 32) - 1))
    a = crc32_u32(a, b >> 32)
    return a

imports = []
crcs = set()
for name in re.findall(r"DEF_IMPORT\(([\s\S]*?)\)", s):
    if name == "name":
        continue
    prefix = struct.unpack("<Q", name.encode()[:8].rjust(8, b"\x00"))[0]
    name_crc = crc32_u64(len(name), prefix) & 0xffff
    assert name_crc not in crcs, f"name collision for {name}"
    crcs.add(name_crc)
    imports.append(str(name_crc))
    imports.append(f"{name} - base_sym")

code = ""

code += "extern unsigned short handlers[];\n"
code += "extern unsigned short opcode_map[];\n"
code += "extern unsigned short imports[];\n"
code += "extern unsigned short imports_end[];\n"

code += 'asm ("'
code += "base_sym: "
code += ".pushsection .rodata.tables; "
code += "handlers: .short " + ", ".join(handlers) + "; "
code += "opcode_map: .byte " + ", ".join(table) + "; "
code += "imports: .short " + ", ".join(imports) + "; imports_end: "
code += ".popsection"
code += '");\n'

with open("table.i", "w") as f:
    f.write(code)
