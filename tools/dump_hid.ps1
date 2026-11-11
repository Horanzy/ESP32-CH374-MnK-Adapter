# Dump HID report descriptors / caps of present Logitech (046D) devices
$cs = @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class HidDump2 {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Auto)]
    struct SP_DEVICE_INTERFACE_DATA { public int cbSize; public Guid interfaceClassGuid; public uint flags; public UIntPtr reserved; }

    [DllImport("setupapi.dll", CharSet = CharSet.Auto)]
    static extern IntPtr SetupDiGetClassDevs(ref Guid classGuid, IntPtr enumerator, IntPtr hwndParent, uint flags);
    [DllImport("setupapi.dll", CharSet = CharSet.Auto)]
    static extern bool SetupDiEnumDeviceInterfaces(IntPtr deviceInfoSet, IntPtr deviceInfoData, ref Guid interfaceClassGuid, uint memberIndex, ref SP_DEVICE_INTERFACE_DATA deviceInterfaceData);
    [DllImport("setupapi.dll", CharSet = CharSet.Auto)]
    static extern bool SetupDiGetDeviceInterfaceDetail(IntPtr deviceInfoSet, ref SP_DEVICE_INTERFACE_DATA deviceInterfaceData, IntPtr deviceInterfaceDetailData, uint deviceInterfaceDetailSize, out uint requiredSize, IntPtr deviceInfoData);
    [DllImport("setupapi.dll")]
    static extern bool SetupDiDestroyDeviceInfoList(IntPtr deviceInfoSet);
    [DllImport("kernel32.dll", CharSet = CharSet.Auto, SetLastError = true)]
    static extern IntPtr CreateFile(string lpFileName, uint dwDesiredAccess, uint dwShareMode, IntPtr lpSecurityAttributes, uint dwCreationDisposition, uint dwFlagsAndAttributes, IntPtr hTemplateFile);
    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr hObject);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(IntPtr hDevice, uint ioControlCode, byte[] inBuffer, uint inBufferSize, byte[] outBuffer, uint outBufferSize, out uint bytesReturned, IntPtr overlapped);
    [DllImport("hid.dll")]
    static extern bool HidD_GetPreparsedData(IntPtr hDevice, out IntPtr preparsedData);
    [DllImport("hid.dll")]
    static extern bool HidD_FreePreparsedData(IntPtr preparsedData);
    [DllImport("hid.dll")]
    static extern int HidP_GetCaps(IntPtr preparsedData, byte[] caps);
    [DllImport("hid.dll")]
    static extern int HidP_GetValueCaps(short reportType, byte[] valueCaps, ref ushort capsLength, IntPtr preparsedData);
    [DllImport("hid.dll")]
    static extern int HidP_GetButtonCaps(short reportType, byte[] buttonCaps, ref ushort capsLength, IntPtr preparsedData);
    [DllImport("hid.dll")]
    static extern bool HidD_GetAttributes(IntPtr hDevice, ref HIDD_ATTRIBUTES attr);

    [StructLayout(LayoutKind.Sequential)]
    public struct HIDD_ATTRIBUTES { public int Size; public ushort VendorID, ProductID; public ushort VersionNumber; }

    const uint DIGCF_PRESENT = 0x2;
    const uint DIGCF_DEVICEINTERFACE = 0x10;

    public static List<string> EnumHidPaths(string contains) {
        Guid hidGuid = new Guid("4D1E55B2-F16F-11CF-88CB-001111000030");
        IntPtr devInfo = SetupDiGetClassDevs(ref hidGuid, IntPtr.Zero, IntPtr.Zero, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        var result = new List<string>();
        if (devInfo == new IntPtr(-1)) return result;
        var did = new SP_DEVICE_INTERFACE_DATA();
        did.cbSize = Marshal.SizeOf(typeof(SP_DEVICE_INTERFACE_DATA));
        uint idx = 0;
        while (SetupDiEnumDeviceInterfaces(devInfo, IntPtr.Zero, ref hidGuid, idx, ref did)) {
            idx++;
            uint needed;
            SetupDiGetDeviceInterfaceDetail(devInfo, ref did, IntPtr.Zero, 0, out needed, IntPtr.Zero);
            IntPtr detail = Marshal.AllocHGlobal((int)needed);
            Marshal.WriteInt32(detail, IntPtr.Size == 8 ? 8 : 6);
            if (SetupDiGetDeviceInterfaceDetail(devInfo, ref did, detail, needed, out needed, IntPtr.Zero)) {
                string path = null;
                foreach (int off in new int[] { 4, 6, 8 }) {
                    string cand = Marshal.PtrToStringUni(new IntPtr(detail.ToInt64() + off));
                    if (cand != null && cand.StartsWith("\\\\?\\")) { path = cand; break; }
                }
                if (path != null && (contains == null || path.ToLowerInvariant().Contains(contains))) result.Add(path);
            }
            Marshal.FreeHGlobal(detail);
        }
        SetupDiDestroyDeviceInfoList(devInfo);
        return result;
    }

    static IntPtr TryOpen(string path) {
        // 依次尝试: 0(查询) / 读 / 读写，共享全部放开
        uint[] accesses = { 0, 0x80000000, 0xC0000000 };
        foreach (uint acc in accesses) {
            IntPtr h = CreateFile(path, acc, 0x3, IntPtr.Zero, 3, 0, IntPtr.Zero);
            if (h != new IntPtr(-1)) return h;
        }
        return new IntPtr(-1);
    }

    static ushort U16(byte[] b, int o) { return (ushort)(b[o] | (b[o + 1] << 8)); }

    static string DumpValueCaps(IntPtr prep, short repType, string name) {
        var sb = new StringBuilder();
        ushort n = 64;
        var buf = new byte[36 * 64];
        int st = HidP_GetValueCaps(repType, buf, ref n, prep);
        sb.AppendLine("  " + name + " (" + n + ", status " + st + "):");
        for (int i = 0; i < n && i < 64; i++) {
            int o = i * 36;
            bool isRange = buf[o + 12] != 0;
            sb.Append("    ReportID=0x" + buf[o + 2].ToString("X2") +
                      " UsagePage=0x" + U16(buf, o).ToString("X4") +
                      " BitField=0x" + U16(buf, o + 4).ToString("X4") +
                      " LinkUsage=0x" + U16(buf, o + 8).ToString("X4"));
            if (isRange) sb.Append(" Range 0x" + U16(buf, o + 20).ToString("X4") + "-0x" + U16(buf, o + 22).ToString("X4"));
            else sb.Append(" Usage=0x" + U16(buf, o + 20).ToString("X4"));
            sb.Append(" Absolute=" + buf[o + 15]);
            sb.AppendLine();
        }
        return sb.ToString();
    }

    static string DumpButtonCaps(IntPtr prep, short repType, string name) {
        var sb = new StringBuilder();
        ushort n = 64;
        var buf = new byte[36 * 64];
        int st = HidP_GetButtonCaps(repType, buf, ref n, prep);
        sb.AppendLine("  " + name + " (" + n + ", status " + st + "):");
        for (int i = 0; i < n && i < 64; i++) {
            int o = i * 36;
            bool isRange = buf[o + 12] != 0;
            sb.Append("    ReportID=0x" + buf[o + 2].ToString("X2") +
                      " UsagePage=0x" + U16(buf, o).ToString("X4") +
                      " BitField=0x" + U16(buf, o + 4).ToString("X4") +
                      " LinkUsage=0x" + U16(buf, o + 8).ToString("X4"));
            if (isRange) sb.Append(" Range 0x" + U16(buf, o + 20).ToString("X4") + "-0x" + U16(buf, o + 22).ToString("X4"));
            else sb.Append(" Usage=0x" + U16(buf, o + 20).ToString("X4"));
            sb.AppendLine();
        }
        return sb.ToString();
    }

    public static string Dump(string path) {
        var sb = new StringBuilder();
        IntPtr h = TryOpen(path);
        if (h == new IntPtr(-1)) return "  <open failed>";
        try {
            var attr = new HIDD_ATTRIBUTES(); attr.Size = Marshal.SizeOf(typeof(HIDD_ATTRIBUTES));
            if (HidD_GetAttributes(h, ref attr))
                sb.AppendLine("  VID=0x" + attr.VendorID.ToString("X4") + " PID=0x" + attr.ProductID.ToString("X4") + " Ver=0x" + attr.VersionNumber.ToString("X4"));

            // 1) 原始报告描述符：尝试几个候选 IOCTL
            uint[] ioctls = { 0xB0199, 0xB000D, 0xB0009, 0xB0019 };
            var buf = new byte[8192];
            foreach (uint code in ioctls) {
                uint ret;
                if (DeviceIoControl(h, code, null, 0, buf, (uint)buf.Length, out ret, IntPtr.Zero) && ret > 0) {
                    var hex = new StringBuilder();
                    for (int i = 0; i < ret; i++) { hex.Append(buf[i].ToString("X2")); hex.Append(' '); }
                    sb.AppendLine("  ReportDescriptor via 0x" + code.ToString("X") + " (" + ret + " bytes):");
                    sb.AppendLine("  " + hex.ToString());
                    break;
                }
            }

            // 2) 结构化 caps
            IntPtr prep;
            if (HidD_GetPreparsedData(h, out prep)) {
                var caps = new byte[66];
                if (HidP_GetCaps(prep, caps) == 0) {
                    sb.AppendLine("  UsagePage=0x" + U16(caps, 2).ToString("X4") + " Usage=0x" + U16(caps, 0).ToString("X4"));
                    sb.AppendLine("  InputReportByteLength=" + U16(caps, 4) + " OutputReportByteLength=" + U16(caps, 6) + " FeatureReportByteLength=" + U16(caps, 8));
                }
                sb.Append(DumpButtonCaps(prep, 0, "InputButtonCaps"));
                sb.Append(DumpValueCaps(prep, 0, "InputValueCaps"));
                sb.Append(DumpValueCaps(prep, 1, "OutputValueCaps"));
                sb.Append(DumpValueCaps(prep, 2, "FeatureValueCaps"));
                HidD_FreePreparsedData(prep);
            }
            return sb.ToString();
        } finally { CloseHandle(h); }
    }
}
'@
Add-Type -TypeDefinition $cs -Language CSharp

Write-Output "=== HID interfaces with vid_046d ==="
$paths = [HidDump2]::EnumHidPaths("vid_046d")
foreach ($p in $paths) {
    Write-Output "PATH: $p"
    try { Write-Output ([HidDump2]::Dump($p)) } catch { Write-Output "  <error: $_>" }
    Write-Output ""
}
