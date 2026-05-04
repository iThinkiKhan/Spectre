
#include "BleSecureSession.h"

#include <string.h>

#include <esp_random.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <mbedtls/gcm.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>

#include "../SecretsConfig.h"

#ifndef SPECTRE_DEVICE_PRIVATE_KEY_HEX
#define SPECTRE_DEVICE_PRIVATE_KEY_HEX ""
#endif

#ifndef SPECTRE_DEVICE_PUBLIC_KEY_HEX
#define SPECTRE_DEVICE_PUBLIC_KEY_HEX ""
#endif

#ifndef SPECTRE_PHONE_PUBLIC_KEY_HEX
#define SPECTRE_PHONE_PUBLIC_KEY_HEX ""
#endif

namespace {
constexpr const char* DEVICE_SIG_LABEL = "SpectreBLEDeviceAuthV1";
constexpr const char* PHONE_SIG_LABEL = "SpectreBLEPhoneAuthV1";
constexpr const char* SESSION_SALT_LABEL = "SpectreBLESessionSaltV1";
constexpr const char* SESSION_INFO_LABEL = "SpectreBLESessionKeysV1";

constexpr size_t AES_KEY_SIZE = 32;
constexpr size_t GCM_IV_SIZE = 12;
constexpr size_t SESSION_KEY_BLOCK_SIZE =
    AES_KEY_SIZE + AES_KEY_SIZE + GCM_IV_SIZE + GCM_IV_SIZE;

int espRng(void*, unsigned char* out, size_t len) {
    esp_fill_random(out, len);
    return 0;
}

int hexNibbleSecure(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

bool decodeFixedHex(const char* hex, uint8_t* out, size_t outLen) {
    if (!hex || !out || strlen(hex) != outLen * 2U) {
        return false;
    }
    for (size_t i = 0; i < outLen; ++i) {
        const int hi = hexNibbleSecure(hex[i * 2U]);
        const int lo = hexNibbleSecure(hex[i * 2U + 1U]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

bool constantTimeEqual(const uint8_t* a, const uint8_t* b, size_t len) {
    if (!a || !b) {
        return false;
    }
    uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i) {
        diff |= static_cast<uint8_t>(a[i] ^ b[i]);
    }
    return diff == 0;
}

void wipeBytes(uint8_t* data, size_t len) {
    if (!data) {
        return;
    }
    volatile uint8_t* p = data;
    while (len--) {
        *p++ = 0;
    }
}

void shaUpdate(mbedtls_sha256_context& ctx, const void* data, size_t len) {
    if (data && len > 0) {
        mbedtls_sha256_update(&ctx,
                              static_cast<const unsigned char*>(data),
                              len);
    }
}

void shaUpdateString(mbedtls_sha256_context& ctx, const char* s) {
    shaUpdate(ctx, s, strlen(s));
}

void writeLe32(uint8_t* out, uint32_t value) {
    out[0] = static_cast<uint8_t>(value & 0xFFU);
    out[1] = static_cast<uint8_t>((value >> 8) & 0xFFU);
    out[2] = static_cast<uint8_t>((value >> 16) & 0xFFU);
    out[3] = static_cast<uint8_t>((value >> 24) & 0xFFU);
}

uint32_t readLe32(const uint8_t* in) {
    return static_cast<uint32_t>(in[0]) |
           (static_cast<uint32_t>(in[1]) << 8) |
           (static_cast<uint32_t>(in[2]) << 16) |
           (static_cast<uint32_t>(in[3]) << 24);
}

void makeNonce(const uint8_t base[GCM_IV_SIZE],
               uint8_t channel,
               uint32_t counter,
               uint8_t out[GCM_IV_SIZE]) {
    memcpy(out, base, GCM_IV_SIZE);
    out[0] ^= channel;
    out[8] ^= static_cast<uint8_t>((counter >> 24) & 0xFFU);
    out[9] ^= static_cast<uint8_t>((counter >> 16) & 0xFFU);
    out[10] ^= static_cast<uint8_t>((counter >> 8) & 0xFFU);
    out[11] ^= static_cast<uint8_t>(counter & 0xFFU);
}

bool validChannel(uint8_t channel) {
    return channel >= PHONE_SECURE_CHANNEL_GPS &&
           channel <= PHONE_SECURE_CHANNEL_ENRICHMENT;
}
}

void BleSecureSession::setError(const char* error) {
    _lastError = error;
}

void BleSecureSession::reset() {
    _ready = false;
    _lastError = nullptr;
    _provisioningLoaded = false;
    wipeBytes(_devicePriv, sizeof(_devicePriv));
    memset(_devicePub, 0, sizeof(_devicePub));
    memset(_phonePub, 0, sizeof(_phonePub));
    memset(_deviceNonce, 0, sizeof(_deviceNonce));
    wipeBytes(_deviceEphPriv, sizeof(_deviceEphPriv));
    memset(_deviceEphPub, 0, sizeof(_deviceEphPub));
    wipeBytes(_devToPhoneKey, sizeof(_devToPhoneKey));
    wipeBytes(_phoneToDevKey, sizeof(_phoneToDevKey));
    memset(_devToPhoneIv, 0, sizeof(_devToPhoneIv));
    memset(_phoneToDevIv, 0, sizeof(_phoneToDevIv));
    memset(_txCounter, 0, sizeof(_txCounter));
    memset(_rxCounter, 0, sizeof(_rxCounter));
}

size_t BleSecureSession::maxPlaintextForPayload(size_t attPayload) {
    if (attPayload <= PHONE_SECURE_ENVELOPE_OVERHEAD) {
        return 0;
    }
    return attPayload - PHONE_SECURE_ENVELOPE_OVERHEAD;
}

bool BleSecureSession::loadProvisioning() {
    if (_provisioningLoaded) {
        return true;
    }

    if (!decodeFixedHex(SPECTRE_DEVICE_PRIVATE_KEY_HEX,
                        _devicePriv,
                        sizeof(_devicePriv))) {
        setError("device private key missing/invalid");
        return false;
    }
    if (!decodeFixedHex(SPECTRE_DEVICE_PUBLIC_KEY_HEX,
                        _devicePub,
                        sizeof(_devicePub)) ||
        _devicePub[0] != 0x04) {
        setError("device public key missing/invalid");
        return false;
    }
    if (!decodeFixedHex(SPECTRE_PHONE_PUBLIC_KEY_HEX,
                        _phonePub,
                        sizeof(_phonePub)) ||
        _phonePub[0] != 0x04) {
        setError("phone public key missing/invalid");
        return false;
    }

    mbedtls_ecp_group grp;
    mbedtls_ecp_point point;
    mbedtls_mpi priv;
    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&point);
    mbedtls_mpi_init(&priv);

    bool ok = false;
    if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
        mbedtls_mpi_read_binary(&priv, _devicePriv, sizeof(_devicePriv)) == 0 &&
        mbedtls_ecp_check_privkey(&grp, &priv) == 0 &&
        mbedtls_ecp_point_read_binary(&grp, &point, _devicePub, sizeof(_devicePub)) == 0 &&
        mbedtls_ecp_check_pubkey(&grp, &point) == 0 &&
        mbedtls_ecp_point_read_binary(&grp, &point, _phonePub, sizeof(_phonePub)) == 0 &&
        mbedtls_ecp_check_pubkey(&grp, &point) == 0) {
        ok = true;
    }

    mbedtls_mpi_free(&priv);
    mbedtls_ecp_point_free(&point);
    mbedtls_ecp_group_free(&grp);

    if (!ok) {
        setError("P-256 provisioning key validation failed");
        return false;
    }

    _provisioningLoaded = true;
    return true;
}

bool BleSecureSession::makeDeviceSignature(uint8_t outSig[PHONE_AUTH_P256_SIGNATURE_SIZE]) {
    uint8_t hash[32];
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    shaUpdateString(sha, DEVICE_SIG_LABEL);
    shaUpdate(sha, _deviceNonce, sizeof(_deviceNonce));
    shaUpdate(sha, _devicePub, sizeof(_devicePub));
    shaUpdate(sha, _phonePub, sizeof(_phonePub));
    shaUpdate(sha, _deviceEphPub, sizeof(_deviceEphPub));
    mbedtls_sha256_finish(&sha, hash);
    mbedtls_sha256_free(&sha);

    mbedtls_ecp_group grp;
    mbedtls_mpi d;
    mbedtls_mpi r;
    mbedtls_mpi s;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);

