#pragma once

#include <Arduino.h>
#include <mbedtls/sha256.h>
#include <mbedtls/aes.h>

class CryptoHelper {
public:
    // Derive SHA-256 hash from a passphrase string
    static void deriveKey(const String &pass, uint8_t *hash);

    // AES-128-CBC encryption with PKCS7 padding
    static void aesEncrypt(const uint8_t *key,   
                           const uint8_t *input, size_t len,   
                           uint8_t *output, size_t &outLen);
    
    // AES-128-CBC decryption with PKCS7 padding removal
    static bool aesDecrypt(const uint8_t *key,
                              const uint8_t *input, size_t len,
                              uint8_t *output, size_t &outLen);
};
