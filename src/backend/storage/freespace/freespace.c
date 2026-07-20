/*-------------------------------------------------------------------------
 *
 * freespace.c
 *	  POSTGRES 空闲空间映射，用于在关系中快速查找空闲空间
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/freespace/freespace.c
 *
 *
 * 说明：
 *
 *	空闲空间映射（FSM）跟踪各页面上的空闲空间量，并允许快速搜索
 *	具有足够空闲空间的页面。FSM 存储在所有堆表及那些需要它的
 *	索引访问方法的专用关系分支中（另见 indexfsm.c）。
 *	更多信息请参见 README。
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/xloginsert.h"
#include "access/xlogutils.h"
#include "miscadmin.h"
#include "storage/freespace.h"
#include "storage/fsm_internals.h"
#include "storage/smgr.h"
#include "utils/rel.h"


/*
 * 我们仅使用一个字节来存储页面上的空闲空间量，因此我们将页面可能拥有的
 * 空闲空间量划分为 256 个不同类别。最高类别 255 表示页面至少有
 * MaxFSMRequestSize 字节的空闲空间，次高类别表示从
 * 254 * FSM_CAT_STEP（含）到 MaxFSMRequestSize（不含）的范围。
 *
 * MaxFSMRequestSize 依赖于体系架构和 BLCKSZ，但假设默认 8k BLCKSZ，
 * 且 MaxFSMRequestSize 为 8164 字节，则类别如下所示：
 *
 *
 * 范围	       类别
 * 0	- 31   0
 * 32	- 63   1
 * ...    ...  ...
 * 8096 - 8127 253
 * 8128 - 8163 254
 * 8164 - 8192 255
 *
 * MaxFSMRequestSize 特殊的原因是：如果 MaxFSMRequestSize 不等于某个范围边界，
 * 那么恰好有 MaxFSMRequestSize 字节空闲空间的页面将无法满足
 * MaxFSMRequestSize 字节的请求。如果在一个完全空白的页面上都没有超过
 * MaxFSMRequestSize 字节的空闲空间，那就意味着我们永远无法满足
 * 恰好 MaxFSMRequestSize 字节的请求。
 */
#define FSM_CATEGORIES	256
#define FSM_CAT_STEP	(BLCKSZ / FSM_CATEGORIES)
#define MaxFSMRequestSize	MaxHeapTupleSize

/*
 * 磁盘上树的深度。我们需要能够寻址 2^32-1 个块，
 * 而 1626 是满足 X^3 >= 2^32-1 的最小数字。同样地，
 * 256 是满足 X^4 >= 2^32-1 的最小数字。实际上，
 * 这意味着 4096 字节是我们可以使用 3 层树的最小 BLCKSZ，
 * 而 512 是我们支持的最小值。
 */
#define FSM_TREE_DEPTH	((SlotsPerFSMPage >= 1626) ? 3 : 4)

#define FSM_ROOT_LEVEL	(FSM_TREE_DEPTH - 1)
#define FSM_BOTTOM_LEVEL 0

/*
 * 内部 FSM 例程使用逻辑寻址方案。树的每一层
 * 可以被视为一个可单独寻址的文件。
 */
typedef struct
{
	int			level;			/* 层级 */
	int			logpageno;		/* 该层级内的页号 */
} FSMAddress;

/* 根页的地址。 */
static const FSMAddress FSM_ROOT_ADDRESS = {FSM_ROOT_LEVEL, 0};

/* 在树中导航的函数 */
static FSMAddress fsm_get_child(FSMAddress parent, uint16 slot);
static FSMAddress fsm_get_parent(FSMAddress child, uint16 *slot);
static FSMAddress fsm_get_location(BlockNumber heapblk, uint16 *slot);
static BlockNumber fsm_get_heap_blk(FSMAddress addr, uint16 slot);
static BlockNumber fsm_logical_to_physical(FSMAddress addr);

static Buffer fsm_readbuf(Relation rel, FSMAddress addr, bool extend);
static Buffer fsm_extend(Relation rel, BlockNumber fsm_nblocks);

