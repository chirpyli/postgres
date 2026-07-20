/*-------------------------------------------------------------------------
 *
 * fd.c
 *	  虚拟文件描述符代码。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/file/fd.c
 *
 * NOTES:
 *
 * 本代码管理一个"虚拟"文件描述符（VFD）缓存。
 * 出于各种原因，服务器会打开许多文件描述符，包括基表文件、
 * 临时文件（如排序和哈希溢出文件），以及调用 C 库例程（如 system(3)）
 * 时引入的随机调用；很容易超过系统对单个进程可打开文件数量的限制。
 * （在现代操作系统上这个限制大约为 1024，但其他系统可能更低。）
 *
 * VFD 以 LRU 池的形式进行管理，实际操作系统文件描述符根据需要
 * 被打开和关闭。显然，如果通过本接口打开了一个文件，所有后续操作
 * 也必须通过本接口进行（File 类型不是真实的文件描述符）。
 *
 * 为使此方案正常工作，服务器中的大多数（即便不是全部）例程都应使用
 * 本接口，而不是直接调用 C 库例程（如 open(2) 和 fopen(3)）。
 * 否则，我们仍然可能面临真实文件描述符不足的问题。
 *
 * 接口例程
 *
 * PathNameOpenFile 和 OpenTemporaryFile 用于打开虚拟文件。
 * 使用 OpenTemporaryFile 打开的 File 在关闭时会自动删除，无论
 * 是显式关闭还是在事务结束或进程退出时隐式关闭。
 * PathNameOpenFile 用于长期打开的文件，例如关系文件。
 * 关闭这些文件是调用者的责任，fd.c 没有自动关闭机制。
 *
 * PathName(Create|Open|Delete)Temporary(File|Dir) 用于管理
 * 有名称的临时文件，以便在多个后端之间共享。
 * 这些文件会自动关闭，并且计入创建它们的后端的临时文件限制，
 * 但与匿名文件不同，它们不会自动删除。
 * 参见 sharedfileset.c 了解一种共享所有权机制，
 * 该机制在最后一个后端从一组后端中分离时提供自动清理。
 *
 * AllocateFile、AllocateDir、OpenPipeStream 和 OpenTransientFile
 * 分别是 fopen(3)、opendir(3)、popen(3) 和 open(2) 的包装函数。
 * 它们的行为类似于相应的原生函数，但区别在于句柄会注册到
 * 当前子事务中，并在中止时自动关闭。
 * 这些函数主要用于短操作，如读取配置文件；
 * 通过这类函数同时打开的文件数量是有限制的。
 *
 * 最后，BasicOpenFile 是 open() 的一个轻量级包装，
 * 可以在必要时释放虚拟文件描述符占用的文件描述符。
 * BasicOpenFile 返回的文件描述符没有自动清理机制，
 * 调用者必须自己负责通过 close(2) 关闭文件描述符。
 *
 * 如果需要长期持有一个非虚拟文件描述符，
 * 请通过调用 AcquireExternalFD 或 ReserveExternalFD
 * （最终调用 ReleaseExternalFD）向 fd.c 报告，
 * 以便我们在决定可以打开多少 VFD 时将其考虑在内。
 * 这适用于通过 BasicOpenFile 获取的 FD，也适用于未经 fd.c API 获取的 FD。
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <dirent.h>
#include <sys/file.h>
#include <sys/param.h>
#include <sys/resource.h>		/* for getrlimit */
#include <sys/stat.h>
#include <sys/types.h>
#ifndef WIN32
#include <sys/mman.h>
#endif
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>

#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/pg_tablespace.h"
#include "common/file_perm.h"
#include "common/file_utils.h"
#include "common/pg_prng.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/startup.h"
#include "storage/aio.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "utils/guc.h"
#include "utils/guc_hooks.h"
#include "utils/resowner.h"
#include "utils/varlena.h"

/* 定义 PG_FLUSH_DATA_WORKS，如果我们有 pg_flush_data 的实现 */
#if defined(HAVE_SYNC_FILE_RANGE)
#define PG_FLUSH_DATA_WORKS 1
#elif !defined(WIN32) && defined(MS_ASYNC)
#define PG_FLUSH_DATA_WORKS 1
#elif defined(USE_POSIX_FADVISE) && defined(POSIX_FADV_DONTNEED)
#define PG_FLUSH_DATA_WORKS 1
#endif

/*
 * 我们必须为 system()、动态加载器和其他不咨询 fd.c
 * 就尝试打开文件的代码保留一些空闲文件描述符。
 * 这就是预留的空闲数量。（虽然我们尽力防止 EMFILE 错误，
 * 但由于其他进程消耗 FD 而导致 ENFILE，我们永远无法保证。
 * 所以不咨询 fd.c 就试图打开文件是个坏主意。
 * 尽管如此，我们无法控制所有代码。）
 *
 * 由于这只是一个固定设置，我们实际上假设没有任何此类代码
 * 会长期持有 FD；否则这个预留空间可能不够。
 * 特别注意，我们期望加载共享库不会导致打开文件数量的永久增加。
 * （截至2004年2月，这在大多数（如果不是全部）平台上似乎是成立的。）
 */
#define NUM_RESERVED_FDS		10

/*
 * 在预留了保留 FD 后，如果我们可用的 FD 数量少于此值，则报错。
 * （选择这个值是为了与 "ulimit -n 64" 配合使用，但不能更低了。
 * 注意，此值确保 numExternalFDs 至少为 16；
 * 截至本文撰写时，contrib/postgres_fdw 回归测试需要它至少增长到 14。）
 */
#define FD_MINFREE				48

/*
 * 许多平台允许单个进程打开比多个进程做同样事情时实际能支持的
 * 更多的文件。此 GUC 参数允许 DBA 将 max_safe_fds 限制在
 * 低于 postmaster 初始探测建议值的范围内。
 */
int			max_files_per_process = 1000;

/*
 * fd.c 已知操作（VFD、AllocateFile 等，或"外部"FD）可打开的文件描述符最大数量。
 * 此值初始化为保守值，在 bootstrap 或 standalone-backend 情况下保持不变。
 * 在正常的 postmaster 操作中，postmaster 在初始化后期调用
 * set_max_safe_fds() 更新该值，然后通过 fork 子进程继承该值。
 *
 * 注意：设置此变量时已考虑 max_files_per_process 的值，
 * 因此无需单独测试。
 */
int			max_safe_fds = FD_MINFREE;	/* 未更改时的默认值 */

/* fsync() 失败后继续运行是否安全。 */
bool		data_sync_retry = false;

/* SyncDataDirectory() 应如何执行其工作。 */
int			recovery_init_sync_method = DATA_DIR_SYNC_METHOD_FSYNC;

/* 数据文件应如何通过补零进行批量扩展。 */
int			file_extend_method = DEFAULT_FILE_EXTEND_METHOD;

/* 哪些类型的文件应使用 PG_O_DIRECT 打开。 */
int			io_direct_flags;

/* 调试用.... */

#ifdef FDDEBUG
#define DO_DB(A) \
	do { \
		int			_do_db_save_errno = errno; \
		A; \
		errno = _do_db_save_errno; \
	} while (0)
#else
#define DO_DB(A) \
	((void) 0)
#endif

#define VFD_CLOSED (-1)

#define FileIsValid(file) \
	((file) > 0 && (file) < (int) SizeVfdCache && VfdCache[file].fileName != NULL)

#define FileIsNotOpen(file) (VfdCache[file].fd == VFD_CLOSED)

/* 以下是分配给 fdstate 的位标志： */
#define FD_DELETE_AT_CLOSE	(1 << 0)	/* T = 关闭时删除 */
#define FD_CLOSE_AT_EOXACT	(1 << 1)	/* T = 在 eoXact 时关闭 */
#define FD_TEMP_FILE_LIMIT	(1 << 2)	/* T = 遵守 temp_file_limit */

typedef struct vfd
{
	int			fd;				/* 当前 FD，若没有则为 VFD_CLOSED */
	unsigned short fdstate;		/* VFD 状态的位标志 */
	ResourceOwner resowner;		/* 所有者，用于自动清理 */
	File		nextFree;		/* 如果位于空闲链表中，指向下一个空闲 VFD */
	File		lruMoreRecently;	/* 使用时间双向链表 */
	File		lruLessRecently;
	off_t		fileSize;		/* 文件当前大小（若非临时文件则为0） */
	char	   *fileName;		/* 文件名，未使用的 VFD 为 NULL */
	/* 注意：fileName 是 malloc 分配的，关闭 VFD 时必须 free */
	int			fileFlags;		/* 用于（重新）打开文件的 open(2) 标志 */
	mode_t		fileMode;		/* 传递给 open(2) 的模式 */
} Vfd;

/*
 * 虚拟文件描述符数组指针和大小。按需增长。
 * 'File' 值是该数组的索引。
 * 注意 VfdCache[0] 不是可用的 VFD，只是一个链表头。
 */
static Vfd *VfdCache;
static Size SizeVfdCache = 0;

/*
 * 已知被 VFD 条目使用的文件描述符数量。
 */
static int	nfile = 0;

/*
 * 标志位，判断是否值得扫描 VfdCache 查找需要关闭的临时文件
 */
static bool have_xact_temporary_files = false;

/*
 * 跟踪所有临时文件的总大小。
 * 注意：当强制实施 temp_file_limit 时，这不会溢出，
 * 因为限制不能超过 INT_MAX KB。
 * 不强制时理论上可能溢出，但我们不关心。
 */
static uint64 temporary_files_size = 0;

/* 临时文件访问是否已初始化且尚未关闭？ */
#ifdef USE_ASSERT_CHECKING
static bool temporary_files_allowed = false;
#endif

/*
 * 使用 AllocateFile、AllocateDir 和 OpenTransientFile 打开的
 * OS 句柄列表。
 */
typedef enum
{
	AllocateDescFile,
	AllocateDescPipe,
	AllocateDescDir,
	AllocateDescRawFD,
} AllocateDescKind;

typedef struct
{
	AllocateDescKind kind;
	SubTransactionId create_subid;
	union
	{
		FILE	   *file;
		DIR		   *dir;
		int			fd;
	}			desc;
} AllocateDesc;

static int	numAllocatedDescs = 0;
static int	maxAllocatedDescs = 0;
static AllocateDesc *allocatedDescs = NULL;

/*
 * 报告给 Reserve/ReleaseExternalFD 的已打开"外部"FD 的数量。
 */
static int	numExternalFDs = 0;

/*
 * 当前会话中打开的临时文件数；
 * 用于生成临时文件名。
 */
static long tempFileCounter = 0;

/*
 * 临时表空间 OID 数组。（某些条目可能为 InvalidOid，
 * 表示应使用当前数据库的默认表空间。）
 * 当 numTempTableSpaces 为 -1 时，表示当前事务中尚未设置。
 */
static Oid *tempTableSpaces = NULL;
static int	numTempTableSpaces = -1;
static int	nextTempTableSpace = 0;


/*--------------------
 *
 * 私有例程
 *
 * Delete		   - 从 Lru 环中删除一个文件
 * LruDelete	   - 从 Lru 环中移除一个文件并关闭其 FD
 * Insert		   - 将文件放在 Lru 环的前端
 * LruInsert	   - 将文件放在 Lru 环的前端并打开它
 * ReleaseLruFile  - 通过关闭 Lru 环中的最后一个条目释放一个 fd
 * ReleaseLruFiles - 释放 fd，直到我们低于 max_safe_fds 限制
 * AllocateVfd	   - 从 VfdCache 获取一个空闲（或新建）文件记录
 * FreeVfd		   - 释放一个文件记录
 *
 * 最近最少使用（LRU）环是一个双向链表，以元素零开始和结束。
 * 元素零是特殊的 —— 它不代表一个文件，其 "fd" 字段始终 == VFD_CLOSED。
 * 元素零只是一个标记，显示环的开始/结束。
 * 只有当前真正打开（已分配 FD）的 VFD 元素才在 Lru 环中。
 * "虚拟"打开的元素可以通过具有非空 fileName 字段来识别。
 *
 * 示例：
 *
 *	   /--less----\				   /---------\
 *	   v		   \			  v			  \
 *	 #0 --more---> LeastRecentlyUsed --more-\ \
 *	  ^\									| |
 *	   \\less--> MostRecentlyUsedFile	<---/ |
 *		\more---/					 \--less--/
 *
 *--------------------
 */
static void Delete(File file);
static void LruDelete(File file);
static void Insert(File file);
static int	LruInsert(File file);
static bool ReleaseLruFile(void);
static void ReleaseLruFiles(void);
static File AllocateVfd(void);
static void FreeVfd(File file);

static int	FileAccess(File file);
static File OpenTemporaryFileInTablespace(Oid tblspcOid, bool rejectError);
static bool reserveAllocatedDesc(void);
static int	FreeDesc(AllocateDesc *desc);

static void BeforeShmemExit_Files(int code, Datum arg);
static void CleanupTempFiles(bool isCommit, bool isProcExit);
static void RemovePgTempRelationFiles(const char *tsdirname);
static void RemovePgTempRelationFilesInDbspace(const char *dbspacedirname);

static void walkdir(const char *path,
					void (*action) (const char *fname, bool isdir, int elevel),
					bool process_symlinks,
					int elevel);
#ifdef PG_FLUSH_DATA_WORKS
static void pre_sync_fname(const char *fname, bool isdir, int elevel);
#endif
static void datadir_fsync_fname(const char *fname, bool isdir, int elevel);
static void unlink_if_exists_fname(const char *fname, bool isdir, int elevel);

static int	fsync_parent_path(const char *fname, int elevel);


/* 用于持有虚拟文件描述符的 ResourceOwner 回调 */
static void ResOwnerReleaseFile(Datum res);
static char *ResOwnerPrintFile(Datum res);

static const ResourceOwnerDesc file_resowner_desc =
{
	.name = "File",
	.release_phase = RESOURCE_RELEASE_AFTER_LOCKS,
	.release_priority = RELEASE_PRIO_FILES,
	.ReleaseResource = ResOwnerReleaseFile,
	.DebugPrint = ResOwnerPrintFile
};

/* ResourceOwnerRemember/Forget 的便捷包装 */
static inline void
ResourceOwnerRememberFile(ResourceOwner owner, File file)
{
	ResourceOwnerRemember(owner, Int32GetDatum(file), &file_resowner_desc);
}
static inline void
ResourceOwnerForgetFile(ResourceOwner owner, File file)
{
	ResourceOwnerForget(owner, Int32GetDatum(file), &file_resowner_desc);
}

/*
 * pg_fsync --- 执行 fsync，带或不带 writethrough
 */
