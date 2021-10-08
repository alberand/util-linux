/*
 * lsfd(1) - list file descriptors
 *
 * Copyright (C) 2021 Red Hat, Inc. All rights reserved.
 * Written by Masatake YAMATO <yamato@redhat.com>
 *            Karel Zak <kzak@redhat.com>
 *
 * Very generally based on lsof(8) by Victor A. Abell <abe@purdue.edu>
 * It supports multiple OSes. lsfd specializes to Linux.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it would be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <stdio.h>
#include <sys/types.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <unistd.h>
#include <getopt.h>

#include <sys/syscall.h>
#include <linux/kcmp.h>
static int kcmp(pid_t pid1, pid_t pid2, int type,
		unsigned long idx1, unsigned long idx2)
{
	return syscall(SYS_kcmp, pid1, pid2, type, idx1, idx2);
}

#include "c.h"
#include "nls.h"
#include "xalloc.h"
#include "list.h"
#include "closestream.h"
#include "strutils.h"
#include "procfs.h"
#include "fileutils.h"
#include "idcache.h"
#include "pathnames.h"

#include "libsmartcols.h"

#include "lsfd.h"
#include "lsfd-filter.h"

/*
 * /proc/$pid/mountinfo entries
 */
struct nodev {
	struct list_head nodevs;
	unsigned long minor;
	char *filesystem;
};

struct nodev_table {
#define NODEV_TABLE_SIZE 97
	struct list_head tables[NODEV_TABLE_SIZE];
} nodev_table;

struct name_manager {
	struct idcache *cache;
	unsigned long next_id;
};

/*
 * Column related stuffs
 */

/* column names */
struct colinfo {
	const char *name;
	double whint;
	int flags;
	int json_type;
	const char *help;
};

/* columns descriptions */
static struct colinfo infos[] = {
	[COL_ASSOC]   = { "ASSOC",    0, SCOLS_FL_RIGHT, SCOLS_JSON_STRING,
		N_("association between file and process") },
	[COL_CHRDRV]  = { "CHRDRV",   0, SCOLS_FL_RIGHT, SCOLS_JSON_STRING,
		N_("character device driver name resolved by /proc/devices") },
	[COL_COMMAND] = { "COMMAND",0.3, SCOLS_FL_TRUNC, SCOLS_JSON_STRING,
		N_("command of the process opening the file") },
	[COL_DELETED] = { "DELETED",  0, SCOLS_FL_RIGHT, SCOLS_JSON_BOOLEAN,
		N_("reachability from the file system") },
	[COL_DEV]     = { "DEV",      0, SCOLS_FL_RIGHT, SCOLS_JSON_STRING,
		N_("ID of device containing file") },
	[COL_DEVTYPE] = { "DEVTYPE",  0, SCOLS_FL_RIGHT, SCOLS_JSON_STRING,
		N_("device type (blk, char, or nodev)") },
	[COL_FLAGS]   = { "FLAGS",    0, SCOLS_FL_RIGHT, SCOLS_JSON_STRING,
		N_("flags specified when opening the file") },
	[COL_FD]      = { "FD",       0, SCOLS_FL_RIGHT, SCOLS_JSON_NUMBER,
		N_("file descriptor for the file") },
	[COL_INODE]   = { "INODE",    0, SCOLS_FL_RIGHT, SCOLS_JSON_NUMBER,
		N_("inode number") },
	[COL_MAJMIN]  = { "MAJ:MIN",  0, SCOLS_FL_RIGHT, SCOLS_JSON_STRING,
		N_("device ID for special, or ID of device containing file") },
	[COL_MAPLEN]  = { "MAPLEN",   0, SCOLS_FL_RIGHT, SCOLS_JSON_NUMBER,
		N_("length of file mapping (in page)") },
	[COL_MISCDEV] = { "MISCDEV",  0, SCOLS_FL_RIGHT, SCOLS_JSON_STRING,
		N_("misc character device name resolved by /proc/misc") },
	[COL_MNT_ID]  = { "MNTID",    0, SCOLS_FL_RIGHT, SCOLS_JSON_NUMBER,
		N_("mount id") },
	[COL_MODE]    = { "MODE",     0, SCOLS_FL_RIGHT, SCOLS_JSON_STRING,
		N_("access mode (rwx)") },
	[COL_NAME]    = { "NAME",   0.4, SCOLS_FL_TRUNC, SCOLS_JSON_STRING,
		N_("name of the file") },
	[COL_NLINK]   = { "NLINK",    0, SCOLS_FL_RIGHT, SCOLS_JSON_NUMBER,
		N_("link count") },
	[COL_PID]     = { "PID",      5, SCOLS_FL_RIGHT, SCOLS_JSON_NUMBER,
		N_("PID of the process opening the file") },
	[COL_PARTITION]={ "PARTITION",0, SCOLS_FL_RIGHT, SCOLS_JSON_STRING,
		N_("block device name resolved by /proc/partition") },
	[COL_POS]     = { "POS",      5, SCOLS_FL_RIGHT, SCOLS_JSON_NUMBER,
		N_("file position") },
	[COL_PROTONAME]={ "PROTONAME",0, SCOLS_FL_RIGHT, SCOLS_JSON_STRING,
		N_("protocol name") },
	[COL_RDEV]    = { "RDEV",     0, SCOLS_FL_RIGHT, SCOLS_JSON_STRING,
		N_("device ID (if special file)") },
	[COL_SIZE]    = { "SIZE",     4, SCOLS_FL_RIGHT, SCOLS_JSON_NUMBER,
		N_("file size"), },
	[COL_SOURCE] = { "SOURCE",  0, SCOLS_FL_RIGHT, SCOLS_JSON_STRING,
		N_("file system, partition, or device containing file") },
	[COL_TID]    = { "TID",       5, SCOLS_FL_RIGHT, SCOLS_JSON_NUMBER,
		N_("thread ID of the process opening the file") },
	[COL_TYPE]    = { "TYPE",     0, SCOLS_FL_RIGHT, SCOLS_JSON_STRING,
		N_("file type") },
	[COL_UID]     = { "UID",      0, SCOLS_FL_RIGHT, SCOLS_JSON_NUMBER,
		N_("user ID number") },
	[COL_USER]    = { "USER",     0, SCOLS_FL_RIGHT, SCOLS_JSON_STRING,
		N_("user of the process") },
};

