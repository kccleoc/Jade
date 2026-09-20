# Taproot key-path signing for outputs with a script tree (Liana-style vault)

Status: implementation plan (read-only investigation; **no source/build changes** in this doc)
Fork: Blockstream Jade DIY (`tdisplays3pro-ov5640` branch)
Scope owner: signing/PSBT layer
Related: `docs/plans/ci-and-test-strategy.md` (CI/QA view), `docs/plans/usb-host-storage.md`

---

## 1. Summary and intended user outcome

Today Jade can sign a BIP-86 `tr(key)` key-path spend (no script tree) but refuses
*any* Taproot output that commits to a script tree, even when the user only wants
the **key path**. The only blocker is a firmware guard; the vendored libwally
already performs the correct BIP-341 tweak using the PSBT merkle root.

**Intended outcome:** a user with a Liana Taproot vault (e.g. `tr(<owner-key>,
<and_v(...timelock...)>` / miniscript) can put their Jade-derived key in the
vault's internal key and **sign the direct key-path spend on Jade**, producing a
valid 64-byte Schnorr signature that a coordinator can finalize. Example:
"Liana taproot vault, sign the owner key path on Jade."

### Non-goals (explicitly out of scope)
- **Script-path / miniscript satisfaction.** Jade will not evaluate tapscripts,
  produce `PSBT_IN_TAP_SCRIPT_SIG`, or build a script-path control-block witness.
- **`wsh()` (SegWit v0) miniscript.** Already supported; untouched.
- **Taproot change-output *validation*.** Taptree change outputs remain
  user-confirmed (see §4). No descriptor/tree parsing.
- **Legacy anti-exfil (`sign_tx`) Taproot tree support** is analysed in §3.3 but
  deferred to a separate, opt-in wire-protocol task (see §7) — the primary target
  is the PSBT `sign_psbt` flow.

---

## 2. Precise current behaviour (verified against source)

### 2.1 The guard
`main/utils/psbt.c:13-37` `key_iter_is_supported_taproot()` is called from
`key_iter_next()` (`main/utils/psbt.c:118-126`) on the first iteration of a
Taproot key iterator, where `keypaths` is the input's `taproot_leaf_paths` map
(`main/utils/psbt.c:77-86`). It returns `false` (⇒ iterator invalid) when:

- `keypaths->num_items > 1` — `main/utils/psbt.c:17-19` ("multisig script-path");
- input has `input->taproot_leaf_scripts.num_items` — `:22-24`
  (`PSBT_IN_TAP_LEAF_SCRIPT`, 0x15);
- input has the merkle-root field — `:25-29`, hardcoded `0x18`
  (`const uint32_t psbt_in_tap_merkle_root = 0x18; // From BIP-174`, with a TODO
  to use a wally accessor);
- output has `output->taproot_tree.num_items` — `:32-34`
  (`PSBT_OUT_TAP_TREE`, 0x06).

Is-taproot is decided by `taproot_leaf_paths.num_items != 0`
(`main/utils/psbt.c:52-57`), i.e. the presence of `PSBT_IN_TAP_BIP32_DERIVATION`
(0x16) entries for Taproot keys.

### 2.2 Why the spend is refused
`sign_psbt()` calls `key_iter_input_begin_public()` per input
(`main/process/sign_psbt.c:814`). When the guard fails, that returns `false`, the
loop `continue`s (`:815-817`), no `sig_types[index]` is set, and the input is never
signed. `signing_flags` stays `0`, so the user simply sees "There are no relevant
inputs to be signed" (`main/process/sign_psbt.c:1052-1055`) and the PSBT comes back
unchanged. The same guard makes `key_iter_output_begin_public()`
(`main/process/sign_psbt.c:557`) ignore taptree outputs for change detection.

### 2.3 libwally already supports the crypto
- `wally_psbt_get_input_signature_hash()` forces the script to `NULL` for
  key-path spends: `components/libwally-core/upstream/src/psbt.c:4617-4621`.
- `wally_psbt_sign_input_bip32()` reads `PSBT_IN_TAP_MERKLE_ROOT` (0x18) and calls
  `wally_ec_private_key_bip341_tweak()` with it:
  `components/libwally-core/upstream/src/psbt.c:4678-4693`. It then writes only a
  key-path `PSBT_IN_TAP_KEY_SIG` (`:4707-4711`).
- The taproot marker for an input is derived from the prevout script type
  (p2tr ⇒ `WALLY_SIGTYPE_SW_V1`):
  `components/libwally-core/upstream/src/psbt.c:160-178`.
