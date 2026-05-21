package com.spectre.companion

/**
 * Phone-side provisioning constants for the BLE secure session.
 *
 * Mirrors the structure of `src/secrets.h` on the Spectre device.  The two
 * sides must agree on the device public key and the phone public key; the
 * phone owns its private key here, the device owns its private key in
 * `SPECTRE_DEVICE_PRIVATE_KEY_HEX` on the firmware side.
 *
 * Generating a fresh P-256 keypair (OpenSSL one-liner):
 *
 *     openssl ecparam -name prime256v1 -genkey -noout -outform DER \
 *         | openssl ec -inform DER -text -noout
 *
 * The "priv" 32 bytes go into [PHONE_PRIVATE_KEY_HEX].
 * The "pub" 65 bytes (starting with 04) go into [PHONE_PUBLIC_KEY_HEX] AND
 * into the device's `SPECTRE_PHONE_PUBLIC_KEY_HEX` so it can verify phone
 * signatures.
 *
 * Until [PHONE_PRIVATE_KEY_HEX] is populated, [BleSecureSession] will fail
 * the AUTH handshake and slices that depend on encryption (commands, log
 * streaming) will not function.
 */
internal object SpectreSecrets {
    // 65-byte uncompressed P-256 public key, hex.  Must match
    // SPECTRE_DEVICE_PUBLIC_KEY_HEX in the device's secrets.h.
    const val DEVICE_PUBLIC_KEY_HEX =
        "04ec1cfed700751091fa87010b554ef2e15a7c573d2a77fe3966b16856fc8ebee9c57d9d348c91937f8cb8e1abea902ec36d3ca3594ebf4fe4d8bc30716eb303f5"

    // 65-byte uncompressed P-256 public key, hex.  Must match
    // SPECTRE_PHONE_PUBLIC_KEY_HEX in the device's secrets.h.
    const val PHONE_PUBLIC_KEY_HEX =
        "04cf8e260012590fb48b86ef7f11049de7d9efaa7ed0d4d8e28960fcfc3d02a0ad8ccbe1aff060877675ece696b8a11c9758771cec0e1df647dc654a7914863348"

    // 32-byte P-256 private scalar, hex.  Pairs with PHONE_PUBLIC_KEY_HEX
    // above.  The original priv key from the pre-rebuild era could not be
    // located on this filesystem, so this is a fresh keypair generated
    // 2026-05-20 — the device's secrets.h has been updated to match.
    const val PHONE_PRIVATE_KEY_HEX =
        "f8aefae75e0df3e3f4988215b5a001ad8cee9f1a2553794d0eb4529aaf4beea9"
}
