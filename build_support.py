"""Shared build metadata, compiler policy, and package validation. Standard library only."""
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import zipfile


def git_output(root, *args):
    try:
        return subprocess.check_output(
            ["git", "-C", str(root), *args], text=True, stderr=subprocess.DEVNULL).strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def git_revision(root):
    top = git_output(root, "rev-parse", "--show-toplevel")
    if not top or Path(top).resolve() != Path(root).resolve():
        return None
    return git_output(root, "rev-parse", "HEAD")


def source_inputs(root):
    root = Path(root)
    files = {root / "AMBuildScript", root / "product.version"}
    files.update(p for p in root.iterdir() if p.is_file() and p.suffix in (".py", ".json"))
    for folder in ("extension", "patches", "configs", "gamedata", "scripting", "buildbot"):
        files.update(p for p in (root / folder).rglob("*")
                     if p.is_file() and "__pycache__" not in p.parts
                     and p.name != "version_auto.h")
    # Dependency trees have their own revision pins; do not hash cloned sources.
    files.update(p for p in (root / "third_party").iterdir() if p.is_file())
    return sorted(files)


def source_identity(root):
    root = Path(root).resolve()
    files = source_inputs(root)
    digest = hashlib.sha256()
    for path in files:
        digest.update(path.relative_to(root).as_posix().encode() + b"\0")
        digest.update(hashlib.sha256(path.read_bytes()).digest())
    fingerprint = digest.hexdigest()
    revision = git_revision(root)
    status = git_output(root, "status", "--porcelain", "--untracked-files=normal") if revision else None
    dirty = bool(status) if status is not None else None
    label = (revision[:12] + ("-dirty" if dirty else "")) if revision else "src-" + fingerprint[:12]
    return {"revision": revision, "dirty": dirty, "source_sha256": fingerprint,
            "label": label}, files


def sourcemod_version(path):
    version = (Path(path) / "product.version").read_text().strip()
    match = re.match(r"^(\d+\.\d+)\.", version)
    if not match:
        raise RuntimeError(f"Unrecognized SourceMod product.version: {version!r}")
    return match[1]


def git_config_inputs(root):
    result = []
    if not git_revision(root):
        return result
    for name in ("HEAD", "index", git_output(root, "symbolic-ref", "-q", "HEAD")):
        if not name:
            continue
        path = git_output(root, "rev-parse", "--git-path", name)
        if path:
            path = Path(path)
            if not path.is_absolute():
                path = Path(root) / path
            if path.is_file():
                result.append(path)
    return result


def version_header(product, identity, sm_version=None, sm_revision=None):
    match = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)([^\r\n]*)", product.strip())
    if not match:
        raise RuntimeError("Invalid product.version")
    major, minor, release, suffix = match.groups()
    tag = suffix + "-" + identity["label"]
    unique = f"{identity['revision'] or 'archive'};src:{identity['source_sha256']}"
    if identity["dirty"]:
        unique += ";dirty"
    if sm_version:
        tag += "-sm" + sm_version
        unique += f";sm:{sm_version}:{sm_revision or 'custom-sources'}"
    return "\n".join([
        "#pragma once",
        f"#define SM_BUILD_TAG {json.dumps(tag)}",
        f"#define SM_BUILD_UNIQUEID {json.dumps(unique)}",
        f'#define SM_VERSION "{major}.{minor}.{release}"',
        "#define SM_FULL_VERSION SM_VERSION SM_BUILD_TAG",
        f"#define SM_FILE_VERSION {major},{minor},{release},0", "",
    ])


def compiler_policy(cxx, optimize, symbols):
    # AMBuild 2.2's pinned API has no public "off" value for symbol_files.
    # Its builders explicitly test symbol_files_ for None. Keep that
    # compatibility workaround here, rather than leaving hidden vendor defaults.
    if symbols:
        cxx.symbol_files = "separate" if cxx.family == "msvc" else "bundled"
        if cxx.family == "gcc":
            cxx.cflags += ["-g"]
    else:
        cxx.symbol_files_ = None
    if optimize:
        cxx.defines += ["NDEBUG"]
        cxx.cflags += ["/O2"] if cxx.family == "msvc" else ["-O3"]


