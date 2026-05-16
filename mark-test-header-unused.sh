#!/usr/bin/env bash
set -euo pipefail

HEADER="loradaemon_refactor/tests/common_loradaemon_test.h"

if [[ ! -f "$HEADER" ]]; then
  echo "ERROR: not found: $HEADER" >&2
  exit 1
fi

python3 - <<'PY'
from pathlib import Path
import re

path = Path("loradaemon_refactor/tests/common_loradaemon_test.h")
text = path.read_text()

macro = """#if defined(__GNUC__)
#define TEST_UNUSED __attribute__((unused))
#else
#define TEST_UNUSED
#endif

"""

if "#define TEST_UNUSED" not in text:
    # Insert after the last #include block.
    matches = list(re.finditer(r'^#include .*$\n', text, flags=re.MULTILINE))
    if not matches:
        raise SystemExit("ERROR: no #include lines found")
    pos = matches[-1].end()
    text = text[:pos] + "\n" + macro + text[pos:]

# Mark shared header-local items as intentionally maybe-unused per test binary.
text = re.sub(
    r'^static (?!TEST_UNUSED\b)(const char \*|int |pid_t |long |void )',
    r'static TEST_UNUSED \1',
    text,
    flags=re.MULTILINE,
)

path.write_text(text)
PY

echo "Patched: $HEADER"
echo
echo "Now test:"
echo "  ./loradaemon_refactor/run_tests.sh --TX --rx-seconds 5"
