/**
 * webos_inject: tvservice プロセスを自動検出し、ASLR/FW 非依存で共有ライブラリ (.so) を注入する
 * dladdr + /proc/<pid>/maps により libc/libdl のオフセットを動的計算し、固定アドレスに依存しない。
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#define __USE_GNU 1
#include <dlfcn.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/user.h>

#ifndef PTRACE_SET_SYSCALL
#define PTRACE_SET_SYSCALL 23
#endif

/** 指定 PID の maps から指定ライブラリ名を含む最初の実行可能/マップ先頭アドレスを取得する */
static uint32_t find_lib_base(pid_t pid, const char *lib_substr, char *matched_path, size_t path_sz) {
    char maps_path[64];
    if (pid <= 0)
        snprintf(maps_path, sizeof(maps_path), "/proc/self/maps");
    else
        snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", (int)pid);

    FILE *f = fopen(maps_path, "r");
    if (!f) return 0;

    char line[512];
    uint32_t base = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, lib_substr)) continue;
        unsigned long start = 0, end = 0, offset = 0;
        char perms[8] = {0}, dev[16] = {0}, path[256] = {0};
        unsigned long inode = 0;
        int n = sscanf(line, "%lx-%lx %7s %lx %15s %lu %255s",
                       &start, &end, perms, &offset, dev, &inode, path);
        if (n >= 7 && offset == 0) {
            base = (uint32_t)start;
            if (matched_path && path_sz > 0) {
                strncpy(matched_path, path, path_sz - 1);
                matched_path[path_sz - 1] = '\0';
            }
            break;
        }
    }
    fclose(f);
    return base;
}

/** 対象プロセス内で既に指定の .so がロード済みか確認する */
static int is_already_loaded(pid_t pid, const char *so_name) {
    char maps_path[64];
    snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", (int)pid);
    FILE *f = fopen(maps_path, "r");
    if (!f) return 0;
    char line[512];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, so_name)) {
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

/** 自プロセス内シンボルのオフセットから対象プロセス側の関数アドレスを計算する */
static uint32_t resolve_remote_func(pid_t pid, const char *lib_soname, const char *sym_name) {
    void *h = dlopen(lib_soname, RTLD_LAZY | RTLD_NOLOAD);
    if (!h) h = dlopen(lib_soname, RTLD_LAZY);
    void *sym = h ? dlsym(h, sym_name) : dlsym(RTLD_DEFAULT, sym_name);
    if (!sym) return 0;

    Dl_info info;
    if (!dladdr(sym, &info) || !info.dli_fname || !info.dli_fbase) return 0;

    const char *base_name = strrchr(info.dli_fname, '/');
    base_name = base_name ? (base_name + 1) : info.dli_fname;

    uint32_t local_addr = (uint32_t)(uintptr_t)sym;
    uint32_t local_base = (uint32_t)(uintptr_t)info.dli_fbase;
    uint32_t off = local_addr - local_base;

    /* 対象プロセスの maps から同一ベース名のファイルを検索（例: libc-2.24.so / libdl-2.24.so） */
    uint32_t remote_base = find_lib_base(pid, base_name, NULL, 0);
    if (!remote_base) {
        /* プレフィックス部分のみでも試す（例: "libc" / "libdl"） */
        char prefix[64];
        snprintf(prefix, sizeof(prefix), "%s", base_name);
        char *dot = strchr(prefix, '-');
        if (dot) *dot = '\0';
        else {
            dot = strchr(prefix, '.');
            if (dot) *dot = '\0';
        }
        remote_base = find_lib_base(pid, prefix, NULL, 0);
    }
    if (!remote_base) {
        fprintf(stderr, "resolve_remote_func(%s, %s): base_name=%s not found in remote maps\n",
                lib_soname, sym_name, base_name);
        return 0;
    }
    return remote_base + off;
}

/** コマンドラインから tvservice プロセスを検索する */
static pid_t find_tvservice_pid(void) {
    DIR *d = opendir("/proc");
    if (!d) return -1;
    struct dirent *ent;
    pid_t found = -1;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] < '0' || ent->d_name[0] > '9') continue;
        pid_t pid = (pid_t)atoi(ent->d_name);
        char path[64], cmd[512];
        snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)pid);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        size_t n = fread(cmd, 1, sizeof(cmd) - 1, f);
        fclose(f);
        if (n == 0) continue;
        for (size_t i = 0; i + 1 < n; i++) {
            if (cmd[i] == '\0') cmd[i] = ' ';
        }
        cmd[n] = '\0';
        if (strstr(cmd, "lg.service.special.broadcast") != NULL ||
            strstr(cmd, "/mnt/lg/lgapp/RELEASE") != NULL) {
            found = pid;
            break;
        }
    }
    closedir(d);
    return found;
}

