=============================
Simple Containers for Android
=============================

:Author: Sudhi Herle <sw-at-herle.net>
:Date: June 26, 2017

This hack builds on Michael Kerrisk's example in
``user_namespaces(7)`` into a simplified container startup tool.

The code is in the ``src`` directory; and it builds a tool called
``ns``.

Usage
-----
The ``ns`` tool is useful to start the ``init`` process of a
``chroot`` sandbox. This of this as a better ``chroot(8)``::

    ns [options] pre /newroot init [uid gid]

Where ::

    PRE      is a pre-exec script that is run in the parent context
             to setup networking, bridges etc. Since this is run in
             the parent's context, it must be an absolute path
             relative to the parent's root directory.

    /newroot is the new RootFS subdir containing a valid root
             filesystem.

    init     is the "init" process that will be started in the cloned
             namespace. Thus, this script or program must be an
             absolute path relative to "/newroot".

Where options are::

    --verbose, -v    Show verbose progress messages
    --network, -n    Additionally clone a network namespace
    --user, -u       Additionally clone a user namespace
    --memory=M, -m M Restrict cloned processes to M bytes of system
                     memory. The size specification can have an
                     optional 'k', 'M' or 'G' suffix to denote kilo,
                     Megabyte or Gigabyte respectively.

If ``--user`` (or ``-u``) option is specified, then ``ns`` will
require two additional command line arguments: ``uid gid``, where::

    uid     UID 0 inside the container is mapped to this 'uid'
    gid     GID 0 inside the container is mapped to this 'gid'


The pre-exec script will be invoked with the following arguments::

    pre.sh PID

Where ``PID`` is the process-id of the cloned child (i.e., the
containerized child). In addition, two other environment variables
are available for **both** scripts::

    CLONE_NEWUSER   This is set to '1' if the user invoked 'ns' with
                    --user option.
    CLONE_NEWNET    This is set to '1' if the user invoked 'ns' with
                    --network option.

The *pre.sh* script can make use of these variables to guide its
actions.

Example Invocation
------------------
Let us start with the following assumptions:

#.  We have an unpacked root file system under
    */tmp/root*; i.e., this directory tree has all the usual directories
    found in a typical Linux desktop or server installation. Further,

#.  We have a pre-exec script in */tmp/pre.sh* that setups a virtual
    network adapter.

#.  We have a init script in */tmp/root/init.sh*. We will refer to
    this as */init.sh* -- since that is path relative to the root
    directory (*/tmp/root*).

::

    sudo ns -m 1G -n /tmp/pre.sh /tmp/root /init.sh

This will start *init.sh* as pid 1 and uid 0 inside an isolated
namespace.

Example implementations of *pre.sh* and *init.sh* are in the *examples/*
subdirectory. 

Building the Code
=================
This builds on any Linux flavor. ::

    cd src
    make

By default, this builds a "debug" build. And, build-output is in a
platform and build-type specific directory. e.g., ``Linux-dbg``,
``Linux-rel``, ``android64-dbg`` etc. All the object files and final
executable are in these build output directories.

Release Builds
--------------
Release builds differ from debug builds in one way: addition of
optimization flags. ::

    make O=1

Android Builds
--------------
You can cross-compile to Android as long as you have a working NDK
installed. The Makefile enables you to build for Android-32 bit or
Android-64 bit::

    make ANDROID=1
    make ANDROID64=1

Of course, you can combine release builds too::

    make O=1 ANDROID64=1

Guide to Source
===============
All the source code is in the *src/* directory and is largely
derived from Michael Kerrisk's original work in
``user-namespaces(7)``.

*ns.c*
    Has ``main()`` and most of the functionality.

*error.c*, *error.h**
    Utility functions to print the error message and die.

*getopt_long.c*, *getopt_long.h*
    Portable ``getopt_long(3)`` implementation from the NetBSD libc
    code. This is BSD licensed (original license).

*mk-test-root.sh*
    Builds a working root directory from busybox-static. This is
    useful to quickly setup a root-dir for test purposes. By
    default, this script builds a root-dir in ``/tmp/zzroot``. Edit
    this script if you wish to customize it.

Other Notes
===========
For unprivileged user namespaces to work::

    sudo echo 1 > /sys/fs/cgroup/cpuset/cgroup.clone_children
    sudo echo 1 > /proc/sys/kernel/unprivileged_userns_clone


.. vim: ft=rst:sw=4:ts=4:tw=72:expandtab:
