/* vim: expandtab:tw=68:ts=4:sw=4:
 *
 * ns.c - Simple namespaces for Android. This enables one to setup
 *        simple containers for Android.
 *
 * Copyright (c) 2017 Sudhi Herle <sw at herle.net>
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
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sched.h>

#include "getopt_long.h"
#include "error.h"

struct container_config {
    char * const rootfs;     // root of the namespaced file-system
    char * const init;       // pid-0

    int  uid, gid;          // remapped UID/GID

    int  fd;    // socketpair fd for communicating with parent
};
typedef struct container_config container_config;


/*
 * Globals
 */
uint64_t    Memlimit = 0;
int         Verbose  = 0;
int         Netns    = 0;
int         Userns   = 0;
int         Unprivns = 0;

/*
 * Internal functions
 */

static uint64_t grok_size(const char *str, const char *optname);
static int      parse_options(int argc, char *const argv[]);
static int      parse_uidgid(const char *str);
static void     validate_exe(const char *root, const char *exe);
static int      child_func(void *arg);
static int      switchroot(const char *root);
static int      maybe_mkdir(const char *dn, int mode);
static void     update_setgroups(pid_t kid, char *str);
static void     limit_memory(pid_t kid, uint64_t membytes);
static void     run_exe(char *const exe, pid_t kid);
static void     writemap(const char *fmt, pid_t kid, int uid);
static int      reap_child(pid_t kid, int opt);
static int      check_unpriv_userns(int euid);

static void wait_socketio(int fd, const char*);
static void signal_socketio(int fd, int eof, const char*);


static void
progress(const char *fmt, ...)
{
    if (!Verbose) return;

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fflush(stdout);
}


static void
usage(char *msg)
{
    if (msg) warn(msg);

    printf("Usage: %s [options] pre-exec.sh /path/to/rootfs post-exec.sh [uid gid]\n"
            "\n"
            "Where:\n"
            " pre-exec.sh     is called by the parent before creating the container. This can\n"
            "                 be used to setup a network namespace and 'veth' ethernet adapter.\n"
            "                 This should be accessible and executable by the parent.\n"
            "                 This script is called with one argument: PID of the child\n"
            " /path/to/rootfs is the path to a directory containing the root file system for\n"
            "                 the container. This directory will become the new 'root' in the\n"
            "                 mount-namespace.\n"
            " post-exec.sh    is called by the parent after the container namespace is setup. This\n"
            "                 script is expected to live inside '/path/to/rootfs' sub-directory.\n"
            "\n"
            "If --user or -u option is specified, then the next two arguments are mandatory:\n"
            " uid             UID-0 inside the container is mapped to this 'uid'.\n"
            " gid             GID-0 inside the container is mapped to this 'gid'.\n"
            "\n"
            "Optional Arguments:\n"
            "  --help, -h     Show this help message and exit\n"
            "  --verbose, -v  Show verbose progress messages\n"
            "  --memory=M, -m M Limit container to M bytes of memory [256M]\n"
            "                   Optional suffixes of 'k', 'M', 'G' denote kilo, Mega and Gigabyte\n"
            "                   multiples.\n"
            "  --network, -n  Setup network namespace as well\n"
            "  --user, -u     Setup user namespace as well (with default uid/gid mapping)\n"
            "", program_name);

}



/*
 * Linux 3.19 made a change in the handling of setgroups(2) and the
 * 'gid_map' file to address a security issue. The issue allowed
 * *unprivileged* users to employ user namespaces in order to drop
 * The upshot of the 3.19 changes is that in order to update the
 * 'gid_maps' file, use of the setgroups() system call in this
 * user namespace must first be disabled by writing "deny" to one
 * of the /proc/PID/setgroups files for this namespace.  That is
 * the purpose of the following function. 
 */
