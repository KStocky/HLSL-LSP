import argparse
import hashlib
import json
import os
import re
import tempfile
import zipfile


def _canonicalize_visual_studio_metadata(entries):
    content = {entry.filename: data for entry, data in entries}
    if "catalog.json" not in content or "manifest.json" not in content:
        return entries, []

    catalog = json.loads(content["catalog.json"].decode("utf-8-sig"))
    manifest = json.loads(content["manifest.json"].decode("utf-8-sig"))
    packages = [
        package for package in catalog.get("packages", []) if package.get("type") == "Vsix"
    ]
    if len(packages) != 1:
        return entries, []

    package = packages[0]
    identity = f"{package['id']}@{package['version']}".encode()
    directory_token = hashlib.sha256(identity).hexdigest()[:11]
    extension_directory = (
        "[installdir]\\Common7\\IDE\\Extensions\\"
        f"{directory_token[:8]}.{directory_token[8:]}"
    )
    package["extensionDir"] = extension_directory
    manifest["extensionDir"] = extension_directory

    pkgdef_encodings = {}
    for name, data in list(content.items()):
        if not name.endswith(".pkgdef"):
            continue
        encoding = "utf-16" if data[:2] in (b"\xff\xfe", b"\xfe\xff") else "utf-8"
        pkgdef_encodings[name] = encoding
        content[name] = re.sub(
            r'("CacheTag"=qword:)[0-9A-Fa-f]+', r"\g<1>0", data.decode(encoding)
        ).encode(encoding)

    payload_hash = hashlib.sha256()
    for name, data in sorted(content.items()):
        if name in {"catalog.json", "manifest.json"}:
            continue
        payload_hash.update(name.encode())
        payload_hash.update(len(data).to_bytes(8, "big"))
        payload_hash.update(data)
    cache_tag = payload_hash.hexdigest()[:16].upper()
    for name, encoding in pkgdef_encodings.items():
        text = content[name].decode(encoding)
        content[name] = re.sub(
            r'("CacheTag"=qword:)[0-9A-Fa-f]+',
            lambda match: match.group(1) + cache_tag,
            text,
        ).encode(encoding)

    content["manifest.json"] = json.dumps(
        manifest, ensure_ascii=False, separators=(",", ":")
    ).encode()
    normalized = [(entry, content[entry.filename]) for entry, _ in entries]
    payloads = [
        payload
        for payload in package.get("payloads", [])
        if payload.get("fileName", "").lower().endswith(".vsix")
    ]
    return normalized, (catalog, payloads)


def _write_archive(path, entries):
    with zipfile.ZipFile(
        path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9
    ) as archive:
        for original, content in sorted(entries, key=lambda value: value[0].filename):
            entry = zipfile.ZipInfo(original.filename, date_time=(1980, 1, 1, 0, 0, 0))
            entry.compress_type = zipfile.ZIP_DEFLATED
            entry.create_system = original.create_system
            entry.external_attr = original.external_attr
            entry.internal_attr = original.internal_attr
            entry.flag_bits = original.flag_bits & 0x800
            archive.writestr(entry, content)


def normalize(path: str) -> None:
    source = os.path.abspath(path)
    with zipfile.ZipFile(source, "r") as archive:
        entries = [(entry, archive.read(entry)) for entry in archive.infolist()]
    entries, visual_studio_metadata = _canonicalize_visual_studio_metadata(entries)

    directory = os.path.dirname(source)
    descriptor, temporary = tempfile.mkstemp(prefix="normalized-", suffix=".zip", dir=directory)
    os.close(descriptor)
    try:
        catalog, payloads = visual_studio_metadata or (None, [])
        for _ in range(10):
            if catalog is not None:
                catalog_content = json.dumps(
                    catalog, ensure_ascii=False, separators=(",", ":")
                ).encode()
                entries = [
                    (entry, catalog_content if entry.filename == "catalog.json" else data)
                    for entry, data in entries
                ]
            _write_archive(temporary, entries)
            archive_size = os.path.getsize(temporary)
            if not payloads or all(payload.get("size") == archive_size for payload in payloads):
                break
            for payload in payloads:
                payload["size"] = archive_size
        else:
            raise RuntimeError("Visual Studio VSIX size metadata did not converge")
        os.replace(temporary, source)
    finally:
        if os.path.exists(temporary):
            os.remove(temporary)


def main() -> None:
    parser = argparse.ArgumentParser(description="Normalize ZIP metadata and entry ordering.")
    parser.add_argument("archives", nargs="+")
    arguments = parser.parse_args()
    for archive in arguments.archives:
        normalize(archive)


if __name__ == "__main__":
    main()
