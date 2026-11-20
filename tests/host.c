/* Host harness for the exact protocol and key code linked into the ARM image. */
#include "native.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>

static void check(int ok, const char *problem)
{
    if (ok) return;
    fprintf(stderr, "%s\n", problem);
    exit(1);
}

static uint32_t read_key(const char *path, PtKeyIO *io)
{
    FILE *f = fopen(path, "rb");
    check(f != NULL, "Could not open key fixture");
    size_t size = fread(io->file, 1, sizeof(io->file), f);
    check(size && feof(f) && !fclose(f), "Invalid key fixture size");
    return (uint32_t)size;
}

static void seed_for_test(void)
{
    uint8_t original[PT_SEED_SIZE + PT_SEED_STATE] = {21, 62, 1, 33};
    uint8_t step[sizeof(PtSeedIO) + PT_SEED_STATE], first[32], second[32];
    PtSeedIO *result = (PtSeedIO *)step;
    check(pt_random(first, sizeof(first)) != 0, "Seedless RNG returned success");
    check(pt_dispatch(PT_IMPORT_SEED, original, 0, 0) == PT_OK, "Seed fixture failed");
    memcpy(step + sizeof(*result), original + PT_SEED_SIZE, PT_SEED_STATE);
    step[sizeof(step) - 1] ^= 1;
    check(pt_dispatch(PT_NEXT_SEED, step, 0, 0) != PT_OK, "Bad seed checksum was accepted");
    step[sizeof(step) - 1] ^= 1;
    check(pt_dispatch(PT_NEXT_SEED, step, 0, 0) == PT_OK, "RNG state advance failed");
    check(pt_dispatch(PT_USE_SEED, result->working, 0, 0) == PT_OK &&
          pt_random(first, sizeof(first)) == 0, "First working seed failed");
    memcpy(step + sizeof(*result), result->next, PT_SEED_STATE);
    check(pt_dispatch(PT_NEXT_SEED, step, 0, 0) == PT_OK, "Second state advance failed");
    check(pt_dispatch(PT_USE_SEED, result->working, 0, 0) == PT_OK &&
          pt_random(second, sizeof(second)) == 0 && memcmp(first, second, sizeof(first)),
          "RNG output repeated across state advances");
}

static void key_test(int n, const char *const *args)
{
    PtKeyIO io = {0};
    int generate = !strcmp(args[1], "generate");
    check(n == (generate ? 4 : 6), "Usage: generate out pass | import in pass out new-pass");
    const char *output = args[generate ? 2 : 4];
    if (generate) {
        check(pt_dispatch(PT_KEY_GENERATE, &io, 0, 0) == PT_OK, "Key generation failed");
    } else {
        uint32_t size = read_key(args[2], &io);
        snprintf(io.password, sizeof(io.password), "%s", args[3]);
        check(pt_dispatch(PT_KEY_IMPORT, &io, size, 0) == PT_OK, "Key import failed");
    }
    snprintf(io.password, sizeof(io.password), "%s", args[generate ? 3 : 5]);
    int32_t size = pt_dispatch(PT_KEY_SAVE, &io, 0, 0);
    check(size > 0, "Could not protect key");
    FILE *f = fopen(output, "wb");
    check(f && !fchmod(fileno(f), 0600) && fwrite(io.file, 1, size, f) == (size_t)size &&
          !fclose(f), "Could not write protected key");
    fputs(io.public_key, stdout);
}

static int dial(unsigned port)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in remote = {.sin_family = AF_INET, .sin_port = htons(port),
                                 .sin_addr = {htonl(INADDR_LOOPBACK)}};
    check(sock >= 0 && !connect(sock, (struct sockaddr *)&remote, sizeof(remote)),
          "Could not reach local sshd");
    check(!fcntl(sock, F_SETFL, O_NONBLOCK), "Could not configure TCP");
    return sock;
}