static const int default_columns[] = {
	COL_COMMAND,
	COL_PID,
	COL_USER,
	COL_ASSOC,
	COL_MODE,
	COL_TYPE,
	COL_SOURCE,
	COL_MNT_ID,
	COL_INODE,
	COL_NAME,
};

static const int default_threads_columns[] = {
	COL_COMMAND,
	COL_PID,
	COL_TID,
	COL_USER,
	COL_ASSOC,
	COL_MODE,
	COL_TYPE,
	COL_SOURCE,
	COL_MNT_ID,
	COL_INODE,
	COL_NAME,
};

static int columns[ARRAY_SIZE(infos) * 2] = {-1};
static size_t ncolumns;

static ino_t *mnt_namespaces;
static size_t nspaces;

struct lsfd_control {
	struct libscols_table *tb;		/* output */
	struct list_head procs;			/* list of all processes */
	const char *sysroot;			/* default is NULL */

	unsigned int	noheadings : 1,
			raw : 1,
			json : 1,
			notrunc : 1,
			threads : 1;

	struct lsfd_filter *filter;
};

static int column_name_to_id(const char *name, size_t namesz)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(infos); i++) {
		const char *cn = infos[i].name;

		if (!strncasecmp(name, cn, namesz) && !*(cn + namesz))
			return i;
	}
	warnx(_("unknown column: %s"), name);

	return LSFD_FILTER_UNKNOWN_COL_ID;
}

static int column_name_to_id_cb(const char *name, void *data __attribute__((__unused__)))
{
	return column_name_to_id(name, strlen(name));
}

static int get_column_id(int num)
{
	assert(num >= 0);
	assert((size_t) num < ncolumns);
	assert(columns[num] < (int) ARRAY_SIZE(infos));

	return columns[num];
}

static const struct colinfo *get_column_info(int num)
{
	return &infos[ get_column_id(num) ];
}

static struct libscols_column *add_column(struct libscols_table *tb, const struct colinfo *col)
{
	struct libscols_column *cl;
	int flags = col->flags;

	cl = scols_table_new_column(tb, col->name, col->whint, flags);
	if (cl)
		scols_column_set_json_type(cl, col->json_type);

	return cl;
}

static struct libscols_column *add_column_by_id_cb(struct libscols_table *tb, int colid, void *data)
{
	if (ncolumns >= ARRAY_SIZE(columns))
		errx(EXIT_FAILURE, _("too many columns are added via filter expression"));

	assert(colid < LSFD_N_COLS);

