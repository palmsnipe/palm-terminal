#include "palm.h"
#include <VFSMgr.h>

enum { RecordVersion = 1, HeaderBytes = 2, KeyHeaderBytes = PT_LABEL_MAX + PT_PUBLIC_MAX };
#define STORE_TYPE 'PtDt'
#define SEED_TYPE 'PtRn'

typedef struct {
    Char host[PT_HOST_MAX];
    Char port[6];
    uint8_t fingerprint[32];
} Pin;

static DmOpenRef open_database(const Char *name, UInt32 type, Boolean create)
{
    LocalID id = DmFindDatabase(0, name);
    if (!id && create && !DmCreateDatabase(0, name, PT_CREATOR, type, false))
        id = DmFindDatabase(0, name);
    if (!id) return NULL;
    UInt32 actual_type = 0, creator = 0;
    UInt16 attributes = 0;
    if (DmDatabaseInfo(0, id, NULL, &attributes, NULL, NULL, NULL, NULL, NULL, NULL,
                      NULL, &actual_type, &creator) || creator != PT_CREATOR ||
        actual_type != type) return NULL;
    if (type == SEED_TYPE && (attributes & dmHdrAttrBackup)) {
        attributes &= (UInt16)~dmHdrAttrBackup;
        if (DmSetDatabaseInfo(0, id, NULL, &attributes, NULL, NULL, NULL, NULL,
                              NULL, NULL, NULL, NULL, NULL)) return NULL;
    }
    return DmOpenDatabase(0, id, dmModeReadWrite);
}

DmOpenRef pt_store_open(void)
{
    return open_database(PT_STORE_NAME, STORE_TYPE, true);
}

static Boolean read_at(DmOpenRef db, UInt16 index, UInt8 type, void *out,
                       UInt16 length, UInt16 offset, UInt32 *record_size)
{
    MemHandle h = DmQueryRecord(db, index);
    if (!h) return false;
    UInt32 size = MemHandleSize(h);
    if (size < HeaderBytes + (UInt32)offset + length) return false;
    const uint8_t *p = MemHandleLock(h);
    if (!p) return false;
    Boolean valid = p[0] == RecordVersion && p[1] == type;
    if (valid && length) MemMove(out, p + HeaderBytes + offset, length);
    MemHandleUnlock(h);
    if (valid && record_size) *record_size = size;
    return valid;
}

Boolean pt_record_read(DmOpenRef db, UInt32 id, UInt8 type, void *out, UInt16 length)
{
    UInt16 index = 0;
    return id && !DmFindRecordByID(db, id, &index) &&
           read_at(db, index, type, out, length, 0, NULL);
}

UInt32 pt_record_find(DmOpenRef db, UInt8 type, UInt16 *index, void *head, UInt16 length)
{
    UInt16 count = DmNumRecords(db);
    while (*index < count) {
        UInt32 id = 0;
        UInt16 current = (*index)++;
        if (read_at(db, current, type, head, length, 0, NULL) &&
            !DmRecordInfo(db, current, NULL, &id, NULL)) return id;
    }
    return 0;
}

UInt32 pt_record_save(DmOpenRef db, UInt8 type, const void *a, UInt16 len_a,
                      const void *b, UInt16 len_b)
{
    UInt16 index = dmMaxRecordIndex;
    UInt32 id = 0;
    uint8_t header[HeaderBytes] = {RecordVersion, type};
    MemHandle h = DmNewRecord(db, &index, HeaderBytes + (UInt32)len_a + len_b);
    if (!h) return 0;
    void *p = MemHandleLock(h);
    Boolean ok = p && !DmWrite(p, HeaderBytes, a, len_a);
    if (ok && len_b) ok = !DmWrite(p, HeaderBytes + len_a, b, len_b);
    if (ok) ok = !DmWrite(p, 0, header, sizeof(header));
    if (p) MemHandleUnlock(h);
    if (DmReleaseRecord(db, index, ok)) return 0;
    if (ok) ok = !DmRecordInfo(db, index, NULL, &id, NULL);
    if (!ok) DmRemoveRecord(db, index);
    return id;
}

Boolean pt_record_remove(DmOpenRef db, UInt32 id, UInt8 type)
{
    UInt16 index = 0;
    return id && !DmFindRecordByID(db, id, &index) &&
           read_at(db, index, type, NULL, 0, 0, NULL) && !DmRemoveRecord(db, index);
}

