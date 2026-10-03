#!/usr/bin/env python3
"""tools/mkcabundle.py - makes src/cabundle.h, the root certificates the
Atom's web proxy checks servers against (WiFiClientSecure::setCACertBundle).

    python3 tools/mkcabundle.py [PEM-FILE] > src/cabundle.h

PEM-FILE defaults to /etc/ssl/certs/ca-certificates.crt (Debian/Ubuntu: the
Mozilla roots trusted for TLS servers). Run it again now and then to follow
changes to the roots, and rebuild the firmware.

The format is the one arduino-esp32 2.0.x's esp_crt_bundle.c reads (as
ESP-IDF's gen_crt_bundle.py writes it): the number of certificates (2 bytes,
big-endian), then for each one its subject name's length and its public
key's length (2 bytes each), the subject (DER) and the public key (DER
SubjectPublicKeyInfo), sorted by subject so the ESP32 can find a server's
issuer by binary search. Only names and keys are kept: about 65 KB of flash
for 150 roots, and no RAM, against 200 KB for the PEM files.
Needs python3-cryptography. MIT licence (see LICENSE at the top of the repository).
"""
import sys
import struct
import warnings
from cryptography import x509
from cryptography.hazmat.primitives import serialization


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else '/etc/ssl/certs/ca-certificates.crt'
    warnings.simplefilter('ignore')    # (an old root's serial number: harmless here)
    certs = x509.load_pem_x509_certificates(open(src, 'rb').read())
    entries = {}
    for c in certs:
        name = c.subject.public_bytes()
        key = c.public_key().public_bytes(serialization.Encoding.DER,
                                          serialization.PublicFormat.SubjectPublicKeyInfo)
        entries.setdefault(name, key)    # (the first of any two with one name)
    out = struct.pack('>H', len(entries))
    for name in sorted(entries):
        key = entries[name]
        out += struct.pack('>HH', len(name), len(key)) + name + key
    w = sys.stdout.write
    w('// cabundle.h - the root certificates the web proxy checks servers against:\n')
    w('// %d roots from %s, as subject names and public keys.\n' % (len(entries), src))
    w('// Made by tools/mkcabundle.py: do not edit (run it again instead).\n')
    w('#ifndef ATOM_CABUNDLE_H\n#define ATOM_CABUNDLE_H\n#include <stdint.h>\n')
    w('static const uint8_t kCaBundle[%d] = {\n' % len(out))
    for i in range(0, len(out), 24):
        w(''.join('%d,' % b for b in out[i:i + 24]) + '\n')
    w('};\n#endif\n')


if __name__ == '__main__':
    main()