int
pg_fsync(int fd)
{
#if !defined(WIN32) && defined(USE_ASSERT_CHECKING)
	struct stat st;

	/*
	 * 某些操作系统对 fsync() 的实现在文件描述符参数被打开时的
	 * 文件访问模式方面有要求，并且这些要求会因文件描述符是否为目录
	 * 而有所不同。
	 *
	 * 对于任何最终可能传递给 fsync() 的文件描述符，
	 * 我们应该以在所有受支持系统上兼容 fsync() 的访问模式打开它，
	 * 否则代码可能不具备可移植性，即使在当前系统上运行正常。
	 *
	 * 我们在此断言：文件的描述符是以写权限（即非 O_RDONLY）打开的，
	 * 目录的描述符是以无写权限（O_RDONLY）打开的。
	 * 注意，即使 fsync() 被禁用，这个断言检查也会执行。
	 *
	 * 如果 fstat() 失败，忽略它并让后续的 fsync() 报错。
	 */
	if (fstat(fd, &st) == 0)
	{
		int			desc_flags = fcntl(fd, F_GETFL);

		desc_flags &= O_ACCMODE;

		if (S_ISDIR(st.st_mode))
			Assert(desc_flags == O_RDONLY);
		else
			Assert(desc_flags != O_RDONLY);
	}
	errno = 0;
#endif

	/* 如果没有需要，使用 #if 来跳过 wal_sync_method 测试 */
#if defined(HAVE_FSYNC_WRITETHROUGH)
	if (wal_sync_method == WAL_SYNC_METHOD_FSYNC_WRITETHROUGH)
		return pg_fsync_writethrough(fd);
	else
#endif
		return pg_fsync_no_writethrough(fd);
}


/*
 * pg_fsync_no_writethrough --- 与 fsync 相同，但当 enableFsync 为 off 时
 *	不做任何操作
 */
int
pg_fsync_no_writethrough(int fd)
{
	int			rc;

	if (!enableFsync)
		return 0;

retry:
	rc = fsync(fd);

	if (rc == -1 && errno == EINTR)
		goto retry;

	return rc;
}

/*
 * pg_fsync_writethrough --- 强制同步写入
 */
int
pg_fsync_writethrough(int fd)
{
	if (enableFsync)
	{
#if defined(F_FULLFSYNC)
		return (fcntl(fd, F_FULLFSYNC, 0) == -1) ? -1 : 0;
#else
		errno = ENOSYS;
		return -1;
#endif
	}
	else
		return 0;
}

/*
 * pg_fdatasync --- 与 fdatasync 相同，但当 enableFsync 为 off 时不做任何操作
 */
int
pg_fdatasync(int fd)
{
	int			rc;

	if (!enableFsync)
		return 0;

retry:
	rc = fdatasync(fd);

	if (rc == -1 && errno == EINTR)
		goto retry;

	return rc;
}

/*
 * pg_file_exists -- 检查文件是否存在。
 *
 * 需要文件的绝对路径。如果文件不是目录则返回 true，否则返回 false。
 */
bool
pg_file_exists(const char *name)
{
	struct stat st;

	Assert(name != NULL);

	if (stat(name, &st) == 0)
		return !S_ISDIR(st.st_mode);
	else if (!(errno == ENOENT || errno == ENOTDIR || errno == EACCES))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not access file \"%s\": %m", name)));

	return false;
}

/*
 * pg_flush_data --- 建议操作系统刷新指定的脏数据
 *
 * offset 为 0 且 nbytes 为 0 表示应刷新整个文件
 */
void
pg_flush_data(int fd, off_t offset, off_t nbytes)
{
	/*
	 * 目前文件刷新主要用于减轻后续 fsync()/fdatasync() 调用的影响。
	 * 因此，如果禁用了 fsync，就不触发刷新——
	 * 这是我们将来可能希望改为可配置的决定。
	 */
	if (!enableFsync)
		return;

	/*
	 * 编译当前平台支持的所有替代方案，
	 * 以便更容易发现可移植性问题。
	 */
#if defined(HAVE_SYNC_FILE_RANGE)
	{
		int			rc;
		static bool not_implemented_by_kernel = false;

		if (not_implemented_by_kernel)
			return;

retry:

		/*
		 * sync_file_range(SYNC_FILE_RANGE_WRITE)，目前是 Linux 特有的，
		 * 告诉操作系统应该开始为指定块启动 writeback，但我们不想等待完成。
		 * 注意，如果该范围内有太多脏数据，此调用可能会阻塞。
		 * 在支持它的操作系统上，这是首选方法，
		 * 因为它在可用时能够可靠地工作（相比 msync()），
		 * 并且不会刷出干净数据（不像 FADV_DONTNEED）。
		 */
		rc = sync_file_range(fd, offset, nbytes,
							 SYNC_FILE_RANGE_WRITE);
		if (rc != 0)
		{
			int			elevel;

			if (errno == EINTR)
				goto retry;

			/*
			 * 对于没有 sync_file_range() 实现的系统（如 Windows WSL），
			 * 仅生成一次警告，然后抑制此进程的所有后续尝试。
			 */
			if (errno == ENOSYS)
			{
				elevel = WARNING;
				not_implemented_by_kernel = true;
			}
			else
				elevel = data_sync_elevel(WARNING);

			ereport(elevel,
					(errcode_for_file_access(),
					 errmsg("could not flush dirty data: %m")));
		}

		return;
	}
#endif
#if !defined(WIN32) && defined(MS_ASYNC)
	{
		void	   *p;
		static int	pagesize = 0;

		/*
		 * 在多个操作系统上，对 mmap 映射的文件执行 msync(MS_ASYNC)
		 * 会触发 writeback。在 Linux 上，只有指定 MS_SYNC 时才会这样做，
		 * 但那样会同步执行 writeback。幸运的是，所有常见的 Linux 系统
		 * 都有 sync_file_range()。这优于 FADV_DONTNEED，
		 * 因为它不会刷出干净数据。
		 *
		 * 我们映射文件（mmap()），告诉内核回写内容（msync()），
		 * 然后再次移除映射（munmap()）。
		 */

		/* 如果我们要映射整个文件，mmap() 需要实际长度 */
		if (offset == 0 && nbytes == 0)
		{
			nbytes = lseek(fd, 0, SEEK_END);
			if (nbytes < 0)
			{
				ereport(WARNING,
						(errcode_for_file_access(),
						 errmsg("could not determine dirty data size: %m")));
				return;
			}
		}

		/*
		 * 某些平台拒绝非整页对齐的 mmap() 尝试。
		 * 为应对此问题，将请求截断到页边界。
		 * 如果有些额外字节没有被刷新，没关系，反正这只是一个提示。
		 */

		/* 仅获取一次页面大小 */
		if (pagesize == 0)
			pagesize = sysconf(_SC_PAGESIZE);

		/* 将长度对齐到页面大小，丢弃不完整的页 */
		if (pagesize > 0)
			nbytes = (nbytes / pagesize) * pagesize;

		/* 不完整页的请求是无操作 */
		if (nbytes <= 0)
			return;

		/*
		 * mmap 很可能会失败，特别是在 32 位平台上，
		 * 可能根本没有足够的地址空间。
		 * 如果是这样，静默地回退到下一个实现。
		 */
		if (nbytes <= (off_t) SSIZE_MAX)
			p = mmap(NULL, nbytes, PROT_READ, MAP_SHARED, fd, offset);
		else
			p = MAP_FAILED;

		if (p != MAP_FAILED)
		{
			int			rc;

			rc = msync(p, (size_t) nbytes, MS_ASYNC);
			if (rc != 0)
			{
				ereport(data_sync_elevel(WARNING),
						(errcode_for_file_access(),
						 errmsg("could not flush dirty data: %m")));
				/* 注意：必须继续执行到 munmap()！ */
			}

			rc = munmap(p, (size_t) nbytes);
			if (rc != 0)
			{
				/* FATAL 错误，因为映射会残留 */
				ereport(FATAL,
						(errcode_for_file_access(),
						 errmsg("could not munmap() while flushing data: %m")));
			}

			return;
		}
	}
#endif
#if defined(USE_POSIX_FADVISE) && defined(POSIX_FADV_DONTNEED)
	{
		int			rc;

		/*
		 * 告知内核传入的范围不应再被缓存。
		 * 这会带来期望的副作用：写出脏数据；
		 * 也会带来不期望的副作用：可能会丢弃有用的干净缓存块。
		 * 出于后一个原因，这是最不可取的方法。
		 */

		rc = posix_fadvise(fd, offset, nbytes, POSIX_FADV_DONTNEED);

		if (rc != 0)
		{
			/* don't error out, this is just a performance optimization */
			ereport(WARNING,
					(errcode_for_file_access(),
					 errmsg("could not flush dirty data: %m")));
		}

		return;
	}
#endif
}

/*
 * 将打开的文件截断到指定长度。
 */
static int
pg_ftruncate(int fd, off_t length)
{
	int			ret;

retry:
	ret = ftruncate(fd, length);

	if (ret == -1 && errno == EINTR)
		goto retry;

	return ret;
}

/*
 * 通过文件名将文件截断到指定长度。
 */
int
pg_truncate(const char *path, off_t length)
{
	int			ret;
#ifdef WIN32
	int			save_errno;
	int			fd;

	fd = OpenTransientFile(path, O_RDWR | PG_BINARY);
	if (fd >= 0)
	{
		ret = pg_ftruncate(fd, length);
		save_errno = errno;
		CloseTransientFile(fd);
		errno = save_errno;
	}
	else
		ret = -1;
#else

retry:
	ret = truncate(path, length);

	if (ret == -1 && errno == EINTR)
		goto retry;
#endif

	return ret;
}

/*
 * fsync_fname -- 对文件或目录执行 fsync，妥善处理错误
 *
 * 尝试对文件或目录执行 fsync。对目录执行时，
 * 忽略那些表明操作系统不允许或不要求 fsync 目录的错误。
 */
void
fsync_fname(const char *fname, bool isdir)
{
	fsync_fname_ext(fname, isdir, false, data_sync_elevel(ERROR));
}

/*
 * durable_rename -- rename(2) 包装函数，发出确保持久性所需的 fsync
 *
 * 此例程确保在返回后，重命名文件的效果在崩溃情况下能够持久。
 * 在此例程运行期间发生崩溃，
 * 新文件的位置将保留旧文件或已移动的文件；
 * 不会出现混合状态或截断文件。
 *
 * 实现方式是在重命名之前对旧文件名和可能存在的目标文件名执行 fsync，
 * 重命名之后对目标文件和目录执行 fsync。
 *
 * 注意，rename() 不能在任意目录之间使用，
 * 因为它们可能不在同一个文件系统上。因此，本函数不支持跨目录重命名。
 *
 * 使用调用者指定的严重级别记录错误。
 *
 * 成功返回 0，否则返回 -1。注意，返回时 errno 无效。
 */
int
durable_rename(const char *oldfile, const char *newfile, int elevel)
{
	int			fd;

	/*
	 * 首先 fsync 旧路径和目标路径（如果存在），确保它们在磁盘上正确持久化。
	 * 同步目标文件不是严格必需的，但这使得对崩溃的推理更容易；
	 * 因为这样就保证了崩溃后源文件或目标文件之一存在。
	 */
	if (fsync_fname_ext(oldfile, false, false, elevel) != 0)
		return -1;

	fd = OpenTransientFile(newfile, PG_BINARY | O_RDWR);
	if (fd < 0)
	{
		if (errno != ENOENT)
		{
			ereport(elevel,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\": %m", newfile)));
			return -1;
		}
	}
	else
	{
		if (pg_fsync(fd) != 0)
		{
			int			save_errno;

			/* 出错时关闭文件，可能不在事务上下文中 */
			save_errno = errno;
			CloseTransientFile(fd);
			errno = save_errno;

			ereport(elevel,
					(errcode_for_file_access(),
					 errmsg("could not fsync file \"%s\": %m", newfile)));
			return -1;
		}

		if (CloseTransientFile(fd) != 0)
		{
			ereport(elevel,
					(errcode_for_file_access(),
					 errmsg("could not close file \"%s\": %m", newfile)));
			return -1;
		}
	}

	/* 是时候执行真正的操作了... */
	if (rename(oldfile, newfile) < 0)
	{
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not rename file \"%s\" to \"%s\": %m",
						oldfile, newfile)));
		return -1;
	}

	/*
	 * 为保证重命名文件的持久性，使用新文件名及其所在目录进行 fsync。
	 */
	if (fsync_fname_ext(newfile, false, false, elevel) != 0)
		return -1;

	if (fsync_parent_path(newfile, elevel) != 0)
		return -1;

	return 0;
}

/*
 * durable_unlink -- 以持久方式删除文件
 *
 * 此例程确保在返回后，删除文件的效果在崩溃情况下能够持久。
 * 在此例程运行期间发生崩溃，系统不会处于混合状态。
 *
 * 实现方式是在实际删除操作之后对文件的父目录执行 fsync。
 *
 * 使用调用者指定的严重级别记录错误。
 *
 * 成功返回 0，否则返回 -1。注意，返回时 errno 无效。
 */
int
durable_unlink(const char *fname, int elevel)
{
	if (unlink(fname) < 0)
	{
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not remove file \"%s\": %m",
						fname)));
		return -1;
	}

	/*
	 * 为保证删除文件的持久性，对其父目录执行 fsync。
	 */
	if (fsync_parent_path(fname, elevel) != 0)
		return -1;

	return 0;
}

/*
 * InitFileAccess --- 在后台进程启动期间初始化此模块
 *
 * 在正常或独立后台进程启动期间调用。
 * 在 postmaster 中*不*会被调用。
 *
 * 注意，这不初始化临时文件访问，
 * 后者通过 InitTemporaryFileAccess() 单独初始化。
 */
void
InitFileAccess(void)
{
	Assert(SizeVfdCache == 0);	/* 请只调用我一次 */

	/* 初始化缓存头条目 */
	VfdCache = (Vfd *) malloc(sizeof(Vfd));
	if (VfdCache == NULL)
		ereport(FATAL,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));

	MemSet(&(VfdCache[0]), 0, sizeof(Vfd));
	VfdCache->fd = VFD_CLOSED;

	SizeVfdCache = 1;
}

/*
 * InitTemporaryFileAccess --- 启动期间初始化临时文件访问
 *
 * 在正常或独立后台进程启动期间调用。
 * 在 postmaster 中*不*会被调用。
 *
 * 这与 InitFileAccess() 分开是因为临时文件清理可能导致 pgstat 报告。
 * 由于 pgstat 在 before_shmem_exit() 期间关闭，
 * 我们的报告必须在此之前发生。底层文件访问应可用更长时间，
 * 因此临时文件处理的初始化/关闭是分开的。
 */
void
InitTemporaryFileAccess(void)
{
	Assert(SizeVfdCache != 0);	/* InitFileAccess() 需要先运行 */
	Assert(!temporary_files_allowed);	/* 只调用我一次 */

	/*
	 * 注册 before-shmem-exit 钩子，确保在仍然能够报告统计信息时删除临时文件。
	 */
	before_shmem_exit(BeforeShmemExit_Files, 0);

#ifdef USE_ASSERT_CHECKING
	temporary_files_allowed = true;
#endif
}

/*
 * count_usable_fds --- 计算系统允许我们打开多少 FD，
 *		并估算已打开的数量。
 *
 * 如果 usable_fds 达到 max_to_probe 则停止计数。
 * 注意：较小的 max_to_probe 值可能会导致 already_open 的低估；
 * 必须填补已用 FD 集合中的所有"空隙"后，
 * already_open 的计算才能给出正确答案。
 * 在实践中，几十个的 max_to_probe 应该足够获得好的结果。
 *
 * 我们假定 stderr（FD 2）可用于 dup。
 * 虽然调用脚本理论上可以关闭它，但那将是一个非常糟糕的主意，
 * 因为这样可能会丢失来自 libc 等的错误消息。
 */
