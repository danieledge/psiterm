#!/usr/bin/env python3
"""certtest.py CHAIN.pem OUT - writes the chain as a TLS 1.3 Certificate message body"""
import sys
from cryptography import x509
from cryptography.hazmat.primitives.serialization import Encoding
certs = x509.load_pem_x509_certificates(open(sys.argv[1], "rb").read())
body = b""
for c in certs:
    d = c.public_bytes(Encoding.DER)
    body += len(d).to_bytes(3, "big") + d + b"\0\0"
open(sys.argv[2], "wb").write(b"\0" + len(body).to_bytes(3, "big") + body)
