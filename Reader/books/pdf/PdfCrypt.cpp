#include "PdfCrypt.h"
#include <cstring>
#include <vector>

namespace pdf {
namespace crypt {

/////////////////////////////////////////////
// MD5 (RFC 1321)
namespace {

inline uint32_t rol32(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }
inline uint32_t ror32(uint32_t x, int c) { return (x >> c) | (x << (32 - c)); }
inline uint64_t ror64(uint64_t x, int c) { return (x >> c) | (x << (64 - c)); }

void md5Block(uint32_t h[4], const uint8_t* p)
{
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
    static const uint8_t R[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21 };
    uint32_t w[16];
    for (int i = 0; i < 16; i++)
        w[i] = p[i * 4] | (p[i * 4 + 1] << 8) | (p[i * 4 + 2] << 16) | ((uint32_t)p[i * 4 + 3] << 24);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    for (int i = 0; i < 64; i++)
    {
        uint32_t f; int g;
        if (i < 16)      { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d;          g = (3 * i + 5) & 15; }
        else             { f = c ^ (b | ~d);       g = (7 * i) & 15; }
        uint32_t t = d; d = c; c = b;
        b = b + rol32(a + f + K[i] + w[g], R[i]);
        a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
}

} // namespace

void md5(const uint8_t* data, size_t len, uint8_t out[16])
{
    uint32_t h[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
    size_t i = 0;
    for (; i + 64 <= len; i += 64) md5Block(h, data + i);
    uint8_t buf[128] = { 0 };
    size_t rem = len - i;
    memcpy(buf, data + i, rem);
    buf[rem] = 0x80;
    size_t total = rem < 56 ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int k = 0; k < 8; k++) buf[total - 8 + k] = (uint8_t)(bits >> (8 * k));
    md5Block(h, buf);
    if (total == 128) md5Block(h, buf + 64);
    for (int k = 0; k < 4; k++)
        for (int j = 0; j < 4; j++) out[k * 4 + j] = (uint8_t)(h[k] >> (8 * j));
}

/////////////////////////////////////////////
// SHA-256 / SHA-384 / SHA-512 (FIPS 180-4)
namespace {

void sha256Block(uint32_t h[8], const uint8_t* p)
{
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | (p[i * 4 + 1] << 16) | (p[i * 4 + 2] << 8) | p[i * 4 + 3];
    for (int i = 16; i < 64; i++)
    {
        uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++)
    {
        uint32_t t1 = hh + (ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void sha512Block(uint64_t h[8], const uint8_t* p)
{
    static const uint64_t K[80] = {
        0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL,
        0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL, 0x12835b0145706fbeULL,
        0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL, 0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL,
        0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
        0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL, 0x983e5152ee66dfabULL,
        0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
        0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL,
        0x53380d139d95b3dfULL, 0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
        0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL,
        0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL, 0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL,
        0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL,
        0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
        0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL, 0xca273eceea26619cULL,
        0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL,
        0x113f9804bef90daeULL, 0x1b710b35131c471bULL, 0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL,
        0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL };
    uint64_t w[80];
    for (int i = 0; i < 16; i++)
    {
        uint64_t v = 0;
        for (int k = 0; k < 8; k++) v = (v << 8) | p[i * 8 + k];
        w[i] = v;
    }
    for (int i = 16; i < 80; i++)
    {
        uint64_t s0 = ror64(w[i - 15], 1) ^ ror64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        uint64_t s1 = ror64(w[i - 2], 19) ^ ror64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint64_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 80; i++)
    {
        uint64_t t1 = hh + (ror64(e, 14) ^ ror64(e, 18) ^ ror64(e, 41)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint64_t t2 = (ror64(a, 28) ^ ror64(a, 34) ^ ror64(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void sha512Core(uint64_t h[8], const uint8_t* data, size_t len)
{
    size_t i = 0;
    for (; i + 128 <= len; i += 128) sha512Block(h, data + i);
    uint8_t buf[256] = { 0 };
    size_t rem = len - i;
    memcpy(buf, data + i, rem);
    buf[rem] = 0x80;
    size_t total = rem < 112 ? 128 : 256;
    uint64_t bits = (uint64_t)len * 8;
    for (int k = 0; k < 8; k++) buf[total - 1 - k] = (uint8_t)(bits >> (8 * k));
    sha512Block(h, buf);
    if (total == 256) sha512Block(h, buf + 128);
}

} // namespace

void sha256(const uint8_t* data, size_t len, uint8_t out[32])
{
    uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    size_t i = 0;
    for (; i + 64 <= len; i += 64) sha256Block(h, data + i);
    uint8_t buf[128] = { 0 };
    size_t rem = len - i;
    memcpy(buf, data + i, rem);
    buf[rem] = 0x80;
    size_t total = rem < 56 ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int k = 0; k < 8; k++) buf[total - 1 - k] = (uint8_t)(bits >> (8 * k));
    sha256Block(h, buf);
    if (total == 128) sha256Block(h, buf + 64);
    for (int k = 0; k < 8; k++)
        for (int j = 0; j < 4; j++) out[k * 4 + j] = (uint8_t)(h[k] >> (24 - 8 * j));
}

void sha384(const uint8_t* data, size_t len, uint8_t out[48])
{
    uint64_t h[8] = { 0xcbbb9d5dc1059ed8ULL, 0x629a292a367cd507ULL, 0x9159015a3070dd17ULL, 0x152fecd8f70e5939ULL,
                      0x67332667ffc00b31ULL, 0x8eb44a8768581511ULL, 0xdb0c2e0d64f98fa7ULL, 0x47b5481dbefa4fa4ULL };
    sha512Core(h, data, len);
    for (int k = 0; k < 6; k++)
        for (int j = 0; j < 8; j++) out[k * 8 + j] = (uint8_t)(h[k] >> (56 - 8 * j));
}

void sha512(const uint8_t* data, size_t len, uint8_t out[64])
{
    uint64_t h[8] = { 0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
                      0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL };
    sha512Core(h, data, len);
    for (int k = 0; k < 8; k++)
        for (int j = 0; j < 8; j++) out[k * 8 + j] = (uint8_t)(h[k] >> (56 - 8 * j));
}

/////////////////////////////////////////////
// RC4
void rc4(const uint8_t* key, size_t keyLen, uint8_t* data, size_t len)
{
    if (!keyLen) return;
    uint8_t s[256];
    for (int i = 0; i < 256; i++) s[i] = (uint8_t)i;
    for (int i = 0, j = 0; i < 256; i++)
    {
        j = (j + s[i] + key[i % keyLen]) & 0xff;
        uint8_t t = s[i]; s[i] = s[j]; s[j] = t;
    }
    for (size_t n = 0, i = 0, j = 0; n < len; n++)
    {
        i = (i + 1) & 0xff;
        j = (j + s[i]) & 0xff;
        uint8_t t = s[i]; s[i] = s[j]; s[j] = t;
        data[n] ^= s[(s[i] + s[j]) & 0xff];
    }
}

/////////////////////////////////////////////
// AES (FIPS 197), byte oriented. Small rather than fast.
namespace {

const uint8_t SBOX[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16 };

struct Aes
{
    uint8_t rk[240];
    int rounds;
    uint8_t inv[256];

    Aes(const uint8_t* key, size_t keyLen)
    {
        for (int i = 0; i < 256; i++) inv[SBOX[i]] = (uint8_t)i;
        int nk = (int)(keyLen / 4);
        rounds = nk + 6;
        int total = 4 * (rounds + 1);
        memcpy(rk, key, keyLen);
        uint8_t rcon = 1;
        for (int i = nk; i < total; i++)
        {
            uint8_t t[4];
            memcpy(t, rk + (i - 1) * 4, 4);
            if (i % nk == 0)
            {
                uint8_t u = t[0];
                t[0] = SBOX[t[1]] ^ rcon; t[1] = SBOX[t[2]]; t[2] = SBOX[t[3]]; t[3] = SBOX[u];
                rcon = xt(rcon);
            }
            else if (nk > 6 && i % nk == 4)
            {
                for (int k = 0; k < 4; k++) t[k] = SBOX[t[k]];
            }
            for (int k = 0; k < 4; k++) rk[i * 4 + k] = rk[(i - nk) * 4 + k] ^ t[k];
        }
    }

    static uint8_t xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }
    static uint8_t mul(uint8_t a, uint8_t b)
    {
        uint8_t r = 0;
        while (b) { if (b & 1) r ^= a; a = xt(a); b >>= 1; }
        return r;
    }

    void encrypt(uint8_t s[16]) const
    {
        for (int k = 0; k < 16; k++) s[k] ^= rk[k];
        for (int r = 1; r <= rounds; r++)
        {
            for (int k = 0; k < 16; k++) s[k] = SBOX[s[k]];
            uint8_t t[16];
            for (int c = 0; c < 4; c++)
                for (int row = 0; row < 4; row++) t[c * 4 + row] = s[((c + row) & 3) * 4 + row];
            if (r != rounds)
            {
                for (int c = 0; c < 4; c++)
                {
                    uint8_t* col = t + c * 4;
                    uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                    col[0] = xt(a0) ^ (xt(a1) ^ a1) ^ a2 ^ a3;
                    col[1] = a0 ^ xt(a1) ^ (xt(a2) ^ a2) ^ a3;
                    col[2] = a0 ^ a1 ^ xt(a2) ^ (xt(a3) ^ a3);
                    col[3] = (xt(a0) ^ a0) ^ a1 ^ a2 ^ xt(a3);
                }
            }
            for (int k = 0; k < 16; k++) s[k] = t[k] ^ rk[r * 16 + k];
        }
    }

    void decrypt(uint8_t s[16]) const
    {
        for (int k = 0; k < 16; k++) s[k] ^= rk[rounds * 16 + k];
        for (int r = rounds - 1; r >= 0; r--)
        {
            uint8_t t[16];
            for (int c = 0; c < 4; c++)
                for (int row = 0; row < 4; row++) t[((c + row) & 3) * 4 + row] = s[c * 4 + row];
            for (int k = 0; k < 16; k++) s[k] = inv[t[k]] ^ rk[r * 16 + k];
            if (r != 0)
            {
                for (int c = 0; c < 4; c++)
                {
                    uint8_t* col = s + c * 4;
                    uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                    col[0] = mul(a0, 14) ^ mul(a1, 11) ^ mul(a2, 13) ^ mul(a3, 9);
                    col[1] = mul(a0, 9) ^ mul(a1, 14) ^ mul(a2, 11) ^ mul(a3, 13);
                    col[2] = mul(a0, 13) ^ mul(a1, 9) ^ mul(a2, 14) ^ mul(a3, 11);
                    col[3] = mul(a0, 11) ^ mul(a1, 13) ^ mul(a2, 9) ^ mul(a3, 14);
                }
            }
        }
    }
};

} // namespace

void aesCbcEncrypt(const uint8_t* key, size_t keyLen, const uint8_t iv[16], uint8_t* data, size_t len)
{
    Aes aes(key, keyLen);
    uint8_t prev[16];
    memcpy(prev, iv, 16);
    for (size_t i = 0; i + 16 <= len; i += 16)
    {
        for (int k = 0; k < 16; k++) data[i + k] ^= prev[k];
        aes.encrypt(data + i);
        memcpy(prev, data + i, 16);
    }
}

void aesCbcDecrypt(const uint8_t* key, size_t keyLen, const uint8_t iv[16], uint8_t* data, size_t len)
{
    Aes aes(key, keyLen);
    uint8_t prev[16], cur[16];
    memcpy(prev, iv, 16);
    for (size_t i = 0; i + 16 <= len; i += 16)
    {
        memcpy(cur, data + i, 16);
        aes.decrypt(data + i);
        for (int k = 0; k < 16; k++) data[i + k] ^= prev[k];
        memcpy(prev, cur, 16);
    }
}

bool aesDecryptPdf(const uint8_t* key, size_t keyLen, const std::string& in, std::string& out)
{
    out.clear();
    if (in.size() < 32) return in.size() == 16; // only an IV: empty string
    size_t len = (in.size() - 16) & ~(size_t)15;
    out.assign(in, 16, len);
    aesCbcDecrypt(key, keyLen, (const uint8_t*)in.data(), (uint8_t*)&out[0], len);
    uint8_t pad = (uint8_t)out.back();
    if (pad >= 1 && pad <= 16 && pad <= out.size()) out.resize(out.size() - pad);
    return true;
}

} // namespace crypt
} // namespace pdf
