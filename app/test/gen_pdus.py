#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_pdus.py -- 独立的 SMS-DELIVER PDU 编码器 + 测试向量生成器

本文件是 **独立实现**，不依赖 app/src/sms_pdu.c 的任何代码或数据结构，
只依据下列规范：

  * 3GPP TS 23.040 (GSM 03.40)   —— PDU 格式 / TP-UDL / TP-SCTS / UDH
  * 3GPP TS 23.038 (GSM 03.38)   —— DCS / GSM 7bit 默认字母表与扩展表

用途：
  1. 手工检查某组参数的 PDU 十六进制串（命令行参数）；
  2. 用 --emit-c-header 生成 C 单元测试用的测试向量头文件
     （形如 static const struct { const char *name; const char *pdu;
       const char *want_number; const char *want_text; } g_vectors[]）。

只使用 Python 标准库。

--------------------------------------------------------------------------
实现要点（易错处，均按规范处理）
--------------------------------------------------------------------------
1. BCD 半字节交换：地址与时间戳都是 "低位半字节在前"，
   即 86 -> 0x68（f(3.1)）。
   地址长度字节 = **有效数字个数**（不算 '+'，不含奇数补齐用的 0xF）。
   TON=1（国际）时 TOA 低半字节的 NPI 取 1（ISDN/电话），高半字节 = 1。
2. GSM 7bit septet 打包（TS 23.040 9.2.3.1.1）：
   第 n 个 septet 的有效位从 bit n*7 开始，因此会产生七字节一个循环的
   "错位" 效果；最后一个字节没用到的 bit 必须填 0。
3. UDH 的填充位（fill bits）：UDH 必须从 septet 边界 0 开始，
   因此 UDH 占用 ceil(udhl+1)*8/7) 个 septet，正文从这个 septet 之后开始，
   跨过的字节里多出来的 bit 必须填 0。
4. UDL 语义：DCS 为 7bit（且带不带 UDH 都一样）时 UDL = **septet 数**
   （含 UDH 占用的 septet）；UCS2 / 8bit 时 UDL = **字节数**（含 UDH 字节）。
5. TP-SCTS 第 7 字节（时区）：仍是半字节交换（十位在低半字节、个位在高半字节），
   符号位是 **低半字节的 bit3**，负值时把 0x08 与十位数字做 OR（f(9.2.3.11)）。
