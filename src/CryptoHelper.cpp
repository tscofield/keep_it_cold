#include "CryptoHelper.h"
#include <mbedtls/aes.h>
#include <Arduino.h>

// --------------------- SHA-256 wrapper ---------------------
#if defined(mbedtls_sha256_starts_ret)
#define SHA256_START(ctx, is224) mbedtls_sha256_starts_ret(ctx, is224)
#define SHA256_UPDATE(ctx, buf, len) mbedtls_sha256_update_ret(ctx, buf, len)
#define SHA256_FINISH(ctx, out) mbedtls_sha256_finish_ret(ctx, out)
#else
#define SHA256_START(ctx, is224) mbedtls_sha256_starts(ctx, is224)
#define SHA256_UPDATE(ctx, buf, len) mbedtls_sha256_update(ctx, buf, len)
#define SHA256_FINISH(ctx, out) mbedtls_sha256_finish(ctx, out)
#endif

void CryptoHelper::deriveKey(const String &pass, uint8_t *hash)
{
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);

    SHA256_START(&ctx, 0);  // 0 = SHA-256, not SHA-224
    SHA256_UPDATE(&ctx, (const unsigned char*)pass.c_str(), pass.length());
    SHA256_FINISH(&ctx, hash);

    mbedtls_sha256_free(&ctx);
}


void CryptoHelper::aesEncrypt(const uint8_t *key,
                              const uint8_t *input, size_t len,
                              uint8_t *output, size_t &outLen) {
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);

    // PKCS7 padding
    size_t padLen = 16 - (len % 16);
    size_t paddedLen = len + padLen;

    if (paddedLen > 256 - 16) { // ensure output buffer will fit (16 bytes IV + ciphertext)
        outLen = 0;
        mbedtls_aes_free(&ctx);
        return;
    }

    uint8_t padded[256];
    memcpy(padded, input, len);
    for (size_t i = 0; i < padLen; i++) {
        padded[len + i] = padLen;
    }

    // Generate random IV
    uint8_t randIV[16];
    for (int i = 0; i < 16; i++) {
        randIV[i] = (uint8_t)random(0, 256);
    }

    // Copy IV to output first
    memcpy(output, randIV, 16);

    // Encrypt with CBC
    mbedtls_aes_setkey_enc(&ctx, key, 128);
    uint8_t ivCopy[16];
    memcpy(ivCopy, randIV, 16);

    int ret = mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_ENCRYPT, paddedLen, ivCopy, padded, output + 16);
    mbedtls_aes_free(&ctx);

    if (ret != 0) {
        outLen = 0;
        return;
    }

    // Total length = IV + ciphertext
    outLen = 16 + paddedLen;
}
/*
void CryptoHelper::aesEncrypt(const uint8_t *key, const uint8_t *iv,
                              const uint8_t *input, size_t len,
                              uint8_t *output, size_t &outLen) {
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);

  // PKCS7 padding
  size_t padLen = 16 - (len % 16);
  size_t paddedLen = len + padLen;
  uint8_t padded[256];
  memcpy(padded, input, len);
  for (size_t i = 0; i < padLen; i++) {
    padded[len + i] = padLen;
  }

  // Generate random IV
  uint8_t randIV[16];
  for (int i = 0; i < 16; i++) {
    randIV[i] = (uint8_t)random(0, 256);
  }

  // Copy IV to output first
  memcpy(output, randIV, 16);

  // Encrypt with CBC
  mbedtls_aes_setkey_enc(&ctx, key, 128);
  uint8_t ivCopy[16];
  memcpy(ivCopy, randIV, 16);
  mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_ENCRYPT, paddedLen, ivCopy,
                        padded, output + 16);

  mbedtls_aes_free(&ctx);

  outLen = paddedLen + 16; // ciphertext + IV
}
*/
bool CryptoHelper::aesDecrypt(const uint8_t *key,
                              const uint8_t *input, size_t len,
                              uint8_t *output, size_t &outLen) {
  // Must have at least IV + 1 block
  if (len < 16 + 16) return false; 

  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);

  // Extract IV
  uint8_t iv[16];
  memcpy(iv, input, 16);

  size_t cipherLen = len - 16;

  mbedtls_aes_setkey_dec(&ctx, key, 128);
  int ret = mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_DECRYPT,
                        cipherLen, iv, input + 16, output);

  mbedtls_aes_free(&ctx);

  if (ret != 0) return false;  // check for AES failure

  // Remove PKCS7 padding
  uint8_t padLen = output[cipherLen - 1];
  if (padLen == 0 || padLen > 16) return false;
  if (cipherLen < padLen) return false;

  outLen = cipherLen - padLen;
  output[outLen] = '\0'; // null-terminate for safe printing
  return true;
}
