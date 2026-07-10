// SPDX-License-Identifier: GPL-2.0-only
/*
 * BEAU virtio-fs backend for the VM1 <-> virtio_proxy <-> VM2 test path.
 *
 * Principle:
 *
 *   VM2 virtio-fs frontend(rw)
 *        -> BEAU virtio_proxy copies one descriptor chain
 *        -> VM1 backend HVC poll receives opaque FUSE bytes
 *        -> VM1 VFS operates on /var/beau
 *        -> VM1 backend HVC reply copies FUSE bytes back
 *
 * BEAU deliberately does not parse FUSE opcodes. This driver is the protocol
 * endpoint; the hypervisor is only the MMIO transport and isolation boundary.
 */

#include <linux/dcache.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/mount.h>
#include <linux/module.h>
#include <linux/namei.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <uapi/linux/fuse.h>
#include <asm/memory.h>

#include "hcall.h"

#define BEAU_PROXY_FRONTEND_VM2		2U
#define BEAU_PROXY_DEVICE_FS		26U
#define BEAU_PROXY_QUEUE_HIPRIO		0U
#define BEAU_PROXY_QUEUE_REQUEST	1U
#define BEAU_ROOT_NODEID		FUSE_ROOT_ID
#define BEAU_FILE_NODEID_BASE		0x1000ULL
#define BEAU_EXPORT_PATH		"/var/beau"
#define BEAU_PATH_MAX			(sizeof(BEAU_EXPORT_PATH) + NAME_MAX + 2U)
#define BEAU_NODE_MAP_MAX		64U
#define BEAU_FUSE_MAX_PAYLOAD		(BEAU_PROXY_DATA_MAX - sizeof(struct fuse_in_header))

struct beau_backend {
	struct task_struct *thread;
	void *in;
	void *out;
	struct beau_proxy_ioc ioc;
	struct mutex map_lock;
	struct beau_node {
		u64 nodeid;
		char path[BEAU_PATH_MAX];
	} map[BEAU_NODE_MAP_MAX];
};

static struct beau_backend beau_backend;

static int beau_reply(struct beau_proxy_ioc *ioc, void *out, u32 out_len)
{
	ioc->op = BEAU_PROXY_OP_REPLY;
	ioc->out_gpa = virt_to_phys(out);
	ioc->out_len = out_len;
	return beau_hcall_virtio_proxy_backend(ioc);
}

static int beau_reply_empty(struct beau_proxy_ioc *ioc)
{
	/*
	 * Some FUSE requests are one-way notifications. virtio still needs the
	 * descriptor head to be consumed on the used ring, but there is no FUSE
	 * out header and usually no writable descriptor. Report a zero-length
	 * used element so the frontend can retire the request without waiting
	 * for a protocol reply that must not exist.
	 */
	ioc->op = BEAU_PROXY_OP_REPLY;
	ioc->out_gpa = 0;
	ioc->out_len = 0;
	return beau_hcall_virtio_proxy_backend(ioc);
}

static void beau_out_header(void *out, u32 payload_len, const struct fuse_in_header *in,
			    int error)
{
	struct fuse_out_header *hdr = out;

	memset(out, 0, sizeof(*hdr) + max(payload_len, 0U));
	hdr->len = sizeof(*hdr) + payload_len;
	hdr->error = error;
	hdr->unique = in->unique;
}

static int beau_reply_error(struct beau_proxy_ioc *ioc, const struct fuse_in_header *in,
			    int error)
{
	beau_out_header(beau_backend.out, 0, in, error);
	return beau_reply(ioc, beau_backend.out, sizeof(struct fuse_out_header));
}

static u32 beau_payload_cap(const struct beau_proxy_ioc *ioc, u32 requested)
{
	u32 cap = BEAU_PROXY_DATA_MAX - sizeof(struct fuse_out_header);

	if (ioc->out_len > sizeof(struct fuse_out_header))
		cap = min(cap, ioc->out_len - sizeof(struct fuse_out_header));
	else
		cap = 0;

	return min(requested, cap);
}

