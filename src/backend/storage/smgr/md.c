/*-------------------------------------------------------------------------
 *
 * md.c
 *	  本代码管理存放在磁盘上的关系。
 *
 * 至少，当伯克利（Berkeley）的那群人给这个文件起名时，他们心里想的是
 * 磁盘。但实际上，这段代码提供的是从 smgr API 到类 Unix 文件系统 API 的
 * 接口，因此只要操作系统提供文件系统支持，它就能与任何类型的设备配合
 * 工作。这些比特位究竟是存放在旋转的磁盘上还是其他某种存储技术上，并不
 * 重要。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/smgr/md.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>

#include "access/xlogutils.h"
#include "commands/tablespace.h"
#include "common/file_utils.h"
#include "miscadmin.h"
#include "pg_trace.h"
#include "pgstat.h"
#include "storage/aio.h"
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "storage/md.h"
#include "storage/relfilelocator.h"
#include "storage/smgr.h"
#include "storage/sync.h"
#include "utils/memutils.h"

/*
 * 磁盘存储管理器在自己的描述符池中跟踪已打开的文件描述符。这样做是为了
 * 更容易支持比操作系统文件大小限制（通常是 2GB）更大的关系。为此，我们
 * 把关系拆分成若干“段”文件，每一段都比操作系统文件大小限制更短。段大小
 * 由 pg_config.h 中的 RELSEG_SIZE 配置常量设定。
 *
 * 在磁盘上，一个关系必须由连续编号的段文件组成，模式如下：
 *	-- 零个或多个恰好为 RELSEG_SIZE 个块的满段
 *	-- 恰好一个大小在 0 <= size < RELSEG_SIZE 个块之间的部分段
 *	-- 可选地，任意数量的、大小为 0 个块的非活动段。
 * 满段和部分段合起来称为“活动”段。非活动段是那些曾经包含数据、但因
 * mdtruncate() 操作而当前不再需要的段。之所以将它们保留为大小为零而不是
 * 取消链接（unlink），是因为其他后端和/或检查点进程可能正持有这些段的
 * 已打开文件引用。如果关系在 mdtruncate() 之后再次扩展，使得某个被停用的
 * 段重新变为活动，那么这些文件引用仍然有效就很重要——否则数据可能会被写
 * 到一份已被取消链接、最终会消失的旧段文件副本上。
 *
 * 文件描述符存放在 SMgrRelation 内部每个 fork 的 md_seg_fds 数组中。这些
 * 数组的长度存放在 md_num_open_segs 中。注意，一个 fork 的 md_num_open_segs
 * 取某个特定值，并不一定意味着该关系没有额外的段；我们可能只是还没有
 * 打开下一个段而已。（无论如何我们都无法把“所有段都在数组中”作为一个
 * 不变量，因为另一个后端可能在我们不注意时扩展该关系。）不过，我们不会
 * 为非活动段保留条目；一旦发现一个部分段，我们就假定其后的任何段都是
 * 非活动的。
 *
 * 整个 MdfdVec 数组在 MdCxt 内存上下文中通过 palloc 分配。
 */

typedef struct _MdfdVec
{
	File		mdfd_vfd;		/* fd.c 池中的 fd 编号 */
	BlockNumber mdfd_segno;		/* 段编号，从 0 开始 */
} MdfdVec;

static MemoryContext MdCxt;		/* 所有 MdfdVec 对象所在的内存上下文 */


/* 填充一个描述 md.c 段文件的文件标签。 */
#define INIT_MD_FILETAG(a,xx_rlocator,xx_forknum,xx_segno) \
( \
	memset(&(a), 0, sizeof(FileTag)), \
	(a).handler = SYNC_HANDLER_MD, \
	(a).rlocator = (xx_rlocator), \
	(a).forknum = (xx_forknum), \
	(a).segno = (xx_segno) \
)


/*** mdopen 与 _mdfd_getseg 的行为标志 ***/
/* 若段不存在则 ereport */
#define EXTENSION_FAIL				(1 << 0)
/* 若段不存在则返回 NULL */
#define EXTENSION_RETURN_NULL		(1 << 1)
/* 按需创建新段 */
#define EXTENSION_CREATE			(1 << 2)
/* 在恢复期间按需创建新段 */
#define EXTENSION_CREATE_RECOVERY	(1 << 3)
/* 若尚未打开，则不要尝试打开段 */
#define EXTENSION_DONT_OPEN			(1 << 5)


/*
 * 用于表示该由 md.c 构建的文件的路径的固定长度字符串。
 *
 * 段的最大数量为 MaxBlockNumber / RELSEG_SIZE，其中 RELSEG_SIZE 可以设为
 * 1（仅用于测试）。
 */
#define SEGMENT_CHARS	OIDCHARS
#define MD_PATH_STR_MAXLEN \
	(\
		REL_PATH_STR_MAXLEN \
		+ sizeof((char)'.') \
		+ SEGMENT_CHARS \
	)
typedef struct MdPathStr
{
	char		str[MD_PATH_STR_MAXLEN + 1];
} MdPathStr;


/* 本地例程 */
static void mdunlinkfork(RelFileLocatorBackend rlocator, ForkNumber forknum,
						 bool isRedo);
static MdfdVec *mdopenfork(SMgrRelation reln, ForkNumber forknum, int behavior);
static void register_dirty_segment(SMgrRelation reln, ForkNumber forknum,
								   MdfdVec *seg);
static void register_unlink_segment(RelFileLocatorBackend rlocator, ForkNumber forknum,
									BlockNumber segno);
static void register_forget_request(RelFileLocatorBackend rlocator, ForkNumber forknum,
									BlockNumber segno);
static void _fdvec_resize(SMgrRelation reln,
						  ForkNumber forknum,
						  int nseg);
static MdPathStr _mdfd_segpath(SMgrRelation reln, ForkNumber forknum,
							   BlockNumber segno);
static MdfdVec *_mdfd_openseg(SMgrRelation reln, ForkNumber forknum,
							  BlockNumber segno, int oflags);
static MdfdVec *_mdfd_getseg(SMgrRelation reln, ForkNumber forknum,
							 BlockNumber blkno, bool skipFsync, int behavior);
static BlockNumber _mdnblocks(SMgrRelation reln, ForkNumber forknum,
							  MdfdVec *seg);

static PgAioResult md_readv_complete(PgAioHandle *ioh, PgAioResult prior_result, uint8 cb_data);
static void md_readv_report(PgAioResult result, const PgAioTargetData *td, int elevel);

const PgAioHandleCallbacks aio_md_readv_cb = {
	.complete_shared = md_readv_complete,
	.report = md_readv_report,
};


static inline int
_mdfd_open_flags(void)
{
	int			flags = O_RDWR | PG_BINARY;

	if (io_direct_flags & IO_DIRECT_DATA)
		flags |= PG_O_DIRECT;

	return flags;
}

/*
 * mdinit() -- 初始化磁盘存储管理器的私有状态。
 */
void
mdinit(void)
{
	MdCxt = AllocSetContextCreate(TopMemoryContext,
								  "MdSmgr",
								  ALLOCSET_DEFAULT_SIZES);
}

/*
 * mdexists() -- 物理文件是否存在？
 *
 * 注意：对于带有待删除标记的残留文件，这也会返回 true
 */
bool
mdexists(SMgrRelation reln, ForkNumber forknum)
{
	/*
	 * 先关闭它，以确保我们能察觉到该 fork 自打开以来是否已被取消链接。
	 * 作为一种优化，在恢复期间可以跳过这一步，因为恢复时丢弃关系时已经
	 * 关闭了它们。
	 */
	if (!InRecovery)
		mdclose(reln, forknum);

	return (mdopenfork(reln, forknum, EXTENSION_RETURN_NULL) != NULL);
}

/*
 * mdcreate() -- 在磁盘上创建一个新关系。
 *
 * 如果 isRedo 为 true，则关系已经存在也是可以接受的。
 */
void
mdcreate(SMgrRelation reln, ForkNumber forknum, bool isRedo)
{
	MdfdVec    *mdfd;
	RelPathStr	path;
	File		fd;

	if (isRedo && reln->md_num_open_segs[forknum] > 0)
		return;					/* 已创建并打开…… */

	Assert(reln->md_num_open_segs[forknum] == 0);

	/*
	 * 这可能是我们在这个数据库中首次使用目标表空间，因此如有必要，创建
	 * 一个按数据库划分的子目录。
	 *
	 * XXX 这相当严重地违反了模块分层，但这似乎是最佳的检查位置。也许
	 * TablespaceCreateDbspace 本应放在这里，而不是放在
	 * commands/tablespace.c 中？但那样就意味着要引入很多 smgr.c 本不该
	 * 了解的东西。
	 */
	TablespaceCreateDbspace(reln->smgr_rlocator.locator.spcOid,
							reln->smgr_rlocator.locator.dbOid,
							isRedo);

	path = relpath(reln->smgr_rlocator, forknum);

	fd = PathNameOpenFile(path.str, _mdfd_open_flags() | O_CREAT | O_EXCL);

	if (fd < 0)
	{
		int			save_errno = errno;

		if (isRedo)
			fd = PathNameOpenFile(path.str, _mdfd_open_flags());
		if (fd < 0)
		{
			/* 务必报告 create 而非 open 所报告的那个错误 */
			errno = save_errno;
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not create file \"%s\": %m", path.str)));
		}
	}

	_fdvec_resize(reln, forknum, 1);
	mdfd = &reln->md_seg_fds[forknum][0];
	mdfd->mdfd_vfd = fd;
	mdfd->mdfd_segno = 0;

	if (!SmgrIsTemp(reln))
		register_dirty_segment(reln, forknum, mdfd);
}

