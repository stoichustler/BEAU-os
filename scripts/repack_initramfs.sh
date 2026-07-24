#!/bin/sh
# Rebuild the shared Linux initramfs used by VM2 and VM3.

set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ARCHIVE=${1:-"$ROOT/sdk/imgs/linux/Initramfs.cpio.gz"}
EDU_TEST_SRC="$ROOT/sdk/imgs/linux/tools/beau-edu-test.c"
RPMSG_TEST_SRC="$ROOT/sdk/imgs/linux/tools/beau-rpmsg-test.c"
EDU_TEST_CC=${EDU_TEST_CC:-aarch64-linux-gnu-gcc}
case "$ARCHIVE" in
/*) ;;
*) ARCHIVE="$ROOT/$ARCHIVE" ;;
esac
TMPDIR_ROOT=${TMPDIR:-/tmp}
WORKDIR=$(mktemp -d "$TMPDIR_ROOT/beau-initramfs.XXXXXX")
ALPINE_MIRROR=${ALPINE_MIRROR:-https://dl-cdn.alpinelinux.org/alpine}

cleanup()
{
	rm -rf "$WORKDIR"
}

trap cleanup EXIT

fetch_url()
{
	url=$1
	out=$2

	if command -v curl >/dev/null 2>&1; then
		curl -fL --connect-timeout 10 --max-time 120 --retry 3 --retry-delay 1 "$url" -o "$out"
	elif command -v wget >/dev/null 2>&1; then
		wget -T 120 -O "$out" "$url"
	else
		echo "curl or wget is required to fetch $url" >&2
		return 1
	fi
}

install_alpine_apk()
{
	pkg=$1
	repo=$2
	target=${3:-$WORKDIR}
	arch=$(cat "$WORKDIR/etc/apk/arch")
	version=$(cut -d. -f1,2 "$WORKDIR/etc/alpine-release")
	index="$WORKDIR/tmp/APKINDEX.$repo.tar.gz"
	apkindex="$WORKDIR/tmp/APKINDEX.$repo"
	apkver=
	apkfile=
	url=

	mkdir -p "$WORKDIR/tmp"
	if [ ! -s "$index" ]; then
		fetch_url "$ALPINE_MIRROR/v$version/$repo/$arch/APKINDEX.tar.gz" "$index"
		tar -xzf "$index" -O APKINDEX > "$apkindex"
	elif [ ! -s "$apkindex" ]; then
		tar -xzf "$index" -O APKINDEX > "$apkindex"
	fi
	apkver=$(awk -v pkg="$pkg" 'BEGIN { RS = ""; FS = "\n" }
		{
			name = "";
			ver = "";
			for (i = 1; i <= NF; i++) {
				if ($i == "P:" pkg) {
					name = pkg;
				} else if (substr($i, 1, 2) == "V:") {
					ver = substr($i, 3);
				}
			}
			if (name != "" && ver != "") {
				print ver;
				exit;
			}
		}' "$apkindex")
	if [ -z "$apkver" ]; then
		echo "package $pkg not found in Alpine v$version/$repo/$arch" >&2
		return 1
	fi

	apkfile="$WORKDIR/tmp/$pkg-$apkver.apk"
	url="$ALPINE_MIRROR/v$version/$repo/$arch/$pkg-$apkver.apk"
	fetch_url "$url" "$apkfile"
	tar --warning=no-unknown-keyword \
		--exclude='.SIGN*' --exclude='.PKGINFO' \
		-xzf "$apkfile" -C "$target"
	rm -f "$target"/.SIGN* "$target/.PKGINFO"
}

install_stress_ng_package()
{
	# Alpine's official stress-ng package is musl-linked and ships in community.
	# Install its shared-library providers explicitly because this repacker does
	# not invoke apk's dependency solver inside the temporary initramfs root.
	for pkg in judy libbsd libmd gmp liblksctp zlib; do
		install_alpine_apk "$pkg" main
	done
	install_alpine_apk stress-ng community
	if [ ! -x "$WORKDIR/usr/bin/stress-ng" ]; then
		echo "Alpine stress-ng package did not provide /usr/bin/stress-ng" >&2
		exit 1
	fi
	mkdir -p "$WORKDIR/usr/local/bin"
	rm -f "$WORKDIR/usr/local/bin/stress-ng"
	install -m 0755 "$WORKDIR/usr/bin/stress-ng" "$WORKDIR/usr/local/bin/stress-ng"
	rm -f "$WORKDIR/usr/bin/stress-ng"
}

install_debug_tools()
{
	# The repacker extracts APKs directly, so install each runtime provider
	# explicitly instead of relying on apk's dependency solver.
	install_alpine_apk libmagic main
	install_alpine_apk file main
	install_alpine_apk lsof main
	install_alpine_apk ncurses-terminfo-base main
	install_alpine_apk libncursesw main
	install_alpine_apk htop main

	for tool in file htop; do
		if [ ! -x "$WORKDIR/usr/bin/$tool" ]; then
			echo "Alpine $tool package did not provide /usr/bin/$tool" >&2
			exit 1
		fi
	done
}

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

PATH="/tmp:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs devtmpfs /dev
mkdir -p /dev/pts
mount -t devpts devpts /dev/pts 2>/dev/null || true
mkdir -p /tmp
if [ -x /usr/local/bin/stress-ng ]; then
	cp /usr/local/bin/stress-ng /tmp/stress-ng 2>/dev/null || true
	chmod 0755 /tmp/stress-ng 2>/dev/null || true
fi

# /var/beau is the virtio-fs handoff point used by the BEAU proxy path:
# VM2 exports it through the backend, while VM3 mounts the frontend there.
#
# Manual virtio-fs smoke test from the BEAU shell:
#   1. vsh 2
#      ls -l /var/beau
#      <Ctrl-D>
#   2. vsh 3
#      mount -t virtiofs -o rw proxy-fs /var/beau
#      echo "hello from vm3" > /var/beau/README
#      <Ctrl-D>
#   3. vsh 2
#      cat /var/beau/README
#
# Current baseline: VM3 is the writable frontend, while VM2 reads the backend
# export state. If VM3 writes /var/beau before the mount command, that file is
# created in VM3's local initramfs and will not appear in VM2. After the rw
# mount, VM3 writes should be visible from VM2 through the backend.
mkdir -p /var/beau
chmod 0755 /var /var/beau 2>/dev/null || true

# BEAU static remoteproc/RPMsg smoke test (VM3 only):
#   rpmsg hello-rpmsg
# The command attaches remoteproc0 when it is detached, then keeps one
# /dev/rpmsg0 fd open for write and read because rpmsg_char clears pending
# receives when a userspace fd is closed.

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
install_alpine_apk i2c-tools community
install_alpine_apk hwdata-pci main
install_alpine_apk pciutils-libs main
install_alpine_apk pciutils main
mkdir -p "$WORKDIR/usr/local/bin"
rm -f "$WORKDIR/usr/local/bin/beau-edu-test" \
	"$WORKDIR/usr/local/bin/beau-rpmsg-test"
if ! command -v "$EDU_TEST_CC" >/dev/null 2>&1; then
	echo "$EDU_TEST_CC is required to build initramfs test tools" >&2
	exit 1
fi
"$EDU_TEST_CC" -Os -static -s -Wall -Wextra -o "$WORKDIR/usr/local/bin/vpci" "$EDU_TEST_SRC"
"$EDU_TEST_CC" -Os -static -s -Wall -Wextra -o "$WORKDIR/usr/local/bin/rpmsg" "$RPMSG_TEST_SRC"
install_stress_ng_package
install_debug_tools
rm -rf "$WORKDIR/tmp"
(cd "$WORKDIR" && find . -print0 | cpio --null -o --quiet -H newc -R 0:0 | gzip -9 > "$ARCHIVE.tmp")
mv "$ARCHIVE.tmp" "$ARCHIVE"