static u64 beau_nodeid_from_name(const char *name, int len)
{
	u64 hash = 1469598103934665603ULL;
	int i;

	for (i = 0; i < len; i++) {
		hash ^= (u8)name[i];
		hash *= 1099511628211ULL;
	}

	return BEAU_FILE_NODEID_BASE | (hash & 0x0000ffffffffffffULL);
}

static bool beau_bad_name(const char *name, int len)
{
	int i;

	if (len <= 0 || len > NAME_MAX)
		return true;

	for (i = 0; i < len; i++) {
		if (name[i] == '/')
			return true;
	}

	return false;
}

static int beau_build_path(u64 nodeid, const char *name, int len, char *path,
			   size_t path_len)
{
	int i;

	if (nodeid == BEAU_ROOT_NODEID && name) {
		if (beau_bad_name(name, len))
			return -EINVAL;
		return scnprintf(path, path_len, "%s/%.*s", BEAU_EXPORT_PATH, len, name);
	}
	if (nodeid == BEAU_ROOT_NODEID)
		return scnprintf(path, path_len, "%s", BEAU_EXPORT_PATH);

	mutex_lock(&beau_backend.map_lock);
	for (i = 0; i < BEAU_NODE_MAP_MAX; i++) {
		if (beau_backend.map[i].nodeid == nodeid) {
			strscpy(path, beau_backend.map[i].path, path_len);
			mutex_unlock(&beau_backend.map_lock);
			return 0;
		}
	}
	mutex_unlock(&beau_backend.map_lock);
	return -ENOENT;
}

static void beau_remember_node(u64 nodeid, const char *path)
{
	int slot = -1;
	int i;

	mutex_lock(&beau_backend.map_lock);
	for (i = 0; i < BEAU_NODE_MAP_MAX; i++) {
		if (beau_backend.map[i].nodeid == nodeid) {
			slot = i;
			break;
		}
		if (slot < 0 && beau_backend.map[i].nodeid == 0)
			slot = i;
	}
	if (slot >= 0) {
		beau_backend.map[slot].nodeid = nodeid;
		strscpy(beau_backend.map[slot].path, path,
			sizeof(beau_backend.map[slot].path));
	}
	mutex_unlock(&beau_backend.map_lock);
}

static void beau_fill_attr(struct fuse_attr *attr, const struct kstat *st, u64 nodeid)
{
	memset(attr, 0, sizeof(*attr));
	attr->ino = nodeid;
	attr->size = st->size;
	attr->blocks = st->blocks;
	attr->atime = st->atime.tv_sec;
	attr->mtime = st->mtime.tv_sec;
	attr->ctime = st->ctime.tv_sec;
	attr->atimensec = st->atime.tv_nsec;
	attr->mtimensec = st->mtime.tv_nsec;
	attr->ctimensec = st->ctime.tv_nsec;
	attr->mode = st->mode;
	attr->nlink = st->nlink;
	attr->uid = from_kuid(&init_user_ns, st->uid);
	attr->gid = from_kgid(&init_user_ns, st->gid);
	attr->rdev = st->rdev;
	attr->blksize = st->blksize;
}

static int beau_stat_path(const char *path_name, struct kstat *st)
{
	struct path path;
	int ret;

	ret = kern_path(path_name, LOOKUP_FOLLOW, &path);
	if (ret)
		return ret;

	ret = vfs_getattr(&path, st, STATX_BASIC_STATS, AT_STATX_SYNC_AS_STAT);
	path_put(&path);
	return ret;
}

static int beau_reply_entry(struct beau_proxy_ioc *ioc, const struct fuse_in_header *in,
			    const char *path, u64 nodeid)
{
	struct fuse_entry_out *entry;
	struct kstat st;
	int ret;

	ret = beau_stat_path(path, &st);
	if (ret)
		return beau_reply_error(ioc, in, ret);

	beau_remember_node(nodeid, path);
	beau_out_header(beau_backend.out, sizeof(*entry), in, 0);
	entry = beau_backend.out + sizeof(struct fuse_out_header);
	entry->nodeid = nodeid;
	entry->generation = 1;
	entry->entry_valid = 1;
	entry->attr_valid = 1;
	beau_fill_attr(&entry->attr, &st, nodeid);

	return beau_reply(ioc, beau_backend.out,
			  sizeof(struct fuse_out_header) + sizeof(*entry));
}

