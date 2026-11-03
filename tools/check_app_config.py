#!/usr/bin/env python3
"""Validate a PS4 React app.json without third-party packages."""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

TITLE_ID = re.compile(r"^[A-Z]{4}[0-9]{5}$")
CONTENT_ID = re.compile(r"^[A-Z]{2}[0-9]{4}-[A-Z]{4}[0-9]{5}_00-[A-Z0-9]{16}$")
VERSION = re.compile(r"^[0-9]{2}\.[0-9]{3}\.[0-9]{3}$")


def size(value: object, field: str, errors: list[str]) -> None:
    if not isinstance(value, dict):
        errors.append(f"{field} must be an object with width and height")
        return
    for key, low, high in (("width", 320, 3840), ("height", 240, 2160)):
        number = value.get(key)
        if not isinstance(number, int) or isinstance(number, bool) or not low <= number <= high:
            errors.append(f"{field}.{key} must be an integer from {low} to {high}")
    extra = set(value) - {"width", "height"}
    if extra:
        errors.append(f"{field} has unknown keys: {', '.join(sorted(extra))}")


def validate(path: Path) -> list[str]:
    try:
        app = json.loads(path.read_text(encoding="utf-8"))
    except OSError as error:
        return [f"cannot read: {error}"]
    except json.JSONDecodeError as error:
        return [f"invalid JSON: {error}"]
    if not isinstance(app, dict):
        return ["the manifest must be a JSON object"]

    errors: list[str] = []
    for field in ("titleId", "contentId", "name", "version", "render", "surface"):
        if field not in app:
            errors.append(f"missing required field: {field}")
    title_id = app.get("titleId")
    if title_id is not None and (not isinstance(title_id, str) or not TITLE_ID.fullmatch(title_id)):
        errors.append("titleId must look like CUSA99059")
    content_id = app.get("contentId")
    if content_id is not None and (
            not isinstance(content_id, str) or len(content_id) != 36
            or not CONTENT_ID.fullmatch(content_id)):
        errors.append("contentId must be the 36-character UP0001-CUSA99059_00-PS4REACTSTARTER0 form")
    name = app.get("name")
    if name is not None and (not isinstance(name, str) or not 1 <= len(name) <= 127):
        errors.append("name must be a string from 1 to 127 characters")
    version = app.get("version")
    if version is not None and (not isinstance(version, str) or not VERSION.fullmatch(version)):
        errors.append("version must look like 01.000.000")
    for field in ("render", "surface"):
        if field in app:
            size(app[field], field, errors)
    if "networking" in app and not isinstance(app["networking"], bool):
        errors.append("networking must be a boolean")
    if "filesystemAccess" in app and app["filesystemAccess"] not in ("sandbox", "console"):
        errors.append('filesystemAccess must be "sandbox" or "console"')
    if "privileged" in app and not isinstance(app["privileged"], bool):
        errors.append("privileged must be a boolean")
    if "timeoutSeconds" in app:
        number = app["timeoutSeconds"]
        if not isinstance(number, (int, float)) or isinstance(number, bool) or number < 1:
            errors.append("timeoutSeconds must be a positive number")
    return errors


def main(argv: list[str]) -> int:
    if not argv:
        print("usage: check_app_config.py <app.json> [...]", file=sys.stderr)
        return 2
    failed = False
    for value in argv:
        path = Path(value)
        errors = validate(path)
        if errors:
            failed = True
            for error in errors:
                print(f"{path}: {error}", file=sys.stderr)
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
