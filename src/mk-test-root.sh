#! /bin/bash

Z=$0
set -e 

die() {
    echo "$Z: $@" 1>&2
    exit 1
}

warn() {
    echo "$Z: $@" 1>&2
}

if [ "x$1" != "x" ]; then
    root=$1
    test -d $root && die "Root dir $root already exists"
else
    root=/tmp/zzroot
fi

me=$(id -u)
[ $me -eq 0 ] || die "Need root privileges to run"

bb=/bin/busybox
pre=/tmp/pre.sh
# This is relative to $root
post=post.sh

devs="null:0600:1:3 \
zero:0666:1:5 \
full:0666:1:7 \
random:0666:1:8 \
urandom:0666:1:9 \
kmsg:0644:1:11 \
console:0600:5:1 \
tty:0666:0:5 \
net/tun:0666:10:200"


b=$(basename $bb)

[ -f $bb ] || die "Can't find busybox"
ldd $bb | grep -q 'not a dynamic' || die "Need busybox-static"


mkdir -p $root/{sbin,bin,etc,proc,dev,sys,tmp}
if [ ! -f $root/bin/$b ]; then
    cp $bb $root/bin
    (cd $root/bin;
     for f in $($bb --list); do
         ln busybox $f || exit 1
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


if [ ! -f $pre ]; then
    cat <<EOF > $pre
#! /bin/bash

# pre-exec script to be run to setup networking etc.
# Must exit with 0 on success; else container setup will fail.

kid=$1
echo "EMPTY pre-exec script. Fill it in.."
echo "UID=$(id -u); kid pid $kid"
env
exit 0
EOF
chmod a+x $pre
fi

