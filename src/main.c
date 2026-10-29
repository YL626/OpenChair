/* Scaffold CLI plus S3C role dispatch (--role hub|light|distance, SIMULATED
 * testing roles; see oc_role.h). --version/--help are unchanged. Not the
 * application; the supervisor (S4) will launch these roles. */
#include <stdio.h>
#include <string.h>

#include "oc_role.h"
#include "openchair_version.h"

static void print_usage(FILE *out)
{
    fputs("usage: openchair [--version | --help]\n", out);
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "--role") == 0)
        return oc_role_main(argc, argv);
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
