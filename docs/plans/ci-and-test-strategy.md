# CI & Test Strategy — USB Host Storage + Taproot Keypath (DIY fork)

Status: plan (read-only investigation; no source/build changes)
Owner: CI/QA view for the two in-flight features
Cross-refs:
- `docs/plans/usb-host-storage.md` — USB feature design (this doc covers the CI/QA angle only)
- `docs/plans/taproot-keypath-taptree.md` — Taproot feature design

Scope: describe which existing CI gates each feature must pass, where the new
tests plug in, what cannot be automated, and the required-vs-optional CI
recommendation. Nothing here changes pipeline config.

---

## 1. Existing CI gate map

Pipeline definition: `.gitlab-ci.yml` (stages at lines 7–17; includes at lines 21–36).
Default image for all jobs: `blockstream/jade_builder@sha256:8b739db85b6b99664db0e3ece57cddfa4e0fee4101a20ca930c391273f21e2f9`
(`.gitlab-ci.yml:19`). GitHub mirror runs a separate, stock-board-only workflow.

| Gate / job | File:line | Stage | What it does | Covers USB | Covers Taproot |
|---|---|---|---|---|---|
| `test_libjade` | `gitlab/test_libjade.yml:11` | `pre_test` | `make_libjade.sh Debug`; `test_jade.py --libjade`; **`pytest -v -s --libjade tests`** | no (host libjade) | **yes** — runs all `tests/` incl. `tests/rpc` |
| `test_libjade_sanitize` | `gitlab/test_libjade.yml:34` | `test` | libjade `Sanitize`; ASAN/UBSAN/Fuzzer: `pytest --libjade tests/`, then `--serialport` device run `pytest --device $SOCKET_LINK tests/` | no | **yes** — memory-safety + serial path |
| `test_libjade_selfcheck` | `gitlab/test_libjade.yml:20` | `test` | per-`libjade/selfcheck/*.c` selfcheck under ASAN | no | indirect (crypto primitives) |
| `test_libjade_coverage` | `gitlab/test_libjade.yml:61` | `test` (manual) | coverage run of `pytest --libjade --no-legacy-flow tests/` | no | optional signal |
| `test_format` | `gitlab/test.yml:14` | `test` | `./format.sh` then `idf.py reconfigure` then `git diff --exit-code` | **yes** (`.inc` is clang-formatted) | yes (`.c/.h`) |
| `test_configs` | `gitlab/test.yml:24` | `test` | `./tools/check_default_configs.sh` then `git diff --exit-code` | **yes** (if defaults touched) | if defaults touched |
| `test_bip85_rsa_key_gen` | `gitlab/test.yml:5` | `test` | tools build + `git diff --exit-code` | no | no |
| `build_diy_display_ttgo_tdisplays3procamera` | `gitlab/diy_fw.yml:42` (template `:7`) | `build_diy` | compile-only for the board, using `configs/sdkconfig_display_ttgo_tdisplays3procamera.defaults` (derived at `diy_fw.yml:13`) | **yes — primary compile gate** | no |
| `build_test_qemu*` | `gitlab/test_fw.yml:56,61,66` | `build_test` | builds qemu `_ci` images via `tools/switch_to.sh qemu` | no | builds generally |
| `flash_qemu*` | `gitlab/flash.yml:25,34` | `flash` | qemu smoke boot (`qemu_ci_flash.sh --sample-percent=33`) | no (no host hw) | boots only |
| `build_api_docs` | `gitlab/apidocs.yml:4` | `release` | `cd docs && make html` → Sphinx | no | no (see §6) |
| `build_v1_1` / `build_v2` / `build_v2c` | `.github/workflows/github-actions-test.yml:4,18,32` | n/a | stock Jade boards only, ESP-IDF **v5.5.4** | **no** (board not built here) | no |

Notes:
- `format.sh` runs `clang-format-19 -i` over `main` `*.c *.h */*.{c,h,inc}` (`format.sh:14`), which
  **includes `main/power/tdisplays3pro.inc`** — so USB edits in that file are format-gated.
