"""导出 ML307A 硬件设计手册中与 BOOT_MODE / 下载控制接口相关的完整原文。"""
import os
import re
import pypdf

BASE = r'out\manuals'
OUT = os.path.join(BASE, 'BOOT_MODE_相关原文.txt')

TARGETS = {
    'ML307A_硬件设计手册_OpenCPU版.pdf': None,   # 全部页，按关键词筛
    'ML307A_SDK开发指导手册.pdf': None,
}

buf = []
for pdf in TARGETS:
    path = os.path.join(BASE, pdf)
    if not os.path.exists(path):
        continue
    reader = pypdf.PdfReader(path)
    buf.append('=' * 78)
    buf.append(f'文件: {pdf}   （共 {len(reader.pages)} 页）')
    buf.append('=' * 78)

    for i, page in enumerate(reader.pages):
        try:
            t = page.extract_text() or ''
        except Exception:
            continue
        if not t.strip():
            continue
        # 命中 BOOT_MODE / 下载控制 / 强制下载 的页面，整页导出
        if re.search(r'(BOOT_MODE|BOOT MODE|下载控制|强制下载|USB强制|DL_MODE|下载模式|固件升级|烧录)', t):
            buf.append(f'\n{"-"*78}\n----- 第 {i+1} 页 -----\n{"-"*78}')
            buf.append(t)

with open(OUT, 'w', encoding='utf-8') as f:
    f.write('\n'.join(buf))

print('已写出:', OUT)
print('内容行数:', len('\n'.join(buf).splitlines()))

# 顺带在控制台给出纯 ASCII 的关键行（避免中文乱码干扰）
print('\n=== 关键 ASCII 行 ===')
for line in '\n'.join(buf).splitlines():
    if re.search(r'BOOT_MODE|BOOT MODE|USB.*DL|DL.*USB|download', line, re.I):
        ascii_line = ''.join(c if ord(c) < 128 else '?' for c in line)
        print('  ' + ascii_line.strip()[:150])
