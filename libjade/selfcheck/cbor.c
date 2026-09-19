#include "selfcheck.h"

typedef struct {
    const char* cbor_hex;
    bool expected_ok;
} cbor_test_t;

static const cbor_test_t cbor_tests[] = {
    // String with length exceeding the input buffer
    { "5AFFFFFFFF", false }
};

static bool test_invalid_cbor(void)
{
    static unsigned char cbor_bytes[8 * 1024];

    for (size_t i = 0; i < sizeof(cbor_tests) / sizeof(cbor_tests[0]); ++i) {
        const cbor_test_t* test = &cbor_tests[i];
        size_t written;
        int ret = wally_hex_to_bytes(test->cbor_hex, cbor_bytes, sizeof(cbor_bytes), &written);
        if (ret != WALLY_OK || written > sizeof(cbor_bytes)) {
            return false;
        }

        CborParser parser;
        CborValue result;
        if (rpc_untrusted_parser_init(cbor_bytes, written, &parser, &result) != test->expected_ok) {
            return false;
        }

        const uint8_t* bytes;
        size_t bytes_len;
        if (bcur_parse_bytes(cbor_bytes, written, &bytes, &bytes_len) != test->expected_ok) {
            return false;
        }

        if (test->expected_ok && (!bytes || !bytes_len)) {
            return false;
        } else if (!test->expected_ok && (bytes || bytes_len)) {
            return false;
        }
    }
    return true;
}

bool debug_selfcheck(jade_process_t* process)
{
    if (!test_invalid_cbor()) {
        FAIL();
    }
    return true;
}
