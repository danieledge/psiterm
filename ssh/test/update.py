#!/usr/bin/env python3
"""End-to-end test of psissh's update mode (mode 2) on Linux, against a real
HTTP server, both through the fake Hayes modem and over a direct socket."""
import subprocess, os, filecmp
# the host build trusts test/update-test.key: sign the test release with it
HERE = os.path.dirname(os.path.abspath(__file__))
def sign(folder, version):
    subprocess.run(["python3", os.path.join(HERE, "..", "..", "tools", "release", "sign.py") if os.path.exists(os.path.join(HERE, "..", "..", "tools", "release", "sign.py")) else "tools/release/sign.py",
                    os.path.join(folder, "PsiTerm.sis"), version, "--key", os.path.join(HERE, "update-test.key")], check=True, capture_output=True)
sign("/tmp/updsrv/psion", open("/tmp/updsrv/psion/version.txt").read().strip())

def run(env_extra):
    env = dict(os.environ, PSI_HOST="127.0.0.1", PSI_PORT="8775", PSI_HOME="/tmp/psihome",
               PSI_UPDATE="/psion/", PSI_SAVE="/tmp/psiupd.sis")
    env.update(env_extra)
    if os.path.exists("/tmp/psiupd.sis"):
        os.remove("/tmp/psiupd.sis")
    p = subprocess.run(["./psissh-host"], env=env, stdin=subprocess.DEVNULL, capture_output=True, timeout=120)
    return p.returncode, p.stdout.decode("latin-1")

for net in ("", "1"):
    tag = "psion-net" if net else "modem"
    e = {"PSI_NET": "1"} if net else {}
    rc, out = run(dict(e, PSI_VERSION="0.12"))
    ok = rc == 10 and filecmp.cmp("/tmp/psiupd.sis", "/tmp/updsrv/psion/PsiTerm.sis", shallow=False)
    print(f"{tag}: older version downloads the new SIS intact: {'PASS' if ok else 'FAIL'} (rc={rc})")
    if not ok:
        print(out[-600:])
    rc, out = run(dict(e, PSI_VERSION="0.30"))
    print(f"{tag}: current version says up to date: {'PASS' if rc == 0 and 'latest version' in out else 'FAIL'}")
    rc, out = run(dict(e, PSI_VERSION="0.12", PSI_UPDATE="/nothere/"))
    print(f"{tag}: missing files reported: {'PASS' if rc == 2 and '404' in out else 'FAIL'}")
