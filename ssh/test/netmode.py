import pexpect, sys, os
env = dict(os.environ, PSI_HOST="127.0.0.1", PSI_PORT="2222", PSI_USER="psitest", PSI_HOME="/tmp/psihome", PSI_NET="1")
c = pexpect.spawn("./psissh-host", env=env, timeout=40, encoding="latin-1")
c.expect("over the Psion's Internet connection")
i = c.expect(["continue connecting", "assword"])
if i == 0: c.send("y"); c.expect("assword")
c.send(os.environ["PSI_TEST_PASS"] + "\r"); c.expect(r"\$ "); c.send("seq 1 20000 | tail -1\r"); c.expect("20000"); c.send("exit\r"); c.expect(pexpect.EOF)
print("net mode login: PASS")
env["PSI_HOST"] = "no.such.host"
c = pexpect.spawn("./psissh-host", env=env, timeout=20, encoding="latin-1")
c.expect("Could not connect: could not look up"); c.expect(pexpect.EOF)
print("net mode bad host reported: PASS")
