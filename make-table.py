import re

with open("interp.c") as f:
    s = f.read()

s = re.sub(r"//.*", "", s)
s = re.sub(r"/\*.*?\*/", "", s)

table = [0] * 256
i = 0
handlers = []

for match in re.findall(r"DEF\(([\s\S]*?)\)", s):
    name, *opcodes = [entry.strip() for entry in match.split(",")]
    if name == "name":
        continue
    handlers.append(f"op_{name}")
    opcodes = [int(opcode, 16) for opcode in opcodes if opcode]
    for opcode in opcodes:
        table[opcode] = i
    i += 1

code = ""
code += "void *handlers[] = {" + ", ".join(handlers) + "};\n"
code += "unsigned char opcode_map[256] = {" + ", ".join(map(str, table)) + "};\n"

with open("table.i", "w") as f:
    f.write(code)
