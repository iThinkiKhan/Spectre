package com.spectre.companion

import java.math.BigInteger
import java.security.AlgorithmParameters
import java.security.KeyFactory
import java.security.KeyPair
import java.security.KeyPairGenerator
import java.security.MessageDigest
import java.security.SecureRandom
import java.security.Signature
import java.security.interfaces.ECPrivateKey
import java.security.interfaces.ECPublicKey
import java.security.spec.ECGenParameterSpec
import java.security.spec.ECParameterSpec
import java.security.spec.ECPoint
import java.security.spec.ECPrivateKeySpec
import java.security.spec.ECPublicKeySpec
import javax.crypto.Cipher
import javax.crypto.KeyAgreement
import javax.crypto.Mac
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

/**
 * Phone-side mirror of `src/security/BleSecureSession.h`.
 *
 * Wire-compatible with the device's session: ECDH(P-256) + HKDF-SHA256 +
 * AES-GCM-256 with per-channel counters.  The phone is the *responder* —
 * the device writes a challenge to the AUTH characteristic, the phone
 * derives the session keys and replies with a response.
 *
 * After [handleChallenge] returns success, [encrypt]/[decrypt] are usable
 * on any of [PHONE_SECURE_CHANNEL_GPS..PHONE_SECURE_CHANNEL_NOTIFICATION]
 * (channels 0x01..0x0a — kept in lock-step with the device's
 * `validChannel()` and the `CHANNEL_MAX` constant below).  Direction
 * mapping mirrors the device side: outbound uses the phone→device key,
 * inbound uses the device→phone key.
 */
internal class BleSecureSession {

    private companion object {
        const val DEVICE_SIG_LABEL = "SpectreBLEDeviceAuthV1"
        const val PHONE_SIG_LABEL = "SpectreBLEPhoneAuthV1"
        const val SESSION_SALT_LABEL = "SpectreBLESessionSaltV1"
        const val SESSION_INFO_LABEL = "SpectreBLESessionKeysV1"

        const val AUTH_FRAME_SIZE = 163  // version + op + 32 + 65 + 64
        const val NONCE_SIZE = 32
        const val EPH_PUB_SIZE = 65       // 0x04 + 32 + 32
        const val EPH_PRIV_SIZE = 32
        const val SIG_SIZE = 64           // raw r||s

        const val AES_KEY_SIZE = 32
        const val GCM_IV_SIZE = 12
        const val GCM_TAG_BITS = 128
        const val GCM_TAG_BYTES = 16
        const val SECURE_HEADER_SIZE = 6  // version + channel + counter(u32 LE)
        const val SECURE_ENVELOPE_OVERHEAD = SECURE_HEADER_SIZE + GCM_TAG_BYTES

        const val PROTOCOL_VERSION: Byte = 1
        const val OP_CHALLENGE: Byte = 0x01
        const val OP_RESPONSE: Byte = 0x02

        const val CHANNEL_MIN = 0x01
        const val CHANNEL_MAX = 0x0a  // up through NOTIFICATION; matches device validChannel()

        const val COUNTER_SLOTS = 16  // mirrors device-side array sizing

        val random = SecureRandom()
    }

    var ready: Boolean = false
        private set

    var lastError: String? = null
        private set

    // Derived session state.
    private var devToPhoneKey: ByteArray = ByteArray(0)
    private var phoneToDevKey: ByteArray = ByteArray(0)
    private var devToPhoneIv: ByteArray = ByteArray(0)
    private var phoneToDevIv: ByteArray = ByteArray(0)
    private val txCounter = LongArray(COUNTER_SLOTS)
    private val rxCounter = LongArray(COUNTER_SLOTS)

    // Long-term keys from SpectreSecrets.
    private var devicePubBytes: ByteArray = ByteArray(0)
    private var phonePubBytes: ByteArray = ByteArray(0)
    private var phonePrivBytes: ByteArray = ByteArray(0)
    private var provisioningLoaded = false

    fun reset() {
        ready = false
        lastError = null
        devToPhoneKey = ByteArray(0)
        phoneToDevKey = ByteArray(0)
        devToPhoneIv = ByteArray(0)
        phoneToDevIv = ByteArray(0)
        txCounter.fill(0)
        rxCounter.fill(0)
    }

    fun phonePublicKeyHex(): String? = if (loadProvisioning()) bytesToHex(phonePubBytes) else null