/* 将空闲空间量转换为 FSM 类别的函数 */
static uint8 fsm_space_avail_to_cat(Size avail);
static uint8 fsm_space_needed_to_cat(Size needed);
static Size fsm_space_cat_to_avail(uint8 cat);

/* 各项操作的核心函数 */
static int	fsm_set_and_search(Relation rel, FSMAddress addr, uint16 slot,
							   uint8 newValue, uint8 minValue);
static BlockNumber fsm_search(Relation rel, uint8 min_cat);
static uint8 fsm_vacuum_page(Relation rel, FSMAddress addr,
							 BlockNumber start, BlockNumber end,
							 bool *eof_p);
static bool fsm_does_block_exist(Relation rel, BlockNumber blknumber);


/******** 公共 API ********/

/*
 * GetPageWithFreeSpace - 尝试在给定关系中找到一个至少具有指定空闲空间量的页面。
 *
 * 如果成功，返回块号；如果不成功，返回 InvalidBlockNumber。
 *
 * 调用方必须准备好应对返回的页面在获取锁时实际可用空间不足的可能性。
 * 在这种情况下，调用方应报告该页面上实际可用的空闲空间量，
 * 然后重试（参见 RecordAndGetPageWithFreeSpace）。
 * 如果返回 InvalidBlockNumber，则扩展关系。
 *
 * 如果任何 FSM 条目指向超出关系末尾的块，此函数可能会触发 FSM 更新。
 */
BlockNumber
GetPageWithFreeSpace(Relation rel, Size spaceNeeded)
{
	uint8		min_cat = fsm_space_needed_to_cat(spaceNeeded);

	return fsm_search(rel, min_cat);
}

/*
 * RecordAndGetPageWithFreeSpace - 更新页面信息并重试。
 *
 * 我们提供这个组合形式，以节省相比分别调用 RecordPageWithFreeSpace +
 * GetPageWithFreeSpace 的锁开销。此外还会尽量返回靠近旧页面的页面；
 * 如果在旧页面所在的同一 FSM 页面上有足够空闲空间的页面，则优先使用。
 */
BlockNumber
RecordAndGetPageWithFreeSpace(Relation rel, BlockNumber oldPage,
							  Size oldSpaceAvail, Size spaceNeeded)
{
	int			old_cat = fsm_space_avail_to_cat(oldSpaceAvail);
	int			search_cat = fsm_space_needed_to_cat(spaceNeeded);
	FSMAddress	addr;
	uint16		slot;
	int			search_slot;

	/* 获取表示该堆块的 FSM 字节的位置 */
	addr = fsm_get_location(oldPage, &slot);

	search_slot = fsm_set_and_search(rel, addr, slot, old_cat, search_cat);

	/*
	 * 如果 fsm_set_and_search 找到了合适的新块，返回它。
	 * 否则，按常规方式搜索。
	 */
	if (search_slot != -1)
	{
		BlockNumber blknum = fsm_get_heap_blk(addr, search_slot);

		/*
		 * 检查该 blknum 是否确实在关系中。如果是，直接返回；
		 * 如果不是，不要尝试更新 FSM，回退到其他情况。
		 */
		if (fsm_does_block_exist(rel, blknum))
			return blknum;
	}
	return fsm_search(rel, search_cat);
}

/*
 * RecordPageWithFreeSpace - 更新页面信息。
 *
 * 注意，如果新的 spaceAvail 值高于 FSM 中存储的旧值，
 * 该空间可能在下次 FreeSpaceMapVacuum 调用（它会更新上层页面）之前
 * 不会对搜索者可见。
 */
void
RecordPageWithFreeSpace(Relation rel, BlockNumber heapBlk, Size spaceAvail)
{
	int			new_cat = fsm_space_avail_to_cat(spaceAvail);
	FSMAddress	addr;
	uint16		slot;

	/* 获取表示该堆块的 FSM 字节的位置 */
	addr = fsm_get_location(heapBlk, &slot);

	fsm_set_and_search(rel, addr, slot, new_cat, 0);
}

/*
 * XLogRecordPageWithFreeSpace - 类似 RecordPageWithFreeSpace，但用于 WAL 回放
 */
