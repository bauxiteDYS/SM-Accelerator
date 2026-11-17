# Complete build guide

This guide builds **Accelerator**, not SourceMod or a game server. The helper
downloads SourceMod's source headers and SDK wrapper and builds the extension
plus its bundled libraries.

**Verification status:** the build paths and compatibility targets are configured,
but complete Windows/Linux builds and live server compatibility have not yet
been verified for this modified code. Passing helper tests is not proof that a
server can load the extension or produce a usable dump. See
[COMPATIBILITY.md](COMPATIBILITY.md) for the live verification checklist.

## Contents

1. [Choose the target](#1-choose-the-target)
2. [Get the modified source](#2-get-the-modified-source)
3. [Method A: GitHub Actions — no local compiler](#method-a-github-actions--no-local-compiler)
4. [Method B: native Windows build](#method-b-native-windows-build)
5. [Method C: native Linux build](#method-c-native-linux-build)
6. [Method D: Linux container — Docker or Podman](#method-d-linux-container--docker-or-podman)
7. [Method E: advanced manual AMBuild](#method-e-advanced-manual-ambuild)
8. [Options and output files](#options-and-output-files)
9. [Install and verify](#install-and-verify)
10. [Rebuild, dependencies, and safe cleanup](#rebuild-dependencies-and-safe-cleanup)
11. [Tests](#tests)
12. [Troubleshooting](#troubleshooting)

## 1. Choose the target

| Choice | Meaning |
| --- | --- |
| Windows / Linux | Operating system of the **game server**, not necessarily your desktop. Windows builds produce DLLs; Linux builds produce `.so` files. |
| `x86` | 32-bit server process. |
| `x86_64` | 64-bit server process. |
| `both` | Build both architectures. This is the default; both toolchains must be available. |
| `--sm-version 1.11` | Compile against the pinned 1.11 SDK, extension API 8. |
| `--sm-version 1.12` | Compile against the pinned 1.12 SDK, extension API 8. This is the default. |
| `--sm-version 1.13` | Compile against the pinned 1.13 development SDK, extension API 11. |

A 64-bit OS can run a 32-bit server. If unsure, build both and let SourceMod use
the appropriate extension directory.

A “1.12 build” describes the SDK used for compilation, not a runtime check
requiring exactly 1.12. A binary may work on both 1.11 and 1.12, but that must be
tested before advertising shared support. The current build system packages each
target separately. Do not use an older API-8 binary on the pinned 1.13 runtime,
or a 1.13 API-11 binary on older SourceMod versions. Earlier 1.13 snapshots may
also require a matching custom SDK.

`dependencies.json` contains the exact source revisions. See
[COMPATIBILITY.md](COMPATIBILITY.md) before choosing a custom SDK.

### Which method should I use?

| Method | Prerequisites on your computer | Output |
| --- | --- | --- |
| A: Actions | GitHub account and the modified source in a repository | Windows, Linux, or both |
| B: native Windows | Git, Python, Visual Studio C++ Build Tools | Windows |
| C: native Linux | Git, Python, C++20 compiler and architecture libraries | Linux |
| D: container | Docker or Podman and the modified source | Linux |
| E: manual AMBuild | Same compiler prerequisites as B/C; more manual steps | Host OS |

There is no native Windows-to-Linux or Linux-to-Windows cross-compilation switch
in `build.py`. Use Actions or a Linux container to build Linux binaries from
Windows. These instructions assume an x86/x64 host; ARM hosts and emulated
toolchains are not covered.

## 2. Get the modified source

Make sure the source includes **these modifications**, particularly `build.py`,
`build_support.py`, `dependencies.json`, and the new workflows. Cloning the
unmodified upstream project will not include these changes.

### From your Git repository

Replace the example account/repository with your actual repository URL:

```sh
git clone https://github.com/YOUR_ACCOUNT/YOUR_REPOSITORY.git accelerator
cd accelerator
```

You do not have to supply `--recursive` here: the helper prepares the required
dependency checkouts. If your Git submodule pins differ from `dependencies.json`,
the helper reports the mismatch rather than silently changing versions.

### From a ZIP

1. Download a ZIP of the modified source.
2. Extract the complete source tree, including hidden `.github` files if you
   intend to put it on GitHub.
3. Open a terminal in the extracted folder containing `build.py`.

Source ZIPs normally have empty Breakpad/zlib submodule folders. The helper can
populate them, but **Git is still required**, even when the main source came
from a ZIP. Nonempty dependency folders without Git metadata are not overwritten.

Prefer a short source path such as `C:\src\accelerator` on Windows. Spaces are
supported, but deeply nested paths can hit compiler or Windows path limits.

## Method A: GitHub Actions — no local compiler

This is the easiest method for producing both platforms without installing
compilers on your computer.

1. Create a repository containing this modified source, or commit the changes
   to your fork. Include the `.github/workflows` files.
2. Ensure the workflows are present on the repository's default branch.
3. Open the repository's **Actions** tab. Enable Actions if prompted.
4. Select **Build packages**, then **Run workflow**.
5. Select the source branch and:
   - **platform:** `windows`, `linux`, or `both`;
   - **sourcemod:** `1.11`, `1.12`, `1.13`, or `all`;
   - **architecture:** `x86`, `x86_64`, or `both`.
6. Start the run. The default selection creates six platform/SDK jobs; each
   builds both architectures.
7. Open each job's logs if a job fails. Look for the first actual configure,
   compiler, test, or packaging error rather than only the final failure message.
8. After success, download the artifacts from the run page:
   - `accelerator-<platform>-sm<version>-<architecture>`: installation ZIP;
   - `symbols-<platform>-sm<version>-<architecture>`: matching debugging ZIP.

The installation ZIP contains `addons/` at its root and a `build-info.json`.
If your download interface adds an outer archive, unpack it until you reach the
installation ZIP containing that layout. Keep the separate symbol archive with
the installation archive.

**Build packages** does not need publication credentials. The separate **Release**
workflow can send installation archives to the pre-existing build service if the
repository has `UPLOAD_PASSWORD` configured. You do not need that workflow or
secret just to compile/download artifacts.

Hosted CI requires the native helper tests and real-Git patch tests to execute.
Missing prerequisites cause failure rather than a silently incomplete test pass.

## Method B: native Windows build

### B1. Install prerequisites

On a Windows x64 development machine, install:

1. [Git for Windows](https://git-scm.com/downloads/win), with command-line PATH
   integration enabled.
2. [Python for Windows](https://www.python.org/downloads/windows/).
   Python **3.12 is recommended**, matching Windows CI. Enable its PATH option,
   or select that interpreter using the Python launcher.
3. Visual Studio 2022 or its standalone **Build Tools**. In Visual Studio
   Installer select **Desktop development with C++**, including:
   - MSVC v143 x86/x64 C++ build tools;
   - a supported Windows SDK.

Neither a SourceMod server installation nor a manually installed AMBuild is
required for compilation.

Close and reopen your terminal after installation. In **regular PowerShell**:

```powershell
git --version
python --version
```

If `python` selects the wrong installation, use the appropriate launcher command,
for example `py -3.12`, in place of `python` throughout these steps.

**Do not use Developer PowerShell/Developer Command Prompt for a combined build.**
AMBuild detects Visual Studio and selects x86/x64 environments itself. An existing
`cl.exe` environment can lock detection to a single architecture. It is normal
for `cl` not to be available in regular PowerShell.

### B2. Open the source folder and compile

```powershell
Set-Location "C:\src\accelerator"
python build.py --sm-version 1.12 --arch both
```

The first run downloads pinned dependencies and creates a private Python
environment. It can take substantially longer than subsequent builds.

Other examples:

```powershell
python build.py --sm-version 1.11 --arch both
python build.py --sm-version 1.13 --arch x86_64
python build.py --sm-version 1.12 --arch x86 --jobs 4
```

No activation of the helper's virtual environment is necessary.

### B3. Locate the output

For the first example:

```text
build/windows-sm1.12-both-symbols/
    package/
    symbols/
    dist/
```

Install the ZIP ending in `-package.zip` from `dist/`. Keep the ZIP ending in
`-symbols.zip`. See [Options and output files](#options-and-output-files).

## Method C: native Linux build

These commands target an **amd64/x86_64 Ubuntu 24.04** development machine.
Other distributions need equivalent compiler, Python, and multilib packages.
Native builds inherit the host's runtime-library requirements; prefer the
Debian 11 container/Actions method when distributing to older Linux hosts.

### C1. Install prerequisites

```sh
sudo dpkg --add-architecture i386
sudo apt-get update
sudo apt-get install git python3 python3-venv python3-pip clang g++-multilib libc6-dev-i386 linux-libc-dev:i386
```

The multilib packages provide the 32-bit headers/libraries needed for `both` or
`x86`. If you only need x86_64, a 64-bit C++20 toolchain is sufficient, but the
command above is the simplest setup for all supported architecture choices.

Check tools:

```sh
git --version
python3 --version
clang++ --version
```

Use `sudo` for system-package installation, **not for `build.py`**.

### C2. Compile

From the source directory:

```sh
CC=clang CXX=clang++ python3 build.py --sm-version 1.12 --arch both
```

Other targets:

```sh
CC=clang CXX=clang++ python3 build.py --sm-version 1.11 --arch both
CC=clang CXX=clang++ python3 build.py --sm-version 1.13 --arch x86_64
```

A sufficiently recent GCC can also be selected explicitly:

```sh
CC=gcc CXX=g++ python3 build.py --sm-version 1.12 --arch both
```

The C++ compiler must support C++20 and the requested architectures. Clang is the
Linux CI compiler and is the recommended starting point.

### C3. Locate the output

```text
build/linux-sm1.12-both-symbols/
    package/
    symbols/
    dist/
```

You can inspect the generated binaries without loading them:

```sh
file build/linux-sm1.12-both-symbols/package/addons/sourcemod/extensions/accelerator.ext.so
file build/linux-sm1.12-both-symbols/package/addons/sourcemod/extensions/x64/accelerator.ext.so
```

The helper also checks the ELF machine type as part of archive creation.

## Method D: Linux container — Docker or Podman

This produces **Linux binaries only**. You do not need a compiler installed on
the host; the container supplies the Linux build environment.

Requirements:

- Docker with a running daemon, or a working Podman installation;
- access to pull `ghcr.io/alliedmodders/build-containers/debian11-clang22:latest`;
- network access for dependency/system-package downloads;
- write access to a complete copy of this modified source.

Use a dedicated source folder for container builds. In particular, avoid sharing
one `.build-deps` tree between native Windows Python and Linux containers.
Containers may create root-owned files on Linux bind mounts; use a disposable
build checkout and account for ownership when accessing the output.

### D1. From a Linux/WSL shell

From the source root, choose either engine:

```sh
CONTAINER_ENGINE=docker bash dockerbuild/accelerator_docker_build.sh --sm-version 1.12 --arch both
```

```sh
CONTAINER_ENGINE=podman bash dockerbuild/accelerator_docker_build.sh --sm-version 1.13 --arch x86_64
```

If `CONTAINER_ENGINE` is omitted, the wrapper tries Docker, then Podman.
The wrapper is retained for convenience but delegates to `build.py`; it is not
a separate compilation pipeline.

### D2. From Windows PowerShell with Docker Desktop

Start Docker Desktop in **Linux containers** mode. In a dedicated source copy:

```powershell
Set-Location "C:\src\accelerator-container"
docker version
docker run --rm --mount "type=bind,source=$($PWD.Path),target=/accelerator" --workdir /accelerator ghcr.io/alliedmodders/build-containers/debian11-clang22:latest bash dockerbuild/_accelerator_docker_build_internal.sh --sm-version 1.12 --arch both
```

Docker Desktop must be permitted to share that directory. This explicit command
avoids needing a host Bash installation or the native Windows C++ tools.

The source is mounted into the container at `/accelerator`, so output persists
on the host at:

```text
build/linux-sm1.12-both-symbols/dist/
```

`--rm` removes the temporary container after exit; it does not remove the mounted
source/output. The container entry point installs prerequisites and invokes the
same helper. It does not force-reset dependency checkouts or delete old builds.

The image uses the project's Debian 11/clang-22 environment. This targets an older
Linux runtime baseline than a native Ubuntu 24.04 build, but still needs testing
on the actual server.

## Method E: advanced manual AMBuild

Use this only when you need to inspect configuration/compiler commands or
integrate with a custom development workflow. Methods B/C are easier and also
validate/create archives automatically.

This method uses the helper's dependency preparation functions so it does not
duplicate revision pins. Do not download arbitrary current SourceMod/Breakpad
sources and assume they match these instructions.

### E1. Prepare sources and AMBuild without compiling

Install the native prerequisites for your host as described above. Open a
terminal at the Accelerator source root. Start Python:

```text
python
```

On Linux, use `python3`. Paste this Python code into the interpreter, changing
`target` if required:

```python
import build

target = "1.12"
spec = build.DEPENDENCIES["sourcemod"]
ref = build.SM_REFS[target]
sm = build.DEPS / f"sourcemod-{target}-{ref[:12]}"
build.checkout(spec["url"], ref, sm)
build.initialize_submodules(sm)
build.prepare_library("breakpad")
build.prepare_library("zlib")
build_python = build.prepare_ambuild()
print("Source root:", build.ROOT)
print("SourceMod sources:", sm)
print("Build Python:", build_python)
exit()
```

Record the three printed absolute paths. These are the source root, prepared SDK,
and Python executable in the environment containing the pinned AMBuild.

### E2. Configure and compile

Use a new build directory; do not configure in the source root or reuse a
directory for incompatible compiler/symbol settings. The examples below target
1.12 and both architectures. Match the target/version flags to E1.

**Windows, regular PowerShell:**

Replace the example paths with the values printed by E1:

```powershell
$SourceRoot = "C:\src\accelerator"
$SmSource = "C:\absolute\path\printed\for\sourcemod"
$BuildPython = "C:\absolute\path\printed\for\venv\Scripts\python.exe"
New-Item -ItemType Directory -Force "$SourceRoot\build\manual-sm1.12" | Out-Null
Set-Location "$SourceRoot\build\manual-sm1.12"
& $BuildPython "$SourceRoot\configure.py" "--sm-path=$SmSource" --sm-version 1.12 "--targets=x86,x86_64" --enable-optimize --enable-debug
& $BuildPython -c "from ambuild2 import run; run.cli_run()" -j 4
```

**Linux:**

Replace the example paths with the values printed by E1:

```sh
SOURCE_ROOT="/absolute/path/to/accelerator"
SM_SOURCE="/absolute/path/printed/for/sourcemod"
BUILD_PYTHON="/absolute/path/printed/for/venv/bin/python"
mkdir -p "$SOURCE_ROOT/build/manual-sm1.12"
cd "$SOURCE_ROOT/build/manual-sm1.12"
CC=clang CXX=clang++ "$BUILD_PYTHON" "$SOURCE_ROOT/configure.py" "--sm-path=$SM_SOURCE" --sm-version 1.12 --targets x86,x86_64 --enable-optimize --enable-debug
"$BUILD_PYTHON" -c "from ambuild2 import run; run.cli_run()" -j 4
```

`--enable-debug` requests symbols, not a debug CRT/runtime. Omit it for no symbols.
`--enable-optimize` enables optimization independently. Explicit `--targets`
prevents a missing architecture from being silently omitted. The old
`--disable-auto-versioning` flag is no longer supported.

If configure fails, stop and inspect its errors; do not treat a later AMBuild run
against an old configuration as verification of the new settings.

### E3. Inspect/package the manual build

After a successful build, `package/` and `symbols/` appear under the manual build
directory. AMBuild itself does not invoke the helper's final ZIP validation.

To run that validation and generate `dist/`, stay in the manual build directory
and start the **Build Python** printed in E1. In Python:

```python
import sys
from pathlib import Path

sys.path.insert(0, r"/absolute/path/to/accelerator")
from build_support import package_archives

platform = "windows" if sys.platform == "win32" else "linux"
print(package_archives(Path.cwd(), "1.12", ["x86", "x86_64"], platform, True))
exit()
```

Replace the source path with E1's source root (a Windows path is fine in that raw
string). Match the version, architectures, and final boolean to E2; `True` means
symbols were enabled. Manually packaging mismatched targets or absent symbol
files is rejected.

## Options and output files

Run `python build.py --help` (`python3` on Linux) for the current interface.

| Option | Default | Purpose |
| --- | --- | --- |
| `--sm-version 1.11/1.12/1.13` | `1.12` | Select the pinned SourceMod SDK target. |
| `--arch x86/x86_64/both` | `both` | Select server process architecture(s). |
| `--jobs N` | `0` | Parallel compilation jobs; zero lets AMBuild decide. |
| `--no-debug` | off | Omit debug symbols; keep optimization. Normally leave this off for crash diagnostics. |
| `--sm-path PATH` | automatic | Use your own matching SourceMod source checkout. |

### Custom SourceMod sources

For example:

```sh
python build.py --sm-version 1.13 --sm-path /path/to/sourcemod-source --arch x86_64
```

Use `python3` on Linux or quote paths as appropriate for your shell. The SDK's
`product.version` must match `--sm-version`. It must contain the required AMTL
and SourcePawn submodules. Initialize those yourself in a checkout you own and
intend to prepare; the helper does not update or patch custom SDK sources.
Your SDK Git revision/dirty state is recorded when available. Arbitrary future
or transitional APIs are not automatically supported.

### Directory layout

Typical default Windows output:

```text
build/windows-sm1.12-both-symbols/
  generated/                  # Generated version header/build metadata
  package/
    build-info.json
    addons/sourcemod/
      configs/accelerator.cfg
      extensions/
        accelerator.autoload
        accelerator.ext.dll   # x86
        x64/
          accelerator.ext.dll # x86_64
      gamedata/
      scripting/
  symbols/
    build-info.json
    x86/                      # Matching DLL and PDB
    x86_64/
  dist/
    accelerator-...-package.zip
    accelerator-...-symbols.zip
```

Linux uses `linux-...` build directories and `.so` binaries containing symbols
by default instead of separate Windows PDBs. `--no-debug` changes the output
directory suffix to `nosymbols` and does not create a symbol ZIP. Custom SDK
builds add `-custom` to the directory name.

Installation archives contain `addons/` at the ZIP root. Debugging archives are
separate and are **not installed into the game server**. Preserve both archives
from each distributed build; rebuilding later does not guarantee identical
binaries or matching symbols.

`build-info.json` records source identity, selected SDK and revisions, compiler
identities, options, dependency pins, and final binary SHA-256 hashes. Archive
filenames include target/platform/architecture and source/binary identity.

## Install and verify

1. Use a disposable/test server before replacing a production installation.
   The game server needs its normal working Metamod/SourceMod installation;
   the build does not install those.
2. Stop the server. Back up existing Accelerator binaries, gamedata, and config.
3. Extract the installation ZIP's `addons/` contents into the game directory.
   Do not add an extra `windows/`, `linux/`, or `package/` directory level.
   Do not rename the binaries to switch architecture.
4. Preserve an existing `addons/sourcemod/configs/accelerator.cfg`, adding new
   settings deliberately rather than overwriting account/privacy choices.
5. Before the first test startup, set `MinidumpDisableUpload` to `"yes"` if you
   do not want existing or test dumps sent to the external crash service.
6. Start the server. In its console, check:

   ```text
   sm version
   sm exts list
   sm exts info <extension-number>
   ```

   Replace `<extension-number>` with Accelerator's index from the list.
7. Inspect SourceMod errors and `addons/sourcemod/logs/accelerator.log`.
   Loading successfully is only the first check; follow
   [COMPATIBILITY.md](COMPATIBILITY.md) to validate controlled crash capture,
   dump readability, retention, uploading, and unload behavior.

Do not deliberately crash a production server to test this extension. A successful
build also does not guarantee matching proprietary game symbols or backend
support for every SourcePawn version.

## Rebuild, dependencies, and safe cleanup

- Repeat the same `build.py` command after editing source. The helper reruns
  configuration; AMBuild reuses applicable compilation outputs.
- SourceMod/AMBuild checkouts and the private Python environment are under
  `.build-deps/`. Breakpad/zlib are under `third_party/`.
- Exact dependency pins come from `dependencies.json`. A wrong revision, dirty
  checkout, or conflicting Gitlink is reported; no hard reset discards edits.
- Breakpad patches are applied to immutable content-addressed snapshots under
  `.build-deps/patched/`, never to the source checkout. Snapshots are outside
  AMBuild's output tree so discovered headers are ordinary source dependencies.
  Multiple builds may share an identical generation. Old generations consume
  disk space; only remove them when no build is using them.
- For a fresh build, choose a new source/build directory or move the relevant
  generated build directory aside. Keep archives, symbols, and any files you
  need before removing generated output.
- For a damaged/incomplete dependency or virtual environment, first preserve
  edits and move **only the named affected directory** aside, then rerun the
  helper. Do not broadly delete `third_party` or reset the project.
- Do not run two first-time bootstraps concurrently in the same source directory.
  Downloads and environment setup are shared.
- Source ZIP builds use a source-content fingerprint instead of a Git revision.
  Custom SDKs and dirty sources are identified in the manifest.
- Pins improve repeatability, but moving hosted toolchains/container images and
  environment differences mean these are not guaranteed bit-for-bit reproducible
  builds.

## Tests

From the source root:

```sh
python -m unittest discover -s tests -v
```

Use `python3` on Linux. Tests include build orchestration, metadata/package
fixtures, isolated patch staging, and source-level upload/lifecycle guards.
Native helper tests compile small C++ programs when GCC, Clang, or MSVC is
available; actual Git integration tests need Git. Local runs can skip these when
tools are missing. **A skip is not a pass.**

For native Windows tests using MSVC, a separate Developer PowerShell window can
provide `cl.exe`. Return to regular PowerShell for a combined-architecture build.
The Actions workflow manages this separation automatically.

CI sets `ACCELERATOR_REQUIRE_NATIVE_TESTS=1` and
`ACCELERATOR_REQUIRE_GIT_TESTS=1` to turn missing tools into failures. Native tests
use small SDK-interface test doubles; they do not replace compiling the complete
extension against the real SDK or running a server.

## Troubleshooting

| Symptom | What to check |
| --- | --- |
| `git` / `python` not found | Reopen the terminal after installation; check PATH. On Windows choose the intended Python installation/launcher. On Linux use `python3`. |
| Python opens the Microsoft Store / wrong interpreter | Install/select a real Python interpreter and check its app-execution alias or use the Python launcher. |
| Architecture-specific MSVC environment error | Open regular PowerShell, not Developer PowerShell. For a single-arch developer build, use an environment matching that architecture. |
| Visual Studio/compiler detection fails | Install the Desktop development with C++ workload, MSVC x86/x64 tools, and a Windows SDK. C++ runtime redistributables alone are not a compiler. |
| x86 compilation cannot find headers/libraries | Install the 32-bit/multilib prerequisites, or request only `--arch x86_64` if that matches the server. |
| Dirty/wrong-revision dependency | Read the named path/ref in the error. Preserve changes and move it aside, or explicitly reconcile pins. Do not use a destructive reset as a generic fix. |
| Dependency folder is nonempty without `.git` | The helper refuses to overwrite manually extracted sources. Preserve/move that folder so the helper can create its pinned checkout. |
| Download/TLS/proxy failure | Check GitHub access, proxy settings, certificates, and credentials for your network. Do not disable certificate checks as a general workaround. |
| AMBuild import/environment failure | Move the affected `venv-py...` directory under `.build-deps/` aside and rerun. The helper uses its own environment; global `pip install` is not normally needed. |
| Wrong SourceMod product version | `--sm-path` must point to source code matching `--sm-version`, not an installed `addons/sourcemod` directory. |
| Unsupported extension API compile error | Check the selected SDK and compatibility guard. Transitional API 10 and unknown APIs are intentionally not treated as supported. |
| Breakpad patch cannot apply | Check the pinned dependency and whether your source already contains partial/conflicting edits. The original checkout is left intact; preserve it while investigating. |
| AMBuild reports a staged header has no corresponding build entry | Update `third_party/Patch`; staged snapshots must be in `.build-deps/patched/`, outside the AMBuild output directory. Start a new Actions run on the updated commit. |
| Git reports dubious ownership in an Actions Linux container | Update `build-extension.yml`, which explicitly trusts only the mounted checkout's physical path before tests/build. Do not globally trust `*`. |
| Long Windows path / cannot open file | Try a shorter source directory. Spaces are supported, but compiler/OS path-length limits can still apply. |
| Container daemon unavailable / mount denied | Start Docker/Podman, select Linux containers on Docker Desktop, and permit the source directory mount. |
| Linux library requirements too new on server | Prefer the project's Debian 11 container/Actions build and verify on the target host; native modern-host builds inherit newer requirements. |
| Missing/wrong architecture binary or PDB during packaging | Treat the build as failed. Inspect earlier configure/linker output; do not ship a partial package. |
| Test reports `skipped` | Install the missing Git/compiler tools or use CI's required-test path; it does not validate the skipped functionality. |
| Binary builds but will not load | Check server OS/process architecture, SourceMod API compatibility, and runtime-library dependencies. Read `sm exts list`/SourceMod errors. |
| Windows process crashes but no Accelerator dump appears | Confirm the updated binary loads, the dump folder is writable, and MinidumpWindowsFirstChance is yes (default). Check for the startup capture-mode message and "Windows exception intercepted" / dump callback messages. Engine handlers may bypass the unhandled filter. Direct termination/fail-fast can bypass handlers entirely. Use a test server with uploads disabled; first-chance snapshots can include handled exceptions or duplicates. |
| `Could not find interface: IGameHelpers` | Update/rebuild Accelerator including extension/smsdk_config.h and extension.cpp. Game helpers are now optional metadata support; an unavailable interface produces a versioned warning rather than rejecting the extension. It is not a separate extension to install. Capture the warning and full `sm version` output for diagnosis; do not assume upgrading SourceMod is necessary. |
| `upload.py is retired` | Build archives with `build.py`; use Actions artifacts or the optional Release workflow for publication. |

When asking for build help, include the command, OS, Python/compiler versions,
selected SDK/architecture, first error with surrounding log output, and
`build-info.json` if generated. Redact credentials and private paths as needed.
Never attach a real crash dump publicly without checking it for sensitive data.