/*
 * mdunlink() -- 取消链接（unlink）一个关系。
 *
 * 注意，传给本函数的是一个 RelFileLocatorBackend —— 等到本函数被调用时，
 * 已经不再有 SMgrRelation 哈希表条目了。
 *
 * forknum 可以是一个用于删除特定 fork 的 fork 编号，也可以是
 * InvalidForkNumber，表示删除所有 fork。
 *
 * 对于普通关系，我们不会取消链接关系的第一个段文件，而只是把它截断为
 * 零长度，并记录一个请求，以便在下一次检查点之后取消链接它。不过，额外的
 * 段可以立即被取消链接。保留这个空文件可以防止该 relfilenumber 被重用。
 * 我们要防范的场景是：
 * 1. 我们删除一个关系（并提交，且实际移除了它的文件）。
 * 2. 我们创建一个新关系，它恰好获得了与被删除关系相同的 relfilenumber
 *	  （要让这种情况发生，OID 必然已经绕回）。
 * 3. 在下一次检查点发生之前我们崩溃了。
 * 在重放过程中，我们会先删除该文件再重新创建它，如果文件内容已由后续的
 * WAL 条目重新填充，那没有问题。但如果我们没有把插入写入 WAL，而是依赖
 * 在填充文件后对其 fsync（就像 wal_level=minimal 时那样），那么文件的内容
 * 将永远丢失。通过把空文件保留到下一次检查点之后，我们可以防止 relfilenumber
 * 在不安全时被重新分配，因为 relfilenumber 的分配会跳过任何已存在的文件。
 *
 * 额外的段（如果有的话）会被截断，然后取消链接。之所以要截断，是因为其他
 * 后端可能仍在 smgr 层持有这些段的已打开 FD，以至于内核暂时还无法移除该
 * 文件。尽管如此，我们仍希望立即回收磁盘空间。
 *
 * 不过，对于临时关系我们不需要这样大费周章，因为我们从不为临时关系写 WAL
 * 条目，因此临时关系对那些接管了其 relfilenumber 的普通关系的健康不会
 * 构成威胁。临时关系与普通关系具有不同的文件命名模式，这提供了额外的安全
 * 保障。其他后端也不应该持有它们的已打开 FD。
 *
 * 在执行二进制升级时我们也不这样做。那种情况下不存在重用风险，因为无论是
 * 崩溃还是仅仅一个普通的 ERROR，升级都会失败，整个集群都必须从头重建。
 * 此外，立即从磁盘上移除这些文件很重要，因为我们可能即将复用相同的
 * relfilenumber。
 *
 * 以上所有内容仅适用于关系的主 fork；其他 fork 可以直接立即移除，因为它们
 * 并不是防止 relfilenumber 被回收所必需的。而且，我们并不仔细跟踪其他 fork
 * 是否已被创建，而只是尝试无条件地取消链接它们；因此我们永远不应该对
 * ENOENT 报错。
 *
 * 如果 isRedo 为 true，那么关系已经消失就不足为奇了。此外，我们应该立即
 * 移除文件，而不是把请求排队留待以后，因为在重做期间不可能创建一个
 * 冲突的关系。
 *
 * 注意：目前我们根本从不就 ENOENT 发出警告。我们本可以在主 fork、非 isRedo
 * 的情况下发出警告，但似乎不值得为此费事。
 *
 * 注意：任何失败都应作为 WARNING 而非 ERROR 报告，因为本函数被调用时我们
 * 通常已经不处于事务中了。
 */
void
mdunlink(RelFileLocatorBackend rlocator, ForkNumber forknum, bool isRedo)
{
	/* 现在执行每个 fork 的工作 */
	if (forknum == InvalidForkNumber)
	{
		for (forknum = 0; forknum <= MAX_FORKNUM; forknum++)
			mdunlinkfork(rlocator, forknum, isRedo);
	}
	else
		mdunlinkfork(rlocator, forknum, isRedo);
}

/*
 * 截断一个文件以释放磁盘空间。
 */
static int
do_truncate(const char *path)
{
	int			save_errno;
	int			ret;

	ret = pg_truncate(path, 0);

	/* 在这里记录警告，以避免在调用方中重复。 */
	if (ret < 0 && errno != ENOENT)
	{
		save_errno = errno;
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("could not truncate file \"%s\": %m", path)));
		errno = save_errno;
	}

	return ret;
}

static void
mdunlinkfork(RelFileLocatorBackend rlocator, ForkNumber forknum, bool isRedo)
{
	RelPathStr	path;
	int			ret;
	int			save_errno;

	path = relpath(rlocator, forknum);

	/*
	 * 截断然后取消链接第一个段，或者只是注册一个请求以便稍后取消链接它，
	 * 详见 mdunlink() 的注释说明。
	 */
	if (isRedo || IsBinaryUpgrade || forknum != MAIN_FORKNUM ||
		RelFileLocatorBackendIsTemp(rlocator))
	{
		if (!RelFileLocatorBackendIsTemp(rlocator))
		{
			/* 阻止其他后端的 fd 占用磁盘空间 */
			ret = do_truncate(path.str);

			/* 忘记针对第一个段的任何待处理同步请求 */
			save_errno = errno;
			register_forget_request(rlocator, forknum, 0 /* 第一个段 */ );
			errno = save_errno;
		}
		else
			ret = 0;

		/* 接下来取消链接该文件，除非它已被发现缺失 */
		if (ret >= 0 || errno != ENOENT)
		{
			ret = unlink(path.str);
			if (ret < 0 && errno != ENOENT)
			{
				save_errno = errno;
				ereport(WARNING,
						(errcode_for_file_access(),
						 errmsg("could not remove file \"%s\": %m", path.str)));
				errno = save_errno;
			}
		}
	}
	else
	{
		/* 阻止其他后端的 fd 占用磁盘空间 */
		ret = do_truncate(path.str);

		/* 注册稍后取消链接第一个段的请求 */
		save_errno = errno;
		register_unlink_segment(rlocator, forknum, 0 /* 第一个段 */ );
		errno = save_errno;
	}

	/*
	 * 删除任何额外的段。
	 *
	 * 注意，因为我们循环直到遇到 ENOENT，所以无论是活动段还是非活动段，
	 * 我们都能正确地移除。理想情况下我们会一直循环到恰好遇到该 errno，
	 * 但如果问题是整个目录范围的（例如，突然无法读取数据目录本身），那就
	 * 有陷入无限循环的风险。我们的折中办法是：在非 ENOENT 的截断错误后
	 * 继续，但在任何取消链接错误后停止。如果确实存在目录范围的问题，额外的
	 * 取消链接尝试也无济于事。
	 */
	if (ret >= 0 || errno != ENOENT)
	{
		MdPathStr	segpath;
		BlockNumber segno;

		for (segno = 1;; segno++)
		{
			sprintf(segpath.str, "%s.%u", path.str, segno);

			if (!RelFileLocatorBackendIsTemp(rlocator))
			{
				/*
				 * 阻止其他后端的 fd 占用磁盘空间。不过，如果我们遇到
				 * ENOENT，就说明已经处理完了。
				 */
				if (do_truncate(segpath.str) < 0 && errno == ENOENT)
					break;

				/*
				 * 在我们尝试取消链接之前，忘记针对此段的任何待处理同步请求。
				 */
				register_forget_request(rlocator, forknum, segno);
			}

			if (unlink(segpath.str) < 0)
			{
				/* 最后一个段之后遇到 ENOENT 是预期的... */
				if (errno != ENOENT)
					ereport(WARNING,
							(errcode_for_file_access(),
							 errmsg("could not remove file \"%s\": %m", segpath.str)));
				break;
			}
		}
	}
}

/*
 * mdextend() -- 向指定关系追加一个块。
 *
 * 其语义与 mdwrite() 几乎相同：在指定位置写入。不过，本函数用于扩展关系
 * 的场景（即 blocknum 位于或超过当前 EOF）。注意我们假设：在当前 EOF 之外
 * 写入一个块，会使中间的文件空间被填充为零。
 */