static int beau_reply_create(struct beau_proxy_ioc *ioc, const struct fuse_in_header *in,
			     const char *path, u64 nodeid)
{
	struct fuse_entry_out *entry;
	struct fuse_open_out *open_out;
	struct kstat st;
	int ret;

	ret = beau_stat_path(path, &st);
	if (ret)
		return beau_reply_error(ioc, in, ret);

	beau_remember_node(nodeid, path);
	beau_out_header(beau_backend.out, sizeof(*entry) + sizeof(*open_out), in, 0);
	entry = beau_backend.out + sizeof(struct fuse_out_header);
	open_out = (void *)(entry + 1);
	entry->nodeid = nodeid;
	entry->generation = 1;
	entry->entry_valid = 1;
	entry->attr_valid = 1;
	beau_fill_attr(&entry->attr, &st, nodeid);
	open_out->fh = nodeid;
	open_out->open_flags = FOPEN_KEEP_CACHE;

	return beau_reply(ioc, beau_backend.out,
			  sizeof(struct fuse_out_header) + sizeof(*entry) +
			  sizeof(*open_out));
}

static int beau_reply_attr(struct beau_proxy_ioc *ioc, const struct fuse_in_header *in,
			   const char *path)
{
	struct fuse_attr_out *out_attr;
	struct kstat st;
	int ret;

	ret = beau_stat_path(path, &st);
	if (ret)
		return beau_reply_error(ioc, in, ret);

	beau_out_header(beau_backend.out, sizeof(*out_attr), in, 0);
	out_attr = beau_backend.out + sizeof(struct fuse_out_header);
	out_attr->attr_valid = 1;
	beau_fill_attr(&out_attr->attr, &st, in->nodeid);

	return beau_reply(ioc, beau_backend.out,
			  sizeof(struct fuse_out_header) + sizeof(*out_attr));
}

static int beau_handle_init(struct beau_proxy_ioc *ioc, const struct fuse_in_header *in)
{
	struct fuse_init_out *init_out;

	beau_out_header(beau_backend.out, sizeof(*init_out), in, 0);
	init_out = beau_backend.out + sizeof(struct fuse_out_header);
	init_out->major = FUSE_KERNEL_VERSION;
	init_out->minor = min_t(u32, FUSE_KERNEL_MINOR_VERSION, 31);
	init_out->max_readahead = 0;
	init_out->flags = FUSE_ASYNC_READ | FUSE_BIG_WRITES | FUSE_NO_OPEN_SUPPORT |
			  FUSE_NO_OPENDIR_SUPPORT;
	init_out->max_background = 8;
	init_out->congestion_threshold = 4;
	init_out->max_write = BEAU_FUSE_MAX_PAYLOAD - sizeof(struct fuse_write_in);
	init_out->time_gran = 1;
	init_out->max_pages = 1;

	return beau_reply(ioc, beau_backend.out,
			  sizeof(struct fuse_out_header) + sizeof(*init_out));
}

static int beau_handle_getattr(struct beau_proxy_ioc *ioc, const struct fuse_in_header *in)
{
	struct fuse_attr_out *out_attr;
	struct kstat st;
	char path[BEAU_PATH_MAX];
	int ret;

	ret = beau_build_path(in->nodeid, NULL, 0, path, sizeof(path));
	if (ret < 0)
		return beau_reply_error(ioc, in, ret);
	ret = beau_stat_path(path, &st);
	if (ret)
		return beau_reply_error(ioc, in, ret);

	beau_out_header(beau_backend.out, sizeof(*out_attr), in, 0);
	out_attr = beau_backend.out + sizeof(struct fuse_out_header);
	out_attr->attr_valid = 1;
	beau_fill_attr(&out_attr->attr, &st, in->nodeid);

	return beau_reply(ioc, beau_backend.out,
			  sizeof(struct fuse_out_header) + sizeof(*out_attr));
}

