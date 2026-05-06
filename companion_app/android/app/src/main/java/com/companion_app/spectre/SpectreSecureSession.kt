package com.companion_app.spectre

import android.os.Build
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import com.companion_app.BuildConfig
import java.math.BigInteger
import java.security.AlgorithmParameters
import java.security.KeyFactory
import java.security.KeyPair
import java.security.KeyPairGenerator
import java.security.KeyStore
import java.security.PrivateKey
import java.security.PublicKey
import java.security.SecureRandom
import java.security.Signature
import java.security.interfaces.ECPublicKey
import java.security.spec.ECGenParameterSpec
import java.security.spec.ECParameterSpec
import java.security.spec.ECPoint
import java.security.spec.ECPublicKeySpec
import javax.crypto.Cipher
import javax.crypto.KeyAgreement
import javax.crypto.Mac
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

class SpectreSecureSession(
  private val log: (String) -> Unit,
) {
  companion object {
    const val AUTH_FRAME_SIZE = 2 + 32 + 65 + 64
    const val SECURE_ENVELOPE_OVERHEAD = 6 + 16

    const val CHANNEL_GPS = 0x01
    const val CHANNEL_CONTROL = 0x02
    const val CHANNEL_META = 0x03
    const val CHANNEL_EVENT_BATCH = 0x04
    const val CHANNEL_ENRICHMENT = 0x05
    const val CHANNEL_STORAGE = 0x06

    private const val KEY_ALIAS = "spectre.phone.p256.signing.v1"
    private const val PROTOCOL_VERSION = 1
    private const val AUTH_OP_CHALLENGE = 0x01
    private const val AUTH_OP_RESPONSE = 0x02
    private const val DEVICE_SIG_LABEL = "SpectreBLEDeviceAuthV1"
    private const val PHONE_SIG_LABEL = "SpectreBLEPhoneAuthV1"
    private const val SESSION_SALT_LABEL = "SpectreBLESessionSaltV1"
    private const val SESSION_INFO_LABEL = "SpectreBLESessionKeysV1"
  }

  private val secureRandom = SecureRandom()

  private var ready = false
  private var devicePublicKeyBytes = ByteArray(0)
  private var phonePublicKeyBytes = ByteArray(0)
  private var devToPhoneKey = ByteArray(0)
  private var phoneToDevKey = ByteArray(0)
  private var devToPhoneIv = ByteArray(0)
  private var phoneToDevIv = ByteArray(0)
  private val txCounters = IntArray(8)
  private val rxCounters = IntArray(8)

  val isReady: Boolean
    get() = ready

  fun reset() {
    ready = false
    devToPhoneKey.fill(0)
    phoneToDevKey.fill(0)
    devToPhoneIv.fill(0)
    phoneToDevIv.fill(0)
    devToPhoneKey = ByteArray(0)
    phoneToDevKey = ByteArray(0)
    devToPhoneIv = ByteArray(0)
    phoneToDevIv = ByteArray(0)
    txCounters.fill(0)
    rxCounters.fill(0)
  }

  fun ensurePhonePublicKeyHex(): String {
    val keyPair = ensurePhoneKeyPair()
    val publicBytes = publicKeyBytes(keyPair.public)
    phonePublicKeyBytes = publicBytes
    val hex = toHex(publicBytes)
    log("phone auth publicKey=$hex")
    return hex
  }

  fun maxPlaintextForPayload(attPayloadBytes: Int): Int =
    (attPayloadBytes - SECURE_ENVELOPE_OVERHEAD).coerceAtLeast(0)

  fun handleChallenge(challenge: ByteArray): ByteArray {
    reset()

    if (challenge.size != AUTH_FRAME_SIZE ||
      challenge[0].toInt() != PROTOCOL_VERSION ||
      challenge[1].toInt() != AUTH_OP_CHALLENGE
    ) {
      throw SecurityException("Invalid auth challenge")
    }

    devicePublicKeyBytes = decodeHex(BuildConfig.SPECTRE_DEVICE_PUBLIC_KEY_HEX, 65)
    if (devicePublicKeyBytes.firstOrNull() != 0x04.toByte()) {
      throw SecurityException("Pinned Spectre device public key missing")
    }

    val phoneKeyPair = ensurePhoneKeyPair()
    phonePublicKeyBytes = publicKeyBytes(phoneKeyPair.public)

    var offset = 2
    val deviceNonce = challenge.copyOfRange(offset, offset + 32)
    offset += 32
    val deviceEphPub = challenge.copyOfRange(offset, offset + 65)
    offset += 65
    val deviceSig = challenge.copyOfRange(offset, offset + 64)

    val deviceTranscript = deviceTranscript(deviceNonce, deviceEphPub)
    if (!verifyRawP256(devicePublicKeyBytes, deviceTranscript, deviceSig)) {
      throw SecurityException("Spectre device signature rejected")
    }

    val phoneNonce = ByteArray(32)
    secureRandom.nextBytes(phoneNonce)

    val eph = generateEphemeralKeyPair()
    val phoneEphPub = publicKeyBytes(eph.public)
    val shared = computeEcdh(eph.private, deviceEphPub)
    deriveKeys(shared, deviceNonce, phoneNonce, deviceEphPub, phoneEphPub)
    shared.fill(0)

    val phoneTranscript = phoneTranscript(deviceNonce, phoneNonce, deviceEphPub, phoneEphPub)
    val phoneSig = signRawP256(phoneKeyPair.private, phoneTranscript)

    ready = true
    log("app secure session ready alg=P-256+ECDH+AES-256-GCM")

    return ByteArray(AUTH_FRAME_SIZE).also { out ->
      var o = 0
      out[o++] = PROTOCOL_VERSION.toByte()
      out[o++] = AUTH_OP_RESPONSE.toByte()
      System.arraycopy(phoneNonce, 0, out, o, phoneNonce.size)
      o += phoneNonce.size
      System.arraycopy(phoneEphPub, 0, out, o, phoneEphPub.size)
      o += phoneEphPub.size
      System.arraycopy(phoneSig, 0, out, o, phoneSig.size)
    }
  }

  fun encrypt(channel: Int, plaintext: ByteArray): ByteArray {
    requireReady()
    require(validChannel(channel)) { "Invalid secure channel" }

    val counter = txCounters[channel] + 1
    if (counter <= 0) {
      throw SecurityException("Secure tx counter exhausted")
    }
    txCounters[channel] = counter

    val header = ByteArray(6)
    header[0] = PROTOCOL_VERSION.toByte()
    header[1] = channel.toByte()
    writeLe32(header, 2, counter)

    val cipher = Cipher.getInstance("AES/GCM/NoPadding")
    cipher.init(
      Cipher.ENCRYPT_MODE,
      SecretKeySpec(phoneToDevKey, "AES"),
      GCMParameterSpec(128, makeNonce(phoneToDevIv, channel, counter)),
    )
    cipher.updateAAD(header)
    val sealed = cipher.doFinal(plaintext)
    return header + sealed
  }

  fun decrypt(channel: Int, envelope: ByteArray): ByteArray {
    requireReady()
    require(validChannel(channel)) { "Invalid secure channel" }
    if (envelope.size < SECURE_ENVELOPE_OVERHEAD ||
      envelope[0].toInt() != PROTOCOL_VERSION ||
      envelope[1].toInt() != channel
    ) {
      throw SecurityException("Invalid secure envelope")
    }

    val counter = readLe32(envelope, 2)
    if (counter <= 0 || counter <= rxCounters[channel]) {
      throw SecurityException("Secure replay/stale counter")
    }

    val cipher = Cipher.getInstance("AES/GCM/NoPadding")
    cipher.init(
      Cipher.DECRYPT_MODE,
      SecretKeySpec(devToPhoneKey, "AES"),
      GCMParameterSpec(128, makeNonce(devToPhoneIv, channel, counter)),
    )
    val header = envelope.copyOfRange(0, 6)
    cipher.updateAAD(header)
    val plaintext = cipher.doFinal(envelope, 6, envelope.size - 6)
    rxCounters[channel] = counter
    return plaintext
  }

  private fun requireReady() {
    if (!ready) {
      throw SecurityException("Secure session not ready")
    }
  }

  private fun ensurePhoneKeyPair(): KeyPair {
    val keyStore = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
    if (!keyStore.containsAlias(KEY_ALIAS)) {
      generatePhoneSigningKey(useStrongBox = true)
    }

    val entry = keyStore.getEntry(KEY_ALIAS, null) as? KeyStore.PrivateKeyEntry
      ?: throw SecurityException("Phone signing key unavailable")
    return KeyPair(entry.certificate.publicKey, entry.privateKey)
  }

  private fun generatePhoneSigningKey(useStrongBox: Boolean) {
    val generator = KeyPairGenerator.getInstance(
      KeyProperties.KEY_ALGORITHM_EC,
      "AndroidKeyStore",
    )
    val builder = KeyGenParameterSpec.Builder(
      KEY_ALIAS,
      KeyProperties.PURPOSE_SIGN or KeyProperties.PURPOSE_VERIFY,
    )
      .setAlgorithmParameterSpec(ECGenParameterSpec("secp256r1"))
      .setDigests(KeyProperties.DIGEST_SHA256)
      .setUserAuthenticationRequired(false)

    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P && useStrongBox) {
      builder.setIsStrongBoxBacked(true)
    }

    try {
      generator.initialize(builder.build())
      generator.generateKeyPair()
      log("phone auth signing key generated strongBox=$useStrongBox")
    } catch (error: Exception) {
      if (useStrongBox) {
        log("StrongBox unavailable for phone auth key; using Android Keystore TEE")
        generatePhoneSigningKey(useStrongBox = false)
      } else {
        throw error
      }
    }
  }

  private fun generateEphemeralKeyPair(): KeyPair {
    val generator = KeyPairGenerator.getInstance("EC")
    generator.initialize(ECGenParameterSpec("secp256r1"), secureRandom)
    return generator.generateKeyPair()
  }

  private fun deviceTranscript(deviceNonce: ByteArray, deviceEphPub: ByteArray): ByteArray =
    DEVICE_SIG_LABEL.toByteArray() +
      deviceNonce +
      devicePublicKeyBytes +
      phonePublicKeyBytes +
      deviceEphPub

  private fun phoneTranscript(
    deviceNonce: ByteArray,
    phoneNonce: ByteArray,
    deviceEphPub: ByteArray,
    phoneEphPub: ByteArray,
  ): ByteArray =
    PHONE_SIG_LABEL.toByteArray() +
      deviceNonce +
      phoneNonce +
      devicePublicKeyBytes +
      phonePublicKeyBytes +
      deviceEphPub +
      phoneEphPub

  private fun deriveKeys(
    shared: ByteArray,
    deviceNonce: ByteArray,
    phoneNonce: ByteArray,
    deviceEphPub: ByteArray,
    phoneEphPub: ByteArray,
  ) {
    val salt = sha256(
      SESSION_SALT_LABEL.toByteArray() +
        deviceNonce +
        phoneNonce +
        devicePublicKeyBytes +
        phonePublicKeyBytes +
        deviceEphPub +
        phoneEphPub,
    )
    val okm = hkdfSha256(
      salt,
      shared,
      SESSION_INFO_LABEL.toByteArray(),
      32 + 32 + 12 + 12,
    )
    var offset = 0
    devToPhoneKey = okm.copyOfRange(offset, offset + 32)
    offset += 32
    phoneToDevKey = okm.copyOfRange(offset, offset + 32)
    offset += 32
    devToPhoneIv = okm.copyOfRange(offset, offset + 12)
    offset += 12
    phoneToDevIv = okm.copyOfRange(offset, offset + 12)
  }

  private fun computeEcdh(privateKey: PrivateKey, peerRawPublicKey: ByteArray): ByteArray {
    val keyAgreement = KeyAgreement.getInstance("ECDH")
    keyAgreement.init(privateKey)
    keyAgreement.doPhase(rawPublicKey(peerRawPublicKey), true)
    return keyAgreement.generateSecret()
  }

  private fun signRawP256(privateKey: PrivateKey, data: ByteArray): ByteArray {
    val sig = Signature.getInstance("SHA256withECDSA")
    sig.initSign(privateKey)
    sig.update(data)
    return derToRawSignature(sig.sign())
  }

  private fun verifyRawP256(publicKeyBytes: ByteArray, data: ByteArray, rawSignature: ByteArray): Boolean {
    val sig = Signature.getInstance("SHA256withECDSA")
    sig.initVerify(rawPublicKey(publicKeyBytes))
    sig.update(data)
    return sig.verify(rawToDerSignature(rawSignature))
  }

  private fun rawPublicKey(raw: ByteArray): PublicKey {
    if (raw.size != 65 || raw[0] != 0x04.toByte()) {
      throw SecurityException("Invalid P-256 public key")
    }
    val spec = ECPublicKeySpec(
      ECPoint(
        BigInteger(1, raw.copyOfRange(1, 33)),
        BigInteger(1, raw.copyOfRange(33, 65)),
      ),
      ecParams(),
    )
    return KeyFactory.getInstance("EC").generatePublic(spec)
  }

  private fun publicKeyBytes(publicKey: PublicKey): ByteArray {
    val ec = publicKey as? ECPublicKey
      ?: throw SecurityException("Expected EC public key")
    return byteArrayOf(0x04) +
      fixed32(ec.w.affineX) +
      fixed32(ec.w.affineY)
  }

  private fun ecParams(): ECParameterSpec {
    val parameters = AlgorithmParameters.getInstance("EC")
    parameters.init(ECGenParameterSpec("secp256r1"))
    return parameters.getParameterSpec(ECParameterSpec::class.java)
  }

  private fun fixed32(value: BigInteger): ByteArray {
    val bytes = value.toByteArray()
    val positive = if (bytes.size > 1 && bytes[0] == 0.toByte()) {
      bytes.copyOfRange(1, bytes.size)
    } else {
      bytes
    }
    if (positive.size > 32) {
      return positive.copyOfRange(positive.size - 32, positive.size)
    }
    return ByteArray(32 - positive.size) + positive
  }

  private fun rawToDerSignature(raw: ByteArray): ByteArray {
    if (raw.size != 64) {
      throw SecurityException("Invalid raw ECDSA signature")
    }
    val r = derInteger(raw.copyOfRange(0, 32))
    val s = derInteger(raw.copyOfRange(32, 64))
    val length = r.size + s.size
    return byteArrayOf(0x30, length.toByte()) + r + s
  }

  private fun derToRawSignature(der: ByteArray): ByteArray {
    if (der.size < 8 || der[0] != 0x30.toByte()) {
      throw SecurityException("Invalid DER ECDSA signature")
    }
    var offset = 2
    if ((der[1].toInt() and 0x80) != 0) {
      val lenBytes = der[1].toInt() and 0x7F
      offset = 2 + lenBytes
    }
    val r = readDerInteger(der, offset)
    offset = r.second
    val s = readDerInteger(der, offset)
    return unsignedFixed32(r.first) + unsignedFixed32(s.first)
  }

  private fun derInteger(raw: ByteArray): ByteArray {
    var value = raw.dropWhile { it == 0.toByte() }.toByteArray()
    if (value.isEmpty()) {
      value = byteArrayOf(0)
    }
    if ((value[0].toInt() and 0x80) != 0) {
      value = byteArrayOf(0) + value
    }
    return byteArrayOf(0x02, value.size.toByte()) + value
  }

  private fun readDerInteger(der: ByteArray, offset: Int): Pair<ByteArray, Int> {
    if (offset + 2 > der.size || der[offset] != 0x02.toByte()) {
      throw SecurityException("Invalid DER integer")
    }
    val len = der[offset + 1].toInt() and 0xFF
    val start = offset + 2
    val end = start + len
    if (end > der.size) {
      throw SecurityException("Invalid DER integer length")
    }
    return der.copyOfRange(start, end) to end
  }

  private fun unsignedFixed32(value: ByteArray): ByteArray {
    val stripped = if (value.size > 1 && value[0] == 0.toByte()) {
      value.copyOfRange(1, value.size)
    } else {
      value
    }
    if (stripped.size > 32) {
      return stripped.copyOfRange(stripped.size - 32, stripped.size)
    }
    return ByteArray(32 - stripped.size) + stripped
  }

  private fun hkdfSha256(salt: ByteArray, ikm: ByteArray, info: ByteArray, length: Int): ByteArray {
    val prkMac = Mac.getInstance("HmacSHA256")
    prkMac.init(SecretKeySpec(salt, "HmacSHA256"))
    val prk = prkMac.doFinal(ikm)

    val out = ByteArray(length)
    var previous = ByteArray(0)
    var offset = 0
    var blockIndex = 1
    while (offset < length) {
      val mac = Mac.getInstance("HmacSHA256")
      mac.init(SecretKeySpec(prk, "HmacSHA256"))
      mac.update(previous)
      mac.update(info)
      mac.update(blockIndex.toByte())
      previous = mac.doFinal()
      val copyLen = minOf(previous.size, length - offset)
      System.arraycopy(previous, 0, out, offset, copyLen)
      offset += copyLen
      blockIndex++
    }
    prk.fill(0)
    previous.fill(0)
    return out
  }

  private fun sha256(data: ByteArray): ByteArray {
    val digest = java.security.MessageDigest.getInstance("SHA-256")
    return digest.digest(data)
  }

  private fun makeNonce(base: ByteArray, channel: Int, counter: Int): ByteArray {
    val nonce = base.copyOf()
    nonce[0] = (nonce[0].toInt() xor channel).toByte()
    nonce[8] = (nonce[8].toInt() xor ((counter ushr 24) and 0xFF)).toByte()
    nonce[9] = (nonce[9].toInt() xor ((counter ushr 16) and 0xFF)).toByte()
    nonce[10] = (nonce[10].toInt() xor ((counter ushr 8) and 0xFF)).toByte()
    nonce[11] = (nonce[11].toInt() xor (counter and 0xFF)).toByte()
    return nonce
  }

  private fun validChannel(channel: Int): Boolean =
    channel in CHANNEL_GPS..CHANNEL_STORAGE

  private fun writeLe32(out: ByteArray, offset: Int, value: Int) {
    out[offset] = (value and 0xFF).toByte()
    out[offset + 1] = ((value ushr 8) and 0xFF).toByte()
    out[offset + 2] = ((value ushr 16) and 0xFF).toByte()
    out[offset + 3] = ((value ushr 24) and 0xFF).toByte()
  }

  private fun readLe32(input: ByteArray, offset: Int): Int =
    (input[offset].toInt() and 0xFF) or
      ((input[offset + 1].toInt() and 0xFF) shl 8) or
      ((input[offset + 2].toInt() and 0xFF) shl 16) or
      ((input[offset + 3].toInt() and 0xFF) shl 24)

  private fun decodeHex(hex: String, expectedBytes: Int): ByteArray {
    if (hex.length != expectedBytes * 2) {
      throw SecurityException("Invalid hex key length")
    }
    return ByteArray(expectedBytes) { index ->
      hex.substring(index * 2, index * 2 + 2).toInt(16).toByte()
    }
  }

  private fun toHex(bytes: ByteArray): String =
    bytes.joinToString("") { "%02x".format(it.toInt() and 0xFF) }
}
