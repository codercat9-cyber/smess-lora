/*
 * Тот же клиент, что на STM32, но на Linux: узел Meshtastic по USB,
 * консоль — терминал. Нужен, чтобы гонять ядро на живом узле без платы.
 *
 *   ./build/smess-lora [-v] [-k файл] [/dev/ttyUSB0]
 *   -v — показывать текстовый журнал узла
 *   -k — где держать свой ключ (по умолчанию ~/.config/smess-lora/key,
 *        права 600; в открытой сборке не нужен)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "app.h"

static int radio_fd = -1;
static int verbose;

static void radio_write(const uint8_t *d, size_t n) {
    while (n) {
        ssize_t w = write(radio_fd, d, n);
        if (w < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            perror("запись в узел");
            exit(1);
        }
        d += w;
        n -= (size_t)w;
    }
    tcdrain(radio_fd);
}

static void console_write(const char *s) {
    fputs(s, stdout);
    fflush(stdout);
}

static uint32_t millis(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000);
}

static uint32_t random32(void) {
    uint32_t v = 0;
    if (getrandom(&v, sizeof v, 0) != sizeof v) v = millis() * 2654435761u;
    return v;
}

/* --- для модуля защиты: случайность ядра и файл ключа --- */

static char key_path[512];

static void entropy(uint8_t *buf, size_t n) {
    while (n) {
        ssize_t r = getrandom(buf, n, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("getrandom");
            exit(1);
        }
        buf += r;
        n -= (size_t)r;
    }
}

static bool secret_load(uint8_t *buf, size_t n) {
    FILE *f = fopen(key_path, "rb");
    if (!f) return false;
    bool ok = fread(buf, 1, n, f) == n;
    fclose(f);
    return ok;
}

static bool secret_store(const uint8_t *buf, size_t n) {
    char dir[512];
    snprintf(dir, sizeof dir, "%s", key_path);
    char *sl = strrchr(dir, '/');
    if (sl && sl != dir) {
        *sl = 0;
        mkdir(dir, 0700); /* ~/.config/smess-lora; есть — не страшно */
    }
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", key_path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    bool ok = write(fd, buf, n) == (ssize_t)n && fsync(fd) == 0;
    close(fd);
    if (ok) ok = rename(tmp, key_path) == 0;
    if (!ok) unlink(tmp);
    return ok;
}

static void radio_log(uint8_t b) {
    if (verbose) fputc(b, stderr);
}

static int open_serial(const char *path) {
    int fd = open(path, O_RDWR | O_NOCTTY);
    if (fd < 0) return -1;
    struct termios t;
    if (tcgetattr(fd, &t) < 0) return -1;
    cfmakeraw(&t);
    cfsetispeed(&t, B115200);
    cfsetospeed(&t, B115200);
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cflag &= ~(tcflag_t)HUPCL; /* не дёргать DTR при закрытии — ESP32 от этого перезагружается */
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &t) < 0) return -1;
    return fd;
}

int main(int argc, char **argv) {
    const char *port = "/dev/ttyUSB0";
    const char *home = getenv("HOME");
    snprintf(key_path, sizeof key_path, "%s/.config/smess-lora/key", home ? home : ".");
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "-k") && i + 1 < argc) snprintf(key_path, sizeof key_path, "%s", argv[++i]);
        else port = argv[i];
    }
    radio_fd = open_serial(port);
    if (radio_fd < 0) {
        fprintf(stderr, "Не открыть %s: %s\n", port, strerror(errno));
        return 1;
    }
    /* Открытие порта перезагружает плату (линии DTR/RTS); даём ей встать */
    usleep(2500 * 1000);
    tcflush(radio_fd, TCIFLUSH);

    static const smesh_crypto_platform cp = { entropy, secret_load, secret_store, NULL };
    static const smess_platform plat = { radio_write, console_write, millis, random32, radio_log, false, &cp };
    app_init(&plat);

    for (;;) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(radio_fd, &rd);
        FD_SET(STDIN_FILENO, &rd);
        struct timeval tv = { 0, 50 * 1000 };
        int r = select(radio_fd + 1, &rd, NULL, NULL, &tv);
        if (r < 0 && errno != EINTR) {
            perror("select");
            return 1;
        }
        if (r > 0 && FD_ISSET(radio_fd, &rd)) {
            uint8_t buf[512];
            ssize_t n = read(radio_fd, buf, sizeof buf);
            if (n < 0 && errno != EAGAIN && errno != EINTR) {
                fprintf(stderr, "Узел отключился: %s\n", strerror(errno));
                return 1;
            }
            for (ssize_t i = 0; i < n; i++) app_radio_byte(buf[i]);
        }
        if (r > 0 && FD_ISSET(STDIN_FILENO, &rd)) {
            char buf[256];
            ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
            if (n == 0) return 0; /* конец ввода */
            for (ssize_t i = 0; i < n; i++) app_console_char(buf[i]);
        }
        app_poll();
    }
}
