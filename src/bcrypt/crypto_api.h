#ifndef PT_BCRYPT_CRYPTO_API_H
#define PT_BCRYPT_CRYPTO_API_H
#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/sha512.h>
#define crypto_hash_sha512_BYTES 64
#define crypto_hash_sha512(out, in, length) wc_Sha512Hash((const byte *)(in), (word32)(length), out)
#include <wolfssl/wolfcrypt/hash.h>
#endif
