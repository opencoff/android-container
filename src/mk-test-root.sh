#! /bin/bash

#
# simple script to make a busybox based rootfs.
#
# Requires busybox-static.
#

Z=$0
set -e 
bb=/bin/busybox

die() {
    echo "$Z: $@" 1>&2
    exit 1
}

warn() {
    echo "$Z: $@" 1>&2
}

me=$(id -u)
if [ "x$1" != "x" ]; then
    root=$1
else
    root=/tmp/zzroot
fi

[ -d $root  ] && die "Root dir $root already exists"
[ $me -eq 0 ] || die "Need root privileges to run"

[ -x $bb ] || die "Can't find busybox"
ldd $bb | grep -q 'not a dynamic' || die "Need busybox-static"

b=$(basename $bb)
post=$root/post.sh
pre=$root/pre.sh    # Its ok to be in this dir

devs="null:0600:1:3 \
zero:0666:1:5 \
full:0666:1:7 \
random:0666:1:8 \
urandom:0666:1:9 \
kmsg:0644:1:11 \
console:0600:5:1 \
tty:0666:0:5 \
net/tun:0666:10:200"

mkdir -p $root/{sbin,bin,etc,proc,dev,sys,tmp}
if [ ! -f $root/bin/$b ]; then
    cp $bb $root/bin
    (cd $root/bin;
     for f in $($bb --list); do
         ln -f busybox $f || exit 1
     done
    ) || exit 1

    (cd $root/dev;
    for line in $devs; do
         line=$(echo $line | sed -e 's!:! !g')
         set -- $line
         d=$1
         mod=$2
         maj=$3
         min=$4
         bn=$(dirname $d)

         test -d $bn || mkdir -p $bn
         mknod $d c $maj $min || exit 1
         chmod $mod $d || exit 1
    done

    ln -s /proc/self/fd/  fd     || exit 1
    ln -s /proc/self/fd/0 stdin  || exit 1
    ln -s /proc/self/fd/1 stdout || exit 1
    ln -s /proc/self/fd/2 stderr || exit 1
    ) || exit 1

fi


[ -f $pre  ] || cp ../examples/pre.sh  $pre  || exit 1
[ -f $post ] || cp ../examples/post.sh $post || exit 1

cat <<EOF
$Z: You can now create a unprivileged container via:

    ns -v -u $root/pre.sh $root $root/init.sh

EOF
