#!/usr/bin/env python3
"""gen_fe25519.py - writes fe25519_mul.h: GF(2^255-19) multiply, square,
add and subtract for the ARM710 (no long multiply), used by fe25519.c.

    python3 web/tls/gen_fe25519.py > web/tls/fe25519_mul.h

A field element is 20 limbs of 13 bits (radix 2^13, 260 bits), each held in
a 32-bit word and allowed to be "loose": up to 2^13 + 2^8. Then every
product is below 2^26.1 and a whole column of the schoolbook product (at
most 20 products) is below 2^30.5: it is summed with plain 32-bit MLAs and
no carries at all. (Dropbear's 16-bit limbs need a carry after every
product.) A square sums each column's cross products a[i]*a[j] (i < j) once
and doubles the sum: 210 multiplies instead of 400.

The code is ARM assembler (inline): gcc 3.0, given the same sums in C,
loads every limb and forms every product at the top of the function and
then spills them all (2,259 instructions for a multiply; this one is
1,015, a square 673). Two columns are summed at a time so that each product
costs one load and one MLA. The top 19 columns are worked out first; each
low column then takes 608 times its high partner (2^260 = 608 mod p) before
its carry, so the product comes out reduced with no second pass.

Elsewhere (the host tests) the same sums are written in C. The arithmetic
was checked against Python's integers with a simulator of these
instructions at the limits of every bound (see PORTING.md, phase 5).
"""
N = 20

def cols():
    for k in range(2 * N - 1):
        yield k, [(i, k - i) for i in range(N) if 0 <= k - i < N]

