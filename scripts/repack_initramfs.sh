#!/bin/sh
# Rebuild the shared Linux initramfs used by VM1 and VM2.

set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ARCHIVE=${1:-"$ROOT/sdk/image/linux/Initramfs.cpio.gz"}
TMPDIR_ROOT=${TMPDIR:-/tmp}
WORKDIR=$(mktemp -d "$TMPDIR_ROOT/beau-initramfs.XXXXXX")

cleanup()
{
	rm -rf "$WORKDIR"
}

trap cleanup EXIT

gzip -dc "$ARCHIVE" | (cd "$WORKDIR" && cpio -id --quiet --no-absolute-filenames)
chmod 0755 "$WORKDIR"

cat > "$WORKDIR/init" <<'EOF'
#!/bin/sh

#  __  __    ______    ______
# /\ \/\ \  /\  __ \  /\  ___\
# \ \ \_\ \ \ \ \/\ \ \ \___  \
#  \ \_____\ \ \_____\ \/\_____\
#   \/_____/  \/_____/  \/_____/
#                                (2026)

PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs devtmpfs /dev
mkdir -p /dev/pts
mount -t devpts devpts /dev/pts 2>/dev/null || true

# /var/beau is the virtio-fs handoff point used by the BEAU proxy path:
# VM1 exports it through the backend, while VM2 mounts the frontend there.
#
# Manual virtio-fs smoke test from the BEAU shell:
#   1. vsh 1
#      echo "hello from vm1" > /var/beau/README
#      <Ctrl-D>
#   2. vsh 2
#      mount -t virtiofs -o ro beau /var/beau
#      ls -l /var/beau
#      cat /var/beau/README
#
# Current baseline: VM1 is the writable backend export and VM2 is the
# read-only frontend. If VM2 writes /var/beau before the mount command, that
# file is created in VM2's local initramfs and will not appear in VM1. If VM2
# writes after the ro mount, the command should fail; that confirms isolation.
mkdir -p /var/beau
chmod 0755 /var /var/beau 2>/dev/null || true

# uos terminal
#
# BEAU Linux guests use virtio-console as the boot console (`console=hvc0`).
# Do not create or probe ttyAMA0 here: a manually-created PL011 node can be a
# character device while still having no backing driver, which makes /bin/sh
# print "can't open /dev/ttyAMA0". Opening /dev/console also does not reliably
# give ash a controlling tty, causing the "can't access tty" job-control warning.
# Starting the shell as a new session on /dev/hvc0 keeps the console quiet.
[ -c /dev/console ] || mknod /dev/console c 5 1
[ -c /dev/tty ] || mknod /dev/tty c 5 0

run_shell_on_tty()
{
	tty=$1

	[ -c "$tty" ] || return 1
	if ! /bin/sh -c "exec <\"$tty\" >\"$tty\" 2>&1" 2>/dev/null; then
		return 1
	fi

	setsid /bin/sh -c "exec <\"$tty\" >\"$tty\" 2>&1; stty sane rows 24 cols 80 -ixon -ixoff 2>/dev/null || true; exec /bin/sh -i"
}

# uos commands
alias ll='ls -la'

TERM=dumb
PS1='\[\033[0;92m\]uos \w\[\033[0m\] '

export PS1 PATH TERM

while true; do
	run_shell_on_tty /dev/hvc0 && continue
	sleep 1
done
EOF

chmod 0755 "$WORKDIR/init"
mkdir -p "$WORKDIR/var/beau"
chmod 0755 "$WORKDIR/var" "$WORKDIR/var/beau"
(cd "$WORKDIR" && find . -print0 | cpio --null -o --quiet -H newc -R 0:0 | gzip -9 > "$ARCHIVE.tmp")
mv "$ARCHIVE.tmp" "$ARCHIVE"