def binary_architecture(path):
    with Path(path).open("rb") as stream:
        header = stream.read(64)
        if header.startswith(b"\x7fELF") and len(header) >= 20:
            endian = "<" if header[5] == 1 else ">"
            machine = struct.unpack_from(endian + "H", header, 18)[0]
            return {3: "x86", 62: "x86_64"}.get(machine)
        if header.startswith(b"MZ") and len(header) >= 64:
            stream.seek(struct.unpack_from("<I", header, 60)[0])
            pe = stream.read(6)
            if pe[:4] == b"PE\0\0":
                return {0x14c: "x86", 0x8664: "x86_64"}.get(struct.unpack_from("<H", pe, 4)[0])
    return None


def package_archives(build, sm_version, arches, platform, symbols):
    build = Path(build)
    package = build / "package"
    info_path = package / "build-info.json"
    info = json.loads(info_path.read_text())
    if info["sourcemod"]["version"] != sm_version:
        raise RuntimeError("Package SourceMod target does not match requested target")
    if set(info["architectures"]) != set(arches):
        raise RuntimeError("Package architecture manifest does not match requested architectures")
    if info["platform"] != platform or info["symbols"] != symbols:
        raise RuntimeError("Package platform/symbol settings do not match requested build")
    extension = "dll" if platform == "windows" else "so"
    binaries_info = {}
    for arch in arches:
        relative = Path("addons/sourcemod/extensions")
        if arch == "x86_64":
            relative /= "x64"
        binary = package / relative / f"accelerator.ext.{extension}"
        if binary_architecture(binary) != arch:
            raise RuntimeError(f"Missing or incorrect architecture: {binary}")
        binaries_info[arch] = hashlib.sha256(binary.read_bytes()).hexdigest()
        if symbols:
            debug_binary = build / "symbols" / arch / binary.name
            if not debug_binary.is_file() or hashlib.sha256(debug_binary.read_bytes()).hexdigest() != binaries_info[arch]:
                raise RuntimeError(f"Symbol archive binary does not match installation binary: {arch}")
        if symbols and platform == "windows" and not (build / "symbols" / arch / "accelerator.ext.pdb").is_file():
            raise RuntimeError(f"Missing matching Windows PDB for {arch}")
    # Reject stale binary artifacts rather than accidentally shipping another target.
    binaries = list((package / "addons/sourcemod/extensions").rglob(f"*.{extension}"))
    if len(binaries) != len(arches):
        raise RuntimeError("Unexpected binaries in package; use a fresh build directory")
    dist = build / "dist"
    dist.mkdir(exist_ok=True)
    # Always include the content fingerprint, even for two dirty builds at the same commit.
    source = info["source"]
    info["binary_sha256"] = binaries_info
    info_path.write_text(json.dumps(info, indent=2) + "\n")
    if symbols:
        (build / "symbols" / "build-info.json").write_text(json.dumps(info, indent=2) + "\n")
    binary_id = hashlib.sha256(json.dumps(binaries_info, sort_keys=True).encode()).hexdigest()[:12]
    stem = f"accelerator-sm{sm_version}-{platform}-{'-'.join(arches)}-{source['label']}-{source['source_sha256'][:12]}-{binary_id}"
    for kind, folder in [("package", package)] + ([("symbols", build / "symbols")] if symbols else []):
        if not folder.is_dir():
            raise RuntimeError(f"Missing {kind} directory: {folder}")
        output = dist / f"{stem}-{kind}.zip"
        with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
            for path in sorted(folder.rglob("*")):
                if path.is_file():
                    archive.write(path, path.relative_to(folder).as_posix())
    return dist
