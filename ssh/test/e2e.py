import pexpect, sys, os
env = dict(os.environ, PSI_HOST="127.0.0.1", PSI_PORT="2222", PSI_USER="psitest", PSI_HOME="/tmp/psihome")
c = pexpect.spawn("./psissh-host", env=env, timeout=40, encoding="latin-1")
c.logfile_read = sys.stdout
i = c.expect(["continue connecting", "assword", pexpect.EOF, pexpect.TIMEOUT])
if i == 0:
    c.send("y")
    i = c.expect(["assword", pexpect.EOF, pexpect.TIMEOUT])
    i = 1 if i == 0 else 9
if i != 1:
    print("\n### FAILED before password prompt"); sys.exit(1)
c.send(os.environ["PSI_TEST_PASS"] + "\r")
c.expect(r"\$ ")
c.send("echo PSION-$((6*7)); tput cols; tput lines\r")
c.expect("PSION-42")
c.expect(r"\$ ")
c.send("exit\r")
c.expect(pexpect.EOF)
print("\n### END-TO-END OK")