static int beau_handle_lookup(struct beau_proxy_ioc *ioc, const struct fuse_in_header *in)
{
	const char *name = (const char *)(in + 1);
	int len = strnlen(name, in->len - sizeof(*in));
	char path[BEAU_PATH_MAX];
	u64 nodeid;
	int ret;

	if (in->nodeid != BEAU_ROOT_NODEID || len <= 0)
		return beau_reply_error(ioc, in, -ENOENT);

	ret = beau_build_path(in->nodeid, name, len, path, sizeof(path));
	if (ret < 0)
		return beau_reply_error(ioc, in, ret);

	nodeid = beau_nodeid_from_name(name, len);
	return beau_reply_entry(ioc, in, path, nodeid);
}

struct beau_readdir_ctx {
	struct dir_context ctx;
	u8 *buf;
	u32 size;
	u32 used;
	u64 next_off;
};

static bool beau_filldir(struct dir_context *ctx, const char *name, int namlen,
			 loff_t offset, u64 ino, unsigned int d_type)
{
	struct beau_readdir_ctx *bctx = container_of(ctx, struct beau_readdir_ctx, ctx);
	struct fuse_dirent *de;
	u32 entsize = FUSE_DIRENT_ALIGN(FUSE_NAME_OFFSET + namlen);

	if (bctx->used + entsize > bctx->size)
		return false;

	de = (struct fuse_dirent *)(bctx->buf + bctx->used);
	memset(de, 0, entsize);
	de->ino = ino ? ino : beau_nodeid_from_name(name, namlen);
	de->off = ++bctx->next_off;
	de->namelen = namlen;
	de->type = d_type;
	memcpy(de->name, name, namlen);
	bctx->used += entsize;
	return true;
}

static int beau_handle_readdir(struct beau_proxy_ioc *ioc, const struct fuse_in_header *in)
{
	const struct fuse_read_in *rin = (const void *)(in + 1);
	struct beau_readdir_ctx bctx;
	struct file *file;
	u32 payload;
	int ret;

	file = filp_open(BEAU_EXPORT_PATH, O_RDONLY | O_DIRECTORY, 0);
	if (IS_ERR(file))
		return beau_reply_error(ioc, in, PTR_ERR(file));

	/*
	 * iterate_dir() initializes ctx->pos from file->f_pos. The backend opens
	 * a fresh host file for every FUSE_READDIR request, so file->f_pos would
	 * otherwise reset to zero and the frontend would see the same entries
	 * forever instead of reaching EOF.
	 */
	file->f_pos = rin->offset;
	payload = beau_payload_cap(ioc, rin->size);
	beau_out_header(beau_backend.out, 0, in, 0);
	bctx.buf = beau_backend.out + sizeof(struct fuse_out_header);
	bctx.size = payload;
	bctx.used = 0;
	bctx.next_off = rin->offset;
	bctx.ctx.actor = beau_filldir;
	bctx.ctx.pos = rin->offset;
	ret = iterate_dir(file, &bctx.ctx);
	filp_close(file, NULL);
	if (ret < 0)
		return beau_reply_error(ioc, in, ret);

	((struct fuse_out_header *)beau_backend.out)->len =
		sizeof(struct fuse_out_header) + bctx.used;
	pr_debug("BEAU virtio-fs readdir offset=%llu size=%u used=%u outcap=%u\n",
		 rin->offset, rin->size, bctx.used, ioc->out_len);
	return beau_reply(ioc, beau_backend.out,
			  sizeof(struct fuse_out_header) + bctx.used);
}

static int beau_handle_open(struct beau_proxy_ioc *ioc, const struct fuse_in_header *in)
{
	struct fuse_open_out *open_out;

	beau_out_header(beau_backend.out, sizeof(*open_out), in, 0);
	open_out = beau_backend.out + sizeof(struct fuse_out_header);
	open_out->fh = in->nodeid;
	open_out->open_flags = FOPEN_KEEP_CACHE;

	return beau_reply(ioc, beau_backend.out,
			  sizeof(struct fuse_out_header) + sizeof(*open_out));
}

