#!/usr/bin/env python3
"""Deterministic generator for the taproot key-path + taptree test vectors.

Generates:
  psbt_ss_p2tr_taptree_keypath.json   positive: single keypath + merkle root
  psbt_ss_p2tr_taptree_scripts.json   positive: keypath + merkle root + a tapleaf script (0x15)
  psbt_ss_p2tr_taptree_multikey.json  negative: more than one taproot keypath

The positive PSBTs are derived from psbt_ss_p2tr_default_all.json (same singlesig
mnemonic and m/86'/1'/0'/0/0 internal key) by replacing the spent output with a
single-leaf taptree tweak and adding PSBT_IN_TAP_MERKLE_ROOT (0x18). A tapleaf
script (0x15) may also be present (wallets such as Liana include the tree); the
spend is still the key path, so it is signed. The multikey negative is rebuilt as
a single-input PSBT (only the guard-triggering input) so no signable input remains
and sign_psbt returns it unchanged.

Run with the repo wallycore bindings (pip wallycore==1.5.3):
  python3 generate_taptree_keypath_vectors.py
"""

import base64
import hashlib
import json
import os

import wallycore as w

HERE = os.path.dirname(os.path.abspath(__file__))
TEMPLATE = os.path.join(HERE, 'psbt_ss_p2tr_default_all.json')
MNEMONIC = 'paddle puppy easily actor poet apart screen drastic city front predict damp'
INTERNAL_PATH = "m/86'/1'/0'/0/0"
LEAF_PATH = "m/86'/1'/0'/0/1"
TAPLEAF_VERSION = 0xc0


def varint(n):
    if n < 0xfd:
        return bytes([n])
    if n <= 0xffff:
        return b'\xfd' + n.to_bytes(2, 'little')
    if n <= 0xffffffff:
        return b'\xfe' + n.to_bytes(4, 'little')
    return b'\xff' + n.to_bytes(8, 'little')


def read_varint(b, off):
    x = b[off]
    off += 1
    if x < 0xfd:
        return x, off
    if x == 0xfd:
        return int.from_bytes(b[off:off + 2], 'little'), off + 2
    if x == 0xfe:
        return int.from_bytes(b[off:off + 4], 'little'), off + 4
    return int.from_bytes(b[off:off + 8], 'little'), off + 8


def parse_maps(b, off, num_maps):
    maps = []
    for _ in range(num_maps):
        kvs = []
        while True:
            klen, off = read_varint(b, off)
            if klen == 0:
                break
            key = b[off:off + klen]
            off += klen
            vlen, off = read_varint(b, off)
            val = b[off:off + vlen]
            off += vlen
            kvs.append((key, val))
        maps.append(kvs)
    return maps, off


def serialize_maps(maps):
    out = b''
    for kvs in maps:
        for key, val in kvs:
            out += varint(len(key)) + key + varint(len(val)) + val
        out += b'\x00'
    return out


def tagged_hash(tag, data):
    tag_hash = hashlib.sha256(tag.encode()).digest()
    return hashlib.sha256(tag_hash + tag_hash + data).digest()


def master_key():
    seed = bytearray(64)
    w.bip39_mnemonic_to_seed(MNEMONIC, '', seed)
    return w.bip32_key_from_seed_alloc(bytes(seed), w.BIP32_VER_TEST_PRIVATE, 0)


def xonly_at(master, path):
    key = w.bip32_key_from_parent_path_str_alloc(master, path, 0, w.BIP32_FLAG_KEY_PRIVATE)
    return w.bip32_key_get_pub_key(key)[1:]


def template_parts():
    """Parse the template, returning input 0's map plus prevout/output details."""
    with open(TEMPLATE, 'r') as f:
        template = json.load(f)
    raw = base64.b64decode(template['input']['psbt'][4:])
    assert raw[:5] == b'psbt\xff'

    global_maps, off = parse_maps(raw, 5, 1)
    tx_bytes = global_maps[0][0][1]

    toff = 4
    num_inputs, toff = read_varint(tx_bytes, toff)
    prev_txid = tx_bytes[toff:toff + 32]
    prev_vout = tx_bytes[toff + 32:toff + 36]
    toff += 36
    slen, toff = read_varint(tx_bytes, toff)
    prev_scriptsig = tx_bytes[toff:toff + slen]
    toff += slen
    prev_sequence = tx_bytes[toff:toff + 4]
    toff += 4
    for _ in range(num_inputs - 1):
        toff += 36
        slen, toff = read_varint(tx_bytes, toff)
        toff += slen + 4
    num_outputs, toff = read_varint(tx_bytes, toff)
    dest_script = None
    for index in range(num_outputs):
        toff += 8
        slen, toff = read_varint(tx_bytes, toff)
        script = tx_bytes[toff:toff + slen]
        toff += slen
        if index == 1:
            dest_script = script

    input_maps, off = parse_maps(raw, off, num_inputs)
    return input_maps[0], prev_txid, prev_vout, prev_scriptsig, prev_sequence, dest_script