static void
count_usable_fds(int max_to_probe, int *usable_fds, int *already_open)
{
	int		   *fd;
	int			size;
	int			used = 0;
	int			highestfd = 0;
	int			j;

#ifdef HAVE_GETRLIMIT
	struct rlimit rlim;
	int			getrlimit_status;
#endif

	size = 1024;
	fd = (int *) palloc(size * sizeof(int));

#ifdef HAVE_GETRLIMIT
	getrlimit_status = getrlimit(RLIMIT_NOFILE, &rlim);
	if (getrlimit_status != 0)
		ereport(WARNING, (errmsg("getrlimit failed: %m")));
#endif							/* HAVE_GETRLIMIT */

	/* 不断 dup 直到失败或达到探测限制 */
	for (;;)
	{
		int			thisfd;

#ifdef HAVE_GETRLIMIT

		/*
		 * 不要超过 RLIMIT_NOFILE；在某些平台上会产生令人困扰的内核日志
		 */
		if (getrlimit_status == 0 && highestfd >= rlim.rlim_cur - 1)
			break;
#endif

		thisfd = dup(2);
		if (thisfd < 0)
		{
			/* 期望得到 EMFILE 或 ENFILE，否则有问题 */
			if (errno != EMFILE && errno != ENFILE)
				elog(WARNING, "duplicating stderr file descriptor failed after %d successes: %m", used);
			break;
		}

		if (used >= size)
		{
			size *= 2;
			fd = (int *) repalloc(fd, size * sizeof(int));
		}
		fd[used++] = thisfd;

		if (highestfd < thisfd)
			highestfd = thisfd;

		if (used >= max_to_probe)
			break;
	}

	/* 释放我们打开的文件 */
	for (j = 0; j < used; j++)
		close(fd[j]);

	pfree(fd);

	/*
	 * 返回结果。usable_fds 就是成功 dup 的次数。
	 * 我们假设系统限制是 highestfd+1（记住 0 是合法的 FD 编号），
	 * 所以 already_open = highestfd+1 - usable_fds。
	 */
	*usable_fds = used;
	*already_open = highestfd + 1 - used;
}

/*
 * set_max_safe_fds
 *		确定 fd.c 允许使用的文件描述符数量
 */
void
set_max_safe_fds(void)
{
	int			usable_fds;
	int			already_open;

	/*----------
	 * 我们希望将 max_safe_fds 设置为
	 *			MIN(usable_fds, max_files_per_process)
	 * 减去为未经 fd.c 协商而打开的文件预留的空间（slop）。
	 * 这确保我们不会打开超过 max_files_per_process
	 * 或实验确定的 EMFILE 限制的额外文件。
	 *----------
	 */
	count_usable_fds(max_files_per_process,
					 &usable_fds, &already_open);

	max_safe_fds = Min(usable_fds, max_files_per_process);

	/*
	 * 减去为 system() 等预留的 FD。
	 */
	max_safe_fds -= NUM_RESERVED_FDS;

	/*
	 * 确保我们仍有足够的 FD 来维持运行。
	 */
	if (max_safe_fds < FD_MINFREE)
		ereport(FATAL,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("insufficient file descriptors available to start server process"),
				 errdetail("System allows %d, server needs at least %d, %d files are already open.",
						   max_safe_fds + NUM_RESERVED_FDS,
						   FD_MINFREE + NUM_RESERVED_FDS,
						   already_open)));

	elog(DEBUG2, "max_safe_fds = %d, usable_fds = %d, already_open = %d",
		 max_safe_fds, usable_fds, already_open);
}

/*
 * 使用 BasicOpenFilePerm() 打开文件，并为 fileMode 参数传递默认文件模式。
 */
int
BasicOpenFile(const char *fileName, int fileFlags)
{
	return BasicOpenFilePerm(fileName, fileFlags, pg_file_create_mode);
}

/*
 * BasicOpenFilePerm --- 与 open(2) 相同，但可以在需要时释放其他 FD
 *
 * 导出的目的是供那些确实需要一个普通内核 FD，
 * 但又需要防止 FD 耗尽的场景使用。
 * 一旦成功返回 FD，调用者必须确保它不会在 ereport() 时泄漏！
 * 大多数用户*不应该*直接调用此例程，
 * 而应使用 VFD 抽象层，后者提供了防止描述符泄漏的保护，
 * 以及对需要长时间打开的文件的管理。
 *
 * 理想情况下，这应该是后端中*唯一*直接调用 open() 的地方。
 * 实际上，postmaster 直接调用了 open()，并且
 * 在后端启动早期也有一些直接 open() 调用。
 * 这些调用是可行的，因为那时本模块还没有任何打开的文件需要关闭。
 */
int
BasicOpenFilePerm(const char *fileName, int fileFlags, mode_t fileMode)
{
	int			fd;

tryAgain:
#ifdef PG_O_DIRECT_USE_F_NOCACHE

	/*
	 * 我们定义用来在用 F_NOCACHE 模拟 O_DIRECT 时替代它的值
	 * 最好不与任何标准标志冲突。
	 */
	StaticAssertStmt((PG_O_DIRECT &
					  (O_APPEND |
					   O_CLOEXEC |
					   O_CREAT |
					   O_DSYNC |
					   O_EXCL |
					   O_RDWR |
					   O_RDONLY |
					   O_SYNC |
					   O_TRUNC |
					   O_WRONLY)) == 0,
					 "PG_O_DIRECT value collides with standard flag");
	fd = open(fileName, fileFlags & ~PG_O_DIRECT, fileMode);
#else
	fd = open(fileName, fileFlags, fileMode);
#endif

	if (fd >= 0)
	{
#ifdef PG_O_DIRECT_USE_F_NOCACHE
		if (fileFlags & PG_O_DIRECT)
		{
			if (fcntl(fd, F_NOCACHE, 1) < 0)
			{
				int			save_errno = errno;

				close(fd);
				errno = save_errno;
				return -1;
			}
		}
#endif

		return fd;				/* 成功！ */
	}

	if (errno == EMFILE || errno == ENFILE)
	{
		int			save_errno = errno;

		ereport(LOG,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("out of file descriptors: %m; release and retry")));
		errno = 0;
		if (ReleaseLruFile())
			goto tryAgain;
		errno = save_errno;
	}

	return -1;					/* 失败 */
}

/*
 * AcquireExternalFD - 尝试预留一个外部文件描述符
 *
 * 应由那些需要长时间持有文件描述符，
 * 但又无法使用本模块提供的其他设施的调用者使用。
 *
 * 此函数与底层 ReserveExternalFD 函数的区别在于，
 * 如果"过多"外部 FD 已被预留，
 * 此函数会报告失败（通过设置 errno 并返回 false）。
 * 这应该用于需要预留的 FD 总数不可预测且不小的代码中。
 */
bool
AcquireExternalFD(void)
{
	/*
	 * 我们不希望超过 max_safe_fds / 3 的 FD 被消耗用于"外部"FD。
	 */
	if (numExternalFDs < max_safe_fds / 3)
	{
		ReserveExternalFD();
		return true;
	}
	errno = EMFILE;
	return false;
}

/*
 * ReserveExternalFD - 报告一个文件描述符的外部消耗
 *
 * 应由那些需要长时间持有文件描述符，
 * 但又无法使用本模块提供的其他设施的调用者使用。
 * 它仅跟踪 FD 的使用并在需要时关闭 VFD，
 * 以确保我们保持 NUM_RESERVED_FDS 个 FD 可用。
 *
 * 仅在无法预留 FD 会导致致命错误的代码中直接调用此函数；
 * 例如，WAL 写入代码就是这样做的，因为其替代方案是会话失败。
 * 此外，在可能每个进程消耗超过一个 FD 的代码中这样做是非常不明智的。
 *
 * 注意：只要每个参与者表现良好，使 NUM_RESERVED_FDS 个 FD 保持可用，
 * 在实际打开 FD 之前还是之后调用此函数并不太重要；
 * 但提前调用可以降低在并非每个人都表现良好时发生 EMFILE 失败的风险。
 * 无论如何，保持外部 FD 计数与现实同步完全是调用者的责任。
 */
void
ReserveExternalFD(void)
{
	/*
	 * 在需要时释放 VFD 以保持安全。
	 * 因为我们在递增 numExternalFDs 之前执行此操作，
	 * 最终状态将如预期一样，即
	 * nfile + numAllocatedDescs + numExternalFDs <= max_safe_fds。
	 */
	ReleaseLruFiles();

	numExternalFDs++;
}

/*
 * ReleaseExternalFD - 报告释放一个外部文件描述符
 *
 * 此函数保证不会更改 errno，因此可在失败路径中使用。
 */
void
ReleaseExternalFD(void)
{
	Assert(numExternalFDs > 0);
	numExternalFDs--;
}


#if defined(FDDEBUG)

static void
_dump_lru(void)
{
	int			mru = VfdCache[0].lruLessRecently;
	Vfd		   *vfdP = &VfdCache[mru];
	char		buf[2048];

	snprintf(buf, sizeof(buf), "LRU: MOST %d ", mru);
	while (mru != 0)
	{
		mru = vfdP->lruLessRecently;
		vfdP = &VfdCache[mru];
		snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "%d ", mru);
	}
	snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "LEAST");
	elog(LOG, "%s", buf);
}
#endif							/* FDDEBUG */

static void
Delete(File file)
{
	Vfd		   *vfdP;

	Assert(file != 0);

	DO_DB(elog(LOG, "Delete %d (%s)",
			   file, VfdCache[file].fileName));
	DO_DB(_dump_lru());

	vfdP = &VfdCache[file];

	VfdCache[vfdP->lruLessRecently].lruMoreRecently = vfdP->lruMoreRecently;
	VfdCache[vfdP->lruMoreRecently].lruLessRecently = vfdP->lruLessRecently;

	DO_DB(_dump_lru());
}

static void
LruDelete(File file)
{
	Vfd		   *vfdP;

	Assert(file != 0);

	DO_DB(elog(LOG, "LruDelete %d (%s)",
			   file, VfdCache[file].fileName));

	vfdP = &VfdCache[file];

	pgaio_closing_fd(vfdP->fd);

	/*
	 * 关闭文件。我们不期望这会失败；如果失败，
	 * 宁可泄漏 FD 也不能搞乱我们的内部状态。
	 */
	if (close(vfdP->fd) != 0)
		elog(vfdP->fdstate & FD_TEMP_FILE_LIMIT ? LOG : data_sync_elevel(LOG),
			 "could not close file \"%s\": %m", vfdP->fileName);
	vfdP->fd = VFD_CLOSED;
	--nfile;

	/* 从 LRU 环中删除 vfd 记录 */
	Delete(file);
}

static void
Insert(File file)
{
	Vfd		   *vfdP;

	Assert(file != 0);

	DO_DB(elog(LOG, "Insert %d (%s)",
			   file, VfdCache[file].fileName));
	DO_DB(_dump_lru());

	vfdP = &VfdCache[file];

	vfdP->lruMoreRecently = 0;
	vfdP->lruLessRecently = VfdCache[0].lruLessRecently;
	VfdCache[0].lruLessRecently = file;
	VfdCache[vfdP->lruLessRecently].lruMoreRecently = file;

	DO_DB(_dump_lru());
}

/* 成功返回 0，重新打开失败返回 -1（并设置 errno） */
static int
LruInsert(File file)
{
	Vfd		   *vfdP;

	Assert(file != 0);

	DO_DB(elog(LOG, "LruInsert %d (%s)",
			   file, VfdCache[file].fileName));

	vfdP = &VfdCache[file];

	if (FileIsNotOpen(file))
	{
		/* 关闭多余的内核 FD。 */
		ReleaseLruFiles();

		/*
		 * 由于文件描述符不足（例如整个系统文件表已满），
		 * 打开操作仍可能失败。因此，准备好必要时释放另一个 FD...
		 */
		vfdP->fd = BasicOpenFilePerm(vfdP->fileName, vfdP->fileFlags,
									 vfdP->fileMode);
		if (vfdP->fd < 0)
		{
			DO_DB(elog(LOG, "re-open failed: %m"));
			return -1;
		}
		else
		{
			++nfile;
		}
	}

	/*
	 * 将其放在 Lru 环的头部
	 */

	Insert(file);

	return 0;
}

/*
 * 通过关闭最近最少使用的 VFD 来释放一个内核 FD。
 */
static bool
ReleaseLruFile(void)
{
	DO_DB(elog(LOG, "ReleaseLruFile. Opened %d", nfile));

	if (nfile > 0)
	{
		/*
		 * 有打开的文件，因此环中至少应该有一个已使用的 vfd。
		 */
		Assert(VfdCache[0].lruMoreRecently != 0);
		LruDelete(VfdCache[0].lruMoreRecently);
		return true;			/* 释放了一个文件 */
	}
	return false;				/* 没有可释放的文件 */
}

/*
 * 根据需要释放内核 FD，使数量低于 max_safe_fds 限制。
 * 调用此函数后，可以安全地尝试打开另一个文件。
 */
static void
ReleaseLruFiles(void)
{
	while (nfile + numAllocatedDescs + numExternalFDs >= max_safe_fds)
	{
		if (!ReleaseLruFile())
			break;
	}
}