- `check_default_configs.sh` regenerates every `configs/*.defaults` (`tools/check_default_configs.sh:12–35`).
  Any hand-edit of `configs/sdkconfig_display_ttgo_tdisplays3procamera.defaults` must survive
  `save-defconfig`, otherwise `test_configs` fails on `git diff --exit-code`.
- The DIY board is **not** built by the GitHub workflow (`.github/workflows/github-actions-test.yml`),
  only by GitLab `build_diy_*`. Local/DIY correctness therefore depends on the GitLab pipeline.

---

## 2. Taproot keypath — CI path for the new RPC vectors

How `tests/rpc` is executed:
- `tests/rpc/test_sign_psbt.py:61–64` (`test_sign_ss_psbt`) is parameterized by
  `@with_test_cases('tests/rpc/data/sign_psbt/psbt_ss_*.json')`; the Liquid variant
  `test_sign_ss_pset` at `:67–70` uses `pset_ss_*.json`.
- The glob expander is `with_test_cases()` in `tests/__init__.py:78–93`: each matching
  JSON file becomes a distinct pytest case id (basename without extension). A new file is
  picked up automatically — **no Python change is needed**.
- The CI runner is `test_libjade` (`gitlab/test_libjade.yml:18`: `pytest -v -s --libjade tests`).
  Because it collects all of `tests/`, a new `tests/rpc/data/sign_psbt/psbt_ss_*.json` runs there.
- `test_libjade_sanitize` re-runs the same corpus under ASAN/UBSAN
  (`gitlab/test_libjade.yml:46`) and again over a serial/socket transport (`:53`), giving the
  taproot paths both memory-safety and transport coverage.

Where the guard lives / what the vectors must exercise:
- `main/utils/psbt.c:13–37` `key_iter_is_supported_taproot()` rejects taproot inputs/outputs
  that carry a script tree: >1 taproot keypath (`:17–19`), `taproot_leaf_scripts` (`:22–24`),
  the BIP-174 merkle-root field (`:25–29`), or a `taproot_tree` on outputs (`:32–34`).

Required new vectors (positive + negative), all JSON only:
1. **Positive keypath-only**: e.g. `tests/rpc/data/sign_psbt/psbt_ss_p2tr_keypath_*.json`
   (mirrors existing `psbt_ss_p2tr_default_all.json`, and `pset_ss_p2tr.json` for Liquid).
   `expected_output.psbt` / optional `expected_output.txn` per `test_sign_psbt.py:37,40–48`.
2. **Negative script-tree**: files the guard must skip. NOTE: a rejected taproot input is
   **not** an RPC error — `key_iter_input_begin_public()` returns false, `signing_flags` stays 0,
   the UI shows "There are no relevant inputs to be signed" and the PSBT is returned
   **unchanged** (`main/process/sign_psbt.c:1052-1055, 1109-1129`). So these cases must
   assert `expected_output.psbt == input psbt` and carry **no** `expected_error` (the harness
   asserts failure if `expected_error` is absent but an error is raised, and vice-versa:
   `tests/rpc/test_sign_psbt.py:29-37`). Precedent: `psbt_ss_p2wpkh_already_signed.json`.
   Cover: input leaf script (0x15), output taptree (0x06), and >1 taproot keypath.
   Do **not** treat the input merkle-root field (0x18) as a negative: it is the enabler that
   makes the *positive* key-path case work (see `docs/plans/taproot-keypath-taptree.md` §3.1).
3. Same set for the `sign_tx` flow if the taproot path is reachable through it
   (`tests/rpc/data/sign_tx/ss_tx_*` positive, `bad_ss_tx_*` negative; see
   `tests/rpc/test_sign_tx.py:205–235`).

Negative-test convention to follow: the existing single-sig negative pattern is the
`bad_ss_tx_*.json` family (`tests/rpc/test_sign_tx.py:232–235`); for psbt the "bad" case is
expressed by adding `expected_error` to an otherwise-`psbt_ss_*` case (there is no separate
`bad_psbt_ss_*` glob). Do not invent a new glob — the parameterization asserts the glob matched
≥1 file (`tests/__init__.py:92`), so a typo'd/new prefix silently becomes a collection error.

