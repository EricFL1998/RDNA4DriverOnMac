#!/bin/bash
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$DIR/../bin/air2gfx12"

if [ ! -x "$BIN" ]; then
    echo "ERROR: Binary not found at $BIN"
    exit 1
fi

echo "Running air2gfx12 --help..."
OUTPUT=$("$BIN" --help 2>&1 || true)

if echo "$OUTPUT" | grep -q "Usage: air2gfx12"; then
    echo "PASS: air2gfx12 help message verified"
else
    echo "FAIL: Expected help message containing 'Usage: air2gfx12'"
    echo "Actual output:"
    echo "$OUTPUT"
    exit 1
fi
