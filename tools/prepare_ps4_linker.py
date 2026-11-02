"""Create a project-local OpenOrbis linker script which keeps split data sections.

OpenOrbis' stock link.x collects only `*(.data)`. Clang can emit `.data.*`,
`.init_array.*`, `.tdata.*` and `.tbss.*`; those sections then fall outside the
file-backed writable LOAD segment and arrive zeroed after SELF conversion.
Stremio PS4 hit this as a launch crash; the same fix applies here.
"""

from pathlib import Path
import sys

sdk_script, destination = map(Path, sys.argv[1:])
text = sdk_script.read_text()
replacements = {
    "*(.data)": "*(.data .data.*)",
    "*(.init_array);": "KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))\n\t\tKEEP(*(.init_array));",
    "*(.tdata);": "*(.tdata .tdata.*);",
    "*(.tbss);": "*(.tbss .tbss.*);",
}
for original, replacement in replacements.items():
    if text.count(original) != 1:
        raise SystemExit(f"Unsupported OpenOrbis link.x: expected exactly one {original!r}")
    text = text.replace(original, replacement)
text = "# Project-local adaptation: collect initialized sections before SELF conversion.\n" + text
destination.parent.mkdir(parents=True, exist_ok=True)
if not destination.exists() or destination.read_text() != text:
    destination.write_text(text)
