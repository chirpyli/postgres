/*-------------------------------------------------------------------------
 *
 * copydir.c
 *	  复制一个目录
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *	While "xcopy /e /i /q" works fine for copying directories, on Windows XP
 *	it requires a Window handle which prevents it from working when invoked
 *	as a service.
 *
 * IDENTIFICATION
 *	  src/backend/storage/file/copydir.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#ifdef HAVE_COPYFILE_H
#include <copyfile.h>
#endif
#include <fcntl.h>
#include <unistd.h>

#include "common/file_utils.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/copydir.h"
#include "storage/fd.h"

/* GUC 参数 */
int			file_copy_method = FILE_COPY_METHOD_COPY;

static void clone_file(const char *fromfile, const char *tofile);

/*
 * copydir: 复制一个目录
 *
 * 如果 recurse 为 false，则忽略子目录。任何非目录或非普通文件的内容
 * 都会被忽略。
 *
 * 此函数使用 file_copy_method GUC 参数。调用此函数的新场景必须在
 * doc/src/sgml/config.sgml 中说明。
 */
void
copydir(const char *fromdir, const char *todir, bool recurse)
{
	DIR		   *xldir;
	struct dirent *xlde;
	char		fromfile[MAXPGPATH * 2];
	char		tofile[MAXPGPATH * 2];

	if (MakePGDirectory(todir) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create directory \"%s\": %m", todir)));

	xldir = AllocateDir(fromdir);

	while ((xlde = ReadDir(xldir, fromdir)) != NULL)
	{
		PGFileType	xlde_type;

		/* 如果在目录复制期间收到取消信号，则退出 */
		CHECK_FOR_INTERRUPTS();

		if (strcmp(xlde->d_name, ".") == 0 ||
			strcmp(xlde->d_name, "..") == 0)
			continue;

		snprintf(fromfile, sizeof(fromfile), "%s/%s", fromdir, xlde->d_name);
		snprintf(tofile, sizeof(tofile), "%s/%s", todir, xlde->d_name);

		xlde_type = get_dirent_type(fromfile, xlde, false, ERROR);

		if (xlde_type == PGFILETYPE_DIR)
		{
			/* 递归处理子目录 */
			if (recurse)
				copydir(fromfile, tofile, true);
		}
		else if (xlde_type == PGFILETYPE_REG)
		{
			if (file_copy_method == FILE_COPY_METHOD_CLONE)
				clone_file(fromfile, tofile);
			else
				copy_file(fromfile, tofile);
		}
	}
	FreeDir(xldir);

	/*
	 * 此处采取保守策略，对所有文件执行 fsync 以确保复制真正完成。
	 * 但如果 fsync 已被禁用，我们到此为止。
	 */
	if (!enableFsync)
		return;

	xldir = AllocateDir(todir);

	while ((xlde = ReadDir(xldir, todir)) != NULL)
	{
		if (strcmp(xlde->d_name, ".") == 0 ||
			strcmp(xlde->d_name, "..") == 0)
			continue;

		snprintf(tofile, sizeof(tofile), "%s/%s", todir, xlde->d_name);

		/*
		 * 此处不需要同步子目录，因为递归 copydir 会在返回前完成同步
		 */
		if (get_dirent_type(tofile, xlde, false, ERROR) == PGFILETYPE_REG)
			fsync_fname(tofile, false);
	}
	FreeDir(xldir);

	/*
	 * 对目标目录本身执行 fsync 非常重要，因为单个文件的 fsync 并不能保证
	 * 该文件的目录项已同步。ext4 的最新版本大大加宽了这个窗口（更易丢失），
	 * 但这在 ext3 和其他文件系统上一直如此。
	 */
	fsync_fname(todir, true);
}

/*
 * 复制一个文件
 */
