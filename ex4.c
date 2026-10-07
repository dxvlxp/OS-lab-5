#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_ARGS 128
#define MAX_JOBS 64

extern char **environ;

struct job {
    volatile sig_atomic_t pid;
    volatile sig_atomic_t done;
    volatile sig_atomic_t status;
};

static struct job jobs[MAX_JOBS];
static volatile sig_atomic_t foreground_pid;
static volatile sig_atomic_t foreground_done;
static volatile sig_atomic_t foreground_status;
static volatile sig_atomic_t interrupted;
static sigset_t child_signals;

static int status_code(int status)
{
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return 1;
}

static void child_handler(int signo)
{
    (void)signo;
    int saved_errno = errno;
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (pid == (pid_t)foreground_pid) {
            foreground_status = status;
            foreground_done = 1;
        } else {
            for (int i = 0; i < MAX_JOBS; ++i) {
                if (jobs[i].pid == (sig_atomic_t)pid) {
                    jobs[i].status = status;
                    jobs[i].done = 1;
                    break;
                }
            }
        }
    }
    errno = saved_errno;
}

static void interrupt_handler(int signo)
{
    (void)signo;
    interrupted = 1;
}

static int install_signals(void)
{
    sigemptyset(&child_signals);
    sigaddset(&child_signals, SIGCHLD);
    struct sigaction action = {0};
    sigemptyset(&action.sa_mask);
    action.sa_handler = child_handler;
    action.sa_flags = SA_RESTART;
    if (sigaction(SIGCHLD, &action, NULL) == -1)
        return -1;
    action.sa_handler = interrupt_handler;
    action.sa_flags = 0;
    if (sigaction(SIGINT, &action, NULL) == -1)
        return -1;
    action.sa_handler = SIG_IGN;
    return sigaction(SIGQUIT, &action, NULL);
}

static void block_children(sigset_t *previous)
{
    if (sigprocmask(SIG_BLOCK, &child_signals, previous) == -1) {
        perror("sigprocmask");
        exit(EXIT_FAILURE);
    }
}

static void restore_mask(const sigset_t *previous)
{
    if (sigprocmask(SIG_SETMASK, previous, NULL) == -1) {
        perror("sigprocmask");
        exit(EXIT_FAILURE);
    }
}

static void report_finished(void)
{
    for (int i = 0; i < MAX_JOBS; ++i) {
        if (jobs[i].pid != 0 && jobs[i].done) {
            fprintf(stderr, "[background PID=%ld] finished, status=%d\n",
                    (long)jobs[i].pid, status_code((int)jobs[i].status));
            jobs[i].pid = 0;
        }
    }
}

static int split_line(char *line, char **argv)
{
    char *read_at = line;
    char *write_at = line;
    int argc = 0;
    while (isspace((unsigned char)*read_at))
        ++read_at;
    if (*read_at == '#') {
        argv[0] = NULL;
        return 0;
    }
    while (*read_at) {
        while (isspace((unsigned char)*read_at))
            ++read_at;
        if (*read_at == '\0')
            break;
        if (argc == MAX_ARGS) {
            fprintf(stderr, "ex4: at most %d arguments are supported.\n", MAX_ARGS);
            return -1;
        }
        argv[argc++] = write_at;
        int quote = 0;
        while (*read_at) {
            unsigned char ch = (unsigned char)*read_at;
            if (!quote && isspace(ch))
                break;
            if ((ch == '\'' && quote != '"') ||
                (ch == '"' && quote != '\'')) {
                quote = quote == 0 ? ch : 0;
                ++read_at;
                continue;
            }
            if (ch == '\\' && quote != '\'') {
                ++read_at;
                if (*read_at == '\0' || *read_at == '\n') {
                    fprintf(stderr, "ex4: unfinished escape sequence.\n");
                    return -1;
                }
                if (quote == '"' && *read_at != '"' && *read_at != '\\' &&
                    *read_at != '$' && (unsigned char)*read_at != 0x60)
                    *write_at++ = '\\';
                *write_at++ = *read_at++;
                continue;
            }
            *write_at++ = *read_at++;
        }
        if (quote) {
            fprintf(stderr, "ex4: unclosed quote.\n");
            return -1;
        }
        if (*read_at)
            ++read_at;
        *write_at++ = '\0';
    }
    argv[argc] = NULL;
    return argc;
}

