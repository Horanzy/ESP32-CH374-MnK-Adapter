# 深入分析：顺序配对控制传输 + 解码 HID 报告描述符 + 统计 EP82 报告 ID
import struct, base64, json

PCAP = r"C:\Program Files\USBPcap\123.pcap"
data = open(PCAP, "rb").read()
endian = "<" if data[:4] == b"\xd4\xc3\xb2\xa1" else ">"
off = 24
pkts = []
while off + 16 <= len(data):
    _, _, incl_len, _ = struct.unpack(endian + "IIII", data[off:off+16])
    off += 16
    pkts.append(data[off:off+incl_len]); off += incl_len

def parse(p):
    hlen, = struct.unpack_from("<H", p, 0)
    fn, info, bus, dev, ep, xfer, dlen = struct.unpack_from("<HBHHBBI", p, 14)
    if xfer == 0x02:
        stage = p[27]
        if stage == 0 and dlen == 8:
            bm, br, wv, wi, wl = struct.unpack_from("<BBHHH", p, 28)
            return ("SETUP", bus, dev, ep, (bm, br, wv, wi, wl), b"")
        return ("DATA" if dlen > 0 else "STATUS", bus, dev, ep, None, p[28:28+dlen])
    return ("XFER", bus, dev, ep, None, p[27:27+dlen])

# 顺序配对: SETUP 后同方向的下一个 DATA 归属它
pending = {}
results = []   # (bus,dev,setup,data)
stats = {}
for i, p in enumerate(pkts):
    kind, bus, dev, ep, setup, payload = parse(p)
    if kind == "SETUP":
        pending[(bus, dev)] = (setup, i)
    elif kind == "DATA" and payload:
        if (bus, dev) in pending:
            s, si = pending.pop((bus, dev))
            results.append((bus, dev, s, payload, si))
    elif kind == "XFER":
        if ep & 0x80:
            key = (bus, dev, ep)
            stats.setdefault(key, []).append(payload)

print("=== 配对后的描述符请求 ===")
reports = {}
for bus, dev, s, payload, si in results:
    bm, br, wv, wi, wl = s
    name = {1: "DEVICE", 2: "CONFIG", 3: "STRING", 0x22: "REPORT(HID)"}.get(wv >> 8, hex(wv >> 8)) if br == 6 else hex(br)
    print(f"bus{bus} dev{dev} {name:12s} wValue=0x{wv:04X} wIndex=0x{wi:04X} wLen={wl:4d} got={len(payload)}")
    if br == 6 and (wv >> 8) == 0x22:
        reports[wi] = payload
    if br == 6 and (wv >> 8) == 1 and len(payload) >= 12:
        vid, pid = struct.unpack_from("<HH", payload, 8)
        print(f"   -> VID=0x{vid:04X} PID=0x{pid:04X}")
    if br == 6 and (wv >> 8) == 2:
        print(f"   -> {payload.hex(' ')}")

print()
print("=== EP82 报告 ID 分布 ===")
for key, arr in stats.items():
    from collections import Counter
    c = Counter(d[0] for d in arr if d)
    print(f"bus{key[0]} dev{key[1]} EP 0x{key[2]:02X}: {len(arr)} 包, 首字节分布: {dict(c)}")

# HID 报告描述符解码器
USAGE_PAGES = {0x01: "GenericDesktop", 0x09: "Button", 0x0C: "Consumer", 0xFF00: "Vendor", 0xFFBC: "VendorBC"}
GD_USAGES = {0x02: "Mouse", 0x05: "GamePad", 0x06: "Keyboard", 0x09: "Pointer", 0x30: "X", 0x31: "Y",
             0x32: "Z", 0x38: "Wheel", 0x3A: "Motion?wakeup", 0x80: "SysCtl", 0x81: "SysKbd?", 0x83: "SysWake?",
             0x01: "Pointer?", 0x08: "Reserved"}

def parse_hid(d, title):
    print(f"--- 报告描述符 ({title}, {len(d)} 字节) ---")
    pos, ri = 0, 0
    usage_page = None; usages = []
    depth = 0
    items = []
    # 先收集 items
    while pos < len(d):
        b = d[pos]; pos += 1
        size = {0: 0, 1: 1, 2: 2, 3: 4}[b & 3]
        typ = (b >> 2) & 3; tag = b >> 4
        val = int.from_bytes(d[pos:pos+size], "little") if size else 0
        signed = typ == 1 and tag in (1, 2)  # logical/physical min/max 有符号
        if signed and val >= 1 << (8*size-1): val -= 1 << (8*size)
        pos += size
        items.append((typ, tag, size, val))
    pos = 0; usage_page = 0; usage_stack = []; cur_usages = []
    ri = 0
    for typ, tag, size, val in items:
        tname = {0: "MAIN", 1: "GLOBAL", 2: "LOCAL"}.get(typ)
        if typ == 1:  # global
            gname = {0: "UsagePage", 1: "LogicalMin", 2: "LogicalMax", 3: "PhysMin", 4: "PhysMax",
                     5: "UnitExp", 6: "Unit", 7: "ReportSize", 8: "ReportID", 9: "ReportCount",
                     10: "Push", 11: "Pop"}.get(tag, hex(tag))
            if tag == 0: usage_page = val
            if tag == 8: ri = val
            print(f"  {tname} {gname} = {val} (0x{val & 0xFFFF:X})")
        elif typ == 2:  # local
            lname = {0: "Usage", 1: "UsageMin", 2: "UsageMax"}.get(tag, hex(tag))
            print(f"  {tname} {lname} = 0x{val:04X}")
        else:  # main
            mname = {8: "INPUT", 9: "OUTPUT", 11: "FEATURE", 10: "COLLECTION", 12: "END_COLLECTION"}.get(tag, hex(tag))
            flags = ""
            if tag in (8, 9, 11):
                flags = "".join(c for bit, c in [(0,"Data"),(1,"Const"),(2,"Array"),(3,"Var"),(4,"Rel"),(5,"Abs")] if val & (1 << bit))
                if val & (1 << 1): flags = "Const"
                print(f"  MAIN {mname} flags=0x{val:02X} ({flags})  <-- 报告ID {ri}")
            elif tag == 10:
                print(f"  MAIN COLLECTION 0x{val:02X}")
            else:
                print(f"  MAIN END_COLLECTION")
    print()

for iface, rd in sorted(reports.items()):
    parse_hid(rd, f"interface {iface}")
    print("  RAW:", rd.hex(" "))
    print()

json.dump({str(k): v.hex() for k, v in reports.items()}, open(r"C:\Users\i1761\Documents\ESP32-CH374-MnK-Adapter\tools\report_descs.json", "w"))