void
copy_file(const char *fromfile, const char *tofile)
{
	char	   *buffer;
	int			srcfd;
	int			dstfd;
	int			nbytes;
	off_t		offset;
	off_t		flush_offset;

	/* 复制缓冲区大小（读写请求大小） */
#define COPY_BUF_SIZE (8 * BLCKSZ)

	/*
	 * 数据刷新请求的步长。在大多数平台上，大约每 1MB 刷新一次似乎
	 * 更有利。但 macOS（至少是 APFS 早期版本）对小块的 mmap/msync
	 * 请求非常不友好，因此在 macOS 上每 32MB 才刷新一次。
	 */
#if defined(__darwin__)
#define FLUSH_DISTANCE (32 * 1024 * 1024)
#else
#define FLUSH_DISTANCE (1024 * 1024)
#endif

	/* 使用 palloc 确保获取对齐缓冲区 */
	buffer = palloc(COPY_BUF_SIZE);

	/*
	 * 打开文件
	 */
	srcfd = OpenTransientFile(fromfile, O_RDONLY | PG_BINARY);
	if (srcfd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", fromfile)));

	dstfd = OpenTransientFile(tofile, O_RDWR | O_CREAT | O_EXCL | PG_BINARY);
	if (dstfd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", tofile)));

	/*
	 * 执行数据复制。
	 */
	flush_offset = 0;
	for (offset = 0;; offset += nbytes)
	{
		/* 如果在文件复制期间收到取消信号，则退出 */
		CHECK_FOR_INTERRUPTS();

		/*
		 * 我们会稍后对文件执行 fsync，但在复制期间，每隔一段时间刷新
		 * 一次，以避免填满缓存，并希望内核在 fsync 到来之前就开始
		 * 写出数据。
		 */
		if (offset - flush_offset >= FLUSH_DISTANCE)
		{
			pg_flush_data(dstfd, flush_offset, offset - flush_offset);
			flush_offset = offset;
		}

		pgstat_report_wait_start(WAIT_EVENT_COPY_FILE_READ);
		nbytes = read(srcfd, buffer, COPY_BUF_SIZE);
		pgstat_report_wait_end();
		if (nbytes < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read file \"%s\": %m", fromfile)));
		if (nbytes == 0)
			break;
		errno = 0;
		pgstat_report_wait_start(WAIT_EVENT_COPY_FILE_WRITE);
		if ((int) write(dstfd, buffer, nbytes) != nbytes)
		{
			/* 如果 write 没有设置 errno，假设问题是磁盘空间不足 */
			if (errno == 0)
				errno = ENOSPC;
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write to file \"%s\": %m", tofile)));
		}
		pgstat_report_wait_end();
	}

	if (offset > flush_offset)
		pg_flush_data(dstfd, flush_offset, offset - flush_offset);

	if (CloseTransientFile(dstfd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", tofile)));

	if (CloseTransientFile(srcfd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", fromfile)));

	pfree(buffer);
}

/*
 * 克隆一个文件
 */
static void
clone_file(const char *fromfile, const char *tofile)
{
#if defined(HAVE_COPYFILE) && defined(COPYFILE_CLONE_FORCE)
	if (copyfile(fromfile, tofile, NULL, COPYFILE_CLONE_FORCE) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not clone file \"%s\" to \"%s\": %m",
						fromfile, tofile)));
#elif defined(HAVE_COPY_FILE_RANGE)
	int			srcfd;
	int			dstfd;
	ssize_t		nbytes;

	srcfd = OpenTransientFile(fromfile, O_RDONLY | PG_BINARY);
	if (srcfd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", fromfile)));

	dstfd = OpenTransientFile(tofile, O_WRONLY | O_CREAT | O_EXCL | PG_BINARY);
	if (dstfd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", tofile)));

	do
	{
		/*
		 * 不要一次复制太多，这样可以在退化为慢速复制时
		 * 定期检查中断信号。
		 */
		CHECK_FOR_INTERRUPTS();
		pgstat_report_wait_start(WAIT_EVENT_COPY_FILE_COPY);
		nbytes = copy_file_range(srcfd, NULL, dstfd, NULL, 1024 * 1024, 0);
		if (nbytes < 0 && errno != EINTR)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not clone file \"%s\" to \"%s\": %m",
							fromfile, tofile)));
		pgstat_report_wait_end();
	}
	while (nbytes != 0);

	if (CloseTransientFile(dstfd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", tofile)));

	if (CloseTransientFile(srcfd) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", fromfile)));
#else
	/* 如果没有 CLONE 支持，此函数不应被调用。 */
	pg_unreachable();
#endif
}