void
XLogRecordPageWithFreeSpace(RelFileLocator rlocator, BlockNumber heapBlk,
							Size spaceAvail)
{
	int			new_cat = fsm_space_avail_to_cat(spaceAvail);
	FSMAddress	addr;
	uint16		slot;
	BlockNumber blkno;
	Buffer		buf;
	Page		page;

	/* 获取表示该堆块的 FSM 字节的位置 */
	addr = fsm_get_location(heapBlk, &slot);
	blkno = fsm_logical_to_physical(addr);

	/* 如果页面尚不存在，则扩展 */
	buf = XLogReadBufferExtended(rlocator, FSM_FORKNUM, blkno,
								 RBM_ZERO_ON_ERROR, InvalidBuffer);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

	page = BufferGetPage(buf);
	if (PageIsNew(page))
		PageInit(page, BLCKSZ, 0);

	/*
	 * 对 FSM 的更改通常使用 MarkBufferDirtyHint 来标记为已修改；
	 * 但在恢复期间，如果启用了校验和，它什么也不做。
	 * 这里假设在恢复期间修改提示位时不应脏化页面，以防止撕裂页，
	 * 因为此时无法生成新的 WAL 数据来存储 FPI。
	 * 这对 FSM 来说并不相关，因为当校验和不匹配时，它的块会被清零。
	 * 因此，我们需要在这里使用常规的 MarkBufferDirty 来标记 FSM 块
	 * 在恢复期间已修改，否则对 FSM 的更改可能会丢失。
	 */
	if (fsm_set_avail(page, slot, new_cat))
		MarkBufferDirty(buf);
	UnlockReleaseBuffer(buf);
}

/*
 * GetRecordedFreeSpace - 根据 FSM 返回特定页面上的空闲空间量。
 */
Size
GetRecordedFreeSpace(Relation rel, BlockNumber heapBlk)
{
	FSMAddress	addr;
	uint16		slot;
	Buffer		buf;
	uint8		cat;

	/* 获取表示该堆块的 FSM 字节的位置 */
	addr = fsm_get_location(heapBlk, &slot);

	buf = fsm_readbuf(rel, addr, false);
	if (!BufferIsValid(buf))
		return 0;
	cat = fsm_get_avail(BufferGetPage(buf), slot);
	ReleaseBuffer(buf);

	return fsm_space_cat_to_avail(cat);
}

/*
 * FreeSpaceMapPrepareTruncateRel - 为关系截断做准备。
 *
 * nblocks 是堆的新大小。
 *
 * 返回新 FSM 的块数。
 * 如果为 InvalidBlockNumber，则无需截断；
 * 否则调用方负责调用 smgrtruncate() 来截断 FSM 页面，
 * 并调用 FreeSpaceMapVacuumRange() 来更新 FSM 中的上层页面。
 */