void
mdextend(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		 const void *buffer, bool skipFsync)
{
	off_t		seekpos;
	int			nbytes;
	MdfdVec    *v;

	/* 如果此构建支持直接 I/O，缓冲区必须按 I/O 对齐。 */
	if (PG_O_DIRECT != 0 && PG_IO_ALIGN_SIZE <= BLCKSZ)
		Assert((uintptr_t) buffer == TYPEALIGN(PG_IO_ALIGN_SIZE, buffer));

	/* 这个断言一直开着代价太高... */
#ifdef CHECK_WRITE_VS_EXTEND
	Assert(blocknum >= mdnblocks(reln, forknum));
#endif

	/*
	 * 如果一个关系增长到 2^32-1 个块，就拒绝再扩展它 --- 我们绝不能创建
	 * 编号恰好是 InvalidBlockNumber 的块。（注意，由于 bufmgr.c 中上游的
	 * 检查，这个失败本应不可达。）
	 */
	if (blocknum == InvalidBlockNumber)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("cannot extend file \"%s\" beyond %u blocks",
						relpath(reln->smgr_rlocator, forknum).str,
						InvalidBlockNumber)));

	v = _mdfd_getseg(reln, forknum, blocknum, skipFsync, EXTENSION_CREATE);

	seekpos = (off_t) BLCKSZ * (blocknum % ((BlockNumber) RELSEG_SIZE));

	Assert(seekpos < (off_t) BLCKSZ * RELSEG_SIZE);

	if ((nbytes = FileWrite(v->mdfd_vfd, buffer, BLCKSZ, seekpos, WAIT_EVENT_DATA_FILE_EXTEND)) != BLCKSZ)
	{
		if (nbytes < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not extend file \"%s\": %m",
							FilePathName(v->mdfd_vfd)),
					 errhint("Check free disk space.")));
		/* 短写入：恰当地报错 */
		ereport(ERROR,
				(errcode(ERRCODE_DISK_FULL),
				 errmsg("could not extend file \"%s\": wrote only %d of %d bytes at block %u",
						FilePathName(v->mdfd_vfd),
						nbytes, BLCKSZ, blocknum),
				 errhint("Check free disk space.")));
	}

	if (!skipFsync && !SmgrIsTemp(reln))
		register_dirty_segment(reln, forknum, v);

	Assert(_mdnblocks(reln, forknum, v) <= ((BlockNumber) RELSEG_SIZE));
}

/*
 * mdzeroextend() -- 向指定关系追加新的、填充为零的块。
 *
 * 与 mdextend() 类似，区别在于关系可以一次性扩展多个块，且新增的块会被
 * 填充为零。
 */
void
mdzeroextend(SMgrRelation reln, ForkNumber forknum,
			 BlockNumber blocknum, int nblocks, bool skipFsync)
{
	MdfdVec    *v;
	BlockNumber curblocknum = blocknum;
	int			remblocks = nblocks;

	Assert(nblocks > 0);

	/* 这个断言一直开着代价太高... */
#ifdef CHECK_WRITE_VS_EXTEND
	Assert(blocknum >= mdnblocks(reln, forknum));
#endif

	/*
	 * 如果一个关系增长到 2^32-1 个块，就拒绝再扩展它 --- 我们绝不能创建
	 * 编号恰好是 InvalidBlockNumber 或更大的块。
	 */
	if ((uint64) blocknum + nblocks >= (uint64) InvalidBlockNumber)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("cannot extend file \"%s\" beyond %u blocks",
						relpath(reln->smgr_rlocator, forknum).str,
						InvalidBlockNumber)));

	while (remblocks > 0)
	{
		BlockNumber segstartblock = curblocknum % ((BlockNumber) RELSEG_SIZE);
		off_t		seekpos = (off_t) BLCKSZ * segstartblock;
		int			numblocks;

		if (segstartblock + remblocks > RELSEG_SIZE)
			numblocks = RELSEG_SIZE - segstartblock;
		else
			numblocks = remblocks;

		v = _mdfd_getseg(reln, forknum, curblocknum, skipFsync, EXTENSION_CREATE);

		Assert(segstartblock < RELSEG_SIZE);
		Assert(segstartblock + numblocks <= RELSEG_SIZE);

		/*
		 * 如果可用且有用的话，使用 posix_fallocate()（经由 FileFallocate()）
		 * 来扩展关系。这通常比使用 write() 更高效，因为它通常不会导致内核
		 * 为扩展出来的页面分配页缓存空间。
		 *
		 * 不过，对于较小的扩展我们不使用 FileFallocate()，因为它会破坏某些
		 * 文件系统上的延迟分配。但这个决策究竟该在哪里做出还不清楚？目前
		 * 就简单地使用一个 8 的阈值，在本地测试里 4 到 8 之间的情况都还
		 * 可以接受。
		 */
		if (numblocks > 8 &&
			file_extend_method != FILE_EXTEND_METHOD_WRITE_ZEROS)
		{
			int			ret = 0;

#ifdef HAVE_POSIX_FALLOCATE
			if (file_extend_method == FILE_EXTEND_METHOD_POSIX_FALLOCATE)
			{
				ret = FileFallocate(v->mdfd_vfd,
									seekpos, (off_t) BLCKSZ * numblocks,
									WAIT_EVENT_DATA_FILE_EXTEND);
			}
			else
#endif
			{
				elog(ERROR, "unsupported file_extend_method: %d",
					 file_extend_method);
			}
			if (ret != 0)
			{
				ereport(ERROR,
						errcode_for_file_access(),
						errmsg("could not extend file \"%s\" with FileFallocate(): %m",
							   FilePathName(v->mdfd_vfd)),
						errhint("Check free disk space."));
			}
		}
		else
		{
			int			ret;

			/*
			 * 即使我们不想使用 fallocate，也仍然可以比逐个写入每个 8kB 块
			 * 更高效地进行扩展。pg_pwrite_zeros()（经由 FileZero()）使用
			 * pg_pwritev_with_retry() 来避免多次写入，也无需为整个扩展长度
			 * 准备一个填充为零的缓冲区。
			 */
			ret = FileZero(v->mdfd_vfd,
						   seekpos, (off_t) BLCKSZ * numblocks,
						   WAIT_EVENT_DATA_FILE_EXTEND);
			if (ret < 0)
				ereport(ERROR,
						errcode_for_file_access(),
						errmsg("could not extend file \"%s\": %m",
							   FilePathName(v->mdfd_vfd)),
						errhint("Check free disk space."));
		}

		if (!skipFsync && !SmgrIsTemp(reln))
			register_dirty_segment(reln, forknum, v);

		Assert(_mdnblocks(reln, forknum, v) <= ((BlockNumber) RELSEG_SIZE));

		remblocks -= numblocks;
		curblocknum += numblocks;
	}
}

/*
 * mdopenfork() -- 打开指定关系的一个 fork。
 *
 * 注意，当存在多个段时，我们只打开第一个段。
 *
 * 如果第一个段不存在，则根据“behavior”要么 ereport，要么返回 NULL。我们把
 * EXTENSION_CREATE 当作与 EXTENSION_FAIL 一样处理；EXTENSION_CREATE 的意思是
 * 允许扩展一个已存在的关系，而不是凭空捏造出一个关系。
 */
static MdfdVec *
mdopenfork(SMgrRelation reln, ForkNumber forknum, int behavior)
{
	MdfdVec    *mdfd;
	RelPathStr	path;
	File		fd;

	/* 若已打开则无事可做 */
	if (reln->md_num_open_segs[forknum] > 0)
		return &reln->md_seg_fds[forknum][0];

	path = relpath(reln->smgr_rlocator, forknum);

	fd = PathNameOpenFile(path.str, _mdfd_open_flags());

	if (fd < 0)
	{
		if ((behavior & EXTENSION_RETURN_NULL) &&
			FILE_POSSIBLY_DELETED(errno))
			return NULL;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path.str)));
	}

	_fdvec_resize(reln, forknum, 1);
	mdfd = &reln->md_seg_fds[forknum][0];
	mdfd->mdfd_vfd = fd;
	mdfd->mdfd_segno = 0;

	Assert(_mdnblocks(reln, forknum, mdfd) <= ((BlockNumber) RELSEG_SIZE));

	return mdfd;
}

/*
 * mdopen() -- 初始化新打开的关系。
 */
void
mdopen(SMgrRelation reln)
{
	/* 标记它为未打开 */
	for (int forknum = 0; forknum <= MAX_FORKNUM; forknum++)
		reln->md_num_open_segs[forknum] = 0;
}

/*
 * mdclose() -- 关闭指定关系（如果尚未关闭的话）。
 */
void
mdclose(SMgrRelation reln, ForkNumber forknum)
{
	int			nopensegs = reln->md_num_open_segs[forknum];

	/* 若已关闭则无事可做 */
	if (nopensegs == 0)
		return;

	/* 从末尾开始关闭段 */
	while (nopensegs > 0)
	{
		MdfdVec    *v = &reln->md_seg_fds[forknum][nopensegs - 1];

		FileClose(v->mdfd_vfd);
		_fdvec_resize(reln, forknum, nopensegs - 1);
		nopensegs--;
	}
}

/*
 * mdprefetch() -- 发起对关系指定块的异步读取
 */
bool
mdprefetch(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		   int nblocks)
{
#ifdef USE_PREFETCH

	Assert((io_direct_flags & IO_DIRECT_DATA) == 0);

	if ((uint64) blocknum + nblocks > (uint64) MaxBlockNumber + 1)
		return false;

	while (nblocks > 0)
	{
		off_t		seekpos;
		MdfdVec    *v;
		int			nblocks_this_segment;

		v = _mdfd_getseg(reln, forknum, blocknum, false,
						 InRecovery ? EXTENSION_RETURN_NULL : EXTENSION_FAIL);
		if (v == NULL)
			return false;

		seekpos = (off_t) BLCKSZ * (blocknum % ((BlockNumber) RELSEG_SIZE));

		Assert(seekpos < (off_t) BLCKSZ * RELSEG_SIZE);

		nblocks_this_segment =
			Min(nblocks,
				RELSEG_SIZE - (blocknum % ((BlockNumber) RELSEG_SIZE)));

		(void) FilePrefetch(v->mdfd_vfd, seekpos, BLCKSZ * nblocks_this_segment,
							WAIT_EVENT_DATA_FILE_PREFETCH);

		blocknum += nblocks_this_segment;
		nblocks -= nblocks_this_segment;
	}
#endif							/* USE_PREFETCH */

	return true;
}