static int beau_handle_read(struct beau_proxy_ioc *ioc, const struct fuse_in_header *in)
{
	const struct fuse_read_in *rin = (const void *)(in + 1);
	struct file *file;
	char path[BEAU_PATH_MAX];
	u32 payload;
	ssize_t got;
	loff_t pos = rin->offset;
	int ret;

	ret = beau_build_path(in->nodeid, NULL, 0, path, sizeof(path));
	if (ret < 0)
		return beau_reply_error(ioc, in, ret);

	file = filp_open(path, O_RDONLY, 0);
	if (IS_ERR(file))
		return beau_reply_error(ioc, in, PTR_ERR(file));

	payload = beau_payload_cap(ioc, rin->size);
	beau_out_header(beau_backend.out, 0, in, 0);
	got = kernel_read(file, beau_backend.out + sizeof(struct fuse_out_header),
			  payload, &pos);
	filp_close(file, NULL);
	if (got < 0)
		return beau_reply_error(ioc, in, got);

	((struct fuse_out_header *)beau_backend.out)->len =
		sizeof(struct fuse_out_header) + got;
	pr_debug("BEAU virtio-fs read node=%llu off=%llu size=%u got=%zd outcap=%u\n",
		 in->nodeid, rin->offset, rin->size, got, ioc->out_len);
	return beau_reply(ioc, beau_backend.out, sizeof(struct fuse_out_header) + got);
}

static int beau_create_file(struct beau_proxy_ioc *ioc, const struct fuse_in_header *in,
			    const char *name, int len, umode_t mode, int flags,
			    bool create_open_reply)
{
	struct file *file;
	char path[BEAU_PATH_MAX];
	u64 nodeid;
	int ret;

	if (in->nodeid != BEAU_ROOT_NODEID || beau_bad_name(name, len))
		return beau_reply_error(ioc, in, -EINVAL);

	ret = beau_build_path(in->nodeid, name, len, path, sizeof(path));
	if (ret < 0)
		return beau_reply_error(ioc, in, ret);

	file = filp_open(path, flags | O_CREAT | O_LARGEFILE, mode & 0777);
	if (IS_ERR(file))
		return beau_reply_error(ioc, in, PTR_ERR(file));
	filp_close(file, NULL);

	nodeid = beau_nodeid_from_name(name, len);
	if (create_open_reply)
		return beau_reply_create(ioc, in, path, nodeid);

	return beau_reply_entry(ioc, in, path, nodeid);
}

static int beau_handle_create(struct beau_proxy_ioc *ioc, const struct fuse_in_header *in)
{
	const struct fuse_create_in *cin = (const void *)(in + 1);
	const char *name;
	int len;
	int flags;

	if (in->len <= sizeof(*in) + sizeof(*cin))
		return beau_reply_error(ioc, in, -EINVAL);

	name = (const char *)(cin + 1);
	len = strnlen(name, in->len - sizeof(*in) - sizeof(*cin));
	flags = cin->flags & O_ACCMODE;
	if (flags == O_RDONLY)
		flags = O_RDWR;

	if (cin->flags & O_TRUNC)
		flags |= O_TRUNC;

	return beau_create_file(ioc, in, name, len, cin->mode, flags, true);
}

static int beau_handle_mknod(struct beau_proxy_ioc *ioc, const struct fuse_in_header *in)
{
	const struct fuse_mknod_in *min = (const void *)(in + 1);
	const char *name;
	int len;
	umode_t mode;

	if (in->len <= sizeof(*in) + sizeof(*min))
		return beau_reply_error(ioc, in, -EINVAL);

	name = (const char *)(min + 1);
	len = strnlen(name, in->len - sizeof(*in) - sizeof(*min));
	mode = min->mode;
	if (mode == 0)
		mode = 0644;
	if (!S_ISREG(mode))
		return beau_reply_error(ioc, in, -EOPNOTSUPP);

	return beau_create_file(ioc, in, name, len, mode, O_RDWR | O_EXCL, false);
}