    const bool ok =
        mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
        mbedtls_mpi_read_binary(&d, _devicePriv, sizeof(_devicePriv)) == 0 &&
        mbedtls_ecdsa_sign(&grp, &r, &s, &d, hash, sizeof(hash), espRng, nullptr) == 0 &&
        mbedtls_mpi_write_binary(&r, outSig, 32) == 0 &&
        mbedtls_mpi_write_binary(&s, outSig + 32, 32) == 0;

    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);
    wipeBytes(hash, sizeof(hash));

    if (!ok) {
        setError("device signature failed");
    }
    return ok;
}

bool BleSecureSession::buildChallenge(uint8_t* out, size_t outCap, size_t& outLen) {
    outLen = 0;
    _ready = false;
    memset(_txCounter, 0, sizeof(_txCounter));
    memset(_rxCounter, 0, sizeof(_rxCounter));

    if (!out || outCap < PHONE_AUTH_FRAME_SIZE) {
        setError("auth challenge buffer too small");
        return false;
    }
    if (!loadProvisioning()) {
        return false;
    }

    esp_fill_random(_deviceNonce, sizeof(_deviceNonce));

    mbedtls_ecp_group grp;
    mbedtls_mpi ephPriv;
    mbedtls_ecp_point ephPub;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&ephPriv);
    mbedtls_ecp_point_init(&ephPub);

    size_t pubLen = 0;
    const bool keyOk =
        mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
        mbedtls_ecp_gen_keypair(&grp, &ephPriv, &ephPub, espRng, nullptr) == 0 &&
        mbedtls_mpi_write_binary(&ephPriv,
                                 _deviceEphPriv,
                                 sizeof(_deviceEphPriv)) == 0 &&
        mbedtls_ecp_point_write_binary(&grp,
                                       &ephPub,
                                       MBEDTLS_ECP_PF_UNCOMPRESSED,
                                       &pubLen,
                                       _deviceEphPub,
                                       sizeof(_deviceEphPub)) == 0 &&
        pubLen == sizeof(_deviceEphPub);

    mbedtls_ecp_point_free(&ephPub);
    mbedtls_mpi_free(&ephPriv);
    mbedtls_ecp_group_free(&grp);

    if (!keyOk) {
        setError("ephemeral P-256 keygen failed");
        return false;
    }

    uint8_t sig[PHONE_AUTH_P256_SIGNATURE_SIZE];
    if (!makeDeviceSignature(sig)) {
        return false;
    }

    size_t offset = 0;
    out[offset++] = COMPANION_PROTOCOL_VERSION;
    out[offset++] = PHONE_AUTH_OP_CHALLENGE;
    memcpy(out + offset, _deviceNonce, sizeof(_deviceNonce));
    offset += sizeof(_deviceNonce);
    memcpy(out + offset, _deviceEphPub, sizeof(_deviceEphPub));
    offset += sizeof(_deviceEphPub);
    memcpy(out + offset, sig, sizeof(sig));
    offset += sizeof(sig);
    outLen = offset;

    wipeBytes(sig, sizeof(sig));
    return true;
}

