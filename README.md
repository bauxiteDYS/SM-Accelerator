# Accelerator

## Building

For complete, start-to-finish instructions, see **[BUILDING.md](BUILDING.md)**.
It covers Actions, native Windows/Linux, Docker/Podman, advanced manual AMBuild,
all options, installation, rebuilding, tests, and troubleshooting.

### Easiest: GitHub Actions (Windows and Linux)

No local compiler is needed:

1. Put this modified source in a GitHub repository. If you fork the original,
   commit these changes to your fork. Ensure the workflow files are included on
   the repository's default branch.
2. Open **Actions → Build packages → Run workflow**.
3. Choose Windows, Linux, or both; SourceMod 1.11, 1.12, 1.13, or all; and the
   server architecture. Choose **both** architectures if unsure.
4. After the build succeeds, open the run and download the `accelerator-...`
   artifact under **Artifacts**. Extract it and copy its `addons` folder into
   your game directory. Preserve an existing `configs/accelerator.cfg` when upgrading.

The separate `symbols-...` artifact contains matching binaries, symbols, and a
`build-info.json` manifest. Keep it for debugging; it is not an installation package.
Linux binaries include debug symbols; Windows builds produce matching PDB files.
These symbols describe Accelerator, not proprietary game-engine code.
Install a package matching your SourceMod target; do not mix 1.13 and older
binaries. See [COMPATIBILITY.md](COMPATIBILITY.md) for the pinned revisions and
verification status. All three targets are configured, not yet runtime-certified.

### Local: one build command after installing prerequisites

The helper downloads SourceMod, AMBuild, Breakpad, and zlib, installs AMBuild in
a private Python environment, then builds an optimized extension with debug
symbols. It works with a Git checkout or an extracted source ZIP. Internet access
is required on first use. Dependency revisions come from `dependencies.json`.
Existing checkouts must match their pin and be clean; they are never reset.

**Windows**

Install Git, Python 3 (3.11 or newer recommended), and Visual Studio 2022 Build
Tools with **Desktop development with C++**, including MSVC x86/x64 tools and a
Windows SDK. Open **regular PowerShell** in this source directory:

```powershell
python build.py
```

This produces Windows DLLs. To produce Linux binaries from Windows without
installing Linux compilers, use the GitHub Actions option above.
AMBuild discovers Visual Studio and sets up each architecture itself. An already
active Developer PowerShell/Command Prompt locks AMBuild to that architecture:
use regular PowerShell for a combined x86/x64 build. The helper reports this
condition rather than silently skipping a target.

**Linux (Debian/Ubuntu)**

Install Python with venv support, Git, and a recent C++20 compiler. For example,
on Ubuntu 24.04:

```sh
sudo dpkg --add-architecture i386
sudo apt-get update
sudo apt-get install git python3 python3-venv python3-pip clang g++-multilib libc6-dev-i386 linux-libc-dev:i386
CC=clang CXX=clang++ python3 build.py
```

Local Linux builds depend on the host's runtime libraries. For distribution to
older hosts, prefer Actions, which uses the project's Debian 11 build container.

**Options and output**

```sh
python build.py --sm-version 1.12 --arch both
python build.py --sm-version 1.11 --arch both
python build.py --sm-version 1.13 --arch x86_64
python build.py --sm-version 1.13 --sm-path /path/to/sourcemod-source --arch x86
```

- Defaults: SourceMod 1.12, both x86 and x86_64, optimization and debug symbols.
- All three SourceMod targets, AMBuild, Breakpad, and zlib use exact revisions
  from `dependencies.json`. Git submodule pins must agree with this manifest.
  Existing wrong-revision, dirty, or incomplete dependency checkouts are rejected
  without discarding edits. Move them aside, or update them explicitly.
- Default output on Windows: `build/windows-sm1.12-both-symbols/`; on Linux:
  `build/linux-sm1.12-both-symbols/`. Each contains:
  - `package/`: installable `addons/` and `build-info.json`.
  - `symbols/`: matching binaries/symbols organized by architecture and manifest.
  - `dist/`: ready-to-download ZIPs, labeled by target, source, and binary identity.
- ZIPs contain `addons/` at their root, with no extra Windows/Linux wrapper.
  Packaging checks binary architecture, requires expected PDBs on Windows, and
  verifies that symbol-archive binaries match the installation binaries.
- `build-info.json` records the source revision (or source-ZIP fingerprint),
  dirty state, target SourceMod revision, compiler identities, build options,
  dependency pins, and final binary SHA-256 values.
- Keep the symbol ZIP for each installation package. `--no-debug` explicitly
  disables AMBuild's implicit symbol generation; it does not disable optimization.
  `--jobs 4` limits parallel compilation.
- Symbols do not enable `_DEBUG` runtime behavior. Optimized production builds
  with symbols are the default on both platforms.
- Match the **server process architecture**, not just the operating system.
- `--sm-path` requires SourceMod **source code**, including its submodules, not
  an installed server's `addons/sourcemod` directory. Its product version must
  match `--sm-version`; its revision/local edits are recorded as a custom target.
  It is not modified by the helper.