BlockNumber
FreeSpaceMapPrepareTruncateRel(Relation rel, BlockNumber nblocks)
{
	BlockNumber new_nfsmblocks;
	FSMAddress	first_removed_address;
	uint16		first_removed_slot;
	Buffer		buf;

	/*
	 * 如果此关系尚未创建 FSM，则无需截断。
	 */
	if (!smgrexists(RelationGetSmgr(rel), FSM_FORKNUM))
		return InvalidBlockNumber;

	/* 获取 FSM 中第一个被移除的堆块的位置 */
	first_removed_address = fsm_get_location(nblocks, &first_removed_slot);

	/*
	 * 将最后一个剩余的 FSM 页面的尾部清零。如果表示第一个被移除堆块的
	 * 槽位正好位于页面边界上（即作为 first_removed_address 所指向 FSM
	 * 页面的第一个槽位），我们可以直接截断该页面。
	 */
	if (first_removed_slot > 0)
	{
		buf = fsm_readbuf(rel, first_removed_address, false);
		if (!BufferIsValid(buf))
			return InvalidBlockNumber;	/* 无需操作；FSM 已经更小了 */

		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

		/* 从此处到更改被日志记录之前，禁止 EREPORT(ERROR) */
		START_CRIT_SECTION();

		fsm_truncate_avail(BufferGetPage(buf), first_removed_slot);

		/*
		 * 此更改非关键，因为 fsm_does_block_exist() 会阻止我们返回
		 * 已被截断的块。但是，由于这可能移除多达 SlotsPerFSMPage 个槽位，
		 * 避免那么多 fsm_does_block_exist() 拒绝的开销总是好的。
		 * 使用完整的 MarkBufferDirty()，而非 MarkBufferDirtyHint()。
		 */
		MarkBufferDirty(buf);

		/*
		 * 像 MarkBufferDirtyHint() 那样记录 WAL，只是为了在这方面
		 * 与文件其余部分保持一致。这是可选的；参见 README 中关于
		 * 完整页映像的说明。XXX 考虑使用 XLogSaveBufferForHint()
		 * 以获得更接近的相似性。
		 *
		 * 更高层的操作在 WAL 回放时调用我们。如果我们在
		 * XLOG_SMGR_TRUNCATE 刷新到磁盘之前崩溃，主分支长度尚未改变，
		 * 我们的分支仍然有效。如果我们在该刷新之后崩溃，
		 * 重做（redo）将回到这里。
		 */
		if (!InRecovery && RelationNeedsWAL(rel) && XLogHintBitIsNeeded())
			log_newpage_buffer(buf, false);

		END_CRIT_SECTION();

		UnlockReleaseBuffer(buf);

		new_nfsmblocks = fsm_logical_to_physical(first_removed_address) + 1;
	}
	else
	{
		new_nfsmblocks = fsm_logical_to_physical(first_removed_address);
		if (smgrnblocks(RelationGetSmgr(rel), FSM_FORKNUM) <= new_nfsmblocks)
			return InvalidBlockNumber;	/* 无需操作；FSM 已经更小了 */
	}

	return new_nfsmblocks;
}

/*
 * FreeSpaceMapVacuum - 更新关系 FSM 中的上层页面
 *
 * 我们假设底层页面已经用新的空闲空间信息更新过。
 */
void
FreeSpaceMapVacuum(Relation rel)
{
	bool		dummy;

	/* 从根节点开始递归扫描树 */
	(void) fsm_vacuum_page(rel, FSM_ROOT_ADDRESS,
						   (BlockNumber) 0, InvalidBlockNumber,
						   &dummy);
}

/*
 * FreeSpaceMapVacuumRange - 更新关系 FSM 中的上层页面
 *
 * 与上面类似，但假设只有介于 start 和 end-1（含）之间的堆页面
 * 具有新的空闲空间信息，因此只更新覆盖该块范围的上层槽位。
 * end == InvalidBlockNumber 等价于"关系的其余所有部分"。
 */
void
FreeSpaceMapVacuumRange(Relation rel, BlockNumber start, BlockNumber end)
{
	bool		dummy;

	/* 从根节点开始递归扫描树 */
	if (end > start)
		(void) fsm_vacuum_page(rel, FSM_ROOT_ADDRESS, start, end, &dummy);
}

/******** 内部例程 ********/

/*
 * 返回对应 x 字节空闲空间的类别
 */
static uint8
fsm_space_avail_to_cat(Size avail)
{
	int			cat;

	Assert(avail < BLCKSZ);

	if (avail >= MaxFSMRequestSize)
		return 255;

	cat = avail / FSM_CAT_STEP;

	/*
	 * 最高类别 255 保留用于 MaxFSMRequestSize 字节或更多。
	 */
	if (cat > 254)
		cat = 254;

	return (uint8) cat;
}

/*
 * 返回给定类别所表示的空闲空间范围的下限。
 */
static Size
fsm_space_cat_to_avail(uint8 cat)
{
	/* 最高类别精确表示 MaxFSMRequestSize 字节。 */
	if (cat == 255)
		return MaxFSMRequestSize;
	else
		return cat * FSM_CAT_STEP;
}

/*
 * 一个页面需要属于哪个类别才能容纳 x 字节的数据？
 * fsm_space_avail_to_cat() 是向下取整，而这里需要向上取整。
 */
