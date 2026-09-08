import argparse
import json
import zipfile
import xml.etree.ElementTree as ET


VSIX_NAMESPACE = "http://schemas.microsoft.com/developer/vsx-schema/2011"
SUPPORTED_VERSION_RANGE = "[17.14,19.0)"
SUPPORTED_PRODUCTS = {
    "Microsoft.VisualStudio.Community",
    "Microsoft.VisualStudio.Pro",
    "Microsoft.VisualStudio.Enterprise",
}


def validate(path: str) -> None:
    with zipfile.ZipFile(path, "r") as archive:
        manifest = ET.fromstring(archive.read("extension.vsixmanifest"))
        setup_manifest = json.loads(archive.read("manifest.json"))
        catalog = json.loads(archive.read("catalog.json"))

    namespace = {"vsix": VSIX_NAMESPACE}
    installation = manifest.find("vsix:Installation", namespace)
    if installation is None:
        raise ValueError("VSIX manifest has no Installation element")

    targets = installation.findall("vsix:InstallationTarget", namespace)
    actual_products = {target.get("Id") for target in targets}
    if actual_products != SUPPORTED_PRODUCTS:
        raise ValueError(
            f"VSIX products are {sorted(actual_products)}; "
            f"expected {sorted(SUPPORTED_PRODUCTS)}"
        )

    for target in targets:
        if target.get("Version") != SUPPORTED_VERSION_RANGE:
            raise ValueError(
                f"{target.get('Id')} targets {target.get('Version')}; "
                f"expected {SUPPORTED_VERSION_RANGE}"
            )
        architecture = target.find("vsix:ProductArchitecture", namespace)
        if architecture is None or architecture.text != "amd64":
            raise ValueError(f"{target.get('Id')} must target amd64")

    prerequisites = manifest.findall(
        "vsix:Prerequisites/vsix:Prerequisite", namespace
    )
    core_editor = [
        prerequisite
        for prerequisite in prerequisites
        if prerequisite.get("Id") == "Microsoft.VisualStudio.Component.CoreEditor"
    ]
    if len(core_editor) != 1:
        raise ValueError("VSIX must have exactly one Core Editor prerequisite")
    if core_editor[0].get("Version") != SUPPORTED_VERSION_RANGE:
        raise ValueError(
            f"Core Editor prerequisite targets {core_editor[0].get('Version')}; "
            f"expected {SUPPORTED_VERSION_RANGE}"
        )

    _validate_setup_dependencies("manifest.json", setup_manifest.get("dependencies"))
    packages = catalog.get("packages")
    components = (
        [package for package in packages if package.get("type") == "Component"]
        if isinstance(packages, list)
        else []
    )
    if len(components) != 1:
        raise ValueError("catalog.json must contain exactly one component package")
    _validate_setup_dependencies("catalog.json", components[0].get("dependencies"))


def _validate_setup_dependencies(name: str, dependencies) -> None:
    if not isinstance(dependencies, dict):
        raise ValueError(f"{name} has no dependency map")
    actual = dependencies.get("Microsoft.VisualStudio.Component.CoreEditor")
    if actual != SUPPORTED_VERSION_RANGE:
        raise ValueError(
            f"{name} Core Editor dependency targets {actual}; "
            f"expected {SUPPORTED_VERSION_RANGE}"
        )


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Validate supported Visual Studio versions in a packaged VSIX."
    )
    parser.add_argument("vsix")
    arguments = parser.parse_args()
    validate(arguments.vsix)


if __name__ == "__main__":
    main()
