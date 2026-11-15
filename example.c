#include "zvec.h"
#include "zbuild.h"

ZCmd *compile(ZBuild *b, const char *filename, const char *outfile) {
    ZCmd *c = zb_cmd(b, "clang");
    zb_arg(c, "-O2");
    zb_arg(c, "-Wall");
    zb_arg(c, "-W");
    zb_arg(c, "-c");
    zb_in(c, filename);
    zb_arg(c, "-o");
    zb_out(c, outfile);
    return c;
}

ZCmd *clang_link(ZBuild *b, ZCmd **inputs, const char *output) {
    ZCmd *c = zb_cmd(b, "clang");
    ZCmd **in = inputs;
    while (*in) {
        vec(const char *) outs = zb_cmd_outputs(*in);

        for (size_t i = 0; i < veclen(outs); i++)
            zb_in(c, outs[i]);
        in++;
    }
    zb_arg(c, "-o");
    zb_out(c, output);

    in = inputs;
    while (*in) { zb_after(c, *in); in++; }

    return c;
}

int main(int argc, char *argv[]) {
    ZB_REBUILD_SELF(argc, argv);
    ZBuild *b = zb_new(16);

    zb_mkdir("build");
    ZCmd *c[] = {
        compile(b, "example.c", "build/example.o"),
        NULL
    };

    vec(char) llvm_version = zb_shell("llvm-config --version");
    printf("%*s", (int)veclen(llvm_version), llvm_version);

    clang_link(b, c, "zb_build");

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
