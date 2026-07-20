/*-------------------------------------------------------------------------
 *
 * fd.h
 *	  虚拟文件描述符定义。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/fd.h
 *
 *-------------------------------------------------------------------------
 */

/*
 * 调用：
 *
 *	File {Close, Read, ReadV, Write, WriteV, Size, Sync}
 *	{Path Name Open, Allocate, Free} File
 *
 * 这些并非仅仅是 UNIX 系统调用的重命名。
 * 所有文件操作都应使用它们...
 *
 *	File fd;
 *	fd = PathNameOpenFile("foo", O_RDONLY);
 *
 *	AllocateFile();
 *	FreeFile();
 *
 * 如果需要一个 stdio 文件 (FILE*)，请使用 AllocateFile 而非 fopen；
 * 然后用 FreeFile 而非 fclose 来关闭它。
 * 避免对需要长期持有的文件使用 stdio，
 * 因为 stdio 无法与其他文件共享内核文件描述符。
 *
 * 同理，使用 AllocateDir/FreeDir 而非 opendir/closedir 来分配
 * 打开的目录 (DIR*)，使用 OpenTransientFile/CloseTransientFile
 * 来获取无缓冲的文件描述符。
 *
 * 如果确实无法使用上述任何接口，至少应调用 AcquireExternalFD
 * 或 ReserveExternalFD 来报告那些长期持有的文件描述符。
 * 否则可能导致不必要的 EMFILE 错误。
 */
#ifndef FD_H
#define FD_H

#include "port/pg_iovec.h"

#include <dirent.h>
#include <fcntl.h>

typedef int File;


#define IO_DIRECT_DATA			0x01
#define IO_DIRECT_WAL			0x02
#define IO_DIRECT_WAL_INIT		0x04

enum FileExtendMethod
{
#ifdef HAVE_POSIX_FALLOCATE
	FILE_EXTEND_METHOD_POSIX_FALLOCATE,
#endif
	FILE_EXTEND_METHOD_WRITE_ZEROS,
};

/* 默认使用第一个可用的 file_extend_method。 */
#define DEFAULT_FILE_EXTEND_METHOD 0

/* GUC 参数 */
extern PGDLLIMPORT int max_files_per_process;
extern PGDLLIMPORT bool data_sync_retry;
extern PGDLLIMPORT int recovery_init_sync_method;
extern PGDLLIMPORT int io_direct_flags;
extern PGDLLIMPORT int file_extend_method;

/*
 * 这是 fd.c 的私有数据，但为了 save/restore_backend_variables() 而导出
 */
extern PGDLLIMPORT int max_safe_fds;

/*
 * 在 Windows 上，我们需要将 EACCES 解释为可能与 ENOENT 含义相同，
 * 因为在该平台上，如果一个文件已经被 unlink 但尚未消失，你就会得到 EACCES。
 * 哎。此代码的设计使得我们不会在没有进一步证据（即
 * 一个待处理的 fsync 请求被取消... 参见 ProcessSyncRequests）的情况下
 * 轻信这些情况是正常的。
 */
#ifndef WIN32
#define FILE_POSSIBLY_DELETED(err)	((err) == ENOENT)
#else
#define FILE_POSSIBLY_DELETED(err)	((err) == ENOENT || (err) == EACCES)
#endif

/*
 * O_DIRECT 不是标准标志，但几乎所有 Unix 系统都支持它。
 * 在 src/port/open.c 中将其转换为适当的 Windows 标志。
 * 在 macOS 上，我们在 fd.c 的 open() 包装函数中通过 fcntl(F_NOCACHE) 来模拟它。
 * 在这种情况下，我们使用 PG_O_DIRECT 名称，而不是定义 O_DIRECT
 * （在 Unix 上这可能不是个好主意）。
 * 不过，只有当编译器能正确对齐 PGIOAlignedBlock 时才能使用它。
 */
#if defined(O_DIRECT) && defined(pg_attribute_aligned)
#define		PG_O_DIRECT O_DIRECT
#elif defined(F_NOCACHE)
#define		PG_O_DIRECT 0x80000000
#define		PG_O_DIRECT_USE_F_NOCACHE
#else
#define		PG_O_DIRECT 0
#endif

/*
 * fd.c 中函数的原型
 */

struct PgAioHandle;

/* 虚拟文件操作 --- 等价于 Unix 内核文件操作 */
extern File PathNameOpenFile(const char *fileName, int fileFlags);
extern File PathNameOpenFilePerm(const char *fileName, int fileFlags, mode_t fileMode);
extern File OpenTemporaryFile(bool interXact);
extern void FileClose(File file);
extern int	FilePrefetch(File file, off_t offset, off_t amount, uint32 wait_event_info);
extern ssize_t FileReadV(File file, const struct iovec *iov, int iovcnt, off_t offset, uint32 wait_event_info);
extern ssize_t FileWriteV(File file, const struct iovec *iov, int iovcnt, off_t offset, uint32 wait_event_info);
extern int	FileStartReadV(struct PgAioHandle *ioh, File file, int iovcnt, off_t offset, uint32 wait_event_info);
extern int	FileSync(File file, uint32 wait_event_info);
extern int	FileZero(File file, off_t offset, off_t amount, uint32 wait_event_info);
extern int	FileFallocate(File file, off_t offset, off_t amount, uint32 wait_event_info);