/*
 * 将一个缓冲区地址数组转换为一个 iovec 对象数组，并返回所需的 iovec 数量。
 * 'iov' 必须有足够的空间容纳最多 'nblocks' 个元素，但实际使用的数量可能
 * 因合并而更少。在连续缓冲区完全相邻的情况下，会填充单个 iovec，它可以作为
 * 普通的非向量化 I/O 来处理。
 */
static int
buffers_to_iovec(struct iovec *iov, void **buffers, int nblocks)
{
	struct iovec *iovp;
	int			iovcnt;

	Assert(nblocks >= 1);

	/* 如果此构建支持直接 I/O，缓冲区必须按 I/O 对齐。 */
	for (int i = 0; i < nblocks; ++i)
	{
		if (PG_O_DIRECT != 0 && PG_IO_ALIGN_SIZE <= BLCKSZ)
			Assert((uintptr_t) buffers[i] ==
				   TYPEALIGN(PG_IO_ALIGN_SIZE, buffers[i]));
	}

	/* 用第一个缓冲区作为第一个 iovec 的起点。 */
	iovp = &iov[0];
	iovp->iov_base = buffers[0];
	iovp->iov_len = BLCKSZ;
	iovcnt = 1;

	/* 尝试合并其余的。 */
	for (int i = 1; i < nblocks; ++i)
	{
		void	   *buffer = buffers[i];

		if (((char *) iovp->iov_base + iovp->iov_len) == buffer)
		{
			/* 与上个 iovec 连续。 */
			iovp->iov_len += BLCKSZ;
		}
		else
		{
			/* 需要一个新的 iovec。 */
			iovp++;
			iovp->iov_base = buffer;
			iovp->iov_len = BLCKSZ;
			iovcnt++;
		}
	}

	return iovcnt;
}

/*
 * mdmaxcombine() -- 返回可以从 blocknum 处开始的 IO 所能合并的
 *				 最大总块数。
 */
uint32
mdmaxcombine(SMgrRelation reln, ForkNumber forknum,
			 BlockNumber blocknum)
{
	BlockNumber segoff;

	segoff = blocknum % ((BlockNumber) RELSEG_SIZE);

	return RELSEG_SIZE - segoff;
}

/*
 * mdreadv() -- 从关系中读取指定的块。
 */
void
mdreadv(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		void **buffers, BlockNumber nblocks)
{
	while (nblocks > 0)
	{
		struct iovec iov[PG_IOV_MAX];
		int			iovcnt;
		off_t		seekpos;
		int			nbytes;
		MdfdVec    *v;
		BlockNumber nblocks_this_segment;
		size_t		transferred_this_segment;
		size_t		size_this_segment;

		v = _mdfd_getseg(reln, forknum, blocknum, false,
						 EXTENSION_FAIL | EXTENSION_CREATE_RECOVERY);

		seekpos = (off_t) BLCKSZ * (blocknum % ((BlockNumber) RELSEG_SIZE));

		Assert(seekpos < (off_t) BLCKSZ * RELSEG_SIZE);

		nblocks_this_segment =
			Min(nblocks,
				RELSEG_SIZE - (blocknum % ((BlockNumber) RELSEG_SIZE)));
		nblocks_this_segment = Min(nblocks_this_segment, lengthof(iov));

		if (nblocks_this_segment != nblocks)
			elog(ERROR, "read crosses segment boundary");

		iovcnt = buffers_to_iovec(iov, buffers, nblocks_this_segment);
		size_this_segment = nblocks_this_segment * BLCKSZ;
		transferred_this_segment = 0;

		/*
		 * 在一次短读之后继续的内层循环。我们会一直进行到遇到 EOF，而不是
		 * 假设短读就意味着已经到达末尾。
		 */
		for (;;)
		{
			TRACE_POSTGRESQL_SMGR_MD_READ_START(forknum, blocknum,
												reln->smgr_rlocator.locator.spcOid,
												reln->smgr_rlocator.locator.dbOid,
												reln->smgr_rlocator.locator.relNumber,
												reln->smgr_rlocator.backend);
			nbytes = FileReadV(v->mdfd_vfd, iov, iovcnt, seekpos,
							   WAIT_EVENT_DATA_FILE_READ);
			TRACE_POSTGRESQL_SMGR_MD_READ_DONE(forknum, blocknum,
											   reln->smgr_rlocator.locator.spcOid,
											   reln->smgr_rlocator.locator.dbOid,
											   reln->smgr_rlocator.locator.relNumber,
											   reln->smgr_rlocator.backend,
											   nbytes,
											   size_this_segment - transferred_this_segment);

#ifdef SIMULATE_SHORT_READ
			nbytes = Min(nbytes, 4096);
#endif

			if (nbytes < 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not read blocks %u..%u in file \"%s\": %m",
								blocknum,
								blocknum + nblocks_this_segment - 1,
								FilePathName(v->mdfd_vfd))));

			if (nbytes == 0)
			{
			/*
			 * 我们位于 EOF 处或已越过了 EOF，或者在 EOF 处读到了一个不完整的块。
			 * 通常这是一个错误；上层永远不应尝试读取不存在的块。不过，如果
			 * zero_damaged_pages 为 ON，或者我们处于 InRecovery，则应该不报错
			 * 而返回零。这允许诸如这样的情况：试图更新一个后来被截断掉的块。
			 *
			 * 注意：我们认为这段代码路径在恢复期间是不可达的，并且在
			 * zero_damaged_pages 下也是不完整的，因为缺失的段不会被创建。
			 * 把磁盘上并不存在的块放进缓冲池是相当有问题的，因为依赖
			 * smgrnblocks() 的扫描不会找到它们，它们位于 EOF 之外。它还可能
			 * 引发关系扩展方面的奇怪问题，因为关系扩展不期望 EOF 之外存在块。
			 *
			 * 因此我们不想把这段逻辑复制到 mdstartreadv() 中，因为在那里由于
			 * IO 的定义者与完成者之间 zero_damaged_pages 设置可能存在的差异，
			 * 它会变得更为复杂。
			 *
			 * 对于 PG 18，我们在 mdreadv() 中放置了一个 Assert(false)
			 * （在启用断言的构建中会触发失败，但在生产构建中仍继续工作）。
			 * 之后我们计划完全移除这段代码。
			 */
				if (zero_damaged_pages || InRecovery)
				{
					Assert(false);	/* 见上方注释 */

					for (BlockNumber i = transferred_this_segment / BLCKSZ;
						 i < nblocks_this_segment;
						 ++i)
						memset(buffers[i], 0, BLCKSZ);
					break;
				}
				else
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("could not read blocks %u..%u in file \"%s\": read only %zu of %zu bytes",
									blocknum,
									blocknum + nblocks_this_segment - 1,
									FilePathName(v->mdfd_vfd),
									transferred_this_segment,
									size_this_segment)));
			}

			/* 通常一次循环就足够。 */
			transferred_this_segment += nbytes;
			Assert(transferred_this_segment <= size_this_segment);
			if (transferred_this_segment == size_this_segment)
				break;

			/* 短读之后调整位置和向量。 */
			seekpos += nbytes;
			iovcnt = compute_remaining_iovec(iov, iov, iovcnt, nbytes);
		}

		nblocks -= nblocks_this_segment;
		buffers += nblocks_this_segment;
		blocknum += nblocks_this_segment;
	}
}

/*
 * mdstartreadv() -- mdreadv() 的异步版本。
 */
