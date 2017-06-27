/*
 * container.c - Simple container management for Linux
 *
 * Copyright (c) 2016 Sudhi Herle <sw at herle.net>
 *
 * This software does not come with any express or implied
 * warranty; it is provided "as is". No claim  is made to its
 * suitability for any purpose.
 */

// XXX WTF? Why?
#define _GNU_SOURCE 1

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <signal.h>
#include <fcntl.h>
#include <sched.h>
#include <syscall.h>
#include <dirent.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mount.h>

#include "containerlib.h"
#include "vect.h"


// list of default devices
static const Dev Default_dev[] = {
      { "null",    0666, 1, 3 }
    , { "zero",    0666, 1, 5 }
    , { "full",    0666, 1, 7 }
    , { "random",  0666, 1, 8 }
    , { "urandom", 0666, 1, 9 }
    , { "kmsg",    0644, 1, 11 }
    , { "console", 0600, 5, 1 }

    // XXX TTYs?

      // Always keep in the end
    , { 0, 0, 0}
};


#define NSFLAGS (SIGCHLD | CLONE_NEWNS | CLONE_NEWCGROUP|CLONE_NEWPID | CLONE_NEWUTS|CLONE_NEWNET|CLONE_NEWIPC|CLONE_NEWUSER)

/*
 * Child stack - in bytes
 */
#define STACK_SIZE          65536

// Static environment block for execve whenever we use it
static char * const _Env[] = {
    "PATH=/sbin:/bin:/usr/sbin:/usr/bin",
    0,
};


static int remap_xid(const char *fmt, pid_t kid, long id);
static int doexec(void *arg);
static int switchroot(const char *root);
static int  kid_error(Container_config *a, int r);
static void kid_ok(Container_config *a);
static int make_dev(Container_config *a, const Dev *);
static int close_most_fd(Container_config *);

int
container_start(Container_config *a)
{
    int p[2];
    int q[2];
    int r;

    r = pipe(p);
    if (r < 0) {
        r = -errno;
        INFOLOG("pipe-p fail: %s (%d)\n", strerror(errno), errno);
        return r;
    }

    r = pipe(q);
    if (r < 0) {
        r = -errno;
        INFOLOG("pipe-q fail: %s (%d)\n", strerror(errno), errno);

        goto fail2;
    }

    a->p_rd = p[0]; // for kid to wait/block until parent gives go ahead
    a->p_wr = q[1]; // for kid to write failures and such

    DEBUGLOG("Starting container at %s..\n", a->root);

    uint8_t *stack = calloc(STACK_SIZE, 1);
    pid_t      kid = clone(doexec, stack+STACK_SIZE, NSFLAGS, a);
    if (kid < 0) {
        r = -errno;
        INFOLOG("clone fail: %s (%d)\n", strerror(errno), errno);
        goto fail;
    }

    /*
     * Do UID/GID mapping:
     *
     * We map uid-0 in the child to a non-zero uid in the parent.
     */
    r = remap_xid("/proc/%ld/uid_map", kid, a->uid);
    if (r < 0) { r = -errno; goto fail; }

    r = remap_xid("/proc/%ld/gid_map", kid, a->gid);
    if (r < 0) { r = -errno; goto fail; }

    DEBUGLOG("Remapped UID/GID to %d/%d..\n", a->uid, a->gid);

    /*
     * Unblock the child and let it proceed
     */
    char c = 1;
    if (1 != write(p[1], &c, 1)) {
        r = -errno;
        INFOLOG("write-to-kid fail: %s (%d)\n", strerror(errno), errno);
        goto fail;
    }

    DEBUGLOG("waiting for kid %d to finish setup..\n", kid);

    /*
     * Wait for status from kid ..
     */
    int z = 0;
    if (sizeof(z) != read(q[0], &z, sizeof z)) {
        r = -errno;
        INFOLOG("read-from-kid fail: %s (%d)\n", strerror(errno), errno);
        goto fail;
    }

    if (z != 0) {
        INFOLOG("kid-setup pid %d fail: %s (%d)\n", kid, strerror(z), z);
        r = -z;
        goto fail;
    }

    r = kid;
    DEBUGLOG("container-started successfully: kid %d\n", kid);


fail:
    free(stack);
    close(q[0]);
    close(q[1]);

fail2:
    close(p[0]);
    close(p[1]);
    return r;
}


/*
 * Update uid/gid mapping for child process 'kid' so that a
 * root-user inside the container maps to a non-root user in the
 * parent namespace.
 *
 * See user_namespaces(7) for more details.
 */
static int
remap_xid(const char *fmt, pid_t kid, long id)
{
    char p[PATH_MAX];
    char s[256];
    size_t n;
    int r;

    snprintf(p, sizeof p, fmt, kid);
    snprintf(s, sizeof s, "0 %ld 1", id);
    n = strlen(s);

    int fd = open(p, O_RDWR);
    if (fd < 0) {
        r = -errno;
        INFOLOG("open %s fail: %s (%d)\n", p, strerror(errno), errno);
        return r;
    }

    if (n != write(fd, s, n)) {
        r = -errno;
        INFOLOG("write %s fail: %s (%d)\n", p, strerror(errno), errno);
        close(fd);
        return r;
    }

    close(fd);
    return 0;
}


/*
 * Child function called by clone(2).
 *
 * We setup the container here and finally change our "root" to the
 * container-root and start "/sbin/init".
 */
