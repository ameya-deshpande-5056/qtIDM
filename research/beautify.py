import sys

def beautify(js):
    res = []
    indent = 0
    in_str = False
    str_char = ''
    escape = False
    for i, c in enumerate(js):
        if in_str:
            res.append(c)
            if escape:
                escape = False
            elif c == '\\':
                escape = True
            elif c == str_char:
                in_str = False
            continue
        if c in ('"', "'", '`'):
            in_str = True
            str_char = c
            res.append(c)
            continue
        if c == '{':
            indent += 1
            res.append('{\n' + '  ' * indent)
        elif c == '}':
            indent = max(0, indent - 1)
            res.append('\n' + '  ' * indent + '}\n' + '  ' * indent)
        elif c == ';':
            res.append(';\n' + '  ' * indent)
        else:
            res.append(c)
    return ''.join(res)

if __name__ == '__main__':
    for name in ['background.js', 'content.js', 'document.js']:
        src = f'research/extracted/crx/{name}'
        dst = f'research/extracted/crx/{name}.beautified.js'
        with open(src, 'r') as f:
            code = f.read()
        with open(dst, 'w') as f:
            f.write(beautify(code))
        print(f'Beautified {name} -> {dst}')

