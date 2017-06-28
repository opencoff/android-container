#! /bin/bash

set -x

odir=dbg
[ -n "$1" ] && odir=rel

exe=./Linux-$odir/ns

# user to whom uid/gid 0 will be mapped
u=sherle
uid=$(id -u $u)
gid=$(id -g $u)

[ -x $exe ] || exit 1

sudo $exe -v /tmp/pre.sh /tmp/zzroot /bin/sh $uid $gid