	struct libscols_column *cl = add_column(tb, infos + colid);
	if (!cl)
		err(EXIT_FAILURE, _("failed to allocate output column"));
	columns[ncolumns++] = colid;

	if (colid == COL_TID) {
		struct lsfd_control *ctl = data;
		ctl->threads = 1;
	}

	return cl;
}

static int has_mnt_ns(ino_t id)
{
	size_t i;

	for (i = 0; i < nspaces; i++) {
		if (mnt_namespaces[i] == id)
			return 1;
	}
	return 0;
}

static void add_mnt_ns(ino_t id)
{
	size_t nmax = 0;

	if (nspaces)
		nmax = (nspaces + 16) / 16 * 16;
	if (nmax <= nspaces + 1) {
		nmax += 16;
		mnt_namespaces = xrealloc(mnt_namespaces,
					sizeof(ino_t) * nmax);
	}
	mnt_namespaces[nspaces++] = id;
}

static const struct file_class *stat2class(struct stat *sb)
{
	assert(sb);

	switch (sb->st_mode & S_IFMT) {
	case S_IFCHR:
		return &cdev_class;
	case S_IFBLK:
		return &bdev_class;
	case S_IFSOCK:
		return &sock_class;
	case S_IFIFO:
		return &fifo_class;
	case S_IFLNK:
	case S_IFREG:
	case S_IFDIR:
		return &file_class;
	default:
		break;
	}

	return &unkn_class;
}

static struct file *new_file(struct proc *proc, const struct file_class *class)
{
	struct file *file;

	file = xcalloc(1, class->size);
	file->proc = proc;

	INIT_LIST_HEAD(&file->files);
	list_add_tail(&file->files, &proc->files);

	return file;
}

static struct file *copy_file(struct file *old)
{
	struct file *file = xcalloc(1, old->class->size);

	INIT_LIST_HEAD(&file->files);
	file->proc = old->proc;
	list_add_tail(&file->files, &old->proc->files);

	file->class = old->class;
	file->association = old->association;
	file->name = xstrdup(old->name);
	file->stat = old->stat;

	return file;
}

static void file_set_path(struct file *file, struct stat *sb, const char *name, int association)
{
	const struct file_class *class = stat2class(sb);

	assert(class);

	file->class = class;
	file->association = association;
	file->name = xstrdup(name);
	file->stat = *sb;
}

static void file_init_content(struct file *file)
{
	if (file->class && file->class->initialize_content)
		file->class->initialize_content(file);
}

static void free_file(struct file *file)
{
	const struct file_class *class = file->class;

	while (class) {
		if (class->free_content)
			class->free_content(file);
		class = class->super;
	}
	free(file);
}


static struct proc *new_process(pid_t pid, struct proc *leader)
{
	struct proc *proc = xcalloc(1, sizeof(*proc));

	proc->pid  = pid;
	proc->leader = leader? leader: proc;
	proc->command = NULL;

	INIT_LIST_HEAD(&proc->files);
	INIT_LIST_HEAD(&proc->procs);

	return proc;
}

static void free_proc(struct proc *proc)
{
	list_free(&proc->files, struct file, files, free_file);

	free(proc->command);
	free(proc);
}

static void read_fdinfo(struct file *file, FILE *fdinfo)
{
	char buf[1024];

	while (fgets(buf, sizeof(buf), fdinfo)) {
		const struct file_class *class;
		char *val = strchr(buf, ':');

		if (!val)
			continue;
		*val++ = '\0';	/* terminate key */

		val = (char *) skip_space(val);
		rtrim_whitespace((unsigned char *) val);

		class = file->class;
		while (class) {
			if (class->handle_fdinfo
			    && class->handle_fdinfo(file, buf, val))
				break;
			class = class->super;
		}
	}
}