void
mdstartreadv(PgAioHandle *ioh,
			 SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
			 void **buffers, BlockNumber nblocks)
{
	off_t		seekpos;
	MdfdVec    *v;
	BlockNumber nblocks_this_segment;
	struct iovec *iov;
	int			iovcnt;
	int			ret;

	v = _mdfd_getseg(reln, forknum, blocknum, false,
					 EXTENSION_FAIL | EXTENSION_CREATE_RECOVERY);

	seekpos = (off_t) BLCKSZ * (blocknum % ((BlockNumber) RELSEG_SIZE));

	Assert(seekpos < (off_t) BLCKSZ * RELSEG_SIZE);

	nblocks_this_segment =
		Min(nblocks,
			RELSEG_SIZE - (blocknum % ((BlockNumber) RELSEG_SIZE)));

	if (nblocks_this_segment != nblocks)
		elog(ERROR, "read crossing segment boundary");

	iovcnt = pgaio_io_get_iovec(ioh, &iov);

	Assert(nblocks <= iovcnt);

	iovcnt = buffers_to_iovec(iov, buffers, nblocks_this_segment);

	Assert(iovcnt <= nblocks_this_segment);

	if (!(io_direct_flags & IO_DIRECT_DATA))
		pgaio_io_set_flag(ioh, PGAIO_HF_BUFFERED);

	pgaio_io_set_target_smgr(ioh,
							 reln,
							 forknum,
							 blocknum,
							 nblocks,
							 false);
	pgaio_io_register_callbacks(ioh, PGAIO_HCB_MD_READV, 0);

	ret = FileStartReadV(ioh, v->mdfd_vfd, iovcnt, seekpos, WAIT_EVENT_DATA_FILE_READ);
	if (ret != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not start reading blocks %u..%u in file \"%s\": %m",
						blocknum,
						blocknum + nblocks_this_segment - 1,
						FilePathName(v->mdfd_vfd))));

	/*
	 * 与 mdreadv() 中读后检查相对应的错误检查位于 md_readv_complete() 中。
	 *
	 * 不过，至少目前我们选择了不实现 mdreadv() 中存在的 zero_damaged_pages
	 * 逻辑。正如 mdreadv() 中所述，那段逻辑相当成问题，而我们想要摆脱它。
	 * 在这里，等效的逻辑会由于 IO 的定义者与完成者之间 zero_damaged_pages
	 * 设置可能存在的差异而必须更加复杂。
	 */
}

/*
 * mdwritev() -- 在适当的位置写入所提供的块。
 *
 * 本函数只应用于更新关系中已存在的块（即当前 EOF 之前的那些）。要扩展关系，
 * 请使用 mdextend()。
 */
void
mdwritev(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
		 const void **buffers, BlockNumber nblocks, bool skipFsync)
{
	/* 这个断言一直开着代价太高... */
#ifdef CHECK_WRITE_VS_EXTEND
	Assert((uint64) blocknum + (uint64) nblocks <= (uint64) mdnblocks(reln, forknum));
#endif

	while (nblocks > 0)
	{
		struct iovec iov[PG_IOV_MAX];
		int			iovcnt;
		off_t		seekpos;
		int			nbytes;
		MdfdVec    *v;
		BlockNumber nblocks_this_segment;
		size_t		transferred_this_segment;
		size_t		size_this_segment;

		v = _mdfd_getseg(reln, forknum, blocknum, skipFsync,
						 EXTENSION_FAIL | EXTENSION_CREATE_RECOVERY);

		seekpos = (off_t) BLCKSZ * (blocknum % ((BlockNumber) RELSEG_SIZE));

		Assert(seekpos < (off_t) BLCKSZ * RELSEG_SIZE);

		nblocks_this_segment =
			Min(nblocks,
				RELSEG_SIZE - (blocknum % ((BlockNumber) RELSEG_SIZE)));
		nblocks_this_segment = Min(nblocks_this_segment, lengthof(iov));

		if (nblocks_this_segment != nblocks)
			elog(ERROR, "write crosses segment boundary");

		iovcnt = buffers_to_iovec(iov, (void **) buffers, nblocks_this_segment);
		size_this_segment = nblocks_this_segment * BLCKSZ;
		transferred_this_segment = 0;

		/*
		 * 在一次短写之后继续的内层循环。如果原因是磁盘空间不足，那么后续
		 * 的尝试应该从内核得到 ENOSPC 错误。
		 */
		for (;;)
		{
			TRACE_POSTGRESQL_SMGR_MD_WRITE_START(forknum, blocknum,
												 reln->smgr_rlocator.locator.spcOid,
												 reln->smgr_rlocator.locator.dbOid,
												 reln->smgr_rlocator.locator.relNumber,
												 reln->smgr_rlocator.backend);
			nbytes = FileWriteV(v->mdfd_vfd, iov, iovcnt, seekpos,
								WAIT_EVENT_DATA_FILE_WRITE);
			TRACE_POSTGRESQL_SMGR_MD_WRITE_DONE(forknum, blocknum,
												reln->smgr_rlocator.locator.spcOid,
												reln->smgr_rlocator.locator.dbOid,
												reln->smgr_rlocator.locator.relNumber,
												reln->smgr_rlocator.backend,
												nbytes,
												size_this_segment - transferred_this_segment);

#ifdef SIMULATE_SHORT_WRITE
			nbytes = Min(nbytes, 4096);
#endif

			if (nbytes < 0)
			{
				bool		enospc = errno == ENOSPC;

				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not write blocks %u..%u in file \"%s\": %m",
								blocknum,
								blocknum + nblocks_this_segment - 1,
								FilePathName(v->mdfd_vfd)),
						 enospc ? errhint("Check free disk space.") : 0));
			}

			/* 通常一次循环就足够。 */
			transferred_this_segment += nbytes;
			Assert(transferred_this_segment <= size_this_segment);
			if (transferred_this_segment == size_this_segment)
				break;

			/* 短写之后调整位置和 iovec。 */
			seekpos += nbytes;
			iovcnt = compute_remaining_iovec(iov, iov, iovcnt, nbytes);
		}

		if (!skipFsync && !SmgrIsTemp(reln))
			register_dirty_segment(reln, forknum, v);

		nblocks -= nblocks_this_segment;
		buffers += nblocks_this_segment;
		blocknum += nblocks_this_segment;
	}
}


/*
 * mdwriteback() -- 通知内核将页面写回存储。
 *
 * 本函数接受一个块范围，因为一次性刷新多个页面比逐个刷新要高效得多。
 */
void
mdwriteback(SMgrRelation reln, ForkNumber forknum,
			BlockNumber blocknum, BlockNumber nblocks)
{
	Assert((io_direct_flags & IO_DIRECT_DATA) == 0);

	/*
	 * 用尽可能少的请求发出刷新请求；不过必须在段边界处拆分，因为那些段
	 * 实际上是独立的文件。
	 */
	while (nblocks > 0)
	{
		BlockNumber nflush = nblocks;
		off_t		seekpos;
		MdfdVec    *v;
		int			segnum_start,
					segnum_end;

		v = _mdfd_getseg(reln, forknum, blocknum, true /* 未使用 */ ,
						 EXTENSION_DONT_OPEN);

		/*
		 * 我们可能正在刷新已被移除关系的缓冲区，这没关系，只需忽略这种情况。
		 * 如果该段文件尚未打开（例如来自最近的 mdwrite()），那么我们不想
		 * 重新打开它，以避免与 PROCSIGNAL_BARRIER_SMGRRELEASE 发生竞争，后者
		 * 可能会留给我们一个指向即将被取消链接的文件的描述符。
		 */
		if (!v)
			return;

		/* 计算在当前段内的偏移 */
		segnum_start = blocknum / RELSEG_SIZE;

		/* 计算当前段内期望的写入数量 */
		segnum_end = (blocknum + nblocks - 1) / RELSEG_SIZE;
		if (segnum_start != segnum_end)
			nflush = RELSEG_SIZE - (blocknum % ((BlockNumber) RELSEG_SIZE));

		Assert(nflush >= 1);
		Assert(nflush <= nblocks);

		seekpos = (off_t) BLCKSZ * (blocknum % ((BlockNumber) RELSEG_SIZE));

		FileWriteback(v->mdfd_vfd, seekpos, (off_t) BLCKSZ * nflush, WAIT_EVENT_DATA_FILE_FLUSH);

		nblocks -= nflush;
		blocknum += nflush;
	}
}

/*
 * mdnblocks() -- 获取关系中存储的块数量。
 *
 * 重要的副作用：关系的所有活动段都会被打开并加入 md_seg_fds 数组。如果本
 * 例程尚未被调用，则数组中只含有直到最后一个实际被访问过的段。
 */
BlockNumber
mdnblocks(SMgrRelation reln, ForkNumber forknum)
{
	MdfdVec    *v;
	BlockNumber nblocks;
	BlockNumber segno;

	mdopenfork(reln, forknum, EXTENSION_FAIL);

	/* mdopen 已经打开了第一个段 */
	Assert(reln->md_num_open_segs[forknum] > 0);

	/*
	 * 从最后一个已打开的段开始，以避免多余的定位。我们之前已经验证过这些段
	 * 恰好是 RELSEG_SIZE 长，因此每次都重新检查是没用的。
	 *
	 * 注意：这个假设只有在另一个后端截断了该关系时才可能出错。我们依赖更高
	 * 层级的代码通过关闭并重新打开 md fd 来处理那种情况，而这经由 relcache
	 * 刷新来实施。（由于检查点进程不参与 relcache 刷新，它可能会持有指向
	 * 非活动段的段条目；这没关系，因为检查点进程从不需要计算关系大小。）
	 */
	segno = reln->md_num_open_segs[forknum] - 1;
	v = &reln->md_seg_fds[forknum][segno];

	for (;;)
	{
		nblocks = _mdnblocks(reln, forknum, v);
		if (nblocks > ((BlockNumber) RELSEG_SIZE))
			elog(FATAL, "segment too big");
		if (nblocks < ((BlockNumber) RELSEG_SIZE))
			return (segno * ((BlockNumber) RELSEG_SIZE)) + nblocks;

		/*
		 * 如果段恰好是 RELSEG_SIZE，则前进到下一个段。
		 */
		segno++;

		/*
		 * 我们以前在这里传入 O_CREAT，但那样有个缺点：它可能会创建一个因某些
		 * 操作系统意外而消失的段。在这种情况下，在这里创建该段会破坏
		 * _mdfd_getseg 在访问缺失段时察觉并报错的做法。
		 */
		v = _mdfd_openseg(reln, forknum, segno, 0);
		if (v == NULL)
			return segno * ((BlockNumber) RELSEG_SIZE);
	}
}