**Conclusion: no new CI job is required for taproot.** New JSON vectors are sufficient; they run
in `test_libjade` and `test_libjade_sanitize`, plus the optional manual `test_libjade_coverage`.

---

## 3. USB host mass storage — CI limits and manual verification

### Why it cannot be CI-tested
- Host mass storage requires an attached **USB mass-storage device** and the board's
  USB-host power path. The runner images are headless containers
  (`blockstream/jade_builder`, `.gitlab-ci.yml:19`); there is no physical USB port, no drive,
  and no battery on the `build_diy`/qemu runners.
- The board is **compile-only** in CI: `build_diy_display_ttgo_tdisplays3procamera`
  (`gitlab/diy_fw.yml:42`) produces artifacts but is never flashed or exercised; it is a
  `build_diy`-stage job with no test dependency.
- qemu cannot emulate the USB-OTG host + storage stack; `flash_qemu*` only boots qemu
  (`gitlab/flash.yml:15–18`).
- Power/battery reporting is stubbed for this board (`main/power/tdisplays3pro.inc:83–93`,
  `usb_is_powered()` returns `true` at `:95`), so any "host powered" assertion is meaningless
  without real hardware. If the USB feature changes these stubs, behavior must be verified
  on-device.

### Gates that must stay green
1. `build_diy_display_ttgo_tdisplays3procamera` (`gitlab/diy_fw.yml:42`) — compile + `idf.py size`
   of the edited `main/power/tdisplays3pro.inc`.
2. `test_format` (`gitlab/test.yml:14`) — clang-format-19 must accept the `.inc`
   (`format.sh:14`), and `idf.py reconfigure` + `git diff --exit-code` must stay clean.
3. `test_configs` (`gitlab/test.yml:24`) — only if `configs/sdkconfig_display_ttgo_tdisplays3procamera.defaults`
   changes (e.g. a new `CONFIG_*_USB_MSD` switch). Regenerated defaults must round-trip.
4. `test_libjade*` — unaffected, but must not regress (no host-side code is expected to change).

### Manual on-device checklist (CI/QA view)
Attach this to `docs/plans/usb-host-storage.md` rather than duplicating depth. Keep the two docs
cross-linked; this is the acceptance/regression checklist.

- [ ] Build the DIY target locally and flash the T-Display S3 Pro.
- [ ] Boot to home screen with a **fully charged battery** and a **known-good FAT32 USB drive**.
- [ ] Feature disabled (default): device boots, no enumeration, no extra current draw, QR/camera
      flows behave as before.
- [ ] Feature enabled: drive enumerates; filesystem mounts/reads (list a known file).
- [ ] Unplug while mounted → clean error state, no panic/reboot loop; screen remains usable.
- [ ] Replug → re-enumerate without power-cycle.
- [ ] Concurrent use: camera live preview + backlight dimming still work (`main/power/tdisplays3pro.inc`
      owns GPIO48 backlight; confirm no GPIO/LEDC conflict).
- [ ] Battery present vs external-only: confirm no brown-out on hot-plug.
- [ ] Deep-sleep / `power_shutdown()` (`main/power/tdisplays3pro.inc:39–44`) with drive attached
      does not corrupt FS.
- [ ] Record serial log; confirm no watchdog resets during the whole session.

---

## 4. Required vs optional CI recommendations

**Required (do now, no pipeline edits):**
- Taproot: add the `psbt_ss_*.json` / `pset_ss_*.json` positive + negative vectors. They are
  collected automatically in `test_libjade` and `test_libjade_sanitize`. No new job.
- USB: keep `build_diy_display_ttgo_tdisplays3procamera`, `test_format` and (if defaults change)
  `test_configs` green; treat the §3 checklist as the acceptance criterion.

