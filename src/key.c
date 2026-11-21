#include "key.h"
#include <string.h>
#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/aes.h>
#include <wolfssl/wolfcrypt/asn_public.h>
#include <wolfssl/wolfcrypt/coding.h>
#include <wolfssl/wolfcrypt/ed25519.h>
#include <wolfssl/wolfcrypt/hash.h>

void bcrypt_hash(uint8_t *pass_hash, uint8_t *salt_hash, uint8_t *out);

static const char begin_key[] = "-----BEGIN OPENSSH PRIVATE KEY-----";
static const char end_key[] = "-----END OPENSSH PRIVATE KEY-----";
static const char magic[] = "openssh-key-v1";
static const char key_type[] = "ssh-ed25519";
static const char ctr_name[] = "aes256-ctr";
static const char gcm_name[] = "aes256-gcm@openssh.com";

typedef struct {
    const uint8_t *data;
    uint32_t length;
} Slice;

enum KeyMode { NoJob, ReadCtr, ReadGcm, SaveGcm };

typedef struct {
    enum KeyMode mode;
    Slice public, private, tag;
    uint8_t pass_hash[64], salt[64 + 4], hash[32], accumulator[32], material[48], check[4];
    uint32_t rounds, iteration, salt_size, block, need;
} KeyJob;

static KeyJob job;

void pt_key_cancel(void)
{
    pt_wipe(&job, sizeof(job));
}

static int derive_start(const char *password, Slice salt, uint32_t rounds, uint32_t need)
{
    if (salt.length > sizeof(job.salt) - 4 || !rounds || !password[0]) return PT_BAD_KEY;
    job.salt_size = salt.length;
    memcpy(job.salt, salt.data, salt.length);
    job.rounds = rounds;
    job.need = need;
    return wc_Sha512Hash((const uint8_t *)password, (word32)strlen(password), job.pass_hash)
           ? PT_BAD_KEY : PT_PROGRESS;
}

/* OpenSSH bcrypt_pbkdf with the same interleaved output, yielding between hashes. */
static int derive_step(void)
{
    uint8_t hash[64];
    int rc;
    if (!job.iteration) {
        job.salt[job.salt_size + 3] = (uint8_t)(job.block + 1);
        rc = wc_Sha512Hash(job.salt, job.salt_size + 4, hash);
    } else {
        rc = wc_Sha512Hash(job.hash, sizeof(job.hash), hash);
    }
    if (!rc) bcrypt_hash(job.pass_hash, hash, job.hash);
    pt_wipe(hash, sizeof(hash));
    if (rc) return PT_BAD_KEY;
    for (uint32_t i = 0; i < sizeof(job.hash); i++) job.accumulator[i] ^= job.hash[i];
    if (++job.iteration < job.rounds) return PT_PROGRESS;
    uint32_t stride = (job.need + sizeof(job.hash) - 1) / sizeof(job.hash);
    for (uint32_t i = 0; job.block + i * stride < job.need; i++)
        job.material[job.block + i * stride] = job.accumulator[i];
    if (++job.block == stride) return PT_OK;
    job.iteration = 0;
    pt_wipe(job.accumulator, sizeof(job.accumulator));
    return PT_PROGRESS;
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8 | p[3];
}

static int number(Slice *input, uint32_t *value)
{
    if (input->length < 4) return -1;
    *value = get32(input->data);
    input->data += 4;
    input->length -= 4;
    return 0;
}

static int string(Slice *input, Slice *output)
{
    if (number(input, &output->length) || output->length > input->length) return -1;
    output->data = input->data;
    input->data += output->length;
    input->length -= output->length;
    return 0;
}

static int is_string(Slice input, const char *name)
{
    return input.length == strlen(name) && !memcmp(input.data, name, input.length);
}

static void put32(uint8_t **p, uint32_t v)
{
    (*p)[0] = (uint8_t)(v >> 24);
    (*p)[1] = (uint8_t)(v >> 16);
    (*p)[2] = (uint8_t)(v >> 8);
    (*p)[3] = (uint8_t)v;
    *p += 4;
}

static void put_string(uint8_t **p, const void *data, uint32_t size)
{
    put32(p, size);
    memcpy(*p, data, size);
    *p += size;
}

static void put_text(uint8_t **p, const char *s)
{
    put_string(p, s, (uint32_t)strlen(s));
}

static uint32_t public_blob(const PtIdentity *key, uint8_t blob[51])
{
    uint8_t *p = blob;
    put_text(&p, key_type);
    put_string(&p, key->public, 32);
    return (uint32_t)(p - blob);
}

static int space(uint8_t ch)
{
    return ch == '\r' || ch == '\n' || ch == ' ' || ch == '\t';
}

static int unarmor(PtKeyIO *io, uint32_t size, uint32_t *length)
{
    const uint32_t begin = sizeof(begin_key) - 1, end = sizeof(end_key) - 1;
    if (size > PT_KEY_FILE_MAX) return -1;
    while (size && space(io->file[size - 1])) size--;
    if (size < begin + end + 2 || memcmp(io->file, begin_key, begin) ||
        memcmp(io->file + size - end, end_key, end)) return -1;
    uint32_t count = 0;
    for (uint32_t i = begin; i < size - end; i++)
        if (!space(io->file[i])) io->file[count++] = io->file[i];
    word32 decoded = PT_KEY_FILE_MAX;
    int rc = Base64_Decode(io->file, count, io->file, &decoded);
    *length = decoded;
    return rc;
}

