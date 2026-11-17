"""Regression tests for isolated patching and standalone C++ safety helpers."""
import importlib.util
import os
import runpy
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("stage_breakpad", ROOT / "third_party/stage_breakpad.py")
stage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stage)


class StagingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix=".build-test-", dir=ROOT)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "source with spaces"
        (self.source / "src/client").mkdir(parents=True)
        self.file = self.source / "src/client/file.txt"
        self.file.write_text("before\n")
        (self.source / ".git").write_text("gitdir: do not touch\n")
        self.diff = self.root / "example.patch"
        self.diff.write_text("--- a/src/client/file.txt\n+++ b/src/client/file.txt\n@@ -1 +1 @@\n-before\n+after\n")

    def fake_git(self, command, **kwargs):
        if "apply" in command and "--check" not in command:
            (Path(command[2]) / "src/client/file.txt").write_text("after\n")
        return subprocess.CompletedProcess(command, 0, b"", b"")

    def test_copy_is_patched_and_source_is_untouched(self):
        with patch.object(stage.subprocess, "run", side_effect=self.fake_git) as run:
            target, files = stage.prepare(self.source, [self.diff], self.root / "build")
            self.assertEqual(self.file.read_text(), "before\n")
            self.assertEqual((target / "src/client/file.txt").read_text(), "after\n")
            self.assertEqual((self.source / ".git").read_text(), "gitdir: do not touch\n")
            self.assertNotIn(self.source / ".git", files)
            self.assertTrue((target / ".complete").exists())
            run.reset_mock()
            cached, _ = stage.prepare(self.source, [self.diff], self.root / "build")
            self.assertEqual(cached, target)
            run.assert_not_called()
            self.file.write_text("user edit\n")
            other, _ = stage.prepare(self.source, [self.diff], self.root / "build")
            self.assertNotEqual(other, target)
            self.assertEqual(self.file.read_text(), "user edit\n")

    def test_failed_patch_never_publishes_a_generation(self):
        def fail(command, **kwargs):
            return subprocess.CompletedProcess(command, 1 if "--check" in command else 0, b"", b"failed")
        with patch.object(stage.subprocess, "run", side_effect=fail):
            with self.assertRaisesRegex(RuntimeError, "Cannot apply"):
                stage.prepare(self.source, [self.diff], self.root / "build")
        self.assertEqual(self.file.read_text(), "before\n")
        self.assertEqual(list((self.root / "build").iterdir()), [])

    def test_ambuild_staged_sources_are_outside_the_generated_output_tree(self):
        builder = SimpleNamespace(
            sourcePath=str(self.root),
            currentSourcePath=str(self.root / "third_party"),
            buildPath=str(self.root / "build/object-tree"),
            AddConfigureFile=lambda path: None,
        )
        accelerator = SimpleNamespace()
        destinations = []

        def prepare(source, patches, destination):
            destinations.append(Path(destination))
            return Path(destination) / "bp-test", []

        script = ROOT / "third_party/Patch"
        with patch.object(runpy, "run_path", return_value={"prepare": prepare}):
            exec(compile(script.read_text(), str(script), "exec"),
                 {"builder": builder, "Accelerator": accelerator})
        destination = destinations[0].resolve()
        self.assertEqual(destination, self.root / ".build-deps/patched")
        self.assertFalse(destination.is_relative_to(Path(builder.buildPath)))
        self.assertEqual(accelerator.breakpad_root, str(destination / "bp-test"))

    def test_real_git_apply_outside_parent_repository(self):
        if not shutil.which("git"):
            if os.environ.get("ACCELERATOR_REQUIRE_GIT_TESTS") == "1":
                self.fail("Git integration tests are required, but Git is unavailable")
            self.skipTest("Git is required for real patch integration")
        target, _ = stage.prepare(self.source, [self.diff], self.root / "build")
        self.assertEqual((target / "src/client/file.txt").read_text(), "after\n")
        self.assertEqual(self.file.read_text(), "before\n")