/*
 * mdtruncate() -- 将关系截断到指定的块数量。
 *
 * 保证不分配内存，因此可以在临界区中使用。调用方必须在持有足够锁（以防止
 * 关系大小发生变化）的情况下调用 smgrnblocks() 来获取 curnblk，并且期间
 * 不能对本关系使用任何 smgr 函数或处理中断。这确保我们已经打开了所有活动段，
 * 以便截断循环能覆盖它们全部！
 *
 * 如果 nblocks > curnblk，在处于 InRecovery 时该请求会被忽略，否则将报错。
 */
void
mdtruncate(SMgrRelation reln, ForkNumber forknum,
		   BlockNumber curnblk, BlockNumber nblocks)
{
	BlockNumber priorblocks;
	int			curopensegs;

	if (nblocks > curnblk)
	{
		/* 无效请求……但在 InRecovery 时不报错 */
		if (InRecovery)
			return;
		ereport(ERROR,
				(errmsg("could not truncate file \"%s\" to %u blocks: it's only %u blocks now",
						relpath(reln->smgr_rlocator, forknum).str,
						nblocks, curnblk)));
	}
	if (nblocks == curnblk)
		return;					/* 无事可做 */

	/*
	 * Truncate segments, starting at the last one. Starting at the end makes
	 * managing the memory for the fd array easier, should there be errors.
	 */
	curopensegs = reln->md_num_open_segs[forknum];
	while (curopensegs > 0)
	{
		MdfdVec    *v;

		priorblocks = (curopensegs - 1) * RELSEG_SIZE;

		v = &reln->md_seg_fds[forknum][curopensegs - 1];

		if (priorblocks > nblocks)
		{
			/*
			 * 这个段不再是活动的了。我们截断该文件，但不删除它，原因见文件
			 * 头部的注释。
			 */
			if (FileTruncate(v->mdfd_vfd, 0, WAIT_EVENT_DATA_FILE_TRUNCATE) < 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not truncate file \"%s\": %m",
								FilePathName(v->mdfd_vfd))));

			if (!SmgrIsTemp(reln))
				register_dirty_segment(reln, forknum, v);

			/* 我们永远不会丢弃第 1 个段 */
			Assert(v != &reln->md_seg_fds[forknum][0]);

			FileClose(v->mdfd_vfd);
			_fdvec_resize(reln, forknum, curopensegs - 1);
		}
		else if (priorblocks + ((BlockNumber) RELSEG_SIZE) > nblocks)
		{
			/*
			 * 这是我们想要保留的最后一个段。把文件截断到正确的长度。注意：
			 * 如果 nblocks 恰好是 RELSEG_SIZE 的 K 倍，我们会把第 K+1 个段
			 * 截断为 0 长度但保留它。这符合文件头部注释中给出的不变量。
			 */
			BlockNumber lastsegblocks = nblocks - priorblocks;

			if (FileTruncate(v->mdfd_vfd, (off_t) lastsegblocks * BLCKSZ, WAIT_EVENT_DATA_FILE_TRUNCATE) < 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not truncate file \"%s\" to %u blocks: %m",
								FilePathName(v->mdfd_vfd),
								nblocks)));
			if (!SmgrIsTemp(reln))
				register_dirty_segment(reln, forknum, v);
		}
		else
		{
			/*
			 * 我们仍然需要这个段，因此本段以及任何更早的段都无需处理。
			 */
			break;
		}
		curopensegs--;
	}
}

/*
 * mdregistersync() -- 将整个关系标记为需要 fsync
 */
void
mdregistersync(SMgrRelation reln, ForkNumber forknum)
{
	int			segno;
	int			min_inactive_seg;

	/*
	 * 注意：mdnblocks 确保我们已经打开了所有活动段，因此下面的循环能覆盖
	 * 它们全部！
	 */
	mdnblocks(reln, forknum);

	min_inactive_seg = segno = reln->md_num_open_segs[forknum];

	/*
	 * 临时打开非活动段，然后在同步之后关闭它们。出错后可能会留下一些仍打开
	 * 的非活动段，但那是无害的。我们不想费心去清理它们而承担进一步出问题的
	 * 风险。下一次 mdclose() 很快就会关闭它们。
	 */
	while (_mdfd_openseg(reln, forknum, segno, 0) != NULL)
		segno++;

	while (segno > 0)
	{
		MdfdVec    *v = &reln->md_seg_fds[forknum][segno - 1];

		register_dirty_segment(reln, forknum, v);

		/* 立即关闭非活动段 */
		if (segno > min_inactive_seg)
		{
			FileClose(v->mdfd_vfd);
			_fdvec_resize(reln, forknum, segno - 1);
		}

		segno--;
	}
}

/*
 * mdimmedsync() -- 立即将关系同步到稳定存储。
 *
 * 注意，只有已经发出的写入才会被同步；本例程对缓冲管理器内部可能存在的脏
 * 缓冲区一无所知。我们会同步活动段和非活动段；smgrDoPendingSyncs() 依赖于
 * 这一点。考虑一个跳过 WAL 的关系。假设某个检查点同步了某个段的块，随后
 * mdtruncate() 使该段变为非活动。如果在下一次检查点同步这个新的非活动段之前
 * 我们发生崩溃，该段可能会在恢复中幸存，从而把不希望的数据重新引入表中。
 */
void
mdimmedsync(SMgrRelation reln, ForkNumber forknum)
{
	int			segno;
	int			min_inactive_seg;

	/*
	 * 注意：mdnblocks 确保我们已经打开了所有活动段，因此下面的循环能覆盖
	 * 它们全部！
	 */
	mdnblocks(reln, forknum);

	min_inactive_seg = segno = reln->md_num_open_segs[forknum];

	/*
	 * 临时打开非活动段，然后在同步之后关闭它们。fsync() 出错后可能会留下一些
	 * 仍打开的非活动段，但那是无害的。我们不想费心去清理它们而承担进一步
	 * 出问题的风险。下一次 mdclose() 很快就会关闭它们。
	 */
	while (_mdfd_openseg(reln, forknum, segno, 0) != NULL)
		segno++;

	while (segno > 0)
	{
		MdfdVec    *v = &reln->md_seg_fds[forknum][segno - 1];

		/*
		 * fsyncs done through mdimmedsync() should be tracked in a separate
		 * IOContext than those done through mdsyncfiletag() to differentiate
		 * between unavoidable client backend fsyncs (e.g. those done during
		 * index build) and those which ideally would have been done by the
		 * checkpointer. Since other IO operations bypassing the buffer
		 * manager could also be tracked in such an IOContext, wait until
		 * these are also tracked to track immediate fsyncs.
		 */
		if (FileSync(v->mdfd_vfd, WAIT_EVENT_DATA_FILE_IMMEDIATE_SYNC) < 0)
			ereport(data_sync_elevel(ERROR),
					(errcode_for_file_access(),
						errmsg("could not fsync file \"%s\": %m",
							FilePathName(v->mdfd_vfd))));

		/* 立即关闭非活动段 */
		if (segno > min_inactive_seg)
		{
			FileClose(v->mdfd_vfd);
			_fdvec_resize(reln, forknum, segno - 1);
		}

		segno--;
	}
}

int
mdfd(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum, uint32 *off)
{
	MdfdVec    *v = mdopenfork(reln, forknum, EXTENSION_FAIL);

	v = _mdfd_getseg(reln, forknum, blocknum, false,
					 EXTENSION_FAIL);

	*off = (off_t) BLCKSZ * (blocknum % ((BlockNumber) RELSEG_SIZE));

	Assert(*off < (off_t) BLCKSZ * RELSEG_SIZE);

	return FileGetRawDesc(v->mdfd_vfd);
}

/*
 * register_dirty_segment() -- 将一个关系段标记为需要 fsync
 *
 * 如果存在一个本地的待处理操作表，就只是向其中插入一个条目，供
 * ProcessSyncRequests 稍后处理。否则，尝试把 fsync 请求转交给检查点进程。
 * 如果那样失败，就在返回前于本地直接执行 fsync（我们希望这种情况不会经常
 * 发生，以至于成为性能问题）。
 */