static int execute(char **argv)
{
    if (strchr(argv[0], '/') != NULL) {
        execve(argv[0], argv, environ);
        int code = errno == ENOENT ? 127 : 126;
        perror(argv[0]);
        return code;
    }
    const char *path = getenv("PATH");
    if (!path)
        path = "/bin:/usr/bin";
    size_t capacity = strlen(path) + strlen(argv[0]) + 2;
    char *candidate = malloc(capacity);
    if (!candidate) {
        perror("malloc");
        return 126;
    }
    int denied = 0;
    const char *directory = path;
    for (;;) {
        const char *colon = strchr(directory, ':');
        size_t length = colon ? (size_t)(colon - directory) : strlen(directory);
        if (length == 0) {
            strcpy(candidate, argv[0]);
        } else {
            memcpy(candidate, directory, length);
            candidate[length] = '/';
            strcpy(candidate + length + 1, argv[0]);
        }
        execve(candidate, argv, environ);
        if (errno == EACCES)
            denied = 1;
        else if (errno != ENOENT && errno != ENOTDIR) {
            perror(argv[0]);
            free(candidate);
            return 126;
        }
        if (!colon)
            break;
        directory = colon + 1;
    }
    free(candidate);
    if (denied) {
        errno = EACCES;
        perror(argv[0]);
        return 126;
    }
    fprintf(stderr, "ex4: command not found: %s\n", argv[0]);
    return 127;
}

static void prepare_child(const sigset_t *previous, int background)
{
    struct sigaction action = {0};
    sigemptyset(&action.sa_mask);
    action.sa_handler = SIG_DFL;
    if (sigaction(SIGCHLD, &action, NULL) == -1 ||
        sigaction(SIGINT, &action, NULL) == -1 ||
        sigaction(SIGQUIT, &action, NULL) == -1 ||
        sigprocmask(SIG_SETMASK, previous, NULL) == -1) {
        perror("child: signals");
        _exit(125);
    }
    if (background) {
        if (setpgid(0, 0) == -1) {
            perror("setpgid");
            _exit(125);
        }
        int input = open("/dev/null", O_RDONLY);
        if (input == -1 || dup2(input, STDIN_FILENO) == -1) {
            perror("background stdin");
            _exit(125);
        }
        if (input != STDIN_FILENO)
            close(input);
    }
}

static int launch(char **argv, int background)
{
    sigset_t previous;
    block_children(&previous);
    report_finished();
    int slot = -1;
    if (background) {
        for (int i = 0; i < MAX_JOBS; ++i) {
            if (jobs[i].pid == 0) {
                slot = i;
                break;
            }
        }
        if (slot == -1) {
            fprintf(stderr, "ex4: too many background jobs.\n");
            restore_mask(&previous);
            return 1;
        }
    }
    fflush(NULL);
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        restore_mask(&previous);
        return 1;
    }
    if (pid == 0) {
        prepare_child(&previous, background);
        _exit(execute(argv));
    }
    if (background) {
        jobs[slot].done = 0;
        jobs[slot].status = 0;
        jobs[slot].pid = (sig_atomic_t)pid;
        (void)setpgid(pid, pid);
        fprintf(stderr, "[background PID=%ld] started: %s\n",
                (long)pid, argv[0]);
        restore_mask(&previous);
        return 0;
    }
    foreground_pid = (sig_atomic_t)pid;
    foreground_done = 0;
    sigset_t wait_mask = previous;
    sigdelset(&wait_mask, SIGCHLD);
    while (!foreground_done)
        sigsuspend(&wait_mask);
    int code = status_code((int)foreground_status);
    foreground_pid = 0;
    report_finished();
    restore_mask(&previous);
    return code;
}

