"""Offline native/shell control tests; never sends an IOKit command."""
from __future__ import annotations

import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
COLLECTOR = ROOT / "collector"


class NativeControlTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="wakeup-control-test-")
        cls.directory = Path(cls.temporary.name)
        cls.tool = cls.directory / "wakeupctl-native"
        subprocess.run([
            "clang", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
            str(COLLECTOR / "wakeupctl.c"), "-framework", "IOKit",
            "-framework", "CoreFoundation", "-o", str(cls.tool),
        ], check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def invoke(self, *arguments):
        return subprocess.run([str(self.tool), *map(str, arguments)],
                              text=True, capture_output=True)

    def test_selftest_never_accesses_iokit(self):
        result = self.invoke("--selftest")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("no IOKit access", result.stdout)

    def test_complete_nested_snapshot_is_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            fixture, xml, text = (directory / name for name in ("fixture.plist", "out.plist", "out.txt"))
            snapshot = {
                "WakeupVersion": "0.2.0",
                "Controllers": [
                    {"ControllerRegistryID": 123, "Name": "XHC0", "WakeCalls": 9, "Nested": {"State": "ready"}},
                    {"ControllerRegistryID": 456, "Name": "XHC2", "WakeCalls": 0, "Error": "not ready"},
                ],
                "RawDiagnosticData": bytes(range(256)),
                "NonAscii": "控制器，未满足条件",
            }
            fixture.write_bytes(plistlib.dumps(snapshot, fmt=plistlib.FMT_BINARY))
            result = self.invoke("--mock-plist", fixture, "--plist", xml, "--text", text, "check")
            self.assertEqual(result.returncode, 0, result.stderr)
            exported = plistlib.loads(xml.read_bytes())
            for key, value in snapshot.items():
                self.assertEqual(exported[key], value)
            self.assertTrue(exported["ControlToolMock"])
            self.assertEqual(exported["ControlToolCommand"], "check")
            for value in ("Controllers", "XHC2", "Nested", "ready", "控制器，未满足条件"):
                self.assertIn(value, text.read_text())

    def test_mock_cannot_send_start_or_stop(self):
        for command in ("start", "stop"):
            result = self.invoke("--mock-plist", "/does/not/exist", "--plist", "/tmp/unused-status.plist",
                                 "--text", "/tmp/unused-status.txt", command)
            self.assertEqual(result.returncode, 64)

    def test_invalid_id_is_rejected_before_iokit_access(self):
        for value in ("0", "-1", "+1", "1s2", "1 ", "9223372036854775808", "0x"):
            result = self.invoke("--plist", "/tmp/unused-status.plist",
                                 "--text", "/tmp/unused-status.txt", "start", value)
            self.assertEqual(result.returncode, 64)
            self.assertNotIn("service unavailable", result.stderr)

    def test_check_cannot_have_controller_id(self):
        result = self.invoke("--plist", "/tmp/unused-status.plist",
                             "--text", "/tmp/unused-status.txt", "check", "123")
        self.assertEqual(result.returncode, 64)

    def test_output_paths_must_differ(self):
        result = self.invoke("--plist", "/tmp/same", "--text", "/tmp/same", "check")
        self.assertEqual(result.returncode, 64)

    def test_mock_invalid_top_level(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            fixture = directory / "fixture.plist"
            fixture.write_bytes(plistlib.dumps(["not a dictionary"]))
            result = self.invoke("--mock-plist", fixture,
                                 "--plist", directory / "out.plist", "--text", directory / "out.txt", "check")
            self.assertEqual(result.returncode, 65)


class ShellExportTests(unittest.TestCase):
    def prepare(self, directory: Path):
        collector = directory / "collector with spaces"
        collector.mkdir()
        shutil.copy2(COLLECTOR / "collect.sh", collector / "collect.sh")
        tool = collector / "wakeupctl-x86_64"
        tool.write_text("""#!/bin/sh
set -eu
[ "$1" = --plist ]
plist=$2
shift 2
[ "$1" = --text ]
text=$2
shift 2
printf '<plist version="1.0"><dict><key>Mock</key><true/></dict></plist>\\n' > "$plist"
printf 'fixture status\\n' > "$text"
printf 'mock_command=%s\\n' "$*"
if [ "${MOCK_CONTROL_FAILURE:-0}" = 1 ]; then
    printf 'fixture command failure\\n' >&2
    exit 3
fi
""")
        tool.chmod(0o755)
        tmp = directory / "tmp with spaces"
        tmp.mkdir()
        environment = dict(os.environ, TMPDIR=str(tmp))
        return collector, environment, tmp

    def test_check_defaults_to_script_location(self):
        with tempfile.TemporaryDirectory() as directory:
            collector, environment, temporary = self.prepare(Path(directory))
            result = subprocess.run(["sh", str(collector / "collect.sh"), "check"],
                                    env=environment, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            original, = temporary.glob("usb-xhci-wakeup.*")
            exported, = collector.glob("usb-xhci-wakeup.*")
            self.assertTrue(original.is_dir())
            for source in original.iterdir():
                self.assertEqual(source.read_bytes(), (exported / source.name).read_bytes())
            self.assertIn("Exported and byte-verified", result.stdout)

    def test_check_explicit_export_location(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            collector, environment, _ = self.prepare(directory)
            export = directory / "card"
            export.mkdir()
            result = subprocess.run(["sh", str(collector / "collect.sh"), "check", str(export)],
                                    env=environment, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            saved, = export.glob("usb-xhci-wakeup.*")
            self.assertTrue((saved / "status.plist").is_file())

    def test_export_failure_retains_complete_original(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            collector, environment, temporary = self.prepare(directory)
            result = subprocess.run(["sh", str(collector / "collect.sh"), "check", str(directory / "absent")],
                                    env=environment, text=True, capture_output=True)
            self.assertEqual(result.returncode, 6)
            saved, = temporary.glob("usb-xhci-wakeup.*")
            self.assertTrue((saved / "status.plist").is_file())
            self.assertIn("Export parent is absent", (saved / "export-errors.txt").read_text())
            self.assertNotIn("Exported and byte-verified", result.stdout)
            self.assertIn(str(saved), result.stderr)

    def test_control_failure_is_exported_and_exit_status_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            collector, environment, _ = self.prepare(Path(directory))
            environment["MOCK_CONTROL_FAILURE"] = "1"
            result = subprocess.run(["sh", str(collector / "collect.sh"), "stop", "123"],
                                    env=environment, text=True, capture_output=True)
            self.assertEqual(result.returncode, 3)
            exported, = collector.glob("usb-xhci-wakeup.*")
            self.assertIn("fixture command failure", (exported / "control-errors.txt").read_text())
            self.assertIn("control_exit_status=3", (exported / "run.txt").read_text())
            self.assertIn("mock_command=stop 123", (exported / "control-output.txt").read_text())

    def test_successful_copy_with_corrupt_content_is_not_reported_as_exported(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            collector, environment, temporary = self.prepare(directory)
            fake_bin = directory / "fake-bin"
            fake_bin.mkdir()
            copying = fake_bin / "cp"
            copying.write_text("""#!/bin/sh
/bin/cp "$@" || exit $?
for last do :; done
if [ -d "$last" ] && [ -f "$last/status.txt" ]; then
    printf 'corrupted destination\\n' > "$last/status.txt"
fi
""")
            copying.chmod(0o755)
            environment["PATH"] = str(fake_bin) + os.pathsep + environment["PATH"]
            result = subprocess.run(["sh", str(collector / "collect.sh"), "check"],
                                    env=environment, text=True, capture_output=True)
            self.assertEqual(result.returncode, 6)
            original, = temporary.glob("usb-xhci-wakeup.*")
            self.assertEqual((original / "status.txt").read_text(), "fixture status\n")
            self.assertIn("Export mismatch: status.txt", result.stderr)
            self.assertNotIn("Exported and byte-verified", result.stdout)

    def test_start_has_no_disk_parameter(self):
        with tempfile.TemporaryDirectory() as directory:
            collector, environment, _ = self.prepare(Path(directory))
            result = subprocess.run(["sh", str(collector / "collect.sh"), "start"],
                                    env=environment, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            exported, = collector.glob("usb-xhci-wakeup.*")
            self.assertIn("mock_command=start", (exported / "control-output.txt").read_text())


if __name__ == "__main__":
    unittest.main()