static struct file *collect_file_symlink(struct path_cxt *pc,
					 struct proc *proc,
					 const char *name,
					 int assoc)
{
	char sym[PATH_MAX] = { '\0' };
	struct stat sb;
	struct file *f, *prev;

	if (ul_path_readlink(pc, sym, sizeof(sym), name) < 0)
		return NULL;

	/* The /proc/#/{fd,ns} often contains the same file (e.g. /dev/tty)
	 * more than once. Let's try to reuse the previous file if the real
	 * path is the same to save stat() call.
	 */
	prev = list_last_entry(&proc->files, struct file, files);
	if (prev && prev->name && strcmp(prev->name, sym) == 0) {
		f = copy_file(prev);
		f->association = assoc;
	} else {
		if (ul_path_stat(pc, &sb, 0, name) < 0)
			return NULL;

		f = new_file(proc, stat2class(&sb));
		file_set_path(f, &sb, sym, assoc);
	}

	file_init_content(f);

	if (is_association(f, EXE))
		proc->uid = sb.st_uid;
	if (is_association(f, NS_MNT))
		proc->ns_mnt = sb.st_ino;

	else if (assoc >= 0) {
		/* file-descriptor based association */
		FILE *fdinfo;

		if (ul_path_stat(pc, &sb, AT_SYMLINK_NOFOLLOW, name) == 0)
			f->mode = sb.st_mode;

		fdinfo = ul_path_fopenf(pc, "r", "fdinfo/%d", assoc);
		if (fdinfo) {
			read_fdinfo(f, fdinfo);
			fclose(fdinfo);
		}
	}

	return f;
}

/* read symlinks from /proc/#/fd
 */
static void collect_fd_files(struct path_cxt *pc, struct proc *proc)
{
	DIR *sub = NULL;
	struct dirent *d = NULL;
	char path[sizeof("fd/") + sizeof(stringify_value(UINT64_MAX))];

	while (ul_path_next_dirent(pc, &sub, "fd", &d) == 0) {
		uint64_t num;

		if (ul_strtou64(d->d_name, &num, 10) != 0)	/* only numbers */
			continue;

		snprintf(path, sizeof(path), "fd/%ju", (uintmax_t) num);
		collect_file_symlink(pc, proc, path, num);
	}
}

static void parse_maps_line(char *buf, struct proc *proc)
{
	uint64_t start, end, offset, ino;
	unsigned long major, minor;
	enum association assoc = ASSOC_MEM;
	struct stat sb;
	struct file *f, *prev;
	char *path, modestr[5];
	dev_t devno;

	/* ignore non-path entries */
	path = strchr(buf, '/');
	if (!path)
		return;
	rtrim_whitespace((unsigned char *) path);

	/* read rest of the map */
	if (sscanf(buf, "%"SCNx64		/* start */
			"-%"SCNx64		/* end */
			" %4[^ ]"		/* mode */
			" %"SCNx64		/* offset */
		        " %lx:%lx"		/* maj:min */
			" %"SCNu64,		/* inode */

			&start, &end, modestr, &offset,
			&major, &minor, &ino) != 7)
		return;

	devno = makedev(major, minor);

	if (modestr[3] == 's')
		assoc = ASSOC_SHM;

	/* The map usually contains the same file more than once, try to reuse
	 * the previous file (if devno and ino are the same) to save stat() call.
	 */
	prev = list_last_entry(&proc->files, struct file, files);

	if (prev && prev->stat.st_dev == devno && prev->stat.st_ino == ino) {
		f = copy_file(prev);
		f->association = -assoc;
	} else {
		if (stat(path, &sb) < 0)
			return;
		f = new_file(proc, stat2class(&sb));
		if (!f)
			return;

		file_set_path(f, &sb, path, -assoc);
	}

	if (modestr[0] == 'r')
		f->mode |= S_IRUSR;
	if (modestr[1] == 'w')
		f->mode |= S_IWUSR;
	if (modestr[2] == 'x')
		f->mode |= S_IXUSR;

	f->map_start = start;
	f->map_end = end;
	f->pos = offset;

	file_init_content(f);
}

static void collect_mem_files(struct path_cxt *pc, struct proc *proc)
{
	FILE *fp;
	char buf[BUFSIZ];

	fp = ul_path_fopen(pc, "r", "maps");
	if (!fp)
		return;

	while (fgets(buf, sizeof(buf), fp))
		parse_maps_line(buf, proc);

	fclose(fp);
}

static void collect_outofbox_files(struct path_cxt *pc,
				   struct proc *proc,
				   enum association assocs[],
				   const char *names[],
				   size_t count)
{
	size_t i;

	for (i = 0; i < count; i++)
		collect_file_symlink(pc, proc, names[assocs[i]], assocs[i] * -1);
}

static void collect_execve_file(struct path_cxt *pc, struct proc *proc)
{
	enum association assocs[] = { ASSOC_EXE };
	const char *names[] = {
		[ASSOC_EXE]  = "exe",
	};
	collect_outofbox_files(pc, proc, assocs, names, ARRAY_SIZE(assocs));
}