    fun phonePublicKeyBytes(): ByteArray? = if (loadProvisioning()) phonePubBytes.copyOf() else null

    /**
     * Process an incoming AUTH characteristic write from the device.  On
     * success, returns the 163-byte response frame the phone should publish
     * on the AUTH characteristic and updates session state to ready=true.
     * Returns null on any failure; [lastError] explains.
     */
    fun handleChallenge(data: ByteArray): ByteArray? {
        reset()
        if (data.size != AUTH_FRAME_SIZE) {
            return fail("auth challenge length invalid (${data.size})")
        }
        if (data[0] != PROTOCOL_VERSION || data[1] != OP_CHALLENGE) {
            return fail("auth challenge header invalid")
        }
        if (!loadProvisioning()) {
            return null
        }

        val deviceNonce = data.copyOfRange(2, 2 + NONCE_SIZE)
        val deviceEphPub = data.copyOfRange(2 + NONCE_SIZE, 2 + NONCE_SIZE + EPH_PUB_SIZE)
        val deviceSig = data.copyOfRange(2 + NONCE_SIZE + EPH_PUB_SIZE, AUTH_FRAME_SIZE)

        if (deviceEphPub[0] != 0x04.toByte()) {
            return fail("device ephemeral key not uncompressed")
        }

        if (!verifyDeviceSignature(deviceNonce, deviceEphPub, deviceSig)) {
            return null
        }

        val phoneNonce = ByteArray(NONCE_SIZE).also { random.nextBytes(it) }
        val ephKp: KeyPair = generateP256()
        val phoneEphPub = encodeP256Public(ephKp.public as ECPublicKey)

        val phoneSig = signPhoneAuth(deviceNonce, phoneNonce, deviceEphPub, phoneEphPub)
            ?: return null

        val deviceEphPubKey = importP256Public(deviceEphPub)
            ?: return fail("device ephemeral pubkey invalid")

        // ECDH against the DEVICE'S EPHEMERAL pubkey, not the long-term one.
        // The device's long-term pubkey was already used by
        // verifyDeviceSignature() above; we don't need it again here.
        val sharedSecret = computeEcdh(ephKp.private as ECPrivateKey, deviceEphPubKey)
            ?: return null

        if (!deriveSessionKeys(sharedSecret, deviceNonce, phoneNonce,
                               deviceEphPub, phoneEphPub)) {
            return null
        }

        val response = ByteArray(AUTH_FRAME_SIZE)
        response[0] = PROTOCOL_VERSION
        response[1] = OP_RESPONSE
        System.arraycopy(phoneNonce, 0, response, 2, NONCE_SIZE)
        System.arraycopy(phoneEphPub, 0, response, 2 + NONCE_SIZE, EPH_PUB_SIZE)
        System.arraycopy(phoneSig, 0, response, 2 + NONCE_SIZE + EPH_PUB_SIZE, SIG_SIZE)

        ready = true
        return response
    }

    fun encrypt(channel: Int, plaintext: ByteArray): ByteArray? {
        if (!ready) return fail("secure session not ready")
        if (channel < CHANNEL_MIN || channel > CHANNEL_MAX) {
            return fail("secure encrypt invalid channel ($channel)")
        }
        if (channel >= COUNTER_SLOTS) return fail("channel out of counter range")

        val counter = txCounter[channel] + 1
        if (counter > 0xFFFFFFFFL || counter == 0L) {
            return fail("secure tx counter exhausted")
        }

        val header = ByteArray(SECURE_HEADER_SIZE).also {
            it[0] = PROTOCOL_VERSION
            it[1] = channel.toByte()
            writeLe32(it, 2, counter)
        }
        val nonce = makeNonce(phoneToDevIv, channel, counter)
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE,
                    SecretKeySpec(phoneToDevKey, "AES"),
                    GCMParameterSpec(GCM_TAG_BITS, nonce))
        cipher.updateAAD(header)
        val combined = try {
            cipher.doFinal(plaintext)
        } catch (error: Exception) {
            return fail("AES-GCM encrypt failed: ${error.message}")
        }

