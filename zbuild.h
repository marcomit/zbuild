/*
 * zbuild.h - a tiny build system for C. Public API.
 *
 * Bootstrap once:  cc -o build build.c zbuild.c
 * Then just run:   ./build
 */
#ifndef ZBUILD_H
#define ZBUILD_H

#include <stddef.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdbool.h>

#if defined(_WIN32)
#   include <windows.h>
#   include <dirent.h>
#   define MKDIR(path) _mkdir(path)
#   define popen _popen
#   define pclose _pclose
#else
#   include <unistd.h>
#   include <sys/stat.h>
#   define MKDIR(path) mkdir(path, 0755)
#endif
#ifdef __linux__
#   include <sched.h>
#endif

#include "zvec.h"

#define VFORMAT() ({                                                            \
    va_list ap;                                                                 \
    va_start(ap, fmt);                                                          \
    const char *formatted = vformat(fmt, ap);                                   \
    va_end(ap);                                                                 \
    formatted;                                                                  \
})

typedef struct ZCmd   ZCmd;     /* one command (argv + files + deps) */
typedef struct ZBuild ZBuild;   /* a set of commands                 */

typedef enum {
    Z_BUILD_DONE,
    Z_BUILD_CYCLE,
    Z_BUILD_NOOP,
    Z_BUILD_GENERIC_ERROR,
} ZBuildState;

struct ZBuild {
    vec(ZCmd *) cmds;
    size_t max_jobs;
    bool show_command;
    bool show_output;
    bool show_skipped_command;
};

#define Z_CMD_END_MASK (1 << 0x8)

typedef enum {
    Z_CMD_PENDING,
    Z_CMD_RUNNING,
    Z_CMD_DONE      = Z_CMD_END_MASK,
    Z_CMD_FAILED,
    Z_CMD_SKIPPED
} ZCmdState;

struct ZCmd {
    vec(const char *)   argv;
    vec(const char *)   inputs;
    vec(const char *)   outputs;
    vec(ZCmd *)         after;
    size_t               remaining;
    ZCmdState           state;
};

static inline void zb_cmd_free(ZCmd *);
static inline int is_out_of_date(ZCmd *);
static inline char *zb_get_cmd(ZCmd *);

static inline int cpu_count() {
#if defined(_WIN32)
    DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return n > 0 ? (int)n : 1;
#elif defined(__linux__)
    cpu_set_t set;
    if (sched_getaffinity(0, sizeof set, &set) == 0) {
        int n = CPU_COUNT(&set);
        if (n > 0) return n;
    }
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
#endif
}

/* Create a build. jobs = max commands running at once (0 = number of CPUs). */
static inline ZBuild *zb_new(int jobs) {
    ZBuild *self = (ZBuild *)malloc(sizeof(ZBuild));
    *self = (ZBuild){ 0 };
    self->max_jobs = jobs == 0 ? cpu_count() : jobs;
    return self;
}

/*
 * It checks whether the dependency-graph has a cycle or not.
 * Checked by the topological sort.
 * Tipically the TS needs a `color` field of three variants (white, gray, blakc)
 * but here the command has the state which can be used as a color:
 * PENDING  = white -> not yet visited
 * RUNNING  = gray  -> visiting
 * DONE     = black -> visited
 * */
static inline int zb_has_cycle(ZCmd *cmd) {
    if (cmd->state == Z_CMD_DONE) return 0;
    if (cmd->state == Z_CMD_RUNNING) return 1;
    cmd->state = Z_CMD_RUNNING;

    for (size_t i = 0; i < veclen(cmd->after); i++)
        if (zb_has_cycle(cmd->after[i]))
            return 1;

    cmd->state = Z_CMD_DONE;
    return 0;
}

static inline int zb_run_cmd(ZBuild *b, ZCmd *c) {
    if (c->state & Z_CMD_END_MASK) return 0;
    c->state = Z_CMD_RUNNING;

    size_t i = 0;
    for (; i < veclen(c->after); i++) {
        if (!is_out_of_date(c->after[i])) continue;
        if (!zb_run_cmd(b, c->after[i])) {
            c->state = Z_CMD_SKIPPED;
            break;
        }
    }

    if (c->state == Z_CMD_SKIPPED) {
        for (; i < veclen(c->after); i++) {
            c->after[i]->state = Z_CMD_SKIPPED;
        }
    }

    if (c->state != Z_CMD_RUNNING) {
        if (b->show_skipped_command) {
            printf("Command %s skipped\n", zb_get_cmd(c));
        }
        return 0;
    }
    const char *cmd = zb_get_cmd(c);
    if (b->show_command) {
        printf("[zb]: %s\n", cmd);
    }

    int code = system(cmd);
    if (code != 0) {
        c->state = Z_CMD_FAILED;
        return 0;
    } else {
        c->state = Z_CMD_DONE;
        return 1;
    }
}

static inline void zb_set_all_pendings(ZBuild *b) {
    for (size_t i = 0; i < veclen(b->cmds); i++)
        b->cmds[i]->state = Z_CMD_PENDING;
}