static void collect_fs_files(struct path_cxt *pc, struct proc *proc)
{
	enum association assocs[] = { ASSOC_EXE, ASSOC_CWD, ASSOC_ROOT };
	const char *names[] = {
		[ASSOC_CWD]  = "cwd",
		[ASSOC_ROOT] = "root",
	};
	collect_outofbox_files(pc, proc, assocs, names, ARRAY_SIZE(assocs));
}

static void collect_namespace_files(struct path_cxt *pc, struct proc *proc)
{
	enum association assocs[] = {
		ASSOC_NS_CGROUP,
		ASSOC_NS_IPC,
		ASSOC_NS_MNT,
		ASSOC_NS_NET,
		ASSOC_NS_PID,
		ASSOC_NS_PID4C,
		ASSOC_NS_TIME,
		ASSOC_NS_TIME4C,
		ASSOC_NS_USER,
		ASSOC_NS_UTS,
	};
	const char *names[] = {
		[ASSOC_NS_CGROUP] = "ns/cgroup",
		[ASSOC_NS_IPC]    = "ns/ipc",
		[ASSOC_NS_MNT]    = "ns/mnt",
		[ASSOC_NS_NET]    = "ns/net",
		[ASSOC_NS_PID]    = "ns/pid",
		[ASSOC_NS_PID4C]  = "ns/pid_for_children",
		[ASSOC_NS_TIME]   = "ns/time",
		[ASSOC_NS_TIME4C] = "ns/time_for_children",
		[ASSOC_NS_USER]   = "ns/user",
		[ASSOC_NS_UTS]    = "ns/uts",
	};
	collect_outofbox_files(pc, proc, assocs, names, ARRAY_SIZE(assocs));
}

static struct nodev *new_nodev(unsigned long minor, const char *filesystem)
{
	struct nodev *nodev = xcalloc(1, sizeof(*nodev));

	INIT_LIST_HEAD(&nodev->nodevs);
	nodev->minor = minor;
	nodev->filesystem = xstrdup(filesystem);

	return nodev;
}

static void free_nodev(struct nodev *nodev)
{
	free(nodev->filesystem);
	free(nodev);
}

static void initialize_nodevs(void)
{
	for (int i = 0; i < NODEV_TABLE_SIZE; i++)
		INIT_LIST_HEAD(&nodev_table.tables[i]);
}

static void finalize_nodevs(void)
{
	for (int i = 0; i < NODEV_TABLE_SIZE; i++)
		list_free(&nodev_table.tables[i], struct nodev, nodevs, free_nodev);

	free(mnt_namespaces);
}

const char *get_nodev_filesystem(unsigned long minor)
{
	struct list_head *n;
	int slot = minor % NODEV_TABLE_SIZE;

	list_for_each (n, &nodev_table.tables[slot]) {
		struct nodev *nodev = list_entry(n, struct nodev, nodevs);
		if (nodev->minor == minor)
			return nodev->filesystem;
	}
	return NULL;
}

static void add_nodevs(FILE *mnt)
{
	/* This can be very long. A line in mountinfo can have more than 3
	 * paths. */
	char line[PATH_MAX * 3 + 256];

	while (fgets(line, sizeof(line), mnt)) {
		unsigned long major, minor;
		char filesystem[256];
		struct nodev *nodev;
		int slot;


		/* 23 61 0:22 / /sys rw,nosuid,nodev,noexec,relatime shared:2 - sysfs sysfs rw,seclabel */
		if(sscanf(line, "%*d %*d %lu:%lu %*s %*s %*s %*[^-] - %s %*[^\n]",
			  &major, &minor, filesystem) != 3)
			/* 1600 1458 0:55 / / rw,nodev,relatime - overlay overlay rw,context="s... */
			if (sscanf(line, "%*d %*d %lu:%lu %*s %*s %*s - %s %*[^\n]",
				   &major, &minor, filesystem) != 3)
				continue;

		if (major != 0)
			continue;
		if (get_nodev_filesystem(minor))
			continue;

		nodev = new_nodev(minor, filesystem);
		slot = minor % NODEV_TABLE_SIZE;

		list_add_tail(&nodev->nodevs, &nodev_table.tables[slot]);
	}
}

static void fill_column(struct proc *proc,
			struct file *file,
			struct libscols_line *ln,
			int column_id,
			size_t column_index)
{
	const struct file_class *class = file->class;

	while (class) {
		if (class->fill_column
		    && class->fill_column(proc, file, ln,
					  column_id, column_index))
			break;
		class = class->super;
	}
}

static void convert_file(struct proc *proc,
		     struct file *file,
		     struct libscols_line *ln)