        val envelope = ByteArray(SECURE_ENVELOPE_OVERHEAD + plaintext.size)
        System.arraycopy(header, 0, envelope, 0, SECURE_HEADER_SIZE)
        System.arraycopy(combined, 0, envelope, SECURE_HEADER_SIZE, combined.size)
        txCounter[channel] = counter
        return envelope
    }

    fun decrypt(channel: Int, envelope: ByteArray): ByteArray? {
        if (!ready) return fail("secure session not ready")
        if (channel < CHANNEL_MIN || channel > CHANNEL_MAX) {
            return fail("secure decrypt invalid channel ($channel)")
        }
        if (channel >= COUNTER_SLOTS) return fail("channel out of counter range")
        if (envelope.size < SECURE_ENVELOPE_OVERHEAD) return fail("envelope too short")
        if (envelope[0] != PROTOCOL_VERSION) return fail("envelope version mismatch")
        if (envelope[1].toInt() and 0xFF != channel) return fail("envelope channel mismatch")

        val counter = readLe32(envelope, 2)
        if (counter == 0L || counter <= rxCounter[channel]) {
            return fail("replay/stale counter (got $counter, last ${rxCounter[channel]})")
        }

        val cipherLen = envelope.size - SECURE_ENVELOPE_OVERHEAD
        val nonce = makeNonce(devToPhoneIv, channel, counter)

        val header = envelope.copyOfRange(0, SECURE_HEADER_SIZE)
        // Java's GCM expects ciphertext||tag concatenated.
        val cipherAndTag = envelope.copyOfRange(SECURE_HEADER_SIZE, envelope.size)

        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.DECRYPT_MODE,
                    SecretKeySpec(devToPhoneKey, "AES"),
                    GCMParameterSpec(GCM_TAG_BITS, nonce))
        cipher.updateAAD(header)
        val plaintext = try {
            cipher.doFinal(cipherAndTag)
        } catch (error: Exception) {
            return fail("AES-GCM decrypt/auth failed: ${error.message}")
        }
        if (plaintext.size != cipherLen) {
            return fail("decrypt produced ${plaintext.size} bytes, expected $cipherLen")
        }

        rxCounter[channel] = counter
        return plaintext
    }

    // ── Internals ────────────────────────────────────────────────────────────

    private fun fail(error: String): ByteArray? {
        lastError = error
        return null
    }

    private fun loadProvisioning(): Boolean {
        if (provisioningLoaded) return true
        val devicePub = decodeHex(SpectreSecrets.DEVICE_PUBLIC_KEY_HEX)
        val phonePub = decodeHex(SpectreSecrets.PHONE_PUBLIC_KEY_HEX)
        val phonePriv = decodeHex(SpectreSecrets.PHONE_PRIVATE_KEY_HEX)
        if (devicePub == null || devicePub.size != EPH_PUB_SIZE || devicePub[0] != 0x04.toByte()) {
            lastError = "DEVICE_PUBLIC_KEY_HEX missing or malformed"
            return false
        }
        if (phonePub == null || phonePub.size != EPH_PUB_SIZE || phonePub[0] != 0x04.toByte()) {
            lastError = "PHONE_PUBLIC_KEY_HEX missing or malformed"
            return false
        }
        if (phonePriv == null || phonePriv.size != EPH_PRIV_SIZE) {
            lastError = "PHONE_PRIVATE_KEY_HEX missing or malformed (paste 32 bytes hex)"
            return false
        }
        devicePubBytes = devicePub
        phonePubBytes = phonePub
        phonePrivBytes = phonePriv
        provisioningLoaded = true
        return true
    }

    private fun verifyDeviceSignature(
        deviceNonce: ByteArray,
        deviceEphPub: ByteArray,
        rawSig: ByteArray,
    ): Boolean {
        if (rawSig.size != SIG_SIZE) {
            lastError = "device signature length invalid"
            return false
        }
        val devicePub = importP256Public(devicePubBytes) ?: return false
        val sigDer = rawSignatureToDer(rawSig) ?: run {
            lastError = "device signature DER conversion failed"
            return false
        }
        val hashInput = buildAuthHash(
            DEVICE_SIG_LABEL,
            deviceNonce,
            null,
            devicePubBytes,
            phonePubBytes,
            deviceEphPub,
            null,
        )
        return try {
            val sigVerifier = Signature.getInstance("NONEwithECDSA")
            sigVerifier.initVerify(devicePub)
            sigVerifier.update(hashInput)
            if (!sigVerifier.verify(sigDer)) {
                lastError = "device signature rejected"
                false
            } else true
        } catch (error: Exception) {
            lastError = "device signature verify error: ${error.message}"
            false
        }
    }

    private fun signPhoneAuth(
        deviceNonce: ByteArray,
        phoneNonce: ByteArray,
        deviceEphPub: ByteArray,
        phoneEphPub: ByteArray,
    ): ByteArray? {
        val phonePriv = importP256Private(phonePrivBytes) ?: return null
        val hashInput = buildAuthHash(
            PHONE_SIG_LABEL,
            deviceNonce,
            phoneNonce,
            devicePubBytes,
            phonePubBytes,
            deviceEphPub,
            phoneEphPub,
        )
        return try {
            val sigSigner = Signature.getInstance("NONEwithECDSA")
            sigSigner.initSign(phonePriv)
            sigSigner.update(hashInput)
            derSignatureToRaw(sigSigner.sign()) ?: run {
                lastError = "phone signature DER->raw failed"
                null
            }
        } catch (error: Exception) {
            lastError = "phone signature error: ${error.message}"
            null
        }
    }

    /** Build the 32-byte SHA-256 digest that ECDSA signs over for either side. */
    private fun buildAuthHash(
        label: String,
        deviceNonce: ByteArray,
        phoneNonce: ByteArray?,
        devicePub: ByteArray,
        phonePub: ByteArray,
        deviceEphPub: ByteArray,
        phoneEphPub: ByteArray?,
    ): ByteArray {
        val md = MessageDigest.getInstance("SHA-256")
        md.update(label.toByteArray(Charsets.UTF_8))
        md.update(deviceNonce)
        if (phoneNonce != null) md.update(phoneNonce)
        md.update(devicePub)
        md.update(phonePub)
        md.update(deviceEphPub)
        if (phoneEphPub != null) md.update(phoneEphPub)
        return md.digest()
    }

    private fun computeEcdh(privateKey: ECPrivateKey, peerPub: ECPublicKey): ByteArray? {
        return try {
            val ka = KeyAgreement.getInstance("ECDH")
            ka.init(privateKey)
            ka.doPhase(peerPub, true)
            ka.generateSecret()
        } catch (error: Exception) {
            fail("ECDH compute failed: ${error.message}")
            null
        }
    }

    private fun deriveSessionKeys(
        sharedSecret: ByteArray,
        deviceNonce: ByteArray,
        phoneNonce: ByteArray,
        deviceEphPub: ByteArray,
        phoneEphPub: ByteArray,
    ): Boolean {
        val saltMd = MessageDigest.getInstance("SHA-256")
        saltMd.update(SESSION_SALT_LABEL.toByteArray(Charsets.UTF_8))
        saltMd.update(deviceNonce)
        saltMd.update(phoneNonce)
        saltMd.update(devicePubBytes)
        saltMd.update(phonePubBytes)
        saltMd.update(deviceEphPub)
        saltMd.update(phoneEphPub)
        val salt = saltMd.digest()

        val blockSize = (AES_KEY_SIZE * 2) + (GCM_IV_SIZE * 2)
        val okm = hkdfSha256(
            salt,
            sharedSecret,
            SESSION_INFO_LABEL.toByteArray(Charsets.UTF_8),
            blockSize,
        ) ?: return fail("HKDF failed") != null

        var offset = 0
        devToPhoneKey = okm.copyOfRange(offset, offset + AES_KEY_SIZE)
        offset += AES_KEY_SIZE
        phoneToDevKey = okm.copyOfRange(offset, offset + AES_KEY_SIZE)
        offset += AES_KEY_SIZE
        devToPhoneIv = okm.copyOfRange(offset, offset + GCM_IV_SIZE)
        offset += GCM_IV_SIZE
        phoneToDevIv = okm.copyOfRange(offset, offset + GCM_IV_SIZE)
        txCounter.fill(0)
        rxCounter.fill(0)
        return true
    }

    private fun makeNonce(baseIv: ByteArray, channel: Int, counter: Long): ByteArray {
        val out = baseIv.copyOf(GCM_IV_SIZE)
        out[0] = (out[0].toInt() xor channel).toByte()
        out[8] = (out[8].toInt() xor ((counter ushr 24).toInt() and 0xFF)).toByte()
        out[9] = (out[9].toInt() xor ((counter ushr 16).toInt() and 0xFF)).toByte()
        out[10] = (out[10].toInt() xor ((counter ushr 8).toInt() and 0xFF)).toByte()
        out[11] = (out[11].toInt() xor (counter.toInt() and 0xFF)).toByte()
        return out
    }

    // ── P-256 import/export helpers ─────────────────────────────────────────

    private val p256Params: ECParameterSpec by lazy {
        val parameters = AlgorithmParameters.getInstance("EC")
        parameters.init(ECGenParameterSpec("secp256r1"))
        parameters.getParameterSpec(ECParameterSpec::class.java)
    }

    private fun importP256Public(uncompressed: ByteArray): ECPublicKey? {
        if (uncompressed.size != EPH_PUB_SIZE || uncompressed[0] != 0x04.toByte()) return null
        return try {
            val x = BigInteger(1, uncompressed.copyOfRange(1, 33))
            val y = BigInteger(1, uncompressed.copyOfRange(33, 65))
            val point = ECPoint(x, y)
            val spec = ECPublicKeySpec(point, p256Params)
            KeyFactory.getInstance("EC").generatePublic(spec) as ECPublicKey
        } catch (error: Exception) {
            fail("import P-256 public failed: ${error.message}")
            null
        }
    }

    private fun importP256Private(scalar: ByteArray): ECPrivateKey? {
        if (scalar.size != EPH_PRIV_SIZE) return null
        return try {
            val d = BigInteger(1, scalar)
            val spec = ECPrivateKeySpec(d, p256Params)
            KeyFactory.getInstance("EC").generatePrivate(spec) as ECPrivateKey
        } catch (error: Exception) {
            fail("import P-256 private failed: ${error.message}")
            null
        }
    }

    private fun generateP256(): KeyPair {
        val kpg = KeyPairGenerator.getInstance("EC")
        kpg.initialize(ECGenParameterSpec("secp256r1"))
        return kpg.generateKeyPair()
    }

    private fun encodeP256Public(key: ECPublicKey): ByteArray {
        val out = ByteArray(EPH_PUB_SIZE)
        out[0] = 0x04
        val x = key.w.affineX.toByteArray()
        val y = key.w.affineY.toByteArray()
        writeFixedBigInt(x, out, 1, 32)
        writeFixedBigInt(y, out, 33, 32)
        return out
    }

    /** Right-align a possibly-leading-zero or oversized BigInteger byte rep into a fixed slot. */
    private fun writeFixedBigInt(src: ByteArray, dst: ByteArray, offset: Int, fieldLen: Int) {
        // Drop leading 0x00 from BigInteger's two's-complement padding, then
        // left-pad zeros if the value is shorter than fieldLen.
        var srcStart = 0
        var srcLen = src.size
        if (srcLen > fieldLen) {
            // Expect a single leading 0x00 from sign byte.
            srcStart = srcLen - fieldLen
            srcLen = fieldLen
        }
        java.util.Arrays.fill(dst, offset, offset + fieldLen, 0)
        System.arraycopy(src, srcStart, dst, offset + fieldLen - srcLen, srcLen)
    }

    // ── ECDSA raw <-> DER conversion ────────────────────────────────────────
    //
    // The C++ side uses raw 64-byte r||s.  JCE produces/consumes DER.

    private fun rawSignatureToDer(raw: ByteArray): ByteArray? {
        if (raw.size != SIG_SIZE) return null
        val r = raw.copyOfRange(0, 32)
        val s = raw.copyOfRange(32, 64)
        val rDer = asn1Integer(r)
        val sDer = asn1Integer(s)
        val seqLen = rDer.size + sDer.size
        val out = ByteArray(2 + seqLen)
        out[0] = 0x30
        out[1] = seqLen.toByte()
        System.arraycopy(rDer, 0, out, 2, rDer.size)
        System.arraycopy(sDer, 0, out, 2 + rDer.size, sDer.size)
        return out
    }

    private fun derSignatureToRaw(der: ByteArray): ByteArray? {
        if (der.size < 8 || der[0] != 0x30.toByte()) return null
        // Tolerate short-form length only (signatures are always < 128 bytes).
        val seqLen = der[1].toInt() and 0xFF
        if (seqLen + 2 != der.size) return null
        var idx = 2
        if (der[idx] != 0x02.toByte()) return null
        idx++
        val rLen = der[idx].toInt() and 0xFF
        idx++
        val rStart = idx
        idx += rLen
        if (der[idx] != 0x02.toByte()) return null
        idx++
        val sLen = der[idx].toInt() and 0xFF
        idx++
        val sStart = idx
        val out = ByteArray(SIG_SIZE)
        copyBigIntFixed(der, rStart, rLen, out, 0, 32)
        copyBigIntFixed(der, sStart, sLen, out, 32, 32)
        return out
    }

    private fun copyBigIntFixed(
        src: ByteArray, srcOffset: Int, srcLen: Int,
        dst: ByteArray, dstOffset: Int, fieldLen: Int,
    ) {
        var s = srcOffset
        var len = srcLen
        // Drop leading 0x00 padding inserted by DER for positive integers.
        while (len > fieldLen && src[s] == 0x00.toByte()) {
            s++
            len--
        }
        if (len > fieldLen) {
            // Shouldn't happen for a valid P-256 component.
            return
        }
        java.util.Arrays.fill(dst, dstOffset, dstOffset + fieldLen, 0)
        System.arraycopy(src, s, dst, dstOffset + fieldLen - len, len)
    }

    private fun asn1Integer(value: ByteArray): ByteArray {
        var start = 0
        while (start < value.size - 1 && value[start] == 0x00.toByte()) {
            start++
        }
        val needsPad = (value[start].toInt() and 0x80) != 0
        val intLen = value.size - start + (if (needsPad) 1 else 0)
        val out = ByteArray(2 + intLen)
        out[0] = 0x02
        out[1] = intLen.toByte()
        if (needsPad) {
            out[2] = 0
            System.arraycopy(value, start, out, 3, value.size - start)
        } else {
            System.arraycopy(value, start, out, 2, value.size - start)
        }
        return out
    }

    // ── HKDF-SHA256 (manual impl; JCE on Android lacks Hkdf until API 33+) ──

    private fun hkdfSha256(
        salt: ByteArray,
        ikm: ByteArray,
        info: ByteArray,
        outLen: Int,
    ): ByteArray? {
        return try {
            val mac = Mac.getInstance("HmacSHA256")
            val saltKey = SecretKeySpec(
                if (salt.isEmpty()) ByteArray(32) else salt, "HmacSHA256")
            mac.init(saltKey)
            val prk = mac.doFinal(ikm)

            val out = ByteArray(outLen)
            var written = 0
            var counter = 1
            var prev = ByteArray(0)
            mac.init(SecretKeySpec(prk, "HmacSHA256"))
            while (written < outLen) {
                mac.reset()
                mac.init(SecretKeySpec(prk, "HmacSHA256"))
                if (prev.isNotEmpty()) mac.update(prev)
                mac.update(info)
                mac.update(byteArrayOf(counter.toByte()))
                prev = mac.doFinal()
                val take = minOf(prev.size, outLen - written)
                System.arraycopy(prev, 0, out, written, take)
                written += take
                counter++
            }
            out
        } catch (error: Exception) {
            fail("HKDF: ${error.message}")
            null
        }
    }

    // ── Byte helpers ────────────────────────────────────────────────────────

    private fun writeLe32(buf: ByteArray, offset: Int, value: Long) {
        buf[offset] = (value and 0xFF).toByte()
        buf[offset + 1] = ((value ushr 8) and 0xFF).toByte()
        buf[offset + 2] = ((value ushr 16) and 0xFF).toByte()
        buf[offset + 3] = ((value ushr 24) and 0xFF).toByte()
    }

    private fun readLe32(buf: ByteArray, offset: Int): Long {
        return ((buf[offset].toLong() and 0xFF)) or
            ((buf[offset + 1].toLong() and 0xFF) shl 8) or
            ((buf[offset + 2].toLong() and 0xFF) shl 16) or
            ((buf[offset + 3].toLong() and 0xFF) shl 24)
    }

    private fun decodeHex(hex: String): ByteArray? {
        if (hex.isEmpty() || hex.length % 2 != 0) return null
        val out = ByteArray(hex.length / 2)
        for (i in out.indices) {
            val hi = nibble(hex[i * 2]) ?: return null
            val lo = nibble(hex[i * 2 + 1]) ?: return null
            out[i] = ((hi shl 4) or lo).toByte()
        }
        return out
    }

    private fun nibble(c: Char): Int? = when (c) {
        in '0'..'9' -> c.code - '0'.code
        in 'a'..'f' -> 10 + (c.code - 'a'.code)
        in 'A'..'F' -> 10 + (c.code - 'A'.code)
        else -> null
    }

    private fun bytesToHex(bytes: ByteArray): String {
        val digits = "0123456789abcdef"
        val sb = StringBuilder(bytes.size * 2)
        for (b in bytes) {
            sb.append(digits[(b.toInt() ushr 4) and 0x0F])
            sb.append(digits[b.toInt() and 0x0F])
        }
        return sb.toString()
    }
}