static void
update_setgroups(pid_t kid, char *str)
{
    char path[PATH_MAX];
    int fd;

    snprintf(path, sizeof path, "/proc/%d/setgroups", kid);

    fd = open(path, O_RDWR);
    if (fd < 0) {
        /* older kernels don't support this file. So, we can make it
         * a benign failure..
         */
        if (errno == ENOENT) return;

        // anything else is a fatal error
        error(1, errno, "can't open %s", path);
    }

    size_t n = strlen(str);
    if (n != write(fd, str, n)) error(1, errno, "i/o error while writing %s", path);
    close(fd);
}

/*
 * Make directory 'dn' if it doesn't already exist.
 */
static int
maybe_mkdir(const char *dn, int mode)
{
    struct stat st;
    int r;

    r = lstat(dn, &st);
    if (r == 0) {
        if (S_ISDIR(st.st_mode)) return 0;

        return -ENOTDIR;
    } else if (errno != ENOENT)  return -errno;

    // We know now that we need to make a dir.

    r = mkdir(dn, mode);
    if (r < 0) return -errno;

    return 0;
}


/*
 * Switch to new root 'root'
 */
static int
switchroot(const char *root)
{
    int r;
    char rootpath[PATH_MAX+1];
    char pivot[PATH_MAX+1];

    realpath(root, rootpath);
    snprintf(pivot, PATH_MAX, "%s/.pivot", rootpath);

    r = maybe_mkdir(pivot, 0700);
    if (r < 0) error(1, errno, "can't mkdir %s", pivot);

    r = mount(rootpath, rootpath, "bind", MS_BIND|MS_REC, "");
    if (r < 0) error(1, errno, "can't bind mount %s", rootpath);

    r = syscall(SYS_pivot_root, rootpath, pivot);
    if (r < 0) error(1, errno, "can't pivot root to %s", pivot);

    chdir("/");

    r = umount2("/.pivot", MNT_DETACH);
    if (r < 0) error(1, errno, "can't umount /.pivot");

    rmdir("/.pivot");
    return 0;
}

static int
child_func(void *arg)
{
    container_config *cc = arg;

    if (getuid() != 0) error(1, 0, "child: I am not uid 0, but %d!\n", getuid());
    if (getpid() != 1) error(1, 0, "child: I am not pid 1, but %d!\n", getpid());

    progress("child: pid %d; remounting / ..\n", getpid());

    /*
     * Setup the rootfs and then exec the kid.
     */
    if (mount(0, "/", 0, MS_PRIVATE | MS_REC, 0) < 0)
        error(1, errno, "child: can't remount / as private");

    progress("child: unmounting old file systems ..\n");
    if (umount2("/proc", MNT_DETACH) < 0) error(1, errno, "child: can't umount /proc");
    if (umount2("/dev",  MNT_DETACH) < 0) error(1, errno, "child: can't umount /dev");

    progress("child: setting up rootfs %s ..\n", cc->rootfs);
    switchroot(cc->rootfs);

    // Signal the parent and wait for it to setup uid/gid maps
    signal_socketio(cc->fd, 0, "child");

    /*
     * Wait until the parent has updated the UID and GID mappings.
     * See the comment in main(). We wait for end of file on a
     * pipe that will be closed by the parent process once it has
     * updated the mappings.
     */
    wait_socketio(cc->fd, "child");

    progress("child: pid %d; uid %d -- resuming..\n", getpid(), getuid());

    progress("child: exec'ing init %s ..\n", cc->init);

    char * const argv[2] = { cc->init, 0 };
    const char * envp[5] = { "PATH=/sbin:/bin:/usr/sbin:/usr/bin", 0, 0, 0, 0 };
    int j = 1;

    // Tell the script whether we have two other options set.
    if (Userns) envp[j++] = "CLONE_USERNS=1";
    if (Netns)  envp[j++] = "CLONE_NETNS=1";

    // This macro is defined in GNUmakefile depending on whether
    // this is a release build or a debug build.
#if __DEBUG_BUILD__ > 0
    envp[j++] = "DEBUG=1";
#endif

    signal_socketio(cc->fd, 1, "parent");

    execvpe(cc->init, argv, (char *const *)envp);
    error(1, errno, "child: execvpe of init failed");
    return 0;
}


