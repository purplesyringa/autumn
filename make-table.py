import re

with open("interp.c") as f:
    s = f.read()

s = re.sub(r"//.*", "", s)
s = re.sub(r"/\*.*?\*/", "", s)

table = ["0, 0"] * 256
i = 0
handlers = []

for match in re.findall(r"DEF\(([\s\S]*?)\)", s):
    name, *opcodes = [entry.strip() for entry in match.split(",")]
    if name == "name":
        continue
    handlers.append(f"op_{name}")
    for opcode in opcodes:
        if opcode:
            opcode, _, arg = opcode.partition(" = ")
            opcode = int(opcode, 0)
            arg = arg or "0"
            table[opcode] = f"{i}, {arg}"
    i += 1

code = ""
code += "void *handlers[] = {" + ", ".join(handlers) + "};\n"
code += "extern unsigned short opcode_map[];\n"
code += 'asm ("opcode_map: .byte ' + ", ".join(table) + '");\n'

with open("table.i", "w") as f:
    f.write(code)
