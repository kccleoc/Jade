# Jade DIY fork — status, takeaways and roadmap

Purpose: a single handover/research doc for the `tdisplays3pro-ov5640` fork, so a
future agent can resume without re-deriving everything. Keep it current after each
change-set. Operational workflow lives in the skill `.opencode/skills/jade-diy-fw/SKILL.md`.

Repo: `/Volumes/Crucial2T/Mac/leo_temp/lilygo/Jade`
Remotes: `origin` = Blockstream/Jade (upstream, GitLab develops; GitHub mirror),
`fork` = kccleoc/Jade (personal, push here).
Toolchain: ESP-IDF **v5.1.2** (`/Volumes/Crucial2T/Mac/leo_temp/lilygo/esp/esp-idf`).
CI builds the DIY board on the `blockstream/jade_builder` image (IDF **v5.5.4**).

---

## 1. Branch model

Two tiers (codified in the skill):

- `tdisplays3pro-ov5640` — **integration** branch (board support + upstream merges). Build/flash from here.
- Topic branches, one change each:

| Branch | Base | Upstreamable | Content |
| --- | --- | --- | --- |
| `fix/gui-split-varargs-abort` | `origin/master` | yes | qrmode/dashboard out-of-range split args |
| `fix/pinserver-reply-timeout` | `origin/master` | yes | bounded wait for the pinserver reply |
| `feat/taproot-keypath-taptree` | `origin/master` | yes | taproot key-path (merkle root + leaf scripts) |
| `feat/usb-host-storage` | board history | no (needs `tdisplays3pro.inc`) | SY6970 OTG USB host storage |
| `backup/tdisplays3pro-pre-topic-split` | — | — | safety snapshot before the split |

All pushed to `fork`. Topic branches based on `origin/master` are MR candidates
(upstream is GitLab; submit there, not GitHub).

Commit inventory (integration, newest first): `7301b1b3` docs, `741d3057` taproot
leaf scripts, `2c61d998` skill fork workflow, `1835b8c2` skill branch model,
`b1628279` taproot key-path, `170fd3ac` USB host storage, `ae29dc54` pinclient
`pdMS_TO_TICKS` fix, `ad26f8e3` plan docs, `32fa924f` PIN-bind/gui + serial +
pinserver, `eb42cda1` skill, `6d089ff2` QR passphrase/backlight/camera,
`33f86217` PWM backlight/USB reliability/custom oracle, plus board commits.

## 2. Shipped and verified

| Item | What | Evidence |
| --- | --- | --- |
| PIN-bind abort fix | `gui_make_vsplit` declared more `parts` than values (`main/ui/qrmode.c`, `main/ui/dashboard.c`) tripped the upstream assert at `main/gui.c:1091` on the pinserver `blkstrm.com/pn` screen | committed `32fa924f` / topic `bf011913`; user confirmed PIN bind works |
| Pinserver reply timeout | `main/process/pinclient.c` + `main/process.c/.h`: `jade_process_*_in_message_with_timeout()`, 60s bound, retryable | `32fa924f` / topic `a88dac1b` |
| Serial TX yield fix | `main/serial.c`: `vTaskDelay(1/portTICK_PERIOD_MS)` == 0 at 100 Hz → `vTaskDelay(1)` | `32fa924f` |
| libjade build fix | `pinclient.c` used `pdMS_TO_TICKS` (libjade shim has only `portTICK_PERIOD_MS`) → `... / portTICK_PERIOD_MS` | `ae29dc54` |
| USB host mass storage | SY6970 OTG; truthful `usb_is_powered()`; camera shares the PMU I²C bus; version-conditional I²C | `170fd3ac` / topic `edf18d42`; user confirmed USB works on device |
| Taproot key-path (BIP341) | allow single-keypath key-path spends of taptree outputs: merkle root **and** leaf scripts allowed; >1 keypath and output taptrees still rejected | `b1628279`, `741d3057`; verified on device |

On-device verification (firmware built from the current tree, flashed via
`/dev/cu.usbmodem14101`, app `/dev/cu.usbmodem1234561`):

```
PING: 0
PASS [positive] psbt_ss_p2tr_taptree_keypath.json   key path + merkle root
PASS [positive] psbt_ss_p2tr_taptree_scripts.json   key path + merkle root + 0x15 leaf script
PASS [negative] psbt_ss_p2tr_taptree_multikey.json  2 keypaths -> refused, returned unchanged
```
Positive cases are byte-identical to the vectors' `expected_output.psbt` (the same
assertion CI's `test_libjade` makes).

## 3. Taproot: what works and the exact guard

`main/utils/psbt.c` `key_iter_is_supported_taproot()` now:
- rejects `keypaths->num_items > 1` (multisig script-path shape),
- rejects output `taproot_tree` (`PSBT_OUT_TAP_TREE`, 0x06),
- **allows** the merkle root (`PSBT_IN_TAP_MERKLE_ROOT`, 0x18) **and** leaf scripts (`PSBT_IN_TAP_LEAF_SCRIPT`, 0x15).

A leaf script is not a script-path signal: libwally's
`wally_psbt_sign_input_bip32()` reads only the merkle root, applies the BIP341
tweak, and writes only `PSBT_IN_TAP_KEY_SIG`. Script-path signing remains
unsupported. This unblocks Liana-style wallets that include the tree in their PSBT.

Vectors: `tests/rpc/data/sign_psbt/psbt_ss_p2tr_taptree_{keypath,scripts,multikey}.json`
+ generator `generate_taptree_keypath_vectors.py` (wallycore 1.5.3). The `psbt_ss_*`
glob auto-collects them; no CI change needed.

## 4. Descriptor registration (important for Liana)

Jade can register/save a descriptor when libwally can parse and address-derive it.
Tested on device:

