# A typical Claude Code screen, written as the escape codes Claude Code sends.
W = 106
E = "\x1b["
def rgb(r,g,b): return f"{E}38;2;{r};{g};{b}m"
ACC, DIM, GRN, RST, BOLD = rgb(215,119,87), rgb(153,153,153), rgb(78,186,101), E+"0m", E+"1m"
def box(lines, color=DIM):
    out = color + "╭" + "─"*(W-2) + "╮" + RST + "\r\n"
    import re
    for l, vis in lines:
        vis = len(re.sub(r"\x1b\[[0-9;]*m", "", l))
        out += color + "│" + RST + " " + l + " "*(W-3-vis) + color + "│" + RST + "\r\n"
    return out + color + "╰" + "─"*(W-2) + "╯" + RST + "\r\n"
s = E+"2J"+E+"H"
s += box([(ACC+"✻"+RST+" Welcome to "+BOLD+"Claude Code"+RST+"!", 24), ("",0),
          (DIM+"  /help for help, /status for your current setup"+RST, 48), ("",0),
          (DIM+"  cwd: ~/projects/psiterm"+RST, 34)], ACC)
s += "\r\n" + DIM + "> " + RST + "Why does the Psion reset when the WiRSa enters modem mode?\r\n\r\n"
s += "⏺ I'll check how PsiTerm configures the serial handshake lines.\r\n\r\n"
s += GRN+"⏺"+RST+" "+BOLD+"Read"+RST+"(ptproj/psiterm/psiterm.cpp)\r\n"
s += DIM+"  ⎿  Read 1840 lines"+RST+"\r\n\r\n"
s += GRN+"⏺"+RST+" "+BOLD+"Search"+RST+"(pattern: \"KConfigFailDSR|iHandshake\")\r\n"
s += DIM+"  ⎿  Found 2 matches in 1 file"+RST+"\r\n\r\n"
s += "⏺ The WiRSa raises "+BOLD+"DSR"+RST+" when it switches to modem mode. With the Psion's default\r\n"
s += "  handshake that signal is treated as a line event, so the fix is to ignore DSR:\r\n"
s += "  • PsiTerm never sets KConfigFailDSR\r\n"
s += "  • pin 6 removed on the adapter, so DSR can't reach the Psion at all\r\n\r\n"
s += ACC+"✻ Thinking… "+RST+DIM+"(14s · ↓ 1.2k tokens · esc to interrupt)"+RST+"\r\n\r\n"
row_input = s.count("\r\n") - s.count(E+"2J")*0 + 1
s += box([("> ", 2)])
s += DIM+"  ? for shortcuts"+RST+"                                                        "+DIM+"⏵⏵ accept edits on"+RST
s += E+"%d;5H" % (row_input + 1)
open("mock.bin","w",encoding="utf-8").write(s)
