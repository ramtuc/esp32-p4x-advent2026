"""Windows で、つながっている USB 機器の速さ（Low / Full / High）を表示する。

USBView と同じように、機器がつながっているハブに
IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX を送り、Speed を読む。

使い方: python usb_link_speed.py [VID:PID ...]   （省略時は 303A: の機器すべて）
"""
import ctypes
import json
import re
import subprocess
import sys
from ctypes import wintypes

SPEED = {0: "Low-Speed", 1: "Full-Speed", 2: "High-Speed", 3: "SuperSpeed"}
IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX = 0x220448
GUID_DEVINTERFACE_USB_HUB = "{f18a0e88-c30c-11d0-8815-00a0c906bed8}"

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.CreateFileW.restype = wintypes.HANDLE
k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                            wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
k32.DeviceIoControl.argtypes = [wintypes.HANDLE, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD,
                                ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
INVALID = wintypes.HANDLE(-1).value


def list_devices(prefixes):
    ps = (
        "[Console]::OutputEncoding = [Text.Encoding]::UTF8; "
        "Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match '^USB\\\\VID_[0-9A-F]{4}&PID_[0-9A-F]{4}\\\\' } | "
        "ForEach-Object { $p = Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName "
        "DEVPKEY_Device_LocationInfo,DEVPKEY_Device_Parent,DEVPKEY_Device_BusReportedDeviceDesc; "
        "[pscustomobject]@{Id=$_.InstanceId; Name=$_.FriendlyName; Loc=$p[0].Data; Parent=$p[1].Data; Desc=$p[2].Data} } "
        "| ConvertTo-Json -Compress"
    )
    out = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True, encoding="utf-8").stdout
    items = json.loads(out) if out.strip() else []
    if isinstance(items, dict):
        items = [items]
    res = []
    for d in items:
        m = re.match(r"USB\\VID_([0-9A-F]{4})&PID_([0-9A-F]{4})", d["Id"], re.I)
        vidpid = f"{m.group(1)}:{m.group(2)}".upper()
        if any(vidpid.startswith(p.upper()) for p in prefixes):
            res.append((vidpid, d))
    return res


def port_speed(parent_id, port):
    path = "\\\\?\\" + parent_id.replace("\\", "#") + "#" + GUID_DEVINTERFACE_USB_HUB
    h = k32.CreateFileW(path, 0x40000000, 0x2, None, 3, 0, None)  # GENERIC_WRITE, FILE_SHARE_WRITE, OPEN_EXISTING
    if h == INVALID:
        raise OSError(ctypes.get_last_error(), f"cannot open hub {path}")
    try:
        buf = ctypes.create_string_buffer(512)
        ctypes.memmove(buf, ctypes.byref(ctypes.c_ulong(port)), 4)  # ConnectionIndex
        n = wintypes.DWORD()
        if not k32.DeviceIoControl(h, IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX, buf, 512, buf, 512,
                                   ctypes.byref(n), None):
            raise OSError(ctypes.get_last_error(), "DeviceIoControl failed")
        raw = buf.raw
        # USB_NODE_CONNECTION_INFORMATION_EX（1 バイト詰め）: ConnectionIndex(4) DeviceDescriptor(18)
        # CurrentConfigurationValue(1) Speed(1) DeviceIsHub(1) DeviceAddress(2) ...
        bcd_usb = raw[4 + 2] | (raw[4 + 3] << 8)
        addr = raw[25] | (raw[26] << 8)  # USBPcapCMD の --devices に渡す番号と同じ
        return raw[23], bcd_usb, addr
    finally:
        k32.CloseHandle(h)


def main():
    prefixes = sys.argv[1:] or ["303A:"]
    devs = list_devices(prefixes)
    if not devs:
        print("no device matches", prefixes)
        return 1
    for vidpid, d in devs:
        m = re.search(r"Port_#(\d+)", d["Loc"] or "")
        if not m:
            print(vidpid, d["Name"], "no port number:", d["Loc"])
            continue
        speed, bcd, addr = port_speed(d["Parent"], int(m.group(1)))
        parent = d["Parent"] or ""
        # 親が ROOT_HUB なら PC のコントローラの口に直接。USB\VID_... ならハブを通っている
        where = "direct (root hub)" if "ROOT_HUB" in parent.upper() else f"via hub {parent.split(chr(92))[1] if chr(92) in parent else parent}"
        print(f"{vidpid}  {SPEED.get(speed, speed):<11} bcdUSB={bcd:04x}  addr={addr}  {d['Desc'] or d['Name']}  ({d['Loc']}, {where})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
