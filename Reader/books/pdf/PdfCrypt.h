/**
 * @file PdfCrypt.h
 * @brief Minimal crypto primitives for the PDF standard security handler.
 *
 * Only what is needed to open PDFs protected with an empty user password
 * (permission-only "owner password" files, very common for e-books):
 * MD5, RC4, AES-128/256 (CBC), SHA-256/384/512.
 * No dependency on the OS crypto API so the same code runs on Windows XP.
 */

#pragma once
#include <cstdint>
#include <cstddef>
#include <string>

namespace pdf {
namespace crypt {

void md5(const uint8_t* data, size_t len, uint8_t out[16]);
void sha256(const uint8_t* data, size_t len, uint8_t out[32]);
void sha384(const uint8_t* data, size_t len, uint8_t out[48]);
void sha512(const uint8_t* data, size_t len, uint8_t out[64]);

// RC4 is symmetric: the same call encrypts and decrypts in place.
void rc4(const uint8_t* key, size_t keyLen, uint8_t* data, size_t len);

// AES-CBC without padding handling; len must be a multiple of 16.
void aesCbcEncrypt(const uint8_t* key, size_t keyLen, const uint8_t iv[16], uint8_t* data, size_t len);
void aesCbcDecrypt(const uint8_t* key, size_t keyLen, const uint8_t iv[16], uint8_t* data, size_t len);

// PDF AES stream/string decryption: first 16 bytes are the IV, PKCS#5 padded.
bool aesDecryptPdf(const uint8_t* key, size_t keyLen, const std::string& in, std::string& out);

} // namespace crypt
} // namespace pdf