static uint8
fsm_space_needed_to_cat(Size needed)
{
	int			cat;

	/* 不能请求超过最高类别所表示的空间 */
	if (needed > MaxFSMRequestSize)
		elog(ERROR, "invalid FSM request size %zu", needed);

	if (needed == 0)
		return 1;

	cat = (needed + FSM_CAT_STEP - 1) / FSM_CAT_STEP;

	if (cat > 255)
		cat = 255;

	return (uint8) cat;
}

/*
 * 返回 FSM 页面的物理块号
 */
static BlockNumber
fsm_logical_to_physical(FSMAddress addr)
{
	BlockNumber pages;
	int			leafno;
	int			l;

	/*
	 * 计算给定页面下方第一个叶子页面的逻辑页号。
	 */
	leafno = addr.logpageno;
	for (l = 0; l < addr.level; l++)
		leafno *= SlotsPerFSMPage;

	/* 计算寻址该叶子页面所需的上层节点数 */
	pages = 0;
	for (l = 0; l < FSM_TREE_DEPTH; l++)
	{
		pages += leafno + 1;
		leafno /= SlotsPerFSMPage;
	}

	/*
	 * 如果我们被请求的页面不在底层，减去上面多算的
	 * 较低层级的页面。
	 */
	pages -= addr.level;

	/* 将页计数转换为从 0 开始的块号 */
	return pages - 1;
}

/*
 * 返回与给定堆块对应的 FSM 位置。
 */
static FSMAddress
fsm_get_location(BlockNumber heapblk, uint16 *slot)
{
	FSMAddress	addr;

	addr.level = FSM_BOTTOM_LEVEL;
	addr.logpageno = heapblk / SlotsPerFSMPage;
	*slot = heapblk % SlotsPerFSMPage;

	return addr;
}

/*
 * 返回与 FSM 中给定位置对应的堆块号。
 */
static BlockNumber
fsm_get_heap_blk(FSMAddress addr, uint16 slot)
{
	Assert(addr.level == FSM_BOTTOM_LEVEL);
	return ((unsigned int) addr.logpageno) * SlotsPerFSMPage + slot;
}

/*
 * 给定子页面的逻辑地址，获取父页面的逻辑页号，
 * 以及父页面中对应于该子页面的槽位。
 */
static FSMAddress
fsm_get_parent(FSMAddress child, uint16 *slot)
{
	FSMAddress	parent;

	Assert(child.level < FSM_ROOT_LEVEL);

	parent.level = child.level + 1;
	parent.logpageno = child.logpageno / SlotsPerFSMPage;
	*slot = child.logpageno % SlotsPerFSMPage;

	return parent;
}

/*
 * 给定父页面的逻辑地址和一个槽号，
 * 获取对应子页面的逻辑地址。
 */
static FSMAddress
fsm_get_child(FSMAddress parent, uint16 slot)
{
	FSMAddress	child;

	Assert(parent.level > FSM_BOTTOM_LEVEL);

	child.level = parent.level - 1;
	child.logpageno = parent.logpageno * SlotsPerFSMPage + slot;

	return child;
}

/*
 * 读取 FSM 页面。
 *
 * 如果页面不存在，返回 InvalidBuffer；如果 'extend' 为 true，
 * 则扩展 FSM 文件。
 */
