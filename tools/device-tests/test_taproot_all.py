#!/usr/bin/env python3
"""Combined on-device test for the taproot key-path mod.

Cases:
  1. psbt_ss_p2tr_taptree_keypath.json          POSITIVE  key path + merkle root
  2. psbt_ss_p2tr_taptree_scripts.json          POSITIVE  key path + merkle root + tapleaf script (0x15)
  3. psbt_ss_p2tr_taptree_multikey_leafhash.json POSITIVE  Liana shape: script key listed first, internal key second
  4. psbt_ss_p2tr_taptree_multikey.json          NEGATIVE  two empty-leaf-hash keypaths -> refused, unchanged
  5. psbt_ss_p2tr_taptree_scriptonly.json        NEGATIVE  script-path key only, no key-path origin -> refused, unchanged

For positives the returned PSBT must be byte-identical to expected_output.psbt
(the key-path signature). For negatives it must equal the input (unchanged).

Run with the ESP-IDF python:
  /Users/kccleoc/.espressif/python_env/idf5.1_py3.12_env/bin/python \
      tools/device-tests/test_taproot_all.py

Device should be factory-reset/UNINIT (no PIN). Approve on-device prompts.
"""
import base64
import json
import os
import sys

from jadepy import JadeAPI
from jadepy.jade import JadeError

JADE_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DATA = os.path.join(JADE_DIR, "tests/rpc/data/sign_psbt")
MNEMONIC = "paddle puppy easily actor poet apart screen drastic city front predict damp"
DEVICE = os.environ.get("JADE_DEVICE", "/dev/cu.usbmodem1234561")

CASES = [
    ("psbt_ss_p2tr_taptree_keypath.json", "positive"),
    ("psbt_ss_p2tr_taptree_scripts.json", "positive"),
    ("psbt_ss_p2tr_taptree_multikey_leafhash.json", "positive"),
    ("psbt_ss_p2tr_taptree_multikey.json", "negative"),
    ("psbt_ss_p2tr_taptree_scriptonly.json", "negative"),
]


def _decode(value):
    if isinstance(value, str) and value.startswith("b64:"):
        return base64.b64decode(value[4:])
    if isinstance(value, str) and value.startswith("0x"):
        return bytes.fromhex(value[2:])
    return value


def main():
    jade = JadeAPI.create_serial(device=DEVICE, timeout=180)
    jade.connect()
    results = []
    try:
        print("PING   :", jade.ping(), flush=True)
        print("setting test mnemonic ...", flush=True)
        jade.set_mnemonic(MNEMONIC)

        for name, kind in CASES:
            with open(os.path.join(DATA, name)) as f:
                c = json.load(f)
            in_psbt = _decode(c["input"]["psbt"])
            exp_psbt = _decode(c["expected_output"]["psbt"])
            print(f"\n[{kind}] {name}", flush=True)
            print("  desc:", c.get("description", "")[:120], flush=True)
            try:
                out = jade.sign_psbt("localtest", in_psbt, None)
            except JadeError as err:
                print("  UNEXPECTED JadeError:", err, flush=True)
                results.append((name, False, "JadeError"))
                continue

            if kind == "positive":
                passed = out == exp_psbt
                print(f"  returned {len(out)} bytes | matches_expected={out == exp_psbt}", flush=True)
            else:
                passed = out == in_psbt and out == exp_psbt
                print(f"  returned {len(out)} bytes | unchanged={out == in_psbt}", flush=True)
            results.append((name, passed, kind))
    finally:
        jade.disconnect()

    print("\n=== RESULTS ===")
    all_ok = True
    for name, passed, kind in results:
        print(f"  {'PASS' if passed else 'FAIL'}  [{kind}] {name}")
        all_ok = all_ok and passed
    print("ALL CASES PASS:", all_ok)
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
