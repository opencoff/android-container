
For unprivileged user namespaces to work::

    sudo echo 1 > /sys/fs/cgroup/cpuset/cgroup.clone_children
    sudo echo 1 > /proc/sys/kernel/unprivileged_userns_clone

To try if this works::

    unshare -r -U /bin/busybox-static

