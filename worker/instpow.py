file_path = "./include/instpow.h"

base   = int(input("Base: "))
maxPow = input("MaxPow: ")

pows = ",\n".join([str(base ** i)  for i in range(int(maxPow)) if base ** i <= 2 ** 64]) + '};\n'
print(len(pows.split(',\n')))

func = f"""
\n\nint maxPow = {len(pows.split(',\n'))};

uint64_t instantPow(int pow) {{
    if (pow > maxPow) {{
        return -1;
    }}

    return pows[pow];
}}
"""

with open(file_path, "w") as file:
    file.write("#ifndef INSTPOW_H\n#define INSTPOW_H\n#include <stdint.h>\n\n")
    file.write("uint64_t instantPow(int pow);\n#ifdef INSTPOW_IMPL\n")
    file.write(f"uint64_t pows[{maxPow}] = {{\n")
    file.write(pows)
    file.write(func)
    file.write("#endif //INSTPOW_H\n" * 2)

print(f"{file_path.split('/')[-1]} generated.")