static File
AllocateVfd(void)
{
	Index		i;
	File		file;

	DO_DB(elog(LOG, "AllocateVfd. Size %zu", SizeVfdCache));

	Assert(SizeVfdCache > 0);	/* InitFileAccess 没有被调用？ */

	if (VfdCache[0].nextFree == 0)
	{
		/*
		 * 空闲链表为空，需要增加数组大小。
		 * 每次发生时我们选择将其加倍。
		 * 不过，从*真正*很小开始没有什么意义。
		 */
		Size		newCacheSize = SizeVfdCache * 2;
		Vfd		   *newVfdCache;

		if (newCacheSize < 32)
			newCacheSize = 32;

		/*
		 * 注意不要让 realloc 失败时破坏 VfdCache 指针。
		 */
		newVfdCache = (Vfd *) realloc(VfdCache, sizeof(Vfd) * newCacheSize);
		if (newVfdCache == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		VfdCache = newVfdCache;

		/*
		 * 初始化新条目并将它们链接到空闲链表中。
		 */
		for (i = SizeVfdCache; i < newCacheSize; i++)
		{
			MemSet(&(VfdCache[i]), 0, sizeof(Vfd));
			VfdCache[i].nextFree = i + 1;
			VfdCache[i].fd = VFD_CLOSED;
		}
		VfdCache[newCacheSize - 1].nextFree = 0;
		VfdCache[0].nextFree = SizeVfdCache;

		/*
		 * 记录新的大小
		 */
		SizeVfdCache = newCacheSize;
	}

	file = VfdCache[0].nextFree;

	VfdCache[0].nextFree = VfdCache[file].nextFree;

	return file;
}

static void
FreeVfd(File file)
{
	Vfd		   *vfdP = &VfdCache[file];

	DO_DB(elog(LOG, "FreeVfd: %d (%s)",
			   file, vfdP->fileName ? vfdP->fileName : ""));

	if (vfdP->fileName != NULL)
	{
		free(vfdP->fileName);
		vfdP->fileName = NULL;
	}
	vfdP->fdstate = 0x0;

	vfdP->nextFree = VfdCache[0].nextFree;
	VfdCache[0].nextFree = file;
}

/* 成功返回 0，重新打开失败返回 -1（并设置 errno） */
static int
FileAccess(File file)
{
	int			returnValue;

	DO_DB(elog(LOG, "FileAccess %d (%s)",
			   file, VfdCache[file].fileName));

	/*
	 * 文件是否已打开？如果未打开，则打开它并将其放在 LRU 环的头部
	 * （可能会关闭最近最少使用的文件以获取 FD）。
	 */

	if (FileIsNotOpen(file))
	{
		returnValue = LruInsert(file);
		if (returnValue != 0)
			return returnValue;
	}
	else if (VfdCache[0].lruLessRecently != file)
	{
		/*
		 * 现在我们知道文件已打开且不是最后访问的，
		 * 因此需要将其移动到 LRU 环的头部。
		 */

		Delete(file);
		Insert(file);
	}

	return 0;
}

/*
 * 当临时文件被删除时调用，以报告其大小。
 */
static void
ReportTemporaryFileUsage(const char *path, off_t size)
{
	pgstat_report_tempfile(size);

	if (log_temp_files >= 0)
	{
		if ((size / 1024) >= log_temp_files)
			ereport(LOG,
					(errmsg("temporary file: path \"%s\", size %lu",
							path, (unsigned long) size)));
	}
}

/*
 * 注册一个临时文件以便自动关闭。
 * 在文件打开之前必须已调用 ResourceOwnerEnlarge(CurrentResourceOwner)。
 */
static void
RegisterTemporaryFile(File file)
{
	ResourceOwnerRememberFile(CurrentResourceOwner, file);
	VfdCache[file].resowner = CurrentResourceOwner;

	/* 事务结束时关闭的备用机制。 */
	VfdCache[file].fdstate |= FD_CLOSE_AT_EOXACT;
	have_xact_temporary_files = true;
}

/*
 * 当收到某个关系的共享失效消息时调用。
 */
#ifdef NOT_USED
void
FileInvalidate(File file)
{
	Assert(FileIsValid(file));
	if (!FileIsNotOpen(file))
		LruDelete(file);
}
#endif

/*
 * 使用 PathNameOpenFilePerm() 打开文件，并为 fileMode 参数传递默认文件模式。
 */
File
PathNameOpenFile(const char *fileName, int fileFlags)
{
	return PathNameOpenFilePerm(fileName, fileFlags, pg_file_create_mode);
}

/*
 * 在任意目录中打开一个文件
 *
 * 注意：如果传递的路径名是相对路径（通常是这样），
 * 它将相对于进程的工作目录来解释
 * （当此代码运行时，工作目录应始终是 $PGDATA）。
 */
File
PathNameOpenFilePerm(const char *fileName, int fileFlags, mode_t fileMode)
{
	char	   *fnamecopy;
	File		file;
	Vfd		   *vfdP;

	DO_DB(elog(LOG, "PathNameOpenFilePerm: %s %x %o",
			   fileName, fileFlags, fileMode));

	/*
	 * 我们需要一个 malloc 分配的文件名副本；如果空间不足则干净地失败。
	 */
	fnamecopy = strdup(fileName);
	if (fnamecopy == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));

	file = AllocateVfd();
	vfdP = &VfdCache[file];

	/* 关闭多余的内核 FD。 */
	ReleaseLruFiles();

	/*
	 * VFD 管理的描述符隐式标记为 O_CLOEXEC。
	 * 客户端不应被期望知道哪些内核描述符当前是打开的，
	 * 因此它们被执行的子程序继承是没有意义的。
	 */
	fileFlags |= O_CLOEXEC;

	vfdP->fd = BasicOpenFilePerm(fileName, fileFlags, fileMode);

	if (vfdP->fd < 0)
	{
		int			save_errno = errno;

		FreeVfd(file);
		free(fnamecopy);
		errno = save_errno;
		return -1;
	}
	++nfile;
	DO_DB(elog(LOG, "PathNameOpenFile: success %d",
			   vfdP->fd));

	vfdP->fileName = fnamecopy;
	/* 保存的标志已调整，适合重新打开文件 */
	vfdP->fileFlags = fileFlags & ~(O_CREAT | O_TRUNC | O_EXCL);
	vfdP->fileMode = fileMode;
	vfdP->fileSize = 0;
	vfdP->fdstate = 0x0;
	vfdP->resowner = NULL;

	Insert(file);

	return file;
}

/*
 * 创建目录 'directory'。如有必要，创建 'basedir'，
 * 后者必须是其上方的目录。
 * 这设计用于在按需创建下方的目录之前，先按需创建顶级临时目录。
 * 如果目录已存在，则不做任何操作。
 *
 * 在顶级临时目录中创建的目录应以 PG_TEMP_FILE_PREFIX 开头，
 * 以便在启动时被 RemovePgTempFiles() 识别为临时文件并删除。
 * 其下的子目录不需要任何特定前缀。
*/
void
PathNameCreateTemporaryDir(const char *basedir, const char *directory)
{
	if (MakePGDirectory(directory) < 0)
	{
		if (errno == EEXIST)
			return;

		/*
		 * 失败了。先尝试创建 basedir，以防它不存在。
		 * 容忍 EEXIST 以应对另一个进程遵循相同算法的竞争条件。
		 */
		if (MakePGDirectory(basedir) < 0 && errno != EEXIST)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("cannot create temporary directory \"%s\": %m",
							basedir)));

		/* 再试一次。 */
		if (MakePGDirectory(directory) < 0 && errno != EEXIST)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("cannot create temporary subdirectory \"%s\": %m",
							directory)));
	}
}

/*
 * 删除一个目录及其中的所有内容（如果存在）。
 */
void
PathNameDeleteTemporaryDir(const char *dirname)
{
	struct stat statbuf;

	/* 静默忽略不存在的目录。 */
	if (stat(dirname, &statbuf) != 0 && errno == ENOENT)
		return;

	/*
	 * 目前 walkdir 不提供让传入函数维持状态的方式。
	 * 也许它应该提供，这样我们就可以告诉调用者此操作是成功还是失败。
	 * 由于此操作用于清理路径，实际上我们不会有不同的行为：
	 * 我们只是记录失败。
	 */
	walkdir(dirname, unlink_if_exists_fname, false, LOG);
}

/*
 * 打开一个临时文件，该文件在关闭时会消失。
 *
 * 此例程负责生成适当的临时文件名。
 * 不需要传入 fileFlags 或 fileMode，因为对临时文件来说只有一种设置有意义。
 *
 * 除非 interXact 为 true，否则文件会被 CurrentResourceOwner 记住，
 * 以确保在不再需要时（通常是在事务结束时）关闭并删除。
 * 在大多数情况下，你不希望临时文件的生命周期超过创建它的事务，
 * 所以这里应为 false——但如果需要"某种程度上"临时的存储，这可能很有用。
 * 无论哪种情况，当 File 被显式关闭时文件都会被删除。
 */
File
OpenTemporaryFile(bool interXact)
{
	File		file = 0;

	Assert(temporary_files_allowed);	/* 检查临时文件访问已启用 */

	/*
	 * 在打开文件之前，确保当前资源所有者为此 File 预留了空间，
	 * 如果我们将在下面注册它的话。
	 */
	if (!interXact)
		ResourceOwnerEnlarge(CurrentResourceOwner);

	/*
	 * 如果给定了临时表空间，则尝试使用下一个。
	 * 如果找不到给定的表空间，则静默回退到数据库的默认表空间。
	 *
	 * 但是：如果临时文件将存活超过当前事务，则强制将其放入数据库的默认表空间，
	 * 以免对可能的表空间删除操作构成威胁。
	 */
	if (numTempTableSpaces > 0 && !interXact)
	{
		Oid			tblspcOid = GetNextTempTableSpace();

		if (OidIsValid(tblspcOid))
			file = OpenTemporaryFileInTablespace(tblspcOid, false);
	}

	/*
	 * 如果没有，或者表空间无效，则在数据库的默认表空间中创建。
	 * MyDatabaseTableSpace 通常在我们到达这里之前就应该设置好了，
	 * 但以防万一没有设置，回退到 pg_default 表空间。
	 */
	if (file <= 0)
		file = OpenTemporaryFileInTablespace(MyDatabaseTableSpace ?
											 MyDatabaseTableSpace :
											 DEFAULTTABLESPACE_OID,
											 true);

	/* 标记为关闭时删除并受临时文件大小限制 */
	VfdCache[file].fdstate |= FD_DELETE_AT_CLOSE | FD_TEMP_FILE_LIMIT;

	/* 向当前资源所有者注册它 */
	if (!interXact)
		RegisterTemporaryFile(file);

	return file;
}

/*
 * 返回给定表空间中临时目录的路径。
 */
void
TempTablespacePath(char *path, Oid tablespace)
{
	/*
	 * 识别此表空间的临时文件目录。
	 *
	 * 如果有人试图指定 pg_global，则改用 pg_default。
	 */
	if (tablespace == InvalidOid ||
		tablespace == DEFAULTTABLESPACE_OID ||
		tablespace == GLOBALTABLESPACE_OID)
		snprintf(path, MAXPGPATH, "base/%s", PG_TEMP_FILES_DIR);
	else
	{
		/* 所有其他表空间通过符号链接访问 */
		snprintf(path, MAXPGPATH, "%s/%u/%s/%s",
				 PG_TBLSPC_DIR, tablespace, TABLESPACE_VERSION_DIRECTORY,
				 PG_TEMP_FILES_DIR);
	}
}

/*
 * 在特定表空间中打开一个临时文件。
 * 这是 OpenTemporaryFile 的子例程，详情参见该函数。
 */
static File
OpenTemporaryFileInTablespace(Oid tblspcOid, bool rejectError)
{
	char		tempdirpath[MAXPGPATH];
	char		tempfilepath[MAXPGPATH];
	File		file;

	TempTablespacePath(tempdirpath, tblspcOid);

	/*
	 * 生成一个应在当前数据库实例内唯一的临时文件名。
	 */
	snprintf(tempfilepath, sizeof(tempfilepath), "%s/%s%d.%ld",
			 tempdirpath, PG_TEMP_FILE_PREFIX, MyProcPid, tempFileCounter++);

	/*
	 * 打开文件。注意：我们不使用 O_EXCL，以防存在可重用的孤立临时文件。
	 */
	file = PathNameOpenFile(tempfilepath,
							O_RDWR | O_CREAT | O_TRUNC | PG_BINARY);
	if (file <= 0)
	{
		/*
		 * 我们可能需要创建表空间的临时文件目录，如果还没有人创建的话。
		 *
		 * 不检查 MakePGDirectory 的错误；如果其他人刚好做了同样的事，
		 * 它可能会失败。如果不成功，我们会在第二次创建尝试时失败。
		 */
		(void) MakePGDirectory(tempdirpath);

		file = PathNameOpenFile(tempfilepath,
								O_RDWR | O_CREAT | O_TRUNC | PG_BINARY);
		if (file <= 0 && rejectError)
			elog(ERROR, "could not create temporary file \"%s\": %m",
				 tempfilepath);
	}

	return file;
}


/*
 * 创建一个新文件。包含该文件的目录必须已经存在。
 * 以这种方式创建的文件受 temp_file_limit 约束，
 * 并在事务结束时自动关闭，但不会在关闭时自动删除，
 * 因为它们旨在多个协作后端之间共享。
 *
 * 如果文件位于顶级临时目录中，其名称应以 PG_TEMP_FILE_PREFIX 开头，
 * 以便在启动时被 RemovePgTempFiles() 识别为临时文件并删除。
 * 或者，它可以位于由 PathNameCreateTemporaryDir() 创建的目录中，
 * 这种情况下不需要前缀。
 */
File
PathNameCreateTemporaryFile(const char *path, bool error_on_failure)
{
	File		file;

	Assert(temporary_files_allowed);	/* 检查临时文件访问已启用 */

	ResourceOwnerEnlarge(CurrentResourceOwner);

	/*
	 * 打开文件。注意：我们不使用 O_EXCL，以防存在可重用的孤立临时文件。
	 */
	file = PathNameOpenFile(path, O_RDWR | O_CREAT | O_TRUNC | PG_BINARY);
	if (file <= 0)
	{
		if (error_on_failure)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not create temporary file \"%s\": %m",
							path)));
		else
			return file;
	}

	/* 标记为计入 temp_file_limit。 */
	VfdCache[file].fdstate |= FD_TEMP_FILE_LIMIT;

	/* 注册以便自动关闭。 */
	RegisterTemporaryFile(file);

	return file;
}

/*
 * 打开一个由 PathNameCreateTemporaryFile 创建的文件，可能是在另一个后端中创建的。
 * 以这种方式打开的文件不计入调用者的 temp_file_limit，
 * 在事务结束时自动关闭，但不会在关闭时删除。
 */
File
PathNameOpenTemporaryFile(const char *path, int mode)
{
	File		file;

	Assert(temporary_files_allowed);	/* 检查临时文件访问已启用 */

	ResourceOwnerEnlarge(CurrentResourceOwner);

	file = PathNameOpenFile(path, mode | PG_BINARY);

	/* 如果没有这样的文件，则不引发错误。 */
	if (file <= 0 && errno != ENOENT)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open temporary file \"%s\": %m",
						path)));

	if (file > 0)
	{
		/* 注册以便自动关闭。 */
		RegisterTemporaryFile(file);
	}

	return file;
}

/*
 * 按路径名删除文件。如果文件存在则返回 true，否则返回 false。
 */
bool
PathNameDeleteTemporaryFile(const char *path, bool error_on_failure)
{
	struct stat filestats;
	int			stat_errno;

	/* 获取最终大小以用于 pgstat 报告。 */
	if (stat(path, &filestats) != 0)
		stat_errno = errno;
	else
		stat_errno = 0;

	/*
	 * 与 FileClose 的自动文件删除代码不同，我们容忍文件不存在，
	 * 以支持 BufFileDeleteFileSet，后者在删除完所有段之前不知道需要删除多少段。
	 */
	if (stat_errno == ENOENT)
		return false;

	if (unlink(path) < 0)
	{
		if (errno != ENOENT)
			ereport(error_on_failure ? ERROR : LOG,
					(errcode_for_file_access(),
					 errmsg("could not unlink temporary file \"%s\": %m",
							path)));
		return false;
	}

	if (stat_errno == 0)
		ReportTemporaryFileUsage(path, filestats.st_size);
	else
	{
		errno = stat_errno;
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m", path)));
	}

	return true;
}

/*
 * 用完文件后关闭它
 */
void
FileClose(File file)
{
	Vfd		   *vfdP;

	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FileClose: %d (%s)",
			   file, VfdCache[file].fileName));

	vfdP = &VfdCache[file];

	if (!FileIsNotOpen(file))
	{
		pgaio_closing_fd(vfdP->fd);

		/* 关闭文件 */
		if (close(vfdP->fd) != 0)
		{
			/*
			 * 关闭非临时文件失败时可能需要 panic；参见 LruDelete。
			 */
			elog(vfdP->fdstate & FD_TEMP_FILE_LIMIT ? LOG : data_sync_elevel(LOG),
				 "could not close file \"%s\": %m", vfdP->fileName);
		}

		--nfile;
		vfdP->fd = VFD_CLOSED;

		/* 从 lru 环中移除文件 */
		Delete(file);
	}

	if (vfdP->fdstate & FD_TEMP_FILE_LIMIT)
	{
		/* 从当前使用量中减去其大小（优先执行以防出错） */
		temporary_files_size -= vfdP->fileSize;
		vfdP->fileSize = 0;
	}

	/*
	 * 如果文件是临时的则删除它，并根据需要记录日志
	 */
	if (vfdP->fdstate & FD_DELETE_AT_CLOSE)
	{
		struct stat filestats;
		int			stat_errno;

		/*
		 * 如果发生错误（可能在 ereport/elog 调用中发生），
		 * 在事务中止期间我们会直接回到这里。重置此标志以确保不会陷入无限循环。
		 * 此代码的安排确保最坏后果是未能发出日志消息，而不是未能尝试 unlink。
		 */
		vfdP->fdstate &= ~FD_DELETE_AT_CLOSE;


		/* 首先尝试 stat() */
		if (stat(vfdP->fileName, &filestats))
			stat_errno = errno;
		else
			stat_errno = 0;

		/* 无论如何都要执行 unlink */
		if (unlink(vfdP->fileName))
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not delete file \"%s\": %m", vfdP->fileName)));

		/* 最后报告 stat 结果 */
		if (stat_errno == 0)
			ReportTemporaryFileUsage(vfdP->fileName, filestats.st_size);
		else
		{
			errno = stat_errno;
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not stat file \"%s\": %m", vfdP->fileName)));
		}
	}

	/* 从资源所有者中注销 */
	if (vfdP->resowner)
		ResourceOwnerForgetFile(vfdP->resowner, file);

	/*
	 * 将 Vfd 槽位归还到空闲链表
	 */
	FreeVfd(file);
}