bool BleSecureSession::verifyPhoneSignature(
    const uint8_t phoneNonce[PHONE_AUTH_NONCE_SIZE],
    const uint8_t phoneEphPub[PHONE_AUTH_P256_PUBLIC_KEY_SIZE],
    const uint8_t sig[PHONE_AUTH_P256_SIGNATURE_SIZE]) {

    uint8_t hash[32];
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    shaUpdateString(sha, PHONE_SIG_LABEL);
    shaUpdate(sha, _deviceNonce, sizeof(_deviceNonce));
    shaUpdate(sha, phoneNonce, PHONE_AUTH_NONCE_SIZE);
    shaUpdate(sha, _devicePub, sizeof(_devicePub));
    shaUpdate(sha, _phonePub, sizeof(_phonePub));
    shaUpdate(sha, _deviceEphPub, sizeof(_deviceEphPub));
    shaUpdate(sha, phoneEphPub, PHONE_AUTH_P256_PUBLIC_KEY_SIZE);
    mbedtls_sha256_finish(&sha, hash);
    mbedtls_sha256_free(&sha);

    mbedtls_ecp_group grp;
    mbedtls_ecp_point q;
    mbedtls_mpi r;
    mbedtls_mpi s;
    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&q);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);

    const bool ok =
        mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
        mbedtls_ecp_point_read_binary(&grp, &q, _phonePub, sizeof(_phonePub)) == 0 &&
        mbedtls_ecp_check_pubkey(&grp, &q) == 0 &&
        mbedtls_mpi_read_binary(&r, sig, 32) == 0 &&
        mbedtls_mpi_read_binary(&s, sig + 32, 32) == 0 &&
        mbedtls_ecdsa_verify(&grp, hash, sizeof(hash), &q, &r, &s) == 0;

    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    mbedtls_ecp_point_free(&q);
    mbedtls_ecp_group_free(&grp);
    wipeBytes(hash, sizeof(hash));

    if (!ok) {
        setError("phone signature rejected");
    }
    return ok;
}

