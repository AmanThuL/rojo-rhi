#!/bin/bash
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
git clone --no-local --no-tags --single-branch --branch main "$1" "$2"
cd "$2"
git fetch -q origin tag r2.4-pre-extraction-evidence --no-tags
git reset -q --hard r2.4-pre-extraction-evidence
git tag -d r2.4-pre-extraction-evidence >/dev/null
git filter-repo --force --refs main --paths-from-file "$here/paths.txt" \
  --path-rename RojoRHI/: --path-rename RHI/: \
  --message-callback 'return re.sub(rb"\(#(\d+)\)", rb"(AmanThuL/Luminex#\1)", message)'
