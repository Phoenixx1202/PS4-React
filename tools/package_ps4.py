"""Package a PS4 React app as an installable PS4 PKG.

Run inside the PacBrew rootfs (or any environment with OPENORBIS set):

    OPENORBIS=/opt/pacbrew/ps4/openorbis \
      python3 tools/package_ps4.py build/native/ps4

The output is a fake/free PKG for jailbroken consoles, made with the
OpenOrbis/LibOrbisPkg toolchain. Pass the title and IDs from the app's
``app.json``; the defaults describe the starter app.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def run(*args: object, env: dict[str, str] | None = None) -> None:
    print("+", " ".join(str(a) for a in args))
    subprocess.run([str(a) for a in args], check=True, env=env)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", nargs="?", default="build/native/ps4",
                        help="directory containing eboot.bin (default: build/native/ps4)")
    parser.add_argument("--app-dir", default=None,
                        help="directory containing sce_module (default: <build>/../app)")
    parser.add_argument("--title-id", default="CUSA99059")
    parser.add_argument("--content-id", default="UP0001-CUSA99059_00-PS4REACTSTARTER0")
    parser.add_argument("--title", default="PS4 React Starter")
    parser.add_argument("--version", default="01.00",
                        help="APP_VER/VERSION as MM.mm (default: 01.00)")
    parser.add_argument("--output", default=None)
    parser.add_argument("--assets", default=str(ROOT / "assets"))
    parser.add_argument("--keep-stage", action="store_true")
    parser.add_argument("--skip-load-image-check", action="store_true",
                        help="skip the initialized-section check (not recommended)")
    args = parser.parse_args()

    sdk = Path(os.environ.get("OPENORBIS", "/opt/pacbrew/ps4/openorbis"))
    pkgtool = sdk / "bin/linux/PkgTool.Core"
    create_gp4 = sdk / "bin/linux/create-gp4"
    if not pkgtool.exists() or not create_gp4.exists():
        print(f"OpenOrbis PKG tools not found under {sdk}", file=sys.stderr)
        return 2

    build = Path(args.build).resolve()
    eboot = build / "eboot.bin"
    if not eboot.is_file():
        print(f"Missing {eboot}; build first (tools/build_ps4.ps1).", file=sys.stderr)
        return 2
    if eboot.read_bytes()[:4] != bytes.fromhex("4f153d1d"):
        print("Refusing to package a file that is not a PS4 SELF.", file=sys.stderr)
        return 2

    app_dir = Path(args.app_dir).resolve() if args.app_dir else build.parent / "app"
    if not (app_dir / "sce_module").is_dir():
        print(f"Missing {app_dir}/sce_module; run the staging target first.", file=sys.stderr)
        return 2

    title_id = args.title_id
    content_id = args.content_id
    if len(content_id) != 36:
        print("content-id must be exactly 36 characters.", file=sys.stderr)
        return 2

    # Stremio PS4 found that OpenOrbis' stock SELF conversion could leave
    # .data.* outside the loaded image, zeroing initialized globals at launch.
    # Our project-local link.x fixes it; refuse to package a regression.
    if not args.skip_load_image_check:
        oelf = next(iter(sorted(build.glob("*.oelf"))), None)
        if oelf is not None:
            elf = oelf.with_suffix("")
            checker = ROOT / "tools/validate_ps4_load_image.py"
            if elf.is_file() and checker.is_file():
                run(sys.executable, checker, elf, oelf)

    output = Path(args.output).resolve() if args.output else build / "PS4-React-Starter.pkg"
    stage = build / "package"
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)
    (stage / "sce_sys").mkdir()
    (stage / "sce_module").mkdir()

    shutil.copy2(eboot, stage / "eboot.bin")
    for prx in (app_dir / "sce_module").glob("*.prx"):
        shutil.copy2(prx, stage / "sce_module" / prx.name)
    # App data staged beside eboot.bin (CA bundle, fonts, assets) lives at
    # /app0 inside the package.
    for item in app_dir.iterdir():
        if item.name in ("eboot.bin", "sce_module"):
            continue
        target = stage / item.name
        if item.is_dir():
            shutil.copytree(item, target)
        elif item.is_file():
            shutil.copy2(item, target)
    for name in ("icon0.png", "pic0.png", "pic1.png"):
        source = Path(args.assets) / name
        if source.is_file():
            shutil.copy2(source, stage / "sce_sys" / name)
    for name in ("LICENSE", "NOTICE", "LICENSE-ATTRIBUTION"):
        source = ROOT / name
        if source.is_file():
            shutil.copy2(source, stage / name)

    env = dict(os.environ, DOTNET_SYSTEM_GLOBALIZATION_INVARIANT="1")
    tool = pkgtool

    sfo = stage / "sce_sys/param.sfo"
    if sfo.exists():
        sfo.unlink()
    run(tool, "sfo_new", sfo, env=env)
    version_parts = args.version.split(".")
    if (not version_parts or not version_parts[0].isdigit()
            or (len(version_parts) > 1 and not version_parts[1].isdigit())):
        print("version must look like MM.mm, for example 01.00.", file=sys.stderr)
        return 2
    version = f"{version_parts[0].zfill(2)[:2]}.{version_parts[1].zfill(2)[:2] if len(version_parts) > 1 else '00'}"

    strings = {
        "APP_VER": (version, 8),
        "CATEGORY": ("gd", 4),
        "FORMAT": ("obs", 4),
        "CONTENT_ID": (content_id, 48),
        "TITLE": (args.title, 128),
        "TITLE_ID": (title_id, 12),
        "VERSION": (version, 8),
    }
    for key, (value, size) in strings.items():
        run(tool, "sfo_setentry", sfo, key, "--type", "Utf8",
            "--maxsize", size, "--value", value, env=env)
    # ATTRIBUTE bit 1 (0x02) fixes Cross as the common-dialog enter button.
    # Without it the IME follows the console's region assignment, so a
    # Circle-confirm console shows Cross as cancel in the keyboard.
    for key, value in {
        "APP_TYPE": 1,
        "ATTRIBUTE": 2,
        "DOWNLOAD_DATA_SIZE": 0,
        "SYSTEM_VER": 0,
    }.items():
        run(tool, "sfo_setentry", sfo, key, "--type", "Integer",
            "--maxsize", 4, "--value", value, env=env)

    gp4 = stage / "project.gp4"
    if gp4.exists():
        gp4.unlink()
    run(create_gp4, "-out", gp4, "--content-id", content_id, "--path", stage)

    # This SDK's create-gp4 may omit directories that only appear in the file
    # list. Rebuild the directory tree from the manifest before pkg_build.
    project = ET.parse(gp4)
    manifest = project.getroot()
    directories = manifest.find("rootdir")
    directories.clear()
    files = manifest.find("files")
    if files is None:
        print("create-gp4 produced no files section.", file=sys.stderr)
        return 2
    for entry in files:
        node = directories
        parts = Path(entry.attrib["targ_path"]).parts[:-1]
        for part in parts:
            existing = next((x for x in node if x.get("targ_name") == part), None)
            if existing is None:
                existing = ET.SubElement(node, "dir", targ_name=part)
            node = existing
        if not (stage / entry.attrib["orig_path"]).is_file():
            print(f"Missing packaged file: {entry.attrib['orig_path']}", file=sys.stderr)
            return 2
    project.write(gp4, encoding="utf-8", xml_declaration=True)

    generated = build / (content_id + ".pkg")
    if generated.exists():
        generated.unlink()
    run(tool, "pkg_build", gp4, build, env=env)
    if not generated.is_file():
        candidates = sorted(build.glob("*.pkg"), key=lambda p: p.stat().st_mtime)
        if not candidates:
            print("pkg_build produced no .pkg", file=sys.stderr)
            return 2
        generated = candidates[-1]
    output.parent.mkdir(parents=True, exist_ok=True)
    generated.replace(output)
    with output.open("rb") as handle:
        if handle.read(4) != b"\x7fCNT":
            print("PKG header is invalid.", file=sys.stderr)
            return 2

    # pkg_validate is available in current OpenOrbis/LibOrbisPkg builds.
    run(tool, "pkg_validate", output, env=env)

    digest = hashlib.sha256(output.read_bytes()).hexdigest()
    print(f"\nPS4 package: {output}")
    print(f"  size:   {output.stat().st_size} bytes")
    print(f"  sha256: {digest}")
    print(f"  title:  {args.title} ({title_id})")
    print(f"  content:{content_id}")
    if not args.keep_stage:
        shutil.rmtree(stage, ignore_errors=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
