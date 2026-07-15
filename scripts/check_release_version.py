#!/usr/bin/env python3

import argparse
import json
from pathlib import Path


def properties_version(path: Path) -> str:
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("version="):
            return line.split("=", 1)[1].strip()
    raise SystemExit(f"version is missing from {path}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tag", default="")
    args = parser.parse_args()

    json_version = json.loads(Path("library.json").read_text(encoding="utf-8"))["version"]
    properties = properties_version(Path("library.properties"))
    if json_version != properties:
        raise SystemExit(
            f"manifest version mismatch: library.json={json_version}, library.properties={properties}"
        )

    if args.tag:
        expected = args.tag[1:] if args.tag.startswith("v") else args.tag
        if json_version != expected:
            raise SystemExit(f"tag {args.tag} does not match manifest version {json_version}")

    print(f"Phase release metadata is consistent: {json_version}")


if __name__ == "__main__":
    main()