- AMBuild lives in a Python-version/revision-specific virtual environment and is
  installed only when needed. If that environment is damaged, move its directory
  under `.build-deps/` aside to recreate it.
- Breakpad patches are applied to immutable content-addressed snapshots under
  `.build-deps/patched/`, outside AMBuild's generated-output directories. The
  source checkout is not reset, cleaned, or patched. Old snapshot generations
  can be removed when no build is using them. Configuring hashes/copies dependency sources, so first configuration
  takes additional time and disk space.

If dependency download is interrupted, the helper will not erase the incomplete
checkout. Move that dependency directory aside and rerun. If compiler detection
fails, check the prerequisites above and the server architecture selected.
The old `configure.py` / AMBuild entry points remain available for custom builds.
The obsolete `--disable-auto-versioning` option was removed: all AMBuild builds
generate build identity in their own output tree. The static `version.h` fallback
is only for unsupported builds that bypass AMBuild.

### CI, releases, and legacy scripts

Manual builds, pull requests, and releases all use the same reusable workflow,
which invokes `build.py`. Standard CI builds all three SourceMod targets on both
operating systems, requiring both architectures. Native helper tests and real-Git
patch tests are mandatory in CI; they cannot silently skip due to missing tools.
Local tests can skip those checks when prerequisites are absent:

```sh
python -m unittest discover -s tests -v
```

Native tests support GCC/Clang and MSVC. Windows CI activates MSVC only inside the
test subprocess, leaving the main build free to discover both architectures.
These tests do not replace live server crash/load/unload tests.

Every CI variant publishes installation and symbol archives. Release jobs collect
the same six target/platform variants without rebuilding or changing ZIP layout.
An existing optional build-service upload is retained only when UPLOAD_PASSWORD
is configured; otherwise artifacts remain downloadable from Actions.
The old standalone `upload.py` is retired and prints migration instructions
without making network requests.

`dockerbuild/accelerator_docker_build.sh` is a legacy Linux-container convenience
wrapper, not a separate build system. It delegates to `build.py`; prefer the
documented local command or Actions. Do not run two first-time bootstraps in the
same source folder concurrently; dependency downloads and virtualenv setup are
shared. Compiler builds can safely share immutable patched snapshots; different
source/patch contents create different generations.

Source dependency pins improve repeatability, but these are not a claim of
bit-for-bit reproducible builds: hosted runner/toolchain and container updates
can still change binaries. The manifest and binary hashes identify the outputs.

## Configuration

Accelerator reads its settings from `addons/sourcemod/configs/accelerator.cfg`.
The file is included in release packages. Set `MinidumpAccount` to your SteamID64
to associate uploaded crash reports with your account. Restart the server after
changing settings.

### Migrating from core.cfg

Move any Accelerator `Minidump*` settings from
`addons/sourcemod/configs/core.cfg` into the `"Accelerator"` section of
`accelerator.cfg`, replacing the corresponding example values. Accelerator no
longer reads these settings from `core.cfg`; no changes to other SourceMod
configuration files are required.

```text
"Accelerator"
{
	"MinidumpAccount" "76561198000000000"
	"MinidumpKeepDumps" "yes"
	"MinidumpDisableUpload" "no"
	"MinidumpUploadLimit" "2"
	"MinidumpPresubmit" "yes"
	"MinidumpSymbolUpload" "3"
	"MinidumpBinaryUpload" "yes"
}
```

See [configs/accelerator.cfg](configs/accelerator.cfg) for all settings,
including upload endpoint overrides. Omitted settings default to keeping dumps
and allowing uploads. Other defaults remain unchanged: no account,
presubmission enabled, symbol upload scope `3`, binary uploads enabled, and the
HTTPS crash.limetech.org endpoints. When upgrading an existing config, change
any old `http://crash.limetech.org/...` endpoint overrides to `https://...`;
explicit custom endpoints are not rewritten.

If the file is missing, Accelerator logs a warning and uses those defaults.
A malformed file prevents the extension from loading and reports the parse
error, rather than uploading with a partially read configuration.
Boolean settings accept `yes/no`, `true/false`, or `1/0` (case-insensitive).
Unknown setting names and invalid values are errors, not silently ignored.

### Upload backlog limit

`MinidumpUploadLimit` defaults to `2` (valid range: `1`–`1000`). On each extension
load, Accelerator scans for unprocessed `.dmp` files and attempts only the newest
two by last-modification time, newest first. Equal timestamps use filename order
for deterministic selection. Dumps whose timestamp cannot be read are logged
and left untouched.

- Dumps with a `.processed` sidecar are excluded before selection and do not
  consume the limit.
- Failed attempts, local processing errors, and server-requested skips consume
  a slot. Older dumps do not replace failed candidates during the same run.
- Older, unselected dumps and their metadata are neither marked nor deleted,
  even with `MinidumpKeepDumps "no"`. They remain eligible on later loads.
