import pexpect, sys, os
def run(pw, expect_prompt):
    env = dict(os.environ, PSI_HOST="127.0.0.1", PSI_PORT="2222", PSI_USER="psitest", PSI_HOME="/tmp/psihome", PSI_PASS=pw)
    c = pexpect.spawn("./psissh-host", env=env, timeout=40, encoding="latin-1")
    c.expect(r"\(saved password\)")
    if expect_prompt:
        c.expect("assword: ?$"); c.send(os.environ["PSI_TEST_PASS"] + "\r")
    c.expect(r"\$ "); c.send("echo OK-$((2*21))\r"); c.expect("OK-42"); c.send("exit\r"); c.expect(pexpect.EOF)
run(os.environ["PSI_TEST_PASS"], False); print("saved password logs straight in: PASS")
run("wrong-one", True); print("wrong saved password falls back to prompt: PASS")
