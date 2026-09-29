"""Key login: psissh offers the login key (PSI_HOME/id_ed25519) and the
server lets it in without a password. Needs sshd -p 2222 with the key's
.pub in psitest's authorized_keys, and fakemodem.py."""
import pexpect, sys, os
home = os.environ.get("KEYHOME", "/tmp/kh")
env = dict(os.environ, PSI_HOST="127.0.0.1", PSI_PORT="2222", PSI_USER="psitest", PSI_HOME=home)
env.pop("PSI_PASS", None)
env.setdefault("PSI_KEY", home + "/id_ed25519")
c = pexpect.spawn("./psissh-host", env=env, timeout=60, encoding="latin-1")
c.logfile_read = sys.stdout
while True:
    i = c.expect(["continue connecting", "assword", r"\$ ", pexpect.EOF, pexpect.TIMEOUT])
    if i == 0: c.send("y"); continue
    if i == 2: break
    print("\n### KEY LOGIN FAILED (%s)" % ["", "asked for a password", "", "EOF", "timeout"][i]); sys.exit(1)
c.send("echo KEY-$((6*7))\r")
c.expect("KEY-42")
c.send("exit\r")
c.expect(pexpect.EOF)
print("\n### KEY LOGIN OK")
