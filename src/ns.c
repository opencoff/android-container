/* userns.c - example namespace management

   Licensed under GNU General Public License v2 or later

   Create a child process that executes a shell command in new
   namespace(s); allow UID and GID mappings to be specified when
   creating a user namespace.
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <syscall.h>
#include <sched.h>

#include "getopt_long.h"
#include "error.h"

struct container_config {
    char * const rootfs;     // root of the namespaced file-system
    char * const init;       // pid-0

    int    pipe_fd[2];      // Pipe used to synchronize parent and child
};
typedef struct container_config container_config;


/*
 * Globals
 */
uint64_t    Memlimit = 256 * 1048576;
int         Verbose  = 0;
int         Netns    = 0;

/*
 * Internal functions
 */

static uint64_t grok_size(const char *str, const char *optname);
static int      parse_options(int argc, char *const argv[]);
static int      parse_uidgid(const char *str);
static void     validate_exe(const char *root, const char *exe);
static int      childFunc(void *arg);
static int      switchroot(const char *root);
static int      maybe_mkdir(const char *dn, int mode);
static void     proc_setgroups_write(pid_t child_pid, char *str);
static void     update_map(char *mapping, char *map_file);


/*
 * External funcs
 */
extern void run_exe(char *const exe);


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

    printf("Usage: %s [options] pre-exec.sh /path/to/rootfs post-exec.sh uid gid\n"
            "\n"
            "Where:\n"
            " pre-exec.sh     is called by the parent before creating the container. This can\n"
            "                 be used to setup a network namespace and 'veth' ethernet adapter.\n"
            "                 This should be accessible and executable by the parent.\n"
            " /path/to/rootfs is the path to a directory containing the root file system for\n"
            "                 the container. This directory will become the new 'root' in the\n"
            "                 mount-namespace.\n"
            " post-exec.sh    is called by the parent after the container namespace is setup. This\n"
            "                 script is expected to live inside '/path/to/rootfs' sub-directory.\n"
            " uid             UID-0 inside the container is mapped to this 'uid'.\n"
            " gid             GID-0 inside the container is mapped to this 'gid'.\n"
            "\n"
            "Optional Arguments:\n"
            "  --help, -h     Show this help message and exit\n"
            "  --verbose, -v  Show verbose progress messages\n"
            "  --memory=M, -m M Limit container to M bytes of memory [256M]\n"
            "", program_name);

}

/* Update the mapping file 'map_file', with the value provided in
 * 'mapping', a string that defines a UID or GID mapping. A UID or
 * GID mapping consists of one or more newline-delimited records
 * of the form:
 *
 * ID_inside-ns    ID-outside-ns   length
 *
 * Requiring the user to supply a string that contains newlines is
 * of course inconvenient for command-line use. Thus, we permit the
 * use of commas to delimit records in this string, and replace them
 * with newlines before writing the string to the file.
 */
