#!/usr/bin/env python3
'''bash -n over the shell launchers that LinuxRuntime.kt writes at runtime.

Kotlin raw strings (triple double quotes) that start with a bash shebang are
extracted, ${'$'} is turned back into $, other ${...} templates become a dummy
path, and the result goes through `bash -n`. Catches quoting mistakes such as
an apostrophe inside a single-quoted bash -c (fold.65 debian-tmux).
'''
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SOURCES = [pathlib.Path(p).resolve() for p in sys.argv[1:]] or sorted((ROOT / "app/android/app/src/main/kotlin").rglob("*.kt"))
RAW = re.compile(r'"""(.*?)"""', re.S)

failed = 0
checked = 0
for path in SOURCES:
    text = path.read_text(encoding="utf-8")
    for match in RAW.finditer(text):
        body = match.group(1).lstrip("\n")
        first = body.split("\n", 1)[0].strip()
        if not (first.startswith("#!") and "bash" in first):
            continue
        script = re.sub(r"\$\{[^}]*\}", "/x", body.replace("${'$'}", "$"))
        line = text.count("\n", 0, match.start()) + 1
        # Through stdin: no temp paths (Git Bash on Windows mangles them), bytes stay UTF-8.
        result = subprocess.run(["bash", "-n"], input=script.encode("utf-8"), capture_output=True)
        checked += 1
        if result.returncode != 0:
            failed += 1
            rel = path.relative_to(ROOT) if path.is_relative_to(ROOT) else path
            print(f"{rel}:{line}: {result.stderr.decode('utf-8', 'replace').strip()}", file=sys.stderr)

print(f"check-launchers: {checked} bash launchers, {failed} with syntax errors")
sys.exit(1 if failed else 0)
