// bestof.c (v2 tool): the fastest wall-clock time of a command over N runs, for `make bench`.
//
//   bestof N command [args...]     prints the best time in ms, e.g. "1.712"
//
// The command is started directly (posix_spawn, no shell), its output discarded. Best of N, not
// the mean: what is measured is the work, and every other run only adds the machine's noise.
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>

extern char **environ;

int main(int argc, char **argv)
{
    if (argc < 3 || atoi(argv[1]) < 1)
    {
        fprintf(stderr, "usage: bestof N command [args...]\n");
        return 2;
    }
    int n = atoi(argv[1]);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    double best = 1e300;
    for (int i = 0; i < n; i++)
    {
        struct timespec a, b;
        pid_t pid;
        clock_gettime(CLOCK_MONOTONIC, &a);
        if (posix_spawnp(&pid, argv[2], &fa, NULL, argv + 2, environ) != 0)
        {
            fprintf(stderr, "bestof: cannot start %s\n", argv[2]);
            return 1;
        }
        int status;
        waitpid(pid, &status, 0);
        clock_gettime(CLOCK_MONOTONIC, &b);
        double ms = (double)(b.tv_sec - a.tv_sec) * 1e3 + (double)(b.tv_nsec - a.tv_nsec) / 1e6;
        best = ms < best ? ms : best;
    }
    printf("%.3f\n", best);
    return 0;
}
