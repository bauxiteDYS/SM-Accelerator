"""Windows capture wiring guards; real engine fault capture needs live testing."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class WindowsCaptureTests(unittest.TestCase):
    def test_first_chance_defaults_and_policy_are_wired(self):
        source = (ROOT / "extension/extension.cpp").read_text()
        self.assertIn('GetBool("MinidumpWindowsFirstChance", true)', source)
        self.assertIn('"MinidumpWindowsFirstChance"', (ROOT / "extension/AcceleratorConfig.cpp").read_text())
        self.assertRegex((ROOT / "configs/accelerator.cfg").read_text(),
                         r'"MinidumpWindowsFirstChance"\s+"yes"')
        self.assertIn("AddVectoredExceptionHandler(1, BreakpadVectoredHandler)", source)
        handler = source.split("LONG CALLBACK BreakpadVectoredHandler", 1)[1].split(
            "static bool dumpCallback", 1)[0]
        self.assertIn("ShouldCaptureWindowsException", handler)
        self.assertLess(handler.index("windowsCaptureGate.TryBegin()"),
                        handler.index("handler->WriteMinidumpForException"))
        self.assertIn("windowsCaptureGate.Finish()", handler)
        self.assertIn("return EXCEPTION_CONTINUE_SEARCH", handler)
        self.assertIn("vectoredRequestThread.store(GetCurrentThreadId())", handler)
        self.assertIn("vectoredThread == handler->get_requesting_thread_id()", source)

    def test_unload_blocks_new_vectored_capture_before_destroying_breakpad(self):
        source = (ROOT / "extension/extension.cpp").read_text()
        unload = source.split("void Accelerator::SDK_OnUnload()", 1)[1].split(
            "void Accelerator::SDK_OnAllLoaded()", 1)[0]
        stop = unload.index("windowsCaptureGate.RequestStop()")
        remove = unload.index("RemoveVectoredExceptionHandler")
        wait = unload.index("while (windowsCaptureGate.IsBusy())")
        destroy = unload.index("delete handler")
        self.assertLess(stop, remove)
        self.assertLess(remove, wait)
        self.assertLess(wait, destroy)

    def test_policy_excludes_debug_and_cpp_exceptions(self):
        policy = (ROOT / "extension/WindowsExceptionPolicy.h").read_text()
        self.assertIn("0xC0000005u", policy)
        self.assertNotIn("case 0x80000003u", policy)
        self.assertNotIn("case 0xE06D7363u", policy)
        self.assertIn("if (!firstChance)", policy)
        self.assertIn("std::atomic_bool::is_always_lock_free", policy)