static Buffer
fsm_readbuf(Relation rel, FSMAddress addr, bool extend)
{
	BlockNumber blkno = fsm_logical_to_physical(addr);
	Buffer		buf;
	SMgrRelation reln = RelationGetSmgr(rel);

	/*
	 * 如果我们尚未缓存 FSM 的大小，先检查一下。
	 * 如果请求的块似乎已超出末尾，也重新检查，因为我们的缓存值可能已过时。
	 * （我们在截断时会发送 smgr 失效消息，但扩展时不会。）
	 */
	if (reln->smgr_cached_nblocks[FSM_FORKNUM] == InvalidBlockNumber ||
		blkno >= reln->smgr_cached_nblocks[FSM_FORKNUM])
	{
		/* 使缓存失效，以便 smgrnblocks 向内核查询。 */
		reln->smgr_cached_nblocks[FSM_FORKNUM] = InvalidBlockNumber;
		if (smgrexists(reln, FSM_FORKNUM))
			smgrnblocks(reln, FSM_FORKNUM);
		else
			reln->smgr_cached_nblocks[FSM_FORKNUM] = 0;
	}

	/*
	 * 对于读取，我们使用 ZERO_ON_ERROR 模式，并在必要时初始化页面。
	 * FSM 信息无论如何都不精确，因此清除损坏页比重接报错更好。
	 * 由于 FSM 更改不记录 WAL，崩溃时的所谓撕裂页问题
	 * 可能导致页面头部损坏，例如。
	 *
	 * 我们在下面的路径中使用同样的方式在扩展关系时初始化页面，
	 * 因为并发扩展可能导致 vm_extend() 返回一个已经初始化的页面。
	 */
	if (blkno >= reln->smgr_cached_nblocks[FSM_FORKNUM])
	{
		if (extend)
			buf = fsm_extend(rel, blkno + 1);
		else
			return InvalidBuffer;
	}
	else
		buf = ReadBufferExtended(rel, FSM_FORKNUM, blkno, RBM_ZERO_ON_ERROR, NULL);

	/*
	 * 在需要时初始化页面比看起来更棘手，因为存在多个后端并发执行此操作的
	 * 可能性，以及我们希望避免在页面正常时无谓地获取缓冲区锁。
	 * 我们必须获取锁来初始化页面，因此在获得锁后重新检查页面是否为新建，
	 * 以防其他人已经完成了初始化。此外，由于我们最初在没有锁的情况下
	 * 检查 PageIsNew，可能出现在其他人仍在初始化页面时我们直接返回缓冲区
	 * 的情况（即，我们可能看到 pd_upper 已设置，但其他页面头部字段仍为零）。
	 * 这对于自身会获取缓冲区锁的调用方来说是无害的，但某些调用方
	 * 会在没有任何锁的情况下检查页面。后者只有在不依赖页面头部具有
	 * 正确内容的情况下才是安全的。当前用法是安全的，因为
	 * PageGetContents() 不需要页面头部正确。
	 */
	if (PageIsNew(BufferGetPage(buf)))
	{
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		if (PageIsNew(BufferGetPage(buf)))
			PageInit(BufferGetPage(buf), BLCKSZ, 0);
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
	}
	return buf;
}

/*
 * 确保 FSM 分支至少为 fsm_nblocks 长，
 * 必要时用空页面进行扩展。这里的"空"指的是页面全部填充为零，
 * 意味着没有空闲空间。
 */
static Buffer
fsm_extend(Relation rel, BlockNumber fsm_nblocks)
{
	return ExtendBufferedRelTo(BMR_REL(rel), FSM_FORKNUM, NULL,
							   EB_CREATE_FORK_IF_NEEDED |
							   EB_CLEAR_SIZE_CACHE,
							   fsm_nblocks,
							   RBM_ZERO_ON_ERROR);
}

/*
 * 在给定的 FSM 页面和槽位中设置值。
 *
 * 如果 minValue > 0，还会在更新后的页面中搜索至少具有 minValue 空闲空间的页面。
 * 如果找到一个，返回其槽号，否则返回 -1。
 */
static int
fsm_set_and_search(Relation rel, FSMAddress addr, uint16 slot,
				   uint8 newValue, uint8 minValue)
{
	Buffer		buf;
	Page		page;
	int			newslot = -1;

	buf = fsm_readbuf(rel, addr, true);
	LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

	page = BufferGetPage(buf);

	if (fsm_set_avail(page, slot, newValue))
		MarkBufferDirtyHint(buf, false);

	if (minValue != 0)
	{
		/* 在我们仍持有锁时进行搜索 */
		newslot = fsm_search_avail(buf, minValue,
								   addr.level == FSM_BOTTOM_LEVEL,
								   true);
	}

	UnlockReleaseBuffer(buf);

	return newslot;
}

/*
 * 在树中搜索至少具有 min_cat 空闲空间的堆页面
 */
