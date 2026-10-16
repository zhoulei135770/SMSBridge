"""核对我应用里用到的所有 SDK 函数是否都有真实头文件声明，并打印真实原型。

用法: python tools/verify_sdk_api.py
"""
import os
import re
import glob

WS = r'C:\Users\<USER>\Documents\deepseek-harness\default-workspace'
APP = os.path.join(WS, 'app', 'src')
SDK_INC = os.path.join(WS, 'sdk', 'ML307A_OpenCPU_Standard_1.4.2', 'include')

# 收集应用源码里所有被调用的 cm_* / os* / lwip 类符号
used = {}
pat = re.compile(r'\b((?:cm|os|lwip)_[a-z0-9_]+)\s*\(')
for f in glob.glob(os.path.join(APP, '*.c')):
    txt = open(f, encoding='utf-8', errors='replace').read()
    # 去掉本工程自己的 sdk_* 包装
    for m in pat.finditer(txt):
        name = m.group(1)
        used.setdefault(name, set()).add(os.path.basename(f))

# 在 SDK 头文件里找声明
headers = []
for root, _, files in os.walk(SDK_INC):
    for fn in files:
        if fn.endswith('.h'):
            headers.append(os.path.join(root, fn))

print(f'应用调用的 SDK 符号共 {len(used)} 个，SDK 头文件 {len(headers)} 个')
print()

decl_pat = re.compile(r'^[^/\n]*?\b(\w+)\s*\([^;{]*\)\s*;', re.M)

found, missing = {}, []
for name in sorted(used):
    decl = None
    for h in headers:
        try:
            txt = open(h, encoding='utf-8', errors='replace').read()
        except Exception:
            continue
        if name not in txt:
            continue
        for m in decl_pat.finditer(txt):
            if m.group(1) == name:
                decl = (os.path.relpath(h, SDK_INC), ' '.join(m.group(0).split()))
                break
        if decl:
            break
    if decl:
        found[name] = decl
    else:
        missing.append(name)

print('=' * 78)
print('== ① 在 SDK 头文件中【找到声明】的（原型可核对）')
print('=' * 78)
for name, (h, d) in sorted(found.items()):
    print(f'  {name}')
    print(f'      [{h}]  {d[:150]}')

print()
print('=' * 78)
print('== ② 在 SDK 头文件中【未找到声明】的（危险：可能是我手写 extern 或符号名有误）')
print('=' * 78)
for name in missing:
    print(f'  {name}   ← 被这些文件调用: {", ".join(sorted(used[name]))}')

