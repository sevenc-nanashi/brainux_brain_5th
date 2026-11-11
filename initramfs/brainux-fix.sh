# Shared by fix-*; boot-root owns the mount when invoked with --mounted.
temporary=''
owned_mount=0
case "$#:$*" in
    0:)
        [ -b /dev/mmcblk0p2 ] || { echo 'SD rootfs partition not found' >&2; exit 1; }
        if mountpoint -q /newroot; then
            echo '/newroot is already mounted; unmount it before retrying' >&2
            exit 1
        fi
        mount -t ext4 -o ro /dev/mmcblk0p2 /newroot
        owned_mount=1
        ;;
    1:--mounted)
        mountpoint -q /newroot || { echo '/newroot is not mounted' >&2; exit 1; }
        ;;
    *) echo "usage: ${0##*/}" >&2; exit 1 ;;
esac
finish_fix() {
    [ -z "$temporary" ] || rm -f "$temporary"
    if [ "$owned_mount" -eq 1 ]; then
        umount /newroot
    fi
}
trap finish_fix EXIT