static Boolean profile_valid(PtProfile *p)
{
    p->host[sizeof(p->host) - 1] = 0;
    p->user[sizeof(p->user) - 1] = 0;
    p->port[sizeof(p->port) - 1] = 0;
    return p->host[0] && p->user[0] && p->port[0];
}

Boolean pt_profile_read(DmOpenRef db, UInt32 id, PtProfile *profile)
{
    return pt_record_read(db, id, PT_RECORD_PROFILE, profile, sizeof(*profile)) &&
           profile_valid(profile);
}

UInt32 pt_profile_save(DmOpenRef db, const PtProfile *profile)
{
    PtProfile saved;
    UInt16 index = 0;
    UInt32 old;
    while ((old = pt_record_find(db, PT_RECORD_PROFILE, &index, &saved, sizeof(saved)))) {
        if (profile_valid(&saved) && !StrCaselessCompare(saved.host, profile->host) &&
            !StrCompare(saved.port, profile->port) && !StrCompare(saved.user, profile->user)) {
            if (saved.key == profile->key) return old;
            break;
        }
    }
    UInt32 created = pt_record_save(db, PT_RECORD_PROFILE, profile, sizeof(*profile), NULL, 0);
    if (created && old) pt_record_remove(db, old, PT_RECORD_PROFILE);
    return created;
}

UInt32 pt_pin_find(DmOpenRef db, const PtProfile *profile, uint8_t fingerprint[32])
{
    Pin saved;
    UInt16 index = 0;
    UInt32 id;
    while ((id = pt_record_find(db, PT_RECORD_PIN, &index, &saved, sizeof(saved)))) {
        saved.host[sizeof(saved.host) - 1] = 0;
        saved.port[sizeof(saved.port) - 1] = 0;
        if (!StrCaselessCompare(saved.host, profile->host) && !StrCompare(saved.port, profile->port)) {
            if (fingerprint) MemMove(fingerprint, saved.fingerprint, sizeof(saved.fingerprint));
            return id;
        }
    }
    return 0;
}

Boolean pt_pin_save(DmOpenRef db, const PtProfile *profile, const uint8_t fingerprint[32])
{
    Pin pin;
    if (pt_pin_find(db, profile, NULL)) return false;
    MemSet(&pin, sizeof(pin), 0);
    StrCopy(pin.host, profile->host);
    StrCopy(pin.port, profile->port);
    MemMove(pin.fingerprint, fingerprint, sizeof(pin.fingerprint));
    return pt_record_save(db, PT_RECORD_PIN, &pin, sizeof(pin), NULL, 0) != 0;
}

UInt32 pt_key_store(DmOpenRef db, const Char *label, PtKeyIO *key, UInt16 length)
{
    Char header[KeyHeaderBytes];
    if (!length || length > PT_KEY_SAVE_MAX || !key->public_key[0]) return 0;
    MemSet(header, sizeof(header), 0);
    StrNCopy(header, label, PT_LABEL_MAX - 1);
    StrNCopy(header + PT_LABEL_MAX, key->public_key, PT_PUBLIC_MAX - 1);
    return pt_record_save(db, PT_RECORD_KEY, header, sizeof(header), key->file, length);
}

Boolean pt_key_stored(DmOpenRef db, UInt32 id, Char label[PT_LABEL_MAX], Char pub[PT_PUBLIC_MAX])
{
    Char header[KeyHeaderBytes];
    if (!pt_record_read(db, id, PT_RECORD_KEY, header, sizeof(header))) return false;
    header[PT_LABEL_MAX - 1] = 0;
    header[sizeof(header) - 1] = 0;
    if (label) StrCopy(label, header);
    if (pub) StrCopy(pub, header + PT_LABEL_MAX);
    return true;
}

UInt16 pt_key_load(DmOpenRef db, UInt32 id, PtKeyIO *key)
{
    UInt16 index = 0;
    UInt32 total = 0;
    if (!id || DmFindRecordByID(db, id, &index) ||
        !read_at(db, index, PT_RECORD_KEY, NULL, 0, KeyHeaderBytes, &total)) return 0;
    UInt32 length = total - KeyHeaderBytes - HeaderBytes;
    if (!length || length > PT_KEY_SAVE_MAX ||
        !read_at(db, index, PT_RECORD_KEY, key->file, (UInt16)length, KeyHeaderBytes, NULL))
        return 0;
    return (UInt16)length;
}

static Boolean seed_read(DmOpenRef db, uint8_t *out)
{
    MemHandle h = DmQueryRecord(db, 0);
    if (!h || MemHandleSize(h) != PT_SEED_STATE + 4) return false;
    const uint8_t *p = MemHandleLock(h);
    if (!p) return false;
    Boolean valid = !MemCmp(p, "PTS1", 4);
    if (valid) MemMove(out, p + 4, PT_SEED_STATE);
    MemHandleUnlock(h);
    return valid;
}

