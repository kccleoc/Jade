#include "descriptor.h"
#include "selfcheck.h"

#define INIT_DESC(d, s)                                                                                                \
    do {                                                                                                               \
        d.script_len = strlen(s);                                                                                      \
        JADE_ASSERT(d.script_len < sizeof(d.script));                                                                  \
        strcpy(d.script, s);                                                                                           \
        d.type = DESCRIPTOR_TYPE_UNKNOWN;                                                                              \
        d.num_values = 0;                                                                                              \
    } while (false)

#define ADD_MAP_VAL(d, k, v)                                                                                           \
    do {                                                                                                               \
        string_value_t* const sv = &(d.values[d.num_values]);                                                          \
        sv->key_len = strlen(k);                                                                                       \
        JADE_ASSERT(sv->key_len < sizeof(sv->key));                                                                    \
        strcpy(sv->key, k);                                                                                            \
        sv->value_len = strlen(v);                                                                                     \
        JADE_ASSERT(sv->value_len < sizeof(sv->value));                                                                \
        strcpy(sv->value, v);                                                                                          \
        ++d.num_values;                                                                                                \
    } while (false)

#define FP_XPUB_MATCH(n, fp, xp)                                                                                       \
    (wally_hex_to_bytes(fp, buf, sizeof(buf), &written) == WALLY_OK && written == sizeof(signers[n].fingerprint)       \
        && !memcmp(buf, signers[n].fingerprint, written) && !strcmp(xp, signers[n].xpub))

static bool check_descriptor_serialisation(descriptor_data_t* const desc)
{
    JADE_ASSERT(desc);

    // Should be same if serialised and de-serialised
    uint8_t serialised[768];
    const size_t serialised_len = DESCRIPTOR_BYTES_LEN(desc);
    JADE_ASSERT(serialised_len < sizeof(serialised));
    if (!descriptor_to_bytes(desc, serialised, serialised_len)) {
        FAIL();
    }

    wally_bzero(desc, sizeof(descriptor_data_t));

    if (!descriptor_from_bytes(serialised, serialised_len, desc)) {
        FAIL();
    }

    // Re-serialise - should be same as first serialisation
    if (DESCRIPTOR_BYTES_LEN(desc) != serialised_len) {
        FAIL();
    }

    uint8_t serialised2[sizeof(serialised)];
    if (!descriptor_to_bytes(desc, serialised2, serialised_len)) {
        FAIL();
    }
    if (memcmp(serialised, serialised2, serialised_len)) {
        FAIL();
    }

    return true;
}

