# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import hashlib
import sys
from pathlib import Path


def main():
    license_file, expected_hash, receipt_file = sys.argv[1:]
    license_text = Path(license_file).read_bytes()
    if hashlib.sha256(license_text).hexdigest() != expected_hash:
        sys.exit("The NVIDIA driver agreement failed its SHA-256 check.")

    try:
        with open("/dev/tty", encoding="utf-8") as terminal_input:
            print(license_text.decode("utf-8"), flush=True)
            print(
                "If you accept this agreement for the NVIDIA guest, type 'yes' to continue: ",
                end="",
                flush=True,
            )
            answer = terminal_input.readline()
    except OSError:
        sys.exit("Run the NVIDIA guest build in an interactive terminal to review and accept the agreement.")
    if answer.strip().lower() != "yes":
        sys.exit("The NVIDIA driver agreement was not accepted. The NVIDIA guest build stopped.")

    Path(receipt_file).write_text(f"{expected_hash}\n", encoding="ascii")


if __name__ == "__main__":
    main()
