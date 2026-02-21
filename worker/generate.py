from pathlib import Path

instpowFP = "./include/instpow.h"
dtoFP     = "./include/dto.h"

def generateInstpowCode():
    instpowCode = ''
    
    base   = int(input("Base: "))
    maxPow = input("MaxPow: ")

    pows = ",\n".join([str(base ** i)  for i in range(int(maxPow)) if base ** i <= 2 ** 64]) + '};\n'
    instpowCode += "#ifndef INSTPOW_H\n#define INSTPOW_H\n#include <stdint.h>\n\n"
    instpowCode += "uint64_t instantPow(int pow);\nint getMaxPow(void);\n#ifdef INSTPOW_IMPL\n"
    instpowCode += f"uint64_t pows[{maxPow}] = {{\n"
    instpowCode += pows
    instpowCode += f"""
    \n\nint maxPow = {len(pows.split(',\n'))};
    
        uint64_t instantPow(int pow) {{
            if (pow > maxPow) {{
                return -1;
            }}
        
            return pows[pow];
        }}
        
        int getMaxPow(void) {{
            return maxPow;
        }}
    """
    instpowCode += "#endif //INSTPOW_H\n" * 2

    with open(instpowFP, "w") as file:
        file.write(instpowCode)

    print(f"{instpowFP.split('/')[-1]} generated.")

def typeFromValue(value):
    if value.isdigit():
        return 'uint64_t'
    elif value[0] == '"' and value[-1] == '"':
        return 'char*'
    elif '[' in value or ']' in value:
        return typeFromValue(value.strip('[]').split(', ')[0]) + '*' 
    else:
        return 'void*'

def fieldFromLine(line):
    field = ''
    fieldName = line.split(':')[0].strip('\t "')

    fieldType = ''
    fieldValue = line.split(':')[1].strip('\t ,')
    fieldType = typeFromValue(fieldValue) 
    field += f'\t{fieldType} {fieldName};'

    return field

def indentStruct(structCode):
    fields = [line for line in structCode.split('\n') if ';' in line and '}' not in line]
    types = [field.split(' ')[0] + ' ' for field in fields]
    typeMaxLen = len(max(types, key=len))
    for t in set(types):
        structCode = structCode.replace(t, t + ' ' * (typeMaxLen - len(t)))

    return structCode


def generateStruct(file):
    try:
        with open(file, 'r') as fileData:
            content = fileData.read()
    except FileNotFoundError:
        print(f"Error: The file '{file}' was not found.")
    
    lines = content.split("\n")
    structCode = f"typedef struct {lines[0]} {{\n"
    structCode += '\n'.join([fieldFromLine(line) for line in lines if ':' in line])
    structCode += f"\n}} {lines[0]};\n"
    structCode = indentStruct(structCode)
    return structCode 
    
def generateDtos():
    dtosCode = "#ifndef DTO_H\n#define DTO_H\n\n"
    directoryPath = Path("./resources/dto")
    filesList = [p for p in directoryPath.iterdir() if p.is_file()]
    dtosCode += '\n'.join([generateStruct(fp) for fp in filesList])
    dtosCode += '\n#endif //DTO_H'

    with open(dtoFP, "w") as file:
        file.write(dtosCode)
    print(f"{dtoFP.split('/')[-1]} generated.")

generateDtos()
generateInstpowCode()