static int exercise(int sock, unsigned port, PtLogin *login, PtKeyIO *key, const char *file)
{
    uint8_t data[PT_IO_CHUNK], details[36], row[PT_GRID_COLS * PT_CELL_BYTES];
    size_t pending = 0, sent = 0, received = 0, fed = 0;
    int phase = 0;
    check(pt_dispatch(PT_CONNECT, login, 0, 0) == PT_OK, "SSH setup failed");
    uint8_t input[PT_IO_CHUNK];
    time_t deadline = time(NULL) + 25;
    while (time(NULL) <= deadline) {
        if (pending == sent) {
            pending = (size_t)pt_dispatch(PT_DRAIN, data, sizeof(data), 0);
            sent = 0;
        }
        if (pending > sent) {
            ssize_t count = write(sock, data + sent, pending - sent);
            if (count > 0) sent += (size_t)count;
        }
        if (received == fed) {
            ssize_t count = read(sock, input, sizeof(input));
            if (count > 0) {
                received = (size_t)count;
                fed = 0;
            } else if (!count) {
                close(sock);
                return 0;
            }
        }
        if (received > fed)
            fed += (size_t)pt_dispatch(PT_FEED, input + fed, (uint32_t)(received - fed), 0);
        int32_t status = pt_dispatch(PT_STEP, details, 0, 0);
        if (status == PT_UNKNOWN_HOST) {
            check(!login->pinned, "Repeated unknown host");
            memcpy(login->fingerprint, details + 4, 32);
            login->pinned = 1;
            close(sock);
            pt_dispatch(PT_STOP, NULL, 0, 0);
            seed_for_test();
            if (login->key_auth) {
                uint32_t count = read_key(file, key);
                check(pt_dispatch(PT_KEY_UNLOCK, key, count, 0) == PT_OK,
                      "Could not unlock connection key");
            }
            sock = dial(port);
            check(pt_dispatch(PT_CONNECT, login, 0, 0) == PT_OK, "Pinned SSH setup failed");
            received = fed = sent = pending = 0;
        } else if (status == PT_CONNECTED) {
            const char command[] = "stty size; yes data | head -c 100000; printf '\\033[2J\\033[Hverified-%s\\n' shell";
            for (const char *p = command; *p; ++p)
                check(pt_dispatch(PT_INPUT, NULL, 0, (uint8_t)*p) == PT_OK, "Terminal input failed");
            pt_dispatch(PT_INPUT, NULL, 0, PT_ENTER);
            phase = 1;
        } else if (status == PT_PROGRESS && phase == 1) {
            if (pt_dispatch(PT_ROW, row, sizeof(row), 0) == PT_OK) {
                char decoded[PT_GRID_COLS + 1] = {0};
                for (unsigned col = 0; col < login->grid.cols; col++)
                    decoded[col] = (char)pt_le32(row + col * PT_CELL_BYTES);
                if (!strncmp(decoded, "verified-shell", 14)) {
                    close(sock);
                    pt_dispatch(PT_STOP, NULL, 0, 0);
                    puts("Verified interactive shell and bulk output");
                    return 1;
                }
            }
        } else if (status < 0) {
            fprintf(stderr, "SSH operation failed (%d/%d)\n", status, (int32_t)pt_le32(details));
            break;
        }
        if (status == PT_IDLE) {
            struct pollfd ready = {sock, POLLIN, 0};
            poll(&ready, 1, 1);
        }
    }
    close(sock);
    return 0;
}

int main(int argc, char **argv)
{
    check(argc >= 2, "Specify generate, import, or shell");
    check(pt_dispatch(PT_BOOT, NULL, 0, 0) == PT_OK, "Native backend initialization failed");
    seed_for_test();
    if (strcmp(argv[1], "shell")) {
        key_test(argc, (const char *const *)argv);
        return 0;
    }
    check(argc == 6, "Usage: shell port user key-file-or-dash secret");
    unsigned port = (unsigned)strtoul(argv[2], NULL, 10);
    PtLogin login = {.appearance = pt_appearance_default(), .grid = {29, 53}};
    PtKeyIO key = {0};
    snprintf(login.user, sizeof(login.user), "%s", argv[3]);
    login.key_auth = strcmp(argv[4], "-") != 0;
    if (login.key_auth) snprintf(key.password, sizeof(key.password), "%s", argv[5]);
    else snprintf(login.password, sizeof(login.password), "%s", argv[5]);
    check(exercise(dial(port), port, &login, &key, argv[4]), "Shell did not reach verification text");
    return 0;
}
