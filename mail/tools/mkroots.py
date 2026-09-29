#!/usr/bin/env python3
"""mkroots.py - writes mail/engine/roots.h, the root certificates PsiMail
trusts (RSA only), from PEM files.

    mail/tools/mkroots.py mail/tools/roots.pem > mail/engine/roots.h

roots.pem came from macOS's system roots (security find-certificate -p):
ISRG Root X1 (Let's Encrypt: Fastmail), GTS Root R1 (Google), DigiCert,
USERTrust (Sectigo), GlobalSign, Amazon, Microsoft, Go Daddy, Starfield.
"""
import sys
from cryptography import x509
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.hazmat.primitives import serialization

def carr(name, b):
    out = ["static const unsigned char %s[%d] = {" % (name, len(b))]
    for i in range(0, len(b), 16):
        out.append("\t" + "".join("0x%02x," % x for x in b[i:i + 16]))
    out.append("};")
    return "\n".join(out)

certs = []
for path in sys.argv[1:]:
    certs += x509.load_pem_x509_certificates(open(path, "rb").read())

print("/* roots.h - root certificates PsiMail trusts. Made by mail/tools/mkroots.py */")
print("typedef struct { const char *name; const unsigned char *subject; int subjectlen;")
print("                 const unsigned char *n; int nlen; const unsigned char *e; int elen; } Root;")
entries = []
for i, c in enumerate(certs):
    k = c.public_key()
    if not isinstance(k, rsa.RSAPublicKey):
        continue
    nums = k.public_numbers()
    n = nums.n.to_bytes((nums.n.bit_length() + 7) // 8, "big")
    e = nums.e.to_bytes((nums.e.bit_length() + 7) // 8, "big")
    subj = c.subject.public_bytes()
    name = c.subject.rfc4514_string()
    print("/* %s, %d bits, until %s */" % (name, nums.n.bit_length(), c.not_valid_after_utc.date()))
    print(carr("k_subj%d" % i, subj))
    print(carr("k_n%d" % i, n))
    print(carr("k_e%d" % i, e))
    entries.append('\t{ "%s", k_subj%d, %d, k_n%d, %d, k_e%d, %d },' % (name.replace('"', '').replace('\\', ''), i, len(subj), i, len(n), i, len(e)))
print("static const Root k_roots[] = {")
print("\n".join(entries))
print("};")