def pair(L, k, two, sq, fold):
    """columns k and k+1 (if two) of a product (sq: of a square's cross
    products), into r5 and r8. a[i] is loaded once for both columns, and the
    b[k-i] loaded for column k is b[(k+1)-(i+1)] for column k+1 at the next
    i: one load per product. %1 = a, %2 = b (a again for a square)."""
    def ok(col, i):
        j = col - i
        if not (0 <= i < N and 0 <= j < N):
            return False
        return i < j if sq else True
    B = "%1" if sq else "%2"
    breg, free = {}, ["r4", "r9"]
    acc = {k: "r5", k + 1: "r8"}
    started = set()
    for i in range(0, N):
        cols = [c for c in ((k + 1, k) if two else (k,)) if ok(c, i)]
        if not cols:
            continue
        L.append("ldr r3, [%%1, #%d]" % (4 * i))
        for c in cols:
            j = c - i
            if j not in breg:
                for x in [x for x in breg if x > j + 1]:
                    free.append(breg.pop(x))
                breg[j] = free.pop(0)
                L.append("ldr %s, [%s, #%d]" % (breg[j], B, 4 * j))
            r = acc[c]
            if c not in started:
                started.add(c)
                if c == k and not sq:
                    L.append("mla %s, r3, %s, r6" % (r, breg[j]))   # + carry from below
                else:
                    L.append("mul %s, r3, %s" % (r, breg[j]))
            else:
                L.append("mla %s, r3, %s, %s" % (r, breg[j], r))
    for c in ((k, k + 1) if two else (k,)):
        r = acc[c]
        if sq:
            # 2 * cross sum + carry (+ the square term)
            if c in started:
                if c == k:
                    L.append("add r5, r6, r5, lsl #1")
                else:
                    L.append("mov r8, r8, lsl #1")
                    L.append("add r8, r8, r5, lsr #13")
            else:
                L.append("mov r5, r6" if c == k else "mov r8, r5, lsr #13")
            if c % 2 == 0:
                L.append("ldr r3, [%%1, #%d]" % (4 * (c // 2)))
                L.append("mla %s, r3, r3, %s" % (r, r))
        else:
            if c == k and c not in started:
                L.append("mov r5, r6")
            if c == k + 1:
                if c in started:
                    L.append("add r8, r8, r5, lsr #13")
                else:
                    L.append("mov r8, r5, lsr #13")
        if fold:   # + 608 * the high limb already worked out
            L.append("ldr r3, [%%0, #%d]" % (4 * (c + N)))
            L.append("mla %s, r3, r10, %s" % (r, r))
        L.append("and r3, %s, r7" % r)
        L.append("str r3, [%%0, #%d]" % (4 * c))
        if c == (k + 1 if two else k):
            L.append("mov r6, %s, lsr #13" % r)

def product(sq):
    """r (%3) = a * b (or a^2) mod p, loose. The high columns (20..38) are
    worked out first into t[20..39] (%0); then the low ones, each with
    608 * t[k+20] added (2^260 = 608 mod p) before it is carried; the carry
    out of column 19 is folded into limbs 0..2; then t[0..19] is copied to
    r (which may be a or b, so it is written last)."""
    L = ["mov r6, #0", "mov r7, #0x1f00", "orr r7, r7, #0xff", "mov r10, #608"]
    for k in range(N, 2 * N - 1, 2):
        pair(L, k, k + 1 < 2 * N - 1, sq, False)
    L.append("str r6, [%%0, #%d]" % (4 * (2 * N - 1)))
    L.append("mov r6, #0")
    for k in range(0, N, 2):
        pair(L, k, True, sq, True)
    # the carry out of column 19 (r6): 608 * it into limbs 0, 1, 2
    L += ["ldr r3, [%0, #0]", "mla r5, r6, r10, r3", "and r3, r5, r7", "str r3, [%0, #0]",
          "ldr r3, [%0, #4]", "add r5, r3, r5, lsr #13", "and r3, r5, r7", "str r3, [%0, #4]",
          "ldr r3, [%0, #8]", "add r3, r3, r5, lsr #13", "str r3, [%0, #8]"]
    for blk in range(5):
        src = "%0" if blk == 0 else "r8"
        dst = "%3" if blk == 0 else "r9"
        if blk:
            L.append("add r8, %%0, #%d" % (16 * blk))
            L.append("add r9, %%3, #%d" % (16 * blk))
        L.append("ldmia %s, {r3, r4, r5, r6}" % src)
        L.append("stmia %s, {r3, r4, r5, r6}" % dst)
    return L

def wrap(L, rr):
    """the carry out of limb 19 (r6) back into limbs 0..2 of rr: 2^260 = 608"""
    L.append("ldr r3, [%s, #0]" % rr)
    L.append("mov r4, #608")
    L.append("mla r5, r6, r4, r3")
    L.append("and r3, r5, r7")
    L.append("str r3, [%s, #0]" % rr)
    L.append("ldr r3, [%s, #4]" % rr)
    L.append("add r5, r3, r5, lsr #13")
    L.append("and r3, r5, r7")
    L.append("str r3, [%s, #4]" % rr)
    L.append("ldr r3, [%s, #8]" % rr)
    L.append("add r3, r3, r5, lsr #13")
    L.append("str r3, [%s, #8]" % rr)

def asm_add():
    L = []
    for i in range(N):
        L.append("ldr r3, [%%1, #%d]" % (4 * i))
        L.append("ldr r4, [%%2, #%d]" % (4 * i))
        L.append("add r6, r6, r3")
        L.append("add r6, r6, r4")
        L.append("and r3, r6, r7")
        L.append("str r3, [%%0, #%d]" % (4 * i))
        L.append("mov r6, r6, lsr #13")
    wrap(L, "%0")
    return L

def asm_sub():
    """r = a + 65p - b: 65p as 20 limbs in [2^14, 2^15) (fe25519.c K65P):
    0x5b2d, 0x5ffd x 18, 0x40fd"""
    L = ["mov r8, #0x6000", "sub r8, r8, #3"]          # 0x5ffd
    for i in range(N):
        L.append("ldr r3, [%%1, #%d]" % (4 * i))
        L.append("ldr r4, [%%2, #%d]" % (4 * i))
        L.append("add r6, r6, r3")
        if i == 0:
            L.append("add r6, r6, r8")
            L.append("sub r6, r6, #0x4d0")              # 0x5b2d
        elif i == N - 1:
            L.append("add r6, r6, r8")
            L.append("sub r6, r6, #0x1f00")             # 0x40fd
        else:
            L.append("add r6, r6, r8")
        L.append("sub r6, r6, r4")
        L.append("and r3, r6, r7")
        L.append("str r3, [%%0, #%d]" % (4 * i))
        L.append("mov r6, r6, lsr #13")
    wrap(L, "%0")
    return L

def c_other():
    return """static void fe_fold(fe r, const fe_w *t)
{
  fe_w c = 0;
  int i;
  for (i = 0; i < 20; i++) { c += t[i] + 608 * t[i + 20]; r[i] = c & 0x1fff; c >>= 13; }
  c = r[0] + 608 * c; r[0] = c & 0x1fff;
  c = r[1] + (c >> 13); r[1] = c & 0x1fff; r[2] += c >> 13;
}
static void fe_add(fe r, const fe a, const fe b)
{
  fe_w c = 0;
  int i;
  for (i = 0; i < 20; i++) { c += a[i] + b[i]; r[i] = c & 0x1fff; c >>= 13; }
  c = r[0] + 608 * c; r[0] = c & 0x1fff;
  c = r[1] + (c >> 13); r[1] = c & 0x1fff; r[2] += c >> 13;
}
static void fe_sub(fe r, const fe a, const fe b)
{
  fe_w c = 0;
  int i;
  for (i = 0; i < 20; i++) { c += a[i] + K65P[i] - b[i]; r[i] = c & 0x1fff; c >>= 13; }
  c = r[0] + 608 * c; r[0] = c & 0x1fff;
  c = r[1] + (c >> 13); r[1] = c & 0x1fff; r[2] += c >> 13;
}""".split("\n")

def emit_asm(name, args, L, inputs, extra="", decl=""):
    out = []
    out.append("static void %s(%s)" % (name, args))
    out.append("{")
    if decl:
        out.append("  " + decl)
    out.append("  __asm__ __volatile__(")
    out.append('    "mov r6, #0\\n\\t"')
    out.append('    "mov r7, #0x1f00\\n\\t"')
    out.append('    "orr r7, r7, #0xff\\n\\t"')
    for l in L:
        out.append('    "%s\\n\\t"' % l)
    out.append('    : : %s' % inputs)
    out.append('    : "r3", "r4", "r5", "r6", "r7"%s, "memory");' % extra)
    out.append("}")
    return out

def c_mul():
    out = ["static void fe_mul_cols(fe_w *t, const fe_w *a, const fe_w *b)", "{", "  fe_w c = 0;"]
    for k, terms in cols():
        out.append("  c += %s; t[%d] = c & 0x1fff; c >>= 13;" % (" + ".join("a[%d]*b[%d]" % t for t in terms), k))
    out.append("  t[%d] = c;" % (2 * N - 1))
    out.append("}")
    return out

def c_sq():
    out = ["static void fe_sq_cols(fe_w *t, const fe_w *a)", "{", "  fe_w c = 0, s;"]
    for k in range(2 * N - 1):
        cross = ["a[%d]*a[%d]" % (i, k - i) for i in range(N) if i < k - i < N]
        s = "  s = %s;" % (" + ".join(cross) if cross else "0")
        sq = " + a[%d]*a[%d]" % (k // 2, k // 2) if k % 2 == 0 else ""
        out.append(s + " c += 2 * s%s; t[%d] = c & 0x1fff; c >>= 13;" % (sq, k))
    out.append("  t[%d] = c;" % (2 * N - 1))
    out.append("}")
    return out

out = ["/* fe25519_mul.h - GENERATED by web/tls/gen_fe25519.py, do not edit. */",
       "#if defined(__GNUC__) && defined(__arm__) && !defined(FE_PORTABLE)"]
out += emit_asm("fe_mul", "fe r, const fe a, const fe b", product(False), '"r"(t), "r"(a), "r"(b), "r"(r)', ', "r8", "r9", "r10"', "fe_w t[40];")
out += emit_asm("fe_sq", "fe r, const fe a", product(True), '"r"(t), "r"(a), "r"(a), "r"(r)', ', "r8", "r9", "r10"', "fe_w t[40];")
out += emit_asm("fe_add", "fe r, const fe a, const fe b", asm_add(), '"r"(r), "r"(a), "r"(b)')
out += emit_asm("fe_sub", "fe r, const fe a, const fe b", asm_sub(), '"r"(r), "r"(a), "r"(b)', ', "r8"')
out.append("#else")
out += c_mul()
out += c_sq()
out += c_other()
out += """static void fe_mul(fe r, const fe a, const fe b)
{
  fe_w t[40];
  fe_mul_cols(t, a, b);
  fe_fold(r, t);
}
static void fe_sq(fe r, const fe a)
{
  fe_w t[40];
  fe_sq_cols(t, a);
  fe_fold(r, t);
}""".split("\n")
out.append("#endif")
print("\n".join(out))
