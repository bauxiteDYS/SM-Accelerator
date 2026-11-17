#!/usr/bin/env python3
"""Bootstrap pinned dependencies and build/package Accelerator on Windows or Linux."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import venv

from build_support import git_output, package_archives, sourcemod_version

ROOT = Path(__file__).resolve().parent
DEPS = ROOT / ".build-deps"
DEPENDENCIES = json.loads((ROOT / "dependencies.json").read_text())
SM_REFS = DEPENDENCIES["sourcemod"]["versions"]


def run(args, cwd=None):
    print("+ " + subprocess.list2cmdline([str(arg) for arg in args]), flush=True)
    subprocess.run([str(arg) for arg in args], cwd=cwd or ROOT, check=True)


def checkout(url, ref, path):
    """Verify or create a pinned checkout; never reset an existing checkout."""
    if not (path / ".git").exists():
        if path.exists() and any(path.iterdir()):
            raise RuntimeError(f"{path} is not a Git checkout and is not empty; refusing to overwrite it.")
        path.mkdir(parents=True, exist_ok=True)
        run(["git", "init", path])
        run(["git", "-C", path, "remote", "add", "origin", url])
        run(["git", "-C", path, "fetch", "--depth", "1", "origin", ref])
        run(["git", "-C", path, "checkout", "--detach", "FETCH_HEAD"])
    actual = git_output(path, "rev-parse", "HEAD")
    if actual != ref:
        raise RuntimeError(f"{path}: expected {ref}, found {actual}. Move the checkout aside or explicitly update it; no reset was performed.")
    dirty = git_output(path, "status", "--porcelain", "--untracked-files=normal")
    if dirty is None or dirty:
        raise RuntimeError(f"{path} is not a clean, readable dependency checkout. Preserve your edits and move it aside; it will not be reset.")


def initialize_submodules(path):
    state = git_output(path, "submodule", "status", "--recursive")
    if state is None:
        raise RuntimeError(f"Cannot inspect submodules in {path}")
    # git_output strips the initial space on the first line, but preserves +/-
    # status prefixes. Never switch an existing submodule's revision implicitly.
    if any(line.startswith(("+", "U")) for line in state.splitlines()):
        raise RuntimeError(f"Submodules in {path} differ from recorded revisions; resolve them explicitly.")
    if any(line.startswith("-") for line in state.splitlines()):
        run(["git", "-C", path, "submodule", "update", "--init", "--recursive"])
    dirty = git_output(path, "status", "--porcelain", "--untracked-files=normal")
    if dirty is None or dirty:
        raise RuntimeError(f"Dependency or submodule changes found in {path}; no reset was performed.")


def prepare_library(name):
    spec = DEPENDENCIES[name]
    path = ROOT / "third_party" / name
    if not (path / ".git").exists() and (ROOT / ".git").exists():
        tracked = subprocess.check_output(
            ["git", "ls-files", "--stage", "--", f"third_party/{name}"], cwd=ROOT, text=True)
        if tracked.startswith("160000 "):
            if tracked.split()[1] != spec["ref"]:
                raise RuntimeError(f"{name}: submodule pin differs from dependencies.json; update the pins together.")
            run(["git", "submodule", "update", "--init", "--recursive", "--", f"third_party/{name}"])
    checkout(spec["url"], spec["ref"], path)
    initialize_submodules(path)


def prepare_ambuild():
    spec = DEPENDENCIES["ambuild"]
    source = DEPS / ("ambuild-" + spec["ref"][:12])
    checkout(spec["url"], spec["ref"], source)
    env = DEPS / f"venv-py{sys.version_info.major}{sys.version_info.minor}-{spec['ref'][:12]}"
    python = env / ("Scripts/python.exe" if sys.platform == "win32" else "bin/python")
    if not python.exists():
        venv.EnvBuilder(with_pip=True).create(env)
    # Check the installed package as well as the revision stamp: a damaged
    # environment should not be considered ready solely because a file exists.
    stamp = env / "ambuild-revision.txt"
    if not stamp.is_file() or stamp.read_text() != spec["ref"]:
        run([python, "-m", "pip", "install", "--disable-pip-version-check", "--no-deps", source])
        stamp.write_text(spec["ref"])
    run([python, "-c", "from ambuild2 import run; assert run.HasAPI('2.2')"])
    return python


def arguments(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sm-version", choices=SM_REFS, default="1.12")
    parser.add_argument("--sm-path", type=Path, help="Use custom SourceMod sources matching --sm-version; preserve local edits")
    parser.add_argument("--arch", choices=("x86", "x86_64", "both"), default="both")
    parser.add_argument("--no-debug", action="store_true", help="Omit compiler debug symbols; optimization remains enabled")
    parser.add_argument("--jobs", type=int, default=0, help="Parallel jobs (0 = automatic)")
    return parser.parse_args(argv)


def main(argv=None):
    args = arguments(argv)
    if args.jobs < 0:
        raise RuntimeError("--jobs must be zero or positive.")
    if sys.platform not in ("win32", "linux"):
        raise RuntimeError("Only native Windows and Linux builds are supported.")
    if not shutil.which("git"):
        raise RuntimeError("Install Git and make sure it is on PATH.")
    if sys.platform == "linux" and not shutil.which(os.environ.get("CXX", "c++")):
        raise RuntimeError("Install a C++20 compiler first; see README.md.")
    if sys.platform == "win32" and shutil.which("cl"):
        active_arch = os.environ.get("VSCMD_ARG_TGT_ARCH")
        requested_arch = "x64" if args.arch == "x86_64" else args.arch
        if args.arch == "both" or (active_arch and active_arch != requested_arch):
            raise RuntimeError("An architecture-specific MSVC environment is active. Open regular PowerShell so AMBuild can select the requested x86/x64 tools.")
    ref = SM_REFS[args.sm_version]
    sm = args.sm_path.resolve() if args.sm_path else DEPS / f"sourcemod-{args.sm_version}-{ref[:12]}"
    if args.sm_path:
        if not (sm / "public/smsdk_ext.h").is_file():
            raise RuntimeError("--sm-path requires SourceMod source code, not a server installation.")
    else:
        checkout(DEPENDENCIES["sourcemod"]["url"], ref, sm)
        initialize_submodules(sm)
    if sourcemod_version(sm) != args.sm_version:
        raise RuntimeError("SourceMod product.version does not match --sm-version; choose the correct target.")
    for name in ("breakpad", "zlib"):
        prepare_library(name)
    python = prepare_ambuild()
    platform = "windows" if sys.platform == "win32" else "linux"
    mode = "nosymbols" if args.no_debug else "symbols"
    custom = "-custom" if args.sm_path else ""
    build = ROOT / "build" / f"{platform}-sm{args.sm_version}-{args.arch}-{mode}{custom}"
    build.mkdir(parents=True, exist_ok=True)
    arches = ["x86", "x86_64"] if args.arch == "both" else [args.arch]
    configure = [python, ROOT / "configure.py", f"--sm-path={sm}",
                 "--sm-version=" + ("custom" if args.sm_path else args.sm_version),
                 "--enable-optimize", "--targets=" + ",".join(arches)]
    if not args.no_debug:
        configure.append("--enable-debug")
    run(configure, cwd=build)
    run([python, "-c", "from ambuild2 import run; run.cli_run()", "-j", str(args.jobs)], cwd=build)
    dist = package_archives(build, args.sm_version, arches, platform, not args.no_debug)
    print(f"\nInstallable files: {build / 'package'}\nArchives: {dist}")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, subprocess.CalledProcessError, ValueError) as error:
        print(f"\nBuild failed: {error}\nSee README.md for prerequisites and troubleshooting.", file=sys.stderr)
        sys.exit(1)