**Optional / consider (only if it adds real signal, still no new job):**
- Add a `libjade/selfcheck` case for the taproot guard if a pure-C entry point is exposed;
  this would run in `test_libjade_selfcheck` (`gitlab/test_libjade.yml:20`). Low priority —
  RPC negative vectors already cover it end-to-end.
- If USB adds a compile-time Kconfig switch, add it to the board defaults and rely on
  `test_configs`; do **not** add a second DIY build variant.
- Do **not** add a USB CI job: it would only re-compile the same source.

**Risk to confirm — toolchain divergence:**
- Local DIY builds use **ESP-IDF v5.1.2**, while the pipeline builder targets **v5.5.4**
  (`Dockerfile:9–10`; `gitlab/docker.yml:24` `IDF_CLONE_BRANCH_OR_TAG=v5.5.4`) and the GitHub
  workflow pins v5.5.4 (`.github/workflows/github-actions-test.yml:14,28,42`). The pinned image
  digest at `.gitlab-ci.yml:19` may predate the v5.5.4 Dockerfile — **verify the pinned
  `jade_builder` sha actually contains v5.5.4** before trusting a green `build_diy` for USB
  (LEDC/USB-host driver API differences between 5.1 and 5.5 can compile locally but fail in CI,
  or vice-versa).

---

## 5. Merge gate checklists

### Taproot keypath
- [ ] Positive keypath-only RPC vector(s) added under `tests/rpc/data/sign_psbt/` as `psbt_ss_*.json`.
- [ ] Negative script-tree vectors added asserting the PSBT is returned unchanged (**no**
      `expected_error`): input leaf script, output taptree, multi-keypath.
- [ ] `test_libjade` green (`pytest --libjade tests`).
- [ ] `test_libjade_sanitize` green (ASAN/UBSAN + serial transport).
- [ ] `test_format` green (`git diff --exit-code` after `./format.sh`).
- [ ] `test_configs` green if any `configs/*.defaults` touched.
- [ ] No new pytest glob / no CI job added.

### USB host storage
- [ ] `build_diy_display_ttgo_tdisplays3procamera` green.
- [ ] `test_format` green; `.inc` passes clang-format-19.
- [ ] `test_configs` green if defaults changed.
- [ ] Manual checklist in §3 executed and serial log attached.
- [ ] Builder-image IDF version confirmed (see §4 risk).
- [ ] Cross-referenced from `docs/plans/usb-host-storage.md`.

---

## 6. Do `docs/plans/*.md` affect the apidocs job?

`build_api_docs` (`gitlab/apidocs.yml:4`) runs `(cd docs && make html)` → `sphinx-build`
(`docs/Makefile:8,19`). Checklist:
- `docs/conf.py` sets **no `source_suffix`** (`docs/conf.py:25–39`) → Sphinx defaults to `.rst`.
- `docs/conf.py` has an **empty `extensions = []`** (`:30–31`) → `myst_parser`/`recommonmark`
  is not enabled, so `.md` is not a recognized source type.
- `exclude_patterns = ['_build', 'Thumbs.db', '.DS_Store']` (`:39`) does not list `plans/`.
- Only `docs/index.rst` is a Sphinx source; a subdirectory containing only `.md` files is
  ignored, and no toctree references it.

**Conclusion: `.md` files under `docs/plans/` are ignored by the Sphinx build and do not affect
`build_api_docs`. No config change is needed.** Recommendation: keep the plans as `.md` in
`docs/plans/` (safest — already invisible to Sphinx). Do not convert them to `.rst` and do not
touch `docs/conf.py`. If a future change enables a Markdown parser, add `'plans'` to
`exclude_patterns` at that time — not now.

---

## Appendix — local reproduction commands (read-only intent, run only when asked)

```sh
# Taproot vectors, host libjade (mirrors test_libjade)
./libjade/make_libjade.sh Debug
LD_LIBRARY_PATH=$PWD/build_linux/libjade pytest -v -s --libjade tests/rpc/test_sign_psbt.py

# Format / config gates (mirror GitLab test stage)
./format.sh && git diff --exit-code
./tools/check_default_configs.sh && git diff --exit-code
```
