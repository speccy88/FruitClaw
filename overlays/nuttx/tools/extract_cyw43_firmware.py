#!/usr/bin/env python3
############################################################################
# tools/extract_cyw43_firmware.py
#
# SPDX-License-Identifier: Apache-2.0
#
# Licensed to the Apache Software Foundation (ASF) under one or more
# contributor license agreements.  See the NOTICE file distributed with
# this work for additional information regarding copyright ownership.  The
# ASF licenses this file to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance with the
# License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
# License for the specific language governing permissions and limitations
# under the License.
#
############################################################################

import pathlib
import re
import sys


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: extract_cyw43_firmware.py <input.h> <output.bin>",
              file=sys.stderr)
        return 2

    src = pathlib.Path(sys.argv[1])
    dst = pathlib.Path(sys.argv[2])
    text = src.read_text(encoding="ascii")
    match = re.search(r"=\s*\{(?P<body>.*?)\};", text, re.S)

    if match is None:
        print(f"{src}: could not find firmware byte array", file=sys.stderr)
        return 1

    data = bytes(int(token, 16)
                 for token in re.findall(r"0x([0-9a-fA-F]{1,2})",
                                         match.group("body")))

    if not data:
        print(f"{src}: no firmware bytes found", file=sys.stderr)
        return 1

    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(data)
    return 0


if __name__ == "__main__":
    sys.exit(main())
