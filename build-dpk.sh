#!/usr/bin/env bash
set -euo pipefail

VERSION="${1:-0.1.0}"
ARCH="${ARCH:-i386}"
NAME="walkies"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

if [[ ! -x "pkg/payload/sbin/walkies" ]]; then
  echo "[ERROR] missing pkg/payload/sbin/walkies"
  echo "        run: make static && make package"
  exit 1
fi

if ! command -v dpkbuild >/dev/null 2>&1; then
  echo "[ERROR] dpkbuild not found on PATH"
  exit 1
fi

python3 - <<PYEOF
import json
from pathlib import Path

manifest_path = Path("pkg/meta/manifest.json")
if manifest_path.exists():
    data = json.loads(manifest_path.read_text(encoding="utf-8"))
else:
    data = {
        "name": "walkies",
        "version": "0.1.0",
        "arch": "i386",
        "description": "BlueyOS network configuration utility",
        "depends": [],
        "recommends": [],
        "conflicts": [],
        "provides": ["walkies"],
        "maintainer": "BlueyOS Maintainers <maintainers@blueyos.example>",
        "homepage": "https://github.com/nzmacgeek/walkies",
        "core": True,
        "files": [],
        "scripts": {},
    }

data["name"] = "${NAME}"
data["version"] = "${VERSION}"
data["arch"] = "${ARCH}"
manifest_path.parent.mkdir(parents=True, exist_ok=True)
manifest_path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
print(f"updated {manifest_path}")
PYEOF

rm -rf build/dpk
mkdir -p build/dpk
find pkg/payload -name '.gitkeep' -type f -delete || true

dpkbuild build pkg

mkdir -p build/dpk
latest="$(ls -1t ${NAME}-*.dpk 2>/dev/null | head -n1)"
if [[ -z "$latest" ]]; then
  echo "[ERROR] dpkbuild produced no .dpk file"
  exit 1
fi
mv -f "$latest" build/dpk/
latest="build/dpk/$latest"
cp -f "$latest" .
echo "[OK] package ready: $(basename "$latest")"