static bool test_miniscript_descriptors(void)
{
    const char* errmsg = NULL;
    descriptor_data_t desc = {};
    bool ret;

    const uint32_t MAINNET = NETWORK_BITCOIN;
    const uint32_t TESTNET = NETWORK_BITCOIN_TESTNET;
    const uint32_t LTESTNET = NETWORK_LIQUID_TESTNET;
    uint8_t buf[BIP32_KEY_FINGERPRINT_LEN];
    size_t num_signers = 0;
    signer_t signers[3];
    size_t written;

    // Anchor Watch example
    INIT_DESC(desc, "wsh(or_d(pk(@0/<0;1>/*),and_v(v:multi(2,@1/<0;1>/*,@2/<0;1>/*),older(4320))))");
    ADD_MAP_VAL(desc, "@0",
        "[1bf12fe0/48'/1'/0'/2']"
        "tpubDEHXLZfMAAM5duEnX6SSnZjGYbrxqXvRJmMxw8MFwr3gu4LC4DSxR9KVEfVDVcZxre4XL5tGcwVRrHwQ9euTMnSq6P"
        "6BqREemaqrFsC96Fy");
    ADD_MAP_VAL(desc, "@1",
        "[eda3d606/48'/1'/0'/2']"
        "tpubDEAmqvQkhqP6SbfbSPu3AeRR9kfHLFXYvNDiWashLy7V2zicg1YLg654AqfomsC6kFwTs4MpcnqwxN2AnYAqi5JZeu"
        "VDBn3rfZZLTaAuS8Y");
    ADD_MAP_VAL(desc, "@2",
        "[e1640396/48'/1'/0'/2']"
        "tpubDFgDvZifofePphQiVjLfkov8YTDg3UPuHRvt6LzbySYMZQhN19p6zvR7NTEXi1ZJAMNostHMTnz2sfXXYcJFQqtyCn"
        "NuUfgYqsahxTLGJq2");

    // Check parsing and key iteration
    ret = descriptor_get_signers("A0", &desc, TESTNET, NULL, NULL, 0, &num_signers, NULL, &errmsg);
    if (!ret) {
        FAIL();
    }
    if (num_signers != 3) {
        FAIL();
    }
    ret = descriptor_get_signers("A0", &desc, TESTNET, &desc.type, signers, 3, &num_signers, NULL, &errmsg);
    if (!ret) {
        FAIL();
    }
    if (desc.type != DESCRIPTOR_TYPE_MIXED) {
        FAIL();
    }
    if (num_signers != 3) {
        FAIL();
    }
    for (size_t i = 0; i < 3; ++i) {
        if (signers[i].derivation_len != 4 || signers[i].derivation[0] != harden(48)
            || signers[i].derivation[1] != harden(1) || signers[i].derivation[2] != harden(0)
            || signers[i].derivation[3] != harden(2)) {
            FAIL();
        }
        if (!signers[i].path_is_string || strcmp(signers[i].path_str, "<0;1>/*")) {
            FAIL();
        }
    }

    if (!FP_XPUB_MATCH(0, "1bf12fe0",
            "tpubDEHXLZfMAAM5duEnX6SSnZjGYbrxqXvRJmMxw8MFwr3gu4LC4DSxR9KVEfVDVcZxre4XL5tGcwVRrHwQ9euTMnSq6P6BqREemaqrFs"
            "C96Fy")) {
        FAIL();
    }
    if (!FP_XPUB_MATCH(1, "eda3d606",
            "tpubDEAmqvQkhqP6SbfbSPu3AeRR9kfHLFXYvNDiWashLy7V2zicg1YLg654AqfomsC6kFwTs4MpcnqwxN2AnYAqi5JZeuVDBn3rfZZLTa"
            "AuS8Y")) {
        FAIL();
    }
    if (!FP_XPUB_MATCH(2, "e1640396",
            "tpubDFgDvZifofePphQiVjLfkov8YTDg3UPuHRvt6LzbySYMZQhN19p6zvR7NTEXi1ZJAMNostHMTnz2sfXXYcJFQqtyCnNuUfgYqsahxT"
            "LGJq2")) {
        FAIL();
    }

    // Check scripts/addresses
    uint32_t multi_index = 0;
    const char* expectedA[2] = { "tb1qcf6egdkhq96vwkn4ge6fyz446zn09alwhuadcz8ezf6remuw7r7stzu9gj",
        "tb1qep0hehn3gl5nse6w5vyqe0g4q9czvhgr3nlzj76uhmfsxvqcz8pq2zyy4c" };

    for (uint32_t child_num = 0; child_num < 2; ++child_num) {
        char* addr = NULL;
        ret = descriptor_to_address("A1", &desc, TESTNET, multi_index, child_num, NULL, &addr, &errmsg);
        if (!ret || strcmp(addr, expectedA[child_num])) {
            wally_free_string(addr);
            FAIL();
        }
        wally_free_string(addr);

        // Pass unknown type - should give same answer
        desc.type = DESCRIPTOR_TYPE_UNKNOWN;
        ret = descriptor_to_address("A2", &desc, TESTNET, multi_index, child_num, NULL, &addr, &errmsg);
        if (!ret || strcmp(addr, expectedA[child_num])) {
            wally_free_string(addr);
            FAIL();
        }
        wally_free_string(addr);

        // Wrong network should fail
        ret = descriptor_to_address("A3", &desc, MAINNET, multi_index, child_num, NULL, &addr, &errmsg);
        if (ret) {
            FAIL();
        }

        // Wrong descriptor type should fail
        desc.type = DESCRIPTOR_TYPE_MINISCRIPT_ONLY;
        ret = descriptor_to_address("A4", &desc, TESTNET, multi_index, child_num, NULL, &addr, &errmsg);
        if (ret) {
            FAIL();
        }
        desc.type = DESCRIPTOR_TYPE_MIXED;
    }

    // Check serialisation
    if (!check_descriptor_serialisation(&desc)) {
        FAIL();
    }

    // Liana example
    INIT_DESC(desc, "wsh(or_d(multi(2,@0/<0;1>/*,@1/<0;1>/*),and_v(v:pkh(@2/<0;1>/*),older(100))))");
    ADD_MAP_VAL(desc, "@0",
        "[7897b5b3/48'/1'/0'/2']"
        "tpubDE8B47dY4JuGLnXVyDzG76UuhBM5hTjc6sXeJjG6ThbPsryiAnKqQY8CmxWcYjM6eVvkyH7CNTVrmPMxSWP9ZzCfHV"
        "Ho6preHp6Xhgd42JH");
    ADD_MAP_VAL(desc, "@1",
        "[1bf12fe0/48'/1'/0'/2']"
        "tpubDEHXLZfMAAM5duEnX6SSnZjGYbrxqXvRJmMxw8MFwr3gu4LC4DSxR9KVEfVDVcZxre4XL5tGcwVRrHwQ9euTMnSq6P"
        "6BqREemaqrFsC96Fy");
    ADD_MAP_VAL(desc, "@2",
        "[7897b5b3/48'/1'/1'/2']"
        "tpubDFf2ES1oUSZRgiCFT4mvBQ4jC2xTfRzVwfa6KewXZthgtL83UquqirWXzo1EKi4et3bx2wQz9QFKLDeu6vXoKpgQnJ"
        "HyV8DomjCjJRT3d57");

    // Check parsing and key iteration
    ret = descriptor_get_signers("B0", &desc, TESTNET, NULL, NULL, 0, &num_signers, NULL, &errmsg);
    if (!ret) {
        FAIL();
    }
    if (num_signers != 3) {
        FAIL();
    }
    ret = descriptor_get_signers("B0", &desc, TESTNET, &desc.type, signers, 3, &num_signers, NULL, &errmsg);
    if (!ret) {
        FAIL();
    }
    if (desc.type != DESCRIPTOR_TYPE_MIXED) {
        FAIL();
    }
    if (num_signers != 3) {
        FAIL();
    }
    for (size_t i = 0; i < 3; ++i) {
        if (signers[i].derivation_len != 4 || signers[i].derivation[0] != harden(48)
            || signers[i].derivation[1] != harden(1) || signers[i].derivation[2] != harden(i == 2 ? 1 : 0)
            || signers[i].derivation[3] != harden(2)) {
            FAIL();
        }
        if (!signers[i].path_is_string || strcmp(signers[i].path_str, "<0;1>/*")) {
            FAIL();
        }
    }

    if (!FP_XPUB_MATCH(0, "7897b5b3",
            "tpubDE8B47dY4JuGLnXVyDzG76UuhBM5hTjc6sXeJjG6ThbPsryiAnKqQY8CmxWcYjM6eVvkyH7CNTVrmPMxSWP9ZzCfHV"
            "Ho6preHp6Xhgd42JH")) {
        FAIL();
    }
    if (!FP_XPUB_MATCH(1, "1bf12fe0",
            "tpubDEHXLZfMAAM5duEnX6SSnZjGYbrxqXvRJmMxw8MFwr3gu4LC4DSxR9KVEfVDVcZxre4XL5tGcwVRrHwQ9euTMnSq6P"
            "6BqREemaqrFsC96Fy")) {
        FAIL();
    }
    if (!FP_XPUB_MATCH(2, "7897b5b3",
            "tpubDFf2ES1oUSZRgiCFT4mvBQ4jC2xTfRzVwfa6KewXZthgtL83UquqirWXzo1EKi4et3bx2wQz9QFKLDeu6vXoKpgQnJ"
            "HyV8DomjCjJRT3d57")) {
        FAIL();
    }

    // Check scripts/addresses
    const char* expectedB[2][2] = { { "tb1q0ddn2fn5y66gt2r69dv6el32lw44lupa2ry9enlm8zduxhpwk6aqen88zh",
                                        "tb1qfj66kfjk98cfcays67c9rvkzals7tnxz2dkxnwrjslwk5tzcd8ysgx9ahf" },
        { "tb1qu6j64q9kezc0dxgfl67fgnm2z9yycc55c0fa09uresf65w6py04s4qwgul",
            "tb1qkmr7qpxagfn7mafmsrt6e3qzzc599w28cl037cktjjegenfnhyysllxj5p" } };

    for (multi_index = 0; multi_index < 2; ++multi_index) {
        for (uint32_t child_num = 0; child_num < 2; ++child_num) {
            // Use unknown type
            char* addr = NULL;
            ret = descriptor_to_address("B1", &desc, TESTNET, multi_index, child_num, NULL, &addr, &errmsg);
            if (!ret || strcmp(addr, expectedB[multi_index][child_num])) {
                wally_free_string(addr);
                FAIL();
            }
            wally_free_string(addr);
        }
    }

    // Check serialisation
    if (!check_descriptor_serialisation(&desc)) {
        FAIL();
    }

    // Addresses should be same
    for (multi_index = 0; multi_index < 2; ++multi_index) {
        for (uint32_t child_num = 0; child_num < 2; ++child_num) {
            // Use unknown type
            char* addr = NULL;
            ret = descriptor_to_address("B2", &desc, TESTNET, multi_index, child_num, NULL, &addr, &errmsg);
            if (!ret || strcmp(addr, expectedB[multi_index][child_num])) {
                wally_free_string(addr);
                FAIL();
            }
            wally_free_string(addr);
        }
    }

    // Another Liana example - NOTE: reusing placeholder @0
    INIT_DESC(desc, "wsh(or_d(multi(2,@0/<0;1>/*,@1/<0;1>/*),and_v(v:pkh(@0/<2;3>/*),older(65535))))");
    ADD_MAP_VAL(desc, "@0",
        "[fb5d3ada/48'/1'/0'/2']"
        "tpubDFa4d4JXKYKrsyxkaxxk6QQscMo1bmwkczNWGKrwkPZiXSbwHueBEsS8Hq4RNTz2cm37MseAhzDRgyrmuaSDTtT6zi"
        "rPxsi8FTVUBY6FLiQ");
    ADD_MAP_VAL(desc, "@1",
        "[077ace32/48'/1'/0'/2']"
        "tpubDDzogRd3Gt71WEQavJggpR6R38iru9kC2uMkMcftBHLF8RzzPNSeZeTUoqvoa9xfXr2qeihmpysKbzwj6NmLbFQ9v2"
        "VHdMw7p8MnQycgAV8");

    // Check parsing and key iteration
    ret = descriptor_get_signers("b0", &desc, TESTNET, NULL, NULL, 0, &num_signers, NULL, &errmsg);
    if (!ret) {
        FAIL();
    }
    if (num_signers != 3) {
        FAIL();
    }
    ret = descriptor_get_signers("b0", &desc, TESTNET, &desc.type, signers, 3, &num_signers, NULL, &errmsg);
    if (!ret) {
        FAIL();
    }
    if (desc.type != DESCRIPTOR_TYPE_MIXED) {
        FAIL();
    }
    if (num_signers != 3) {
        FAIL();
    }
    for (size_t i = 0; i < 2; ++i) {
        if (signers[i].derivation_len != 4 || signers[i].derivation[0] != harden(48)
            || signers[i].derivation[1] != harden(1) || signers[i].derivation[2] != harden(0)
            || signers[i].derivation[3] != harden(2)) {
            FAIL();
        }
        if (!signers[i].path_is_string || strcmp(signers[i].path_str, i == 2 ? "<2;3>/*" : "<0;1>/*")) {
            FAIL();
        }
    }

    if (!FP_XPUB_MATCH(0, "fb5d3ada",
            "tpubDFa4d4JXKYKrsyxkaxxk6QQscMo1bmwkczNWGKrwkPZiXSbwHueBEsS8Hq4RNTz2cm37MseAhzDRgyrmuaSDTtT6zi"
            "rPxsi8FTVUBY6FLiQ")) {
        FAIL();
    }
    if (!FP_XPUB_MATCH(1, "077ace32",
            "tpubDDzogRd3Gt71WEQavJggpR6R38iru9kC2uMkMcftBHLF8RzzPNSeZeTUoqvoa9xfXr2qeihmpysKbzwj6NmLbFQ9v2"
            "VHdMw7p8MnQycgAV8")) {
        FAIL();
    }

    // Check scripts/addresses
    const char* expectedb[2][2] = { { "tb1q8uu3hj86chgu2zgpn4x32crv7cxl8skjdexue2ku2mjgwjvm7t3q4x2yxa",
                                        "tb1qf84tqnrcp36vtghf530406wyct00ns4jma3hw3ztfv5h98cka52qajm5v8" },
        { "tb1qx8zx3x9wf33uaar3ghvzy8y7l5wzh7e8vs5gthwvxr5z3x7ekuvsn8689q",
            "tb1q3a3k2m7qt3v05gar4kynpuu6xspqucet529mavjru4ca90ppayqqj0ad9f" } };

    for (multi_index = 0; multi_index < 2; ++multi_index) {
        for (uint32_t child_num = 0; child_num < 2; ++child_num) {
            // Use unknown type
            char* addr = NULL;
            ret = descriptor_to_address("b1", &desc, TESTNET, multi_index, child_num, NULL, &addr, &errmsg);
            if (!ret || strcmp(addr, expectedb[multi_index][child_num])) {
                wally_free_string(addr);
                FAIL();
            }
            wally_free_string(addr);
        }
    }

    // Check serialisation
    if (!check_descriptor_serialisation(&desc)) {
        FAIL();
    }

    // Addresses should be same
    for (multi_index = 0; multi_index < 2; ++multi_index) {
        for (uint32_t child_num = 0; child_num < 2; ++child_num) {
            // Use unknown type
            char* addr = NULL;
            ret = descriptor_to_address("b2", &desc, TESTNET, multi_index, child_num, NULL, &addr, &errmsg);
            if (!ret || strcmp(addr, expectedb[multi_index][child_num])) {
                wally_free_string(addr);
                FAIL();
            }
            wally_free_string(addr);
        }
    }

    // Another miniscript example... NOTE: not a 'wallet policy'
    INIT_DESC(desc,
        "sh(wsh(or_d(thresh(1,pk("
        "[7897b5b3/48'/1'/0'/2']"
        "tpubDE8B47dY4JuGLnXVyDzG76UuhBM5hTjc6sXeJjG6ThbPsryiAnKqQY8CmxWcYjM6eVvkyH7CNTVrmPMxSWP9ZzCfHV"
        "Ho6preHp6Xhgd42JH/0/*))"
        ",and_v(v:thresh(1,pk("
        "[1bf12fe0/48'/1'/0'/2']"
        "tpubDEHXLZfMAAM5duEnX6SSnZjGYbrxqXvRJmMxw8MFwr3gu4LC4DSxR9KVEfVDVcZxre4XL5tGcwVRrHwQ9euTMnSq6P"
        "6BqREemaqrFsC96Fy/0/*))"
        ",older(30)))))");

    // Check parsing and key iteration
    ret = descriptor_get_signers("C0", &desc, TESTNET, NULL, NULL, 0, &num_signers, NULL, &errmsg);
    if (!ret) {
        FAIL();
    }
    if (num_signers != 2) {
        FAIL();
    }
    ret = descriptor_get_signers("C0", &desc, TESTNET, &desc.type, signers, 3, &num_signers, NULL, &errmsg);
    if (!ret) {
        FAIL();
    }
    if (desc.type != DESCRIPTOR_TYPE_MIXED) {
        FAIL();
    }
    if (num_signers != 2) {
        FAIL();
    }
    for (size_t i = 0; i < 2; ++i) {
        if (signers[i].derivation_len != 4 || signers[i].derivation[0] != harden(48)
            || signers[i].derivation[1] != harden(1) || signers[i].derivation[2] != harden(0)
            || signers[i].derivation[3] != harden(2)) {
            FAIL();
        }
        if (!signers[i].path_is_string || strcmp(signers[i].path_str, "0/*")) {
            FAIL();
        }
    }

    if (!FP_XPUB_MATCH(0, "7897b5b3",
            "tpubDE8B47dY4JuGLnXVyDzG76UuhBM5hTjc6sXeJjG6ThbPsryiAnKqQY8CmxWcYjM6eVvkyH7CNTVrmPMxSWP9ZzCfHV"
            "Ho6preHp6Xhgd42JH")) {
        FAIL();
    }
    if (!FP_XPUB_MATCH(1, "1bf12fe0",
            "tpubDEHXLZfMAAM5duEnX6SSnZjGYbrxqXvRJmMxw8MFwr3gu4LC4DSxR9KVEfVDVcZxre4XL5tGcwVRrHwQ9euTMnSq6P"
            "6BqREemaqrFsC96Fy")) {
        FAIL();
    }

    // Check scripts/addresses
    const char* expectedC[2] = { "2MzcimvUcAwWKDDufQmTwq2FU4qenKL51fL", "2Mw3VJsaTKNCuZ2Drw3UMBvQg7QyHEvt8Nz" };

    multi_index = 0;
    for (uint32_t child_num = 0; child_num < 2; ++child_num) {
        // Use unknown type
        char* addr = NULL;
        ret = descriptor_to_address("C1", &desc, TESTNET, multi_index, child_num, NULL, &addr, &errmsg);
        if (!ret || strcmp(addr, expectedC[child_num])) {
            wally_free_string(addr);
            FAIL();
        }
        wally_free_string(addr);
    }

    // Check serialisation
    if (!check_descriptor_serialisation(&desc)) {
        FAIL();
    }

    // Addresses should be same
    multi_index = 0;
    for (uint32_t child_num = 0; child_num < 2; ++child_num) {
        // Use unknown type
        char* addr = NULL;
        const bool ret = descriptor_to_address("C2", &desc, TESTNET, multi_index, child_num, NULL, &addr, &errmsg);
        if (!ret || strcmp(addr, expectedC[child_num])) {
            wally_free_string(addr);
            FAIL();
        }
        wally_free_string(addr);
    }

    // Test Ledger issue - NOTE: not a 'wallet policy' and 'miniscript-only'
    INIT_DESC(desc,
        "and_b(pk([7897b5b3/48'/1'/0'/2']"
        "tpubDE8B47dY4JuGLnXVyDzG76UuhBM5hTjc6sXeJjG6ThbPsryiAnKqQY8CmxWcYjM6eVvkyH7CNTVrmPMxSWP9ZzCfHVHo6"
        "preHp6Xhgd42JH/0/*),a:1)");

    // Check parsing and key iteration
    ret = descriptor_get_signers("L0", &desc, TESTNET, NULL, NULL, 0, &num_signers, NULL, &errmsg);
    if (!ret) {
        FAIL();
    }
    if (num_signers != 1) {
        FAIL();
    }
    ret = descriptor_get_signers("L0", &desc, TESTNET, &desc.type, signers, 3, &num_signers, NULL, &errmsg);
    if (!ret) {
        FAIL();
    }
    if (desc.type != DESCRIPTOR_TYPE_MIXED) {
        FAIL();
    }
    if (num_signers != 1) {
        FAIL();
    }
    if (signers[0].derivation_len != 4 || signers[0].derivation[0] != harden(48)
        || signers[0].derivation[1] != harden(1) || signers[0].derivation[2] != harden(0)
        || signers[0].derivation[3] != harden(2)) {
        FAIL();
    }
    if (!signers[0].path_is_string || strcmp(signers[0].path_str, "0/*")) {
        FAIL();
    }

    if (!FP_XPUB_MATCH(0, "7897b5b3",
            "tpubDE8B47dY4JuGLnXVyDzG76UuhBM5hTjc6sXeJjG6ThbPsryiAnKqQY8CmxWcYjM6eVvkyH7CNTVrmPMxSWP9ZzCfHV"
            "Ho6preHp6Xhgd42JH")) {
        FAIL();
    }

    // The expected/correct script: <pubkey> OP_CHECKSIG OP_TOALTSTACK 1 OP_FROMALTSTACK OP_BOOLAND
    const char* expectedL[2] = { "2102afd5b7ce022720e0bbef9a34d2ed81196b700e5639229087c817119dd421483cac6b516c9a",
        "210272269b3301a5565a844c8a4d9940a70f0df0adaa1f2d7fd7cb20d8783d225501ac6b516c9a" };

    multi_index = 0;
    for (uint32_t child_num = 0; child_num < 2; ++child_num) {
        // Use unknown type
        uint8_t* script = NULL;
        size_t script_len = 0;
        ret = descriptor_to_script("L1", &desc, TESTNET, multi_index, child_num, NULL, &script, &script_len, &errmsg);
        if (!ret) {
            FAIL();
        }

        char* hex = NULL;
        JADE_WALLY_VERIFY(wally_hex_from_bytes(script, script_len, &hex));
        if (!hex || strcmp(hex, expectedL[child_num])) {
            free(hex);
            free(script);
            FAIL();
        }

        free(hex);
        free(script);
    }

    // Check serialisation
    if (!check_descriptor_serialisation(&desc)) {
        FAIL();
    }

    // Liquid example
    if (!descriptor_allow_liquid()) {
        return true; // Skip test if Liquid support not enabled
    }
    for (int i = 0; i < 2; i++) {
        INIT_DESC(desc, "ct(slip77(@B),wpkh(@0/<0;1>/*))");
        ADD_MAP_VAL(desc, "@B", "afacc503637e85da661ca1706c4ea147f1407868c48d8f92dd339ac272293cdc");
        ADD_MAP_VAL(desc, "@0",
            "[e3ebcc79/84'/1'/0']"
            "tpubDC2Q4xK4XH72Gae41fkTzHPCugZXvDDZJwEDzVT5osM5ejQULHVtUhbjFqUBqTnMX5RVwtkstpBvjdznWG89y3Kt48"
            "4Ta3qxTFPPWoyTh8s");
        bool get_bk = i;
        char* blinding_key = NULL;
        ret = descriptor_get_signers(
            "LQ0", &desc, LTESTNET, NULL, NULL, 0, &num_signers, get_bk ? &blinding_key : NULL, &errmsg);
        if (!ret) {
            FAIL();
        }
        if (num_signers != 1) {
            FAIL();
        }
        if (get_bk && blinding_key) {
            FAIL(); // Blinding key should not be returned when testing number of signers
        }
        ret = descriptor_get_signers(
            "LQ0", &desc, LTESTNET, &desc.type, signers, 3, &num_signers, get_bk ? &blinding_key : NULL, &errmsg);
        if (!ret) {
            FAIL();
        }
        if (desc.type != DESCRIPTOR_TYPE_MIXED) {
            FAIL();
        }
        if (num_signers != 1) {
            FAIL();
        }
        if (get_bk && !blinding_key) {
            FAIL(); // Blinding key should be returned
        }
        if (get_bk && strcmp(blinding_key, "afacc503637e85da661ca1706c4ea147f1407868c48d8f92dd339ac272293cdc")) {
            FAIL(); // Blinding key should match expected value
        }
        if (blinding_key) {
            wally_free_string(blinding_key);
        }

        if (signers[0].derivation_len != 3 || signers[0].derivation[0] != harden(84)
            || signers[0].derivation[1] != harden(1) || signers[0].derivation[2] != harden(0)) {
            FAIL();
        }
        if (!signers[0].path_is_string || strcmp(signers[0].path_str, "<0;1>/*")) {
            FAIL();
        }

        if (!FP_XPUB_MATCH(0, "e3ebcc79",
                "tpubDC2Q4xK4XH72Gae41fkTzHPCugZXvDDZJwEDzVT5osM5ejQULHVtUhbjFqUBqTnMX5RVwtkstpBvjdznWG89y3Kt48"
                "4Ta3qxTFPPWoyTh8s")) {
            FAIL();
        }

        // The expected/correct scriptPubkey
        const char* expectedLQ[2]
            = { "00142aca142db0227433da1e44ccc914b0cf6ad8cf72", "00144ae0383771d8140d4ce37cdfe08f0728b5c599ed" };

        multi_index = 0;
        for (uint32_t child_num = 0; child_num < 2; ++child_num) {
            // Use unknown type
            uint8_t* script = NULL;
            size_t script_len = 0;
            ret = descriptor_to_script(
                "LQ0", &desc, LTESTNET, multi_index, child_num, NULL, &script, &script_len, &errmsg);
            if (!ret) {
                FAIL();
            }

            char* hex = NULL;
            JADE_WALLY_VERIFY(wally_hex_from_bytes(script, script_len, &hex));
            if (!hex || strcmp(hex, expectedLQ[child_num])) {
                free(hex);
                free(script);
                FAIL();
            }

            free(hex);
            free(script);
        }

        if (!check_descriptor_serialisation(&desc)) {
            FAIL();
        }
    }

    return true;
}

bool debug_selfcheck(jade_process_t* process)
{
    if (!selfcheck_set_test_mnemonic(process, true, true)) {
        FAIL();
    }

    if (!test_miniscript_descriptors()) {
        FAIL();
    }

    return true;
}
