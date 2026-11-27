#!/usr/bin/env python3
"""Read-only preflight for an IPA: safe ZIP paths, root app identity, architectures, and FairPlay cryptid.

This never decrypts or rewrites an IPA. Exit 0 means every Mach-O slice found has cryptid=0; exit 2 means an
encrypted slice was found; exit 1 means the archive is malformed or unsupported.
"""
import argparse
import json
import plistlib
import struct
import sys
import zipfile
from pathlib import PurePosixPath

MAGIC = {
    b"\xce\xfa\xed\xfe": ("<", False), b"\xcf\xfa\xed\xfe": ("<", True),
    b"\xfe\xed\xfa\xce": (">", False), b"\xfe\xed\xfa\xcf": (">", True),
}
FAT = {b"\xca\xfe\xba\xbe": (">", False), b"\xbe\xba\xfe\xca": ("<", False),
       b"\xca\xfe\xba\xbf": (">", True), b"\xbf\xba\xfe\xca": ("<", True)}
CPU = {0x0100000C: "arm64", 0x01000007: "x86_64", 12: "arm", 7: "x86"}
LC_ENCRYPTION_INFO = 0x21
LC_ENCRYPTION_INFO_64 = 0x2C


def safe_name(name):
    p = PurePosixPath(name)
    return bool(name) and not name.startswith("/") and ".." not in p.parts and "\\" not in name and "\0" not in name


def thin(data, base=0, limit=None):
    if limit is None:
        limit = len(data) - base
    if base < 0 or limit < 4 or base + limit > len(data):
        raise ValueError("slice outside file")
    key = data[base:base + 4]
    if key not in MAGIC:
        return None
    endian, is64 = MAGIC[key]
    hsize = 32 if is64 else 28
    if limit < hsize:
        raise ValueError("cut Mach-O header")
    cputype, cpusubtype, _, ncmds, sizeofcmds = struct.unpack_from(endian + "iiIII", data, base + 4)
    if ncmds > 65535 or sizeofcmds > limit - hsize:
        raise ValueError("invalid load-command table")
    at, end = base + hsize, base + hsize + sizeofcmds
    cryptids = []
    for _ in range(ncmds):
        if at + 8 > end:
            raise ValueError("cut load command")
        cmd, size = struct.unpack_from(endian + "II", data, at)
        if size < 8 or at + size > end:
            raise ValueError("invalid load command size")
        if cmd in (LC_ENCRYPTION_INFO, LC_ENCRYPTION_INFO_64):
            if size < 20:
                raise ValueError("cut encryption command")
            cryptids.append(struct.unpack_from(endian + "I", data, at + 16)[0])
        at += size
    if at != end:
        raise ValueError("load-command size mismatch")
    return {"arch": CPU.get(cputype & 0xFFFFFFFF, hex(cputype & 0xFFFFFFFF)),
            "cpusubtype": cpusubtype & 0xFFFFFFFF, "cryptids": cryptids, "encrypted": any(cryptids)}


def slices(data):
    direct = thin(data)
    if direct is not None:
        return [direct]
    key = data[:4]
    if key not in FAT:
        return []
    endian, fat64 = FAT[key]
    if len(data) < 8:
        raise ValueError("cut fat header")
    count = struct.unpack_from(endian + "I", data, 4)[0]
    entry = 32 if fat64 else 20
    if not count or count > 64 or 8 + count * entry > len(data):
        raise ValueError("invalid fat slice count")
    out = []
    for i in range(count):
        at = 8 + i * entry
        if fat64:
            _, _, offset, size, _, _ = struct.unpack_from(endian + "iiQQII", data, at)
        else:
            _, _, offset, size, _ = struct.unpack_from(endian + "iiIII", data, at)
        item = thin(data, offset, size)
        if item is None:
            raise ValueError("fat entry is not Mach-O")
        out.append(item)
    return out


def check(path):
    report = {"ipa": str(path), "bundle": None, "machos": [], "encrypted": [], "warnings": []}
    with zipfile.ZipFile(path) as z:
        names = z.namelist()
        bad = [n for n in names if not safe_name(n)]
        if bad:
            raise ValueError("unsafe ZIP path: " + bad[0])
        roots = [n for n in names if n.startswith("Payload/") and n.endswith(".app/Info.plist") and n.count("/") == 2]
        if len(roots) != 1:
            raise ValueError(f"expected one Payload/*.app/Info.plist, found {len(roots)}")
        info = plistlib.loads(z.read(roots[0]))
        root = roots[0].rsplit("/", 1)[0] + "/"
        report["bundle"] = {k: info.get(k) for k in ("CFBundleIdentifier", "CFBundleName", "CFBundleDisplayName",
                                                        "CFBundleExecutable", "MinimumOSVersion")}
        injected = [n for n in names if any(word in n for word in ("CydiaSubstrate.framework", "iGameGod.framework",
                                                                    "SubstrateInjection.dylib", "SubstrateLoader.dylib"))]
        if injected:
            report["warnings"].append({"kind": "injected-jailbreak-components", "paths": injected})
        for name in names:
            if not name.startswith(root) or name.endswith("/"):
                continue
            entry = z.getinfo(name)
            if entry.file_size < 4 or entry.file_size > 512 * 1024 * 1024:
                continue
            with z.open(name) as f:
                head = f.read(4)
            if head not in MAGIC and head not in FAT:
                continue
            data = z.read(name)
            ss = slices(data)
            if not ss:
                continue
            item = {"path": name, "bytes": len(data), "slices": ss}
            report["machos"].append(item)
            if any(s["encrypted"] for s in ss):
                report["encrypted"].append(name)
    if not report["machos"]:
        raise ValueError("no Mach-O files in app bundle")
    return report


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("ipa")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()
    try:
        result = check(args.ipa)
    except (OSError, zipfile.BadZipFile, plistlib.InvalidFileException, ValueError) as e:
        print(f"ipa-check: refused: {e}", file=sys.stderr)
        return 1
    if args.json:
        print(json.dumps(result, indent=2))
    else:
        b = result["bundle"]
        print(f"bundle: {b.get('CFBundleIdentifier')} ({b.get('CFBundleDisplayName') or b.get('CFBundleName')})")
        print(f"Mach-O files: {len(result['machos'])}")
        for item in result["machos"]:
            desc = ", ".join(f"{s['arch']} cryptid={','.join(map(str, s['cryptids'])) if s['cryptids'] else 'none'}" for s in item["slices"])
            print(f"  {item['path']}: {desc}")
        for warning in result["warnings"]:
            if warning["kind"] == "injected-jailbreak-components":
                print(f"WARNING: {len(warning['paths'])} injected jailbreak/cheat component(s) found; review the source before running this app")
                for path in warning["paths"][:8]:
                    print(f"  {path}")
        if result["encrypted"]:
            print("REFUSED: encrypted/FairPlay Mach-O slices remain; appinst never strips DRM")
        else:
            print("PASS: every Mach-O slice found has cryptid=0 (decrypted input)")
    return 2 if result["encrypted"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