static int check_key(PtIdentity *key, Slice private, Slice public, unsigned block)
{
    Slice type, pub, pair, comment;
    uint32_t first, second;
    if (number(&private, &first) || number(&private, &second) || first != second ||
        string(&private, &type) || !is_string(type, key_type) ||
        string(&private, &pub) || pub.length != 32 ||
        string(&private, &pair) || pair.length != 64 ||
        memcmp(pair.data + 32, pub.data, 32) ||
        string(&private, &comment) || private.length >= block) return -1;
    for (uint32_t i = 0; i < private.length; i++)
        if (private.data[i] != i + 1) return -1;

    uint8_t expected[51];
    memcpy(key->seed, pair.data, 32);
    memcpy(key->public, pub.data, 32);
    if (public.length != public_blob(key, expected) ||
        memcmp(public.data, expected, public.length)) return -1;

    ed25519_key ed;
    int rc = wc_ed25519_init(&ed);
    uint8_t derived[32];
    if (!rc) rc = wc_ed25519_import_private_only(key->seed, 32, &ed);
    if (!rc) rc = wc_ed25519_make_public(&ed, derived, sizeof(derived));
    if (!rc && memcmp(derived, key->public, sizeof(derived))) rc = -1;
    wc_ed25519_free(&ed);
    pt_wipe(&ed, sizeof(ed));
    return rc;
}

int pt_key_read(PtIdentity *key, PtKeyIO *io, uint32_t size)
{
    uint32_t decoded, count, rounds;
    Slice cipher, kdf, opts, public, private, salt;
    int rc = PT_BAD_KEY;
    pt_key_cancel();
    if (unarmor(io, size, &decoded) || decoded < sizeof(magic) ||
        memcmp(io->file, magic, sizeof(magic))) goto done;
    Slice input = {io->file + sizeof(magic), decoded - sizeof(magic)};
    if (string(&input, &cipher) || string(&input, &kdf) || string(&input, &opts) ||
        number(&input, &count) || count != 1 || string(&input, &public) ||
        string(&input, &private) || !private.length) goto done;

    if (is_string(cipher, "none") && is_string(kdf, "none") && !opts.length &&
        !input.length && private.length % 8 == 0) {
        rc = check_key(key, private, public, 8) ? PT_BAD_KEY : PT_OK;
        goto done;
    }

    int gcm = is_string(cipher, gcm_name);
    if ((!gcm && !is_string(cipher, ctr_name)) || !is_string(kdf, "bcrypt") ||
        input.length != (gcm ? 16u : 0u) || private.length % 16 ||
        string(&opts, &salt) || salt.length < 16 || salt.length > 64 ||
        number(&opts, &rounds) || opts.length || !rounds || rounds > 128) goto done;

    job.mode = gcm ? ReadGcm : ReadCtr;
    job.public = public;
    job.private = private;
    job.tag = input;
    rc = derive_start(io->password, salt, rounds, gcm ? 44 : 48);

done:
    pt_wipe(io->password, sizeof(io->password));
    if (rc != PT_PROGRESS) {
        pt_wipe(io->file, sizeof(io->file));
        pt_key_cancel();
    }
    if (rc < 0) pt_wipe(key, sizeof(*key));
    return rc;
}

static int finish_read(PtIdentity *key, PtKeyIO *io)
{
    Aes aes;
    int rc = wc_AesInit(&aes, NULL, INVALID_DEVID);
    int initialized = !rc;
    uint8_t *payload = io->file + (job.private.data - io->file);
    if (!rc && job.mode == ReadGcm) {
        rc = wc_AesGcmSetKey(&aes, job.material, 32);
        if (!rc) rc = wc_AesGcmDecrypt(&aes, payload, payload, job.private.length,
                        job.material + 32, 12, job.tag.data, job.tag.length, NULL, 0);
    } else if (!rc) {
        rc = wc_AesSetKey(&aes, job.material, 32, job.material + 32, AES_ENCRYPTION);
        if (!rc) rc = wc_AesCtrEncrypt(&aes, payload, payload, job.private.length);
    }
    if (initialized) wc_AesFree(&aes);
    pt_wipe(&aes, sizeof(aes));
    if (!rc) rc = check_key(key, job.private, job.public, 16);
    pt_wipe(io->file, sizeof(io->file));
    if (rc) pt_wipe(key, sizeof(*key));
    return rc ? PT_BAD_KEY : PT_OK;
}

int pt_key_generate(PtIdentity *key)
{
    ed25519_key ed;
    int rc = wc_ed25519_init(&ed);
    if (!rc) rc = pt_random(key->seed, 32);
    if (!rc) rc = wc_ed25519_import_private_only(key->seed, 32, &ed);
    if (!rc) rc = wc_ed25519_make_public(&ed, key->public, 32);
    if (rc) pt_wipe(key, sizeof(*key));
    wc_ed25519_free(&ed);
    pt_wipe(&ed, sizeof(ed));
    return rc;
}

