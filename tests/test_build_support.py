import json
from pathlib import Path
import struct
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import zipfile

import build_support as support

ROOT = Path(__file__).resolve().parents[1]


class BuildSupportTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix=".build-test-", dir=ROOT)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def test_compiler_flags_and_explicit_symbol_off(self):
        for family in ("msvc", "gcc", "clang"):
            for symbols in (True, False):
                cxx = SimpleNamespace(family=family, cflags=[], defines=[], symbol_files_="vendor-default")
                support.compiler_policy(cxx, True, symbols)
                self.assertIn("/O2" if family == "msvc" else "-O3", cxx.cflags)
                self.assertIn("NDEBUG", cxx.defines)
                self.assertNotIn("_DEBUG", cxx.defines)
                if not symbols:
                    self.assertIsNone(cxx.symbol_files_)
                else:
                    self.assertEqual(cxx.symbol_files, "separate" if family == "msvc" else "bundled")

    def test_archive_identity_changes_with_source_and_header_has_no_manual_id(self):
        (self.root / "third_party").mkdir()
        (self.root / "extension").mkdir()
        (self.root / "AMBuildScript").write_text("build")
        (self.root / "product.version").write_text("2.6.0")
        source = self.root / "extension/test.cpp"
        source.write_text("first")
        with patch.object(support, "git_output", return_value=None):
            first, _ = support.source_identity(self.root)
            source.write_text("second")
            second, _ = support.source_identity(self.root)
        self.assertNotEqual(first["source_sha256"], second["source_sha256"])
        header = support.version_header("2.6.0", second)
        self.assertIn(second["source_sha256"], header)
        self.assertNotIn("MANUAL BUILD", header)
        target_header = support.version_header("2.6.0", second, "1.13", "b" * 40)
        self.assertIn("-sm1.13", target_header)
        self.assertIn(";sm:1.13:" + "b" * 40, target_header)

    def test_custom_source_version_is_read_from_product_file(self):
        (self.root / "product.version").write_text("1.11.0-dev")
        self.assertEqual(support.sourcemod_version(self.root), "1.11")

    def test_parent_repository_is_not_misreported_as_a_custom_sdk_revision(self):
        with patch.object(support, "git_output", return_value=str(self.root.parent)):
            self.assertIsNone(support.git_revision(self.root))
            self.assertEqual(support.git_config_inputs(self.root), [])

    def create_package(self, platform, arches):
        package = self.root / "package"
        package.mkdir()
        symbols = self.root / "symbols"
        symbols.mkdir()
        info = {"sourcemod": {"version": "1.13"}, "architectures": arches, "platform": platform, "symbols": True,
                "source": {"label": "test", "source_sha256": "a" * 64}}
        (package / "build-info.json").write_text(json.dumps(info))
        for arch in arches:
            folder = package / "addons/sourcemod/extensions"
            if arch == "x86_64":
                folder /= "x64"
            folder.mkdir(parents=True, exist_ok=True)
            header = bytearray(128)
            if platform == "windows":
                header[:2] = b"MZ"
                struct.pack_into("<I", header, 60, 64)
                header[64:68] = b"PE\0\0"
                struct.pack_into("<H", header, 68, 0x14c if arch == "x86" else 0x8664)
                suffix = "dll"
            else:
                header[:4] = b"\x7fELF"
                header[5] = 1
                struct.pack_into("<H", header, 18, 3 if arch == "x86" else 62)
                suffix = "so"
            (folder / f"accelerator.ext.{suffix}").write_bytes(header)
            (symbols / arch).mkdir()
            (symbols / arch / f"accelerator.ext.{suffix}").write_bytes(header)
            if platform == "windows":
                (symbols / arch / "accelerator.ext.pdb").write_bytes(b"pdb fixture")
        return package, symbols

    def test_zip_has_addons_at_root_and_matching_manifest(self):
        self.create_package("windows", ["x86", "x86_64"])
        dist = support.package_archives(self.root, "1.13", ["x86", "x86_64"], "windows", True)
        self.assertEqual(len(list(dist.glob("*.zip"))), 2)
        with zipfile.ZipFile(next(dist.glob("*-package.zip"))) as archive:
            self.assertIn("addons/sourcemod/extensions/accelerator.ext.dll", archive.namelist())
            self.assertFalse(any(name.startswith("windows/") for name in archive.namelist()))
            info = json.loads(archive.read("build-info.json"))
            self.assertEqual(set(info["binary_sha256"]), {"x86", "x86_64"})

    def test_wrong_architecture_is_rejected(self):
        package, _ = self.create_package("linux", ["x86"])
        info = json.loads((package / "build-info.json").read_text())
        info["symbols"] = False
        (package / "build-info.json").write_text(json.dumps(info))
        binary = package / "addons/sourcemod/extensions/accelerator.ext.so"
        data = bytearray(binary.read_bytes())
        struct.pack_into("<H", data, 18, 62)
        binary.write_bytes(data)
        with self.assertRaisesRegex(RuntimeError, "incorrect architecture"):
            support.package_archives(self.root, "1.13", ["x86"], "linux", False)

    def test_missing_pdb_is_rejected(self):
        _, symbols = self.create_package("windows", ["x86"])
        (symbols / "x86/accelerator.ext.pdb").unlink()
        with self.assertRaisesRegex(RuntimeError, "Missing matching Windows PDB"):
            support.package_archives(self.root, "1.13", ["x86"], "windows", True)

    def test_mismatching_symbol_binary_is_rejected(self):
        _, symbols = self.create_package("windows", ["x86"])
        (symbols / "x86/accelerator.ext.dll").write_bytes(b"wrong binary")
        with self.assertRaisesRegex(RuntimeError, "does not match installation"):
            support.package_archives(self.root, "1.13", ["x86"], "windows", True)

    def test_workflows_share_build_entry_and_all_targets(self):
        reusable = (ROOT / ".github/workflows/build-extension.yml").read_text()
        ci = (ROOT / ".github/workflows/ci.yml").read_text()
        easy = (ROOT / ".github/workflows/easy-build.yml").read_text()
        release = (ROOT / ".github/workflows/release.yml").read_text()
        self.assertIn("python3 build.py", reusable)
        for text in (ci, easy):
            self.assertIn("./.github/workflows/build-extension.yml", text)
            for version in ("1.11", "1.12", "1.13"):
                self.assertIn(version, text)
        self.assertNotIn("zip -r", release)
        self.assertIn("merge-multiple: true", release)

    def test_container_git_trust_is_scoped_and_precedes_tests_and_build(self):
        workflow = (ROOT / ".github/workflows/build-extension.yml").read_text()
        trust = 'git config --global --add safe.directory "$(pwd -P)"'
        self.assertIn(trust, workflow)
        self.assertLess(workflow.index(trust), workflow.index("Required regression tests"))
        self.assertLess(workflow.index(trust), workflow.index("Build and validate packages"))
        self.assertNotIn('safe.directory "*"', workflow)
