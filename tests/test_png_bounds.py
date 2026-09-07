#!/usr/bin/env python3
# test_png_bounds.py - a PNG must not be trusted about its own chunk lengths
# usage: python tests/test_png_bounds.py
#
# collect_idat (src/png.c) copies every IDAT payload with the length taken
# from the file. A crafted PNG that declares an IDAT of 0xFFFFFFFF bytes
# used to make memcpy read past the file buffer and write past the IDAT
# buffer. The tool must now refuse it with "bad chunk length" and exit 1,
# and must still accept a well-formed PNG (the positive control: without it
# a tool that refused every PNG would pass the first check).

import os
import struct
import subprocess
import sys
import tempfile
import zlib

TOOL = "./bin/imgpoison"
SIG = b"\x89PNG\r\n\x1a\n"


def chunk(ctype, data):
    """a well-formed chunk: length | type | data | crc."""
    return (struct.pack(">I", len(data)) + ctype + data
            + struct.pack(">I", zlib.crc32(ctype + data) & 0xFFFFFFFF))


def ihdr(width, height):
    # 8-bit RGB, no interlace
    return chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))


def valid_png():
    row = b"\x00" + b"\x80\x80\x80"  # filter None + one RGB pixel
    return SIG + ihdr(1, 1) + chunk(b"IDAT", zlib.compress(row)) + chunk(b"IEND", b"")


def crafted_png():
    # IDAT declares 0xFFFFFFFF bytes; only 4 bytes follow, and no IEND.
    return SIG + ihdr(1, 1) + struct.pack(">I", 0xFFFFFFFF) + b"IDAT" + b"\x00" * 4


def run(png_bytes):
    with tempfile.NamedTemporaryFile(suffix=".png", delete=False) as f:
        f.write(png_bytes)
        path = f.name
    try:
        return subprocess.run([TOOL, "--extract", "--method", "lsb", path],
                              capture_output=True, text=True)
    finally:
        os.unlink(path)


def main():
    if not os.path.exists(TOOL):
        print(f"{TOOL} not found - run make first")
        return 2

    failures = 0

    r = run(crafted_png())
    if r.returncode == 1 and "bad chunk length 4294967295" in r.stderr:
        print("PASS crafted PNG (IDAT length 0xFFFFFFFF) refused with exit 1")
    else:
        failures += 1
        print(f"FAIL crafted PNG: exit {r.returncode}, stderr: {r.stderr.strip()[:200]}")

    r = run(valid_png())
    if r.returncode >= 0 and "bad chunk length" not in r.stderr:
        print("PASS well-formed PNG passes the length check")
    else:
        failures += 1
        print(f"FAIL well-formed PNG: exit {r.returncode}, stderr: {r.stderr.strip()[:200]}")

    print("all passed" if failures == 0 else f"{failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
