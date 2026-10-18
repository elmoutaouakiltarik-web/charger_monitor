#!/usr/bin/env bash
# Date-certify an anchor file with an RFC 3161 time-stamp authority, using only
# openssl (and curl for the HTTP step). Nothing but a 32-byte SHA-256 digest of
# the anchor leaves the site.
#
#   scripts/notarise.sh request  bay-07.anchor                       -> bay-07.anchor.tsq
#   scripts/notarise.sh submit   bay-07.anchor https://tsa.example/tsr -> bay-07.anchor.tsr
#   scripts/notarise.sh verify   bay-07.anchor tsa-root.pem [tsa-chain.pem]
#   scripts/notarise.sh inspect  bay-07.anchor
#
# What a time stamp proves: this exact digest existed before the TSA's genTime.
# It does not prove the content was true, who wrote it, or that nothing was
# altered before the anchor was made. `anchored_at_utc` inside the anchor is the
# local clock's claim; only the TSA's genTime counts. Keep the .tsr, the TSA
# chain and (if you can) revocation data next to the anchor on append-only
# storage: TSA certificates expire, and verification years later must be done
# "as of" the genTime (see `verify`).
set -euo pipefail

usage() { sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
need() { command -v "$1" >/dev/null 2>&1 || { echo "notarise: '$1' is required" >&2; exit 2; }; }

[ $# -ge 2 ] || usage
cmd="$1"; file="$2"
[ -f "$file" ] || { echo "notarise: no such file: $file" >&2; exit 2; }
need openssl

case "$cmd" in
    request)
        # A random nonce is included by default: it binds the reply to this request.
        openssl ts -query -data "$file" -sha256 -cert -out "$file.tsq"
        echo "wrote $file.tsq (digest $(openssl dgst -sha256 -r "$file" | cut -d' ' -f1))"
        ;;
    submit)
        [ $# -eq 3 ] || usage
        need curl
        [ -f "$file.tsq" ] || { echo "notarise: run 'request' first" >&2; exit 2; }
        curl -sS --fail -H 'Content-Type: application/timestamp-query' \
            --data-binary @"$file.tsq" "$3" -o "$file.tsr"
        openssl ts -reply -in "$file.tsr" -text | sed -n '1,12p'
        echo "wrote $file.tsr"
        ;;
    inspect)
        openssl ts -reply -in "$file.tsr" -text
        ;;
    verify)
        [ $# -ge 3 ] || usage
        root="$3"; chain="${4:-}"
        args=(-verify -data "$file" -in "$file.tsr" -CAfile "$root")
        [ -n "$chain" ] && args+=(-untrusted "$chain")
        # The TSA's own certificate has a short life. Verify as of the time stamp,
        # not as of today, or a perfectly good old time stamp "fails" after expiry.
        gen="$(openssl ts -reply -in "$file.tsr" -text | sed -n 's/^Time stamp: //p' | head -1)"
        if [ -n "$gen" ] && epoch="$(date -u -d "$gen" +%s 2>/dev/null)"; then
            args+=(-attime "$epoch")
            echo "verifying as of genTime: $gen"
        fi
        openssl ts "${args[@]}"
        ;;
    *) usage ;;
esac
