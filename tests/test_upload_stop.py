"""Source-level wiring guards; native request-gate behavior is tested in C++."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


class UploadStopTests(unittest.TestCase):
    def setUp(self):
        self.source = (ROOT / "extension/extension.cpp").read_text()

    def test_every_network_call_uses_the_stop_gate(self):
        calls = re.findall(
            r"control\.RunRequest\(\[&\]\(\) \{\s*return xfer->PostAndDownload\(", self.source)
        self.assertEqual(len(calls), 4)
        self.assertEqual(self.source.count("->PostAndDownload("), 4)

    def test_unload_signals_stop_before_waiting_and_reset_precedes_start(self):
        unload = self.source.split("void Accelerator::SDK_OnUnload()", 1)[1].split(
            "void Accelerator::SDK_OnAllLoaded()", 1)[0]
        self.assertLess(unload.index("uploadThread.RequestStop()"), unload.index("WaitForThread()"))
        load = self.source.split("bool Accelerator::SDK_OnLoad(", 1)[1].split(
            "void Accelerator::SDK_OnUnload()", 1)[0]
        self.assertLess(load.index("uploadThread.Reset()"), load.index("threader->MakeThread("))
        worker = self.source.split("void RunThread(", 1)[1].split("void OnTerminate(", 1)[0]
        self.assertNotIn("control.Reset()", worker)  # must not erase an early unload request

    def test_presubmit_failure_does_not_fall_through_to_dump_upload(self):
        failure = self.source.split("case kPRRemoteError:", 1)[1].split(
            "case kPRStopped:", 1)[0]
        self.assertIn("control.RecordFailure()", failure)
        self.assertIn("break;", failure)
        self.assertNotIn("UploadCrashDump(", failure)
        supporting = self.source.split("if (canBinarySubmit && submitBinary)", 1)[1].split(
            'if (log) fprintf(log, "PresubmitCrashDump complete', 1)[0]
        self.assertEqual(supporting.count("return kPRStopped;"), 2)

    def test_incomplete_work_is_preserved_before_marker_and_cleanup(self):
        worker = self.source.split("for (const DumpCandidate &candidate : queue.Entries())", 1)[1].split(
            "void OnTerminate(", 1)[0]
        guard = worker.index("if (!processed && control.ShouldStop())")
        self.assertIn("break;", worker[guard:worker.index("if (processed)", guard)])
        self.assertLess(guard, worker.index('fopen(processedPath.c_str(), "wb")'))
        cleanup = worker.index("RemoveDump(path, metapath, processedPath);", guard)
        self.assertLess(guard, cleanup)
        self.assertIn("if (control.ShouldStop())", worker[:worker.index("++attempted;")])