--------------------------------------------------------------------------
注意：本文件是"参照实现"，如与 C 解码器不一致，需先判断是哪一边错了。
--------------------------------------------------------------------------
"""

import argparse
import os
import sys

# ======================================================================
# 1. GSM 03.38 默认字母表
# ======================================================================

# 基本表：septet(0..127) -> Unicode 码点。索引 0x1B 是扩展转义前缀（非字符）。
GSM7_BASIC = [
    0x0040, 0x00A3, 0x0024, 0x00A5, 0x00E8, 0x00E9, 0x00F9, 0x00EC,
    0x00F2, 0x00C7, 0x000A, 0x00D8, 0x00F8, 0x000D, 0x00C5, 0x00E5,
    0x0394, 0x005F, 0x03A6, 0x0393, 0x039B, 0x03A9, 0x03A0, 0x03A8,
    0x03A3, 0x0398, 0x039E, 0x001B, 0x00C6, 0x00E6, 0x00DF, 0x00C9,
    0x0020, 0x0021, 0x0022, 0x0023, 0x00A4, 0x0025, 0x0026, 0x0027,
    0x0028, 0x0029, 0x002A, 0x002B, 0x002C, 0x002D, 0x002E, 0x002F,
    0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
    0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F,
    0x00A1, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047,
    0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,
    0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057,
    0x0058, 0x0059, 0x005A, 0x00C4, 0x00D6, 0x00D1, 0x00DC, 0x00A7,
    0x00BF, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
    0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F,
    0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077,
    0x0078, 0x0079, 0x007A, 0x00E4, 0x00F6, 0x00F1, 0x00FC, 0x00E0,
]

# 扩展表（0x1B 之后紧跟的 septet）-> Unicode 码点（TS 23.038 表 3）
GSM7_EXT = {
    0x0A: 0x000C,   # 换页 FORM FEED
    0x14: 0x005E,   # ^
    0x28: 0x007B,   # {
    0x29: 0x007D,   # }
    0x2F: 0x005C,   # \
    0x3C: 0x005B,   # [
    0x3D: 0x007E,   # ~
    0x3E: 0x005D,   # ]
    0x40: 0x007C,   # |
    0x65: 0x20AC,   # €
}

# 反向表：Unicode 码点 -> 基本表 septet
CP_TO_BASIC = {}
for _s, _cp in enumerate(GSM7_BASIC):
    if _s == 0x1B:
        continue            # 转义前缀不作为可编码字符
    CP_TO_BASIC.setdefault(_cp, _s)

# 反向表：Unicode 码点 -> 扩展表 septet
CP_TO_EXT = {}
for _s, _cp in GSM7_EXT.items():
    CP_TO_EXT[_cp] = _s


def gsm7_encode_text(text):
    """文本 -> septet 列表；无法用 GSM 7bit 表示的字符抛 ValueError。

    扩展表字符占 2 个 septet：0x1B + 扩展码。
    """
    septets = []
    for ch in text:
        cp = ord(ch)
        if cp in CP_TO_BASIC:
            septets.append(CP_TO_BASIC[cp])
        elif cp in CP_TO_EXT:
            septets.append(0x1B)
            septets.append(CP_TO_EXT[cp])
        else:
            raise ValueError(
                "字符 U+%04X (%r) 无法用 GSM 7bit 默认字母表表示" % (cp, ch))
    return septets


def septets_to_octets(septets):
    """按 TS 23.040 9.2.3.1.1 把 septet 序列打包成八位组（末尾补 0 填充位）。"""
    out = bytearray((len(septets) * 7 + 7) // 8)
    for i, s in enumerate(septets):
        bit = i * 7
        byte = bit // 8
        shift = bit % 8
        out[byte] |= (s << shift) & 0xFF
        if shift > 1:
            out[byte + 1] |= (s >> (8 - shift)) & 0xFF
    return bytes(out)


def pack_septets_bitstream(septets):
    """独立参考实现：先构造 LSB-first 比特流再按 8bit 切分。

    与 septets_to_octets 的"逐 septet 移位"写法互为交叉验证用。
    """
    bits = []
    for s in septets:
        for k in range(7):
            bits.append((s >> k) & 1)
    out = bytearray()
    for i in range(0, len(bits), 8):
        chunk = bits[i:i + 8]
        while len(chunk) < 8:
            chunk.append(0)
        v = 0
        for k, b in enumerate(chunk):
            v |= b << k
        out.append(v)
    return bytes(out)


def octets_to_septets(octets, count=None):
    """septets_to_octets 的逆运算（仅用于自检 / 测试）。"""
    if count is None:
        count = (len(octets) * 8) // 7
    out = []
    for i in range(count):
        bit = i * 7
        byte = bit // 8
        shift = bit % 8
        if byte >= len(octets):
            break
        v = octets[byte]
        if shift > 1:
            if byte + 1 >= len(octets):
                break
            v |= octets[byte + 1] << 8
        out.append((v >> shift) & 0x7F)
    return out


# ======================================================================
# 2. 域编码器
# ======================================================================

# TOA（Type-of-Address）字节：真实现网 PDU 的常见写法。
# 注: TS 23.040 9.1.2.5 把 TON 放在 bit6..4、NPI 放在 bit3..0，
#     据此 0x91 会被读成 TON=1/NPI=1（"国际 + ISDN"）。
#     本编码器沿用现网通行的 0x91/0x81 字面值，理由与证据见测试报告。
TOA_INTL_ISDN = 0x91        # 国际号码
TOA_NATIONAL_ISDN = 0x81    # 国内号码


def bcd_swap(digits):
    """数字串（可含 'F'）-> 半字节交换后的 hex 字符串（低位半字节在前）。"""
    if len(digits) % 2:
        digits += 'F'
    out = []
    for i in range(0, len(digits), 2):
        out.append(digits[i + 1])
        out.append(digits[i])
    return ''.join(out)


def encode_address(number, ton=None, npi=None, toa=None):
    """TP-OA / TP-SCA 字段（含长度字节与 TOA 字节）。

    number  : 号码字符串，可带前导 '+'（国际号码的常见写法）
    ton     : 1 = 国际，2 = 国内，0 = 未知；None 时按有无 '+' 推断
    npi     : 编号计划标识；None 时取 1（ISDN/电话编号计划）
    toa     : 直接给出 TOA 字节（会覆盖 ton/npi）
    返回 (hex_string, ton_value)；ton_value 为写进 TOA 的 TON。

    本实现采用真实现网 PDU 的 TOA 写法（参见本文件顶部 TOA_* 常量与
    README/报告中的"待确认"说明）:
        国际号码 -> 0x91，国内号码 -> 0x81。
    """
    digits = number
    has_plus = digits.startswith('+')
    if has_plus:
        digits = digits[1:]
    for c in digits:
        if c not in '0123456789':
            raise ValueError("号码含非数字字符: %r" % c)
    if len(digits) > 20:
        raise ValueError("号码过长: %d 位" % len(digits))

    if toa is None:
        if npi is None:
            npi = 1                       # ISDN / 电话编号计划
        if ton is None:
            ton = 1 if has_plus else 2
        # 0x91 = 国际 + ISDN；0x81 = 国内(或未知) + ISDN
        toa = TOA_INTL_ISDN if ton == 1 else TOA_NATIONAL_ISDN
    else:
        ton = (toa >> 4) & 0x07

    addr_len = len(digits)
    return '%02X%02X%s' % (addr_len, toa, bcd_swap(digits)), ton


def encode_sca(number, toa=TOA_INTL_ISDN):
    """TP-SCA 字段。

    ⚠ 重要规范差异（本生成器 v2 修正）：
      TP-SCA 的**长度字节是"八位组数"（octets，含 TOA 字节）**，
      而 TP-OA 的长度字节是"半字节数（数字位数）"。两者语义不同！
      依据：3GPP TS 23.040 §9.1.2.1 / 真实现网 PDU 例
        "07917283010010F5..." 里 07 = SCA 信息占 7 个八位组
        （91 + 6 字节 = 12 位号码），而不是 12 或 13。
      早期版本这里误用了 encode_address（半字节长度），已修正；
      自检里的参考 PDU 同步修正为 08 开头。
    """
    if not number:
        return '00'                      # 无 SCA：长度字节 0

    digits = number[1:] if number.startswith('+') else number
    for c in digits:
        if c not in '0123456789':
            raise ValueError("SCA 含非数字字符: %r" % c)

    octets = 1 + (len(digits) + 1) // 2  # TOA 字节 + BCD 号码字节
    return '%02X%02X%s' % (octets, toa, bcd_swap(digits))


def encode_scts(dt=None, tz_quarters=32):
    """TP-SCTS：7 字节（14 个半字节）。

    dt          : datetime；默认 2024-06-15 12:34:56
    tz_quarters : 时区，单位 15 分钟（+8 区 = 32）
    """
    if dt is None:
        import datetime
        dt = datetime.datetime(2024, 6, 15, 12, 34, 56)
    half = []
    for value, width in ((dt.year % 100, 2), (dt.month, 2), (dt.day, 2),
                         (dt.hour, 2), (dt.minute, 2), (dt.second, 2)):
        s = '%0*d' % (width, value)
        half.append(s[1])
        half.append(s[0])
    # 时区（3GPP TS 23.040 §9.2.3.11）：
    #   数值 = 与 GMT 的差值，单位 15 分钟；BCD 半字节交换，即
    #   **低半字节 = 十位，高半字节 = 个位**；符号位是低半字节的 bit3
    #   （0=正, 1=负，即 octet bit 3）。
    #   真实例：中国移动 SCTS 末字节 0x23 = 十位3/个位2 = 32 → +8 小时。
    #   -20（-5 小时）→ 低半字节 2|0x08 = 0xA，高半字节 0 → 0x0A。
    #   （早期版本把符号位加在个位上，得到 0x82，属实现错误，已修正。）
    neg = tz_quarters < 0
    tz = abs(tz_quarters)
    tens, units = divmod(tz, 10)
    # 字节 = 高半字节(个位) << 4 | 低半字节(十位 | 符号位)；再按"先高后半"输出十六进制
    tz_byte = ((units & 0x0F) << 4) | ((tens & 0x07) | (0x08 if neg else 0x00))
    half.append('%X' % ((tz_byte >> 4) & 0x0F))
    half.append('%X' % (tz_byte & 0x0F))
    return ''.join(half)


def encode_udh(ies):
    """把若干 (iei, data_bytes) 组装成 UDH（含 UDHL 长度字节）。

    ⚠ 规范修正（本生成器 v2）：
      UDHL = **UDH 的字节数，不含 UDHL 字节自身**（3GPP TS 23.040 §9.2.3.24）。
      真实现网级联短信 PDU 例（AT+CMGL 常见形态）：
          ... 0A 05 00 03 17 02 01 A0 05 ...
      其中 UDL=0x0A，UD 以 `05` 开头，其后恰好 5 个字节 `00 03 17 02 01`
      （IEI=00 级联8bit, IEDL=03, ref=0x17, total=02, seq=01）。
      早期版本误写成 len(body)+1（含自身），已在自检与用例中一并修正。
    """
    body = bytearray()
    for iei, data in ies:
        if len(data) > 255:
            raise ValueError("IE 数据过长")
        body.append(iei)
        body.append(len(data))
        body.extend(data)
    if len(body) > 255:
        raise ValueError("UDH 过长")
    return bytes([len(body)]) + bytes(body)


def udh_cat8(ref, total, seq):
    """级联长短信 IEI 0x00（8bit 参考号），返回 IE 数据字节。"""
    return bytes([ref & 0xFF, total & 0xFF, seq & 0xFF])


def udh_cat16(ref, total, seq):
    """级联长短信 IEI 0x08（16bit 参考号，大端），返回 IE 数据字节。"""
    return bytes([(ref >> 8) & 0xFF, ref & 0xFF,
                  total & 0xFF, seq & 0xFF])


def encode_ucs2(text):
    """文本 -> UTF-16BE 字节（BMP 外字符用代理对）。"""
    return text.encode('utf-16-be')


# ======================================================================
# 3. SMS-DELIVER PDU 组装
# ======================================================================

def build_deliver(text=None,
                  number='+8613800210500',
                  ton=1,
                  sca=None,
                  dcs=0x00,
                  scts=None,
                  tz_quarters=32,
                  udh=None,
                  udhi=False,
                  ud=None,
                  pid=0x00,
                  toa=None,
                  sca_toa=TOA_INTL_ISDN):
    """生成一条 SMS-DELIVER PDU 十六进制串。

    text       : 消息正文（dcs=8bit 时忽略，用 ud 给原始字节）
    number/ton : 发件号码与 TON（ton=1 时号码带 '+'）
    sca        : 短信中心号码；None 表示写 '00'（无 SCA）
    dcs        : TP-DCS 原始字节。0x00=7bit，0x04=8bit，0x08=UCS2
    scts       : datetime 或 None（用默认值）
    tz_quarters: 时区（15 分钟单位）
    udh        : UDH 字节串（含 UDHL）或 None
    udhi       : 是否置 TP-UDHI 位（若给了 udh 则强制置位）
    ud         : 8bit 数据短信的原始字节
    toa        : TP-OA 的 TOA 字节；None = 按 ton 选 0x91(国际)/0x81(其它)
    sca_toa    : TP-SCA 的 TOA 字节
    """
    if udh is not None:
        udhi = True
    if udhi and udh is None:
        raise ValueError("udhi=1 但未提供 udh")

    # --- 首字节：TP-MTI=00 (SMS-DELIVER)、TP-UDHI、TP-SRI 等全 0 ---
    first = 0x00
    if udhi:
        first |= 0x40

    dcs_alpha = (dcs >> 2) & 0x03 if (dcs & 0xC0) == 0x00 else None
    if (dcs & 0xF0) == 0xF0:
        dcs_alpha = (dcs >> 2) & 0x03

    udh_len = len(udh) if udh is not None else 0
    if udh is not None and udh_len > 140:
        raise ValueError("UDH 超过 140 字节")

    if dcs_alpha == 0x00:                       # 7bit
        if text is None:
            raise ValueError("7bit 需要 text")
        septets = gsm7_encode_text(text)
        if udh is not None:
            # UDH 必须从 septet 0 开始，占 ceil(udh_len*8/7) 个 septet
            hdr_septets = (udh_len * 8 + 6) // 7
            septets = [0] * hdr_septets + septets
            udl = len(septets)
            body = septets_to_octets(septets)
            # 前 udh_len 个字节必须是 UDH 本身，用规范算法等价地覆盖写入
            body = bytearray(body)
            body[0:udh_len] = udh
            body = bytes(body)
        else:
            udl = len(septets)
            body = septets_to_octets(septets)
    elif dcs_alpha == 0x02:                     # UCS2
        if text is None:
            raise ValueError("UCS2 需要 text")
        payload = encode_ucs2(text)
        if udh is not None:
            payload = udh + payload
        udl = len(payload)
        body = payload
    else:                                       # 8bit / 其它
        if ud is None:
            raise ValueError("8bit 需要 ud 原始字节")
        payload = ud
        if udh is not None:
            payload = udh + payload
        udl = len(payload)
        body = payload

    if udl > 255:
        raise ValueError("UDL=%d 超过 255" % udl)
    if len(body) > 140:
        raise ValueError("TP-UD 长度 %d 超过 140 字节" % len(body))

    pdu = (encode_sca(sca, sca_toa)
           + '%02X' % first
           + encode_address(number, ton=ton, toa=toa)[0]
           + '%02X' % pid
           + '%02X' % dcs
           + encode_scts(scts, tz_quarters)
           + '%02X' % udl
           + body.hex().upper())
    return pdu


def hexify(text):
    """文本 -> 大写 hex（'414243' 形式），用于把 C 断言字符串写进头文件。"""
    return text.encode('utf-8').hex().upper()


# ======================================================================
# 4. 测试向量
# ======================================================================

EXT_CHARS = '^{}\\[~]|' + '\u20ac'      # ^ { } \ [ ] ~ | €


def build_vectors():
    """返回 (vectors, notes)。vector 字段与 C 头文件中的结构一致。"""
    vectors = []
    notes = []

    def add(name, pdu, number, text, scts=None, alpha=None, concat=None,
            tz=None, expect_err=None, **extra):
        v = {
            'name': name,
            'pdu': pdu,
            'number': number,
            'text': text,
        }
        if scts is not None:
            v['scts'] = scts
        if alpha is not None:
            v['alpha'] = alpha
        if concat is not None:
            v['concat'] = concat
        if tz is not None:
            v['tz'] = tz
        if expect_err is not None:
            v['expect_err'] = expect_err
        v.setdefault('udhi', 0)
        v.update(extra)
        vectors.append(v)
        return v

    # ---------------------------------------------------------------
    # 1) 纯英文短消息（7bit，无 UDH）
    # ---------------------------------------------------------------
    add('english_hello',
        build_deliver('Hello', number='+8613800210500', ton=1, sca='+8613800210500'),
        '+8613800210500', 'Hello', scts='2024-06-15 12:34:56', alpha='7BIT')

    add('english_lower_case',
        build_deliver('the quick brown fox', number='+8613800210500',
                      sca='+8613800210500'),
        '+8613800210500', 'the quick brown fox',
        scts='2024-06-15 12:34:56', alpha='7BIT')

    # ---------------------------------------------------------------
    # 2) septet 跨字节边界：7 字符（正好 49bit，无填充）与 8 字符
    # ---------------------------------------------------------------
    add('len7_exact_octets',
        build_deliver('ABCDEFG', number='+8613800210500', sca='+8613800210500'),
        '+8613800210500', 'ABCDEFG', scts='2024-06-15 12:34:56', alpha='7BIT')

    add('len8_cross_boundary',
        build_deliver('ABCDEFGH', number='+8613800210500', sca='+8613800210500'),
        '+8613800210500', 'ABCDEFGH', scts='2024-06-15 12:34:56', alpha='7BIT')

    add('len1_single_char',
        build_deliver('A', number='+8613800210500', sca='+8613800210500'),
        '+8613800210500', 'A', scts='2024-06-15 12:34:56', alpha='7BIT')

    add('len14_two_octet_cycle',
        build_deliver('ABCDEFGHIJKLMN', number='+8613800210500',
                      sca='+8613800210500'),
        '+8613800210500', 'ABCDEFGHIJKLMN',
        scts='2024-06-15 12:34:56', alpha='7BIT')

    # 160 septet 满长度（7bit 单条上限）
    add('len160_max_single',
        build_deliver('A' * 160, number='+8613800210500', sca='+8613800210500'),
        '+8613800210500', 'A' * 160, scts='2024-06-15 12:34:56', alpha='7BIT')

    # 跨 7 字节循环边界的典型文本
    add('len9_septet_shift',
        build_deliver('123456789', number='+8613800210500', sca='+8613800210500'),
        '+8613800210500', '123456789', scts='2024-06-15 12:34:56', alpha='7BIT')

    # ---------------------------------------------------------------
    # 3) 扩展表字符（每个字符 2 个 septet，且 0x1B 可能跨字节）
    # ---------------------------------------------------------------
    add('gsm7_ext_all',
        build_deliver(EXT_CHARS, number='+8613800210500', sca='+8613800210500'),
        '+8613800210500', EXT_CHARS, scts='2024-06-15 12:34:56', alpha='7BIT')

    add('gsm7_ext_euro_only',
        build_deliver('\u20ac', number='+8613800210500', sca='+8613800210500'),
        '+8613800210500', '\u20ac', scts='2024-06-15 12:34:56', alpha='7BIT')

    add('gsm7_ext_brackets',
        build_deliver('a[b]c{d}e', number='+8613800210500', sca='+8613800210500'),
        '+8613800210500', 'a[b]c{d}e', scts='2024-06-15 12:34:56', alpha='7BIT')

    add('gsm7_ext_mixed_long',
        build_deliver('AB^CD~EF|GH\u20acIJ' * 3,
                      number='+8613800210500', sca='+8613800210500'),
        '+8613800210500', 'AB^CD~EF|GH\u20acIJ' * 3,
        scts='2024-06-15 12:34:56', alpha='7BIT')

    # 扩展字符位于 7 字符边界（0x1B 正好跨字节）
    add('gsm7_ext_at_boundary',
        build_deliver('ABCDEF^{', number='+8613800210500', sca='+8613800210500'),
        '+8613800210500', 'ABCDEF^{', scts='2024-06-15 12:34:56', alpha='7BIT')

    # 基本表中的非 ASCII 字符（£ ¥ è 等）
    add('gsm7_basic_nonascii',
        build_deliver('\u00a3\u00a5\u00e8\u00f1\u00dc', number='+8613800210500',
                      sca='+8613800210500'),
        '+8613800210500', '\u00a3\u00a5\u00e8\u00f1\u00dc',
        scts='2024-06-15 12:34:56', alpha='7BIT')

    # ---------------------------------------------------------------
    # 4) UCS2
    # ---------------------------------------------------------------
    add('ucs2_chinese',
        build_deliver('\u4f60\u597d\u4e16\u754c', number='+8613800210500',
                      dcs=0x08, sca='+8613800210500'),
        '+8613800210500', '\u4f60\u597d\u4e16\u754c',
        scts='2024-06-15 12:34:56', alpha='UCS2')

    add('ucs2_mixed_cn_en',
        build_deliver('\u4f60\u597d ABC \u4e16\u754c 123',
                      number='+8613800210500', dcs=0x08, sca='+8613800210500'),
        '+8613800210500', '\u4f60\u597d ABC \u4e16\u754c 123',
        scts='2024-06-15 12:34:56', alpha='UCS2')

    add('ucs2_bmp_other',
        build_deliver('\u00e9\u00fc\u00df \u20ac \u2192',
                      number='+8613800210500', dcs=0x08, sca='+8613800210500'),
        '+8613800210500', '\u00e9\u00fc\u00df \u20ac \u2192',
        scts='2024-06-15 12:34:56', alpha='UCS2')

    add('ucs2_ascii_only',
        build_deliver('Plain ASCII via UCS2', number='+8613800210500',
                      dcs=0x08, sca='+8613800210500'),
        '+8613800210500', 'Plain ASCII via UCS2',
        scts='2024-06-15 12:34:56', alpha='UCS2')

    # 70 个 UCS2 字符（140 字节满长度）
    add('ucs2_len70_cn',
        build_deliver('\u6d4b' * 70, number='+8613800210500',
                      dcs=0x08, sca='+8613800210500'),
        '+8613800210500', '\u6d4b' * 70,
        scts='2024-06-15 12:34:56', alpha='UCS2')

    # ---------------------------------------------------------------
    # 5) 号码格式：国际 / 国内 / 奇偶位数
    # ---------------------------------------------------------------
    add('number_intl_even',
        build_deliver('intl even', number='+8613800210500', ton=1,
                      sca='+8613800210500'),
        '+8613800210500', 'intl even', scts='2024-06-15 12:34:56', alpha='7BIT')

    add('number_intl_odd',
        build_deliver('intl odd', number='+861380021050', ton=1,
                      sca='+8613800210500'),
        '+861380021050', 'intl odd', scts='2024-06-15 12:34:56', alpha='7BIT')

    add('number_local_even',
        build_deliver('local even', number='13800210500', ton=2,
                      sca='+8613800210500'),
        '13800210500', 'local even', scts='2024-06-15 12:34:56', alpha='7BIT')

    add('number_local_odd',
        build_deliver('local odd', number='1380021050', ton=2,
                      sca='+8613800210500'),
        '1380021050', 'local odd', scts='2024-06-15 12:34:56', alpha='7BIT')

    add('number_intl_short_odd',
        build_deliver('short intl', number='+8613', ton=1,
                      sca='+8613800210500'),
        '+8613', 'short intl', scts='2024-06-15 12:34:56', alpha='7BIT')

    add('number_local_two_digit',
        build_deliver('x', number='10', ton=2, sca='+8613800210500'),
        '10', 'x', scts='2024-06-15 12:34:56', alpha='7BIT')

    add('no_sca',
        build_deliver('no sca here', number='+8613800210500', sca=None),
        '+8613800210500', 'no sca here', scts='2024-06-15 12:34:56', alpha='7BIT')

    # ---------------------------------------------------------------
    # 6) SCTS 与时区
    # ---------------------------------------------------------------
    add('scts_epoch_2000',
        build_deliver('y2k', number='+8613800210500', sca='+8613800210500',
                      scts=__import__('datetime').datetime(2000, 1, 1, 0, 0, 0),
                      tz_quarters=0),
        '+8613800210500', 'y2k', scts='2000-01-01 00:00:00', alpha='7BIT',
        tz=0)

    add('scts_1999',
        build_deliver('old', number='+8613800210500', sca='+8613800210500',
                      scts=__import__('datetime').datetime(1999, 12, 31, 23, 59, 59),
                      tz_quarters=32),
        '+8613800210500', 'old', scts='1999-12-31 23:59:59', alpha='7BIT', tz=32)

    add('scts_tz_negative',
        build_deliver('ny', number='+12025550143', ton=1,
                      sca='+8613800210500',
                      scts=__import__('datetime').datetime(2024, 1, 2, 3, 4, 5),
                      tz_quarters=-20),
        '+12025550143', 'ny', scts='2024-01-02 03:04:05', alpha='7BIT', tz=-20)

    add('scts_tz_quarter_hour',
        build_deliver('nepal', number='+9779800000000', ton=1,
                      sca='+8613800210500',
                      scts=__import__('datetime').datetime(2023, 7, 8, 9, 10, 11),
                      tz_quarters=23),
        '+9779800000000', 'nepal', scts='2023-07-08 09:10:11', alpha='7BIT',
        tz=23)

    # ---------------------------------------------------------------
    # 7) UDH 级联（8bit 与 16bit 参考号）+ 填充位
    # ---------------------------------------------------------------
    cat8_part2 = encode_udh([(0x00, udh_cat8(0x42, 3, 2))])
    add('concat_8bit_part2',
        build_deliver('\u7b2c\u4e8c\u7247hello', number='+8613800210500',
                      dcs=0x08, udh=cat8_part2, sca='+8613800210500'),
        '+8613800210500', '\u7b2c\u4e8c\u7247hello',
        scts='2024-06-15 12:34:56', alpha='UCS2',
        concat=(1, 0, 0x42, 3, 2), udhi=1)

    cat16_part5 = encode_udh([(0x08, udh_cat16(0x1234, 10, 5))])
    add('concat_16bit_part5',
        build_deliver('\u5206\u7247\u4e94', number='+8613800210500',
                      dcs=0x08, udh=cat16_part5, sca='+8613800210500'),
        '+8613800210500', '\u5206\u7247\u4e94',
        scts='2024-06-15 12:34:56', alpha='UCS2',
        concat=(1, 1, 0x1234, 10, 5), udhi=1)

    add('concat_16bit_ref_zero',
        build_deliver('zero ref', number='+8613800210500', dcs=0x00,
                      udh=encode_udh([(0x08, udh_cat16(0, 2, 1))]),
                      sca='+8613800210500'),
        '+8613800210500', 'zero ref', scts='2024-06-15 12:34:56', alpha='7BIT',
        concat=(1, 1, 0, 2, 1), udhi=1)

    # 7bit + UDH，重点验证 fill bits：
    #   * 8bit 参考号 UDH = 6 字节 -> 占 ceil(48/7)=7 个 septet，无填充
    #   * 16bit 参考号 UDH = 7 字节 -> 占 ceil(56/7)=8 个 septet，无填充
    #   * 5 字节 UDH（自定义 IE）-> 占 ceil(40/7)=6 个 septet，有 2 个填充位
    add('concat_8bit_7bit_text',
        build_deliver('ABCDEFGH', number='+8613800210500', dcs=0x00,
                      udh=encode_udh([(0x00, udh_cat8(0x7F, 2, 1))]),
                      sca='+8613800210500'),
        '+8613800210500', 'ABCDEFGH', scts='2024-06-15 12:34:56', alpha='7BIT',
        concat=(1, 0, 0x7F, 2, 1), udhi=1)

    add('concat_16bit_7bit_text',
        build_deliver('ABCDEFGH', number='+8613800210500', dcs=0x00,
                      udh=encode_udh([(0x08, udh_cat16(0xBEEF, 2, 1))]),
                      sca='+8613800210500'),
        '+8613800210500', 'ABCDEFGH', scts='2024-06-15 12:34:56', alpha='7BIT',
        concat=(1, 1, 0xBEEF, 2, 1), udhi=1)

    # 5 字节 UDH（UDHL=5，非 7 的整数倍）=> 必须验证 fill bit 与 septet 偏移
    five_byte_udh = encode_udh([(0x70, b'\x01\x02\x03')])
    add('udh_fill_bits_5byte',
        build_deliver('XY', number='+8613800210500', dcs=0x00,
                      udh=five_byte_udh, sca='+8613800210500'),
        '+8613800210500', 'XY', scts='2024-06-15 12:34:56', alpha='7BIT',
        concat=None, udhi=1)

    # 1 字节 UDH（UDHL=1，空 IE 列表）=> 占 ceil(8/7)=2 septet，1 个填充位
    add('udh_fill_bits_1byte',
        build_deliver('XY', number='+8613800210500', dcs=0x00,
                      udh=bytes([0x00]), sca='+8613800210500'),
        '+8613800210500', 'XY', scts='2024-06-15 12:34:56', alpha='7BIT',
        udhi=1)

    # 3 字节 UDH（UDHL=3）=> 占 ceil(24/7)=4 septet，4 个填充位
    add('udh_fill_bits_3byte',
        build_deliver('ABC', number='+8613800210500', dcs=0x00,
                      udh=encode_udh([(0x70, b'\xAA')]),
                      sca='+8613800210500'),
        '+8613800210500', 'ABC', scts='2024-06-15 12:34:56', alpha='7BIT',
        udhi=1)

    # ---------------------------------------------------------------
    # 8) UDL = 0 的空短信
    # ---------------------------------------------------------------
    add('empty_7bit',
        build_deliver('', number='+8613800210500', sca='+8613800210500'),
        '+8613800210500', '', scts='2024-06-15 12:34:56', alpha='7BIT')

    add('empty_ucs2',
        build_deliver('', number='+8613800210500', dcs=0x08,
                      sca='+8613800210500'),
        '+8613800210500', '', scts='2024-06-15 12:34:56', alpha='UCS2')

    # ---------------------------------------------------------------
    # 9) 8bit 数据短信（UDHI=0）
    # ---------------------------------------------------------------
    add('data8bit_binary',
        build_deliver(None, number='+8613800210500', dcs=0x04,
                      ud=bytes([0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01, 0x7F, 0x80, 0xFF]),
                      sca='+8613800210500'),
        '+8613800210500', '', scts='2024-06-15 12:34:56', alpha='8BIT',
        ud_hex='DEADBEEF00017F80FF', udl=9)

    add('data8bit_ota_like',
        build_deliver(None, number='+8613800210500', dcs=0x04,
                      ud=bytes(range(0, 40)), sca='+8613800210500'),
        '+8613800210500', '', scts='2024-06-15 12:34:56', alpha='8BIT',
        ud_hex=bytes(range(0, 40)).hex().upper(), udl=40)

    add('data8bit_with_udh',
        build_deliver(None, number='+8613800210500', dcs=0x04,
                      ud=bytes([0x01, 0x02, 0x03, 0x04]),
                      udh=bytes([0x05, 0x00, 0x03, 0x99, 0x02, 0x01]),
                      sca='+8613800210500'),
        '+8613800210500', '', scts='2024-06-15 12:34:56', alpha='8BIT',
        concat=(1, 0, 0x99, 2, 1), udhi=1)

    # ---------------------------------------------------------------
    # 10) DCS 变体（带 message class、MWI）
    # ---------------------------------------------------------------
    add('dcs_class0_7bit',
        build_deliver('flash sms', number='+8613800210500', dcs=0x10,
                      sca='+8613800210500'),
        '+8613800210500', 'flash sms', scts='2024-06-15 12:34:56', alpha='7BIT')

    add('dcs_ucs2_class',
        build_deliver('\u4f60\u597d', number='+8613800210500', dcs=0x18,
                      sca='+8613800210500'),
        '+8613800210500', '\u4f60\u597d', scts='2024-06-15 12:34:56',
        alpha='UCS2')

    add('dcs_8bit_class',
        build_deliver(None, number='+8613800210500', dcs=0x14,
                      ud=bytes([0x11, 0x22, 0x33]), sca='+8613800210500'),
        '+8613800210500', '', scts='2024-06-15 12:34:56', alpha='8BIT',
        ud_hex='112233', udl=3)

    # 含空格/冒号的十六进制串（+CMT 上报格式变体）
    spaced = build_deliver('spaced hex', number='+8613800210500',
                           sca='+8613800210500')
    spaced_in = ' '.join(spaced[i:i + 2] for i in range(0, len(spaced), 2))
    add('hex_with_spaces',
        spaced_in, '+8613800210500', 'spaced hex',
        scts='2024-06-15 12:34:56', alpha='7BIT')

    # 小写十六进制
    add('hex_lowercase',
        spaced.lower(), '+8613800210500', 'spaced hex',
        scts='2024-06-15 12:34:56', alpha='7BIT')

    return vectors, notes


# ======================================================================
# 5. C 头文件输出
# ======================================================================

C_HEADER_TEMPLATE = r'''/* 本文件由 app/test/gen_pdus.py 自动生成，请勿手工修改。
 *
 * 生成命令: python app/test/gen_pdus.py --emit-c-header app/test/sms_pdu_vectors.h
 *
 * 编码器实现依据 3GPP TS 23.040 / TS 23.038，与被测的 sms_pdu.c 相互独立。
 * 约定: PDU 之外的期望值一律用 UTF-8 字节的十六进制大写串表示，避免源文件
 *       编码差异影响结果。
 */
#ifndef SMS_PDU_VECTORS_H
#define SMS_PDU_VECTORS_H

#include <stddef.h>

struct sms_pdu_vector {
    const char *name;        /* 用例名 */
    const char *pdu;         /* 输入的 PDU 十六进制串 */
    const char *want_number; /* 期望的发件号码 (UTF-8 字面量) */
    const char *want_text_hex; /* 期望的正文, UTF-8 字节 hex */
    const char *want_scts;   /* 期望时间戳, NULL 表示不校验 */
    const int   want_alpha;  /* 期望 sms_alpha_t, -1 表示不校验 */
    const int   want_udhi;   /* 期望 TP-UDHI, -1 表示不校验 */
    const int   want_concat_present; /* -1 = 不校验 */
    const int   want_concat_16bit;
    const int   want_concat_ref;
    const int   want_concat_total;
    const int   want_concat_seq;
    const int   want_tz;     /* 期望 tz_quarters, INT_MIN 表示不校验 */
    const int   want_udl;    /* 期望 TP-UDL, -1 表示不校验 */
};

#define SMS_VEC_TZ_ANY   (-2147483647 - 1)
#define SMS_VEC_ANY      (-1)

static const struct sms_pdu_vector g_vectors[] = {
'''

ALPHA_ENUM = {'7BIT': 'SMS_ALPHA_7BIT', '8BIT': 'SMS_ALPHA_8BIT',
              'UCS2': 'SMS_ALPHA_UCS2'}


def c_string(s):
    """UTF-8 字符串 -> C 字面量（只保留 ASCII 可打印字符，其余转义为 \\xHH）。

    注意: \\xHH 之后若紧跟十六进制字符会被 C 解析成更长的转义，
    因此本函数不用 \\xHH 形式，而是把非 ASCII 字节放在独立字符串段中，
    并用 "" 明确断开（例如 "A" "\xe4\xbd\xa0" "B"）。
    """
    parts = []
    cur = []
    for b in s.encode('utf-8'):
        if 0x20 <= b <= 0x7E and b not in (0x22, 0x5C):   # 可打印且非 " \
            cur.append(chr(b))
        else:
            if cur:
                parts.append('"' + ''.join(cur) + '"')
                cur = []
            parts.append('"\\x%02X"' % b)
    if cur:
        parts.append('"' + ''.join(cur) + '"')
    if not parts:
        return '""'
    return ' '.join(parts)


def write_c_header(path, vectors):
    lines = [C_HEADER_TEMPLATE]
    for v in vectors:
        alpha = ALPHA_ENUM.get(v.get('alpha'), 'SMS_VEC_ANY')
        cat = v.get('concat')
        fields = [
            c_string(v['name']),
            c_string(v['pdu']),
            c_string(v['number']),
            c_string(hexify(v['text'])),
            c_string(v['scts']) if v.get('scts') else 'NULL',
            alpha,
            str(v['udhi']),
            str(cat[0]) if cat else 'SMS_VEC_ANY',
            str(cat[1]) if cat else 'SMS_VEC_ANY',
            str(cat[2]) if cat else 'SMS_VEC_ANY',
            str(cat[3]) if cat else 'SMS_VEC_ANY',
            str(cat[4]) if cat else 'SMS_VEC_ANY',
            str(v['tz']) if 'tz' in v else 'SMS_VEC_TZ_ANY',
            str(v['udl']) if 'udl' in v else 'SMS_VEC_ANY',
        ]
        # C 结构初始化不允许最后一个成员后面跟逗号（否则会多出一个全零条目）
        lines.append('    { ' + ',\n      '.join(fields[:-1]) + ',\n      '
                     + fields[-1] + ' },\n')
    lines.append('};\n')
    lines.append('\n#define SMS_VEC_COUNT ((int)(sizeof(g_vectors) / sizeof(g_vectors[0])))\n')
    lines.append('\n#endif /* SMS_PDU_VECTORS_H */\n')
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines))


# ======================================================================
# 6. 自检
# ======================================================================

def self_check():
    """编码器自身的往返自检（打包/解包、BCD、UCS2），返回错误列表。"""
    errs = []

    # 7bit 打包/解包往返 + 与"比特流"独立实现交叉验证
    import random
    rng = random.Random(20240615)
    for n in list(range(0, 40)) + [70, 71, 139, 140, 159, 160]:
        septets = [(i * 7 + 3) & 0x7F for i in range(n)]
        packed = septets_to_octets(septets)
        back = octets_to_septets(packed, n)
        if back != septets:
            errs.append('septet 往返失败 n=%d' % n)
            break
        if len(packed) != (n * 7 + 7) // 8:
            errs.append('septet 打包长度错误 n=%d' % n)
            break
        if packed != pack_septets_bitstream(septets):
            errs.append('septet 打包与比特流参考实现不一致 n=%d: %s vs %s'
                        % (n, packed.hex(), pack_septets_bitstream(septets).hex()))
            break
    for _ in range(200):
        n = rng.randint(0, 160)
        septets = [rng.randint(0, 127) for _ in range(n)]
        packed = septets_to_octets(septets)
        if packed != pack_septets_bitstream(septets):
            errs.append('随机 septet 序列打包不一致 n=%d' % n)
            break
        if octets_to_septets(packed, n) != septets:
            errs.append('随机 septet 序列往返失败 n=%d' % n)
            break

    # 'Hello' 的 7bit 打包（LSB-first 比特流的手工核对结果）
    if septets_to_octets([0x48, 0x65, 0x6C, 0x6C, 0x6F]).hex().upper() != 'C8329BFD06':
        errs.append("'Hello' 打包结果异常: %s"
                    % septets_to_octets([0x48, 0x65, 0x6C, 0x6C, 0x6F]).hex().upper())

    # 打包产生的填充位必须为 0（每个 7 字节循环最后多出来的 bit）
    for n in range(1, 160):
        packed = septets_to_octets([0x7F] * n)
        used = n * 7                     # 实际使用的 bit 数
        if used % 8:                     # 最后一个字节有填充位
            pad = 8 - (used % 8)
            if packed[-1] != (0xFF >> pad):
                errs.append('n=%d 的填充位非 0: 末字节 %02X' % (n, packed[-1]))
                break

    # BCD 交换
    if bcd_swap('12345') != '2143F5':
        errs.append('bcd_swap 奇数位错误: %s' % bcd_swap('12345'))
    if bcd_swap('1234') != '2143':
        errs.append('bcd_swap 偶数位错误: %s' % bcd_swap('1234'))

    # 地址（TOA = 0x80 | (TON<<4) | NPI；国际+ISDN = 0x91）
    addr, ton_out = encode_address('+8613800210500')
    if addr != '0D91683108200105F0' or ton_out != 1:
        errs.append('国际号码编码错误: %s' % addr)
    addr, ton_out = encode_address('1380021050', ton=2, npi=1)
    if addr != '0A813108200105' or ton_out != 2:
        errs.append('国内奇数位号码编码错误: %s' % addr)

    # SCTS 时区符号位（十位在低半字节、个位在高半字节，符号位=低半字节 bit3）
    if encode_scts(tz_quarters=32)[12:] != '23':
        errs.append('+8 区时区编码错误: %s' % encode_scts(tz_quarters=32))
    if encode_scts(tz_quarters=-20)[12:] != '0A':
        errs.append('-5 区时区编码错误: %s' % encode_scts(tz_quarters=-20))
    if encode_scts(tz_quarters=-8)[12:] != '88':
        errs.append('-2 区时区编码错误: %s' % encode_scts(tz_quarters=-8))

    # UCS2
    if encode_ucs2('\u4f60').hex().upper() != '4F60':
        errs.append('UCS2 编码错误')

    # 手工核对过的整条 PDU（回归）：
    #   SCA=+8613800210500(0x91)，长度字节 08 = 8 个八位组（TOA + 7 字节 BCD）
    #   MTI=0x00 TON=0x91 +8613800210500
    #   PID=00 DCS=00 SCTS=2024-06-15 12:34:56 +8 UDL=05 "Hello"
    ref = ('0891683108200105F0'      # TP-SCA: len=08 octets, TOA=91, 13 位号码
           '00'                      # 首字节: MTI=00, UDHI=0
           '0D91683108200105F0'      # TP-OA: len=0D 半字节(13 位), TOA=91
           '00'                      # TP-PID
           '00'                      # TP-DCS
           '426051214365' '23'       # TP-SCTS
           '05'                      # TP-UDL = 5 septets
           'C8329BFD06')             # "Hello" 7bit 打包（LSB-first 比特流）
    got = build_deliver('Hello', number='+8613800210500', sca='+8613800210500')
    if got != ref:
        # 打印首个差异的字节位置，便于定位
        rb, gb = bytes.fromhex(ref), bytes.fromhex(got)
        d = next((i for i in range(max(len(rb), len(gb)))
                  if rb[i:i + 1] != gb[i:i + 1]), -1)
        errs.append('参考 PDU 不一致 (首个差异字节 %d, 期望 %s, 实际 %s):\n'
                    '  期望 %s\n  实际 %s'
                    % (d, rb[d:d + 1].hex() if d >= 0 else '-',
                       gb[d:d + 1].hex() if d >= 0 else '-', ref, got))

    return errs


def build_expected_deliver_pdu():
    """手工核对过的参考 PDU（用于防止编码器自身回归）。

    SCA=+8613800210500, OA=+8613800210500(ton=1), PID=00, DCS=00,
    SCTS=2024-06-15 12:34:56 tz=+8 (32), UDL=05, text='Hello'
    """
    return build_deliver('Hello', number='+8613800210500',
                         sca='+8613800210500')


def main():
    ap = argparse.ArgumentParser(description='SMS-DELIVER PDU 编码器 / 测试向量生成器')
    ap.add_argument('--emit-c-header', metavar='PATH',
                    help='把测试向量写成 C 头文件')
    ap.add_argument('--dump', action='store_true',
                    help='打印全部测试向量及其 PDU')
    ap.add_argument('--self-check', action='store_true',
                    help='只运行编码器自检')
    ap.add_argument('--number', default=None, help='直接编码: 发件号码')
    ap.add_argument('--ton', type=int, default=1, help='直接编码: TON')
    ap.add_argument('--sca', default=None, help='直接编码: 短信中心号码')
    ap.add_argument('--dcs', type=lambda x: int(x, 0), default=0x00,
                    help='直接编码: DCS（如 0x08）')
    ap.add_argument('--text', default=None, help='直接编码: 正文')
    ap.add_argument('--ud', default=None, help='直接编码: 8bit 数据（hex）')
    ap.add_argument('--concat', default=None,
                    help='直接编码: 级联参数 ref,total,seq[,16]')
    args = ap.parse_args()

    errs = self_check()
    for e in errs:
        print('SELF-CHECK FAIL: %s' % e, file=sys.stderr)
    if errs and not args.emit_c_header:
        return 2
    if args.self_check:
        print('自检: %s' % ('通过' if not errs else '失败'))
        return 0 if not errs else 1

    # 直接编码模式
    if args.number is not None and (args.text is not None or args.ud is not None):
        udh = None
        if args.concat:
            parts = [int(x, 0) for x in args.concat.split(',')]
            if len(parts) == 4 and parts[3]:
                udh = encode_udh([udh_cat16(parts[0], parts[1], parts[2])])
            else:
                udh = encode_udh([udh_cat8(parts[0], parts[1], parts[2])])
        print(build_deliver(args.text, number=args.number, ton=args.ton,
                            sca=args.sca, dcs=args.dcs, udh=udh,
                            ud=bytes.fromhex(args.ud) if args.ud else None))
        return 0

    vectors, _notes = build_vectors()

    if args.dump:
        for v in vectors:
            print('%-26s %s' % (v['name'], v['pdu']))
        print('共 %d 条' % len(vectors))

    if args.emit_c_header:
        out = args.emit_c_header
        d = os.path.dirname(os.path.abspath(out))
        if d and not os.path.isdir(d):
            os.makedirs(d, exist_ok=True)
        write_c_header(out, vectors)
        print('已生成 %s（%d 条向量）' % (out, len(vectors)))

    return 0


if __name__ == '__main__':
    sys.exit(main())
