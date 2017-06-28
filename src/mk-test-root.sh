#! /bin/bash

Z=$0
set -e 

set -x

die() {
    echo "$Z: $@" 1>&2
    exit 1
}

warn() {
    echo "$Z: $@" 1>&2
}

bb=/bin/busybox
root=/tmp/zzroot
pre=/tmp/pre.sh
# This is relative to $root
post=post.sh


b=$(basename $bb)

[ -f $bb ] || die "Can't find busybox"
ldd $bb | grep -q 'not a dynamic' || die "Need busybox-static"


mkdir -p $root/{sbin,bin,etc}
if [ ! -f $root/bin/$b ]; then
    cp $bb $root/bin
    (cd $root/bin;
     for f in $($bb --list); do
         ln busybox $f
     done
    )
fi


if [ ! -f $pre ]; then
    cat <<EOF > $pre
#! /bin/bash

# pre-exec script to be run to setup networking etc.
# Must exit with 0 on success; else container setup will fail.

echo "EMPTY pre-exec script. Fill it in.."
echo "UID=$(id -u)"
exit 0
EOF
chmod a+x $pre
fi

