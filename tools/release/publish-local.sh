#!/bin/bash
# Puts a PsiMail (or PsiWeb) build on the local update server, signed, so
# the Psion can fetch it with Tools > Update PsiMail.
#   tools/release/publish-local.sh PsiMail 0.3.1 [dist/PsiMail.sis]
# The server (server/psion-update.sh) serves $PSION_UPDATE_DIR
# (default ~/PsionDownloads/update).
set -e
PRODUCT=${1:?product: PsiMail or PsiWeb}
VERSION=${2:?version, e.g. 0.3.1}
HERE=$(cd "$(dirname "$0")/../.." && pwd)
SIS=${3:-$HERE/dist/$PRODUCT.sis}
DIR="${PSION_UPDATE_DIR:-$HOME/PsionDownloads/update}"
mkdir -p "$DIR"
cp "$SIS" "$DIR/$PRODUCT.sis.new"
python3 "$HERE/tools/release/sign.py" ${PSION_SIGN_KEY:+--key "$PSION_SIGN_KEY"} --product "$PRODUCT" "$DIR/$PRODUCT.sis.new" "$VERSION" >/dev/null
mv "$DIR/$PRODUCT.sis.new" "$DIR/$PRODUCT.sis"
mv "$DIR/$PRODUCT.sis.new.sig" "$DIR/$PRODUCT.sis.sig"
echo "$VERSION" > "$DIR/$PRODUCT-version.txt"
echo "$PRODUCT $VERSION is on the update server ($DIR)"
