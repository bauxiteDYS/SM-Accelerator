"""Create an immutable, patched build copy without changing the source checkout."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def source_files(source):
    for directory, dirs, files in os.walk(source):
        dirs[:] = sorted(name for name in dirs if name != ".git")
        for name in sorted(files):
            if name != ".git":
                yield Path(directory) / name


def prepare(source, patches, destination):
    source, destination = Path(source), Path(destination)
    patches = [Path(path) for path in patches]
    files = list(source_files(source))
    if not (source / "src" / "client").is_dir():
        raise RuntimeError("Breakpad sources are missing; run build.py to download dependencies.")
    # Content-addressed generations let multiple build configurations coexist.
    digest = hashlib.sha256()
    digest.update(Path(__file__).read_bytes())
    for path in files:
        digest.update(path.relative_to(source).as_posix().encode())
        digest.update(b"\0")
        digest.update(hashlib.sha256(path.read_bytes()).digest())
    for patch in patches:
        digest.update(patch.name.encode())
        digest.update(hashlib.sha256(patch.read_bytes()).digest())
    fingerprint = digest.hexdigest()
    # Keep Windows compiler paths short; verify the full digest on cache hits.
    target = destination / ("bp-" + fingerprint[:16])
    if (target / ".complete").is_file():
        if (target / ".complete").read_text() != fingerprint:
            raise RuntimeError("Breakpad staging key collision or invalid cache; move that generation aside.")
        return target, files
    destination.mkdir(parents=True, exist_ok=True)
    # TemporaryDirectory only removes the private staging directory it creates.
    with tempfile.TemporaryDirectory(prefix="breakpad-staging-", dir=destination) as temp:
        work = Path(temp) / "source"
        shutil.copytree(source, work, ignore=shutil.ignore_patterns(".git"))
        # A private repository prevents git apply from finding the parent project.
        subprocess.run(["git", "init", "--quiet", str(work)], check=True)
        for patch in patches:
            check = subprocess.run(["git", "-C", str(work), "apply", "--check", str(patch)],
                                   capture_output=True)
            if check.returncode == 0:
                subprocess.run(["git", "-C", str(work), "apply", str(patch)], check=True)
            else:
                # Older builds may already have patched the source checkout.
                reverse = subprocess.run(
                    ["git", "-C", str(work), "apply", "--reverse", "--check", str(patch)],
                    capture_output=True)
                if reverse.returncode != 0:
                    raise RuntimeError(f"Cannot apply {patch.name} to the build copy:\n"
                                       + check.stderr.decode("utf-8", errors="replace"))
        (work / ".complete").write_text(fingerprint)
        try:
            work.rename(target)
        except OSError:
            # Another configure process may have published the same generation.
            if not (target / ".complete").is_file() or (target / ".complete").read_text() != fingerprint:
                raise
    return target, files