static int
doexec(void *xargs)
{
    char c;
    int  r;
    Container_config *a = xargs;

    a->pid = getpid();

    assert(a->hostname);
    assert(a->initargv);

    DEBUGLOG("kid-%d: Starting container setup..\n", a->pid);

    /*
     * Wait for parent to setup the user-namespace before
     * progressing.
     */
    if (read(a->p_rd, &c, 1) != 1) return kid_error(a, errno);

    DEBUGLOG("kid-%d: Remounting / as private..\n", a->pid);

    /*
     * Remount / as private - just to be sure.
     */
    r = mount("/", "/", "none", MS_PRIVATE | MS_REC, 0);
    if (r < 0) return kid_error(a, errno);


    DEBUGLOG("kid-%d: Mounting all file systems ..\n", a->pid);


    /*
     * Mount rest of namespace needed:
     *   - root under a->root
     *   - sdcard under   $root/sdcard
     *   - userhome under $root/home -- maybe?
     *   - proc -- mount again
     *   - sys  -- mount again
     */

    DEBUGLOG("kid-%d: switching root to %s..\n", a->pid, a->root);

    // Finally, change to new root
    r = switchroot(a->root);
    if (r < 0) return kid_error(a, -r);

    DEBUGLOG("kid-%d: making default device nodes ..\n", a->pid);

    /*
     * mknod() devices we need. We only create a subset of devices.
     */
    r = make_dev(a, Default_dev);
    if (r < 0) return kid_error(a, -r);

    if (a->devices) {
        DEBUGLOG("kid-%d: making other device nodes ..\n", a->pid);
        r = make_dev(a, a->devices);
        if (r < 0) return kid_error(a, -r);
    }

    if (a->domain && strlen(a->domain) > 0)
        setdomainname(a->domain, strlen(a->domain));

    sethostname(a->hostname, strlen(a->hostname));

    r = setipaddr(a);
    if (r < 0) return kid_error(a, -r);

    /*
     * Finally, switch to a user-namespace before calling /sbin/init
     */

    /*
     * XXX I have to tell the parent that everything worked. If the
     * exec fails for whatever reason, the parent will never be
     * notified now!
     */

    // We can't close 0, 1, 2; but close everything else..
    r = close_most_fd(a);
    if (r < 0) return kid_error(a, -r);

    DEBUGLOG("kid-%d: starting %s ..\n", a->pid, a->initargv[0]);

    kid_ok(a);


    r = execve(a->initargv[0], a->initargv, _Env);

    // XXX Wot to do about failures??
    return 0;
}


/*
 * Make directory 'dn' if it doesn't already exist.
 */
int
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
 * Switch FS root to 'root'
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


/*
 * Make devices in the array 'd'.
 */
static int
make_dev(Container_config *a, const Dev *d)
{
    const Dev *x = d;
    char p[PATH_MAX];
    int r;
    struct stat st;

    for (; x->name; x++) {
        dev_t z = makedev(x->major, x->minor);

        snprintf(p, sizeof p, "/dev/%s", x->name);

        r = stat(p, &st);
        if (r == 0) {
            if (st.st_dev == z) continue;

            INFOLOG("kid-%d: dev %s maj/min mismatch (exp %d/%d, saw %d/%d)\n",
                    a->pid, p, x->major, x->minor, major(st.st_dev), minor(st.st_dev));
            return -r;
        } else if (r < 0 && errno != ENOENT) {
            r = errno;
            INFOLOG("kid-%d: stat %s fail: %s (%d)\n", a->pid, p, strerror(r), r);
            return -r;
        }

        r = mknod(p, x->mode, z);
        if (r < 0) {
            r = errno;
            INFOLOG("kid-%d: mknod %s fail: %s (%d)\n", a->pid, p, strerror(r), r);
            return -r;
        }
    }
    return 0;
}


/*
 * Send error indication to parent.
 */
static int
kid_error(Container_config *a, int err)
{
    if (sizeof(err) != write(a->p_wr, &err, sizeof err)) {
        int r = errno;

        INFOLOG("kid-%d: write-to-parent fail: %s (%d)\n", a->pid, strerror(r), r);
        err = -r;
    }
    return -err;
}


/*
 * Send "OK" indication to parent.
 */
static void
kid_ok(Container_config *a)
{
    kid_error(a, 0);
}



/*
 * Close almost all fd except 0, 1, 2
 */
static int
close_most_fd(Container_config *a)
{
    VECT_TYPEDEF(intvect, int);

    int r;
    char p[PATH_MAX];
    snprintf(p, sizeof p, "/proc/%d/fd", getpid());     // this is namespaced pid!

    DIR * d = opendir(p);
    if (!d) {
        r = errno;
        INFOLOG("kid-%d: Can't open %p: %s (%d)\n", a->pid, p, strerror(r), r);
        return -r;
    }

    struct dirent *de;
    char *e;
    intvect v;

    /*
     * We won't close as we read each dir-entry because readdir() may have kept an
     * open fd. So, we gather all the fds in a vect and then close
     * them after readdir() has ended.
     */

    VECT_INIT(&v, 16);
    while ((de = readdir(d))) {
        if (0 == strcmp(de->d_name, ".") ||
            0 == strcmp(de->d_name, ".."))  continue;

        errno  = 0;
        int fd = (int)strtol(de->d_name, &e, 10);
        switch (fd) {
            case 0:
            case 1:
            case 2:
                continue;

            default:
                if (errno == 0) VECT_PUSH_BACK(&v, fd);
                break;
        }
    }
    closedir(d);

    int *x;
    VECT_FOR_EACH(&v, x) {
        close(*x);
    }

    VECT_FINI(&v);
    return 0;
}

/* EOF */