{
	for (size_t i = 0; i < ncolumns; i++)
		fill_column(proc, file, ln, get_column_id(i), i);
}

static void convert(struct list_head *procs, struct lsfd_control *ctl)
{
	struct list_head *p;

	list_for_each (p, procs) {
		struct proc *proc = list_entry(p, struct proc, procs);
		struct list_head *f;

		list_for_each (f, &proc->files) {
			struct file *file = list_entry(f, struct file, files);
			struct libscols_line *ln = scols_table_new_line(ctl->tb, NULL);
			if (!ln)
				err(EXIT_FAILURE, _("failed to allocate output line"));

			convert_file(proc, file, ln);

			if (!lsfd_filter_apply(ctl->filter, ln))
				scols_table_remove_line(ctl->tb, ln);
		}
	}
}

static void delete(struct list_head *procs, struct lsfd_control *ctl)
{
	list_free(procs, struct proc, procs, free_proc);

	scols_unref_table(ctl->tb);
	lsfd_filter_free(ctl->filter);
}

static void emit(struct lsfd_control *ctl)
{
	scols_print_table(ctl->tb);
}


static void initialize_class(const struct file_class *class)
{
	if (class->initialize_class)
		class->initialize_class();
}

static void initialize_classes(void)
{
	initialize_class(&file_class);
	initialize_class(&cdev_class);
	initialize_class(&bdev_class);
	initialize_class(&sock_class);
	initialize_class(&unkn_class);
}

static void finalize_class(const struct file_class *class)
{
	if (class->finalize_class)
		class->finalize_class();
}

static void finalize_classes(void)
{
	finalize_class(&file_class);
	finalize_class(&cdev_class);
	finalize_class(&bdev_class);
	finalize_class(&sock_class);
	finalize_class(&unkn_class);
}



struct name_manager *new_name_manager(void)
{
	struct name_manager *nm = xcalloc(1, sizeof(struct name_manager));

	nm->cache = new_idcache();
	if (!nm->cache)
		err(EXIT_FAILURE, _("failed to allocate an idcache"));

	nm->next_id = 1;	/* 0 is never issued as id. */
	return nm;
}

void free_name_manager(struct name_manager *nm)
{
	free_idcache(nm->cache);
	free(nm);
}

const char *get_name(struct name_manager *nm, unsigned long id)
{
	struct identry *e;

	e = get_id(nm->cache, id);

	return e? e->name: NULL;
}

unsigned long add_name(struct name_manager *nm, const char *name)
{
	struct identry *e = NULL, *tmp;

	for (tmp = nm->cache->ent; tmp; tmp = tmp->next) {
		if (strcmp(tmp->name, name) == 0) {
			e = tmp;
			break;
		}
	}

	if (e)
		return e->id;

	e = xmalloc(sizeof(struct identry));
	e->name = xstrdup(name);
	e->id = nm->next_id++;
	e->next = nm->cache->ent;
	nm->cache->ent = e;

	return e->id;
}

static void read_process(struct lsfd_control *ctl, struct path_cxt *pc,
			 pid_t pid, struct proc *leader)
{
	char buf[BUFSIZ];
	struct proc *proc;

	if (procfs_process_init_path(pc, pid) != 0)
		return;

	proc = new_process(pid, leader);
	proc->command = procfs_process_get_cmdname(pc, buf, sizeof(buf)) > 0 ?
			xstrdup(buf) : xstrdup(_("(unknown)"));

	collect_execve_file(pc, proc);

	if (proc->pid == proc->leader->pid
	    || kcmp(proc->leader->pid, proc->pid, KCMP_FS, 0, 0) != 0)
		collect_fs_files(pc, proc);

	collect_namespace_files(pc, proc);

	if (proc->ns_mnt == 0 || !has_mnt_ns(proc->ns_mnt)) {
		FILE *mnt = ul_path_fopen(pc, "r", "mountinfo");
		if (mnt) {
			add_nodevs(mnt);
			if (proc->ns_mnt)
				add_mnt_ns(proc->ns_mnt);
			fclose(mnt);
		}
	}

	/* If kcmp is not available,
	 * there is no way to no whether threads share resources.
	 * In such cases, we must pay the costs: call collect_mem_files()
	 * and collect_fd_files().
	 */
	if (proc->pid == proc->leader->pid
	    || kcmp(proc->leader->pid, proc->pid, KCMP_VM, 0, 0) != 0)
		collect_mem_files(pc, proc);

	if (proc->pid == proc->leader->pid
	    || kcmp(proc->leader->pid, proc->pid, KCMP_FILES, 0, 0) != 0)
		collect_fd_files(pc, proc);

	list_add_tail(&proc->procs, &ctl->procs);

	/* The tasks collecting overwrites @pc by /proc/<task-pid>/. Keep it as
	 * the last path based operation in read_process()
	 */
	if (ctl->threads && leader == NULL) {
		DIR *sub = NULL;;
		pid_t tid;

		while (procfs_process_next_tid(pc, &sub, &tid) == 0) {
			if (tid == pid)
				continue;
			read_process(ctl, pc, tid, proc);
		}
	}

	/* Let's be careful with number of open files */
        ul_path_close_dirfd(pc);
}