/* Run every out-of-date command, in parallel where dependencies allow.
 * Returns 0 on success, 1 if a command failed or there is a cycle. */
static inline ZBuildState zb_run(ZBuild *b) {
    zb_set_all_pendings(b);

    for (size_t i = 0; i < veclen(b->cmds); i++)
        if (zb_has_cycle(b->cmds[i]))
            return Z_BUILD_CYCLE;

    zb_set_all_pendings(b);

    int flag = 0;
    for (size_t i = 0; i < veclen(b->cmds); i++) {
        if (is_out_of_date(b->cmds[i])) {
            if (!zb_run_cmd(b, b->cmds[i])) {
                return Z_BUILD_GENERIC_ERROR;
            }
            flag = 1;
        } else if (b->show_skipped_command) {
            printf("Command %s is out of date\n", zb_get_cmd(b->cmds[i]));
        }
    }

    return flag ? Z_BUILD_DONE : Z_BUILD_NOOP;
}

/* Delete every output file and depfile. */
static inline void zb_clean(ZBuild *b) {
    for (size_t i = 0; i < veclen(b->cmds); i++) {
         ZCmd *c = b->cmds[i];
         for (size_t j = 0; j < veclen(c->outputs); j++) {
            remove(c->outputs[j]);
        }
    }
}

/* Free the build and all its commands. */
static inline void zb_free(ZBuild *b) {
    for (size_t i = 0; i < veclen(b->cmds); i++)
        zb_cmd_free(b->cmds[i]);
    free(b);
}

/* ---- Commands -------------------------------------------------------- */

// Last modification time in nanoseconds since 1970-01-01, or -1 if the file doesn't exist.
static inline long long file_mtime(const char *path) {
#if defined(_WIN32)
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &d)) return -1;
    // FILETIME = 100 ns ticks since 1601-01-01
    long long t = ((long long)d.ftLastWriteTime.dwHighDateTime << 32)
                | d.ftLastWriteTime.dwLowDateTime;
    return (t - 116444736000000000LL) * 100;   // shift to 1970, convert to ns
#else
    struct stat st;
    if (stat(path, &st) != 0) return -1;
#if defined(__APPLE__)
    return (long long)st.st_mtimespec.tv_sec * 1000000000LL + st.st_mtimespec.tv_nsec;
#else
    return (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
#endif
#endif
}

/* Checks if the command is out-of-date.
 * A command is out of date if there's an input file
 * which is newer than any output:
 * conceptually: newest input < oldest input
 * */
static inline int is_out_of_date(ZCmd *cmd) {
    size_t in_len = veclen(cmd->inputs);
    size_t out_len = veclen(cmd->outputs);

    if (in_len == 0 || out_len == 0) return 0;

    long long newest_input  = file_mtime(cmd->inputs[0]);
    long long oldest_output = file_mtime(cmd->outputs[0]);

    for (size_t i = 1; i < in_len; i++) {
        long long cur = file_mtime(cmd->inputs[i]);
        if (cur > newest_input) newest_input = cur;
    }

    for (size_t i = 1; i < out_len; i++) {
        long long cur = file_mtime(cmd->outputs[i]);
        if (cur < oldest_output) oldest_output = cur;
    }

    if (newest_input <= oldest_output) return 0;
    return 1;
}

/* Create a command and add it to the build. `program` becomes argv[0]. */
static inline ZCmd *zb_cmd(ZBuild *b, const char *program) {
    ZCmd *self = (ZCmd *)malloc(sizeof(ZCmd));
    *self = (ZCmd){ 0 };
    vecpush(self->argv, strdup(program));
    vecpush(b->cmds, self);
    return self;
}

/* Get the outputs of a command. */
static inline vec(const char *) zb_cmd_outputs(ZCmd *c) {
    return c->outputs;
}

static inline vec(const char *) zm_cmd_inputs(ZCmd *c) {
    return c->inputs;
}

static inline void zb_cmd_free(ZCmd *cmd) {
    #define freelist(l) for (size_t i = 0; i < veclen(l); i++) free((void *)l[i]);
    freelist(cmd->argv);
    freelist(cmd->inputs);
    freelist(cmd->outputs);
    #undef freelist

    for (size_t i = 0; i < veclen(cmd->after); i++) {
        zb_cmd_free(cmd->after[i]);
        cmd->after[i] = NULL;
    }

    vecfree(cmd->argv);
    vecfree(cmd->inputs);
    vecfree(cmd->outputs);
    vecfree(cmd->after);

    free(cmd);
}

static inline char *vformat(const char *fmt, va_list args) {
    va_list copy;
    va_copy(copy, args);
    int len = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (len < 0) return NULL;

    char *out = (char *)malloc((size_t)len + 1);
    if (out) vsnprintf(out, (size_t)len + 1, fmt, args);
    return out;
}

#define zb_args(c, ...) _zb_args(c, __VA_ARGS__, NULL)
static inline void _zb_args(ZCmd *c, ...) {
    va_list ap;
    const char *arg;
    va_start(ap, c);

    while (( arg = va_arg(ap, const char *)))
        vecpush(c->argv, strdup(arg));

    va_end(ap);
}

