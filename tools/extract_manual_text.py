"""从已下载的 ML307A 官方 PDF 中提取文本，并搜索下载/BOOT 引脚相关信息。"""
import os
import re
import pypdf

BASE = r'out\manuals'
PDFS = [
    'ML307A_硬件设计手册_OpenCPU版.pdf',
    'ML307A_SDK开发指导手册.pdf',
    'ML307A_OpenCPU资源综述.pdf',
]

# 关注的关键词：下载模式 / BOOT 引脚 / 复位
KEYS = [
    r'BOOT', r'DL_', r'\bDL\b', r'STRAP', r'DOWNLOAD', r'下载', r'烧录', r'升级',
    r'拉高', r'拉低', r'短接', r'复位', r'RESET', r'\bRES\b', r'USB_BOOT', r'boot',
]

for pdf in PDFS:
    path = os.path.join(BASE, pdf)
    if not os.path.exists(path):
        print(f'== 跳过（不存在）: {pdf}')
        continue
    print('=' * 70)
    print(f'== {pdf}  ({os.path.getsize(path):,} 字节)')
    try:
        reader = pypdf.PdfReader(path)
    except Exception as e:
        print('   打开失败:', e)
        continue

    pages_text = []
    for i, page in enumerate(reader.pages):
        try:
            t = page.extract_text() or ''
        except Exception:
            t = ''
        pages_text.append(t)

    total = sum(len(t) for t in pages_text)
    print(f'   页数 {len(reader.pages)}，可提取文本合计 {total} 字符')
    if total == 0:
        print('   → 纯扫描件，无文本层，无法提取')
        continue

    out_txt = os.path.join(BASE, os.path.splitext(pdf)[0] + '.txt')
    with open(out_txt, 'w', encoding='utf-8') as f:
        for i, t in enumerate(pages_text):
            f.write(f'\n===== 第 {i+1} 页 =====\n{t}\n')
    print(f'   → 已写出 {out_txt}')

    # 按关键词定位页面
    for i, t in enumerate(pages_text):
        hits = [k for k in KEYS if re.search(k, t)]
        if hits:
            # 只打印含"下载/BOOT 引脚"意味较强的页面摘要
            strong = re.search(r'(BOOT|STRAP|下载|USB_BOOT|拉高|拉低|短接)', t)
            if strong:
                print(f'\n   --- 第 {i+1} 页命中 {sorted(set(hits))} ---')
                # 提取包含关键词的行及上下文
                for line in t.splitlines():
                    if re.search(r'(BOOT|STRAP|下载|USB_BOOT|拉高|拉低|短接|DL\b)', line):
                        print('      ' + line.strip()[:160])