#define STACK_SIZE  (4 * 1048576)
#define STACK_SIZE_WORDS (STACK_SIZE / sizeof(uint64_t))
uint64_t Stack[STACK_SIZE_WORDS];

/*
 * Usage:
 *    $0 pre-exec.sh /path/to/rootfs post-exec.sh unpriv-uid unpriv-gid
 */
int
main(int argc, char * const argv[])
{
    program_name = argv[0];

    int r = parse_options(argc, argv);
    argc -= r;
    argv  = &argv[r];

    if (argc < 3) {
        usage("Insufficient arguments!");
        exit(1);
    }


    char * const preexec  = argv[0];
    char * const rootfs   = argv[1];
    char * const postexec = argv[2];

    int pfd[2];
    int uid = 0,
        gid = 0,
        fd  = 0;    // parent's end of socketpair()

    argc -= 3;
    argv  = &argv[3];

    validate_exe("/",    preexec);
    validate_exe(rootfs, postexec);

    int flags;
    container_config cc = { .rootfs = rootfs, .init = postexec, };

    /* bi-directional pipe to communicate with kid and vice-versa */
    if (socketpair(AF_LOCAL, SOCK_SEQPACKET, 0, pfd) < 0)
        error(1, errno, "can't create socketpair");


    cc.fd  = pfd[0];
    fd     = pfd[1];

    flags  = CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWUTS;
    flags |= CLONE_NEWIPC;

#if 0
    // XXX Not supported on android!
    flags |= CLONE_NEWCGROUP;
#endif

    if (Netns) flags  |= CLONE_NEWNET;

    if (Userns) {
        if (argc < 2) {
            usage("Insufficient arguments!");
            exit(1);
        }

        const char *ustr = argv[0];
        const char *gstr = argv[1];

        uid = parse_uidgid(ustr);
        gid = parse_uidgid(gstr);

        int euid = geteuid();

        if (euid != 0) check_unpriv_userns(euid);

        flags |= CLONE_NEWUSER;
    }

    progress("parent: starting child under new namespace ..\n");
    
    pid_t kid = clone(child_func, Stack+STACK_SIZE_WORDS, flags |SIGCHLD, &cc);
    if (kid == (pid_t)-1) error(1, errno, "can't clone");

    progress("parent: cloned child %d ..\n", kid);

    // complex handshake here;  have to wait until child can
    // unshare(CLONE_NEWUSER) and signal us.
    wait_socketio(fd, "kid");

    if (Userns) {
        progress("parent: fixing up container uid/gid to %d/%d\n", uid, gid);

        /*
         * Now, remap the ZERO uid/pid in the cloned namespace.
         */
        writemap("/proc/%d/uid_map", kid, uid);

        update_setgroups(kid, "deny");
        writemap("/proc/%d/gid_map", kid, gid);
    }

    if (Memlimit > 0) {
        progress("parent: Limiting container to %d bytes of memory ..\n", Memlimit);
        limit_memory(kid, Memlimit);
    }

    progress("parent: running %s before handing control to kid ..\n", preexec);
    run_exe(preexec, kid);

    /*
     * Finally, signal the kid that we are ready to go; we do this
     * by closing tne pipe.
     */
    progress("parent: resuming container child ..\n");
    signal_socketio(fd, 1, "kid");

    close(pfd[0]);

    reap_child(kid, 0);
    progress("parent: Done\n");

    return 0;
}


static int
check_unpriv_userns(int euid)
{
    char buf[32];
    const char *procf = "/proc/sys/kernel/unprivileged_userns_clone";
    int         fd    = open(procf, O_RDONLY);

    if (fd < 0)                        error(1, errno, "can't open %s", procf);
    if (read(fd, buf, sizeof buf) < 0) error(1, errno, "i/o error while reading %s", procf);
    close(fd);

    if (buf[0] == '1') return 0;

    die("Unprivileged user (uid %d) can't create user namespace.\n"
        "   %s  is 0", euid, procf);

    return 0;
}


