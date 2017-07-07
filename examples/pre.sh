#! /bin/sh

# pre-exec script run in the parent's context. Use this to setup
# network plumbing for the cloned namespace.
#
# Environment vars set conditionally:
#
#   CLONE_NEWUSER
#   CLONE_NEWNET
#
#
# Must exit with 0 on success; else container setup will fail.


Child=$1

if [ -z "$CLONE_NEWNET" ]; then
    exit 0
fi

# Network name of 'eth0' should be the same one used in the child
# namespace. 'veth0' is the interface name in the host (parent namespace).
ip link add name veth0 type veth peer eth0 netns $Child || exit 1
ip addr add 10.99.88.1/24 dev veth0                     || exit 2
ip link set up dev veth0                                || exit 3

# XXX We will assume NAT and routing is already in place.

exit 0
