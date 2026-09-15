#pragma once

#include <juce_core/juce_core.h>
#include <juce_cryptography/juce_cryptography.h>

/*
    UNI 76 - shared RSA sign/verify helpers for the offline license system.

    Included by both the plugin itself (Core/LicenseState.cpp, which only
    ever calls verifySignature()) and the developer-only offline keygen
    tool (Tools/LicenseKeygen/main.cpp, which only ever calls
    signPayload()) - kept in one place so the two sides can never drift
    into subtly incompatible hash/signature conventions. See
    docs/LICENSING.md for the full workflow.

    Deliberately plain "textbook" RSA (BigInteger::applyToValue(), no
    PKCS#1 padding) - this is a closed system with exactly one signer (the
    developer's own offline private key) and one verifier (this plugin
    binary's embedded public key), not a general-purpose protocol talking
    to untrusted peers, so padding's usual purpose (defeating chosen-
    ciphertext attacks against a service that signs attacker-supplied
    data) doesn't apply here. A SHA-256 digest (256 bits) is always far
    smaller than the RSA modulus (3072 bits, see Tools/LicenseKeygen), so
    the raw modular-exponentiation operation is well-defined.

    signature == hash^d mod n (the private-key operation, "signing")
    hash      == signature^e mod n (the public-key operation, "verifying")

    Comparison is done as BigInteger objects, not hex-string text, so that
    a hash value with a leading zero nibble (SHA256's own toHexString()
    zero-pads to 64 hex chars; BigInteger::toString(16) never pads) can
    never produce a false verification failure.
*/
namespace uni76::license
{
    inline juce::BigInteger hashPayloadAsBigInteger (const juce::String& payload)
    {
        juce::BigInteger result;
        result.parseString (juce::SHA256 (payload.toUTF8()).toHexString(), 16);
        return result;
    }

    /** Developer-only (Tools/LicenseKeygen) - encrypts the payload's hash
        with the private key. Returns the signature as a hex string. */
    inline juce::String signPayload (const juce::String& payload, const juce::String& privateKeyString)
    {
        juce::RSAKey privateKey (privateKeyString);
        jassert (privateKey.isValid());

        auto hash = hashPayloadAsBigInteger (payload);
        privateKey.applyToValue (hash);
        return hash.toString (16);
    }

    /** Plugin-side - decrypts the signature with the embedded public key
        and compares against a fresh hash of the payload. Fails closed
        (false) on any malformed input - an invalid/empty signature, an
        invalid public key string, or a mismatch. */
    inline bool verifySignature (const juce::String& payload, const juce::String& signatureHex, const juce::String& publicKeyString)
    {
        if (payload.isEmpty() || signatureHex.isEmpty())
            return false;

        juce::RSAKey publicKey (publicKeyString);
        if (! publicKey.isValid())
            return false;

        juce::BigInteger signature;
        signature.parseString (signatureHex, 16);
        if (signature.isZero())
            return false;

        publicKey.applyToValue (signature);
        return signature == hashPayloadAsBigInteger (payload);
    }
}