bool BleSecureSession::deriveKeys(
    const uint8_t phoneNonce[PHONE_AUTH_NONCE_SIZE],
    const uint8_t phoneEphPub[PHONE_AUTH_P256_PUBLIC_KEY_SIZE]) {

    uint8_t shared[32];
    uint8_t salt[32];
    uint8_t okm[SESSION_KEY_BLOCK_SIZE];

    mbedtls_ecp_group grp;
    mbedtls_ecp_point phoneQ;
    mbedtls_mpi d;
    mbedtls_mpi z;
    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&phoneQ);
    mbedtls_mpi_init(&d);
    mbedtls_mpi_init(&z);

    bool ok =
        mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
        mbedtls_ecp_point_read_binary(&grp,
                                      &phoneQ,
                                      phoneEphPub,
                                      PHONE_AUTH_P256_PUBLIC_KEY_SIZE) == 0 &&
        mbedtls_ecp_check_pubkey(&grp, &phoneQ) == 0 &&
        mbedtls_mpi_read_binary(&d,
                                _deviceEphPriv,
                                sizeof(_deviceEphPriv)) == 0 &&
        mbedtls_ecdh_compute_shared(&grp, &z, &phoneQ, &d, espRng, nullptr) == 0 &&
        mbedtls_mpi_write_binary(&z, shared, sizeof(shared)) == 0;

    mbedtls_mpi_free(&z);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_point_free(&phoneQ);
    mbedtls_ecp_group_free(&grp);

    if (!ok) {
        setError("ECDH shared secret failed");
        return false;
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    shaUpdateString(sha, SESSION_SALT_LABEL);
    shaUpdate(sha, _deviceNonce, sizeof(_deviceNonce));
    shaUpdate(sha, phoneNonce, PHONE_AUTH_NONCE_SIZE);
    shaUpdate(sha, _devicePub, sizeof(_devicePub));
    shaUpdate(sha, _phonePub, sizeof(_phonePub));
    shaUpdate(sha, _deviceEphPub, sizeof(_deviceEphPub));
    shaUpdate(sha, phoneEphPub, PHONE_AUTH_P256_PUBLIC_KEY_SIZE);
    mbedtls_sha256_finish(&sha, salt);
    mbedtls_sha256_free(&sha);

    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    ok = md != nullptr &&
         mbedtls_hkdf(md,
                      salt,
                      sizeof(salt),
                      shared,
                      sizeof(shared),
                      reinterpret_cast<const unsigned char*>(SESSION_INFO_LABEL),
                      strlen(SESSION_INFO_LABEL),
                      okm,
                      sizeof(okm)) == 0;
    if (ok) {
        size_t offset = 0;
        memcpy(_devToPhoneKey, okm + offset, sizeof(_devToPhoneKey));
        offset += sizeof(_devToPhoneKey);
        memcpy(_phoneToDevKey, okm + offset, sizeof(_phoneToDevKey));
        offset += sizeof(_phoneToDevKey);
        memcpy(_devToPhoneIv, okm + offset, sizeof(_devToPhoneIv));
        offset += sizeof(_devToPhoneIv);
        memcpy(_phoneToDevIv, okm + offset, sizeof(_phoneToDevIv));
        memset(_txCounter, 0, sizeof(_txCounter));
        memset(_rxCounter, 0, sizeof(_rxCounter));
        _ready = true;
    } else {
        setError("HKDF session key derivation failed");
    }

    wipeBytes(shared, sizeof(shared));
    wipeBytes(salt, sizeof(salt));
    wipeBytes(okm, sizeof(okm));
    wipeBytes(_deviceEphPriv, sizeof(_deviceEphPriv));
    return ok;
}

