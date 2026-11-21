"""Refresh the source ports from the exact Git commits recorded in origins.json."""
import argparse
import json
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PORTS = ROOT / "src" / "third-party" / "ports"
ASSETS = ROOT / "data" / "components"
CODE = {".c", ".cpp", ".cc", ".m", ".mm", ".hpp", ".h", ".inl", ".ui", ".qrc"}


def checkout(origin, dest):
    repo, commit = origin["project"], origin["commit"]
    if not re.fullmatch(r"[\w.-]+/[\w.-]+", repo) or not re.fullmatch("[0-9a-f]{40}", commit):
        raise ValueError("Invalid project pin")
    subprocess.run(["git", "init", "-q", str(dest)], check=True)
    subprocess.run(["git", "-C", str(dest), "fetch", "-q", "--depth=1",
                    "https://github.com/" + repo + ".git", commit], check=True)
    subprocess.run(["git", "-C", str(dest), "checkout", "-q", "--detach", commit], check=True)


def install_file(src, dest):
    if src.is_symlink():
        raise ValueError("Unexpected symlink: " + str(src))
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, dest)


def import_project(origin, src):
    name = origin["key"]
    if not re.fullmatch(r"[a-z0-9]+", name):
        raise ValueError("Invalid port folder")
    dest = PORTS / name
    assets = ASSETS / name
    for path in src.rglob("*"):
        rel = path.relative_to(src)
        if rel.parts[0] == ".git" or not path.is_file():
            continue
        if rel.parts[0] == "data" and len(rel.parts) > 1:
            install_file(path, assets.joinpath(*rel.parts[1:]))
        elif path.name in {"LICENSE", "COPYING", "NOTICE", "AUTHORS", "LICENSE.md"}:
            install_file(path, dest / rel)
            install_file(path, assets / "notices" / rel)
        elif name == "ndi" and rel.parts[:2] == ("lib", "ndi") and path.suffix == ".h":
            if "Permission is hereby granted" not in path.read_text(encoding="utf-8-sig")[:4000]:
                raise RuntimeError("Unexpected license on " + str(rel))
            install_file(path, dest / rel)
        elif name == "ndi" and rel.name == "NDI SDK License Agreement.pdf":
            install_file(path, assets / "notices" / rel.name)
        elif rel.parts[0] == "src" or len(rel.parts) == 1:
            if path.suffix in CODE or path.name in {"version.h.in", "plugin-support.c.in", "README.md", "buildspec.json"}:
                install_file(path, dest / rel)
    if not (dest / "LICENSE").is_file():
        raise RuntimeError("Missing license for " + name)
    (assets / "ORIGIN.txt").write_text(
        f"{origin['project']} {origin['version']}\nhttps://github.com/{origin['project']}/tree/{origin['commit']}\n"
        "StageKit maintains the OBS names, saved identifiers and integration separately. "
        "The upstream authors retain their copyrights. See notices and the StageKit source archive.\n",
        encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("projects", nargs="*", help="Restrict a refresh to these names")
    args = parser.parse_args()
    pins = json.loads((PORTS / "origins.json").read_text(encoding="utf-8"))
    for origin in pins["projects"]:
        if args.projects and origin["key"] not in args.projects:
            continue
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            checkout(origin, folder)
            import_project(origin, folder)
    if not args.projects or "masks" in args.projects:
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            checkout(pins["json_header"], folder)
            install_file(folder / "single_include/nlohmann/json.hpp", PORTS / "json/nlohmann/json.hpp")
            install_file(folder / "LICENSE.MIT", PORTS / "json/LICENSE.MIT")
            install_file(folder / "LICENSE.MIT", ASSETS / "masks/notices/nlohmann-json.LICENSE")


if __name__ == "__main__":
    main()
