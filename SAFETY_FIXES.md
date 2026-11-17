# High-priority review follow-up

The original review is a static assessment, not proof that every scenario has
been reproduced. This follow-up covers fixes for issues 1–8 in `../issues.txt`
and closely related safety problems. Other findings are not all resolved.

| Issue | Action |
| --- | --- |
| 1: invalid map ordering | Use a strict descending comparator, with Windows case folding; identical paths compare false. |
| 2: response overreads | Download buffers are always terminated and empty responses have a valid empty buffer. Parse in place rather than copying an extra byte. |
| 3: steam.inf overflow | Bounded formatter writes only complete records and reserves the terminator. |
| 4: thread/handler lifetime | Retain and join the upload thread before unload. Restore the previous Linux terminate handler if ours is still installed. Dispatch completion from the main-thread frame hook, not queued extension callbacks. |
| 5: Windows x64 branches | Use the extension's `_WINDOWS` platform macro instead of relying on `WIN32` being provided by the compiler or another header. The old finding was conditional on the build environment. |
| 6: first-chance handling | After a missing-dump regression report, restore selected first-chance native faults by default (MinidumpWindowsFirstChance). Never delete the handler or rewrite exception records; return CONTINUE_SEARCH, guard recursion/concurrent explicit capture, and wait for active capture on unload. Handled/duplicate snapshots remain a documented trade-off. |
| 7: HTTP defaults | Use HTTPS defaults, warn about explicit HTTP overrides, and do not fall back to HTTP on failure. |
| 8: destructive patching | Patch immutable content-addressed snapshots under .build-deps/patched, outside AMBuild's output tree. Never reset/clean/patch the original checkout. Legacy container entry point delegates to build.py without deleting builds or force-updating dependencies. |

## Related corrections

- Bound backend responses to 1 MiB, check multiplication/allocation failure, and
  retain the old allocation on realloc failure.
- Own Webternet forms/sessions with RAII and handle null creation results.
- Check thread index and main-module availability before dereferencing dump data.
- Avoid mismatched Windows new[]/delete and check path-conversion failure.
- Use a bounded wide-character Windows metadata path; do not overwrite the
  dump directory shared with the upload thread in crash callbacks.
- Accept explicit true/false booleans and reject unknown/invalid config settings
  instead of silently turning off retention or re-enabling uploads.
- Only require Webternet when uploads are enabled.
- Start uploads after installing the crash handler and initializing metadata.

## Validation and remaining limitations

`python -m unittest discover -s tests -v` covers build orchestration, staging
isolation/cache/failure behavior, and source-level lifecycle guards. When Git is
available it also exercises real git apply in an isolated staging repository.
When a GCC-compatible C++ compiler is available, it compiles and executes the
standalone comparator, steam.inf, downloader, and config tests under both platform
macro variants. Those native tests use small public-interface test doubles; they
are not a substitute for building the full extension against SourceMod.
The easy-build workflow runs these tests before compiling.

This checkout's local environment lacks Git and a C++ compiler on PATH, so the
real-Git integration and native tests are skipped locally. Full extension
compilation and live crash tests still need Windows/Linux servers.

The HTTPS endpoints answered read-only HTTPS requests (including expected 405
responses for upload-only endpoints). No crash data was sent during validation.
Successful uploads, certificate verification in the deployed Webternet version,
and TLS trust-store availability still need target-server testing. Existing
explicit HTTP overrides are preserved: update them to HTTPS.

Windows exception-filter coexistence with Valve/third-party handlers needs live
testing. A report confirmed process termination after ent_remove worldspawn
without callback messages, making the narrowed capture path a strong suspect.
Selected first-chance faults are restored by default without the old exception
record mutation/handler deletion. No live retest of this correction has yet
confirmed capture. Handled exceptions/duplicate dumps can occur; there is no
claim that every first-chance fault is fatal or that fail-fast/termination is
catchable. Crash callbacks still call engine GetSpew and, on
Windows, buffered I/O. They have not been fully redesigned for crash-time safety.

Unload waits for in-flight uploads; the current Webternet API does not expose
per-request cancellation/timeouts here. A slow request can therefore delay unload.
The upload pass now has a first-request-failure circuit breaker and a cooperative
unload stop flag. All four request types use the gate. Presubmit protocol failures
also stop the pass, with no full-upload fallback. No timer, forced interruption,
new dependency, or Webternet change was added. Incomplete dumps are left unmarked
and retained when the pass stops, even with keep=no; successful final submissions
still get their marker and normal retention handling.
Missing-config defaults are unchanged. MinidumpUploadLimit defaults to the newest two unprocessed
dumps per extension load; unselected older dumps are never deleted by that pass.
This limits dump attempts, not individual HTTP requests or disk usage.
Retention remains unlimited and markers are not an exactly-once upload guarantee.

The staging cache hashes dependency content at configure time and copies only for
new generations. Original sources and patches are configure dependencies; rerun
build.py after adding new dependency files. Immutable snapshots live outside the
object directories at .build-deps/patched so AMBuild treats discovered headers as
source dependencies. Old generations consume disk until explicitly removed when
no build uses them.

## Build and compatibility follow-up

The helper and every Actions build now share dependencies.json and build.py.
SourceMod 1.11/1.12/1.13 have explicit pinned targets and separate packages;
SourcePawn interface differences are centralized in SourcePawnCompat.h.
AMBuild generates build identity and manifests, applies explicit optimization
and symbol policy, and packages matched binary/symbol archives. Both requested
architectures are mandatory and validated. CI requires native and real-Git tests;
local runs can skip unavailable toolchains. See COMPATIBILITY.md for pins and the
live verification checklist. Implementation is in place; full compilation,
server compatibility, and hosted release execution are still unverified.