| Descriptor | Result |
| --- | --- |
| keyspend `tr(@0/<0;1>/*)` | **registered + saved** (`register: True`, read back via `get_registered_descriptor`) |
| Liana `wsh(or_d(multi(2,…),and_v(v:pkh(…),older(100))))` | supported (fixture `test_data/descriptor_ss_liana.json`) |
| Liana taproot `tr(@0/<0;1>/*,{and_v(v:multi_a(3,…),older(20)),and_v(v:multi_a(2,…),older(65534))})` | **JadeError -32602 "Failed to parse descriptor"** |

So today Liana can push/save its `wsh` descriptor but **not** its taproot
descriptor. Signing the taproot key path still works (uses PSBT fields, not the
registered descriptor), but Jade then has no descriptor for address verification or
change recognition (outputs are manual/external).

## 5. Roadmap: libwally-level work (the main future research)

The remaining gaps are in the vendored libwally
(`components/libwally-core/upstream`, pinned `release_1.5.6-11-g3bf543cd`,
`3bf543cd06a67fdd877688a6304808f270351aee`). This pin is **newer than the latest
upstream release** (`release_1.5.6`, 2026-07-11); no newer release adds the
following, so it would have to be implemented (ideally upstreamed to
elementsProject/libwally-core).

1. **Taproot script-tree descriptors** — `src/descriptor.c`:
   - `verify_tr()` rejects `child_count != 1` (`/* FIXME: Support script paths */`).
   - `generate_tr()` passes a NULL merkle root.
   Needed: build the taptree from `tr(KEY,{script,…})`/`tr(KEY,script)`, compute the
   merkle root and the tweaked output key, and implement `to_script`/`to_address`
   for such descriptors. This alone would let Jade **register/save** a Liana taproot
   descriptor (and do address verification + change detection).
2. **Script-path signing** — `src/psbt.c`:
   - `wally_psbt_get_input_signature_hash()` forces `script = NULL` (key-path
     sighash) with `/* FIXME: Support script path spends */`; needs BIP342
     (tapscript) sighash with the leaf script/version, codeseparator and annex.
   - `wally_psbt_sign_input_bip32()` only emits the key-path
     `PSBT_IN_TAP_KEY_SIG`; needs per-leaf signing producing
     `PSBT_IN_TAP_SCRIPT_SIG` from `taproot_leaf_scripts`/`taproot_leaf_hashes`
     and control blocks.
   - There is **no** miniscript satisfaction / witness-assembly engine
     (`wally_descriptor_*` exposes parse + metadata + `to_script`/`to_address` only).
3. **Then Jade layers** (only after the above): `main/descriptor.c`
   (register/verify `tr()` trees), `main/process/sign_psbt.c` (leaf selection, leaf
   sighash, script-sig assembly, change recognition), `main/utils/psbt.c` (parse/
   expose leaf scripts + control blocks + leaf hashes), `main/process/sign_utils.c`
   (sighash policy currently ALL/DEFAULT), and `main/ui/sign_tx.c` (display the
   miniscript policy/timelocks being spent).
4. **BIP327 MuSig** is also absent from the built firmware (`ENABLE_MODULE_MUSIG`
   is not defined in `main/amalgamated.c`; secp256k1-zkp musig source is present
   but not compiled) — relevant only for multisig taproot key-path aggregation.

Recommendation: a real Liana-taproot end-to-end test (descriptor registration +
key-path signing) should first validate the signing path we shipped; the taptree
descriptor + script-path work is a libwally project, best proposed upstream.

## 6. Verification / CI notes

- `test_libjade*` (Linux) runs `pytest tests` including the taproot vectors; the
  local macOS host **cannot** build libjade (pre-existing `explicit_bzero` gap in
  vendored libwally) → CI is the gate there.
- `test_format` uses `clang-format-19`; install via `pip install clang-format==19.1.7`
  (brew only has newer/older). Run it only on changed files, not `./format.sh`, when
  other edits are in flight.
- `test_configs` regenerates `configs/*.defaults`; the DIY build job is
  `build_diy_display_ttgo_tdisplays3procamera` (compile-only).
- On-device scripts live in `../lilygo/`: `test_taproot_optionA.py` (key-path
  positive), `test_taproot_negatives.py` (legacy leaf-script negative — superseded),
  `test_taproot_all.py` (all three current cases). Run with the IDF python and
  `PYTHONPATH=<repo>`; `set_mnemonic` is RAM-only (`debug_set_mnemonic`), so use a
  factory-reset/UNINIT device and reload the real seed afterwards.

## 7. Guardrails (learned the hard way)

- **Never** change the shared toolchain: no `git checkout`/`switch_to.sh`/`install.sh`
  in the ESP-IDF repo. It is pinned to v5.1.2 while CI uses v5.5.4; guard any newer
  API with `ESP_IDF_VERSION` (see `main/power/tdisplays3pro.inc`).
- Never commit `build_s3pro/` or `sdkconfig_tdisplays3pro`.
- `drain()` in jadepy blocks for the full serial read timeout — don't call it with a
  long timeout (it looks like a hang).
- After any upstream merge, re-verify every registry item in the skill before building.

## 8. Open questions / next steps

1. End-to-end with real **Liana**: does its key-path PSBT carry `0x16/0x17/0x18`
   as expected, and can Jade be the sole signer? (Firmware side is proven.)
2. Decide whether to pursue **libwally taptree descriptors** (enables Liana taproot
   descriptor registration and, later, script-path) — likely an upstream MR.
3. USB host storage: on-device checklist for the three actions + no-battery hot-plug
   (user reported the feature working).
4. Optional: ship the taproot change as an upstream MR from the
   `feat/taproot-keypath-taptree` topic branch.
