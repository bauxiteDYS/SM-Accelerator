# SourceMod build targets

These are explicit, separately packaged targets, **not verified runtime support
claims**. Full Windows/Linux builds and live server tests are the next step.
See [BUILDING.md](BUILDING.md) for complete compilation instructions.

| Target | SourceMod source revision | Extension API |
| --- | --- | --- |
| 1.11 | f53cb134ef83b580c83e1f4bf35f60d11c4571dd | 8 |
| 1.12 | 3f7460671f7adc4a3c6791a47d21ba3e7b5fba91 | 8 |
| 1.13 | c88a840fc032af022824b0e39c40e1e3b83800c2 | 11 |

`dependencies.json` is authoritative. Update it deliberately when moving a
target; update recorded Git submodule pins as well if they change.

## Why separate packages?

1.11 and 1.12 use the older SourcePawn interfaces. They may allow shared binaries,
but this project does not assume that without runtime testing.

The pinned 1.13 loader requires extension API 9 or newer, rejects transitional
API 10, and exposes API 11. An API-8 binary cannot load there; an API-11 binary
cannot load on 1.11/1.12 (or older API-9-only 1.13 snapshots).

`extension/SourcePawnCompat.h` centralizes public-function enumeration:

- API 8: the older IPluginRuntime interface.
- APIs 9/11: the SourcePawn BaseRuntime interface.
- Other API versions: explicit compile error rather than an untested fallback.

The BaseRuntime dependency is internal, making the pinned 1.13 source revision
important. The older standalone SourcePawn factory/JIT metadata path is only used
below API 9. The old SourceMod-version sidecar field remains unavailable on that
newer path; build-info.json records the compile target, not the running server's
exact SourceMod installation.

Native dump capture and upload safeguards are shared. Changes to SourcePawn
metadata do not supply proprietary game symbols, nor guarantee that a remote
crash backend understands every new SourcePawn runtime.

`IGameHelpers` is nonessential metadata support and is acquired non-fatally with
the compile SDK's declared interface version. Missing helpers can omit map and
command-line information, but should not prevent the dump handler from loading.
The interface is retried after extension loading and at map start.
The reported 1.11.0.6939 server commit (720dc510) and pinned 1.11 SDK have identical
IGameHelpers/IShareSys headers and relevant registration code. The user later
confirmed that the lookup failure came from installing the wrong extension DLL;
loading the correct DLL resolved it. The optional-interface change remains a
resilience measure, not evidence of an SDK-age mismatch. Loading the extension
does not by itself validate crash capture.

## Custom SDK sources

```sh
python build.py --sm-version 1.13 --sm-path /path/to/sourcemod-source
```

The source product.version must match the selected major/minor target. Custom
sources are not reset, updated, or patched. Initialize their submodules yourself.
Their revision and dirty state are recorded, and they use a `-custom` build tree.
This is an explicit escape hatch, not a guarantee for arbitrary development APIs.

## Verification checklist

For each target, OS, and server architecture:

1. Run the required helper tests, compile, and validate the package/symbol files.
2. Confirm the extension loads on that SourceMod installation and reports its ID.
3. Start with uploads disabled and a disposable server installation.
4. Trigger a controlled native test crash; confirm dump and sidecar creation.
5. Open the dump and verify the exception/thread/module data and matching symbols.
6. Check plugin-load/unload metadata handling, especially the new 1.13 path.
7. Exercise the two-dump limit, processed markers, failed uploads, and unload stop.
8. On Windows, verify the default first-chance capture route against a controlled
   native crash. Engine-installed handlers can prevent the unhandled filter from
   seeing a crash. First-chance snapshots may also represent handled exceptions
   or duplicate an unhandled dump; inspect FirstChanceCapture in their metadata.

Do not substitute load/crash tests with a successful compile or stub-based unit
tests. Supported games can have different gamedata and exception-handler behavior.
