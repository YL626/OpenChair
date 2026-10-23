/* Scaffold CLI so the build/test toolchain has something real to exercise.
 * Not the application architecture; replace when real modules exist. */
#include <stdio.h>
#include <string.h>

#include "openchair_version.h"

static void print_usage(FILE *out)
{
    fputs("usage: openchair [--version | --help]\n", out);
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        return printf("%s\n", oc_version()) < 0 ? 1 : 0;
    }
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        print_usage(stdout);
        return 0;
    }
    fputs("openchair: unrecognized or missing arguments\n", stderr);
    print_usage(stderr);
    return 2;
}