static Boolean seed_write(DmOpenRef db, const uint8_t *state)
{
    UInt16 index = 0;
    MemHandle h;
    if (DmNumRecords(db)) {
        h = DmGetRecord(db, index);
        if (h && MemHandleSize(h) != PT_SEED_STATE + 4) {
            DmReleaseRecord(db, index, false);
            h = DmResizeRecord(db, index, PT_SEED_STATE + 4);
            if (!h) return false;
            h = DmGetRecord(db, index);
        }
    } else {
        h = DmNewRecord(db, &index, PT_SEED_STATE + 4);
    }
    if (!h) return false;
    void *p = MemHandleLock(h);
    /* Overwrite the only copy. An interrupted write must not select an old seed. */
    Boolean ok = p && !DmWrite(p, 4, state, PT_SEED_STATE) && !DmWrite(p, 0, "PTS1", 4);
    if (p) MemHandleUnlock(h);
    if (DmReleaseRecord(db, index, ok)) return false;
    uint8_t verify[PT_SEED_STATE];
    ok = ok && seed_read(db, verify) && !MemCmp(state, verify, sizeof(verify));
    pt_wipe(verify, sizeof(verify));
    return ok;
}

Boolean pt_seed_install(PtArm *arm, uint8_t seed[PT_SEED_SIZE])
{
    uint8_t packet[PT_SEED_SIZE + PT_SEED_STATE], old[PT_SEED_STATE];
    MemMove(packet, seed, PT_SEED_SIZE);
    DmOpenRef db = open_database(PT_SEED_NAME, SEED_TYPE, true);
    Boolean ok = db && pt_call(arm, PT_IMPORT_SEED, packet, 0, 0) == PT_OK &&
        (!seed_read(db, old) || MemCmp(old + 64, packet + PT_SEED_SIZE + 64, 32)) &&
        seed_write(db, packet + PT_SEED_SIZE);
    if (db && DmCloseDatabase(db)) ok = false;
    pt_wipe(packet, sizeof(packet));
    pt_wipe(old, sizeof(old));
    return ok;
}

Boolean pt_seed_reserve(PtArm *arm)
{
    uint8_t packet[sizeof(PtSeedIO) + PT_SEED_STATE];
    PtSeedIO *next = (PtSeedIO *)packet;
    DmOpenRef db = open_database(PT_SEED_NAME, SEED_TYPE, false);
    Boolean ok = db && seed_read(db, packet + sizeof(*next)) &&
                 pt_call(arm, PT_NEXT_SEED, next, 0, 0) == PT_OK &&
                 seed_write(db, next->next);
    if (db && DmCloseDatabase(db)) ok = false;
    if (ok) ok = pt_call(arm, PT_USE_SEED, next->working, 0, 0) == PT_OK;
    if (!ok) pt_call(arm, PT_FORGET_SEED, NULL, 0, 0);
    pt_wipe(packet, sizeof(packet));
    return ok;
}

static Boolean sd_volume(UInt16 *volume)
{
    UInt32 version, iterator = vfsIteratorStart;
    return !FtrGet(sysFileCVFSMgr, vfsFtrIDVersion, &version) &&
           !VFSVolumeEnumerate(volume, &iterator);
}

UInt16 pt_sd_read(const Char *path, void *out, UInt16 capacity)
{
    UInt16 volume;
    FileRef file;
    UInt32 size = 0, count = 0;
    if (!sd_volume(&volume) || VFSFileOpen(volume, path, vfsModeRead, &file)) return 0;
    Boolean ok = !VFSFileSize(file, &size) && size && size <= capacity &&
                 !VFSFileRead(file, size, out, &count) && count == size;
    return VFSFileClose(file) || !ok ? 0 : (UInt16)count;
}

Boolean pt_sd_create(const Char *path, const void *data, UInt16 length)
{
    UInt16 volume;
    FileRef file;
    if (!sd_volume(&volume) || VFSFileCreate(volume, path)) return false;
    UInt32 written;
    Boolean ok = !VFSFileOpen(volume, path, vfsModeWrite, &file);
    if (ok) {
        ok = !VFSFileWrite(file, length, data, &written) && written == length;
        if (VFSFileClose(file)) ok = false;
    }
    if (!ok) VFSFileDelete(volume, path);
    return ok;
}
