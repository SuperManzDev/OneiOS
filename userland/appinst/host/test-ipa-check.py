#!/usr/bin/env python3
import importlib.util
import io
import plistlib
import struct
import tempfile
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("ipa_check", HERE / "ipa-check.py")
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
checks = failures = 0

def check(ok, why):
    global checks, failures
    checks += 1
    if not ok:
        failures += 1
        print("FAIL", why)

def macho(cryptid):
    # little-endian mach_header_64 + one LC_ENCRYPTION_INFO_64
    header = struct.pack("<IiiIIIII", 0xfeedfacf, 0x0100000c, 2, 2, 1, 24, 0, 0)
    command = struct.pack("<IIIIII", mod.LC_ENCRYPTION_INFO_64, 24, 0x4000, 0x1000, cryptid, 0)
    return header + command

def ipa(path, cryptid=0, unsafe=False):
    info = plistlib.dumps({"CFBundleIdentifier":"org.oneios.fixture", "CFBundleName":"Fixture",
                           "CFBundleExecutable":"Fixture", "MinimumOSVersion":"15.0"})
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("Payload/Fixture.app/Info.plist", info)
        z.writestr("Payload/Fixture.app/Fixture", macho(cryptid))
        if unsafe:
            z.writestr("../escape", b"no")

with tempfile.TemporaryDirectory() as d:
    clear = Path(d) / "clear.ipa"; ipa(clear, 0)
    r = mod.check(clear)
    check(r["bundle"]["CFBundleIdentifier"] == "org.oneios.fixture", "bundle identity")
    check(len(r["machos"]) == 1 and not r["encrypted"], "clear Mach-O accepted")
    check(r["machos"][0]["slices"][0]["arch"] == "arm64", "arm64 named")

    encrypted = Path(d) / "encrypted.ipa"; ipa(encrypted, 1)
    r = mod.check(encrypted)
    check(r["encrypted"] == ["Payload/Fixture.app/Fixture"], "cryptid=1 found")

    unsafe = Path(d) / "unsafe.ipa"; ipa(unsafe, 0, True)
    try:
        mod.check(unsafe)
        check(False, "zip-slip refused")
    except ValueError as e:
        check("unsafe ZIP path" in str(e), "zip-slip reason")

    cut = macho(0)[:-3]
    try:
        mod.slices(cut)
        check(False, "cut command refused")
    except ValueError:
        check(True, "cut command refused")

print(f"ipa-check: {checks} checks, {failures} failed")
raise SystemExit(bool(failures))
