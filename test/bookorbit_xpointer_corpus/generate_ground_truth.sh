#!/usr/bin/env bash
#
# Regenerates the crengine xpointer ground-truth corpus and the spine XHTML the
# resolver test reads. Needs the KOReader emulator; CI does not, because the
# output is committed.
#
#   test/bookorbit_xpointer_corpus/generate_ground_truth.sh
#
# Override the emulator location with KOREADER_EMULATOR_DIR.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
FIXTURE_DIR="${REPO_ROOT}/test/bookorbit_xpointer_corpus/fixtures"
GENERATOR="${REPO_ROOT}/test/bookorbit_xpointer_corpus/generate_ground_truth.lua"
EMULATOR_DIR="${KOREADER_EMULATOR_DIR:-/home/monish/repos/koreader/koreader-emulator-x86_64-pc-linux-gnu-debug/koreader}"

if [ ! -x "${EMULATOR_DIR}/luajit" ]; then
  echo "KOReader emulator not found at ${EMULATOR_DIR}" >&2
  echo "Set KOREADER_EMULATOR_DIR, or leave the committed fixtures as they are." >&2
  exit 1
fi

# Both DOM versions: the oldest predates xpointer normalization and emits
# unindexed steps, the latest emits fully-indexed ones. Our parser must accept
# both, so both are corpus material.
DOM_VERSIONS=(20171225 20260812)

mkdir -p "${FIXTURE_DIR}"
rm -f "${FIXTURE_DIR}"/*.csv "${FIXTURE_DIR}"/*.xhtml

for epub in "${REPO_ROOT}"/test/epubs/*.epub; do
  name="$(basename "${epub}" .epub)"

  for dom in "${DOM_VERSIONS[@]}"; do
    # crengine reads the zip directly; nothing is unpacked for the corpus.
    ( cd "${EMULATOR_DIR}" && SDL_VIDEODRIVER=dummy ./luajit "${GENERATOR}" \
        "${epub}" "${FIXTURE_DIR}/${name}_dom${dom}.csv" "${dom}" ) >/dev/null
    echo "generated ${name}_dom${dom}.csv"
  done

  # The C++ resolver test has no zip reader, so each spine item is extracted
  # under its DocFragment index: DocFragment[N] is the Nth spine itemref.
  python3 - "${epub}" "${FIXTURE_DIR}/${name}" <<'PYEOF'
import os
import re
import sys
import zipfile

epub_path, prefix = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(epub_path) as archive:
    opf = next(n for n in archive.namelist() if n.endswith(".opf"))
    manifest = archive.read(opf).decode("utf-8")

    hrefs = {}
    for item in re.finditer(r"<item\b[^>]*>", manifest):
        tag = item.group(0)
        item_id = re.search(r'id="([^"]+)"', tag)
        href = re.search(r'href="([^"]+)"', tag)
        if item_id and href:
            hrefs[item_id.group(1)] = href.group(1)

    base = os.path.dirname(opf)
    for index, idref in enumerate(re.findall(r'<itemref[^>]*idref="([^"]+)"', manifest), start=1):
        entry = os.path.normpath(os.path.join(base, hrefs[idref])).replace("\\", "/")
        with open("%s_frag%d.xhtml" % (prefix, index), "wb") as out:
            out.write(archive.read(entry))
PYEOF
  echo "extracted spine of ${name}"
done

echo "corpus rows: $(cat "${FIXTURE_DIR}"/*.csv | grep -c '^[0-9]')"
