#!/usr/bin/env python3
"""Check control names in the first column of the guide's Setting tables."""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
OBS_LABELS = {"Filters", "Properties", "Position", "Bounds", "Crop"}


def settings_in_guide(markdown):
    table = False
    for number, line in enumerate(markdown.splitlines(), 1):
        if not line.startswith("|"):
            table = False
            continue
        cell = line.split("|", 2)[1].strip()
        if cell == "Setting":
            table = True
        elif table and cell and not re.fullmatch(r":?-+:?", cell):
            yield number, cell.replace("*", "").replace(chr(96), "").strip()


def check(markdown, locale):
    labels = {name.casefold() for name in OBS_LABELS}
    for line in locale.splitlines():
        field = re.fullmatch(r'([\w.-]+)\s*=\s*"(.*)"', line)
        if field and not field[1].endswith((".Tip", ".Help")):
            labels.add(field[2].casefold())
    errors = []
    count = 0
    for number, cell in settings_in_guide(markdown):
        count += 1
        if cell.casefold() in labels:
            continue
        for part in cell.split(" / "):
            if part.strip().casefold() not in labels:
                errors.append(f"README.md:{number} unknown setting label {part.strip()!r}")
    if not count:
        errors.append("README.md has no Setting table to check")
    return count, errors


if __name__ == "__main__":
    count, failures = check((ROOT / "README.md").read_text(encoding="utf-8"),
                            (ROOT / "data/locale/en-US.ini").read_text(encoding="utf-8"))
    if failures:
        print("\n".join(failures), file=sys.stderr)
        sys.exit(1)
    print(f"Checked {count} setting rows")
