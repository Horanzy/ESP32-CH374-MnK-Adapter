# 解析 USBPcap 抓包：提取控制传输(描述符)与中断 IN 数据
import struct, sys

PCAP = r"C:\Program Files\USBPcap\123.pcap"

data = open(PCAP, "rb").read()
magic = data[:4]
assert magic in (b"\xd4\xc3\xb2\xa1", b"\xa1\xb2\xc3\xd4"), magic
endian = "<" if magic == b"\xd4\xc3\xb2\xa1" else ">"
_, _, _, _, _, snaplen, linktype = struct.unpack(endian + "IHHiIII", data[:24])
assert linktype == 249, f"linktype={linktype}"

off = 24
pkts = []
while off + 16 <= len(data):
    ts_sec, ts_usec, incl_len, orig_len = struct.unpack(endian + "IIII", data[off:off+16])
    off += 16
    pkts.append(data[off:off+incl_len])
    off += incl_len

CTRL_SETUP, CTRL_DATA, CTRL_STATUS = 0, 1, 2

# USBPcap 包头(打包后 27 字节): headerLen(2) irpId(8) status(4) function(2) info(1) bus(2) device(2) endpoint(1) transfer(1) dataLength(4)
# 控制传输在偏移 27 处多一个 stage 字节(headerLen=28)，数据从 28 开始；中断/批量数据从 27 开始
def parse_usbpcap(p):
    if len(p) < 27: return None
    hlen, = struct.unpack_from("<H", p, 0)
    irp, status = struct.unpack_from("<QI", p, 2)
    fn, info, bus, dev, ep, xfer, dlen = struct.unpack_from("<HBHHBBI", p, 14)
    if xfer == 0x02:  # control
        stage = p[27]
        data = p[28:]
        setup = None
        if stage == 0 and dlen == 8 and len(data) >= 8:
            bm, br, wv, wi, wl = struct.unpack_from("<BBHHH", data, 0)
            setup = (bm, br, wv, wi, wl)
            data = b""
        return dict(irp=irp, bus=bus, dev=dev, ep=ep, xfer=xfer, stage=stage, dlen=dlen, setup=setup, data=data if dlen > 0 and stage != 0 else b"")
    return dict(irp=irp, bus=bus, dev=dev, ep=ep, xfer=xfer, stage=None, dlen=dlen, setup=None, data=p[27:])

# irpId 关联 setup 与 data 阶段
ctrl = {}   # irp -> dict(setup=..., datas=[...])
intr = {}   # (bus,dev,ep) -> [data...]

for i, p in enumerate(pkts):
    r = parse_usbpcap(p)
    if not r: continue
    if r["xfer"] == 0x02:
        e = ctrl.setdefault(r["irp"], dict(setup=None, datas=[], frames=[]))
        e["frames"].append(i)
        if r["setup"]: e["setup"] = r["setup"]
        elif r["data"]: e["datas"].append(r["data"])
    elif r["xfer"] == 0x01:  # interrupt
        if r["ep"] & 0x80 and r["data"]:
            intr.setdefault((r["bus"], r["dev"], r["ep"]), []).append((i, r["data"]))

def desc_name(bm, br, wv):
    if br == 6:
        t = wv >> 8
        return {1: "DEVICE", 2: "CONFIG", 3: "STRING", 0x0F: "BOS", 0x22: "REPORT(HID)", 0x23: "PHYS(HID)"}.get(t, hex(t))
    return {5: "SET_ADDRESS", 9: "SET_CONFIG", 0x0B: "SET_PROTOCOL", 0x0A: "SET_IDLE"}.get(br, hex(br))

print("=== 控制传输 ===")
for irp, e in sorted(ctrl.items(), key=lambda kv: kv[1]["frames"][0]):
    if not e["setup"]: continue
    bm, br, wv, wi, wl = e["setup"]
    resp = b"".join(e["datas"])
    print(f"frame~{e['frames'][0]:4d} dev? bm=0x{bm:02X} req={desc_name(bm,br,wv):12s} wValue=0x{wv:04X} wIndex=0x{wi:04X} wLen={wl} got={len(resp)}")

print()
print("=== 中断 IN 端点 ===")
for k, v in sorted(intr.items()):
    bus, dev, ep = k
    print(f"bus{bus} dev{dev} EP 0x{ep:02X}: {len(v)} packets; 前 8 个: ", [f"{i}:{d.hex(' ')[:60]}" for i, d in v[:8]])

# 导出描述符供后续解析
import json, base64
out = {"configs": [], "reports": [], "devices": []}
for irp, e in sorted(ctrl.items(), key=lambda kv: kv[1]["frames"][0]):
    if not e["setup"]: continue
    bm, br, wv, wi, wl = e["setup"]
    resp = b"".join(e["datas"])
    if br == 6 and (wv >> 8) == 1 and len(resp) >= 12:
        vid, pid = struct.unpack_from("<HH", resp, 8)
        out["devices"].append(dict(vid=vid, pid=pid, bcd=struct.unpack_from("<H", resp, 2)[0]))
    if br == 6 and (wv >> 8) == 2 and len(resp) >= 9:
        out["configs"].append(dict(index=wv & 0xFF, iface=wi, data=base64.b64encode(resp).decode()))
    if br == 6 and (wv >> 8) == 0x22:
        out["reports"].append(dict(iface=wi, index=wv & 0xFF, data=base64.b64encode(resp).decode()))
json.dump(out, open(r"C:\Users\i1761\Documents\ESP32-CH374-MnK-Adapter\tools\pcap_desc.json", "w"))
print()
print("devices:", [(hex(d['vid']), hex(d['pid']), hex(d['bcd'])) for d in out["devices"]])
print("configs:", [(c['index'], c['iface'], len(base64.b64decode(c['data']))) for c in out["configs"]])
print("report descs:", [(r['iface'], r['index'], len(base64.b64decode(r['data']))) for r in out["reports"]])
