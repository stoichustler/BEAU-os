#!/bin/sh
# Rebuild the shared Linux initramfs used by VM1, VM2, and VM3.

set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ARCHIVE=${1:-"$ROOT/sdk/imgs/linux/Initramfs.cpio.gz"}
EDU_TEST_SRC="$ROOT/sdk/imgs/linux/tools/beau-edu-test.c"
RPMSG_TEST_SRC="$ROOT/sdk/imgs/linux/tools/beau-rpmsg-test.c"
VSOCK_TEST_SRC="$ROOT/sdk/imgs/linux/tools/beau-vsock-test.c"
EDU_TEST_CC=${EDU_TEST_CC:-aarch64-linux-gnu-gcc}
RT_TESTS_SRC=${RT_TESTS_SRC:-"$ROOT/../rt-tests-2.10"}
RT_TESTS_VERSION=2.10
NUMACTL_VERSION=2.0.19
NUMACTL_ARCHIVE="numactl-$NUMACTL_VERSION.tar.gz"
NUMACTL_URL="https://github.com/numactl/numactl/releases/download/v$NUMACTL_VERSION/$NUMACTL_ARCHIVE"
NUMACTL_SHA256=f2672a0381cb59196e9c246bf8bcc43d5568bc457700a697f1a1df762b9af884
# QEMU Linux guests reserve the final 4 KiB before pstore for their generated FDT.
INITRAMFS_MAX_SIZE=62910464
case "$ARCHIVE" in
/*) ;;
*) ARCHIVE="$ROOT/$ARCHIVE" ;;
esac
if [ ! -d "$RT_TESTS_SRC" ] || [ ! -f "$RT_TESTS_SRC/Makefile" ]; then
	echo "RT_TESTS_SRC must name an rt-tests-$RT_TESTS_VERSION source directory" >&2
	exit 1
fi
if ! grep -qx "VERSION = $RT_TESTS_VERSION" "$RT_TESTS_SRC/Makefile"; then
	echo "RT_TESTS_SRC is not rt-tests-$RT_TESTS_VERSION" >&2
	exit 1
fi
RT_TESTS_SRC=$(CDPATH= cd -- "$RT_TESTS_SRC" && pwd)
TMPDIR_ROOT=${TMPDIR:-/tmp}
WORKDIR=$(mktemp -d "$TMPDIR_ROOT/beau-initramfs.XXXXXX")
ALPINE_MIRROR=${ALPINE_MIRROR:-https://dl-cdn.alpinelinux.org/alpine}
LIBNUMA_PREFIX=

cleanup()
{
	rm -rf "$WORKDIR"
	rm -f "$ARCHIVE.tmp"
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

copy_rt_tests_source()
{
	target=$1

	mkdir -p "$target"
	(
		cd "$RT_TESTS_SRC"
		tar --exclude='./.git' --exclude='./bld' \
			--exclude='*.a' --exclude='*.d' --exclude='*.o' -cf - .
	) | (
		cd "$target"
		tar -xf -
	)
}

build_static_libnuma()
{
	numactl_tar="$WORKDIR/tmp/$NUMACTL_ARCHIVE"
	numactl_src="$WORKDIR/tmp/numactl-$NUMACTL_VERSION"
	LIBNUMA_PREFIX="$WORKDIR/tmp/numactl-prefix"

	fetch_url "$NUMACTL_URL" "$numactl_tar"
	printf '%s  %s\n' "$NUMACTL_SHA256" "$numactl_tar" | sha256sum -c -
	tar -xzf "$numactl_tar" -C "$WORKDIR/tmp"
	if [ ! -x "$numactl_src/configure" ]; then
		echo "numactl-$NUMACTL_VERSION does not contain configure" >&2
		exit 1
	fi
	(
		cd "$numactl_src"
		CC="$EDU_TEST_CC" ./configure --host=aarch64-linux-gnu \
			--disable-shared --enable-static --prefix="$LIBNUMA_PREFIX"
		make
		make install
	)
	if [ ! -s "$LIBNUMA_PREFIX/lib/libnuma.a" ]; then
		echo "failed to build AArch64 static libnuma" >&2
		exit 1
	fi
}

install_rt_tests()
{
	rt_tests_build="$WORKDIR/tmp/rt-tests-$RT_TESTS_VERSION-build"
	rt_tests_src_dest="$WORKDIR/usr/local/src/rt-tests-$RT_TESTS_VERSION"
	rt_tests_bin_dest="$WORKDIR/usr/local/lib/rt-tests/$RT_TESTS_VERSION/bin"

	build_static_libnuma
	copy_rt_tests_source "$rt_tests_src_dest"
	copy_rt_tests_source "$rt_tests_build"
	make -C "$rt_tests_build" clean
	make -C "$rt_tests_build" CROSS_COMPILE="${EDU_TEST_CC%gcc}" \
		no_libcpupower=1 CPPFLAGS="-D_GNU_SOURCE -Isrc/include -I$LIBNUMA_PREFIX/include" \
		LDFLAGS="-static -s -L$LIBNUMA_PREFIX/lib"
	mkdir -p "$rt_tests_bin_dest" "$WORKDIR/usr/local/bin"
	for tool in cyclictest hackbench pip_stress pi_stress pmqtest ptsematest \
		rt-migrate-test signaltest sigwaittest svsematest cyclicdeadline \
		deadline_test queuelat ssdd oslat; do
		if [ ! -x "$rt_tests_build/$tool" ]; then
			echo "rt-tests build did not produce $tool" >&2
			exit 1
		fi
		install -m 0755 "$rt_tests_build/$tool" "$rt_tests_bin_dest/$tool"
		ln -sf "../lib/rt-tests/$RT_TESTS_VERSION/bin/$tool" \
			"$WORKDIR/usr/local/bin/$tool"
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

# stress-ng usage
# ===============
#
# stress-ng --cpu $(nproc) --timeout 300s --metrics-brief
# stress-ng --cpu $(nproc) --cpu-method matrixprod -t 300
# stress-ng --cpu $(nproc) --cpu-method rand -t 300
# stress-ng --cpu $(nproc) --cpu-load 70 -t 300
# stress-ng --vm 2 --vm-bytes 4G --vm-keep --timeout 300s
# stress-ng --vm 1 --vm-bytes 90% --vm-keep -t 300
# stress-ng --context 16 --timeout 300s
# stress-ng --sched 8 --timeout 300s
# stress-ng --class scheduler --timeout 600s
#
# vCPU scheduling:
#
# stress-ng --cpu $(nproc) --cpu-method matrixprod -t 600
# stress-ng --vm $(nproc) --vm-bytes 92% --vm-keep --page-in -t 600
#
# -------------------------------------------------------------------------
#
# VM1 exports isolated virtio-fs roots for the VM2 and VM3 proxy frontends.
#
# Manual virtio-fs smoke test from the BEAU shell:
#   1. vsh 1
#      ls -l /var/beau/vm2 /var/beau/vm3
#      <Ctrl-D>
#   2. vsh 3
#      mount -t virtiofs -o rw proxy-fs /var/beau
#      echo "hello from vm3" > /var/beau/README
#      <Ctrl-D>
#   3. vsh 1
#      cat /var/beau/vm3/README
#
# VM2 and VM3 use separate backend roots so filesystem state cannot cross the
# frontend ownership boundary.
mkdir -p /var/beau/vm2 /var/beau/vm3
chmod 0755 /var /var/beau /var/beau/vm2 /var/beau/vm3 2>/dev/null || true

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
# print "can't open /dev/ttyAMA0". BusyBox getty owns the hvc0 session and
# controlling tty so an interactive shell cannot exit immediately on an
# unowned terminal.
[ -c /dev/console ] || mknod /dev/console c 5 1
[ -c /dev/tty ] || mknod /dev/tty c 5 0

run_getty_on_tty()
{
	tty_name=$1

	[ -c "/dev/$tty_name" ] || return 1
	/sbin/getty -L -n -l /usr/local/bin/beau-console-shell \
		115200 "$tty_name" dumb
}

export PATH

while true; do
	run_getty_on_tty hvc0 || true
	# Bound retries even when getty or the interactive shell exits successfully.
	sleep 1
done
EOF

chmod 0755 "$WORKDIR/init"
mkdir -p "$WORKDIR/var/beau/vm2" "$WORKDIR/var/beau/vm3"
chmod 0755 "$WORKDIR/var" "$WORKDIR/var/beau" \
	"$WORKDIR/var/beau/vm2" "$WORKDIR/var/beau/vm3"
install_alpine_apk i2c-tools community
install_alpine_apk hwdata-pci main
install_alpine_apk pciutils-libs main
install_alpine_apk pciutils main
mkdir -p "$WORKDIR/usr/local/bin"
cat > "$WORKDIR/usr/local/bin/beau-console-shell" <<'EOF'
#!/bin/sh
# getty has already established hvc0 as the controlling terminal.
stty sane rows 24 cols 80 -ixon -ixoff 2>/dev/null || true
TERM=dumb
PS1='\[\033[0;92m\]uos \w\[\033[0m\] '
export PS1 TERM
exec /bin/sh -i
EOF
chmod 0755 "$WORKDIR/usr/local/bin/beau-console-shell"
rm -f "$WORKDIR/usr/local/bin/beau-edu-test" \
	"$WORKDIR/usr/local/bin/beau-rpmsg-test" \
	"$WORKDIR/usr/local/bin/beau-vsock-test"
if ! command -v "$EDU_TEST_CC" >/dev/null 2>&1; then
	echo "$EDU_TEST_CC is required to build initramfs test tools" >&2
	exit 1
fi
"$EDU_TEST_CC" -Os -static -s -Wall -Wextra -o "$WORKDIR/usr/local/bin/vpci" "$EDU_TEST_SRC"
"$EDU_TEST_CC" -Os -static -s -Wall -Wextra -o "$WORKDIR/usr/local/bin/rpmsg" "$RPMSG_TEST_SRC"
"$EDU_TEST_CC" -Os -static -s -Wall -Wextra -o "$WORKDIR/usr/local/bin/vsock" "$VSOCK_TEST_SRC"
install_rt_tests
install_stress_ng_package
install_debug_tools
rm -f "$WORKDIR/usr/bin/stress.sh"
cat > "$WORKDIR/usr/bin/snap.sh" <<'EOF'
#!/bin/sh
# Run a bounded CPU, memory, and scheduler stress-ng workload for BEAU Linux.

set -eu

duration=${1:-120}
case "$duration" in
''|*[!0-9]*|0)
	echo "usage: ${0##*/} [duration_seconds]" >&2
	exit 64
	;;
