"""Wiring guards for optional game-helper metadata; live server checks still required."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


class OptionalMetadataTests(unittest.TestCase):
    def test_sdk_does_not_eagerly_require_game_helpers(self):
        config = (ROOT / "extension/smsdk_config.h").read_text()
        self.assertIsNone(re.search(r"^\s*#define\s+SMEXT_ENABLE_GAMEHELPERS\b", config, re.M))
        source = (ROOT / "extension/extension.cpp").read_text()
        acquire = source.split("static bool AcquireGameHelpers()", 1)[1].split("typedef void", 1)[0]
        self.assertIn("SMINTERFACE_GAMEHELPERS_VERSION, myself, &iface", acquire)
        self.assertIn("return false;", acquire)
        self.assertNotIn("SM_GET_IFACE(GAMEHELPERS", source)

    def test_command_line_and_map_access_are_guarded(self):
        source = (ROOT / "extension/extension.cpp").read_text()
        command = source.split("const char *GetCmdLine()", 1)[1].split("Accelerator::Accelerator()", 1)[0]
        self.assertLess(command.index("if (!optionalGameHelpers)"),
                        command.index("optionalGameHelpers->GetValveCommandLine()"))
        self.assertIn("if (!cmdline)", command)
        map_start = source.split("void Accelerator::OnCoreMapStart(", 1)[1].split("/* 010", 1)[0]
        self.assertLess(map_start.index("if (AcquireGameHelpers())"),
                        map_start.index("optionalGameHelpers->GetCurrentMap()"))
        self.assertIn("m_maphasstarted.store(true)", map_start)

    def test_retry_and_diagnostic_do_not_fail_extension_load(self):
        source = (ROOT / "extension/extension.cpp").read_text()
        all_loaded = source.split("void Accelerator::SDK_OnAllLoaded()", 1)[1].split(
            "void Accelerator::OnCoreMapStart(", 1)[0]
        self.assertIn("if (!AcquireGameHelpers())", all_loaded)
        self.assertIn("interface version %u unavailable", all_loaded)
        self.assertIn("crash capture remains active", all_loaded)
        self.assertNotIn("return false", all_loaded)
