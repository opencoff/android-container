#! /bin/sh

# post-exec script run in the parent's context. Use this to setup
# network plumbing for the cloned namespace and mounting a devtmpfs
# etc.
#
# Finally, exec the real-init of the chroot.
#
# Environment vars set conditionally:
#
#   CLONE_NEWUSER
#   CLONE_NEWNET
#

mkdir -p /proc /dev /sys          || exit 1

mount -t proc proc /proc          || exit 2
mount -t devtmpfs devtmpfs /dev   || exit 3
mount -t sysfs sysfs /sys         || exit 4


# The IP address here mirrors the setup in pre.sh.
# For production use, rely on DHCP instead of hard-coding these.
ip addr add 10.99.88.2/24 dev eth0 || exit 5
ip link set up dev eth0            || exit 6

# Environment var set by the caller
if [ -n "$DEBUG" ]; then
    exec /bin/sh                   || exit 7
else
    for f in /sbin /bin; do
        if [ -f $f/init ]; then
            exec $f/init           || exit 7
        fi
    done
    echo "$0: No working init found!" 1>&2
    exit 8
fi
