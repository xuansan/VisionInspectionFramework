"""Build an extension with the framework source; formal verification includes the full regression."""
import argparse
import datetime
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
from common import require_compatible

def cmake_path(explicit):
    if explicit:
        path = Path(explicit).resolve(strict=True)
    else:
        vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
        if vswhere.is_file():
            vs = subprocess.run([str(vswhere), "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"], check=True, capture_output=True, text=True).stdout.strip()
            path = Path(vs) / "Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
        else:
            found = shutil.which("cmake")
            if not found:
                raise ValueError("CMake missing; pass --cmake or prepare the documented VS2022 toolchain")
            path = Path(found)
    if not path.is_file():
        raise ValueError("CMake not found: " + str(path))
    return path

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--framework-root", required=True)
    p.add_argument("--project", required=True)
    p.add_argument("--scope", choices=["all", "extension"], default="all", help="extension is a development check, not full acceptance")
    p.add_argument("--cmake")
    a = p.parse_args()
    logs = None
    results = []
    success = False
    try:
        root = require_compatible(a.framework_root)
        project = Path(a.project).resolve(strict=True)
        marker = json.loads((project / "vision-project.json").read_text(encoding="utf-8"))
        name = marker["name"]
        if marker.get("format") != 1 or not re.fullmatch(r"[a-z][a-z0-9-]{2,39}", name):
            raise ValueError("Invalid generated project identity")
        if marker.get("kind") not in ("application", "output-plugin"):
            raise ValueError("Unsupported template kind")
        if os.name != "nt":
            raise ValueError("This template is validated only on Windows; no Linux success claim")
        cmake = cmake_path(a.cmake)
        ctest = cmake.with_name("ctest.exe")
        if not ctest.is_file():
            raise ValueError("CTest missing beside CMake")
        for output_path in (project / "out", project / "out/build", project / "out/validation"):
            if not output_path.resolve().is_relative_to(project):
                raise ValueError("Build/log directory escapes the business project")
        build = project / "out/build"
        stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
        logs = project / "out/validation" / stamp
        logs.mkdir(parents=True)
        env = dict(os.environ)
        for key in list(env):
            if key.upper() in ("QTDIR", "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH", "CMAKE_PREFIX_PATH", "CFLAGS", "CXXFLAGS", "LDFLAGS"):
                del env[key]
        jobs = []
        if a.scope == "all":
            node = shutil.which("node")
            if not node:
                raise ValueError("Node required to invoke the framework full verification runner")
            jobs.append(("framework-all", [node, str(root / "tools/build.cjs"), "windows-all"], 1200))
        jobs.extend([
            ("configure", [str(cmake), "-S", str(root), "-B", str(build), "-G", "Visual Studio 17 2022", "-A", "x64", "-T", "v143", "-DBUILD_TESTING=ON", "-DVISION_BUILD_GUI=ON", "-DVISION_BUILD_LOCAL_IPC=ON", "-DVISION_USE_LOCAL_SDK=ON", "-DVISION_ALLOW_DOWNLOADS=OFF", "-DVISION_EXTENSION_DIR=" + project.as_posix()], 180),
            ("build", [str(cmake), "--build", str(build), "--config", "Release", "--parallel", "4"], 600),
            ("extension-tests", [str(ctest), "--test-dir", str(build), "-C", "Release", "--output-on-failure", "--no-tests=error", "-R", "^user[.]" + name + "[.]"], 180),
        ])
        for label, command, timeout in jobs:
            print("Running " + label, flush=True)
            with (logs / (label + ".log")).open("w", encoding="utf-8") as log:
                try:
                    result = subprocess.run(command, cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=timeout, creationflags=subprocess.CREATE_NO_WINDOW)
                    code = result.returncode
                except subprocess.TimeoutExpired:
                    code = 124
            results.append({"name": label, "exit_code": code, "passed": code == 0})
            if code:
                raise ValueError(label + " failed; see " + str(logs / (label + ".log")))
        success = True
        print("Verification logs: " + str(logs))
        return 0
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(str(error), file=sys.stderr)
        return 1
    finally:
        if logs:
            (logs / "summary.json").write_text(json.dumps({"scope": a.scope, "passed": success, "production_ready": False, "results": results}, indent=2), encoding="utf-8")
if __name__ == "__main__":
    raise SystemExit(main())