- libwally's own script-path support is still a stub:
  `components/libwally-core/upstream/src/psbt.c:4618`
  (`/* FIXME: Support script path spends */`) and `:4986`
  (`/* TODO support tapleaf spends input->taproot_leaf_signatures */`).

### 2.4 Signing loop and BIP-86 baseline
- `main/process/sign_psbt.c:1059-1107` is the signing loop. It computes the
  sighash with `wally_psbt_get_input_signature_hash()` (`:1083-1085`) — safe as-is
  for Taproot key path — then calls `wally_psbt_sign_input_bip32()` (`:1096-1097`).
- BIP-86 `tr(key)` works today and has vectors:
  `tests/rpc/data/sign_psbt/psbt_ss_p2tr_default_all.json` (and Liquid
  `tests/rpc/data/sign_psbt/pset_ss_p2tr.json`).

### 2.5 Legacy anti-exfil flow (confirmed)
`main/process/sign_tx.c:766-841` batches Taproot inputs and calls
`wallet_get_tx_input_hash()` (`main/wallet.c:1257-1280`), then
`wallet_sign_tx_input_hash()`. At `main/wallet.c:1190-1191` the BIP-341 tweak is
applied with an **empty** merkle root:

```c
wret = wally_ec_private_key_bip341_tweak(privkey, sizeof(privkey), NULL, 0, flags,
                                         tweaked, sizeof(tweaked));
```

Confirmed: for a taptree output this produces a signature over the BIP-86 tweak
(wrong key), so the legacy flow would emit an invalid signature, not a valid one.
There is currently **no way to convey a merkle root** through this flow:
`input_data_t` (`main/wallet.h:17-27`) has no merkle-root member and
`params_tx_input_signing_data()` (`main/process/process_utils.c:322-407`) has no
such RPC parameter. Plumbing it therefore requires a wire-protocol addition, not
just a `wallet.c` edit.

---

## 3. Minimal design

### 3.1 New condition for `key_iter_is_supported_taproot()`
Goal: allow a single-key key-path spend of a taptree output; keep refusing genuine
script-path shapes and multi-key trees. Delete the merkle-root rejection (it is
the *enabler*, not a script-path signal) and keep everything else:

```c
// main/utils/psbt.c — key_iter_is_supported_taproot()
static bool key_iter_is_supported_taproot(const key_iter* iter, const struct wally_map* keypaths)
{
    if (keypaths->num_items > 1) {
        return false; // More than one keypath: a multisig script-path spend
    }
    if (iter->is_input) {
        const struct wally_psbt_input* input = &iter->psbt->inputs[iter->index];
        if (input->taproot_leaf_scripts.num_items) {
            return false; // Leaf script present: script-path spend / unsupported
        }
        // PSBT_IN_TAP_MERKLE_ROOT is REQUIRED for a key-path spend of a taptree
        // output and is applied by libwally; allow it.
    } else {
        const struct wally_psbt_output* output = &iter->psbt->outputs[iter->index];
        if (output->taproot_tree.num_items) {
            return false; // Outputs are never signed; leave taptree detection as-is
        }
    }
    return true; // One keypath, no leaf scripts: key-path spend (with or w/o tree)
}
```

- **Allow:** exactly one keypath (the internal key) + `PSBT_IN_TAP_MERKLE_ROOT`.
- **Still reject:** `>1` keypath, `taproot_leaf_scripts` (0x15), output
  `taproot_tree` (0x06). Output rejection is kept deliberately; see §4.3.
- The `// TODO: use the wally merkle root accessor` and the hard-coded `0x18` are
  removed — the guard no longer needs to *read* the root.
- Only `main/utils/psbt.c` changes. The sign request must still carry
  `PSBT_IN_TAP_BIP32_DERIVATION` (0x16) for the internal key (that is what makes
  `is_taproot` true and lets `key_iter_next` derive the key); a bare
  `PSBT_IN_TAP_INTERNAL_KEY` with no derivation cannot be signed and is not claimed.

### 3.2 Merkle-root accessor?
Not needed for the PSBT flow. libwally reads the root internally
(`components/libwally-core/upstream/src/psbt.c:4680-4683`) and the guard only
needed to *reject* it. libwally exposes **no** public
`wally_psbt_input_get_taproot_merkle_root()`; the constant
`PSBT_IN_TAP_MERKLE_ROOT` is private (`.../src/psbt_io.h:118`), while the struct
field `input->psbt_fields` is public (Jade already used it at `main/utils/psbt.c:27`).
Recommendation: **do not fork libwally**. If a future feature needs the value
(e.g. an on-device display or `sign_tx` validation), define a single local
constant in Jade rather than a new upstream symbol.

