"""Smoke tests for tools/package.py (no toolchain needed)."""
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import package  # noqa: E402

RESOURCES = ROOT / "sdk" / "tools" / "pack"


class PackageTests(unittest.TestCase):
    def build(self, app: bytes, product: str = "FM-1_8000014"):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "t.fwsc"
            report = package.create(app, RESOURCES, out, package.USER_SIZE, product)
            return report, out.read_bytes()

    def test_layout_fits_1mb_flash(self):
        report, data = self.build(bytes(range(256)) * 256)
        self.assertEqual(report["flash_bytes"], 1 << 20)
        self.assertLessEqual(report["USER_RESERVED_END"], 1 << 20)
        self.assertLess(report["FLASH_BIN_SIZE"], report["VM_END_ADDR"])
        self.assertEqual(report["package_bytes"], len(data))

    def test_deterministic(self):
        app = b"\x55" * 10000
        self.assertEqual(self.build(app)[1], self.build(app)[1])

    def test_rejects_bad_identity(self):
        with self.assertRaises(ValueError):
            self.build(b"\x00" * 1024, product="ERISIAN")

    def test_rejects_empty_and_oversize(self):
        with self.assertRaises(ValueError):
            self.build(b"")
        with self.assertRaises(ValueError):
            self.build(b"\x00" * (1 << 20))


if __name__ == "__main__":
    unittest.main()