static void
register_dirty_segment(SMgrRelation reln, ForkNumber forknum, MdfdVec *seg)
{
	FileTag		tag;

	INIT_MD_FILETAG(tag, reln->smgr_rlocator.locator, forknum, seg->mdfd_segno);

	/* 临时关系永远不应被 fsync */
	Assert(!SmgrIsTemp(reln));

	if (!RegisterSyncRequest(&tag, SYNC_REQUEST, false /* 出错时重试 */ ))
	{
		instr_time	io_start;

		ereport(DEBUG1,
				(errmsg_internal("could not forward fsync request because request queue is full")));

		io_start = pgstat_prepare_io_time(track_io_timing);

		if (FileSync(seg->mdfd_vfd, WAIT_EVENT_DATA_FILE_SYNC) < 0)
			ereport(data_sync_elevel(ERROR),
					(errcode_for_file_access(),
					 errmsg("could not fsync file \"%s\": %m",
							FilePathName(seg->mdfd_vfd))));

		/*
		 * 在这一点上，我们无法得知当前 IOContext 是 IOCONTEXT_NORMAL 还是
		 * IOCONTEXT_[BULKREAD, BULKWRITE, VACUUM]，因此把这个 fsync 计入
		 * IOCONTEXT_NORMAL 这个 IOContext。这可能没问题，因为后端 fsync 的
		 * 数量并不能说明 BufferAccessStrategy 的有效性。而且，在调查后端
		 * fsync 数量时，把在 IOCONTEXT_NORMAL 和
		 * IOCONTEXT_[BULKREAD, BULKWRITE, VACUUM] 中完成的 fsync 都计入
		 * IOCONTEXT_NORMAL 下，可能反而更清晰。
		 */
		pgstat_count_io_op_time(IOOBJECT_RELATION, IOCONTEXT_NORMAL,
								IOOP_FSYNC, io_start, 1, 0);
	}
}

/*
 * register_unlink_segment() -- 安排在下次检查点之后删除一个文件
 */
static void
register_unlink_segment(RelFileLocatorBackend rlocator, ForkNumber forknum,
						BlockNumber segno)
{
	FileTag		tag;

	INIT_MD_FILETAG(tag, rlocator.locator, forknum, segno);

	/* 绝不应与临时关系一起使用 */
	Assert(!RelFileLocatorBackendIsTemp(rlocator));

	RegisterSyncRequest(&tag, SYNC_UNLINK_REQUEST, true /* 出错时重试 */ );
}

/*
 * register_forget_request() -- 忘记针对某关系 fork 段的任何 fsync
 */
static void
register_forget_request(RelFileLocatorBackend rlocator, ForkNumber forknum,
						BlockNumber segno)
{
	FileTag		tag;

	INIT_MD_FILETAG(tag, rlocator.locator, forknum, segno);

	RegisterSyncRequest(&tag, SYNC_FORGET_REQUEST, true /* 出错时重试 */ );
}

/*
 * ForgetDatabaseSyncRequests -- 忘记针对某个数据库的 fsync 与取消链接
 */
void
ForgetDatabaseSyncRequests(Oid dbid)
{
	FileTag		tag;
	RelFileLocator rlocator;

	rlocator.dbOid = dbid;
	rlocator.spcOid = 0;
	rlocator.relNumber = 0;

	INIT_MD_FILETAG(tag, rlocator, InvalidForkNumber, InvalidBlockNumber);

	RegisterSyncRequest(&tag, SYNC_FILTER_REQUEST, true /* 出错时重试 */ );
}

/*
 * DropRelationFiles -- 丢弃所有给定关系的文件
 */
void
DropRelationFiles(RelFileLocator *delrels, int ndelrels, bool isRedo)
{
	SMgrRelation *srels;
	int			i;

	srels = palloc(sizeof(SMgrRelation) * ndelrels);
	for (i = 0; i < ndelrels; i++)
	{
		SMgrRelation srel = smgropen(delrels[i], INVALID_PROC_NUMBER);

		if (isRedo)
		{
			ForkNumber	fork;

			for (fork = 0; fork <= MAX_FORKNUM; fork++)
				XLogDropRelation(delrels[i], fork);
		}
		srels[i] = srel;
	}

	smgrdounlinkall(srels, ndelrels, isRedo);

	for (i = 0; i < ndelrels; i++)
		smgrclose(srels[i]);
	pfree(srels);
}


/*
 * _fdvec_resize() -- 调整 fork 的已打开段数组的大小
 */
static void
_fdvec_resize(SMgrRelation reln,
			  ForkNumber forknum,
			  int nseg)
{
	if (nseg == 0)
	{
		if (reln->md_num_open_segs[forknum] > 0)
		{
			pfree(reln->md_seg_fds[forknum]);
			reln->md_seg_fds[forknum] = NULL;
		}
	}
	else if (reln->md_num_open_segs[forknum] == 0)
	{
		reln->md_seg_fds[forknum] =
			MemoryContextAlloc(MdCxt, sizeof(MdfdVec) * nseg);
	}
	else if (nseg > reln->md_num_open_segs[forknum])
	{
		/*
		 * 为了让 repalloc() 调用均摊而把代码弄复杂，似乎并不值得。那些调用
		 * 远比 PathNameOpenFile() 或 FileClose() 快，而且内存上下文内部有时
		 * 会避免实际重新分配。
		 */
		reln->md_seg_fds[forknum] =
			repalloc(reln->md_seg_fds[forknum],
					 sizeof(MdfdVec) * nseg);
	}
	else
	{
		/*
		 * 我们不会对更小的数组重新分配，因为我们希望 mdtruncate() 能够承诺
		 * 它不会分配内存，从而允许它在临界区中使用。这意味着数组中现在有一
		 * 点空间被浪费了，直到下一次我们新增一个段并重新分配。
		 */
	}

	reln->md_num_open_segs[forknum] = nseg;
}

/*
 * 返回关系指定段的文件名。返回的字符串是通过 palloc 分配的。
 */
static MdPathStr
_mdfd_segpath(SMgrRelation reln, ForkNumber forknum, BlockNumber segno)
{
	RelPathStr	path;
	MdPathStr	fullpath;

	path = relpath(reln->smgr_rlocator, forknum);

	if (segno > 0)
		sprintf(fullpath.str, "%s.%u", path.str, segno);
	else
		strcpy(fullpath.str, path.str);

	return fullpath;
}

/*
 * 打开关系的指定段，并为其创建一个 MdfdVec 对象。失败时返回 NULL。
 */
static MdfdVec *
_mdfd_openseg(SMgrRelation reln, ForkNumber forknum, BlockNumber segno,
			  int oflags)
{
	MdfdVec    *v;
	File		fd;
	MdPathStr	fullpath;

	fullpath = _mdfd_segpath(reln, forknum, segno);

	/* 打开文件 */
	fd = PathNameOpenFile(fullpath.str, _mdfd_open_flags() | oflags);

	if (fd < 0)
		return NULL;

		/*
		 * 段总是按从低到高的顺序打开，因此我们现在一定是在末尾新增一个段。
		 */
	Assert(segno == reln->md_num_open_segs[forknum]);

	_fdvec_resize(reln, forknum, segno + 1);

	/* 填充该条目 */
	v = &reln->md_seg_fds[forknum][segno];
	v->mdfd_vfd = fd;
	v->mdfd_segno = segno;

	Assert(_mdnblocks(reln, forknum, v) <= ((BlockNumber) RELSEG_SIZE));

	/* 全部完成 */
	return v;
}

/*
 * _mdfd_getseg() -- 找到持有指定块的关系段。
 *
 * 如果段不存在，则根据“behavior”要么 ereport、返回 NULL，要么创建该段。
 * 注意：skipFsync 仅用于 EXTENSION_CREATE 的情况。
 */