static int write_mem_ptrace(pid_t pid, uint32_t addr, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    size_t i = 0;
    while (i < len) {
        long word = 0;
        size_t chunk = (len - i >= 4) ? 4 : (len - i);
        if (chunk < 4) {
            errno = 0;
            word = ptrace(PTRACE_PEEKDATA, pid, (void *)(uintptr_t)(addr + i), NULL);
            if (errno != 0) return -1;
        }
        memcpy(&word, p + i, chunk);
        if (ptrace(PTRACE_POKEDATA, pid, (void *)(uintptr_t)(addr + i), (void *)word) != 0)
            return -1;
        i += 4;
    }
    return 0;
}

static int read_mem_ptrace(pid_t pid, uint32_t addr, void *data, size_t len) {
    uint8_t *p = (uint8_t *)data;
    size_t i = 0;
    while (i < len) {
        errno = 0;
        long word = ptrace(PTRACE_PEEKDATA, pid, (void *)(uintptr_t)(addr + i), NULL);
        if (errno != 0) return -1;
        size_t chunk = (len - i >= 4) ? 4 : (len - i);
        memcpy(p + i, &word, chunk);
        i += 4;
    }
    return 0;
}

/** 対象プロセス内で関数を呼び出す（Thumb/ARM 両対応、LR=0xdead0000 トラップ利用） */
static int call_remote(pid_t pid, uint32_t func_addr, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3, uint32_t *ret_out) {
    struct user_regs regs, saved;
    if (ptrace(PTRACE_GETREGS, pid, NULL, &saved) != 0) {
        perror("PTRACE_GETREGS");
        return -1;
    }
    memcpy(&regs, &saved, sizeof(regs));
    ptrace(PTRACE_SET_SYSCALL, pid, 0, (void *)-1);

    uint32_t sp = (saved.uregs[13] - 64) & ~7U;
    regs.uregs[0] = r0;
    regs.uregs[1] = r1;
    regs.uregs[2] = r2;
    regs.uregs[3] = r3;
    regs.uregs[13] = sp;
    regs.uregs[14] = 0xdead0000;
    regs.uregs[15] = func_addr & ~1U;
    if (func_addr & 1U)
        regs.uregs[16] |= (1U << 5);
    else
        regs.uregs[16] &= ~(1U << 5);
    regs.uregs[17] = (uint32_t)-1;

    if (ptrace(PTRACE_SETREGS, pid, NULL, &regs) != 0) {
        perror("PTRACE_SETREGS");
        return -1;
    }
    if (ptrace(PTRACE_CONT, pid, NULL, NULL) != 0) {
        perror("PTRACE_CONT");
        return -1;
    }

    int status = 0;
    while (waitpid(pid, &status, 0) > 0) {
        if (WIFSTOPPED(status)) {
            int sig = WSTOPSIG(status);
            struct user_regs cur;
            ptrace(PTRACE_GETREGS, pid, NULL, &cur);
            if (sig == SIGSEGV && (cur.uregs[15] & ~1U) == 0xdead0000) {
                if (ret_out) *ret_out = cur.uregs[0];
                ptrace(PTRACE_SETREGS, pid, NULL, &saved);
                return 0;
            }
            if (sig != SIGSEGV && sig != SIGILL && sig != SIGBUS && sig != SIGABRT) {
                /* 他シグナル（例: タイマー等）はそのまま対象に配送して継続 */
                ptrace(PTRACE_CONT, pid, NULL, (void *)(uintptr_t)sig);
                continue;
            }
            fprintf(stderr, "Unexpected stop sig=%d pc=0x%08lx r0=0x%08lx lr=0x%08lx\n",
                    sig, cur.uregs[15], cur.uregs[0], cur.uregs[14]);
            ptrace(PTRACE_SETREGS, pid, NULL, &saved);
            return -1;
        }
        break;
    }
    return -1;
}

