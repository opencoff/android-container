#! /bin/sh

# post-exec script run in the parent's context. Use this to setup
# network plumbing for the cloned namespace and mounting a devtmpfs
# etc. Finally, exec the real-init of the chroot.
#
# Environment vars set conditionally:
#
#   CLONE_NEWUSER   -- set if 'ns' is invoked with '-u'
#   CLONE_NEWNET    -- set if 'ns' is invoked with '-n'
#   DEBUG           -- set if the 'ns' utility is built in debug mode
#
# By the time this script is invoked, 'ns' has already done the
# following:
#  - parent has:
#     * setup uid/gid mapping (if needed)
#     * setup network plumbing
#  - child has:
#  -  * new root is mounted and the process has pivoted its root
#  -  * mounted /proc
#

# Looks like we can't mount these two if we are running under a
# user-namespace (at least on kernel <= 4.9)
if [  -n "$CLONE_NEWUSER" ]; then
    mount -t devtmpfs devtmpfs /dev   || exit 3
    mount -t sysfs sysfs /sys         || exit 4
fi


# The IP address here mirrors the setup in pre.sh.
# For production use, rely on DHCP instead of hard-coding these.
ip addr add 10.99.88.2/24 dev eth0  || exit 5
ip link set up dev eth0             || exit 6
ip route add default via 10.99.88.1 || exit 7

# Environment var set by the caller
if [ -n "$DEBUG" ]; then
    exec /bin/sh                   || exit 8
else
    for f in /sbin /bin; do
        if [ -f $f/init ]; then
            exec $f/init           || exit 9
        fi
    done
    echo "$0: No working init found!" 1>&2
    exit 8
fi