bool BleSecureSession::completeFromResponse(const uint8_t* data, size_t len) {
    _ready = false;
    if (!data || len != PHONE_AUTH_FRAME_SIZE) {
        setError("auth response length invalid");
        return false;
    }
    if (data[0] != COMPANION_PROTOCOL_VERSION ||
        data[1] != PHONE_AUTH_OP_RESPONSE) {
        setError("auth response header invalid");
        return false;
    }

    const uint8_t* phoneNonce = data + 2;
    const uint8_t* phoneEphPub = phoneNonce + PHONE_AUTH_NONCE_SIZE;
    const uint8_t* sig = phoneEphPub + PHONE_AUTH_P256_PUBLIC_KEY_SIZE;

    if (phoneEphPub[0] != 0x04) {
        setError("phone ephemeral public key invalid");
        return false;
    }

    if (!verifyPhoneSignature(phoneNonce, phoneEphPub, sig)) {
        return false;
    }

    return deriveKeys(phoneNonce, phoneEphPub);
}

bool BleSecureSession::encrypt(uint8_t channel,
                               const uint8_t* plaintext,
                               size_t plaintextLen,
                               uint8_t* out,
                               size_t outCap,
                               size_t& outLen) {
    outLen = 0;
    if (!_ready) {
        setError("secure session not ready");
        return false;
    }
    if (!validChannel(channel) ||
        !plaintext ||
        !out ||
        outCap < plaintextLen + PHONE_SECURE_ENVELOPE_OVERHEAD) {
        setError("secure encrypt invalid input");
        return false;
    }

    uint32_t counter = _txCounter[channel] + 1U;
    if (counter == 0) {
        setError("secure tx counter exhausted");
        return false;
    }

    uint8_t* header = out;
    header[0] = COMPANION_PROTOCOL_VERSION;
    header[1] = channel;
    writeLe32(header + 2, counter);

    uint8_t nonce[GCM_IV_SIZE];
    makeNonce(_devToPhoneIv, channel, counter, nonce);

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    const int rc =
        mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, _devToPhoneKey, 256) == 0
            ? mbedtls_gcm_crypt_and_tag(&gcm,
                                        MBEDTLS_GCM_ENCRYPT,
                                        plaintextLen,
                                        nonce,
                                        sizeof(nonce),
                                        header,
                                        PHONE_SECURE_HEADER_SIZE,
                                        plaintext,
                                        out + PHONE_SECURE_HEADER_SIZE,
                                        PHONE_SECURE_TAG_SIZE,
                                        out + PHONE_SECURE_HEADER_SIZE + plaintextLen)
            : -1;
    mbedtls_gcm_free(&gcm);

    if (rc != 0) {
        setError("AES-GCM encrypt failed");
        return false;
    }

    _txCounter[channel] = counter;
    outLen = plaintextLen + PHONE_SECURE_ENVELOPE_OVERHEAD;
    return true;
}

bool BleSecureSession::decrypt(uint8_t channel,
                               const uint8_t* envelope,
                               size_t envelopeLen,
                               uint8_t* out,
                               size_t outCap,
                               size_t& outLen) {
    outLen = 0;
    if (!_ready) {
        setError("secure session not ready");
        return false;
    }
    if (!validChannel(channel) ||
        !envelope ||
        !out ||
        envelopeLen < PHONE_SECURE_ENVELOPE_OVERHEAD ||
        envelope[0] != COMPANION_PROTOCOL_VERSION ||
        envelope[1] != channel) {
        setError("secure decrypt invalid envelope");
        return false;
    }

    const uint32_t counter = readLe32(envelope + 2);
    if (counter == 0 || counter <= _rxCounter[channel]) {
        setError("secure decrypt replay/stale counter");
        return false;
    }

    const size_t cipherLen = envelopeLen - PHONE_SECURE_ENVELOPE_OVERHEAD;
    if (outCap < cipherLen) {
        setError("secure decrypt output too small");
        return false;
    }

    uint8_t nonce[GCM_IV_SIZE];
    makeNonce(_phoneToDevIv, channel, counter, nonce);

    const uint8_t* ciphertext = envelope + PHONE_SECURE_HEADER_SIZE;
    const uint8_t* tag = envelope + PHONE_SECURE_HEADER_SIZE + cipherLen;

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    const int rc =
        mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, _phoneToDevKey, 256) == 0
            ? mbedtls_gcm_auth_decrypt(&gcm,
                                       cipherLen,
                                       nonce,
                                       sizeof(nonce),
                                       envelope,
                                       PHONE_SECURE_HEADER_SIZE,
                                       tag,
                                       PHONE_SECURE_TAG_SIZE,
                                       ciphertext,
                                       out)
            : -1;
    mbedtls_gcm_free(&gcm);

    if (rc != 0) {
        setError("AES-GCM decrypt/auth failed");
        return false;
    }

    _rxCounter[channel] = counter;
    outLen = cipherLen;
    return true;
}

