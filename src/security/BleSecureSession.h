

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../protocol/CompanionProtocol.h"

class BleSecureSession {
public:
    void reset();

    bool isReady() const { return _ready; }
    const char* lastError() const { return _lastError; }

    bool buildChallenge(uint8_t* out, size_t outCap, size_t& outLen);
    bool completeFromResponse(const uint8_t* data, size_t len);

    bool encrypt(uint8_t channel,
                 const uint8_t* plaintext,
                 size_t plaintextLen,
                 uint8_t* out,
                 size_t outCap,
                 size_t& outLen);

    bool decrypt(uint8_t channel,
                 const uint8_t* envelope,
                 size_t envelopeLen,
                 uint8_t* out,
                 size_t outCap,
                 size_t& outLen);

    static size_t maxPlaintextForPayload(size_t attPayload);

private:
    void setError(const char* error);
    bool loadProvisioning();
    bool makeDeviceSignature(uint8_t outSig[PHONE_AUTH_P256_SIGNATURE_SIZE]);
    bool verifyPhoneSignature(const uint8_t phoneNonce[PHONE_AUTH_NONCE_SIZE],
                              const uint8_t phoneEphPub[PHONE_AUTH_P256_PUBLIC_KEY_SIZE],
                              const uint8_t sig[PHONE_AUTH_P256_SIGNATURE_SIZE]);
    bool deriveKeys(const uint8_t phoneNonce[PHONE_AUTH_NONCE_SIZE],
                    const uint8_t phoneEphPub[PHONE_AUTH_P256_PUBLIC_KEY_SIZE]);
    bool crypt(bool encrypting,
               uint8_t channel,
               uint32_t counter,
               const uint8_t* input,
               size_t inputLen,
               uint8_t* output,
               size_t outputCap,
               size_t& outputLen);

    bool _ready = false;
    const char* _lastError = nullptr;

    bool _provisioningLoaded = false;
    uint8_t _devicePriv[PHONE_AUTH_P256_PRIVATE_KEY_SIZE] = {};
    uint8_t _devicePub[PHONE_AUTH_P256_PUBLIC_KEY_SIZE] = {};
    uint8_t _phonePub[PHONE_AUTH_P256_PUBLIC_KEY_SIZE] = {};

    uint8_t _deviceNonce[PHONE_AUTH_NONCE_SIZE] = {};
    uint8_t _deviceEphPriv[PHONE_AUTH_P256_PRIVATE_KEY_SIZE] = {};
    uint8_t _deviceEphPub[PHONE_AUTH_P256_PUBLIC_KEY_SIZE] = {};

    uint8_t _devToPhoneKey[32] = {};
    uint8_t _phoneToDevKey[32] = {};
    uint8_t _devToPhoneIv[12] = {};
    uint8_t _phoneToDevIv[12] = {};

    uint32_t _txCounter[8] = {};
    uint32_t _rxCounter[8] = {};
};


