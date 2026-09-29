#!/usr/bin/env python3
"""Signs a PsiTerm release so the updater will accept it.

    tools/release/sign.py dist/PsiTerm.sis 0.23 [--key ~/.psiterm-signing/release.key]
    tools/release/sign.py --product PsiWeb dist/PsiWeb.sis 0.2

Writes dist/PsiTerm.sis.sig:
    line 1: the version
    line 2: Ed25519 signature (hex) over
            b"PsiTerm update\\n" + version + b"\\n" + SHA-256(PsiTerm.sis)
PsiTerm checks this with the public key in ssh/psishim.c (KUpdateKey) and
refuses any update that does not match. Keep the private key out of git.
"""
import argparse, hashlib, os, sys
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

ap = argparse.ArgumentParser()
ap.add_argument("sis"); ap.add_argument("version")
ap.add_argument("--product", default="PsiTerm", choices=["PsiTerm", "PsiWeb"])
ap.add_argument("--key", default=os.path.expanduser("~/.psiterm-signing/release.key"))
a = ap.parse_args()
key = Ed25519PrivateKey.from_private_bytes(bytes.fromhex(open(a.key).read().strip()))
digest = hashlib.sha256(open(a.sis, "rb").read()).digest()
msg = a.product.encode() + b" update\n" + a.version.encode() + b"\n" + digest
sig = key.sign(msg)
out = a.sis + ".sig"
open(out, "w").write(a.version + "\n" + sig.hex() + "\n")
print("wrote", out)
