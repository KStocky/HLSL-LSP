import importlib.util
import json
import os
import tempfile
import unittest
import zipfile


SCRIPT = os.path.join(
    os.path.dirname(__file__), "..", "..", "tools", "validate_visual_studio_vsix.py"
)
SPEC = importlib.util.spec_from_file_location("validate_visual_studio_vsix", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class ValidateVisualStudioVsixTests(unittest.TestCase):
    def test_accepts_visual_studio_2022_and_2026_range(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "extension.vsix")
            self._write_vsix(path, "[17.14,19.0)")

            MODULE.validate(path)

    def test_rejects_visual_studio_2026_only_range(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "extension.vsix")
            self._write_vsix(path, "[18.0,19.0)")

            with self.assertRaisesRegex(ValueError, "expected"):
                MODULE.validate(path)

    def test_rejects_missing_edition(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "extension.vsix")
            products = set(MODULE.SUPPORTED_PRODUCTS)
            products.remove("Microsoft.VisualStudio.Pro")
            self._write_vsix(path, "[17.14,19.0)", products=products)

            with self.assertRaisesRegex(ValueError, "VSIX products"):
                MODULE.validate(path)

    def test_rejects_wrong_architecture(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "extension.vsix")
            self._write_vsix(path, "[17.14,19.0)", architecture="x86")

            with self.assertRaisesRegex(ValueError, "must target amd64"):
                MODULE.validate(path)

    def test_rejects_missing_core_editor_prerequisite(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "extension.vsix")
            self._write_vsix(path, "[17.14,19.0)", include_prerequisite=False)

            with self.assertRaisesRegex(ValueError, "exactly one Core Editor"):
                MODULE.validate(path)

    def test_rejects_divergent_setup_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "extension.vsix")
            self._write_vsix(path, "[17.14,19.0)", setup_range="[18.0,19.0)")

            with self.assertRaisesRegex(ValueError, "manifest.json"):
                MODULE.validate(path)

    @staticmethod
    def _write_vsix(
        path,
        version_range,
        *,
        products=None,
        architecture="amd64",
        include_prerequisite=True,
        setup_range=None,
    ):
        products = MODULE.SUPPORTED_PRODUCTS if products is None else products
        setup_range = version_range if setup_range is None else setup_range
        targets = "".join(
            f"""
            <InstallationTarget Id="{product}" Version="{version_range}">
              <ProductArchitecture>{architecture}</ProductArchitecture>
            </InstallationTarget>
            """
            for product in sorted(products)
        )
        prerequisite = (
            f"""
            <Prerequisite Id="Microsoft.VisualStudio.Component.CoreEditor"
              Version="{version_range}" />
            """
            if include_prerequisite
            else ""
        )
        manifest = f"""<?xml version="1.0" encoding="utf-8"?>
        <PackageManifest Version="2.0.0"
          xmlns="{MODULE.VSIX_NAMESPACE}">
          <Installation>{targets}</Installation>
          <Prerequisites>{prerequisite}</Prerequisites>
        </PackageManifest>
        """
        dependencies = {
            "Microsoft.VisualStudio.Component.CoreEditor": setup_range,
        }
        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr("extension.vsixmanifest", manifest)
            archive.writestr("manifest.json", json.dumps({"dependencies": dependencies}))
            archive.writestr(
                "catalog.json",
                json.dumps(
                    {
                        "packages": [
                            {"type": "Component", "dependencies": dependencies},
                            {"type": "Vsix", "dependencies": {}},
                        ]
                    }
                ),
            )


if __name__ == "__main__":
    unittest.main()