extern off_t FileSize(File file);
extern int	FileTruncate(File file, off_t offset, uint32 wait_event_info);
extern void FileWriteback(File file, off_t offset, off_t nbytes, uint32 wait_event_info);
extern char *FilePathName(File file);
extern int	FileGetRawDesc(File file);
extern int	FileGetRawFlags(File file);
extern mode_t FileGetRawMode(File file);

/* 用于共享命名临时文件的操作 */
extern File PathNameCreateTemporaryFile(const char *path, bool error_on_failure);
extern File PathNameOpenTemporaryFile(const char *path, int mode);
extern bool PathNameDeleteTemporaryFile(const char *path, bool error_on_failure);
extern void PathNameCreateTemporaryDir(const char *basedir, const char *directory);
extern void PathNameDeleteTemporaryDir(const char *dirname);
extern void TempTablespacePath(char *path, Oid tablespace);

/* 允许使用常规 stdio 的操作 --- 请谨慎使用 */
extern FILE *AllocateFile(const char *name, const char *mode);
extern int	FreeFile(FILE *file);

/* 允许使用管道流的操作（popen/pclose） */
extern FILE *OpenPipeStream(const char *command, const char *mode);
extern int	ClosePipeStream(FILE *file);

/* 允许使用 <dirent.h> 库例程的操作 */
extern DIR *AllocateDir(const char *dirname);
extern struct dirent *ReadDir(DIR *dir, const char *dirname);
extern struct dirent *ReadDirExtended(DIR *dir, const char *dirname,
									  int elevel);
extern int	FreeDir(DIR *dir);

/* 允许使用普通内核 FD 的操作，带自动清理 */
extern int	OpenTransientFile(const char *fileName, int fileFlags);
extern int	OpenTransientFilePerm(const char *fileName, int fileFlags, mode_t fileMode);
extern int	CloseTransientFile(int fd);

/* 如果你确实、真的必须要一个普通内核 FD，请使用此函数 */
extern int	BasicOpenFile(const char *fileName, int fileFlags);
extern int	BasicOpenFilePerm(const char *fileName, int fileFlags, mode_t fileMode);

/* 在其他情况下使用这些函数，也用于长期持有的 BasicOpenFile FD */
extern bool AcquireExternalFD(void);
extern void ReserveExternalFD(void);
extern void ReleaseExternalFD(void);

/* 使用默认权限创建一个目录 */
extern int	MakePGDirectory(const char *directoryName);

/* 杂项支持例程 */
extern void InitFileAccess(void);
extern void InitTemporaryFileAccess(void);
extern void set_max_safe_fds(void);
extern void closeAllVfds(void);
extern void SetTempTablespaces(Oid *tableSpaces, int numSpaces);
extern bool TempTablespacesAreSet(void);
extern int	GetTempTablespaces(Oid *tableSpaces, int numSpaces);
extern Oid	GetNextTempTableSpace(void);
extern void AtEOXact_Files(bool isCommit);
extern void AtEOSubXact_Files(bool isCommit, SubTransactionId mySubid,
							  SubTransactionId parentSubid);
extern void RemovePgTempFiles(void);
extern void RemovePgTempFilesInDir(const char *tmpdirname, bool missing_ok,
								   bool unlink_all);
extern bool looks_like_temp_rel_name(const char *name);

extern int	pg_fsync(int fd);
extern int	pg_fsync_no_writethrough(int fd);
extern int	pg_fsync_writethrough(int fd);
extern int	pg_fdatasync(int fd);
extern bool pg_file_exists(const char *name);
extern void pg_flush_data(int fd, off_t offset, off_t nbytes);
extern int	pg_truncate(const char *path, off_t length);
extern void fsync_fname(const char *fname, bool isdir);
extern int	fsync_fname_ext(const char *fname, bool isdir, bool ignore_perm, int elevel);
extern int	durable_rename(const char *oldfile, const char *newfile, int elevel);
extern int	durable_unlink(const char *fname, int elevel);
extern void SyncDataDirectory(void);
extern int	data_sync_elevel(int elevel);

static inline ssize_t
FileRead(File file, void *buffer, size_t amount, off_t offset,
		 uint32 wait_event_info)
{
	struct iovec iov = {
		.iov_base = buffer,
		.iov_len = amount
	};

	return FileReadV(file, &iov, 1, offset, wait_event_info);
}

static inline ssize_t
FileWrite(File file, const void *buffer, size_t amount, off_t offset,
		  uint32 wait_event_info)
{
	struct iovec iov = {
		.iov_base = unconstify(void *, buffer),
		.iov_len = amount
	};

	return FileWriteV(file, &iov, 1, offset, wait_event_info);
}

#endif							/* FD_H */