static int wait_background(int terminate)
{
    sigset_t previous;
    block_children(&previous);
    if (terminate) {
        for (int i = 0; i < MAX_JOBS; ++i) {
            if (jobs[i].pid != 0 && !jobs[i].done) {
                pid_t pid = (pid_t)jobs[i].pid;
                if (kill(-pid, SIGTERM) == -1 && errno == ESRCH)
                    (void)kill(pid, SIGTERM);
            }
        }
    }
    sigset_t wait_mask = previous;
    sigdelset(&wait_mask, SIGCHLD);
    int pending;
    do {
        pending = 0;
        for (int i = 0; i < MAX_JOBS; ++i)
            if (jobs[i].pid != 0 && !jobs[i].done)
                pending = 1;
        if (pending)
            sigsuspend(&wait_mask);
    } while (pending);
    int code = 0;
    for (int i = 0; i < MAX_JOBS; ++i)
        if (jobs[i].pid != 0)
            code = status_code((int)jobs[i].status);
    report_finished();
    restore_mask(&previous);
    return code;
}

int main(void)
{
    if (install_signals() == -1) {
        perror("sigaction");
        return EXIT_FAILURE;
    }
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IOLBF, 0);
    int interactive = isatty(STDIN_FILENO);
    int last_status = 0;
    char *line = NULL;
    size_t capacity = 0;
    for (;;) {
        sigset_t previous;
        block_children(&previous);
        report_finished();
        restore_mask(&previous);
        if (interrupted) {
            interrupted = 0;
            if (interactive)
                fputc('\n', stderr);
        }
        if (interactive)
            fputs("mini-shell> ", stderr);
        errno = 0;
        if (getline(&line, &capacity, stdin) == -1) {
            if (errno == EINTR) {
                clearerr(stdin);
                continue;
            }
            if (!feof(stdin)) {
                perror("stdin");
                last_status = 1;
            }
            break;
        }
        char *argv[MAX_ARGS + 1];
        int argc = split_line(line, argv);
        if (argc == -1) {
            last_status = 2;
            continue;
        }
        if (argc == 0)
            continue;
        if (strcmp(argv[0], "exit") == 0) {
            if (argc > 2) {
                fprintf(stderr, "Usage: exit [0..255]\n");
                last_status = 2;
                continue;
            }
            if (argc == 2) {
                char *end;
                errno = 0;
                long code = strtol(argv[1], &end, 10);
                if (errno || argv[1] == end || *end || code < 0 || code > 255) {
                    fprintf(stderr, "Usage: exit [0..255]\n");
                    last_status = 2;
                    continue;
                }
                last_status = (int)code;
            }
            break;
        }
        if (strcmp(argv[0], "cd") == 0) {
            const char *directory = argc == 1 ? getenv("HOME") : argv[1];
            if (argc > 2 || !directory) {
                fprintf(stderr, "Usage: cd [directory] (HOME must be set)\n");
                last_status = 1;
            } else if (chdir(directory) == -1) {
                perror("cd");
                last_status = 1;
            } else {
                last_status = 0;
            }
            continue;
        }
        if (strcmp(argv[0], "jobs") == 0 || strcmp(argv[0], "wait") == 0) {
            if (argc != 1) {
                fprintf(stderr, "ex4: %s takes no arguments.\n", argv[0]);
                last_status = 2;
            } else if (strcmp(argv[0], "wait") == 0) {
                last_status = wait_background(0);
            } else {
                int count = 0;
                block_children(&previous);
                report_finished();
                for (int i = 0; i < MAX_JOBS; ++i) {
                    if (jobs[i].pid != 0) {
                        printf("PID=%ld running\n", (long)jobs[i].pid);
                        ++count;
                    }
                }
                restore_mask(&previous);
                if (count == 0)
                    puts("No background jobs.");
                last_status = 0;
            }
            continue;
        }
        int background = strcmp(argv[0], "bg") == 0;
        if (background && argc == 1) {
            fprintf(stderr, "Usage: bg command [arguments...]\n");
            last_status = 2;
            continue;
        }
        last_status = launch(background ? argv + 1 : argv, background);
    }
    free(line);
    (void)wait_background(1);
    return last_status;
}