class NativeSafetyTests(unittest.TestCase):
    def test_native_helpers(self):
        compiler = (shutil.which(os.environ.get("CXX", "clang++"))
                    or shutil.which("g++") or shutil.which("cl"))
        if not compiler:
            if os.environ.get("ACCELERATOR_REQUIRE_NATIVE_TESTS") == "1":
                self.fail("Native tests are required, but no supported C++ compiler is available")
            self.skipTest("A C++ compiler (GCC, Clang, or MSVC) is required")
        with tempfile.TemporaryDirectory(prefix=".build-test-", dir=ROOT) as temp:
            for windows in (False, True):
                binary = Path(temp) / ("safety-windows.exe" if windows else "safety.exe")
                msvc = Path(compiler).name.lower() in ("cl", "cl.exe")
                command = [compiler, "-std=c++20", "-Wall", "-Wextra", "-Wno-unused-parameter",
                           "-I", str(ROOT / "tests/stubs"), "-I", str(ROOT / "extension"),
                           str(ROOT / "tests/test_crash_safety.cpp"),
                           str(ROOT / "extension/MemoryDownloader.cpp"),
                           str(ROOT / "extension/AcceleratorConfig.cpp"), "-o", str(binary)]
                if msvc:
                    command = [compiler, "/nologo", "/std:c++20", "/EHsc",
                               "/I" + str(ROOT / "tests/stubs"), "/I" + str(ROOT / "extension"),
                               str(ROOT / "tests/test_crash_safety.cpp"),
                               str(ROOT / "extension/MemoryDownloader.cpp"),
                               str(ROOT / "extension/AcceleratorConfig.cpp"),
                               "/Fe:" + str(binary)]
                if windows:
                    command.append("/D_WINDOWS" if msvc else "-D_WINDOWS")
                subprocess.run(command, check=True, cwd=temp)
                subprocess.run([str(binary)], check=True)


class LifecycleGuardTests(unittest.TestCase):
    def test_worker_is_joined_before_resources_are_released(self):
        source = (ROOT / "extension/extension.cpp").read_text()
        unload = source.split("void Accelerator::SDK_OnUnload()", 1)[1].split(
            "void Accelerator::SDK_OnAllLoaded()", 1)[0]
        self.assertLess(unload.index("WaitForThread()"), unload.index("extforwards::Shutdown()"))
        self.assertLess(unload.index("WaitForThread()"), unload.index("delete handler"))
        self.assertIn("threader->MakeThread(&uploadThread, Thread_Default)", source)
        self.assertNotIn("AddFrameAction", (ROOT / "extension/forwards.cpp").read_text())

    def test_windows_handler_does_not_destroy_or_rewrite_exception_state(self):
        source = (ROOT / "extension/extension.cpp").read_text()
        handler = source.split("LONG CALLBACK BreakpadVectoredHandler", 1)[1].split(
            "static bool dumpCallback", 1)[0]
        self.assertNotIn("delete handler", handler)
        self.assertNotIn("return EXCEPTION_EXECUTE_HANDLER", handler)
        self.assertIn("ShouldCaptureWindowsException", handler)
        self.assertIn("windowsCaptureGate.TryBegin()", handler)
        self.assertIn("return EXCEPTION_CONTINUE_SEARCH", handler)
        self.assertNotIn("#ifndef WIN32", source)

    def test_https_defaults_and_safe_response_ownership(self):
        source = (ROOT / "extension/extension.cpp").read_text()
        self.assertNotIn('"http://crash.limetech.org', source)
        self.assertNotIn('"http://crash.limetech.org', (ROOT / "configs/accelerator.cfg").read_text())
        self.assertNotIn("responseSize + 1", source)
        self.assertEqual(source.count("std::unique_ptr<IWebForm>"), 4)
        self.assertEqual(source.count("std::unique_ptr<IWebTransfer>"), 4)