### 3.3 Legacy-flow merkle-root plumb (`main/wallet.c`)
Only needed if a client uses the anti-exfil `sign_tx` RPC for taptree key paths.
The minimal, self-contained change set is:

1. `main/wallet.h:17-27` — add to `input_data_t`:
   `uint8_t tap_merkle_root[SHA256_LEN];` and `size_t tap_merkle_root_len;`.
2. `main/process/process_utils.c:322-407` `params_tx_input_signing_data()` — accept
   an optional per-input `taproot_merkle_root` byte string, only valid when the
   prevout script is p2tr (`is_p2tr` branch at `:387-397`); reject if provided for
   non-Taproot or if length ≠ 32.
3. `main/wallet.c:1186-1193` — pass `input_data->tap_merkle_root` /
   `input_data->tap_merkle_root_len` to `wally_ec_private_key_bip341_tweak()`
   instead of `NULL, 0` (for `WALLY_SIGTYPE_SW_V1`).
4. `main/process/sign_tx.c` — no structural change beyond the shared param parser;
   the sighash at `:834` is already key-path (`script=NULL`).

Risk: this is a client wire-protocol change (existing AE clients do not send the
field; they keep BIP-86 behaviour). It is **not required** for `sign_psbt`, so it
is scheduled as a separate, lower-priority task (§7) and gated on a real client
need. Do **not** silently fall back to the empty root for a tree output: without
the field the legacy flow should keep refusing taptree inputs rather than sign the
wrong tweak.

### 3.4 Does `main/process/sign_psbt.c` need changes?
No. Input recognition flows through `key_iter_input_begin_public()` (`:814`),
sighash through `wally_psbt_get_input_signature_hash()` (`:1083`, which nulls the
script for Taproot), and signing through `wally_psbt_sign_input_bip32()` (`:1096`,
which applies the merkle root). All the required behaviour comes from the relaxed
guard. Output recognition for taptree outputs deliberately stays unvalidated; the
existing code already treats unverified outputs as external/user-confirmed.

---

## 4. Open questions to resolve and record

### 4.1 What do real clients actually put in a key-path PSBT?
- **BIP-371 canonical key-path spend of a taptree output** is:
  `PSBT_IN_TAP_BIP32_DERIVATION` (0x16) for the internal key with **zero leaf
  hashes**, plus `PSBT_IN_TAP_MERKLE_ROOT` (0x18), and **no**
  `PSBT_IN_TAP_LEAF_SCRIPT` (0x15). Bitcoin Core's `walletprocesspsbt` "script
  root key spend" case follows exactly this shape. This is what §3.1 supports.