static void collect_processes(struct lsfd_control *ctl)
{
	DIR *dir;
	struct dirent *d;
	struct path_cxt *pc = NULL;

	pc = ul_new_path(NULL);
	if (!pc)
		err(EXIT_FAILURE, _("failed to alloc procfs handler"));

	dir = opendir(_PATH_PROC);
	if (!dir)
		err(EXIT_FAILURE, _("failed to open /proc"));

	while ((d = readdir(dir))) {
		pid_t pid;

		if (procfs_dirent_get_pid(d, &pid) != 0)
			continue;
		read_process(ctl, pc, pid, 0);
	}

	closedir(dir);
	ul_unref_path(pc);
}

static void __attribute__((__noreturn__)) usage(void)
{
	FILE *out = stdout;
	size_t i;

	fputs(USAGE_HEADER, out);
	fprintf(out, _(" %s [options]\n"), program_invocation_short_name);

	fputs(USAGE_OPTIONS, out);
	fputs(_(" -l, --threads         list in threads level\n"), out);
	fputs(_(" -J, --json            use JSON output format\n"), out);
	fputs(_(" -n, --noheadings      don't print headings\n"), out);
	fputs(_(" -o, --output <list>   output columns\n"), out);
	fputs(_(" -r, --raw             use raw output format\n"), out);
	fputs(_("     --sysroot <dir>   use specified directory as system root\n"), out);
	fputs(_(" -u, --notruncate      don't truncate text in columns\n"), out);
	fputs(_(" -Q, --filter <expr>   apply display filter\n"), out);
	fputs(_("     --source <source> add filter by SOURCE\n"), out);

	fputs(USAGE_SEPARATOR, out);
	printf(USAGE_HELP_OPTIONS(23));

	fprintf(out, USAGE_COLUMNS);

	for (i = 0; i < ARRAY_SIZE(infos); i++)
		fprintf(out, " %11s  %-10s%s\n", infos[i].name,
			infos[i].json_type == SCOLS_JSON_STRING?  "<string>":
			infos[i].json_type == SCOLS_JSON_NUMBER?  "<number>":
			"<boolean>",
			_(infos[i].help));

	printf(USAGE_MAN_TAIL("lsfd(1)"));

	exit(EXIT_SUCCESS);
}

static void xstrappend(char **a, const char *b)
{
	if (strappend(a, b) < 0)
		err(EXIT_FAILURE, _("failed to allocate memory for string"));
}

static char * quote_filter_expr(char *expr)
{
	char c[] = {'\0', '\0'};
	char *r = strdup("");
	while (*expr) {
		switch (*expr) {
		case '\'':
			xstrappend(&r, "\\'");
			break;
		case '"':
			xstrappend(&r, "\\\"");
			break;
		default:
			c[0] = *expr;
			xstrappend(&r, c);
			break;
		}
		expr++;
	}
	return r;
}

static void append_filter_expr(char **a, const char *b, bool and)
{
	if (*a == NULL) {
		*a = xstrdup(b);
		return;
	}

	char *tmp = *a;
	*a = NULL;

	xstrappend(a, "(");
	xstrappend(a, tmp);
	xstrappend(a, ")");
	if (and)
		xstrappend(a, "and(");
	else
		xstrappend(a, "or(");
	xstrappend(a, b);
	xstrappend(a, ")");
}