int main(int argc, char **argv) {
    const char *so_path = NULL;
    pid_t pid = -1;

    if (argc == 2) {
        if (argv[1][0] >= '0' && argv[1][0] <= '9')
            pid = (pid_t)atoi(argv[1]);
        else
            so_path = argv[1];
    } else if (argc >= 3) {
        pid = (pid_t)atoi(argv[1]);
        so_path = argv[2];
    }

    if (pid <= 0) pid = find_tvservice_pid();
    if (pid <= 0) {
        fprintf(stderr, "tvservice process not found\n");
        return 1;
    }
    if (!so_path) {
        fprintf(stderr, "Usage: %s [pid] <so_path>\n", argv[0]);
        return 1;
    }

    const char *so_base = strrchr(so_path, '/');
    so_base = so_base ? (so_base + 1) : so_path;
    if (is_already_loaded(pid, so_base)) {
        printf("Already loaded %s in pid %d\n", so_base, (int)pid);
        return 0;
    }

    uint32_t r_malloc  = resolve_remote_func(pid, "libc.so.6", "malloc");
    uint32_t r_free    = resolve_remote_func(pid, "libc.so.6", "free");
    uint32_t r_dlopen  = resolve_remote_func(pid, "libdl.so.2", "dlopen");
    uint32_t r_dlerror = resolve_remote_func(pid, "libdl.so.2", "dlerror");
    if (!r_malloc || !r_free || !r_dlopen) {
        fprintf(stderr, "Failed to resolve remote funcs: malloc=0x%x free=0x%x dlopen=0x%x\n",
                r_malloc, r_free, r_dlopen);
        return 1;
    }

    if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) != 0) {
        perror("PTRACE_ATTACH");
        return 1;
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        ptrace(PTRACE_DETACH, pid, NULL, NULL);
        return 1;
    }

    size_t slen = strlen(so_path) + 1;
    uint32_t remote_buf = 0;
    int rc = 0;
    if (call_remote(pid, r_malloc, (uint32_t)((slen + 15) & ~15U), 0, 0, 0, &remote_buf) != 0 || remote_buf == 0) {
        fprintf(stderr, "Remote malloc failed\n");
        rc = 1;
    } else {
        write_mem_ptrace(pid, remote_buf, so_path, slen);
        uint32_t handle = 0;
        if (call_remote(pid, r_dlopen, remote_buf, RTLD_NOW, 0, 0, &handle) == 0 && handle != 0) {
            printf("Injected %s into pid %d (handle=0x%08x)\n", so_path, (int)pid, handle);
        } else {
            rc = 1;
            fprintf(stderr, "Remote dlopen failed\n");
            if (r_dlerror) {
                uint32_t err_ptr = 0;
                if (call_remote(pid, r_dlerror, 0, 0, 0, 0, &err_ptr) == 0 && err_ptr != 0) {
                    char errbuf[256] = {0};
                    read_mem_ptrace(pid, err_ptr, errbuf, sizeof(errbuf) - 1);
                    fprintf(stderr, "dlerror: %s\n", errbuf);
                }
            }
        }
        call_remote(pid, r_free, remote_buf, 0, 0, 0, NULL);
    }

    ptrace(PTRACE_DETACH, pid, NULL, NULL);
    return rc;
}