static MdfdVec *
_mdfd_getseg(SMgrRelation reln, ForkNumber forknum, BlockNumber blkno,
			 bool skipFsync, int behavior)
{
	MdfdVec    *v;
	BlockNumber targetseg;
	BlockNumber nextsegno;

	/* 必须指定某种处理不存在段的方式 */
	Assert(behavior &
		   (EXTENSION_FAIL | EXTENSION_CREATE | EXTENSION_RETURN_NULL |
			EXTENSION_DONT_OPEN));

	targetseg = blkno / ((BlockNumber) RELSEG_SIZE);

	/* 如果是已存在且已打开的段，则大功告成 */
	if (targetseg < reln->md_num_open_segs[forknum])
	{
		v = &reln->md_seg_fds[forknum][targetseg];
		return v;
	}

	/* 调用方只在该段已经被打开时才想要它。 */
	if (behavior & EXTENSION_DONT_OPEN)
		return NULL;

	/*
	 * 目标段尚未打开。在最后一个已打开的段与目标段之间的所有段上迭代。这样，
	 * 缺失的段要么报错，要么被创建（取决于 'behavior'）。从最后一个已打开的
	 * 段开始；如果之前没有打开过任何段，则从第一个段开始。
	 */
	if (reln->md_num_open_segs[forknum] > 0)
		v = &reln->md_seg_fds[forknum][reln->md_num_open_segs[forknum] - 1];
	else
	{
		v = mdopenfork(reln, forknum, behavior);
		if (!v)
			return NULL;		/* 若 behavior 含 EXTENSION_RETURN_NULL */
	}

	for (nextsegno = reln->md_num_open_segs[forknum];
		 nextsegno <= targetseg; nextsegno++)
	{
		BlockNumber nblocks = _mdnblocks(reln, forknum, v);
		int			flags = 0;

		Assert(nextsegno == v->mdfd_segno + 1);

		if (nblocks > ((BlockNumber) RELSEG_SIZE))
			elog(FATAL, "segment too big");

		if ((behavior & EXTENSION_CREATE) ||
			(InRecovery && (behavior & EXTENSION_CREATE_RECOVERY)))
		{
			/*
			 * 通常我们只在调用方授权时才创建新段（即我们在做 mdextend()）。
			 * 但在做 WAL 恢复时，无论如何都要创建段；这允许诸如重放写入了
			 * 某个后来被删除关系的高编号段的 WAL 数据这样的情况。我们想要
			 * 继续创建这些段，以便能够完成重放。
			 *
			 * 我们必须维持这样一个不变量：最后一个活动段之前的段大小都是
			 * RELSEG_SIZE；因此，如果在扩展，则在需要时用零把它们填充完整。
			 * （这只在恢复期间，或者调用方不连续地扩展关系时才重要，不过在
			 * 哈希索引中可能发生。）
			 */
			if (nblocks < ((BlockNumber) RELSEG_SIZE))
			{
				char	   *zerobuf = palloc_aligned(BLCKSZ, PG_IO_ALIGN_SIZE,
													 MCXT_ALLOC_ZERO);

				mdextend(reln, forknum,
						 nextsegno * ((BlockNumber) RELSEG_SIZE) - 1,
						 zerobuf, skipFsync);
				pfree(zerobuf);
			}
			flags = O_CREAT;
		}
		else if (nblocks < ((BlockNumber) RELSEG_SIZE))
		{
			/*
			 * 在不扩展的情况下，只有当前段恰好是 RELSEG_SIZE 时才打开下一个段。
			 * 如果不是（即走到这个分支），则要么返回 NULL，要么报错。
			 */
			if (behavior & EXTENSION_RETURN_NULL)
			{
				/*
				 * 某些调用方会根据 errno 来区分 _mdfd_getseg() 返回 NULL 的原因。
				 * 由于这种情况并不涉及失败的 syscall，因此显式地把 errno 设为
				 * ENOENT，因为这似乎是最贴切的解释。
				 */
				errno = ENOENT;
				return NULL;
			}

			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\" (target block %u): previous segment is only %u blocks",
							_mdfd_segpath(reln, forknum, nextsegno).str,
							blkno, nblocks)));
		}

		v = _mdfd_openseg(reln, forknum, nextsegno, flags);

		if (v == NULL)
		{
			if ((behavior & EXTENSION_RETURN_NULL) &&
				FILE_POSSIBLY_DELETED(errno))
				return NULL;
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open file \"%s\" (target block %u): %m",
							_mdfd_segpath(reln, forknum, nextsegno).str,
							blkno)));
		}
	}

	return v;
}

/*
 * 获取单个磁盘文件中存在的块数量
 */
static BlockNumber
_mdnblocks(SMgrRelation reln, ForkNumber forknum, MdfdVec *seg)
{
	off_t		len;

	len = FileSize(seg->mdfd_vfd);
	if (len < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not seek to end of file \"%s\": %m",
						FilePathName(seg->mdfd_vfd))));
	/* 注意这个计算会忽略 EOF 处任何不完整的块 */
	return (BlockNumber) (len / BLCKSZ);
}

/*
 * 给定文件标签，将文件同步到磁盘。将路径写入一个输出缓冲区，以便调用方可以
 * 在错误消息中使用它。
 *
 * 成功返回 0，失败返回 -1，并设置 errno。
 */
int
mdsyncfiletag(const FileTag *ftag, char *path)
{
	SMgrRelation reln = smgropen(ftag->rlocator, INVALID_PROC_NUMBER);
	File		file;
	instr_time	io_start;
	bool		need_to_close;
	int			result,
				save_errno;

	/* 看文件是否已经打开，还是需要打开它。 */
	if (ftag->segno < reln->md_num_open_segs[ftag->forknum])
	{
		file = reln->md_seg_fds[ftag->forknum][ftag->segno].mdfd_vfd;
		strlcpy(path, FilePathName(file), MAXPGPATH);
		need_to_close = false;
	}
	else
	{
		MdPathStr	p;

		p = _mdfd_segpath(reln, ftag->forknum, ftag->segno);
		strlcpy(path, p.str, MD_PATH_STR_MAXLEN);

		file = PathNameOpenFile(path, _mdfd_open_flags());
		if (file < 0)
			return -1;
		need_to_close = true;
	}

	io_start = pgstat_prepare_io_time(track_io_timing);

	/* 同步该文件。 */
	result = FileSync(file, WAIT_EVENT_DATA_FILE_SYNC);
	save_errno = errno;

	if (need_to_close)
		FileClose(file);

	pgstat_count_io_op_time(IOOBJECT_RELATION, IOCONTEXT_NORMAL,
							IOOP_FSYNC, io_start, 1, 0);

	errno = save_errno;
	return result;
}

/*
 * 给定文件标签，取消链接一个文件。将路径写入一个输出缓冲区，以便调用方可以
 * 在错误消息中使用它。
 *
 * 成功返回 0，失败返回 -1，并设置 errno。
 */
int
mdunlinkfiletag(const FileTag *ftag, char *path)
{
	RelPathStr	p;

	/* 计算路径。 */
	p = relpathperm(ftag->rlocator, MAIN_FORKNUM);
	strlcpy(path, p.str, MAXPGPATH);

	/* 尝试取消链接该文件。 */
	return unlink(path);
}

/*
 * 在处理 SYNC_FILTER_REQUEST 请求时，检查某个给定的候选请求是否与给定的标签
 * 匹配。会对所有待处理请求调用本函数，以判断是否需要忘记它们。
 */
bool
mdfiletagmatches(const FileTag *ftag, const FileTag *candidate)
{
	/*
	 * 目前我们仅在丢弃数据库时，把过滤请求用作一种丢弃所有与该数据库相关的
	 * 已调度回调的方式。对于所有与来自 SYNC_FILTER_REQUEST 请求的 ftag 具有相同
	 * 数据库 OID 的候选，我们都返回 true，从而将它们遗忘。
	 */
	return ftag->rlocator.dbOid == candidate->rlocator.dbOid;
}

/*
 * mdstartreadv() 的 AIO 完成回调。
 */
static PgAioResult
md_readv_complete(PgAioHandle *ioh, PgAioResult prior_result, uint8 cb_data)
{
	PgAioTargetData *td = pgaio_io_get_target_data(ioh);
	PgAioResult result = prior_result;

	if (prior_result.result < 0)
	{
		result.status = PGAIO_RS_ERROR;
		result.id = PGAIO_HCB_MD_READV;
		/* 对于“硬”错误，把错误号记录在 error_data 中 */
		result.error_data = -prior_result.result;
		result.result = 0;

		/*
		 * 立即就 IO 错误记录一条消息，但只写入服务器日志。之所以要立即这样做，
		 * 是因为 IO 的发起方可能不会立即处理查询结果（因为它正忙于查询处理的
		 * 其他部分），或者根本不处理（例如，如果它因另一个也失败的 IO 而被取消
		 * 或报错）。IO 的定义方在处理 IO 的结果时会发出一个 ERROR
		 */
		pgaio_result_report(result, td, LOG_SERVER_ONLY);

		return result;
	}

	/*
	 * 如 smgrstartreadv() 上方所述，smgr API 操作的是块的层面，而不是字节。
	 * 进行转换。
	 */
	result.result /= BLCKSZ;

	Assert(result.result <= td->smgr.nblocks);

	if (result.result == 0)
	{
		/* 将读取 0 个块视为失败 */
		result.status = PGAIO_RS_ERROR;
		result.id = PGAIO_HCB_MD_READV;
		result.error_data = 0;

		/* 见“硬错误”情形上方的注释 */
		pgaio_result_report(result, td, LOG_SERVER_ONLY);

		return result;
	}

	if (result.status != PGAIO_RS_ERROR &&
		result.result < td->smgr.nblocks)
	{
		/* 部分读取应在上层重试 */
		result.status = PGAIO_RS_PARTIAL;
		result.id = PGAIO_HCB_MD_READV;
	}

	return result;
}

/*
 * mdstartreadv() 的 AIO 错误报告回调。
 *
 * 错误编码方式如下：
 * - PgAioResult.error_data != 0 表示以该 errno 失败的 I/O
 * - PgAioResult.error_data == 0 表示未读取所有数据的 I/O
 */
static void
md_readv_report(PgAioResult result, const PgAioTargetData *td, int elevel)
{
	RelPathStr	path;

	path = relpathbackend(td->smgr.rlocator,
						  td->smgr.is_temp ? MyProcNumber : INVALID_PROC_NUMBER,
						  td->smgr.forkNum);

	if (result.error_data != 0)
	{
		/* 供 errcode_for_file_access() 与 %m 使用 */
		errno = result.error_data;

		ereport(elevel,
				errcode_for_file_access(),
				errmsg("could not read blocks %u..%u in file \"%s\": %m",
					   td->smgr.blockNum,
					   td->smgr.blockNum + td->smgr.nblocks - 1,
					   path.str));
	}
	else
	{
		/*
		 * 注意：这通常只在重试一个部分 IO 时输出到调试消息中。
		 */
		ereport(elevel,
				errcode(ERRCODE_DATA_CORRUPTED),
				errmsg("could not read blocks %u..%u in file \"%s\": read only %zu of %zu bytes",
					   td->smgr.blockNum,
					   td->smgr.blockNum + td->smgr.nblocks - 1,
					   path.str,
					   result.result * (size_t) BLCKSZ,
					   td->smgr.nblocks * (size_t) BLCKSZ));
	}
}