- With retention enabled, a failed newest dump can be selected again on the next
  load. A new batch is not guaranteed if the same files keep failing.
- The first Webternet request failure (including an HTTP error) stops the pass.
  Presubmit protocol errors also stop it instead of triggering a full-upload
  fallback. There are no automatic retries during that load.
- This is not a disk-retention limit and not a limit on individual HTTP requests:
  a selected dump can trigger presubmission, symbol/binary uploads, and submission.
  Directory scanning still visits all files, but candidate memory is bounded by
  the configured limit. Selection and outcome counts are logged.
- The completion forward and `Accelerator_IsDoneUploadingCrashes()` refer to
  this load's selected batch, not to all dumps remaining on disk.
- Use `MinidumpDisableUpload "yes"` to disable uploads, not a limit of zero.

### Local dumps and upload control

- `MinidumpKeepDumps` defaults to `"yes"`. Dumps (`.dmp`) and their metadata
  (`.dmp.txt`) remain in `addons/sourcemod/data/dumps` regardless of upload outcome.
  Successful uploads (including metadata-only uploads) and server-requested skips
  get an empty `.dmp.processed` sidecar. Once the marker exists, these dumps are
  skipped on later restarts. A crash after remote acceptance but before marker
  creation can still cause a duplicate; this is not an exactly-once guarantee.
  A metadata-only submission also marks the full local dump as processed.
  Keep the sidecars alongside retained dumps; deleting a sidecar
  makes its dump eligible for submission again.
- Failed uploads are left unmarked and eligible for retry on the next restart
  when retention is enabled, subject to the newest-dump limit. If a completion marker cannot be created, Accelerator logs an error;
  that dump may be submitted again on restart.
- Setting `MinidumpKeepDumps` to `"no"` restores deletion after processing selected
  dumps, including local failures not associated with a stopped pass. If a
  request/protocol failure or unload stops the pass, incomplete dumps and metadata
  are kept even with retention off. Previously marked dumps are deleted without
  being resubmitted.
- `MinidumpDisableUpload` defaults to `"no"` (uploads remain enabled). Set it to
  `"yes"` to disable all dump, metadata, symbol, and binary uploads. Crash capture
  still works, no upload thread runs, and no existing dump files are processed or
  deleted, even if retention is off. Unmarked dumps become eligible for upload
  when uploads are re-enabled and the server restarts.

There is no automatic retention limit. Monitor disk usage and remove old dumps,
their metadata, and their sidecars together when no longer needed.

### Safety notes

`IGameHelpers` is optional: it supplies map/command-line metadata, not native
dump generation. Accelerator requests the SDK's declared version, retries after
extension loading and at map start, and logs the requested version if unavailable.
Missing helpers no longer prevent crash capture from loading; affected metadata
may be empty. No unverified older interface layout is used as a fallback.

Uploads use HTTPS by default with no HTTP fallback. Certificates and TLS support
depend on SourceMod's Webternet installation; test uploads on your target server.
Dumps, command lines, and console history can contain secrets even when encrypted
in transit. Use `MinidumpDisableUpload "yes"` for local-only capture; this mode
does not require the Webternet interface.

Unloading requests a cooperative stop before waiting for the upload thread.
The worker checks during scanning, between dumps/modules, and immediately before
each request. The first request failure also blocks all later requests, including
supporting symbol/binary requests and the final dump submission. Incomplete work
is left unmarked for later loads; a final upload that succeeds while unload is
being requested still gets its completion marker and normal retention handling.
These safeguards add no dependency and do not modify Webternet.

This is **not a timeout or an upload-pass time budget**. A request racing the stop
check can still begin, and an in-flight Webternet request cannot be interrupted.
Unloading still waits for it (or any ongoing local dump/symbol processing) to
return; no thread is killed or detached. Completion forwards are dispatched
on the main game thread without queued callbacks into unloaded extension code.

On Windows, `MinidumpWindowsFirstChance` defaults to `"yes"` to restore capture
before engine handlers can consume selected native faults. This preserves the
legacy capture route for access violations and similar faults while never deleting
the handler, rewriting exception records, or returning an invalid vectored result.
The handler returns `EXCEPTION_CONTINUE_SEARCH`, leaving the application's normal
exception handling in place. Recursive/concurrent explicit capture is skipped,
and unload waits for an active vectored writer before destroying Breakpad.

**First-chance does not mean proven fatal:** recoverable/handled exceptions can
also produce dumps, and the unhandled fallback can produce a second dump for the
same incident. Windows metadata includes `FirstChanceCapture=yes/no` to identify
the route. These snapshots participate in normal retention/upload behavior;
disable uploads during controlled tests if you do not want them submitted.
Set this option to `"no"` to rely on the unhandled filter for ordinary faults
(Valve's fatal noncontinuable `Sys_Error` is still intercepted), but some engines
may then crash without an Accelerator dump. This setting is ignored on Linux.
Certain fail-fast exits or direct process termination bypass exception handlers
regardless of the setting. Real game crash tests remain necessary.