static int
reap_child(pid_t kid, int opt)
{
    int r = 0;

    progress("parent: checking on child %d to exit..\n", kid);

    pid_t p = waitpid(kid, &r, opt);

    if (p == 0) return 0;
    if (p == (pid_t)-1) error(1, errno, "waitpid on %d failed", kid);

    if (WIFEXITED(r)) {
        int x = WEXITSTATUS(r);
        if (x != 0) die("kid exited with non-zero code %d", x);

        return 1;   // success
    } else if (WIFSIGNALED(r)) {
        int sig = WTERMSIG(r);
        die("kid caught signal %d and aborted", sig);
    }

    return 0;
}


static void
wait_socketio(int fd, const char *who)
{
    char c;

    if (1 != read(fd, &c, 1)) error(1, 0, "incomplete pipe read from %s", who);
}


static void
signal_socketio(int fd, int eof, const char *who)
{
    char c = 1;

    if (1 != write(fd, &c, 1)) error(1, 0, "incomplete pipe write to %s", who);
    if (eof) close(fd);
}


/*
 * Parse uid or gid in a string.
 */
static int
parse_uidgid(const char *str)
{
    int id = 0;
    int c;

    if ((c = *str) == '-') error(1, 0, "uid/gid %s can't be negative", str);

    while ((c = *str++)) {
        if (!isdigit(c)) error(1, 0, "invalid character '%c' in uid/gid '%s", c, str);
        id *= 10;
        id += c - '0';
    }

    return id;
}


/*
 * Validate 'exe' residing under 'root' to be executable.
 */
static void
validate_exe(const char *root, const char *exe)
{
    struct stat st;
    char path[PATH_MAX];

    if (exe[0]  != '/')        die("%s is not an absolute path", exe);
    if (root[0] != '/')        die("%s is not an absolute path", root);
    if (lstat(root, &st) < 0)  error(1, errno, "can't stat '%s'", root);
    if (!S_ISDIR(st.st_mode))  die("%s is not a directory", root);

    exe++;
    snprintf(path, sizeof path, "%s/%s", root, exe);

    if (lstat(path, &st) < 0)  error(1, errno, "can't stat '%s'", path);
    if (!S_ISREG(st.st_mode))  die("%s is not a file", path);
    if ((st.st_mode & 0500) != 0500)  die("%s is not executable", path);
}


/*
 * map the zero uid/gid in kid to a regular use in parent NS
 */
static void
writemap(const char *fmt, pid_t kid, int id)
{
    char file[PATH_MAX];

    snprintf(file, sizeof file, fmt, kid);

    int fd = open(file, O_CLOEXEC|O_WRONLY);
    if (fd < 0) error(1, errno, "can't open %s", file);
    if (dprintf(fd, "0 %d 131072\n", id) < 0) error(1, errno, "i/o error while writing to %s", file);
    close(fd);

    progress("parent: remapped UID/GID %d to 0 for child namespace.", id);
}


/*
 * Run an external program; die on error.
 *
 * Returns Only if external program ran successfully and exited with
 * a zero code.
 */
static void
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


static void
run_exe(char * const exe, pid_t kid)
{
    char b[32]; snprintf(b, sizeof b, "%d", kid);
    char * const pargs[] = { exe, b, 0 };
    const char * envp[4] = { "PATH=/sbin:/bin:/usr/sbin:/usr/bin", 0, 0, 0 };
    int j = 1;

    // Tell the script whether we have two other options set.
    if (Userns) envp[j++] = "CLONE_USERNS=1";
    if (Netns)  envp[j++] = "CLONE_NETNS=1";

    run_argv(pargs, (char * const *)envp);
}



/*
 * Write a 64-bit value to dir/file.
 */
