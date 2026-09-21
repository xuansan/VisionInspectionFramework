"""Shared helpers; standard library only. Paths are explicit, no global configuration."""
import hashlib
import json
import re
from pathlib import Path
SKILL = Path(__file__).resolve().parents[1]

def inspect(root):
    root = Path(root).expanduser().resolve(strict=True)
    contract = json.loads((SKILL / "references/compatibility.json").read_text(encoding="utf-8"))
    errors = []
    for name, digest in contract["interface_sha256"].items():
        path = root / name
        if not path.is_file():
            errors.append("missing interface: " + name)
        elif hashlib.sha256(path.read_bytes().replace(b"\r\n", b"\n")).hexdigest() != digest:
            errors.append("interface changed: " + name)
    prerequisites = {
        "qt": (root / "开发环境/SDK/Qt/6.8.3/msvc2022_64/lib/cmake/Qt6/Qt6Config.cmake").is_file(),
        "onnxruntime": (root / "开发环境/SDK/ONNXRuntime/onnxruntime-win-x64-1.30.0/lib/onnxruntime.dll").is_file(),
        "sqlite": (root / "开发环境/SDK/native/lib/sqlite3.lib").is_file(),
        "curl": (root / "开发环境/SDK/native/lib/cmake/CURL").is_dir(),
        "json": (root / "开发环境/SDK/native/include/nlohmann/json.hpp").is_file(),
        "doctest": (root / "开发环境/SDK/native/include/doctest/doctest.h").is_file(),
    }
    return {"framework_root": str(root), "prerequisites": prerequisites,
            "missing_prerequisites": [name for name, present in prerequisites.items() if not present],
            "skill_version": contract["skill_version"],
            "compatible": not errors, "errors": errors,
            "validated_platform": contract["validated_platform"],
            "production_ready": False}

def require_compatible(root):
    result = inspect(root)
    if not result["compatible"]:
        raise ValueError("Framework/skill mismatch; review interfaces before updating fingerprints: " + "; ".join(result["errors"]))
    return Path(result["framework_root"])

def create(kind, root, destination, name):
    root = require_compatible(root)
    if not re.fullmatch(r"[a-z][a-z0-9-]{2,39}", name) or name.startswith("vision-") or name in {"con", "prn", "aux", "nul", *["com"+str(i) for i in range(1,10)], *["lpt"+str(i) for i in range(1,10)]}:
        raise ValueError("Name: 3-40 lowercase letters/digits/hyphens; start with letter; vision- is reserved")
    destination = Path(destination).expanduser().absolute()
    if destination.exists() or destination.is_symlink():
        raise ValueError("Destination already exists; refusing to overwrite")
    # Require the parent to exist: never create a mistaken directory hierarchy.
    parent = destination.parent.resolve(strict=True)
    destination = parent / destination.name
    for part in ("src", "apps", "plugins", "cmake", ".agents", ".git", "docs", "tests", "schemas", "tools", "dependencies", "examples"):
        if destination.is_relative_to(root / part):
            raise ValueError("Use a separate business directory, not framework internals")
    template = SKILL / "assets" / (kind + "-template")
    rendered = {}
    for source in sorted(template.rglob("*")):
        if source.is_symlink():
            raise ValueError("Template symlinks are not supported")
        if source.is_file():
            text = source.read_text(encoding="utf-8").replace("@PROJECT_NAME@", name)
            rendered[source.relative_to(template)] = text
    if not rendered:
        raise ValueError("Missing template")
    destination.mkdir()  # Exclusive creation; never merge into another project.
    for relative, text in rendered.items():
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        with target.open("x", encoding="utf-8", newline="\n") as stream:
            stream.write(text)
    marker = {"format": 1, "name": name, "kind": kind, "skill_version": "0.1.0", "production_ready": False}
    (destination / "vision-project.json").write_text(json.dumps(marker, indent=2) + "\n", encoding="utf-8")
    return {"created": str(destination), "name": name, "kind": kind, "next": "verify_project.py --framework-root <root> --project <created> --scope all"}
