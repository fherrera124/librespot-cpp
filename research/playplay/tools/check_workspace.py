#!/usr/bin/env python3
"""Check the PlayPlay catalog, local documentation links, and evidence hashes.

All paths in catalog.json and its manifests are relative to the repository root.
This tool reads files only and never runs the resources it checks.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from urllib.parse import unquote, urlsplit


CATALOG = Path("research/playplay/catalog.json")
LINK = re.compile(r"!?\[[^\]]*\]\(\s*(<[^>]*>|[^)\s]+)(?:\s+[^)]*)?\)")
REFERENCE = re.compile(r"^\s*\[[^\]]+\]:\s*(<[^>]*>|\S+)", re.MULTILINE)
FENCE = re.compile(r"^\s*(```|~~~)")


class Check:
    def __init__(self, root: Path):
        self.root = root.resolve()
        self.errors: list[str] = []

    def error(self, label: str, reason: str) -> None:
        self.errors.append(f"{label}: {reason}")

    def path(self, raw: object, label: str) -> Path | None:
        if not isinstance(raw, str) or not raw or Path(raw).is_absolute():
            self.error(label, "expected a nonempty repository-relative path")
            return None
        resolved = (self.root / raw).resolve()
        if not resolved.is_relative_to(self.root):
            self.error(label, "path escapes repository")
            return None
        if not resolved.exists():
            self.error(label, f"missing: {raw}")
            return None
        return resolved

    def object(self, value: object, label: str) -> dict | None:
        if not isinstance(value, dict):
            self.error(label, "expected an object")
            return None
        return value

    def array(self, value: object, label: str) -> list:
        if not isinstance(value, list):
            self.error(label, "expected an array")
            return []
        return value

    def read_json(self, path: Path, label: str) -> dict | None:
        try:
            return self.object(json.loads(path.read_text(encoding="utf-8")), label)
        except (OSError, UnicodeError, json.JSONDecodeError) as exc:
            self.error(label, f"cannot read JSON: {exc}")
            return None

    def links(self, path: Path) -> None:
        label = str(path.relative_to(self.root))
        try:
            source = path.read_text(encoding="utf-8")
        except (OSError, UnicodeError) as exc:
            self.error(label, f"cannot read Markdown: {exc}")
            return
        lines = []
        fence = None
        for line in source.splitlines():
            match = FENCE.match(line)
            if match:
                marker = match.group(1)[0]
                fence = None if fence == marker else marker if fence is None else fence
                continue
            if fence is None:
                lines.append(line)
        source = "\n".join(lines)
        source = re.sub(r"`[^`\n]*`", "", source)
        for raw in [*LINK.findall(source), *REFERENCE.findall(source)]:
            target = raw.removeprefix("<").removesuffix(">")
            if not target or target.startswith(("#", "//")):
                continue
            url = urlsplit(target)
            if url.scheme or url.netloc:
                continue
            local = unquote(url.path)
            if not local:
                continue
            resolved = (path.parent / local).resolve()
            if not resolved.is_relative_to(self.root):
                self.error(label, f"link escapes repository: {target}")
            elif not resolved.exists():
                self.error(label, f"broken link: {target}")

    def manifest(self, raw: object) -> None:
        path = self.path(raw, "evidence manifest")
        if path is None:
            return
        label = str(path.relative_to(self.root))
        manifest = self.read_json(path, label)
        if manifest is None:
            return
        if manifest.get("schema_version") != 1:
            self.error(label, "schema_version must be 1")
        for field in ("run_id", "status", "summary"):
            if not isinstance(manifest.get(field), str) or not manifest[field]:
                self.error(label, f"{field} must be a nonempty string")
        for index, item in enumerate(self.array(manifest.get("artifacts"), f"{label}.artifacts")):
            artifact_label = f"{label}.artifacts[{index}]"
            artifact = self.object(item, artifact_label)
            if artifact is None:
                continue
            artifact_path = self.path(artifact.get("path"), artifact_label)
            if artifact_path is None:
                continue
            if not artifact_path.is_file():
                self.error(artifact_label, "artifact is not a file")
                continue
            digest = artifact.get("sha256")
            size = artifact.get("bytes")
            if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-fA-F]{64}", digest):
                self.error(artifact_label, "sha256 must be 64 hex digits")
            if type(size) is not int or size < 0:
                self.error(artifact_label, "bytes must be a nonnegative integer")
            if not isinstance(artifact.get("role"), str) or not artifact["role"]:
                self.error(artifact_label, "role must be a nonempty string")
            if isinstance(size, int) and artifact_path.stat().st_size != size:
                self.error(artifact_label, f"size mismatch: {artifact['path']}")
            if isinstance(digest, str) and re.fullmatch(r"[0-9a-fA-F]{64}", digest):
                hasher = hashlib.sha256()
                with artifact_path.open("rb") as stream:
                    for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                        hasher.update(chunk)
                if hasher.hexdigest() != digest.lower():
                    self.error(artifact_label, f"SHA256 mismatch: {artifact['path']}")

    def catalog(self) -> dict | None:
        catalog_path = self.path(str(CATALOG), "catalog")
        if catalog_path is None:
            return None
        catalog = self.read_json(catalog_path, str(CATALOG))
        if catalog is None:
            return None
        if catalog.get("schema_version") != 1:
            self.error(str(CATALOG), "schema_version must be 1")
        ids: set[str] = set()
        for index, item in enumerate(self.array(catalog.get("resources"), "resources")):
            label = f"resources[{index}]"
            resource = self.object(item, label)
            if resource is None:
                continue
            identifier = resource.get("id")
            if not isinstance(identifier, str) or not identifier:
                self.error(label, "id must be a nonempty string")
            elif identifier in ids:
                self.error(label, f"duplicate id: {identifier}")
            else:
                ids.add(identifier)
            for field in ("status", "summary"):
                if not isinstance(resource.get(field), str) or not resource[field]:
                    self.error(label, f"{field} must be a nonempty string")
            for raw in self.array(resource.get("paths"), f"{label}.paths"):
                self.path(raw, label)
        for raw in self.array(catalog.get("evidence_manifests"), "evidence_manifests"):
            self.manifest(raw)
        for raw in self.array(catalog.get("documentation"), "documentation"):
            path = self.path(raw, "documentation")
            if path is not None:
                if path.suffix.lower() != ".md" or not path.is_file():
                    self.error(str(raw), "documentation must be a Markdown file")
                else:
                    self.links(path)
        return catalog


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[3])
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--list", action="store_true", help="print resource id, status, and summary")
    group.add_argument("--resource", metavar="ID", help="print one resource entry")
    args = parser.parse_args(argv)
    check = Check(args.root)
    catalog = check.catalog()
    if catalog is not None:
        resources = catalog.get("resources", [])
        if args.list and isinstance(resources, list):
            for item in resources:
                if isinstance(item, dict):
                    print(f"{item.get('id', '?')}\t{item.get('status', '?')}\t{item.get('summary', '?')}")
        elif args.resource and isinstance(resources, list):
            selected = next((item for item in resources if isinstance(item, dict) and item.get("id") == args.resource), None)
            if selected is None:
                check.error("resource", f"unknown id: {args.resource}")
            else:
                print(json.dumps(selected, ensure_ascii=False, indent=2))
    for error in check.errors:
        print(f"ERROR {error}", file=sys.stderr)
    if check.errors:
        print(f"{len(check.errors)} error(s)", file=sys.stderr)
        return 1
    if not (args.list or args.resource):
        print("OK: catalog, documentation links, and evidence verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
