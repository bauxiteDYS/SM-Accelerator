"""Source-level wiring checks; native queue/config behavior is tested in C++."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class UploadLimitTests(unittest.TestCase):
    def test_default_and_config_are_wired(self):
        config = (ROOT / "configs/accelerator.cfg").read_text()
        self.assertRegex(config, r'"MinidumpUploadLimit"\s+"2"')
        source = (ROOT / "extension/extension.cpp").read_text()
        self.assertIn('acceleratorConfig.GetValue("MinidumpUploadLimit")', source)
        self.assertIn("DumpUploadQueue queue(uploadLimit ? static_cast<size_t>(atoi(uploadLimit)) : 2);", source)
        self.assertIn('std::strcmp(key, "MinidumpUploadLimit") == 0',
                      (ROOT / "extension/AcceleratorConfig.cpp").read_text())

    def test_processed_filter_and_timestamp_selection_precede_uploads(self):
        source = (ROOT / "extension/extension.cpp").read_text()
        run = source.split("void RunThread(", 1)[1].split("void OnTerminate(", 1)[0]
        scan, upload = run.split("for (const DumpCandidate &candidate : queue.Entries())", 1)
        self.assertLess(scan.index("PathExists(processedPath.c_str())"),
                        scan.index("queue.Consider(path, modified)"))
        self.assertIn("FileTime_LastChange", scan)
        self.assertIn("libsys->CloseDirectory(dumps)", scan)
        self.assertNotIn("PresubmitCrashDump(", scan)
        self.assertNotIn("UploadCrashDump(", scan)
        # Deferred unprocessed files are not deleted or marked by the scan.
        pending_scan = scan.split("++pending;", 1)[1]
        self.assertNotIn("RemoveDump(", pending_scan)
        self.assertNotIn('fopen(', pending_scan)
        self.assertNotIn("queue.Consider(", upload)  # no backfill after failures
        self.assertLess(upload.index("++attempted;"), upload.index("PresubmitCrashDump("))
        self.assertIn("if (!keepDumps)", upload)
        self.assertIn("PathExists(processedPath.c_str())", upload)  # recheck before attempt

    def test_limit_is_not_a_retention_or_http_request_limit(self):
        docs = (ROOT / "README.md").read_text()
        self.assertIn("MinidumpUploadLimit", docs)
        self.assertIn("not a disk-retention limit", docs)
        self.assertIn("not a limit on individual HTTP requests", docs)