int main(int argc, char *argv[])
{
	int c;
	size_t i;
	char *outarg = NULL;
	struct lsfd_control ctl = {};
	char  *filter_expr = NULL;

	enum {
		OPT_SYSROOT = CHAR_MAX + 1,
		OPT_SOURCE,
	};
	static const struct option longopts[] = {
		{ "noheadings", no_argument, NULL, 'n' },
		{ "output",     required_argument, NULL, 'o' },
		{ "version",    no_argument, NULL, 'V' },
		{ "help",	no_argument, NULL, 'h' },
		{ "json",       no_argument, NULL, 'J' },
		{ "raw",        no_argument, NULL, 'r' },
		{ "threads",    no_argument, NULL, 'l' },
		{ "notruncate", no_argument, NULL, 'u' },
		{ "sysroot",    required_argument, NULL, OPT_SYSROOT },
		{ "filter",     required_argument, NULL, 'Q' },
		{ "source",     required_argument, NULL, OPT_SOURCE },
		{ NULL, 0, NULL, 0 },
	};

	setlocale(LC_ALL, "");
	bindtextdomain(PACKAGE, LOCALEDIR);
	textdomain(PACKAGE);
	close_stdout_atexit();

	while ((c = getopt_long(argc, argv, "no:JrVhluQ:", longopts, NULL)) != -1) {
		switch (c) {
		case 'n':
			ctl.noheadings = 1;
			break;
		case 'o':
			outarg = optarg;
			break;
		case 'J':
			ctl.json = 1;
			break;
		case 'r':
			ctl.raw = 1;
			break;
		case 'l':
			ctl.threads = 1;
			break;
		case 'u':
			ctl.notrunc = 1;
			break;
		case OPT_SYSROOT:
			ctl.sysroot = optarg;
			break;
		case 'Q':
			append_filter_expr(&filter_expr, optarg, true);
			break;
		case OPT_SOURCE: {
			char * quoted_source = quote_filter_expr(optarg);
			char * source_expr = NULL;
			xstrappend(&source_expr, "(SOURCE == '");
			xstrappend(&source_expr, quoted_source);
			xstrappend(&source_expr, "')");
			append_filter_expr(&filter_expr, source_expr, true);
			free(source_expr);
			free(quoted_source);
			break;
		}

		case 'V':
			print_version(EXIT_SUCCESS);
		case 'h':
			usage();
		default:
			errtryhelp(EXIT_FAILURE);
		}
	}

#define INITIALIZE_COLUMNS(COLUMN_SPEC)				\
	for (i = 0; i < ARRAY_SIZE(COLUMN_SPEC); i++)	\
		columns[ncolumns++] = COLUMN_SPEC[i]
	if (!ncolumns) {
		if (ctl.threads)
			INITIALIZE_COLUMNS(default_threads_columns);
		else
			INITIALIZE_COLUMNS(default_columns);
	}

	if (outarg && string_add_to_idarray(outarg, columns, ARRAY_SIZE(columns),
					    &ncolumns, column_name_to_id) < 0)
		return EXIT_FAILURE;

	scols_init_debug(0);

	INIT_LIST_HEAD(&ctl.procs);

	/* inilialize scols table */
	ctl.tb = scols_new_table();
	if (!ctl.tb)
		err(EXIT_FAILURE, _("failed to allocate output table"));

	scols_table_enable_noheadings(ctl.tb, ctl.noheadings);
	scols_table_enable_raw(ctl.tb, ctl.raw);
	scols_table_enable_json(ctl.tb, ctl.json);
	if (ctl.json)
		scols_table_set_name(ctl.tb, "lsfd");

	/* create output columns */
	for (i = 0; i < ncolumns; i++) {
		const struct colinfo *col = get_column_info(i);
		struct libscols_column *cl = add_column(ctl.tb, col);

		if (!cl)
			err(EXIT_FAILURE, _("failed to allocate output column"));

		if (ctl.notrunc) {
			int flags = scols_column_get_flags(cl);
			flags &= ~SCOLS_FL_TRUNC;
			scols_column_set_flags(cl, flags);
		}
	}

	/* make fitler */
	if (filter_expr) {
		ctl.filter = lsfd_filter_new(filter_expr, ctl.tb,
					     LSFD_N_COLS,
					     column_name_to_id_cb,
					     add_column_by_id_cb, &ctl);
		const char *errmsg = lsfd_filter_get_errmsg(ctl.filter);
		if (errmsg)
			errx(EXIT_FAILURE, "%s", errmsg);
		free(filter_expr);
	}

	/* collect data */
	initialize_nodevs();
	initialize_classes();

	collect_processes(&ctl);

	convert(&ctl.procs, &ctl);
	emit(&ctl);

	/* cleanup */
	delete(&ctl.procs, &ctl);

	finalize_classes();
	finalize_nodevs();

	return 0;
}
