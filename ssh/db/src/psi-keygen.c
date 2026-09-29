/* psi-keygen.c - PsiTerm's SSH login key (not part of upstream Dropbear).
 *
 * Makes an Ed25519 key once, saved in Dropbear's own format as
 * <dir>/id_ed25519, and writes the OpenSSH public key line to
 * <dir>/id_ed25519.pub so it can be added to a server's authorized_keys. */
#include "includes.h"
#include "dbutil.h"
#include "buffer.h"
#include "signkey.h"
#include "gened25519.h"
#include "dbrandom.h"
#include "runopts.h"

#define PSI_KEY_COMMENT "psiterm@psion"

static void keypath(char *out, int max, const char *dir, const char *name)
{
	int n = (int)strlen(dir);
	const char *sep = (n > 0 && (dir[n - 1] == '\\' || dir[n - 1] == '/')) ? "" :
#ifdef PSI_HOST_TEST
		"/";
#else
		"\\";
#endif
	snprintf(out, max, "%s%s%s", dir, sep, name);
}

/* 0 = made a new key, 1 = there already was one, -1 = error (why set).
   pub gets "ssh-ed25519 AAAA... psiterm@psion". */
int psi_keygen(const char *dir, char *pub, int pubmax, char *why, int whymax)
{
	char path[200], pubpath[200];
	sign_key *key = new_sign_key();
	enum signkey_type type = DROPBEAR_SIGNKEY_ANY;
	buffer *b;
	unsigned char b64[200];
	unsigned long b64len = sizeof(b64);
	int made = 0;
	FILE *f;

	keypath(path, sizeof(path), dir, "id_ed25519");
	keypath(pubpath, sizeof(pubpath), dir, "id_ed25519.pub");

	if (readhostkey(path, key, &type) != DROPBEAR_SUCCESS || type != DROPBEAR_SIGNKEY_ED25519) {
		sign_key_free(key);
		key = new_sign_key();
		seedrandom();
		key->ed25519key = gen_ed25519_priv_key(256);
		key->type = DROPBEAR_SIGNKEY_ED25519;
		b = buf_new(MAX_PRIVKEY_SIZE);
		buf_put_priv_key(b, key, DROPBEAR_SIGNKEY_ED25519);
		f = fopen(path, "wb");
		if (!f || fwrite(b->data, 1, b->len, f) != b->len) {
			if (f) fclose(f);
			buf_burn_free(b);
			sign_key_free(key);
			snprintf(why, whymax, "could not save %s", path);
			return -1;
		}
		fclose(f);
		buf_burn_free(b);
		made = 1;
	}

	b = buf_new(MAX_PUBKEY_SIZE);
	buf_put_pub_key(b, key, DROPBEAR_SIGNKEY_ED25519);
	/* skip the blob's own length: the .pub line holds the bare blob */
	if (b->len < 4 || base64_encode(b->data + 4, b->len - 4, b64, &b64len) != CRYPT_OK) {
		buf_free(b);
		sign_key_free(key);
		snprintf(why, whymax, "could not encode the public key");
		return -1;
	}
	buf_free(b);
	sign_key_free(key);
	snprintf(pub, pubmax, "ssh-ed25519 %s %s", b64, PSI_KEY_COMMENT);
	f = fopen(pubpath, "wb");
	if (f) {
		fwrite(pub, 1, strlen(pub), f);
		fwrite("\n", 1, 1, f);
		fclose(f);
	}
	return made ? 0 : 1;
}

/* Is there a login key to offer? */
int psi_have_key(const char *dir, char *path, int max)
{
	FILE *f;
	keypath(path, max, dir, "id_ed25519");
	f = fopen(path, "rb");
	if (!f)
		return 0;
	fclose(f);
	return 1;
}