- **Liana** today does **not** support Jade for Taproot descriptors
  (`doc/SIGNING_DEVICES.md`: "Support for use in Taproot descriptors is not yet
  available in the firmware"). Its own signer (`liana/src/signer.rs`) keys off
  `tap_internal_key` + `tap_merkle_root` for the key path and separately iterates
  `tap_key_origins`/leaf hashes for script paths, so its fully-populated PSBTs can
  legitimately carry `tap_scripts`. **Unconfirmed** whether the PSBT it hands to an
  external signer for a *key-path* spend includes 0x15.
- **BDK / rust-bitcoin** represent the whole tree in `Psbt::tap_scripts`; a
  fully-populated PSBT commonly includes leaf scripts even when the key path is
  intended.
- **Action before finalising the guard:** obtain one real Liana (and one BDK)
  key-path PSBT built with the Jade test singlesig mnemonic and inspect it
  (`bitcoin-cli decodepsbt` / `wally.psbt_to_json`) for field 0x15. Commit the
  resulting decision as a test vector (§5) so it cannot silently regress.

### 4.2 Does relaxing the guard risk signing a leaf script we do not understand?
**No.** libwally's `wally_psbt_sign_input_bip32()` unconditionally emits a BIP-340
**key-path** signature (`PSBT_IN_TAP_KEY_SIG`) tweaked by the merkle root
(`components/libwally-core/upstream/src/psbt.c:4678-4711`); script-path signing is a
stub (`:4986`). Jade cannot be tricked into producing a tapscript signature.
The residual risk is *transparency*, not key safety: a key-path signature spends
the vault without revealing (or letting Jade verify) the script conditions (e.g.
timelocks). v1 limits this by requiring the PSBT to present as a pure key path.

### 4.3 Safest guard decision
Recommended v1: **exactly one keypath + no `taproot_leaf_scripts` + allow merkle
root**, outputs unchanged. This is the conservative reading of the spec and keeps
Jade off any input that also advertises script-path data. Because allowing 0x15 is
technically safe (§4.2), the fallback if §4.1 shows Liana/BDK include it is a
one-line follow-up (drop the `taproot_leaf_scripts` check) plus a new positive
vector. Keep the output `taproot_tree` rejection: relaxing it has no functional
effect anyway, since `verify_singlesig_script_matches()` builds the p2tr script
with the **empty**-root BIP-86 tweak
(`main/process/sign_psbt.c:216-247` → `main/wallet.c:938-944` →
`components/libwally-core/upstream/src/script.c:1299-1326`), so a real taptree
output can never auto-validate and will always require explicit user confirmation.

### 4.4 Change/output recognition
Taptree spend outputs stay unrecognised by `psbt_update_outputs()`
(`main/process/sign_psbt.c:540-701`) and are shown to the user for manual
confirmation, exactly as any other non-wallet output. No taptree descriptor
support is attempted (libwally `tr()` only accepts one child, see Appendix).

---

## 5. Tests

All new tests are **JSON-only** and are auto-collected by the existing glob:
`tests/rpc/test_sign_psbt.py:61-64` uses
`@with_test_cases('tests/rpc/data/sign_psbt/psbt_ss_*.json')`
(`with_test_cases` in `tests/__init__.py:78-93`). No Python change is needed.
The singlesig test mnemonic is `tests/__init__.py:24-26`. The harness
(`tests/rpc/test_sign_psbt.py:8-48`) asserts `rslt == expected_output.psbt` (`:37`)
and, when `expected_output.txn` is present, finalizes + extracts the tx and
asserts byte-equality (`:40-48`).

### 5.1 Positive vector — `tests/rpc/data/sign_psbt/psbt_ss_p2tr_taptree_keypath.json`
Mirror `psbt_ss_p2tr_default_all.json` (shape, network `localtest`, singlesig
mnemonic), but the spent output commits to a script tree:
- one p2tr input whose internal key is the Jade singlesig key (e.g.
  `m/86'/1'/0'/0/0`, matching the existing vector's derivation and address);
- a small tree, e.g. a single leaf `<owner_xonly> OP_CHECKSIG` or a CSV script;
- input fields: `witness_utxo` (or non-witness utxo),
  `PSBT_IN_TAP_INTERNAL_KEY`, `PSBT_IN_TAP_BIP32_DERIVATION` (0 leaf hashes,
  fingerprint + path) and `PSBT_IN_TAP_MERKLE_ROOT`;
- **no** `PSBT_IN_TAP_LEAF_SCRIPT`.
`expected_output.psbt` adds the key-path `PSBT_IN_TAP_KEY_SIG`;
`expected_output.txn` is the finalized witness tx, so the schnorr signature
against the taptree-tweaked key is validated end-to-end.

### 5.2 Negative vector — script-path still refused
`tests/rpc/data/sign_psbt/psbt_ss_p2tr_taptree_scripts.json`: same input **plus**
`PSBT_IN_TAP_LEAF_SCRIPT` (0x15). With the guard, the input is skipped and the PSBT
is returned unchanged — express this as `expected_output.psbt` equal to the input
`psbt` and no `expected_output.txn` (same convention as
`psbt_ss_p2wpkh_already_signed.json`, which relies on the returned-unchanged
pattern; note the guard produces **no** error, so do not use `expected_error`).
Optionally also add a `>1`-keypath negative case for completeness.
(If §4.1 leads us to *allow* 0x15, this file becomes a positive vector instead —
keep it in the suite either way so the behaviour is pinned.)

### 5.3 How to generate the vectors
- **Primary generator (byte-compatible):** a small throwaway Python script using
  the repo's `wallycore` bindings and the test singlesig mnemonic:
  derive the internal key, build a one-leaf taptree, compute the leaf hash and
  merkle root, tweak the output key (verify it equals the `scriptPubKey`), then
  construct the PSBT and sign via libwally so the serialization matches what Jade
  will emit. Dump the JSON in the existing format. Template:
  `tests/rpc/data/sign_psbt/psbt_ss_p2tr_default_all.json`.
- **Independent oracle (correctness):** reproduce the output key, merkle root and
  finalized transaction with Bitcoin Core (`bitcoin-cli` descriptor wallet on
  regtest: `importdescriptors` with `tr(<xpub>/*, <leaf>)`, fund, create PSBT,
  `walletprocesspsbt`, `finalizepsbt`) or rust-bitcoin. `expected_output.txn` must
  match the libwally-generated tx exactly; this guards against a libwally bug
  matching a Jade bug. The taproot script-path test vectors in BIP-371 and the
  rust-bitcoin PSBT signer are useful cross-references for the field layout.
- Do **not** hand-craft base64; regenerate deterministically and record the tool
  and derivation path in the `description` field.

### 5.4 Test flow
1. Add the positive vector first and confirm it **fails** on current firmware
   (input silently skipped ⇒ output PSBT lacks `tap_key_sig`) — red.
2. Apply the §3.1 guard change — green.
3. Add the negative vector — green (unchanged).

---

## 6. CI impact

- The new JSON files are picked up automatically by the `psbt_ss_*` glob
  (`tests/rpc/test_sign_psbt.py:61-64`; collection in `tests/__init__.py:78-93`).
  They run in `test_libjade` (`pytest --libjade tests`) and are re-run under
  ASAN/UBSAN and over the serial transport in `test_libjade_sanitize`
  (see `docs/plans/ci-and-test-strategy.md` §1-2). **No new pytest glob and no new
  CI job.**
- `test_format`: any C changes (`main/utils/psbt.c`, and if §3.3 is done
  `main/wallet.c`/`wallet.h`/`process_utils.c`) must pass clang-format-19 via
  `./format.sh && git diff --exit-code`.
- `test_configs`: unaffected (no `configs/*.defaults` edits).
- The change touches shared firmware code, so the standard board/DIY compile gates
  (`build_*`, including the DIY `tdisplays3procamera` build) must stay green; the
  guard is board-agnostic so no board-specific risk is expected.

---

## 7. Task breakdown, estimate, rollback

Ordered, small steps (do not combine the source change with the vector commit
blindly — keep the red→green history visible if convenient):

1. **Vector scaffolding (test-only).** Add
   `tests/rpc/data/sign_psbt/psbt_ss_p2tr_taptree_keypath.json` with a generator
   script; run on current firmware to confirm it fails (input skipped).
2. **Guard relaxation.** Edit `main/utils/psbt.c:13-37` per §3.1 (allow merkle
   root; keep single-keypath + leaf-script rejection; outputs unchanged). Grep for
   any other caller expecting the old behaviour (`main/utils/psbt.c:125` is the
   only validation site).
3. **Negative vector + full run.** Add §5.2, then run
   `pytest -v -s --libjade tests/rpc/test_sign_psbt.py` locally and `./format.sh`.
4. **Resolve §4.1.** Capture a real Liana/BDK keypath PSBT; if it contains 0x15,
   apply the one-line follow-up (allow leaf scripts, but still only sign key path)
   and convert/extend the vector; document the decision in this file.
5. **(Optional / separate, only on client demand) Legacy `sign_tx` plumb.**
   `input_data_t` field + RPC param + `wallet.c` tweak per §3.3, plus
   `tests/rpc/data/sign_tx/` vectors.

Estimate: steps 1-3 ≈ **0.5 day**; step 4 ≈ 0.5 day (depends on getting a real
client PSBT); step 5 ≈ **1-1.5 days** including vectors and protocol review.

Rollback: step 2 is a small, isolated change in `main/utils/psbt.c`. Reverting the
feature commit restores the old guard and removes the now-failing positive vector
(the vectors are inert test data and cause no runtime risk). No persistent state
or storage-format change is involved, so rollback is a plain `git revert`.

---

## Appendix — future work: full script-path support (not planned here)

Enabling actual Taproot **script-path** (miniscript) signing would require, at
minimum:

- **libwally (vendored):**
  - `components/libwally-core/upstream/src/psbt.c:4618` — real script-path sighash
    (`wally_tx_get_btc_taproot_signature_hash` with the tapleaf script/version,
    codeseparator, annex) instead of forcing `script = NULL`.
  - `components/libwally-core/upstream/src/psbt.c:4986` — sign
    `input->taproot_leaf_scripts` / emit `PSBT_IN_TAP_SCRIPT_SIG`
    (`wally_psbt_sign_input_bip32` today only does the key path).
  - `components/libwally-core/upstream/src/descriptor.c:788-798` `verify_tr()`
    rejects `child_count != 1` and `:1554-1586` `generate_tr()` passes a `NULL`
    merkle root (`/* FIXME: Support script path */`): both must understand a
    miniscript tree.
- **Jade layers:** descriptor parsing/verification (`main/descriptor.c`,
  `main/process/sign_psbt.c:319-496`), change-output recognition for `tr()`
  descriptors (`main/process/sign_psbt.c:581-701`), sighash policy
  (`main/process/sign_utils.c:659-662`, currently only ALL/DEFAULT), and an
  on-device display of the spending conditions before signing.

None of the above is needed for the key-path goal in this plan.
