import sys
from PIL import Image, ImageDraw, ImageFont
cells_file, out, title = sys.argv[1], sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else ""
lines = open(cells_file).read().split("\n")
R, C, cr, cc = map(int, lines[0].split())
CW, CH, ASC = 6, 8, 7
SW, SH = 640, 240
OX, OY = (SW - C*CW)//2, (SH - R*CH)//2
# Psion 5mx LCD: 16 greys, dark ink on a grey-green backlit panel
DARK, LIGHT = (28, 34, 30), (176, 186, 168)
def grey(g): t = g/15; return tuple(int(DARK[i] + (LIGHT[i]-DARK[i])*t) for i in range(3))
img = Image.new("RGB", (SW, SH), grey(15)); d = ImageDraw.Draw(img)
d.fontmode = "1"
font = ImageFont.truetype(sys.argv[4] if len(sys.argv) > 4 else "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", 9)
L,Rt,U,Dn,HV,DB = 1,2,4,8,16,32
QUAD = [4,8,1,13,9,7,11,2,6,14]
for i in range(R*C):
    ch, fg, bg, bold, ul, seg, byte, width = map(int, lines[1+i].split())
    r, c = divmod(i, C)
    x0, y0 = OX + c*CW, OY + r*CH; x1, y1 = x0+CW, y0+CH
    d.rectangle([x0, y0, x1-1, y1-1], fill=grey(bg))
    if ch in (0, 32): continue
    F = grey(fg)
    if seg > 0:
        w = 2 if seg & HV else 1; cx, cy = x0 + CW//2, y0 + CH//2
        def ln(a, b): d.line([a, b], fill=F, width=w)
        if seg & DB:
            if seg & (L|Rt):
                a = x0 if seg & L else cx; b = x1 if seg & Rt else cx+1
                ln((a, cy-1), (b-1, cy-1)); ln((a, cy+1), (b-1, cy+1))
            if seg & (U|Dn):
                a = y0 if seg & U else cy; b = y1 if seg & Dn else cy+1
                ln((cx-1, a), (cx-1, b-1)); ln((cx+1, a), (cx+1, b-1))
        else:
            if seg & L: ln((x0, cy), (cx, cy))
            if seg & Rt: ln((cx, cy), (x1-1, cy))
            if seg & U: ln((cx, y0), (cx, cy))
            if seg & Dn: ln((cx, cy), (cx, y1-1))
        continue
    if 0x2580 <= ch <= 0x259F:
        lvl = fg; rx0, ry0, rx1, ry1 = x0, y0, x1, y1
        if ch == 0x2580: ry1 = y0 + CH//2
        elif ch == 0x2584: ry0 = y0 + CH//2
        elif ch == 0x258C: rx1 = x0 + CW//2
        elif ch == 0x2590: rx0 = x0 + CW//2
        elif ch == 0x2591: lvl = (fg + bg*3)//4
        elif ch == 0x2592: lvl = (fg + bg)//2
        elif ch == 0x2593: lvl = (fg*3 + bg)//4
        elif ch >= 0x2596:
            q = QUAD[ch-0x2596]; mx, my = x0+CW//2, y0+CH//2
            for bit, rc in ((1,(x0,y0,mx,my)),(2,(mx,y0,x1,my)),(4,(x0,my,mx,y1)),(8,(mx,my,x1,y1))):
                if q & bit: d.rectangle([rc[0], rc[1], rc[2]-1, rc[3]-1], fill=grey(fg))
            continue
        elif 0x2581 <= ch <= 0x2587: ry0 = y1 - CH*(ch-0x2580)//8
        elif 0x2589 <= ch <= 0x258F: rx1 = x0 + CW*(0x2590-ch)//8
        d.rectangle([rx0, ry0, rx1-1, ry1-1], fill=grey(lvl)); continue
    if 0x2800 <= ch <= 0x28FF:
        bits = ch - 0x2800; dx=[0,0,0,1,1,1,0,1]; dy=[0,1,2,0,1,2,3,3]; sx, sy = CW//2, max(1, CH//4)
        for k in range(8):
            if bits & (1<<k):
                px = x0 + dx[k]*sx + sx//2 - 1; py = y0 + dy[k]*sy + sy//2
                d.rectangle([px, py, px+1, py+1], fill=F)
        continue
    if width > 1 or byte < 0: byte = ord('?')
    if byte == 0: continue
    t = bytes([byte]).decode("cp1252", "replace")
    d.text((x0, y0 + ASC), t, font=font, fill=F, anchor="ls")
    if bold: d.text((x0+1, y0 + ASC), t, font=font, fill=F, anchor="ls")
    if ul: d.line([(x0, y1-1), (x1-1, y1-1)], fill=F)
# cursor (inverted cell)
cx0, cy0 = OX + cc*CW, OY + cr*CH
for yy in range(cy0, cy0+CH):
    for xx in range(cx0, cx0+CW):
        p = img.getpixel((xx, yy)); img.putpixel((xx, yy), tuple(DARK[i]+LIGHT[i]-p[i] for i in range(3)))
S = 3
big = img.resize((SW*S, SH*S), Image.NEAREST)
# simple Psion-style bezel
pad, bar = 36, 28
frame = Image.new("RGB", (SW*S + pad*2, SH*S + pad*2 + bar), (46, 48, 52))
fd = ImageDraw.Draw(frame)
fd.rounded_rectangle([8, 8, frame.width-9, frame.height-9], radius=22, fill=(62, 64, 70))
frame.paste(big, (pad, pad + bar))
fd.text((pad, 14), title, font=ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 16), fill=(200, 200, 205))
frame.save(out)
img.save(out.replace(".png", "-native.png"))
print(out, frame.size)