/*
 * FilePrefetch - 启动文件给定范围的异步读取。
 *
 * 成功返回 0，否则返回一个 errno 错误码（类似 posix_fadvise()）。
 *
 * posix_fadvise() 是实现此功能的最简单的标准化接口。
 */
int
FilePrefetch(File file, off_t offset, off_t amount, uint32 wait_event_info)
{
	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FilePrefetch: %d (%s) " INT64_FORMAT " " INT64_FORMAT,
			   file, VfdCache[file].fileName,
			   (int64) offset, (int64) amount));

#if defined(USE_POSIX_FADVISE) && defined(POSIX_FADV_WILLNEED)
	{
		int			returnCode;

		returnCode = FileAccess(file);
		if (returnCode < 0)
			return returnCode;

retry:
		pgstat_report_wait_start(wait_event_info);
		returnCode = posix_fadvise(VfdCache[file].fd, offset, amount,
								   POSIX_FADV_WILLNEED);
		pgstat_report_wait_end();

		if (returnCode == EINTR)
			goto retry;

		return returnCode;
	}
#elif defined(__darwin__)
	{
		struct radvisory
		{
		off_t		ra_offset;	/* 文件中的偏移量 */
		int			ra_count;	/* 读取大小         */
		}			ra;
		int			returnCode;

		returnCode = FileAccess(file);
		if (returnCode < 0)
			return returnCode;

		ra.ra_offset = offset;
		ra.ra_count = amount;
		pgstat_report_wait_start(wait_event_info);
		returnCode = fcntl(VfdCache[file].fd, F_RDADVISE, &ra);
		pgstat_report_wait_end();
		if (returnCode != -1)
			return 0;
		else
			return errno;
	}
#else
	return 0;
#endif
}

void
FileWriteback(File file, off_t offset, off_t nbytes, uint32 wait_event_info)
{
	int			returnCode;

	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FileWriteback: %d (%s) " INT64_FORMAT " " INT64_FORMAT,
			   file, VfdCache[file].fileName,
			   (int64) offset, (int64) nbytes));

	if (nbytes <= 0)
		return;

	if (VfdCache[file].fileFlags & PG_O_DIRECT)
		return;

	returnCode = FileAccess(file);
	if (returnCode < 0)
		return;

	pgstat_report_wait_start(wait_event_info);
	pg_flush_data(VfdCache[file].fd, offset, nbytes);
	pgstat_report_wait_end();
}

ssize_t
FileReadV(File file, const struct iovec *iov, int iovcnt, off_t offset,
		  uint32 wait_event_info)
{
	ssize_t		returnCode;
	Vfd		   *vfdP;

	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FileReadV: %d (%s) " INT64_FORMAT " %d",
			   file, VfdCache[file].fileName,
			   (int64) offset,
			   iovcnt));

	returnCode = FileAccess(file);
	if (returnCode < 0)
		return returnCode;

	vfdP = &VfdCache[file];

retry:
	pgstat_report_wait_start(wait_event_info);
	returnCode = pg_preadv(vfdP->fd, iov, iovcnt, offset);
	pgstat_report_wait_end();

	if (returnCode < 0)
	{
		/*
		 * Windows 可能耗尽内核缓冲区并返回 "Insufficient
		 * system resources" 错误。等待片刻后重试以解决此问题。
		 *
		 * 据传在某些 Unix 文件系统上也可能出现 EINTR，
		 * 这种情况下需要立即重试。
		 */
#ifdef WIN32
		DWORD		error = GetLastError();

		switch (error)
		{
			case ERROR_NO_SYSTEM_RESOURCES:
				pg_usleep(1000L);
				errno = EINTR;
				break;
			default:
				_dosmaperr(error);
				break;
		}
#endif
		/* 如果被中断则重试 */
		if (errno == EINTR)
			goto retry;
	}

	return returnCode;
}

int
FileStartReadV(PgAioHandle *ioh, File file,
			   int iovcnt, off_t offset,
			   uint32 wait_event_info)
{
	int			returnCode;
	Vfd		   *vfdP;

	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FileStartReadV: %d (%s) " INT64_FORMAT " %d",
			   file, VfdCache[file].fileName,
			   (int64) offset,
			   iovcnt));

	returnCode = FileAccess(file);
	if (returnCode < 0)
		return returnCode;

	vfdP = &VfdCache[file];

	pgaio_io_start_readv(ioh, vfdP->fd, iovcnt, offset);

	return 0;
}

ssize_t
FileWriteV(File file, const struct iovec *iov, int iovcnt, off_t offset,
		   uint32 wait_event_info)
{
	ssize_t		returnCode;
	Vfd		   *vfdP;

	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FileWriteV: %d (%s) " INT64_FORMAT " %d",
			   file, VfdCache[file].fileName,
			   (int64) offset,
			   iovcnt));

	returnCode = FileAccess(file);
	if (returnCode < 0)
		return returnCode;

	vfdP = &VfdCache[file];

	/*
	 * 如果强制实施 temp_file_limit 且这是临时文件，
	 * 检查此写入是否会超过 temp_file_limit，如果是则抛出错误。
	 * 注意：在此处抛出错误确实是模块化设计的违规；
	 * 我们应该设置 errno 并返回 -1。
	 * 然而，如果这样做，就没有办法报告合适的错误消息。
	 * 所有当前调用者反正也会立即抛出错误，所以目前这是安全的。
	 */
	if (temp_file_limit >= 0 && (vfdP->fdstate & FD_TEMP_FILE_LIMIT))
	{
		off_t		past_write = offset;

		for (int i = 0; i < iovcnt; ++i)
			past_write += iov[i].iov_len;

		if (past_write > vfdP->fileSize)
		{
			uint64		newTotal = temporary_files_size;

			newTotal += past_write - vfdP->fileSize;
			if (newTotal > (uint64) temp_file_limit * (uint64) 1024)
				ereport(ERROR,
						(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
						 errmsg("temporary file size exceeds \"temp_file_limit\" (%dkB)",
								temp_file_limit)));
		}
	}

retry:
	pgstat_report_wait_start(wait_event_info);
	returnCode = pg_pwritev(vfdP->fd, iov, iovcnt, offset);
	pgstat_report_wait_end();

	if (returnCode >= 0)
	{
		/*
		 * 某些调用者期望短写入会设置 errno，传统上我们假设这表示磁盘空间不足。
		 * 我们不想浪费 CPU 周期在这里累加总大小，
		 * 所以对所有成功的写入都设置它，
		 * 以防某个调用者确定写入是短写入并 ereports("%m")。
		 */
		errno = ENOSPC;

		/*
		 * 如果是临时文件，维护 fileSize 和 temporary_files_size。
		 */
		if (vfdP->fdstate & FD_TEMP_FILE_LIMIT)
		{
			off_t		past_write = offset + returnCode;

			if (past_write > vfdP->fileSize)
			{
				temporary_files_size += past_write - vfdP->fileSize;
				vfdP->fileSize = past_write;
			}
		}
	}
	else
	{
		/*
		 * 参见 FileReadV() 中的注释
		 */
#ifdef WIN32
		DWORD		error = GetLastError();

		switch (error)
		{
			case ERROR_NO_SYSTEM_RESOURCES:
				pg_usleep(1000L);
				errno = EINTR;
				break;
			default:
				_dosmaperr(error);
				break;
		}
#endif
		/* 如果被中断则重试 */
		if (errno == EINTR)
			goto retry;
	}

	return returnCode;
}

int
FileSync(File file, uint32 wait_event_info)
{
	int			returnCode;

	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FileSync: %d (%s)",
			   file, VfdCache[file].fileName));

	returnCode = FileAccess(file);
	if (returnCode < 0)
		return returnCode;

	pgstat_report_wait_start(wait_event_info);
	returnCode = pg_fsync(VfdCache[file].fd);
	pgstat_report_wait_end();

	return returnCode;
}

/*
 * 将文件的一个区域置零。
 *
 * 成功返回 0，否则返回 -1。在后一种情况下 errno 设置为相应的错误码。
 */
int
FileZero(File file, off_t offset, off_t amount, uint32 wait_event_info)
{
	int			returnCode;
	ssize_t		written;

	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FileZero: %d (%s) " INT64_FORMAT " " INT64_FORMAT,
			   file, VfdCache[file].fileName,
			   (int64) offset, (int64) amount));

	returnCode = FileAccess(file);
	if (returnCode < 0)
		return returnCode;

	pgstat_report_wait_start(wait_event_info);
	written = pg_pwrite_zeros(VfdCache[file].fd, amount, offset);
	pgstat_report_wait_end();

	if (written < 0)
		return -1;
	else if (written != amount)
	{
		/* 如果 errno 未设置，假设问题是磁盘空间不足 */
		if (errno == 0)
			errno = ENOSPC;
		return -1;
	}

	return 0;
}

/*
 * 尝试使用 posix_fallocate() 预留文件空间。
 * 如果操作系统未实现 posix_fallocate() 或失败返回 EINVAL / EOPNOTSUPP，
 * 则改用 FileZero() 替代。
 *
 * 注意，如果文件系统未实现 posix_fallocate()，
 * 至少 glibc 会在用户空间实现它。但并非所有环境都是这样。
 *
 * 成功返回 0，否则返回 -1。在后一种情况下 errno 设置为相应的错误码。
 */
int
FileFallocate(File file, off_t offset, off_t amount, uint32 wait_event_info)
{
#ifdef HAVE_POSIX_FALLOCATE
	int			returnCode;

	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FileFallocate: %d (%s) " INT64_FORMAT " " INT64_FORMAT,
			   file, VfdCache[file].fileName,
			   (int64) offset, (int64) amount));

	returnCode = FileAccess(file);
	if (returnCode < 0)
		return -1;

retry:
	pgstat_report_wait_start(wait_event_info);
	returnCode = posix_fallocate(VfdCache[file].fd, offset, amount);
	pgstat_report_wait_end();

	if (returnCode == 0)
		return 0;
	else if (returnCode == EINTR)
		goto retry;

	/* 为了兼容 %m 打印等 */
	errno = returnCode;

	/*
	 * 在"真实"失败的情况下返回，
	 * 如果不支持 fallocate，则回退到 FileZero() 实现。
	 */
	if (returnCode != EINVAL && returnCode != EOPNOTSUPP)
		return -1;
#endif

	return FileZero(file, offset, amount, wait_event_info);
}

off_t
FileSize(File file)
{
	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FileSize %d (%s)",
			   file, VfdCache[file].fileName));

	if (FileIsNotOpen(file))
	{
		if (FileAccess(file) < 0)
			return (off_t) -1;
	}

	return lseek(VfdCache[file].fd, 0, SEEK_END);
}

int
FileTruncate(File file, off_t offset, uint32 wait_event_info)
{
	int			returnCode;

	Assert(FileIsValid(file));

	DO_DB(elog(LOG, "FileTruncate %d (%s)",
			   file, VfdCache[file].fileName));

	returnCode = FileAccess(file);
	if (returnCode < 0)
		return returnCode;

	pgstat_report_wait_start(wait_event_info);
	returnCode = pg_ftruncate(VfdCache[file].fd, offset);
	pgstat_report_wait_end();

	if (returnCode == 0 && VfdCache[file].fileSize > offset)
	{
		/* 调整临时文件截断后的状态 */
		Assert(VfdCache[file].fdstate & FD_TEMP_FILE_LIMIT);
		temporary_files_size -= VfdCache[file].fileSize - offset;
		VfdCache[file].fileSize = offset;
	}

	return returnCode;
}

/*
 * 返回与打开文件关联的路径名。
 *
 * 返回的字符串指向内部缓冲区，该缓冲区在文件关闭之前有效。
 */
char *
FilePathName(File file)
{
	Assert(FileIsValid(file));

	return VfdCache[file].fileName;
}

/*
 * 返回打开文件的原始文件描述符。
 *
 * 返回的文件描述符在文件关闭之前有效，但有很多事情可能导致文件关闭。
 * 因此调用者应注意在完成返回文件描述符的使用之前不要做太多其他操作。
 */
int
FileGetRawDesc(File file)
{
	int			returnCode;

	returnCode = FileAccess(file);
	if (returnCode < 0)
		return returnCode;

	Assert(FileIsValid(file));
	return VfdCache[file].fd;
}

/*
 * FileGetRawFlags - 返回 open(2) 时的文件标志
 */
int
FileGetRawFlags(File file)
{
	Assert(FileIsValid(file));
	return VfdCache[file].fileFlags;
}

/*
 * FileGetRawMode - 返回传递给 open(2) 的模式位掩码
 */
mode_t
FileGetRawMode(File file)
{
	Assert(FileIsValid(file));
	return VfdCache[file].fileMode;
}

/*
 * 如果需要和可能的话，为 allocatedDescs[] 数组腾出另一个条目空间。
 * 如果有可用的数组元素则返回 true。
 */
