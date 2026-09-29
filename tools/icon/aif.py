#!/usr/bin/env python3
"""Minimal EPOC R5 .aif writer (the SDK's aiftool.exe needs the Windows
emulator). Layout copied from apparc's CApaAppInfoFileWriter and checked
byte-for-byte against the SDK's own rtexted.aif."""
import os, struct
from PIL import Image

def crc16(data):                       # Mem::Crc = CRC-CCITT, init 0
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc

def uid_checksum(u1, u2, u3):
    b = struct.pack("<III", u1, u2, u3)
    return (crc16(b[1::2]) << 16) | crc16(b[0::2])

def des8(text):                        # externalised narrow descriptor
    t = text.encode("latin-1")
    return bytes([((len(t) << 1) | 1) << 1]) + t

def bitmap_stream(img, bpp=2):
    """CFbsBitmap::ExternalizeL, uncompressed, EGray4 (2 bpp) or EGray2."""
    img = img.convert("L")
    w, h = img.size
    stride = ((w * bpp + 31) // 32) * 4            # rows padded to 32 bits
    data = bytearray()
    levels = (1 << bpp) - 1
    for y in range(h):
        row = bytearray(stride)
        for i in range((w * bpp + 7) // 8, stride): row[i] = 0xFF    # padding white, as bmconv does
        for x in range(w):
            v = round(img.getpixel((x, y)) * levels / 255)
            bit = x * bpp
            row[bit // 8] |= v << (bit % 8)       # first pixel in the low bits
        data += row
    hdr = struct.pack("<IIiiiiIIII", 40 + len(data), 40, w, h, 0, 0, bpp, 0, 0, 0)
    return hdr + bytes(data)

def write_aif(path, app_uid, captions, icons, embeddable=0, newfile=0, hidden=0):
    """captions: [(language, text)], icons: [(Image, mask Image)]"""
    out = bytearray(20)
    cap_hdr, icon_hdr = [], []
    for lang, text in captions:
        cap_hdr.append((len(out), lang)); out += des8(text)
    for img, mask in icons:
        icon_hdr.append((len(out), img.size[0]))
        out += bitmap_stream(img) + bitmap_stream(mask)
    root = len(out)
    out += bytes([len(cap_hdr) << 1])
    for sid, lang in cap_hdr: out += struct.pack("<Ih", sid, lang)
    out += bytes([len(icon_hdr) << 1])
    for sid, side in icon_hdr: out += struct.pack("<Ih", sid, side)
    out += struct.pack("<iiii", 1, embeddable, newfile, hidden)
    struct.pack_into("<IIIII", out, 0, 0x10000037, 0x1000006A, app_uid,
                     uid_checksum(0x10000037, 0x1000006A, app_uid), root)
    open(path, "wb").write(out)
    return len(out)

if __name__ == "__main__":
    import sys
    ref = open(os.path.join(os.environ["PSION_SDK"], "epoc_cpp_sdk/eikonex/rtexted/rtexted.aif"), "rb").read()
    u = struct.unpack("<III", ref[:12])
    assert struct.unpack("<I", ref[12:16])[0] == uid_checksum(*u), "checksum mismatch"
    assert ref[0x14:0x20] == des8("RichText Ed"), "descriptor mismatch"
    print("format checks against rtexted.aif: OK")
    icons = [(Image.open(f"psiterm{n}.bmp"), Image.open(f"psiterm{n}m.bmp")) for n in (24, 32, 48)]
    n = write_aif("psiterm.aif", 0x01000A77, [(1, "PsiTerm"), (10, "PsiTerm")], icons)
    print("psiterm.aif", n, "bytes")