static int beau_handle_write(struct beau_proxy_ioc *ioc, const struct fuse_in_header *in)
{
	const struct fuse_write_in *win = (const void *)(in + 1);
	const void *data = win + 1;
	struct fuse_write_out *write_out;
	struct file *file;
	char path[BEAU_PATH_MAX];
	u32 avail;
	loff_t pos;
	ssize_t written;
	int ret;

	if (in->len < sizeof(*in) + sizeof(*win))
		return beau_reply_error(ioc, in, -EINVAL);

	avail = in->len - sizeof(*in) - sizeof(*win);
	if (win->size > avail)
		return beau_reply_error(ioc, in, -EINVAL);

	pos = win->offset;
	ret = beau_build_path(in->nodeid, NULL, 0, path, sizeof(path));
	if (ret < 0)
		return beau_reply_error(ioc, in, ret);

	file = filp_open(path, O_WRONLY | O_LARGEFILE, 0);
	if (IS_ERR(file))
		return beau_reply_error(ioc, in, PTR_ERR(file));

	written = kernel_write(file, data, win->size, &pos);
	filp_close(file, NULL);
	if (written < 0)
		return beau_reply_error(ioc, in, written);

	beau_out_header(beau_backend.out, sizeof(*write_out), in, 0);
	write_out = beau_backend.out + sizeof(struct fuse_out_header);
	write_out->size = written;

	return beau_reply(ioc, beau_backend.out,
			  sizeof(struct fuse_out_header) + sizeof(*write_out));
}

static int beau_handle_setattr(struct beau_proxy_ioc *ioc, const struct fuse_in_header *in)
{
	const struct fuse_setattr_in *sin = (const void *)(in + 1);
	struct iattr attr = { };
	struct path path;
	char path_name[BEAU_PATH_MAX];
	int ret;

	if (in->len < sizeof(*in) + sizeof(*sin))
		return beau_reply_error(ioc, in, -EINVAL);

	ret = beau_build_path(in->nodeid, NULL, 0, path_name, sizeof(path_name));
	if (ret < 0)
		return beau_reply_error(ioc, in, ret);

	ret = kern_path(path_name, LOOKUP_FOLLOW, &path);
	if (ret)
		return beau_reply_error(ioc, in, ret);

	if (sin->valid & FATTR_MODE) {
		attr.ia_valid |= ATTR_MODE;
		attr.ia_mode = sin->mode;
	}
	if (sin->valid & FATTR_SIZE) {
		ret = vfs_truncate(&path, sin->size);
		if (ret)
			goto out;
	}
	if (sin->valid & FATTR_ATIME) {
		attr.ia_valid |= ATTR_ATIME;
		if (!(sin->valid & FATTR_ATIME_NOW)) {
			attr.ia_valid |= ATTR_ATIME_SET;
			attr.ia_atime.tv_sec = sin->atime;
			attr.ia_atime.tv_nsec = sin->atimensec;
		}
	}
	if (sin->valid & FATTR_MTIME) {
		attr.ia_valid |= ATTR_MTIME;
		if (!(sin->valid & FATTR_MTIME_NOW)) {
			attr.ia_valid |= ATTR_MTIME_SET;
			attr.ia_mtime.tv_sec = sin->mtime;
			attr.ia_mtime.tv_nsec = sin->mtimensec;
		}
	}

	if (attr.ia_valid) {
		ret = mnt_want_write(path.mnt);
		if (ret)
			goto out;
		inode_lock(d_inode(path.dentry));
		ret = notify_change(mnt_idmap(path.mnt), path.dentry, &attr, NULL);
		inode_unlock(d_inode(path.dentry));
		mnt_drop_write(path.mnt);
		if (ret)
			goto out;
	}

	ret = 0;
out:
	path_put(&path);
	if (ret)
		return beau_reply_error(ioc, in, ret);

	return beau_reply_attr(ioc, in, path_name);
}

