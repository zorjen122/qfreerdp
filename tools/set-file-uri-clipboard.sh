#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'USAGE'
Usage:
  tools/set-file-uri-clipboard.sh FILE [FILE...]

Writes the given local files/directories to the Linux clipboard as text/uri-list,
so Qt QClipboard can expose them through QMimeData::hasUrls()/urls().

Examples:
  printf 'hello\n' > /tmp/qf-clip-test.txt
  tools/set-file-uri-clipboard.sh /tmp/qf-clip-test.txt

Then paste inside the remote Windows Explorer/Desktop from qfreerdp.
USAGE
}

if [[ $# -eq 0 || "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi

if ! command -v python3 >/dev/null 2>&1; then
    echo "error: python3 is required to build file:// URIs" >&2
    exit 1
fi

uris=$(
python3 - "$@" <<'PY'
import pathlib
import sys

for raw in sys.argv[1:]:
    path = pathlib.Path(raw).expanduser()
    if not path.exists():
        print(f"error: path does not exist: {raw}", file=sys.stderr)
        sys.exit(2)
    print(path.resolve().as_uri())
PY
)

# text/uri-list is line-oriented. CRLF is accepted by most consumers; Qt handles
# LF as well, but CRLF keeps us close to the MIME convention.
payload=$(printf '%s\r\n' "$uris")

if command -v wl-copy >/dev/null 2>&1; then
    printf '%s' "$payload" | wl-copy --type text/uri-list
    echo "set clipboard via wl-copy as text/uri-list"
elif command -v xclip >/dev/null 2>&1; then
    printf '%s' "$payload" | xclip -selection clipboard -t text/uri-list
    echo "set clipboard via xclip as text/uri-list"
elif command -v xsel >/dev/null 2>&1; then
    # xsel can set clipboard text, but not all builds reliably preserve the MIME
    # target. Prefer wl-copy/xclip when possible.
    printf '%s' "$payload" | xsel --clipboard --input
    echo "set clipboard via xsel; MIME target may appear as plain text"
else
    cat >&2 <<'ERR'
error: no supported clipboard writer found.
Install one of:
  sudo apt install wl-clipboard
  sudo apt install xclip
  sudo apt install xsel
ERR
    exit 1
fi

echo "clipboard URIs:"
printf '%s\n' "$uris"
