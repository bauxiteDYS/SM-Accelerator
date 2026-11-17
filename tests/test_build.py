"""Build orchestration tests; no compiler or network required."""
import contextlib
import importlib.util
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("build_helper", ROOT / "build.py")
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


class BuildTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix=".build-test-", dir=ROOT)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def test_defaults(self):
        args = build.arguments([])
        self.assertEqual((args.sm_version, args.arch, args.no_debug), ("1.12", "both", False))

    def test_checkout_refuses_unrelated_files(self):
        (self.root / "user.txt").write_text("keep me", encoding="utf-8")
        with patch.object(build, "run") as run:
            with self.assertRaisesRegex(RuntimeError, "refusing to overwrite"):
                build.checkout("url", "ref", self.root)
            run.assert_not_called()
        self.assertEqual((self.root / "user.txt").read_text(), "keep me")

    def test_existing_checkout_is_not_reset(self):
        (self.root / ".git").mkdir()
        with patch.object(build, "run") as run, patch.object(build, "git_output", side_effect=["ref", ""]):
            build.checkout("url", "ref", self.root)
            run.assert_not_called()

    def test_wrong_revision_is_rejected_without_reset(self):
        (self.root / ".git").mkdir()
        with patch.object(build, "run") as run, patch.object(build, "git_output", return_value="wrong"):
            with self.assertRaisesRegex(RuntimeError, "expected ref"):
                build.checkout("url", "ref", self.root)
            run.assert_not_called()

    def test_dirty_dependency_is_rejected_without_reset(self):
        (self.root / ".git").mkdir()
        with patch.object(build, "run") as run, patch.object(build, "git_output", side_effect=["ref", " M file"]):
            with self.assertRaisesRegex(RuntimeError, "not a clean"):
                build.checkout("url", "ref", self.root)
            run.assert_not_called()

    def test_fresh_checkout_is_pinned(self):
        with patch.object(build, "run") as run, patch.object(build, "git_output", side_effect=["commit-id", ""]):
            build.checkout("url", "commit-id", self.root)
            commands = [call.args[0] for call in run.call_args_list]
        self.assertIn(["git", "-C", self.root, "fetch", "--depth", "1", "origin", "commit-id"], commands)
        self.assertEqual(commands[-1], ["git", "-C", self.root, "checkout", "--detach", "FETCH_HEAD"])

    def test_honours_recorded_submodule(self):
        (self.root / ".git").mkdir()
        ref = build.DEPENDENCIES["breakpad"]["ref"]
        with patch.object(build, "ROOT", self.root), patch.object(build, "run") as run, \
                patch.object(build, "checkout"), patch.object(build, "initialize_submodules"), \
                patch.object(build.subprocess, "check_output", return_value=f"160000 {ref} 0\tthird_party/breakpad\n"):
            build.prepare_library("breakpad")
        run.assert_called_once_with(
            ["git", "submodule", "update", "--init", "--recursive", "--", "third_party/breakpad"])

    def check_build(self, options, target, debug):
        deps = self.root / ".build-deps"
        python = deps / "venv" / ("Scripts/python.exe" if sys.platform == "win32" else "bin/python")
        python.parent.mkdir(parents=True)
        python.touch()
        with patch.object(build, "ROOT", self.root), patch.object(build, "DEPS", deps), \
                patch.object(build.shutil, "which", side_effect=lambda name: None if name == "cl" else "tool"), \
                patch.object(build, "checkout"), patch.object(build, "prepare_library"), \
                patch.object(build, "initialize_submodules"), \
                patch.object(build, "sourcemod_version", return_value=options[options.index("--sm-version") + 1] if "--sm-version" in options else "1.12"), \
                patch.object(build, "prepare_ambuild", return_value=python), \
                patch.object(build, "package_archives") as archive, \
                patch.object(build, "run") as run, contextlib.redirect_stdout(io.StringIO()):
            build.main(options)
        commands = [call.args[0] for call in run.call_args_list]
        configure = next(command for command in commands if self.root / "configure.py" in command)
        self.assertIn("--targets=" + target, configure)
        self.assertIn("--enable-optimize", configure)
        self.assertEqual("--enable-debug" in configure, debug)
        self.assertEqual(commands[-1][-2:], ["-j", "0"])
        self.assertEqual(archive.call_args.args[2], target.split(","))

    def test_default_build_is_both_architectures_with_symbols(self):
        self.check_build([], "x86,x86_64", True)

    def test_single_arch_without_symbols(self):
        self.check_build(["--arch", "x86_64", "--no-debug"], "x86_64", False)

    def test_sourcemod_111_is_a_first_class_target(self):
        self.check_build(["--sm-version", "1.11"], "x86,x86_64", True)

    def test_sourcemod_113_is_a_first_class_target(self):
        self.check_build(["--sm-version", "1.13"], "x86,x86_64", True)

    def test_pins_are_immutable(self):
        for ref in build.SM_REFS.values():
            self.assertRegex(ref, r"^[0-9a-f]{40}$")
        for name in ("ambuild", "breakpad", "zlib"):
            self.assertRegex(build.DEPENDENCIES[name]["ref"], r"^[0-9a-f]{40}$")

    def test_combined_windows_build_rejects_an_active_single_arch_environment(self):
        with patch.object(build.sys, "platform", "win32"), \
                patch.object(build.shutil, "which", return_value="tool"):
            with self.assertRaisesRegex(RuntimeError, "regular PowerShell"):
                build.main([])

    def test_existing_submodule_revision_is_not_switched(self):
        with patch.object(build, "git_output", return_value="+changed submodule"), \
                patch.object(build, "run") as run:
            with self.assertRaisesRegex(RuntimeError, "resolve them explicitly"):
                build.initialize_submodules(self.root)
            run.assert_not_called()

    def test_ambuild_install_is_cached_by_python_and_revision(self):
        ref = build.DEPENDENCIES["ambuild"]["ref"]
        env = self.root / f"venv-py{sys.version_info.major}{sys.version_info.minor}-{ref[:12]}"
        python = env / ("Scripts/python.exe" if sys.platform == "win32" else "bin/python")
        python.parent.mkdir(parents=True)
        python.touch()
        (env / "ambuild-revision.txt").write_text(ref)
        with patch.object(build, "DEPS", self.root), patch.object(build, "checkout"), \
                patch.object(build, "run") as run:
            self.assertEqual(build.prepare_ambuild(), python)
        self.assertFalse(any("pip" in call.args[0] for call in run.call_args_list))

    def test_command_wrapper_handles_spaces_and_only_stamps_success(self):
        wrapper = ROOT / "third_party/cwd_cmd.py"
        folder = self.root / "folder with spaces"
        folder.mkdir()
        stamp = folder / "success stamp.txt"
        output = folder / "output file.txt"
        command = [sys.executable, str(wrapper), str(folder), str(stamp),
                   sys.executable, "-c", "import pathlib,sys; pathlib.Path(sys.argv[1]).write_text('ok')",
                   str(output), "&&", sys.executable, "-c", "pass"]
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(stamp.exists())
        self.assertEqual(output.read_text(), "ok")
        failed_stamp = folder / "failed stamp.txt"
        result = subprocess.run(
            [sys.executable, str(wrapper), str(folder), str(failed_stamp),
             sys.executable, "-c", "import sys; sys.exit(3)", "&&",
             sys.executable, "-c", "raise RuntimeError('must not run')"],
            capture_output=True, text=True)
        self.assertEqual(result.returncode, 3)
        self.assertFalse(failed_stamp.exists())


if __name__ == "__main__":
    unittest.main()
