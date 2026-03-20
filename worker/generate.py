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


def readDtoFile(file):
    try:
        with open(file, 'r') as fileData:
            return fileData.read()
    except FileNotFoundError:
        print(f"Error: The file '{file}' was not found.")
        return None

def generateStruct(content):
    lines = content.split("\n")
    structCode = f"typedef struct {lines[0]} {{\n"
    structCode += '\n'.join([fieldFromLine(line) for line in lines if ':' in line])
    structCode += f"\n}} {lines[0]};\n"
    structCode = indentStruct(structCode)
    return structCode

def parseAssignment(varName, fieldName, fieldType):
    tok_val = f"json + t[i + 1].start, t[i + 1].end - t[i + 1].start"
    if fieldType == 'char*':
        return f"\t\t\t{varName}->{fieldName} = temp_strndup({tok_val});\n"
    elif fieldType == 'uint64_t':
        return f"\t\t\t{varName}->{fieldName} = strtol(json + t[i + 1].start, NULL, 10);\n"
    elif fieldType == 'char**':
        code  = f"\t\t\tint arrSize_{fieldName} = t[i + 1].size;\n"
        code += f"\t\t\t{varName}->{fieldName} = temp_alloc(sizeof(char*) * arrSize_{fieldName});\n"
        code += f"\t\t\tfor (int j = 0; j < arrSize_{fieldName}; ++j) {{\n"
        code += f"\t\t\t\t{varName}->{fieldName}[j] = temp_strndup(json + t[i + 2 + j].start, t[i + 2 + j].end - t[i + 2 + j].start);\n"
        code += f"\t\t\t}}\n"
        code += f"\t\t\ti += arrSize_{fieldName} + 1;\n"
        return code
    else:
        return f"\t\t\t// TODO: unsupported type {fieldType} for {fieldName}\n"

def generateParser(content):
    lines = content.split("\n")
    structName = lines[0]
    varName = structName[0].lower() + structName[1:]
    fields = [line for line in lines if ':' in line]

    code  = f"int parse{structName}({structName}* {varName}, const char* json, size_t jsonLen) {{\n"
    code += "\tjsmn_parser p;\n"
    code += "\tjsmntok_t t[128];\n\n"
    code += "\tjsmn_init(&p);\n"
    code += "\tint tokCount = jsmn_parse(&p, json, jsonLen, t, sizeof(t) / sizeof(t[0]));\n"
    code += "\tif (tokCount < 0) return -1;\n"
    code += "\tif (tokCount < 1 || t[0].type != JSMN_OBJECT) return -1;\n\n"
    code += "\tfor (int i = 1; i < tokCount; ++i) {\n"

    first = True
    for field_line in fields:
        fieldName = field_line.split(':')[0].strip('\t "')
        fieldValue = field_line.split(':')[1].strip('\t ,')
        fieldType = typeFromValue(fieldValue)

        elsePrefix = "" if first else "else "
        first = False

        code += f'\t\t{elsePrefix}if (t[i].type == JSMN_STRING && (int)strlen("{fieldName}") == t[i].end - t[i].start &&\n'
        code += f'\t\t\t\tstrncmp(json + t[i].start, "{fieldName}", t[i].end - t[i].start) == 0) {{\n'
        code += parseAssignment(varName, fieldName, fieldType)
        code += "\t\t}\n"

    code += "\t}\n"
    code += "\treturn 0;\n"
    code += "}\n"
    return code

def toStringFormat(fieldName, fieldType):
    if fieldType == 'char*':
        return '%s'
    elif fieldType == 'uint64_t':
        return '%lu'
    elif fieldType == 'char**':
        return '[...]'
    else:
        return '%p'

def generateToString(content):
    lines = content.split("\n")
    structName = lines[0]
    varName = structName[0].lower() + structName[1:]
    fields = [line for line in lines if ':' in line]

    fieldEntries = []
    fieldArgs = []
    for field_line in fields:
        fieldName = field_line.split(':')[0].strip('\t "')
        fieldValue = field_line.split(':')[1].strip('\t ,')
        fieldType = typeFromValue(fieldValue)
        fmt = toStringFormat(fieldName, fieldType)
        fieldEntries.append(f"\\t{fieldName}: {fmt}")
        if fieldType != 'char**':
            fieldArgs.append(f"\t\t{varName}->{fieldName}")

    fmtStr = f'{structName}: {{\\n' + ',\\n'.join(fieldEntries) + '\\n}'

    code  = f"char* {structName}ToString({structName}* {varName}) {{\n"
    code += f'\treturn temp_sprintf("{fmtStr}",\n'
    code += ',\n'.join(fieldArgs) + '\n'
    code += "\t);\n"
    code += "}\n"
    return code

def generateDtos():
    dtosCode = "#ifndef DTO_H\n#define DTO_H\n\n#include \"../thirdparty/jsmn.h\"\n\n"
    directoryPath = Path("./resources/dto")
    filesList = [p for p in directoryPath.iterdir() if p.is_file()]
    contents = [readDtoFile(fp) for fp in filesList]
    contents = [c for c in contents if c is not None]

    dtosCode += '\n'.join([generateStruct(c) for c in contents])

    dtosCode += "\n#ifdef DTO_IMPL\n\n"
    dtosCode += '\n'.join([generateParser(c) for c in contents])
    dtosCode += '\n'
    dtosCode += '\n'.join([generateToString(c) for c in contents])
    dtosCode += "\n#endif //DTO_IMPL\n"

    dtosCode += '\n#endif //DTO_H'

    with open(dtoFP, "w") as file:
        file.write(dtosCode)
    print(f"{dtoFP.split('/')[-1]} generated.")

generateDtos()
generateInstpowCode()
