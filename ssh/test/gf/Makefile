# host-side differential test of the 32-bit field arithmetic (see gftest.c)
CF := -O2 -g -std=gnu99 -w -DLOCALOPTIONS_H_EXISTS -D__EPOC32__ -DPSI_HOST_TEST -DDROPBEAR_CLIENT -include compat/psicompat.h \
      -I test/hostcfg -I db -I db/src -I db/libtomcrypt/src/headers -I db/libtommath
DS := /home/claude/dropbear-src
test/gf/gftest: test/gf/gftest.c test/gf/wrap_new.c test/gf/wrap_ref.c db/src/curve25519.c db/src/curve25519_gf32.h
	gcc $(CF) -o $@ test/gf/gftest.c -Dpsi_printf=printf -Dpsi_sscanf=sscanf test/gf/wrap_new.c test/gf/wrap_ref.c $(DS)/libtomcrypt/libtomcrypt.a
run: test/gf/gftest
	./test/gf/gftest 3000