static void
write64(const char *dir, const char *file, uint64_t val)
{
    char path[PATH_MAX];

    snprintf(path, sizeof path, "%s/%s", dir, file);

    int fd   = open(path, O_CLOEXEC|O_RDWR|O_CREAT, 0600);
    if (fd < 0) error(1, errno, "can't open %s", path);
    if (dprintf(fd, "%" PRIu64 "\n", val) < 0) error(1, errno, "i/o error while writing to %s", path);
    close(fd);
}


/*
 * Limit the child container to 'memlimit' bytes of memory.
 * We do this by writing to a cgroup file:
 *      /sys/fs/cgroup/memory/$PID/memory.limit_in_bytes
 */
static void
limit_memory(pid_t pid, uint64_t memlimit)
{
    char dir[PATH_MAX];
    int r;

    snprintf(dir, sizeof dir, "/sys/fs/cgroup/memory/%d", pid);
    r = maybe_mkdir(dir, 0700);
    if (r < 0) error(1, -r, "can't setup memory cgroup for %d", pid);

    write64(dir, "memory.limit_in_bytes", memlimit);
    write64(dir, "memory.memsw.limit_in_bytes", 0); // no swap space!
    write64(dir, "cgroup.procs", pid);
}


static const struct option Longopt[] = 
{
      {"help",                  no_argument, 0, 'h'}
    , {"verbose",               no_argument, 0, 'v'}
    , {"memory",                required_argument, 0, 'c'}
    , {"network",               no_argument, 0, 'n'}
    , {"user",                  no_argument, 0, 'u'}
    , {0, 0, 0, 0}
};
static const char Shortopt[] = "hvm:nu";

static int
parse_options(int argc, char * const argv[])
{
    int c, errs = 0;

    while ((c = getopt_long(argc, argv, Shortopt, Longopt, 0)) != EOF) {
        switch (c) {
            case 'h':  /* help */
                usage(0);
                exit(0);
                break;

            case 'm':  /* memsize */
                if (optarg && *optarg) Memlimit = grok_size(optarg, "memory");
                break;

            case 'v':  /* verbose */
                Verbose = 1;
                break;

            case 'n': // network namespace
                Netns = 1;
                break;

            case 'u':
                Userns = 1;
                break;

            default:
                ++errs;
                break;
        }
    }

    if (errs > 0) die("too many errors");

    return optind;
}


static uint64_t
grok_size(const char * str, const char * option)
{
    uint64_t   xxbase = 0,
               xxmult = 1,
               xxval  = 0;

    char * xxend = 0;

    /* MS is weird. They deliberately chose NOT to use names that the rest
     * of the world uses. */
#ifdef _MSC_VER
#define strtoull(a,b,c,)  _strtoui64(a,b,c)
#define _ULLCONST(n) n##ui64
#else
#define _ULLCONST(n) n##ULL
#endif

#define UL_MAX__    _ULLCONST(18446744073709551615)
#define _kB         _ULLCONST(1024)
#define _MB         (_kB * 1024)
#define _GB         (_MB * 1024)
#define _TB         (_GB * 1024)
#define _PB         (_TB * 1024)

    xxbase = strtoull(str, &xxend, 0);

    if ( xxend && *xxend ) {
        switch (*xxend) {
            case 'b': case 'B':
                break;
            case 'k': case 'K':
                xxmult = _kB;
                break;
            case 'M':
                xxmult = _MB;
                break;
            case 'G':
                xxmult = _GB;
                break;
            case 'T':
                xxmult = _TB;
                break;
            case 'P':
                xxmult = _PB;
                break;
            default:
                error(1, 0, "unknown multilplier constant '%c'  for '%s'",
                        *xxend, option);
                break;
        }

        xxval = xxbase * xxmult;
        if ((xxbase == UL_MAX__ && errno == ERANGE) || (xxval < xxbase)) {
            error(1, 0, "size value overflow for '%s' (base %lu, multiplier %lu)",
                    option, xxbase, xxmult);
        }
    }
    else
        xxval = xxbase;
    return xxval;
}
