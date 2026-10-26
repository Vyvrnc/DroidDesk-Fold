package com.orailnoor.droiddesk.runtime

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyPermanentlyInvalidatedException
import android.security.keystore.KeyProperties
import android.util.Base64
import android.util.Log
import java.security.KeyStore
import java.security.SecureRandom
import javax.crypto.AEADBadTagException
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/**
 * Password of Debian's login keyring (gnome-keyring, droiddesk-session-bus).
 *
 * A random 32-byte password, generated once, kept only encrypted (IV + AES-256-GCM ciphertext) in
 * the app's private preferences, outside the Debian rootfs. The AES key lives in AndroidKeyStore:
 * not exportable, usable only while the device is unlocked (setUnlockedDeviceRequired, API 28 =
 * minSdk). A copy of the app data alone therefore does not open the keyring.
 *
 * When the key is gone or no longer matches (app data restored without it, Keystore wiped or the
 * key invalidated) a new password is made and [Secret.reset] tells the session bus host to
 * archive the old keyring and create a new one; stored secrets are lost then. While the device is
 * locked (or on any other Keystore error) nothing is reset: [load] returns null and the next
 * session start tries again.
 */
object KeyringSecret {
    private const val TAG = "KeyringSecret"
    private const val ALIAS = "droiddesk-keyring"
    private const val PREFS = "droiddesk_keyring"
    // "<base64 IV>:<base64 ciphertext>"
    private const val KEY_BLOB = "password_v1"
    // Set when a new password replaced a lost one, until the host has reset the keyring.
    private const val KEY_RESET = "reset_pending"
    private const val GCM_TAG_BITS = 128

    /** [password] is ASCII (base64 of the random bytes); the caller clears it after use. */
    class Secret(val password: ByteArray, val reset: Boolean)

    private class KeyLost(message: String) : Exception(message)

    @Synchronized
    fun load(context: Context): Secret? {
        val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        val keyStore = runCatching { KeyStore.getInstance("AndroidKeyStore").apply { load(null) } }
            .onFailure { Log.w(TAG, "AndroidKeyStore unavailable: ${it.message}") }
            .getOrNull() ?: return null
        val blob = prefs.getString(KEY_BLOB, null)
        var lost = false
        if (blob != null) {
            try {
                val key = keyStore.getKey(ALIAS, null) as? SecretKey ?: throw KeyLost("key $ALIAS missing")
                return Secret(decrypt(key, blob), prefs.getBoolean(KEY_RESET, false))
            } catch (error: Exception) {
                if (!isPermanent(error)) {
                    // Device locked (UnlockedDeviceRequired) or a passing Keystore error: try later.
                    Log.w(TAG, "Keyring password not available now: $error")
                    return null
                }
                Log.w(TAG, "Klíč klíčenky v Android Keystore se ztratil ($error), klíčenka Debianu bude založena znovu")
                lost = true
            }
        } else if (runCatching { keyStore.containsAlias(ALIAS) }.getOrDefault(false)) {
            // The key without its password (preferences cleared): the keyring cannot be opened either.
            lost = true
        }
        return try {
            runCatching { keyStore.deleteEntry(ALIAS) }
            val key = newKey()
            val password = Base64.encode(ByteArray(32).also(SecureRandom()::nextBytes), Base64.NO_WRAP)
            val cipher = Cipher.getInstance("AES/GCM/NoPadding").apply { init(Cipher.ENCRYPT_MODE, key) }
            val encoded = Base64.encodeToString(cipher.iv, Base64.NO_WRAP) + ":" +
                Base64.encodeToString(cipher.doFinal(password), Base64.NO_WRAP)
            val reset = lost || prefs.getBoolean(KEY_RESET, false)
            if (!prefs.edit().putString(KEY_BLOB, encoded).putBoolean(KEY_RESET, reset).commit()) {
                password.fill(0)
                Log.w(TAG, "Keyring password could not be stored")
                return null
            }
            Log.i(TAG, "New keyring password created${if (reset) " (keyring reset pending)" else ""}")
            Secret(password, reset)
        } catch (error: Exception) {
            Log.w(TAG, "Keyring password could not be created: $error")
            null
        }
    }

    /** The host created the keyring with the new password. */
    @Synchronized
    fun resetDone(context: Context) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().remove(KEY_RESET).apply()
    }

    private fun isPermanent(error: Exception): Boolean =
        error is KeyLost || error is KeyPermanentlyInvalidatedException ||
            error is AEADBadTagException || error is IllegalArgumentException

    private fun decrypt(key: SecretKey, blob: String): ByteArray {
        val parts = blob.split(":")
        require(parts.size == 2) { "malformed keyring password" }
        val iv = Base64.decode(parts[0], Base64.NO_WRAP)
        val data = Base64.decode(parts[1], Base64.NO_WRAP)
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.DECRYPT_MODE, key, GCMParameterSpec(GCM_TAG_BITS, iv))
        return cipher.doFinal(data)
    }

    private fun newKey(): SecretKey {
        fun generate(unlockedDeviceRequired: Boolean): SecretKey {
            val spec = KeyGenParameterSpec.Builder(ALIAS, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256)
                .setUnlockedDeviceRequired(unlockedDeviceRequired)
                .build()
            return KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore")
                .apply { init(spec) }
                .generateKey()
        }
        return try {
            generate(true)
        } catch (error: Exception) {
            // Some Keystore implementations refuse the flag (no secure lock screen); a key without
            // it still keeps the password off the disk in plain form.
            Log.w(TAG, "Key with UnlockedDeviceRequired refused ($error), creating one without it")
            generate(false)
        }
    }
}
