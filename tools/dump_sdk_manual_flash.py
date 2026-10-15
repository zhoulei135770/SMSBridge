"""提取 SDK 开发指导手册里与烧录/下载/工具相关的原文。"""
import os
import re
import pypdf

BASE = r'out\manuals'
pdf = 'ML307A_SDK开发指导手册.pdf'
path = os.path.join(BASE, pdf)
reader = pypdf.PdfReader(path)

OUT = os.path.join(BASE, 'SDK手册_烧录相关原文.txt')
buf = [f'文件: {pdf}  共 {len(reader.pages)} 页', '=' * 78]

KEY = r'(烧录|下载|BOOT_MODE|BOOT|工具|aboot|QFlash|升级|固件|串口|USB|驱动|编译)'
for i, page in enumerate(reader.pages):
    try:
        t = page.extract_text() or ''
    except Exception:
        continue
    if re.search(KEY, t):
        buf.append(f'\n{"-"*78}\n----- 第 {i+1} 页 -----\n{"-"*78}')
        buf.append(t)

with open(OUT, 'w', encoding='utf-8') as f:
    f.write('\n'.join(buf))
print('已写出:', OUT)

# 只打印纯 ASCII 关键行（避免控制台中文乱码）
print('\n=== ASCII 关键行 ===')
for line in '\n'.join(buf).splitlines():
    if re.search(r'(BOOT|aboot|QFlash|USB|Download|\.exe|COM\d|baud)', line, re.I):
        a = ''.join(c if ord(c) < 128 else '?' for c in line)
        print('  ' + a.strip()[:150])