static bool
reserveAllocatedDesc(void)
{
	AllocateDesc *newDescs;
	int			newMax;

	/* 如果数组已有空闲槽位，快速返回。 */
	if (numAllocatedDescs < maxAllocatedDescs)
		return true;

	/*
	 * 如果数组在当前进程中尚未创建，则用 FD_MINFREE / 3 个元素进行初始化。
	 * 在许多场景中，这已经是我们需要的最大数量了。
	 * 我们不希望立即查看 max_safe_fds，
	 * 因为 set_max_safe_fds() 可能尚未运行。
	 */
	if (allocatedDescs == NULL)
	{
		newMax = FD_MINFREE / 3;
		newDescs = (AllocateDesc *) malloc(newMax * sizeof(AllocateDesc));
		/* 已经内存不足？视为致命错误。 */
		if (newDescs == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of memory")));
		allocatedDescs = newDescs;
		maxAllocatedDescs = newMax;
		return true;
	}

	/*
	 * 考虑将数组扩展到超出上述初始分配的大小。
	 * 到这一步时，max_safe_fds 应该已被准确获知。
	 *
	 * 我们不能让已分配的描述符占用所有可用的 FD，
	 * 在实践中最好为 VFD 使用保留合理数量的 FD。
	 * 因此将最大值设置为 max_safe_fds / 3。
	 * （这肯定至少与初始大小 FD_MINFREE / 3 一样大，
	 * 所以这里没有加强限制。）
	 * 回想一下，"外部"FD 允许消耗另外三分之一的 max_safe_fds。
	 */
	newMax = max_safe_fds / 3;
	if (newMax > maxAllocatedDescs)
	{
		newDescs = (AllocateDesc *) realloc(allocatedDescs,
											newMax * sizeof(AllocateDesc));
		/* 将内存不足视为非致命错误。 */
		if (newDescs == NULL)
			return false;
		allocatedDescs = newDescs;
		maxAllocatedDescs = newMax;
		return true;
	}

	/* 无法再扩展 allocatedDescs[]。 */
	return false;
}

/*
 * 想要使用 stdio（即 FILE*）的例程应使用 AllocateFile
 * 而不是普通的 fopen()。这让 fd.c 在必要时释放 FD 以打开文件。
 * 完成后，调用 FreeFile 而不是 fclose。
 *
 * 注意，将长时间打开的文件*不*应通过这种方式处理，
 * 因为它们不能与其他文件共享内核文件描述符；
 * 如果有人锁定了太多 FD，会有严重的 FD 耗尽风险。
 * 大多数调用者只是读取一个配置文件，读完后立即关闭。
 *
 * fd.c 会在事务提交或中止时自动关闭所有通过 AllocateFile 打开的文件；
 * 这可以防止当调用 AllocateFile 的例程被 ereport(ERROR) 提前终止时
 * 出现 FD 泄漏。
 *
 * 理想情况下，这应该是后端中*唯一*直接调用 fopen() 的地方。
 */
FILE *
AllocateFile(const char *name, const char *mode)
{
	FILE	   *file;

	DO_DB(elog(LOG, "AllocateFile: Allocated %d (%s)",
			   numAllocatedDescs, name));

	/* 我们还能分配另一个非虚拟 FD 吗？ */
	if (!reserveAllocatedDesc())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("exceeded maxAllocatedDescs (%d) while trying to open file \"%s\"",
						maxAllocatedDescs, name)));

	/* 关闭多余的内核 FD。 */
	ReleaseLruFiles();

TryAgain:
	if ((file = fopen(name, mode)) != NULL)
	{
		AllocateDesc *desc = &allocatedDescs[numAllocatedDescs];

		desc->kind = AllocateDescFile;
		desc->desc.file = file;
		desc->create_subid = GetCurrentSubTransactionId();
		numAllocatedDescs++;
		return desc->desc.file;
	}

	if (errno == EMFILE || errno == ENFILE)
	{
		int			save_errno = errno;

		ereport(LOG,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("out of file descriptors: %m; release and retry")));
		errno = 0;
		if (ReleaseLruFile())
			goto TryAgain;
		errno = save_errno;
	}

	return NULL;
}

/*
 * 使用 OpenTransientFilePerm() 打开文件，并为 fileMode 参数传递默认文件模式。
 */
int
OpenTransientFile(const char *fileName, int fileFlags)
{
	return OpenTransientFilePerm(fileName, fileFlags, pg_file_create_mode);
}

/*
 * 类似 AllocateFile，但返回类似 open(2) 的无缓冲 fd
 */
int
OpenTransientFilePerm(const char *fileName, int fileFlags, mode_t fileMode)
{
	int			fd;

	DO_DB(elog(LOG, "OpenTransientFile: Allocated %d (%s)",
			   numAllocatedDescs, fileName));

	/* 我们还能分配另一个非虚拟 FD 吗？ */
	if (!reserveAllocatedDesc())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("exceeded maxAllocatedDescs (%d) while trying to open file \"%s\"",
						maxAllocatedDescs, fileName)));

	/* 关闭多余的内核 FD。 */
	ReleaseLruFiles();

	fd = BasicOpenFilePerm(fileName, fileFlags, fileMode);

	if (fd >= 0)
	{
		AllocateDesc *desc = &allocatedDescs[numAllocatedDescs];

		desc->kind = AllocateDescRawFD;
		desc->desc.fd = fd;
		desc->create_subid = GetCurrentSubTransactionId();
		numAllocatedDescs++;

		return fd;
	}

	return -1;					/* 失败 */
}

/*
 * 想要启动管道流的例程应使用 OpenPipeStream
 * 而不是普通的 popen()。这让 fd.c 在必要时释放 FD。
 * 完成后，调用 ClosePipeStream 而不是 pclose。
 *
 * 此函数还确保 popen 启动的程序使用默认的 SIGPIPE 处理，
 * 而不是后端通常使用的 SIG_IGN 设置。
 * 这确保了例如提前关闭读管道时的预期响应。
 */
FILE *
OpenPipeStream(const char *command, const char *mode)
{
	FILE	   *file;
	int			save_errno;

	DO_DB(elog(LOG, "OpenPipeStream: Allocated %d (%s)",
			   numAllocatedDescs, command));

	/* 我们还能分配另一个非虚拟 FD 吗？ */
	if (!reserveAllocatedDesc())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("exceeded maxAllocatedDescs (%d) while trying to execute command \"%s\"",
						maxAllocatedDescs, command)));

	/* 关闭多余的内核 FD。 */
	ReleaseLruFiles();

TryAgain:
	fflush(NULL);
	pqsignal(SIGPIPE, SIG_DFL);
	errno = 0;
	file = popen(command, mode);
	save_errno = errno;
	pqsignal(SIGPIPE, SIG_IGN);
	errno = save_errno;
	if (file != NULL)
	{
		AllocateDesc *desc = &allocatedDescs[numAllocatedDescs];

		desc->kind = AllocateDescPipe;
		desc->desc.file = file;
		desc->create_subid = GetCurrentSubTransactionId();
		numAllocatedDescs++;
		return desc->desc.file;
	}

	if (errno == EMFILE || errno == ENFILE)
	{
		ereport(LOG,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("out of file descriptors: %m; release and retry")));
		if (ReleaseLruFile())
			goto TryAgain;
		errno = save_errno;
	}

	return NULL;
}

/*
 * 释放任意类型的 AllocateDesc。
 *
 * 参数*必须*指向 allocatedDescs[] 数组中的一个元素。
 */
static int
FreeDesc(AllocateDesc *desc)
{
	int			result;

	/* 关闭底层对象 */
	switch (desc->kind)
	{
		case AllocateDescFile:
			result = fclose(desc->desc.file);
			break;
		case AllocateDescPipe:
			result = pclose(desc->desc.file);
			break;
		case AllocateDescDir:
			result = closedir(desc->desc.dir);
			break;
		case AllocateDescRawFD:
			pgaio_closing_fd(desc->desc.fd);
			result = close(desc->desc.fd);
			break;
		default:
			elog(ERROR, "AllocateDesc kind not recognized");
			result = 0;			/* 让编译器安静 */
			break;
	}

	/* 压缩 allocatedDescs 数组中的存储 */
	numAllocatedDescs--;
	*desc = allocatedDescs[numAllocatedDescs];

	return result;
}

/*
 * 关闭 AllocateFile 返回的文件。
 *
 * 注意我们不检查 fclose 的返回值 —— 处理关闭错误是调用者的责任。
 */
int
FreeFile(FILE *file)
{
	int			i;

	DO_DB(elog(LOG, "FreeFile: Allocated %d", numAllocatedDescs));

	/* 从已分配文件列表中移除该文件（如果存在） */
	for (i = numAllocatedDescs; --i >= 0;)
	{
		AllocateDesc *desc = &allocatedDescs[i];

		if (desc->kind == AllocateDescFile && desc->desc.file == file)
			return FreeDesc(desc);
	}

	/* 只有传入不在 allocatedDescs 中的文件才会到这里 */
	elog(WARNING, "file passed to FreeFile was not obtained from AllocateFile");

	return fclose(file);
}

/*
 * 关闭 OpenTransientFile 返回的文件。
 *
 * 注意我们不检查 close 的返回值 —— 处理关闭错误是调用者的责任。
 */
int
CloseTransientFile(int fd)
{
	int			i;

	DO_DB(elog(LOG, "CloseTransientFile: Allocated %d", numAllocatedDescs));

	/* 从已分配文件列表中移除该 fd（如果存在） */
	for (i = numAllocatedDescs; --i >= 0;)
	{
		AllocateDesc *desc = &allocatedDescs[i];

		if (desc->kind == AllocateDescRawFD && desc->desc.fd == fd)
			return FreeDesc(desc);
	}

	/* 只有传入不在 allocatedDescs 中的文件才会到这里 */
	elog(WARNING, "fd passed to CloseTransientFile was not obtained from OpenTransientFile");

	pgaio_closing_fd(fd);

	return close(fd);
}

/*
 * 想要使用 <dirent.h>（即 DIR*）的例程应使用 AllocateDir
 * 而不是普通的 opendir()。这让 fd.c 在必要时释放 FD 以打开目录，
 * 并在 elog 后关闭它。完成后，调用 FreeDir 而不是 closedir。
 *
 * 失败时返回 NULL 并设置 errno。
 * 注意，失败检测通常留给后续的 ReadDir 或 ReadDirExtended 调用；
 * 参见 ReadDir 的注释。
 *
 * 理想情况下，这应该是后端中*唯一*直接调用 opendir() 的地方。
 */
DIR *
AllocateDir(const char *dirname)
{
	DIR		   *dir;

	DO_DB(elog(LOG, "AllocateDir: Allocated %d (%s)",
			   numAllocatedDescs, dirname));

	/* 我们还能分配另一个非虚拟 FD 吗？ */
	if (!reserveAllocatedDesc())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("exceeded maxAllocatedDescs (%d) while trying to open directory \"%s\"",
						maxAllocatedDescs, dirname)));

	/* 关闭多余的内核 FD。 */
	ReleaseLruFiles();

TryAgain:
	if ((dir = opendir(dirname)) != NULL)
	{
		AllocateDesc *desc = &allocatedDescs[numAllocatedDescs];

		desc->kind = AllocateDescDir;
		desc->desc.dir = dir;
		desc->create_subid = GetCurrentSubTransactionId();
		numAllocatedDescs++;
		return desc->desc.dir;
	}

	if (errno == EMFILE || errno == ENFILE)
	{
		int			save_errno = errno;

		ereport(LOG,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("out of file descriptors: %m; release and retry")));
		errno = 0;
		if (ReleaseLruFile())
			goto TryAgain;
		errno = save_errno;
	}

	return NULL;
}

/*
 * 读取由 AllocateDir 打开的目录，对任何错误使用 ereport 报告。
 *
 * 这比原始的 readdir() 更易用，因为它处理了一些原本相当繁琐且
 * 容易出错的 errno 操作。此外，如果你对 AllocateDir 失败的通用错误消息感到满意，
 * 可以这样做：
 *
 *		dir = AllocateDir(path);
 *		while ((dirent = ReadDir(dir, path)) != NULL)
 *			process dirent;
 *		FreeDir(dir);
 *
 * 因为 NULL 的 dir 参数被视为 AllocateDir 失败的指示。
 * （如果使用此快捷方式，请确保 errno 在 AllocateDir 和 ReadDir 之间没有改变。）
 *
 * 传递给 AllocateDir 的路径名也必须传递给此例程，但它仅用于错误报告。
 */
struct dirent *
ReadDir(DIR *dir, const char *dirname)
{
	return ReadDirExtended(dir, dirname, ERROR);
}

/*
 * ReadDir 的替代版本，允许调用者为任何错误报告指定 elevel
 * （无论是报告 AllocateDir 的初始失败还是后续的目录读取失败）。
 *
 * 如果 elevel < ERROR，则在任何错误后返回 NULL。
 * 使用正常的编码模式，这会立即跳出循环，就像目录中没有（更多）条目一样。
 */
struct dirent *
ReadDirExtended(DIR *dir, const char *dirname, int elevel)
{
	struct dirent *dent;

	/* 如果调用者没有报告，则为 AllocateDir 失败提供通用消息 */
	if (dir == NULL)
	{
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not open directory \"%s\": %m",
						dirname)));
		return NULL;
	}

	errno = 0;
	if ((dent = readdir(dir)) != NULL)
		return dent;

	if (errno)
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not read directory \"%s\": %m",
						dirname)));
	return NULL;
}

/*
 * 关闭由 AllocateDir 打开的目录。
 *
 * 返回 closedir 的返回值（如果不为 0 则设置 errno）。
 * 注意我们不检查返回值 —— 如果需要，处理关闭错误是调用者的责任。
 *
 * 如果 dir == NULL 则不做任何操作；我们假设目录打开失败已经在
 * 需要时被报告了。
 */
int
FreeDir(DIR *dir)
{
	int			i;

	/* 如果 AllocateDir 失败则无需操作 */
	if (dir == NULL)
		return 0;

	DO_DB(elog(LOG, "FreeDir: Allocated %d", numAllocatedDescs));

	/* 从已分配目录列表中移除该 dir（如果存在） */
	for (i = numAllocatedDescs; --i >= 0;)
	{
		AllocateDesc *desc = &allocatedDescs[i];

		if (desc->kind == AllocateDescDir && desc->desc.dir == dir)
			return FreeDesc(desc);
	}

	/* 只有传入不在 allocatedDescs 中的目录才会到这里 */
	elog(WARNING, "dir passed to FreeDir was not obtained from AllocateDir");

	return closedir(dir);
}


/*
 * 关闭由 OpenPipeStream 返回的管道流。
 */
int
ClosePipeStream(FILE *file)
{
	int			i;

	DO_DB(elog(LOG, "ClosePipeStream: Allocated %d", numAllocatedDescs));

	/* 从已分配文件列表中移除该文件（如果存在） */
	for (i = numAllocatedDescs; --i >= 0;)
	{
		AllocateDesc *desc = &allocatedDescs[i];

		if (desc->kind == AllocateDescPipe && desc->desc.file == file)
			return FreeDesc(desc);
	}

	/* 只有传入不在 allocatedDescs 中的文件才会到这里 */
	elog(WARNING, "file passed to ClosePipeStream was not obtained from OpenPipeStream");

	return pclose(file);
}

/*
 * closeAllVfds
 *
 * 强制所有 VFD 进入物理关闭状态，使正在使用的内核文件描述符数量最少。
 * VFD 的逻辑状态不会改变。
 */
void
closeAllVfds(void)
{
	Index		i;

	if (SizeVfdCache > 0)
	{
		Assert(FileIsNotOpen(0));	/* 确保环未被破坏 */
		for (i = 1; i < SizeVfdCache; i++)
		{
			if (!FileIsNotOpen(i))
				LruDelete(i);
		}
	}
}


/*
 * SetTempTablespaces
 *
 * 定义用于临时文件的表空间 OID 列表（实际上是数组）。
 * 此列表将在事务结束之前使用，除非在此之前再次调用此函数。
 * 调用者有责任确保传入的数组具有足够的生命周期
 * （通常它会在 TopTransactionContext 中分配）。
 *
 * 数组中的某些条目可能为 InvalidOid，表示应使用当前数据库的默认表空间。
 */
void
SetTempTablespaces(Oid *tableSpaces, int numSpaces)
{
	Assert(numSpaces >= 0);
	tempTableSpaces = tableSpaces;
	numTempTableSpaces = numSpaces;

	/*
	 * 在列表中随机选择一个起始点。这是为了最小化后端之间的冲突，
	 * 这些后端很可能共享相同的临时表空间列表。
	 * 注意，如果我们在同一事务中创建多个临时文件，
	 * 我们会循环遍历列表 —— 这确保大型临时排序文件
	 * 能很好地分散到所有可用表空间中。
	 */
	if (numSpaces > 1)
		nextTempTableSpace = pg_prng_uint64_range(&pg_global_prng_state,
												  0, numSpaces - 1);
	else
		nextTempTableSpace = 0;
}

