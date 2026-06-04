#!/bin/bash
# Generates a self-signed TLS certificate for local development.
# Output: ssl/cert.pem and ssl/key.pem
# Valid for 2 years, covers localhost and 127.0.0.1.
#
# Usage: ./bhs.sh createcert
#   or:  ./scripts/create_local_cert.sh

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT_DIR"

SSL_DIR="./ssl"
CERT="$SSL_DIR/cert.pem"
KEY="$SSL_DIR/key.pem"

source ./scripts/show/divbar
echo "Generating self-signed TLS certificate for local development..."
source ./scripts/show/divbar

if ! command -v openssl &>/dev/null; then
    echo "Error: openssl not found. Install it first."
    echo "  Linux : sudo apt install openssl"
    echo "  macOS : brew install openssl"
    exit 1
fi

mkdir -p "$SSL_DIR"

openssl req -x509 \
    -newkey rsa:4096 \
    -keyout "$KEY" \
    -out    "$CERT" \
    -days   730 \
    -nodes \
    -subj   "/C=US/ST=Local/L=Local/O=Development/CN=localhost" \
    -addext "subjectAltName=DNS:localhost,DNS:127.0.0.1,IP:127.0.0.1" \
    2>/dev/null

echo ""
echo "Certificate : $CERT"
echo "Private key : $KEY"
echo ""
openssl x509 -in "$CERT" -noout -text 2>/dev/null \
    | grep -E "Not Before:|Not After :|Subject:|DNS:|IP Address:"
echo ""

# Also copy into bin/ssl/ if bin/ exists (so rundebug picks it up immediately)
if [ -d ./bin ]; then
    mkdir -p ./bin/ssl
    cp "$CERT" ./bin/ssl/cert.pem
    cp "$KEY"  ./bin/ssl/key.pem
    echo "Copied to bin/ssl/ as well."
fi

source ./scripts/show/divbar
echo "Done! Enable HTTPS in configs/config.txt:"
echo "  ssl_enabled=1"
echo ""