int pt_key_der(const PtIdentity *key, uint8_t *der, uint32_t capacity)
{
    ed25519_key ed;
    int rc = wc_ed25519_init(&ed);
    if (!rc) rc = wc_ed25519_import_private_key(key->seed, 32, key->public, 32, &ed);
    if (!rc) rc = wc_Ed25519KeyToDer(&ed, der, capacity);
    wc_ed25519_free(&ed);
    pt_wipe(&ed, sizeof(ed));
    return rc;
}

void pt_key_public(const PtIdentity *key, PtKeyIO *io)
{
    uint8_t blob[51];
    uint32_t size = public_blob(key, blob);
    memcpy(io->public_key, key_type, sizeof(key_type) - 1);
    io->public_key[sizeof(key_type) - 1] = ' ';
    word32 encoded = PT_PUBLIC_MAX - sizeof(key_type) - 2;
    if (Base64_Encode_NoNl(blob, size, (uint8_t *)io->public_key + sizeof(key_type), &encoded)) {
        io->public_key[0] = 0;
        return;
    }
    memcpy(io->public_key + sizeof(key_type) + encoded, "\n", 2);
}

int pt_key_save(const PtIdentity *key, PtKeyIO *io)
{
    (void)key;
    uint8_t salt[16];
    pt_key_cancel();
    int rc = pt_random(salt, sizeof(salt));
    if (!rc) rc = pt_random(job.check, sizeof(job.check));
    if (!rc) {
        Slice source = {salt, sizeof(salt)};
        rc = derive_start(io->password, source, 32, 44);
    }
    pt_wipe(io->password, sizeof(io->password));
    pt_wipe(salt, sizeof(salt));
    job.mode = SaveGcm;
    if (rc != PT_PROGRESS) pt_key_cancel();
    return rc;
}

static int finish_save(const PtIdentity *key, PtKeyIO *io)
{
    uint8_t raw[384], private[160], options[24];
    uint8_t pub[51], pair[64], encoded[512];
    int rc = 0;
    uint8_t *p = private;
    put32(&p, get32(job.check));
    put32(&p, get32(job.check));
    put_text(&p, key_type);
    put_string(&p, key->public, 32);
    memcpy(pair, key->seed, 32);
    memcpy(pair + 32, key->public, 32);
    put_string(&p, pair, sizeof(pair));
    put_text(&p, "Terminal");
    for (uint8_t i = 1; (p - private) % 16; i++) *p++ = i;
    uint32_t private_size = (uint32_t)(p - private);
    p = options;
    put_string(&p, job.salt, job.salt_size);
    put32(&p, job.rounds);

    p = raw;
    memcpy(p, magic, sizeof(magic));
    p += sizeof(magic);
    put_text(&p, gcm_name);
    put_text(&p, "bcrypt");
    put_string(&p, options, sizeof(options));
    put32(&p, 1);
    uint32_t public_size = public_blob(key, pub);
    put_string(&p, pub, public_size);
    put32(&p, private_size);
    Aes aes;
    int initialized = 0;
    if (!rc) {
        rc = wc_AesInit(&aes, NULL, INVALID_DEVID);
        initialized = !rc;
    }
    if (!rc) rc = wc_AesGcmSetKey(&aes, job.material, 32);
    if (!rc) rc = wc_AesGcmEncrypt(&aes, p, private, private_size, job.material + 32, 12,
                                 p + private_size, 16, NULL, 0);
    if (initialized) wc_AesFree(&aes);
    p += private_size + 16;
    word32 length = sizeof(encoded);
    if (!rc) rc = Base64_Encode_NoNl(raw, (word32)(p - raw), encoded, &length);
    int result = -1;
    if (!rc) {
        p = io->file;
        memcpy(p, begin_key, sizeof(begin_key) - 1);
        p += sizeof(begin_key) - 1;
        *p++ = '\n';
        for (word32 offset = 0; offset < length;) {
            word32 take = length - offset > 70 ? 70 : length - offset;
            memcpy(p, encoded + offset, take);
            offset += take;
            p += take;
            *p++ = '\n';
        }
        memcpy(p, end_key, sizeof(end_key) - 1);
        p += sizeof(end_key) - 1;
        *p++ = '\n';
        result = (int)(p - io->file);
        pt_key_public(key, io);
    }
    pt_wipe(&aes, sizeof(aes));
    pt_wipe(raw, sizeof(raw));
    pt_wipe(private, sizeof(private));
    pt_wipe(pair, sizeof(pair));
    return result;
}

int pt_key_step(PtIdentity *key, PtKeyIO *io)
{
    if (job.mode == NoJob) return PT_BAD_KEY;
    int rc = derive_step();
    if (rc == PT_PROGRESS) return rc;
    if (rc == PT_OK)
        rc = job.mode == SaveGcm ? finish_save(key, io) : finish_read(key, io);
    pt_key_cancel();
    return rc;
}