static void
update_map(char *mapping, char *map_file)
{
    int fd, j;
    size_t map_len;     /* Length of 'mapping' */

    map_len = strlen(mapping);
    for (j = 0; j < map_len; j++) {
        if (mapping[j] == ',') mapping[j] = '\n';
    }

    fd = open(map_file, O_RDWR);
    if (fd < 0) error(1, errno, "can't open %s", map_file);

    if (write(fd, mapping, map_len) != map_len) error(1, errno, "partial write to %s", map_file);

    close(fd);
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
proc_setgroups_write(pid_t child_pid, char *str)
{
    char path[PATH_MAX];
    int fd;

    snprintf(path, sizeof path, "/proc/%d/setgroups", child_pid);

    fd = open(path, O_RDWR);
    if (fd < 0) {
        /* We may be on a system that doesn't support
         * /proc/PID/setgroups. In that case, the file won't exist,
         * and the system won't impose the restrictions that Linux 3.19
         * added. That's fine: we don't need to do anything in order
         * to permit 'gid_map' to be updated.
         *
         * However, if the error from open() was something other than
         * the ENOENT error that is expected for that case,  let the
         * user know.
         */

        if (errno != ENOENT) error(1, errno, "can't open %s", path);
        return;
    }

    if (write(fd, str, strlen(str)) < 0) error(1, errno, "write error %s", path);
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
    if (r < 0) return -errno;

    r = mount(rootpath, rootpath, "bind", MS_BIND|MS_REC, "");
    if (r < 0) return -errno;

    r = syscall(SYS_pivot_root, rootpath, pivot);
    if (r < 0) return -errno;

    chdir("/");

    r = umount2("/.pivot", MNT_DETACH);
    if (r < 0) return -errno;

    rmdir("/.pivot");
    return 0;
}

static int
childFunc(void *arg)
{
    container_config *cc = arg;
    char ch;
    int r;

    /*
     * Wait until the parent has updated the UID and GID mappings.
     * See the comment in main(). We wait for end of file on a
     * pipe that will be closed by the parent process once it has
     * updated the mappings.
     */
    close(cc->pipe_fd[1]); 

    if (read(cc->pipe_fd[0], &ch, 1) != 0) die("pipe read returned data?");

    /*
     * Setup the rootfs and then exec the kid.
     */

    /* Execute a shell command */
    r = mount("/", "/", "none", MS_PRIVATE | MS_REC, 0);
    if (r < 0) error(1, errno, "child: can't remount / as private");

    umount2("/proc", MNT_DETACH);
    umount2("/dev",  MNT_DETACH);

    r = switchroot(cc->rootfs);
    if (r < 0) error(1, -r, "child: can't pivot root to %s", cc->rootfs);

    progress("child: my pid=%d; About to exec %s\n", getpid(), cc->init);


    char * const argv[2] = { cc->init, 0 };
    char * const envp[]  = { "PATH=/sbin:/bin:/usr/sbin:/usr/bin:/usr/local/bin", 0 };
    execvpe(cc->init, argv, envp);
    error(1, errno, "execvp of kid failed");
    return 0;
}


// 4MB stack for the child
#define STACK_SIZE (4 * 1048576)
static char child_stack[STACK_SIZE];    /* Space for child's stack */


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
    if (!(st.st_mode & 0x500)) die("%s is not executable", path);
}


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

    if (argc < 5) {
        usage("Insufficient arguments!");
        exit(1);
    }


    char * const preexec  = argv[0];
    char * const rootfs   = argv[1];
    char * const postexec = argv[2];
    const char *ustr      = argv[3];
    const char *gstr      = argv[4];
    int uid, gid;


    validate_exe("/",    preexec);
    validate_exe(rootfs, postexec);

    uid = parse_uidgid(ustr);
    gid = parse_uidgid(gstr);

    int flags;
    pid_t child_pid;
    char map_buf[128];
    char map_path[PATH_MAX];
    container_config cc = { .rootfs = rootfs, .init = postexec, };


    /*
     * In our hard-coded version, we want the following:
     *
     *  - all CLONE_xxx flags
     *  - map uid/gid 0 to a non-zero UID/GID
     */

    flags   = CLONE_NEWIPC | CLONE_NEWNS | CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWUTS;
    if (Netns) flags |= CLONE_NEWNET;

    /* We use a pipe to synchronize the parent and child, in order to
     * ensure that the parent sets the UID and GID maps before the child
     * calls execve(). This ensures that the child maintains its
     * capabilities during the execve() in the common case where we
     * want to map the child's effective user ID to 0 in the new user
     * namespace. Without this synchronization, the child would lose
     * its capabilities if it performed an execve() with nonzero
     * user IDs (see the capabilities(7) man page for details of the
     * transformation of a process's capabilities during execve()).
     */

    if (pipe(cc.pipe_fd) < 0) error(1, errno, "can't create pipe");

    child_pid = clone(childFunc, child_stack + STACK_SIZE, flags | SIGCHLD, &cc);
    if (child_pid == -1) error(1, errno, "can't clone new namespace");


    /*
     * Now, remap the ZERO uid/pid in the cloned namespace.
     */
    snprintf(map_path, sizeof map_path, "/proc/%d/uid_map", child_pid);
    snprintf(map_buf, sizeof map_buf, "0 %d 1", uid);
    update_map(map_buf, map_path);

    proc_setgroups_write(child_pid, "deny");

    snprintf(map_path, sizeof map_path, "/proc/%d/gid_map", child_pid);
    snprintf(map_buf, sizeof map_buf, "0 %d 1", gid);
    update_map(map_buf, map_path);

    run_exe(preexec);

    /*
     * Finally, signal the kid that we are ready to go; we do this
     * by closing tne pipe.
     */
    close(cc.pipe_fd[1]);

    if (waitpid(child_pid, NULL, 0) == -1) error(1, errno, "waitpid on %d failed", child_pid);

    return 0;
}



static const struct option Longopt[] = 
{
      {"help",                  no_argument, 0, 'h'}
    , {"verbose",               no_argument, 0, 'v'}
    , {"memory",                required_argument, 0, 'c'}
    , {"network",               no_argument, 0, 'n'}
    , {0, 0, 0, 0}
};
static const char Shortopt[] = "hvm:n";

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
