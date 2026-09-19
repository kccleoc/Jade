from . import *


def _check_msg_signature(jade, test_case, actual):
    """
    Helper to verify a message signature - handles checking an Anti-Exfil signature
    contains the entropy that was passed in by the host.
    """
    expected = test_case['expected_output']
    assert len(actual) == len(expected)

    inputdata = test_case['input']
    host_entropy = inputdata.get('ae_host_entropy')
    network = 'localtest'  # Network is irrelevant to sign-msg

    if host_entropy:
        # Anti-Exfil signer_commitment and signature
        assert tuple(expected) == actual, [actual[0].hex(), actual[1]]
        signer_commitment, signature = actual
    else:
        # Standard EC signature
        assert actual == expected, actual
        signer_commitment, signature = None, actual  # No signer_commitment for EC sig

    # Get the message hash
    msgbytes = inputdata['message'].encode('utf8')
    msghash = wally.format_bitcoin_message(msgbytes, wally.BITCOIN_MESSAGE_FLAG_HASH)
    rawsig = base64.b64decode(signature)  # un-base64 the returned signature

    # Verify the signature
    verify_signature(jade, network, msghash, inputdata['path'],
                     host_entropy, signer_commitment, rawsig, is_schnorr=False)


@with_test_cases('tests/rpc/data/sign_message/msg_*.json')
def test_sign_message(jade, test_case):
    inputdata = test_case['input']
    rslt = jade.sign_message(inputdata['path'],
                             inputdata['message'],
                             inputdata.get('use_ae_signatures'),
                             inputdata.get('ae_host_commitment'),
                             inputdata.get('ae_host_entropy'))

    # Check returned signature
    _check_msg_signature(jade, test_case, rslt)


@with_test_cases('tests/rpc/data/sign_message/msgfile_*.json')
def test_sign_message_file(jade, test_case):
    inputdata = test_case['input']
    expected_output = test_case.get('expected_output')
    expected_error = test_case.get('expected_error')
    assert expected_output or expected_error

    try:
        rslt = jade.sign_message_file(inputdata['filedata'])
        assert expected_error is None, f'Expected error: {expected_error}'
        assert rslt == expected_output, 'Expected output: ' + expected_output
    except JadeError as e:
        assert expected_output is None, f'Expected output: {expected_output}'
        assert e.message == expected_error, 'Expected error: ' + expected_error
