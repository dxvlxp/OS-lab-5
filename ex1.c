#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int print_process(const char *name, clock_t started)
{
    pid_t pid = getpid();
    pid_t ppid = getppid();
    clock_t finished = clock();

    if (started == (clock_t)-1 || finished == (clock_t)-1) {
        fprintf(stderr, "%s: clock() could not measure CPU time.\n", name);
        return EXIT_FAILURE;
    }

    double milliseconds = (double)(finished - started) * 1000.0 /
                          (double)CLOCKS_PER_SEC;
    if (printf("%s: PID=%ld PPID=%ld execution_time_ms=%.3f\n",
               name, (long)pid, (long)ppid, milliseconds) < 0 ||
        fflush(stdout) == EOF) {
        perror("stdout");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

static int wait_for_child(pid_t child)
{
    int status;
    pid_t result;
    do {
        result = waitpid(child, &status, 0);
    } while (result == -1 && errno == EINTR);

    if (result == -1) {
        perror("waitpid");
        return EXIT_FAILURE;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS) {
        fprintf(stderr, "Child PID=%ld did not finish successfully.\n",
                (long)child);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

int main(void)
{
    pid_t child1 = fork();
    clock_t main_started = clock(); 

    if (child1 == -1) {
        perror("first fork");
        return EXIT_FAILURE;
    }
    if (child1 == 0) {
        _exit(print_process("child1", main_started));
    }

    pid_t child2 = fork();
    clock_t child2_started = clock();
    if (child2 == -1) {
        perror("second fork");
        (void)wait_for_child(child1);
        return EXIT_FAILURE;
    }
    if (child2 == 0)
        _exit(print_process("child2", child2_started));

    int failed = 0;
    if (wait_for_child(child1) != EXIT_SUCCESS)
        failed = 1;
    if (wait_for_child(child2) != EXIT_SUCCESS)
        failed = 1;
    if (print_process("main", main_started) != EXIT_SUCCESS)
        failed = 1;

    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
