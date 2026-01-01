#include "syscall.h"

#define AT_FDCWD ((int64_t)-100)

/* st_mode file types (POSIX). */
#define S_IFMT 0170000u
#define S_IFDIR 0040000u

static int streq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static uint64_t cstr_len_u64_local(const char *s) {
    uint64_t n = 0;
    while (s && s[n] != '\0') n++;
    return n;
}

static int contains_char(const char *s, char c) {
    if (!s) return 0;
    for (uint64_t i = 0; s[i] != '\0'; i++) {
        if (s[i] == c) return 1;
    }
    return 0;
}

static const char *getenv_local(char **envp, const char *key) {
    if (!envp || !key) return 0;
    uint64_t klen = cstr_len_u64_local(key);
    for (uint64_t i = 0; envp[i]; i++) {
        const char *e = envp[i];
        if (!e) continue;
        uint64_t j = 0;
        while (j < klen && e[j] == key[j]) j++;
        if (j == klen && e[j] == '=') {
            return e + j + 1;
        }
    }
    return 0;
}

static int join_path(char *out, uint64_t cap, const char *base, const char *name) {
    if (!out || cap == 0 || !base || !name) return -1;
    uint64_t bl = cstr_len_u64_local(base);
    uint64_t nl = cstr_len_u64_local(name);
    if (bl == 0) return -1;

    int need_slash = 1;
    if (base[bl - 1] == '/') need_slash = 0;

    uint64_t total = bl + (uint64_t)need_slash + nl;
    if (total + 1 > cap) return -1;

    uint64_t o = 0;
    for (uint64_t i = 0; i < bl; i++) out[o++] = base[i];
    if (need_slash) out[o++] = '/';
    for (uint64_t i = 0; i < nl; i++) out[o++] = name[i];
    out[o] = '\0';
    return 0;
}

static int mode_is_dir(uint32_t mode) {
    return (mode & S_IFMT) == S_IFDIR;
}

static int exists_as_non_dir(const char *path) {
    linux_stat_t st;
    int64_t rc = (int64_t)sys_newfstatat((uint64_t)AT_FDCWD, path, &st, 0);
    if (rc < 0) return 0;
    if (mode_is_dir(st.st_mode)) return 0;
    return 1;
}

static void usage(void) {
    sys_puts("usage: which [-a] NAME...\n");
    sys_puts("       which -h|--help\n");
}

static int which_one(char **envp, const char *name, int opt_all) {
    if (!name || name[0] == '\0') return -1;

    int found = 0;

    if (contains_char(name, '/')) {
        if (exists_as_non_dir(name)) {
            sys_puts(name);
            sys_puts("\n");
            return 0;
        }
        return -1;
    }

    const char *path = getenv_local(envp, "PATH");
    if (!path || path[0] == '\0') {
        path = "/bin";
    }

    char full[256];

    uint64_t i = 0;
    for (;;) {
        /* Extract next PATH element. */
        char dir[128];
        uint64_t dlen = 0;
        while (path[i] != '\0' && path[i] != ':') {
            if (dlen + 1 < sizeof(dir)) dir[dlen++] = path[i];
            i++;
        }
        dir[dlen] = '\0';

        const char *dir_use = dir;
        if (dir_use[0] == '\0') {
            dir_use = ".";
        }

        if (join_path(full, sizeof(full), dir_use, name) == 0) {
            if (exists_as_non_dir(full)) {
                sys_puts(full);
                sys_puts("\n");
                found = 1;
                if (!opt_all) return 0;
            }
        }

        if (path[i] == ':') {
            i++;
            continue;
        }
        break;
    }

    return found ? 0 : -1;
}

int main(int argc, char **argv, char **envp) {
    int opt_all = 0;

    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!a) break;
        if (a[0] != '-') break;
        if (streq(a, "--")) {
            i++;
            break;
        }
        if (streq(a, "-h") || streq(a, "--help")) {
            usage();
            return 0;
        }
        if (streq(a, "-a")) {
            opt_all = 1;
            continue;
        }
        usage();
        return 2;
    }

    if (i >= argc) {
        usage();
        return 2;
    }

    int status = 0;
    for (; i < argc; i++) {
        const char *name = argv[i];
        if (!name || name[0] == '\0') continue;
        if (which_one(envp, name, opt_all) != 0) {
            status = 1;
        }
    }

    return status;
}