def _swap_utxo_script(key, val, scriptpubkey):
    if key == b'\x01':  # PSBT_IN_WITNESS_UTXO: swap the 34-byte p2tr script
        assert val[-34:-32] == b'\x51\x20'
        val = val[:-34] + scriptpubkey
    return val


def build_input_psbt(scriptpubkey, extra_input_entries=()):
    """Load the template PSBT, swap input 0's spent script and add extra fields."""
    with open(TEMPLATE, 'r') as f:
        template = json.load(f)
    raw = base64.b64decode(template['input']['psbt'][4:])
    assert raw[:5] == b'psbt\xff'

    off = 5
    global_maps, off = parse_maps(raw, off, 1)
    tx_bytes = global_maps[0][0][1]

    toff = 4
    num_inputs, toff = read_varint(tx_bytes, toff)
    for _ in range(num_inputs):
        toff += 36
        slen, toff = read_varint(tx_bytes, toff)
        toff += slen + 4
    num_outputs, toff = read_varint(tx_bytes, toff)

    input_maps, off = parse_maps(raw, off, num_inputs)
    output_maps, off = parse_maps(raw, off, num_outputs)
    assert off == len(raw)

    new_input = [(k, _swap_utxo_script(k, v, scriptpubkey)) for k, v in input_maps[0]]
    new_input += list(extra_input_entries)
    input_maps[0] = new_input

    return b'psbt\xff' + serialize_maps(global_maps) + serialize_maps(input_maps) + serialize_maps(output_maps)


def build_single_input_psbt(scriptpubkey, dest_script, output_amount, extra_input_entries=()):
    """Build a fresh 1-input/1-output PSBT spending only the guard-triggering input.

    With no second (signable) input present, a guard rejection leaves nothing to
    sign, so sign_psbt returns the PSBT unchanged (plan section 5.2)."""
    input0, prev_txid, prev_vout, prev_scriptsig, prev_sequence, _ = template_parts()
    tx = b'\x02\x00\x00\x00' + varint(1)
    tx += prev_txid + prev_vout + varint(len(prev_scriptsig)) + prev_scriptsig + prev_sequence
    tx += varint(1) + output_amount.to_bytes(8, 'little') + varint(len(dest_script)) + dest_script
    tx += b'\x00\x00\x00\x00'

    new_input = [(k, _swap_utxo_script(k, v, scriptpubkey)) for k, v in input0]
    new_input += list(extra_input_entries)
    return (b'psbt\xff' + serialize_maps([[(b'\x00', tx)]]) + serialize_maps([new_input]) + serialize_maps([[]]))


def canonicalize(input_bytes):
    """Parse/re-serialize so the vector matches libwally's canonical field order."""
    return w.psbt_to_bytes(w.psbt_from_bytes(input_bytes, w.WALLY_PSBT_PARSE_FLAG_STRICT), 0)


def sign_and_extract(input_bytes, master):
    psbt = w.psbt_from_bytes(input_bytes, w.WALLY_PSBT_PARSE_FLAG_STRICT)
    assert w.psbt_to_bytes(psbt, 0) == input_bytes, 'PSBT does not round-trip'
    w.psbt_sign_bip32(psbt, master, 0)
    signed = w.psbt_to_bytes(psbt, 0)
    w.psbt_finalize(psbt, 0)
    tx = w.psbt_extract(psbt, w.WALLY_PSBT_EXTRACT_OPT_FINAL)
    tx_bytes = w.tx_to_bytes(tx, w.WALLY_TX_FLAG_USE_WITNESS)
    return signed, tx_bytes


def write_vector(filename, description, input_bytes, expected):
    case = {
        'description': description,
        'input': {'network': 'localtest', 'psbt': 'b64:' + base64.b64encode(input_bytes).decode()},
        'expected_output': {'psbt': 'b64:' + base64.b64encode(expected['psbt']).decode()},
    }
    if 'txn' in expected:
        case['expected_output']['txn'] = '0x' + expected['txn'].hex()
    path = os.path.join(HERE, filename)
    with open(path, 'w') as f:
        json.dump(case, f, indent=2)
        f.write('\n')
    print('wrote', path)


