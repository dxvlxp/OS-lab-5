#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int parse_n(const char *text, int *n)
{
    char *end;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno != 0 || text == end || *end != '\0' ||
        value < 1 || value > INT_MAX)
        return -1;
    *n = (int)value;
    return 0;
}

int main(int argc, char **argv)
{
    int n;
    if (argc != 2 || parse_n(argv[1], &n) == -1) {
        fprintf(stderr, "Usage: %s n (positive integer; use 3 or 5 for the experiment)\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    int failed = 0;
    for (int i = 0; i < n; ++i) {
        pid_t child = fork();
        if (child == -1) {
            perror("fork");
            failed = 1;
            break;
        }

        unsigned int remaining = 5;
        while (remaining != 0)
            remaining = sleep(remaining);
    }

    for (;;) {
        int status;
        pid_t child = wait(&status);
        if (child > 0) {
            if (!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS)
                failed = 1;
            continue;
        }
        if (errno == EINTR)
            continue;
        if (errno != ECHILD) {
            perror("wait");
            failed = 1;
        }
        break;
    }
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