/*
 * TempTablespacesAreSet
 *
 * 如果当前事务中已调用 SetTempTablespaces 则返回 true。
 * （这只是为了让 tablespaces.c 不需要自己的每事务状态。）
 */
bool
TempTablespacesAreSet(void)
{
	return (numTempTableSpaces >= 0);
}

/*
 * GetTempTablespaces
 *
 * 使用应用于临时文件的表空间 OID 填充一个数组。
 * （某些条目可能为 InvalidOid，表示应使用当前数据库的默认表空间。）
 * 最多填充 numSpaces 个条目。
 * 返回复制到输出数组中的 OID 数量。
 */
int
GetTempTablespaces(Oid *tableSpaces, int numSpaces)
{
	int			i;

	Assert(TempTablespacesAreSet());
	for (i = 0; i < numTempTableSpaces && i < numSpaces; ++i)
		tableSpaces[i] = tempTableSpaces[i];

	return i;
}

/*
 * GetNextTempTableSpace
 *
 * 选择下一个要使用的临时表空间。
 * 返回 InvalidOid 表示使用当前数据库的默认表空间。
 */
Oid
GetNextTempTableSpace(void)
{
	if (numTempTableSpaces > 0)
	{
		/* 递增 nextTempTableSpace 计数器（循环回绕） */
		if (++nextTempTableSpace >= numTempTableSpaces)
			nextTempTableSpace = 0;
		return tempTableSpaces[nextTempTableSpace];
	}
	return InvalidOid;
}


/*
 * AtEOSubXact_Files
 *
 * 处理子事务的提交/中止。在中止时，关闭该子事务可能打开的临时文件。
 * 在提交时，将打开的文件重新分配给父子事务。
 */
void
AtEOSubXact_Files(bool isCommit, SubTransactionId mySubid,
				  SubTransactionId parentSubid)
{
	Index		i;

	for (i = 0; i < numAllocatedDescs; i++)
	{
		if (allocatedDescs[i].create_subid == mySubid)
		{
			if (isCommit)
				allocatedDescs[i].create_subid = parentSubid;
			else
			{
				/* FreeDesc 后必须重新检查该项（不雅观） */
				FreeDesc(&allocatedDescs[i--]);
			}
		}
	}
}

/*
 * AtEOXact_Files
 *
 * 此例程在事务提交或中止时调用。
 * 所有仍打开的每事务临时文件 VFD 都会被关闭，
 * 这也会导致底层文件被删除（尽管它们应该已被 ResourceOwner 清理关闭了）。
 * 此外，所有"已分配"的 stdio 文件也会被关闭。
 * 我们还会忘记事务本地的临时表空间列表。
 *
 * isCommit 标志仅用于决定是否发出未关闭文件的警告。
 */
void
AtEOXact_Files(bool isCommit)
{
	CleanupTempFiles(isCommit, false);
	tempTableSpaces = NULL;
	numTempTableSpaces = -1;
}

/*
 * BeforeShmemExit_Files
 *
 * before_shmem_exit 钩子，在后端关闭期间清理临时文件。
 * 这里我们希望清理*所有*临时文件，包括跨事务的临时文件。
 */
static void
BeforeShmemExit_Files(int code, Datum arg)
{
	CleanupTempFiles(false, true);

	/* 防止进一步创建临时文件 */
#ifdef USE_ASSERT_CHECKING
	temporary_files_allowed = false;
#endif
}

/*
 * 关闭临时文件并删除其底层文件。
 *
 * isCommit：如果为 true，则是正常事务提交，我们不期望有任何剩余文件；
 * 如果有则发出警告。
 *
 * isProcExit：如果为 true，则在后端进程退出时调用。
 * 如果是这种情况，我们应该删除所有临时文件；
 * 如果不是，我们被调用以处理事务提交/中止，
 * 应该只删除事务本地的临时文件。
 * 无论哪种情况，也清理"已分配"的 stdio 文件、目录和 fd。
 */
static void
CleanupTempFiles(bool isCommit, bool isProcExit)
{
	Index		i;

	/*
	 * 这里要小心：在 proc_exit 时我们需要额外的清理，不仅仅是 xact_temporary 文件。
	 */
	if (isProcExit || have_xact_temporary_files)
	{
		Assert(FileIsNotOpen(0));	/* 确保环未被破坏 */
		for (i = 1; i < SizeVfdCache; i++)
		{
			unsigned short fdstate = VfdCache[i].fdstate;

			if (((fdstate & FD_DELETE_AT_CLOSE) || (fdstate & FD_CLOSE_AT_EOXACT)) &&
				VfdCache[i].fileName != NULL)
			{
				/*
				 * 如果我们正在退出后端进程，关闭所有临时文件。
				 * 否则，只关闭当前事务本地的临时文件。
				 * 它们应该已被 ResourceOwner 机制关闭，所以这只是调试交叉检查。
				 */
				if (isProcExit)
					FileClose(i);
				else if (fdstate & FD_CLOSE_AT_EOXACT)
				{
					elog(WARNING,
						 "temporary file %s not closed at end-of-transaction",
						 VfdCache[i].fileName);
					FileClose(i);
				}
			}
		}

		have_xact_temporary_files = false;
	}

	/* 如果提交时仍有已分配的文件未关闭，发出警告。 */
	if (isCommit && numAllocatedDescs > 0)
		elog(WARNING, "%d temporary files and directories not closed at end-of-transaction",
			 numAllocatedDescs);

	/* 清理"已分配"的 stdio 文件、目录和 fd。 */
	while (numAllocatedDescs > 0)
		FreeDesc(&allocatedDescs[0]);
}


/*
 * 删除先前 postmaster 会话遗留的临时文件和临时关系文件
 *
 * 这应在 postmaster 启动期间调用。它将强制删除
 * OpenTemporaryFile 创建的任何遗留文件和 mdcreate 创建的
 * 任何遗留临时关系文件。
 *
 * 在后端崩溃后重启周期中，当启用 remove_temp_files_after_crash GUC 时调用此例程。
 * 当查询正在使用临时文件时，多次崩溃可能导致无用的存储消耗，
 * 这些消耗只能通过服务重启来回收。
 * 反对启用它的理由是，有人可能希望检查临时文件以进行调试。
 * 但这意味着 OpenTemporaryFile 最好允许与已存在的临时文件名冲突。
 *
 * 注意：此函数及其子例程通常使用 ereport(LOG) 报告系统调用失败并继续运行。
 * 删除临时文件不那么关键，我们不应在无法做到时无法启动数据库。
 */
void
RemovePgTempFiles(void)
{
	char		temp_path[MAXPGPATH + sizeof(PG_TBLSPC_DIR) + sizeof(TABLESPACE_VERSION_DIRECTORY) + sizeof(PG_TEMP_FILES_DIR)];
	DIR		   *spc_dir;
	struct dirent *spc_de;

	/*
	 * 首先处理 pg_default ($PGDATA/base) 中的临时文件
	 */
	snprintf(temp_path, sizeof(temp_path), "base/%s", PG_TEMP_FILES_DIR);
	RemovePgTempFilesInDir(temp_path, true, false);
	RemovePgTempRelationFiles("base");

	/*
	 * 遍历所有非默认表空间的临时目录。
	 */
	spc_dir = AllocateDir(PG_TBLSPC_DIR);

	while ((spc_de = ReadDirExtended(spc_dir, PG_TBLSPC_DIR, LOG)) != NULL)
	{
		if (strcmp(spc_de->d_name, ".") == 0 ||
			strcmp(spc_de->d_name, "..") == 0)
			continue;

		snprintf(temp_path, sizeof(temp_path), "%s/%s/%s/%s",
				 PG_TBLSPC_DIR, spc_de->d_name, TABLESPACE_VERSION_DIRECTORY,
				 PG_TEMP_FILES_DIR);
		RemovePgTempFilesInDir(temp_path, true, false);

		snprintf(temp_path, sizeof(temp_path), "%s/%s/%s",
				 PG_TBLSPC_DIR, spc_de->d_name, TABLESPACE_VERSION_DIRECTORY);
		RemovePgTempRelationFiles(temp_path);
	}

	FreeDir(spc_dir);

	/*
	 * 在 EXEC_BACKEND 情况下，DataDir 的顶层也有一个 pgsql_tmp 目录。
	 * 但是这里*不*清理它，因为这样做会导致竞争条件。
	 * 它在 postmaster 启动时单独、更早地进行处理。
	 */
}

/*
 * 为 RemovePgTempFiles 处理一个 pgsql_tmp 目录。
 *
 * 如果 missing_ok 为 true，则命名目录不存在也是可以的。
 * 任何其他问题会导致 LOG 消息。
 * （在顶层 missing_ok 应为 true，因为 pgsql_tmp 目录在需要时才会创建。）
 *
 * 在顶层，应以 unlink_all = false 调用，以便只有匹配临时名称前缀的
 * 文件才会被 unlink。递归时将以 unlink_all = true 调用，
 * 以 unlink 顶级临时目录下的所有内容。
 *
 * （这两个标志可以合并为一个，但保持分开似乎更清晰。）
 */
void
RemovePgTempFilesInDir(const char *tmpdirname, bool missing_ok, bool unlink_all)
{
	DIR		   *temp_dir;
	struct dirent *temp_de;
	char		rm_path[MAXPGPATH * 2];

	temp_dir = AllocateDir(tmpdirname);

	if (temp_dir == NULL && errno == ENOENT && missing_ok)
		return;

	while ((temp_de = ReadDirExtended(temp_dir, tmpdirname, LOG)) != NULL)
	{
		if (strcmp(temp_de->d_name, ".") == 0 ||
			strcmp(temp_de->d_name, "..") == 0)
			continue;

		snprintf(rm_path, sizeof(rm_path), "%s/%s",
				 tmpdirname, temp_de->d_name);

		if (unlink_all ||
			strncmp(temp_de->d_name,
					PG_TEMP_FILE_PREFIX,
					strlen(PG_TEMP_FILE_PREFIX)) == 0)
		{
			PGFileType	type = get_dirent_type(rm_path, temp_de, false, LOG);

			if (type == PGFILETYPE_ERROR)
				continue;
			else if (type == PGFILETYPE_DIR)
			{
			/* 递归移除内容，然后删除目录本身 */
			RemovePgTempFilesInDir(rm_path, false, true);

				if (rmdir(rm_path) < 0)
					ereport(LOG,
							(errcode_for_file_access(),
							 errmsg("could not remove directory \"%s\": %m",
									rm_path)));
			}
			else
			{
				if (unlink(rm_path) < 0)
					ereport(LOG,
							(errcode_for_file_access(),
							 errmsg("could not remove file \"%s\": %m",
									rm_path)));
			}
		}
		else
			ereport(LOG,
					(errmsg("unexpected file found in temporary-files directory: \"%s\"",
							rm_path)));
	}

	FreeDir(temp_dir);
}

/* 处理一个表空间目录，查找每个数据库的子目录 */
static void
RemovePgTempRelationFiles(const char *tsdirname)
{
	DIR		   *ts_dir;
	struct dirent *de;
	char		dbspace_path[MAXPGPATH * 2];

	ts_dir = AllocateDir(tsdirname);

	while ((de = ReadDirExtended(ts_dir, tsdirname, LOG)) != NULL)
	{
		/*
		 * 我们只对每个数据库的目录感兴趣，这些目录有数字名称。
		 * 注意，此代码也会（正确地）忽略 "." 和 ".."。
		 */
		if (strspn(de->d_name, "0123456789") != strlen(de->d_name))
			continue;

		snprintf(dbspace_path, sizeof(dbspace_path), "%s/%s",
				 tsdirname, de->d_name);
		RemovePgTempRelationFilesInDbspace(dbspace_path);
	}

	FreeDir(ts_dir);
}

/* 处理 RemovePgTempRelationFiles 的一个每数据库空间目录 */
static void
RemovePgTempRelationFilesInDbspace(const char *dbspacedirname)
{
	DIR		   *dbspace_dir;
	struct dirent *de;
	char		rm_path[MAXPGPATH * 2];

	dbspace_dir = AllocateDir(dbspacedirname);

	while ((de = ReadDirExtended(dbspace_dir, dbspacedirname, LOG)) != NULL)
	{
		if (!looks_like_temp_rel_name(de->d_name))
			continue;

		snprintf(rm_path, sizeof(rm_path), "%s/%s",
				 dbspacedirname, de->d_name);

		if (unlink(rm_path) < 0)
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not remove file \"%s\": %m",
							rm_path)));
	}

	FreeDir(dbspace_dir);
}

/* 格式为 t<数字>_<数字>，或 t<数字>_<数字>_<fork名称> */
bool
looks_like_temp_rel_name(const char *name)
{
	int			pos;
	int			savepos;

	/* 必须以 "t" 开头。 */
	if (name[0] != 't')
		return false;

	/* 后跟非空数字字符串，然后是下划线。 */
	for (pos = 1; isdigit((unsigned char) name[pos]); ++pos)
		;
	if (pos == 1 || name[pos] != '_')
		return false;

	/* 后跟另一个非空数字字符串。 */
	for (savepos = ++pos; isdigit((unsigned char) name[pos]); ++pos)
		;
	if (savepos == pos)
		return false;

	/* 可能有 _forkname 或 .segment 或两者都有。 */
	if (name[pos] == '_')
	{
		int			forkchar = forkname_chars(&name[pos + 1], NULL);

		if (forkchar <= 0)
			return false;
		pos += forkchar + 1;
	}
	if (name[pos] == '.')
	{
		int			segchar;

		for (segchar = 1; isdigit((unsigned char) name[pos + segchar]); ++segchar)
			;
		if (segchar <= 1)
			return false;
		pos += segchar;
	}

	/* 现在我们应该在末尾了。 */
	if (name[pos] != '\0')
		return false;
	return true;
}

#ifdef HAVE_SYNCFS
static void
do_syncfs(const char *path)
{
	int			fd;

	ereport_startup_progress("syncing data directory (syncfs), elapsed time: %ld.%02d s, current path: %s",
							 path);

	fd = OpenTransientFile(path, O_RDONLY);
	if (fd < 0)
	{
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));
		return;
	}
	if (syncfs(fd) < 0)
		ereport(LOG,
				(errcode_for_file_access(),
				 errmsg("could not synchronize file system for file \"%s\": %m", path)));
	CloseTransientFile(fd);
}
#endif

/*
 * 根据 recovery_init_sync_method 设置，递归地对 PGDATA 及其所有内容执行
 * fsync，或者对所有潜在文件系统执行 syncfs。
 *
 * 我们对所有常规文件和目录执行 fsync，但仅对 pg_wal 和
 * pg_tblspc 直属目录下的符号链接进行跟踪。
 * 其他符号链接假定指向我们无需负责 fsync 的文件，甚至可能没有写入权限。
 *
 * 错误会被记录但不视为致命错误；因为该函数仅用于数据库启动期间，
 * 以应对可能存在已发出但未同步的写入挂起在数据目录上的情况。
 * 我们希望确保此类写入在新运行中所做的任何操作之前到达磁盘。
 * 然而，对错误进行中止会导致无害情况（例如数据目录中的只读文件）无法启动，
 * 这也不好。
 *
 * 注意，如果我们之前由于 fsync() 上的 PANIC 导致崩溃，
 * 我们将在恢复期间重新写入所有更改。
 *
 * 注意，我们假设一开始就已经 chdir 到 PGDATA。
 */