static BlockNumber
fsm_search(Relation rel, uint8 min_cat)
{
	int			restarts = 0;
	FSMAddress	addr = FSM_ROOT_ADDRESS;

	for (;;)
	{
		int			slot;
		Buffer		buf;
		uint8		max_avail = 0;

		/* 读取 FSM 页面。 */
		buf = fsm_readbuf(rel, addr, false);

		/* 在页面内搜索 */
		if (BufferIsValid(buf))
		{
			LockBuffer(buf, BUFFER_LOCK_SHARE);
			slot = fsm_search_avail(buf, min_cat,
									(addr.level == FSM_BOTTOM_LEVEL),
									false);
			if (slot == -1)
			{
				max_avail = fsm_get_max_avail(BufferGetPage(buf));
				UnlockReleaseBuffer(buf);
			}
			else
			{
				/* 保留 pin 以便下面的可能更新 */
				LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			}
		}
		else
			slot = -1;

		if (slot != -1)
		{
			/*
			 * 沿树下移，如果已在底层则返回找到的块。
			 */
			if (addr.level == FSM_BOTTOM_LEVEL)
			{
				BlockNumber blkno = fsm_get_heap_blk(addr, slot);
				Page		page;

				if (fsm_does_block_exist(rel, blkno))
				{
					ReleaseBuffer(buf);
					return blkno;
				}

				/*
				 * 块已超出关系末尾。更新 FSM，并从根节点重新开始。
				 * 常用的 "advancenext" 行为在这种罕见场景下是最糟糕的，
				 * 因为之后的每个槽位都会以同样的方式不可用。
				 * 我们可以将同一 FSM 页面上所有受影响的槽位清零，
				 * 但不要指望这种优化的好处能证明其编译代码量的合理性。
				 */
				page = BufferGetPage(buf);
				LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
				fsm_set_avail(page, slot, 0);
				MarkBufferDirtyHint(buf, false);
				UnlockReleaseBuffer(buf);
				if (restarts++ > 10000) /* 与下方的理由相同 */
					return InvalidBlockNumber;
				addr = FSM_ROOT_ADDRESS;
			}
			else
			{
				ReleaseBuffer(buf);
			}
			addr = fsm_get_child(addr, slot);
		}
		else if (addr.level == FSM_ROOT_LEVEL)
		{
			/*
			 * 在根节点失败意味着 FSM 中没有具有足够空闲空间的页面。放弃。
			 */
			return InvalidBlockNumber;
		}
		else
		{
			uint16		parentslot;
			FSMAddress	parent;

			/*
			 * 在较低层级，失败可能是因为上层节点中的值未反映下层页面的值。
			 * 更新上层节点以避免再次陷入同样的陷阱，然后重新开始。
			 *
			 * 这里存在竞态条件：如果另一个后端在我们释放它之后立即更新
			 * 此页面，并在我们之前获取父页面的锁，我们将用现在已经过时的
			 * 信息更新父页面。这没有问题，因为这种情况很少发生，
			 * 且将在下一次 vacuum 中被修复。
			 */
			parent = fsm_get_parent(addr, &parentslot);
			fsm_set_and_search(rel, parent, parentslot, max_avail, 0);

			/*
			 * 如果上层页面严重过时，我们可能需要循环相当多的次数，
			 * 在过程中更新它们。任何不一致最终都应得到纠正，循环应结束。
			 * 尽管如此，无限循环是可怕的，因此提供一个紧急出口。
			 */
			if (restarts++ > 10000)
				return InvalidBlockNumber;

			/* 从根节点重新开始搜索 */
			addr = FSM_ROOT_ADDRESS;
		}
	}
}


/*
 * FreeSpaceMapVacuum 的递归核心
 *
 * 检查 addr 指示的 FSM 页面及其子页面，
 * 更新覆盖堆块范围从 start 到 end-1 的上层节点。
 * （如果 end 超出映射的实际末尾也没关系。）
 * 返回此页面上的最大空闲空间值。
 *
 * 如果 addr 超出 FSM 的末尾，将 *eof_p 设为 true 并返回 0。
 *
 * 这以深度优先顺序遍历树。树在物理上按深度优先顺序存储，
 * 因此这应该具有相当高的 I/O 效率。
 */