def main():
    master = master_key()
    internal = xonly_at(master, INTERNAL_PATH)
    owner = xonly_at(master, LEAF_PATH)

    leaf_script = b'\x20' + owner + b'\xac'  # <owner_xonly> OP_CHECKSIG
    leaf_hash = tagged_hash('TapLeaf', bytes([TAPLEAF_VERSION]) + varint(len(leaf_script)) + leaf_script)
    merkle_root = leaf_hash  # single-leaf tree: root == leaf hash
    output_key = w.ec_public_key_bip341_tweak(internal, merkle_root, 0)
    scriptpubkey = b'\x51\x20' + output_key[1:]

    assert internal.hex() == 'f658082d7f5ec466d61220aed1429391d7bedf8f03428c9d7e4062d80e37345a'
    assert w.ec_public_key_bip341_tweak(internal, None, 0)[1:].hex() \
        == 'b71aa79cab0ae2d83b82d44cbdc23f5dcca3797e8ba622c4e45a8f7dce28ba0e'

    gen = 'generate_taptree_keypath_vectors.py (wallycore==1.5.3)'

    # Positive: exactly one keypath (0x16) + merkle root (0x18), no leaf script.
    input_bytes = build_input_psbt(scriptpubkey, extra_input_entries=[(b'\x18', merkle_root)])
    signed, tx_bytes = sign_and_extract(input_bytes, master)
    # Independently verify the key-path signature against the taptree-tweaked key.
    psbt = w.psbt_from_bytes(signed, w.WALLY_PSBT_PARSE_FLAG_STRICT)
    sig = w.psbt_get_input_taproot_signature(psbt, 0)
    tx = w.psbt_get_global_tx_alloc(psbt)
    sighash = w.psbt_get_input_signature_hash(psbt, 0, tx, b'', 0)
    assert w.ec_sig_verify(output_key, bytes(sighash), w.EC_FLAG_SCHNORR, bytes(sig)) is None
    write_vector(
        'psbt_ss_p2tr_taptree_keypath.json',
        'Taproot keypath spend of a single-leaf taptree output. Internal key '
        f'{INTERNAL_PATH}, leaf <{LEAF_PATH}> OP_CHECKSIG, merkle root {merkle_root.hex()}, '
        f'output key {output_key[1:].hex()}. Generated by {gen}.',
        input_bytes, {'psbt': signed, 'txn': tx_bytes})

    # The leaf-script vector and the multikey negative are single-input PSBTs (only
    # the guard-triggering taproot input); the multikey case leaves no signable input,
    # so sign_psbt returns it unchanged (plan 5.2).
    input0, _, _, _, _, dest_script = template_parts()
    deriv_val = next(v for k, v in input0 if k[:1] == b'\x16')
    output_amount = 9000  # input 0 is 10000 sat, so the fee sanity check passes

    # Positive: keypath + merkle root + a tapleaf script (0x15). The leaf script is
    # present (as Liana-style wallets include the tree) but the spend is still the
    # key path, so libwally signs it with PSBT_IN_TAP_KEY_SIG.
    control_block = bytes([TAPLEAF_VERSION]) + internal
    pos_scripts = canonicalize(build_single_input_psbt(
        scriptpubkey, dest_script, output_amount,
        extra_input_entries=[(b'\x18', merkle_root), (b'\x15' + control_block, leaf_script)]))
    signed_scripts, tx_scripts = sign_and_extract(pos_scripts, master)
    write_vector(
        'psbt_ss_p2tr_taptree_scripts.json',
        'Single-input taproot PSBT with a merkle root AND a PSBT_IN_TAP_LEAF_SCRIPT (0x15). '
        f'The spend is still the key path, so it is signed with PSBT_IN_TAP_KEY_SIG. {gen}.',
        pos_scripts, {'psbt': signed_scripts, 'txn': tx_scripts})

    # Negative: more than one taproot keypath (0x16): input is skipped.
    # Reuse the internal key's derivation data for a second (owner) xonly key.
    neg_multikey = canonicalize(build_single_input_psbt(
        scriptpubkey, dest_script, output_amount,
        extra_input_entries=[(b'\x18', merkle_root), (b'\x16' + owner, deriv_val)]))
    write_vector(
        'psbt_ss_p2tr_taptree_multikey.json',
        'Single-input taproot PSBT with a merkle root and two taproot keypaths (0x16): '
        f'ambiguous/multisig keypath shape, so the input is silently skipped and the PSBT is '
        f'returned unchanged. {gen}.',
        neg_multikey, {'psbt': neg_multikey})


if __name__ == '__main__':
    main()