/* Append ONE argument built printf-style: zb_argf(c, "-DVERSION=%d", 3); */
static inline void zb_arg(ZCmd *c, const char *fmt, ...) {
    const char *arg = VFORMAT();
    vecpush(c->argv, arg);
}

/* ---- Files ----------------------------------------------------------- */

/* Record a file without adding an argument (headers, side outputs...). */
static inline void zb_input (ZCmd *c, const char *fmt, ...) {
    const char *path = VFORMAT();
    vecpush(c->inputs, strdup(path));
}

static inline void zb_output(ZCmd *c, const char *fmt, ...) {
    const char *path = VFORMAT();
    vecpush(c->outputs, strdup(path));
}


/* Append `path` as an argument AND record it as an input / output. */
static inline void zb_in(ZCmd *c, const char *fmt, ...) {
    const char *path = VFORMAT();
    vecpush(c->argv, strdup(path));
    vecpush(c->inputs, strdup(path));
}

static inline void zb_out(ZCmd *c, const char *fmt, ...) {
    const char *path = VFORMAT();
    vecpush(c->argv, strdup(path));
    vecpush(c->outputs, strdup(path));

}

/* A gcc/clang depfile (-MMD -MF path); headers listed in it become inputs. */
static inline void zb_depfile(ZCmd *c, const char *path) {
    (void)c;
    (void)path;
}

static inline char *zb_get_cmd(ZCmd *cmd) {
    char *out = NULL;
    for (size_t i = 0; i < veclen(cmd->argv); i++) {
        if (i != 0) vecpush(out, ' ');
        size_t arglen = strlen(cmd->argv[i]);
        vecunion(out, cmd->argv[i], arglen);
    }
    vecpush(out, '\0');
    return out;
}

/* ---- Ordering -------------------------------------------------------- */

/* Run c after dep. Usually not needed: if c has an input that another
 * command lists as an output, that edge is added automatically. */
static inline void zb_after(ZCmd *c, ZCmd *dep) {
    vecpush(c->after, dep);
}

/* ---- Self rebuild ---------------------------------------------------- */

/* Put at the top of main(): if build.c, zbuild.c or zbuild.h is newer than
 * the running executable, recompile it and restart with the same arguments. */
#define ZB_REBUILD_SELF(argc, argv) zb__rebuild_self((argc), (argv), __FILE__)
static inline void zb__rebuild_self(int argc, char **argv, const char *build_src) {
    (void)argc;
    ZBuild *b = zb_new(0);
    b->show_command = true;

    ZCmd *c = zb_cmd(b, "clang");
    zb_args(c, "-O2", "-Wall", "-Wextra");
    zb_in(c, build_src);
    zb_arg(c, "-o");
    zb_out(c, argv[0]);

    ZBuildState state = zb_run(b);
    if (state == Z_BUILD_NOOP) return;
    else printf("Rebuilding...\n");
    if (state != Z_BUILD_DONE) exit(1);
    execvp(argv[0], argv);
    perror("execv");
    exit(1);

}

/* ---- Utilities ------------------------------------------------------- */

static inline vec(char) zb_shell(const char *cmd) {
    vec(char) out = NULL;
    char line[512];
    FILE *f = popen(cmd, "r");

    if (!f) return NULL;

    while (fgets(line, sizeof line, f)) {
        size_t linelen = strlen(line);
        size_t outlen = veclen(out);
        vecreserve(out, linelen);
        vecsetlen(out, outlen + linelen);
        memcpy(out + outlen, line, linelen);
    }
    pclose(f);
    return out;
}

static inline bool direxists(const char *path) {
#ifdef _WIN32
#   define STAT_STRUCT struct _stat
#   define STAT_FUNC _stat
#   define IS_DIR(m) (((m) & _S_IFDIR) != 0)
#else
#   define STAT_STRUCT struct stat
#   define STAT_FUNC stat
#   define IS_DIR(m) S_ISDIR(m)
#endif

    STAT_STRUCT info;
    if (STAT_FUNC(path, &info) != 0) {
        return false;
    }
    return IS_DIR(info.st_mode);
#undef STAT_FUNC
#undef STAT_STRUCT
#undef IS_DIR
}

static inline const char *zb_mkdir(const char *path) {
    if (!path || !*path) return path;

    char *copied = strdup(path);
    if (!copied) return NULL;

    char *s = copied;
    while (true) {
        char *slash = strpbrk(s, "/\\");
        if (!slash) break;

        char orig = *slash;
        *slash = '\0';

        bool is_drive = (slash - copied == 2 && copied[1] == ':');
        if (*copied != '\0' && !is_drive && !direxists(copied)) {
            MKDIR(copied);
        }

        *slash = orig;
        s = slash + 1;
    }

    if (*copied != '\0' && !direxists(copied)) {
        MKDIR(copied);
    }

    free(copied);
    return path;
}

#endif /* ZBUILD_H */
