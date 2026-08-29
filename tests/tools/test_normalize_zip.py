import importlib.util
import json
import os
import tempfile
import unittest
import zipfile


SCRIPT = os.path.join(os.path.dirname(__file__), "..", "..", "tools", "normalize_zip.py")
SPEC = importlib.util.spec_from_file_location("normalize_zip", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class NormalizeZipTests(unittest.TestCase):
    def test_archives_with_different_metadata_become_identical(self):
        with tempfile.TemporaryDirectory() as directory:
            first = os.path.join(directory, "first.zip")
            second = os.path.join(directory, "second.zip")
            self._write(first, [("b.txt", b"two"), ("a.txt", b"one")], (2025, 1, 1, 1, 2, 4))
            self._write(second, [("a.txt", b"one"), ("b.txt", b"two")], (2026, 2, 2, 3, 4, 6))

            MODULE.normalize(first)
            MODULE.normalize(second)

            with open(first, "rb") as left, open(second, "rb") as right:
                self.assertEqual(left.read(), right.read())

            with zipfile.ZipFile(first, "r") as archive:
                mode = archive.getinfo("b.txt").external_attr >> 16
                self.assertEqual(mode, 0o100755)

    def test_visual_studio_metadata_becomes_reproducible(self):
        with tempfile.TemporaryDirectory() as directory:
            first = os.path.join(directory, "first.vsix")
            second = os.path.join(directory, "second.vsix")
            self._write_visual_studio_vsix(first, "randomone.abc", "0000000000000001", 123)
            self._write_visual_studio_vsix(second, "randomtwo.xyz", "0000000000000002", 456)

            MODULE.normalize(first)
            MODULE.normalize(second)

            with open(first, "rb") as left, open(second, "rb") as right:
                self.assertEqual(left.read(), right.read())
            with zipfile.ZipFile(first, "r") as archive:
                catalog = json.loads(archive.read("catalog.json"))
                package = catalog["packages"][0]
                self.assertEqual(
                    archive.getinfo("catalog.json").compress_type, zipfile.ZIP_STORED
                )
                self.assertEqual(package["payloads"][0]["size"], os.path.getsize(first))
                self.assertEqual(
                    package["extensionDir"],
                    json.loads(archive.read("manifest.json"))["extensionDir"],
                )
                pkgdef = archive.read("extension.pkgdef").decode("utf-16")
                self.assertNotIn("0000000000000001", pkgdef)

    @staticmethod
    def _write(path, entries, timestamp):
        with zipfile.ZipFile(path, "w") as archive:
            for name, content in entries:
                info = zipfile.ZipInfo(name, timestamp)
                info.external_attr = (0o100755 if name == "b.txt" else 0o100644) << 16
                archive.writestr(info, content)

    @staticmethod
    def _write_visual_studio_vsix(path, extension_directory, cache_tag, size):
        catalog = {
            "packages": [
                {
                    "id": "KStocky.HlslLsp.VisualStudio",
                    "version": "0.6.0",
                    "type": "Vsix",
                    "extensionDir": extension_directory,
                    "payloads": [{"fileName": "HlslLsp.VisualStudio.vsix", "size": size}],
                }
            ]
        }
        manifest = {"extensionDir": extension_directory}
        entries = [
            ("catalog.json", json.dumps(catalog).encode()),
            ("manifest.json", json.dumps(manifest).encode()),
            ("extension.pkgdef", f'"CacheTag"=qword:{cache_tag}\n'.encode("utf-16")),
            ("extension.dll", b"deterministic payload"),
        ]
        NormalizeZipTests._write(path, entries, (2026, 2, 2, 3, 4, 6))


if __name__ == "__main__":
    unittest.main()
