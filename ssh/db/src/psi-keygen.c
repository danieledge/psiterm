/* psi-keygen.c - PsiTerm's SSH login keys (not part of upstream Dropbear).
 *
 * Each key lives in PsiTerm's Keys folder as three files sharing a base name:
 *   <base>.key  the private key, in Dropbear's own format
 *   <base>.pub  the OpenSSH public key line, for a server's authorized_keys
 *   <base>.fp   its SHA256 fingerprint, as ssh-keygen -l shows it
 * Keys are made here (Ed25519) or imported from an OpenSSH or Dropbear
 * private key file (Ed25519 or RSA, without a passphrase). */
#include "includes.h"
#include "dbutil.h"
#include "buffer.h"
#include "signkey.h"
#include "gened25519.h"
#include "dbrandom.h"
#include "runopts.h"
#include "keyimport.h"

static int write_file(const char *path, const void *data, unsigned int len)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return -1;
	if (len && fwrite(data, 1, len, f) != len) {
		fclose(f);
		return -1;
	}
	fclose(f);
	return 0;
}

/* Saves key as <base>.key/.pub/.fp. pub gets the public key line. */
static int save_key(sign_key *key, enum signkey_type type, const char *base,
	const char *name, char *pub, int pubmax, char *why, int whymax)
{
	char path[200];
	buffer *b;
	unsigned char b64[800];
	unsigned long b64len = sizeof(b64);
	const char *tname = signkey_name_from_type(type, NULL);
	char *fp;
	int r;

	b = buf_new(MAX_PRIVKEY_SIZE);
	buf_put_priv_key(b, key, type);
	snprintf(path, sizeof(path), "%s.key", base);
	if (write_file(path, b->data, b->len) != 0) {
		buf_burn_free(b);
		snprintf(why, whymax, "could not save %s", path);
		return -1;
	}
	buf_burn_free(b);

	b = buf_new(MAX_PUBKEY_SIZE);
	buf_put_pub_key(b, key, type);
	/* the .pub line holds the bare blob, without the buffer's length */
	if (b->len < 4 || base64_encode(b->data + 4, b->len - 4, b64, &b64len) != CRYPT_OK) {
		buf_free(b);
		snprintf(why, whymax, "could not encode the public key");
		return -1;
	}
	fp = sign_key_fingerprint(b->data + 4, b->len - 4);
	buf_free(b);
	snprintf(pub, pubmax, "%s %s %s", tname, b64, name && *name ? name : "psiterm@psion");
	snprintf(path, sizeof(path), "%s.pub", base);
	{
		char line[900];
		snprintf(line, sizeof(line), "%s\n", pub);
		r = write_file(path, line, strlen(line));
	}
	if (r != 0) {
		m_free(fp);
		snprintf(why, whymax, "could not save %s", path);
		return -1;
	}
	snprintf(path, sizeof(path), "%s.fp", base);
	write_file(path, fp, strlen(fp));
	m_free(fp);
	return 0;
}

/* Makes a new Ed25519 key. 0 = done, -1 = error (why set). */
int psi_keygen(const char *base, const char *name, char *pub, int pubmax, char *why, int whymax)
{
	sign_key *key = new_sign_key();
	int r;
	seedrandom();
	key->ed25519key = gen_ed25519_priv_key(256);
	key->type = DROPBEAR_SIGNKEY_ED25519;
	r = save_key(key, DROPBEAR_SIGNKEY_ED25519, base, name, pub, pubmax, why, whymax);
	sign_key_free(key);
	return r;
}

/* Imports a private key file (OpenSSH, or Dropbear's own format). */
int psi_keyimport(const char *src, const char *base, const char *name,
	char *pub, int pubmax, char *why, int whymax)
{
	enum signkey_type type = DROPBEAR_SIGNKEY_ANY;
	sign_key *key = NULL;
	FILE *f;
	char first[64];
	int r;

	f = fopen(src, "rb");
	if (!f) {
		snprintf(why, whymax, "cannot open %s", src);
		return -1;
	}
	first[0] = 0;
	if (!fgets(first, sizeof(first), f))
		first[0] = 0;
	fclose(f);

	seedrandom();
	if (strncmp(first, "-----BEGIN ", 11) == 0) {
		if (strstr(first, "ENCRYPTED") != NULL) {
			snprintf(why, whymax, "the key has a passphrase - remove it first (ssh-keygen -p)");
			return -1;
		}
		key = import_read(src, NULL, KEYFILE_OPENSSH);
		if (!key) {
			snprintf(why, whymax, "not a key PsiTerm can read (a passphrase, or an unsupported type?)");
			return -1;
		}
		type = key->type;
	} else {
		key = new_sign_key();
		if (readhostkey(src, key, &type) != DROPBEAR_SUCCESS) {
			sign_key_free(key);
			snprintf(why, whymax, "not an OpenSSH or Dropbear private key");
			return -1;
		}
		key->type = type;
	}
	if (type != DROPBEAR_SIGNKEY_ED25519 && type != DROPBEAR_SIGNKEY_RSA) {
		sign_key_free(key);
		snprintf(why, whymax, "only Ed25519 and RSA keys are supported");
		return -1;
	}
	r = save_key(key, type, base, name, pub, pubmax, why, whymax);
	sign_key_free(key);
	return r;
}

/* Does <base>.key exist? path gets its name. */
int psi_have_key(const char *base, char *path, int max)
{
	FILE *f;
	snprintf(path, max, "%s.key", base);
	f = fopen(path, "rb");
	if (!f)
		return 0;
	fclose(f);
	return 1;
}