esac

stress_ng=/usr/local/bin/stress-ng
if [ ! -x "$stress_ng" ]; then
	echo "${0##*/}: stress-ng is unavailable at $stress_ng" >&2
	exit 127
fi

workers=$(nproc 2>/dev/null || printf '%s\n' 1)
case "$workers" in
''|*[!0-9]*|0)
	workers=1
	;;
esac
if [ "$workers" -gt 2 ]; then
	workers=2
fi

echo "${0##*/}: ${duration}s, cpu=${workers}, vm=32M, context=2, sched=1"
exec "$stress_ng" \
	--cpu "$workers" --cpu-load 80 \
	--vm 1 --vm-bytes 32M --vm-keep --page-in \
	--context 2 --sched rr \
	--timeout "${duration}s" --metrics-brief --verify
EOF
chmod 0755 "$WORKDIR/usr/bin/snap.sh"
rm -rf "$WORKDIR/tmp"
(cd "$WORKDIR" && find . -print0 | cpio --null -o --quiet -H newc -R 0:0 | gzip -9 > "$ARCHIVE.tmp")
archive_size=$(stat -c %s "$ARCHIVE.tmp")
if [ "$archive_size" -gt "$INITRAMFS_MAX_SIZE" ]; then
	echo "initramfs size $archive_size exceeds QEMU RAM/FDT limit $INITRAMFS_MAX_SIZE" >&2
	exit 1
fi
mv "$ARCHIVE.tmp" "$ARCHIVE"