static int beau_handle_one(struct beau_proxy_ioc *ioc)
{
	const struct fuse_in_header *in = beau_backend.in;
	int ret;

	if (ioc->in_len < sizeof(*in) || in->len > ioc->in_len)
		return -EINVAL;
	if (in->len < sizeof(*in))
		return beau_reply_error(ioc, in, -EINVAL);

	switch (in->opcode) {
	case FUSE_INIT:
		ret = beau_handle_init(ioc, in);
		break;
	case FUSE_LOOKUP:
		ret = beau_handle_lookup(ioc, in);
		break;
	case FUSE_GETATTR:
		ret = beau_handle_getattr(ioc, in);
		break;
	case FUSE_SETATTR:
		ret = beau_handle_setattr(ioc, in);
		break;
	case FUSE_MKNOD:
		ret = beau_handle_mknod(ioc, in);
		break;
	case FUSE_CREATE:
		ret = beau_handle_create(ioc, in);
		break;
	case FUSE_OPEN:
	case FUSE_OPENDIR:
		ret = beau_handle_open(ioc, in);
		break;
	case FUSE_READDIR:
		ret = beau_handle_readdir(ioc, in);
		break;
	case FUSE_READ:
		ret = beau_handle_read(ioc, in);
		break;
	case FUSE_WRITE:
		ret = beau_handle_write(ioc, in);
		break;
	case FUSE_FORGET:
	case FUSE_BATCH_FORGET:
		ret = beau_reply_empty(ioc);
		break;
	case FUSE_RELEASE:
	case FUSE_RELEASEDIR:
	case FUSE_FLUSH:
	case FUSE_DESTROY:
		ret = beau_reply_error(ioc, in, 0);
		break;
	default:
		ret = beau_reply_error(ioc, in, -EROFS);
		break;
	}
	pr_debug("BEAU virtio-fs opcode=%u unique=%llu node=%llu q=%u in=%u outcap=%u ret=%d\n",
		 in->opcode, in->unique, in->nodeid, ioc->queue_id, ioc->in_len,
		 ioc->out_len, ret);
	return ret;
}

static int beau_backend_thread(void *data)
{
	struct beau_proxy_ioc *ioc = &beau_backend.ioc;
	static const u16 queues[] = {
		BEAU_PROXY_QUEUE_HIPRIO,
		BEAU_PROXY_QUEUE_REQUEST,
	};
	long ret;
	unsigned int idx = 0;
	unsigned int idle_polls = 0U;

	memset(ioc, 0, sizeof(*ioc));
	ioc->op = BEAU_PROXY_OP_REGISTER;
	ioc->device_id = BEAU_PROXY_DEVICE_FS;
	ioc->frontend_vmid = BEAU_PROXY_FRONTEND_VM2;
	while (!kthread_should_stop()) {
		ret = beau_hcall_virtio_proxy_backend(ioc);
		if (!ret)
			break;
		msleep(1000);
	}

	while (!kthread_should_stop()) {
		memset(ioc, 0, sizeof(*ioc));
		ioc->op = BEAU_PROXY_OP_POLL;
		ioc->device_id = BEAU_PROXY_DEVICE_FS;
		ioc->frontend_vmid = BEAU_PROXY_FRONTEND_VM2;
		ioc->queue_id = queues[idx++ % ARRAY_SIZE(queues)];
		ioc->in_gpa = virt_to_phys(beau_backend.in);
		ioc->in_len = BEAU_PROXY_DATA_MAX;
		ret = beau_hcall_virtio_proxy_backend(ioc);
		if (ret) {
			beau_proxy_poll_idle_delay(&idle_polls);
			continue;
		}
		beau_proxy_poll_active(&idle_polls);
		ret = beau_handle_one(ioc);
		if (ret)
			beau_proxy_poll_idle_delay(&idle_polls);
	}

	return 0;
}

static int __init beau_virtiofs_backend_init(void)
{
	const char *model = NULL;

	if (!of_root || of_property_read_string(of_root, "model", &model) ||
	    !model || !strstr(model, "VM1"))
		return 0;

	beau_backend.in = kzalloc(BEAU_PROXY_DATA_MAX, GFP_KERNEL);
	beau_backend.out = kzalloc(BEAU_PROXY_DATA_MAX, GFP_KERNEL);
	if (!beau_backend.in || !beau_backend.out)
		return -ENOMEM;
	mutex_init(&beau_backend.map_lock);

	beau_backend.thread = kthread_run(beau_backend_thread, NULL,
					  "beau-virtiofs-backend");
	if (IS_ERR(beau_backend.thread))
		return PTR_ERR(beau_backend.thread);

	pr_info("BEAU virtio-fs backend started for %s\n", BEAU_EXPORT_PATH);
	return 0;
}

static void __exit beau_virtiofs_backend_exit(void)
{
	if (beau_backend.thread && !IS_ERR(beau_backend.thread))
		kthread_stop(beau_backend.thread);
	kfree(beau_backend.in);
	kfree(beau_backend.out);
}

late_initcall(beau_virtiofs_backend_init);
module_exit(beau_virtiofs_backend_exit);

MODULE_DESCRIPTION("BEAU VM1 virtio-fs backend");
MODULE_LICENSE("GPL");
