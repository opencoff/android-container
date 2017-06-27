/* vim: expandtab:tw=68:ts=4:sw=4:
 *
 * runprog.c - fork/exec an external program.
 *
 * Copyright (c) 1999-2017 Sudhi Herle <sw at herle.net>
 *
 * Licensing Terms: GPLv2 
 *
 * If you need a commercial license for this work, please contact
 * the author.
 *
 * This software does not come with any express or implied
 * warranty; it is provided "as is". No claim  is made to its
 * suitability for any purpose.
 */

#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include "error.h"

/*
 * Run an external program; die on error.
 *
 * Returns Only if external program ran successfully and exited with
 * a zero code.
 */
void
run_argv(char * const argv[], char * const env[])
{
    const char * const exe = argv[0];

    pid_t pid = fork();
    if (pid == -1) error(1, errno, "can't fork %s", exe);

    if (pid == 0) { // child

        chdir("/tmp");
        execve(exe, argv, env);
        error(1, errno, "can't exec %s", exe);
    } else {
        // parent. Wait for child to finish.
        int r = 0;
        waitpid(pid, &r, 0);

        if (WIFEXITED(r)) {
            int x = WEXITSTATUS(r);
            if (x != 0) die("%s exited with non-zero code %d", exe, x);
        } else if (WIFSIGNALED(r)) {
            int sig = WTERMSIG(r);
            die("%s caught signal %d and aborted", exe, sig);
        }
    }
}


void
run_exe(char * const exe)
{
    char * const pargs[2] = { exe, 0 };
    char * const envp[]   = { "PATH=/sbin:/bin:/usr/sbin:/usr/bin", 0 };

    run_argv(pargs, envp);
}