void
SyncDataDirectory(void)
{
	bool		xlog_is_symlink;

	/* 如果禁用了 fsync，我们可以跳过整个过程。 */
	if (!enableFsync)
		return;

	/*
	 * 如果 pg_wal 是一个符号链接，我们需要单独递归进入它，
	 * 因为下面的第一个 walkdir 会忽略它。
	 */
	xlog_is_symlink = false;

	{
		struct stat st;

		if (lstat("pg_wal", &st) < 0)
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("could not stat file \"%s\": %m",
							"pg_wal")));
		else if (S_ISLNK(st.st_mode))
			xlog_is_symlink = true;
	}

#ifdef HAVE_SYNCFS
	if (recovery_init_sync_method == DATA_DIR_SYNC_METHOD_SYNCFS)
	{
		DIR		   *dir;
		struct dirent *de;

		/*
		 * 在 Linux 上，我们不必逐个打开每个文件。
		 * 可以使用 syncfs() 来同步整个文件系统。
		 * 我们只期望在容忍符号链接的地方存在文件系统边界，
		 * 即 pg_wal 和表空间，因此我们为每个这些目录调用 syncfs()。
		 */

		/* 准备报告通过 syncfs 同步数据目录的进度。 */
		begin_startup_progress_phase();

		/* 同步顶级 pgdata 目录。 */
		do_syncfs(".");
		/* 如果配置了任何表空间，同步每个表空间。 */
		dir = AllocateDir(PG_TBLSPC_DIR);
		while ((de = ReadDirExtended(dir, PG_TBLSPC_DIR, LOG)))
		{
			char		path[MAXPGPATH];

			if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
				continue;

			snprintf(path, MAXPGPATH, "%s/%s", PG_TBLSPC_DIR, de->d_name);
			do_syncfs(path);
		}
		FreeDir(dir);
		/* 如果 pg_wal 是符号链接，也处理它。 */
		if (xlog_is_symlink)
			do_syncfs("pg_wal");
		return;
	}
#endif							/* !HAVE_SYNCFS */

#ifdef PG_FLUSH_DATA_WORKS
	/* 准备报告预 fsync 阶段的进度。 */
	begin_startup_progress_phase();

	/*
	 * 如果可能，向内核提示我们即将 fsync 数据目录及其内容。
	 * 此步骤中的错误比普通的更无关紧要，因此仅在 DEBUG1 级别记录。
	 */
	walkdir(".", pre_sync_fname, false, DEBUG1);
	if (xlog_is_symlink)
		walkdir("pg_wal", pre_sync_fname, false, DEBUG1);
	walkdir(PG_TBLSPC_DIR, pre_sync_fname, true, DEBUG1);
#endif

	/* 准备报告通过 fsync 同步数据目录的进度。 */
	begin_startup_progress_phase();

	/*
	 * 现在我们以相同的顺序执行 fsync()。
	 *
	 * 主调用忽略符号链接，因此除了特别处理作为符号链接的 pg_wal 外，
	 * pg_tblspc 必须单独以 process_symlinks = true 进行访问。
	 * 注意，如果 pg_tblspc 中有任何普通目录，它们会被 fsync 两次。
	 * 这不是预期情况，所以我们不担心优化它。
	 */
	walkdir(".", datadir_fsync_fname, false, LOG);
	if (xlog_is_symlink)
		walkdir("pg_wal", datadir_fsync_fname, false, LOG);
	walkdir(PG_TBLSPC_DIR, datadir_fsync_fname, true, LOG);
}

/*
 * walkdir：递归遍历一个目录，对每个常规文件和目录
 * （包括命名目录本身）执行指定操作。
 *
 * 如果 process_symlinks 为 true，则操作和递归也会应用于
 * 给定目录中符号链接所指向的常规文件和目录；
 * 否则忽略符号链接。符号链接在子目录中始终被忽略，
 * 即我们有意不将 process_symlinks 标志传递给递归调用。
 *
 * 错误以级别 elevel 报告，elevel 可能是 ERROR 或更低。
 *
 * 另见 file_utils.c 中的 walkdir，这是此逻辑的前端版本。
 */
static void
walkdir(const char *path,
		void (*action) (const char *fname, bool isdir, int elevel),
		bool process_symlinks,
		int elevel)
{
	DIR		   *dir;
	struct dirent *de;

	dir = AllocateDir(path);

	while ((de = ReadDirExtended(dir, path, elevel)) != NULL)
	{
		char		subpath[MAXPGPATH * 2];

		CHECK_FOR_INTERRUPTS();

		if (strcmp(de->d_name, ".") == 0 ||
			strcmp(de->d_name, "..") == 0)
			continue;

		snprintf(subpath, sizeof(subpath), "%s/%s", path, de->d_name);

		switch (get_dirent_type(subpath, de, process_symlinks, elevel))
		{
			case PGFILETYPE_REG:
				(*action) (subpath, false, elevel);
				break;
			case PGFILETYPE_DIR:
				walkdir(subpath, action, false, elevel);
				break;
			default:

				/*
				 * 错误已由 get_dirent_type() 直接报告，
				 * 其余符号链接和未知文件类型被忽略。
				 */
				break;
		}
	}

	FreeDir(dir);				/* 我们在此忽略任何错误 */

	/*
	 * 对目标目录本身执行 fsync 很重要，
	 * 因为单独文件的 fsync 不能保证该文件的目录条目已被同步。
	 * 但是，如果 AllocateDir 失败则跳过此步骤；
	 * action 函数可能无法应对此情况。
	 */
	if (dir)
		(*action) (path, true, elevel);
}


/*
 * 向操作系统提示它应该准备好对此文件进行 fsync()。
 *
 * 忽略尝试打开不可读文件时的错误，并以调用者指定的级别记录其他错误。
 */
#ifdef PG_FLUSH_DATA_WORKS

static void
pre_sync_fname(const char *fname, bool isdir, int elevel)
{
	int			fd;

	/* 不要尝试刷新目录，大概率会失败 */
	if (isdir)
		return;

	ereport_startup_progress("syncing data directory (pre-fsync), elapsed time: %ld.%02d s, current path: %s",
							 fname);

	fd = OpenTransientFile(fname, O_RDONLY | PG_BINARY);

	if (fd < 0)
	{
		if (errno == EACCES)
			return;
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", fname)));
		return;
	}

	/*
	 * pg_flush_data() 忽略错误，这是可以的，因为这只是一种提示。
	 */
	pg_flush_data(fd, 0, 0);

	if (CloseTransientFile(fd) != 0)
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", fname)));
}

#endif							/* PG_FLUSH_DATA_WORKS */

static void
datadir_fsync_fname(const char *fname, bool isdir, int elevel)
{
	ereport_startup_progress("syncing data directory (fsync), elapsed time: %ld.%02d s, current path: %s",
							 fname);

	/*
	 * 我们希望静默忽略关于不可读文件的错误。
	 * 将此意愿传递给 fsync_fname_ext()。
	 */
	fsync_fname_ext(fname, isdir, true, elevel);
}

static void
unlink_if_exists_fname(const char *fname, bool isdir, int elevel)
{
	if (isdir)
	{
		if (rmdir(fname) != 0 && errno != ENOENT)
			ereport(elevel,
					(errcode_for_file_access(),
					 errmsg("could not remove directory \"%s\": %m", fname)));
	}
	else
	{
		/* 使用 PathNameDeleteTemporaryFile 来报告文件大小 */
		PathNameDeleteTemporaryFile(fname, false);
	}
}

/*
 * fsync_fname_ext -- 尝试对文件或目录执行 fsync
 *
 * 如果 ignore_perm 为 true，则忽略尝试打开不可读文件时的错误。
 * 以调用者指定的级别记录其他错误。
 *
 * 成功返回 0，否则返回 -1。
 */
int
fsync_fname_ext(const char *fname, bool isdir, bool ignore_perm, int elevel)
{
	int			fd;
	int			flags;
	int			returncode;

	/*
	 * 某些操作系统要求目录以只读方式打开，
	 * 而其他系统不允许我们对只读打开的文件执行 fsync；
	 * 所以这里需要两种情况。
	 * 使用 O_RDWR 将导致我们无法 fsync 那些对当前用户不可写的文件，
	 * 但我们假设这是可以接受的。
	 */
	flags = PG_BINARY;
	if (!isdir)
		flags |= O_RDWR;
	else
		flags |= O_RDONLY;

	fd = OpenTransientFile(fname, flags);

	/*
	 * 某些操作系统根本不允许我们打开目录（Windows 返回 EACCES），
	 * 在这种情况下直接忽略错误即可。
	 * 如果期望，也静默忽略关于不可读文件的错误。记录其他错误。
	 */
	if (fd < 0 && isdir && (errno == EISDIR || errno == EACCES))
		return 0;
	else if (fd < 0 && ignore_perm && errno == EACCES)
		return 0;
	else if (fd < 0)
	{
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", fname)));
		return -1;
	}

	returncode = pg_fsync(fd);

	/*
	 * 某些操作系统根本不允许我们对目录执行 fsync，
	 * 因此可以忽略这些错误。其他任何错误都需要记录。
	 */
	if (returncode != 0 && !(isdir && (errno == EBADF || errno == EINVAL)))
	{
		int			save_errno;

		/* 出错时关闭文件，可能不在事务上下文中 */
		save_errno = errno;
		(void) CloseTransientFile(fd);
		errno = save_errno;

		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not fsync file \"%s\": %m", fname)));
		return -1;
	}

	if (CloseTransientFile(fd) != 0)
	{
		ereport(elevel,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", fname)));
		return -1;
	}

	return 0;
}

/*
 * fsync_parent_path -- 对文件或目录的父路径执行 fsync
 *
 * 这旨在使文件操作在操作系统崩溃或电源故障时在磁盘上持久化。
 */
static int
fsync_parent_path(const char *fname, int elevel)
{
	char		parentpath[MAXPGPATH];

	strlcpy(parentpath, fname, MAXPGPATH);
	get_parent_directory(parentpath);

	/*
	 * 如果输入参数只是文件名（参见 path.c 中的注释），
	 * get_parent_directory() 返回空字符串，因此将其视为当前目录。
	 */
	if (strlen(parentpath) == 0)
		strlcpy(parentpath, ".", MAXPGPATH);

	if (fsync_fname_ext(parentpath, true, false, elevel) != 0)
		return -1;

	return 0;
}

/*
 * 创建一个 PostgreSQL 数据子目录
 *
 * 数据目录本身及其大多数子目录在 initdb 时创建，
 * 但我们确实有些场合在后端中创建目录（例如 CREATE TABLESPACE）。
 * 在这些情况下，我们希望确保这些目录以一致的方式创建。
 * 目前，这意味着确保创建的目录具有正确的权限，pg_dir_create_mode 为我们跟踪这一点。
 *
 * 注意，我们还根据我们对正确权限的理解来设置 umask()（参见 file_perm.c）。
 *
 * 对于默认权限之外的情况，可以直接使用 mkdir()，
 * 但请务必仔细考虑这种情况 ——
 * PostgreSQL 数据目录中权限不正确的子目录可能导致备份和其他进程失败。
 */
int
MakePGDirectory(const char *directoryName)
{
	return mkdir(directoryName, pg_dir_create_mode);
}

/*
 * 返回传入的错误级别，如果 data_sync_retry 关闭则返回 PANIC。
 *
 * fsync 任何数据文件失败都是立即 panic 的原因，除非启用了 data_sync_retry。
 * 数据可能已被写入操作系统并从我们的缓冲池中移除，
 * 如果我们在一个在写回失败时会遗忘脏数据的操作系统上运行，
 * 可能只剩下一份数据副本：在 WAL 中。
 * 后续再次尝试 fsync 可能会错误地报告成功。
 * 因此我们绝不能允许尝试任何进一步的检查点。
 * 理论上，data_sync_retry 可以在已知不会在写回失败时丢弃脏缓冲数据的系统上启用
 * （其可能结果是检查点将继续失败，直到底层问题被修复）。
 *
 * 任何报告 fsync() 或相关函数失败的代码都应使用此函数过滤错误级别。
 */
int
data_sync_elevel(int elevel)
{
	return data_sync_retry ? elevel : PANIC;
}

bool
check_debug_io_direct(char **newval, void **extra, GucSource source)
{
	bool		result = true;
	int			flags;

#if PG_O_DIRECT == 0
	if (strcmp(*newval, "") != 0)
	{
		GUC_check_errdetail("\"%s\" is not supported on this platform.",
							"debug_io_direct");
		result = false;
	}
	flags = 0;
#else
	List	   *elemlist;
	ListCell   *l;
	char	   *rawstring;

	/* Need a modifiable copy of string */
	rawstring = pstrdup(*newval);

	if (!SplitGUCList(rawstring, ',', &elemlist))
	{
		GUC_check_errdetail("Invalid list syntax in parameter \"%s\".",
							"debug_io_direct");
		pfree(rawstring);
		list_free(elemlist);
		return false;
	}

	flags = 0;
	foreach(l, elemlist)
	{
		char	   *item = (char *) lfirst(l);

		if (pg_strcasecmp(item, "data") == 0)
			flags |= IO_DIRECT_DATA;
		else if (pg_strcasecmp(item, "wal") == 0)
			flags |= IO_DIRECT_WAL;
		else if (pg_strcasecmp(item, "wal_init") == 0)
			flags |= IO_DIRECT_WAL_INIT;
		else
		{
			GUC_check_errdetail("Invalid option \"%s\".", item);
			result = false;
			break;
		}
	}

	/*
	 * It's possible to configure block sizes smaller than our assumed I/O
	 * alignment size, which could result in invalid I/O requests.
	 */
#if XLOG_BLCKSZ < PG_IO_ALIGN_SIZE
	if (result && (flags & (IO_DIRECT_WAL | IO_DIRECT_WAL_INIT)))
	{
		GUC_check_errdetail("\"%s\" is not supported for WAL because %s is too small.",
							"debug_io_direct", "XLOG_BLCKSZ");
		result = false;
	}
#endif
#if BLCKSZ < PG_IO_ALIGN_SIZE
	if (result && (flags & IO_DIRECT_DATA))
	{
		GUC_check_errdetail("\"%s\" is not supported for data because %s is too small.",
							"debug_io_direct", "BLCKSZ");
		result = false;
	}
#endif

	pfree(rawstring);
	list_free(elemlist);
#endif

	if (!result)
		return result;

	/* Save the flags in *extra, for use by assign_debug_io_direct */
	*extra = guc_malloc(LOG, sizeof(int));
	if (!*extra)
		return false;
	*((int *) *extra) = flags;

	return result;
}

void
assign_debug_io_direct(const char *newval, void *extra)
{
	int		   *flags = (int *) extra;

	io_direct_flags = *flags;
}

/* ResourceOwner callbacks */

static void
ResOwnerReleaseFile(Datum res)
{
	File		file = (File) DatumGetInt32(res);
	Vfd		   *vfdP;

	Assert(FileIsValid(file));

	vfdP = &VfdCache[file];
	vfdP->resowner = NULL;

	FileClose(file);
}

static char *
ResOwnerPrintFile(Datum res)
{
	return psprintf("File %d", DatumGetInt32(res));
}
