#!/usr/bin/env python3
"""Gzip the device web UI so the firmware embeds it compressed.

The server sends the blob with Content-Encoding: gzip, so the browser
decompresses it and the application image only carries ~15 KB instead of
~66 KB of HTML. The output is deterministic (mtime is zeroed), so the file
does not change when the source does not.
"""

import argparse
import gzip
import pathlib


def pack(source: pathlib.Path, target: pathlib.Path) -> None:
    data = source.read_bytes()
    with open(target, "wb") as raw:
        with gzip.GzipFile(fileobj=raw, mode="wb", compresslevel=9, mtime=0) as gz:
            gz.write(data)
    print(f"Packed {source.name}: {len(data)} -> {target.stat().st_size} bytes")


def main() -> None:
    default_source = pathlib.Path(__file__).resolve().parents[1] / "main/features/web/www/index.html"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=pathlib.Path, default=default_source)
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()

    source = args.input.resolve()
    target = args.output.resolve() if args.output else source.with_suffix(source.suffix + ".gz")
    pack(source, target)


if __name__ == "__main__":
    main()
