#include "zvec.h"
#include "zbuild.h"

#define LLVM_VERSION "22"

static vec(const char *) CFLAGS         = NULL;
static vec(const char *) INCLUDES       = NULL;
static vec(const char *) ZMODE_FLAGS    = NULL;
static const char *SANITIZE             = NULL;
#define SRC_DIR "src"
#define LIB_DIR "lib"

static void init_cflags() {
    vecunion(CFLAGS, ((const char *[]){
        "-g", "-Wall", "-Wdeprecated-declarations", "-O2"
    }), 4);

    if (SANITIZE) vecpush(CFLAGS, SANITIZE);
    vecunion(CFLAGS, INCLUDES, veclen(INCLUDES));
}

static void zinc_lsp(ZBuild *b, int argc, char *argv[]) {
    vec(const char *) lsp_files = zb_walk(argv[2], (ZBWalkFilter){
        .skip = "src/codegen", .suffix = ".c"
    });

    for (size_t i = 0; i < veclen(lsp_files); i++)
        printf("%s\n", lsp_files[i]);
}

static void zinc_all(ZBuild *b, int argc, char *argv[]) {
    vec(const char *) files = zb_walk("../zinc/src", (ZBWalkFilter){
        .skip = "src/lsp", .suffix = ".c"
    });

    printf("all files %zu\n", veclen(files));
    for (size_t i = 0; i < veclen(files); i++)
        printf("%s\n", files[i]);
}

typedef struct {
    const char *name;
    void (*fn) (ZBuild *, int, char *[]);
} Command;

static const Command Commands[] = {
    { "all",    zinc_all        },
    { "lsp",    zinc_lsp        },
    { NULL,     NULL            },
};

static bool run_command(ZBuild *b, int argc, char *argv[]) {
    const Command *cmds = Commands;
    while (cmds->name && cmds->fn) {
        if (strcmp(cmds->name, argv[1]) == 0) break;
        cmds++;
    }
    if (!cmds->name || !cmds->fn) return false;
    if (cmds->fn) cmds->fn(b, argc, argv);
    return true;
}

int main(int argc, char *argv[]) {
    ZB_REBUILD_SELF(argc, argv);
    ZBuild *b = zb_new(0);

    vecunion(INCLUDES, ((const char *[]){
        "-I", "include", "-I", "lib"
    }), 4);

    init_cflags();

    if (!run_command(b, argc, argv)) {
        return 1;
        printf("Command %s unrecognized\n", argv[1]);
    }
    

    switch (zb_run(b)) {
    case Z_BUILD_GENERIC_ERROR:
        printf("error\n");
        break;
    case Z_BUILD_NOOP:
        printf("nothing changed\n");
        break;
    case Z_BUILD_CYCLE:
        printf("detected cycle\n");
        break;
    case Z_BUILD_DONE:
        printf("successfully built\n");
        break;
    }

    return 0;
}