static uint8
fsm_vacuum_page(Relation rel, FSMAddress addr,
				BlockNumber start, BlockNumber end,
				bool *eof_p)
{
	Buffer		buf;
	Page		page;
	uint8		max_avail;

	/* 读取页面（如果存在），否则返回 EOF */
	buf = fsm_readbuf(rel, addr, false);
	if (!BufferIsValid(buf))
	{
		*eof_p = true;
		return 0;
	}
	else
		*eof_p = false;

	page = BufferGetPage(buf);

	/*
	 * 如果我们在底层之上，递归进入子页面，
	 * 并修正此层存储的子页面信息。
	 */
	if (addr.level > FSM_BOTTOM_LEVEL)
	{
		FSMAddress	fsm_start,
					fsm_end;
		uint16		fsm_start_slot,
					fsm_end_slot;
		int			slot,
					start_slot,
					end_slot;
		bool		eof = false;

		/*
		 * 根据请求的堆块范围，计算我们需要在此页面上更新的槽位范围。
		 * 第一个要更新的槽位是覆盖 "start" 块的槽位，
		 * 最后一个槽位是覆盖 "end - 1" 的槽位。
		 * （其中一些工作会在每次递归调用中重复，
		 * 但成本很低，不值得担心。）
		 */
		fsm_start = fsm_get_location(start, &fsm_start_slot);
		fsm_end = fsm_get_location(end - 1, &fsm_end_slot);

		while (fsm_start.level < addr.level)
		{
			fsm_start = fsm_get_parent(fsm_start, &fsm_start_slot);
			fsm_end = fsm_get_parent(fsm_end, &fsm_end_slot);
		}
		Assert(fsm_start.level == addr.level);

		if (fsm_start.logpageno == addr.logpageno)
			start_slot = fsm_start_slot;
		else if (fsm_start.logpageno > addr.logpageno)
			start_slot = SlotsPerFSMPage;	/* 不应该走到这里... */
		else
			start_slot = 0;

		if (fsm_end.logpageno == addr.logpageno)
			end_slot = fsm_end_slot;
		else if (fsm_end.logpageno > addr.logpageno)
			end_slot = SlotsPerFSMPage - 1;
		else
			end_slot = -1;		/* 不应该走到这里... */

		for (slot = start_slot; slot <= end_slot; slot++)
		{
			int			child_avail;

			CHECK_FOR_INTERRUPTS();

			/* 遇到文件末尾后，直接将剩余槽位清零 */
			if (!eof)
				child_avail = fsm_vacuum_page(rel, fsm_get_child(addr, slot),
											  start, end,
											  &eof);
			else
				child_avail = 0;

			/* 更新子页面信息 */
			if (fsm_get_avail(page, slot) != child_avail)
			{
				LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
				fsm_set_avail(page, slot, child_avail);
				MarkBufferDirtyHint(buf, false);
				LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			}
		}
	}

	/* 现在获取页面上的最大值以返回给调用方 */
	max_avail = fsm_get_max_avail(page);

	/*
	 * 重置下一个槽位指针。这鼓励使用低编号的页面，
	 * 增加后续 vacuum 可以截断关系的可能性。
	 * 我们在此处不费心获取锁，也不标记页面为脏（如果之前不脏），
	 * 因为这只是一个提示。
	 */
	((FSMPage) PageGetContents(page))->fp_next_slot = 0;

	ReleaseBuffer(buf);

	return max_avail;
}


/*
 * 检查块号是否超出关系的末尾。这在 WAL 回放后可能发生，
 * 如果 FSM 已写入磁盘，但它引用的新扩展页面还没有。
 */
static bool
fsm_does_block_exist(Relation rel, BlockNumber blknumber)
{
	SMgrRelation smgr = RelationGetSmgr(rel);

	/*
	 * 如果低于缓存的 nblocks，该块肯定存在。否则，我们面临一个权衡。
	 * 我们选择与最新的 nblocks 比较，这会产生 lseek() 开销。
	 * 另一种方案是假设该块不存在，但这会导致 FSM 为主分支扩展刚刚记录的块
	 * 将可用空间设为零。
	 */
	return ((BlockNumberIsValid(smgr->smgr_cached_nblocks[MAIN_FORKNUM]) &&
			 blknumber < smgr->smgr_cached_nblocks[MAIN_FORKNUM]) ||
			blknumber < RelationGetNumberOfBlocks(rel));
}
