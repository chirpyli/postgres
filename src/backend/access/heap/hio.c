/*-------------------------------------------------------------------------
 *
 * hio.c
 *	  POSTGRES 堆访问方法输入/输出代码。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/heap/hio.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "access/hio.h"
#include "access/htup_details.h"
#include "access/visibilitymap.h"
#include "storage/bufmgr.h"
#include "storage/freespace.h"
#include "storage/lmgr.h"


/*
 * RelationPutHeapTuple - 将元组放置在指定页面
 *
 * !!! 此函数内禁止 EREPORT(ERROR)!!!  失败时必须 PANIC！！！
 *
 * 注意 - 调用者必须持有该缓冲区的 BUFFER_LOCK_EXCLUSIVE 锁。
 */
void
RelationPutHeapTuple(Relation relation,
					 Buffer buffer,
					 HeapTuple tuple,
					 bool token)
{
	Page		pageHeader;
	OffsetNumber offnum;

	/*
	 * 正在被推测性插入的元组应当已经设置好了它的 token。
	 */
	Assert(!token || HeapTupleHeaderIsSpeculative(tuple->t_data));

	/*
	 * 不允许将带有无效提示位组合的元组放置到页面上。contrib/amcheck 的逻辑
	 * 会将此组合检测为损坏，因此如果你禁用了此断言，请在那里做出相应的修改。
	 */
	Assert(!((tuple->t_data->t_infomask & HEAP_XMAX_COMMITTED) &&
			 (tuple->t_data->t_infomask & HEAP_XMAX_IS_MULTI)));

	/* 将元组添加到页面 */
	pageHeader = BufferGetPage(buffer);

	offnum = PageAddItem(pageHeader, (Item) tuple->t_data,
						 tuple->t_len, InvalidOffsetNumber, false, true);

	if (offnum == InvalidOffsetNumber)
		elog(PANIC, "failed to add tuple to page");

	/* 将 tuple->t_self 更新为它实际存储的位置 */
	ItemPointerSet(&(tuple->t_self), BufferGetBlockNumber(buffer), offnum);

	/*
	 * 也要将正确的位置插入到已存储元组的 CTID 中（除非这是一个推测性插入，
	 * 这种情况下 token 保存在 CTID 字段中）。
	 */
	if (!token)
	{
		ItemId		itemId = PageGetItemId(pageHeader, offnum);
		HeapTupleHeader item = (HeapTupleHeader) PageGetItem(pageHeader, itemId);

		item->t_ctid = tuple->t_self;
	}
}

/*
 * 以给定模式读入一个缓冲区，如果 bistate 不为 NULL 则使用批量插入策略。
 */
static Buffer
ReadBufferBI(Relation relation, BlockNumber targetBlock,
			 ReadBufferMode mode, BulkInsertState bistate)
{
	Buffer		buffer;

	/* 如果不是批量插入，则与 ReadBuffer 完全相同 */
	if (!bistate)
		return ReadBufferExtended(relation, MAIN_FORKNUM, targetBlock,
								  mode, NULL);

	/* 如果所需的块已经处于 pin 状态，重新 pin 并返回它 */
	if (bistate->current_buf != InvalidBuffer)
	{
		if (BufferGetBlockNumber(bistate->current_buf) == targetBlock)
		{
			/*
			 * 当前 LOCK 变体仅用于扩展关系，永远不应该到达这个分支。
			 */
			Assert(mode != RBM_ZERO_AND_LOCK &&
				   mode != RBM_ZERO_AND_CLEANUP_LOCK);

			IncrBufferRefCount(bistate->current_buf);
			return bistate->current_buf;
		}
		/* ... 否则丢弃旧缓冲区 */
		ReleaseBuffer(bistate->current_buf);
		bistate->current_buf = InvalidBuffer;
	}

	/* 使用缓冲区策略执行读取 */
	buffer = ReadBufferExtended(relation, MAIN_FORKNUM, targetBlock,
								mode, bistate->strategy);

	/* 将选定的块保存为将来插入的目标 */
	IncrBufferRefCount(buffer);
	bistate->current_buf = buffer;

	return buffer;
}

/*
 * 对于每一个全可见的堆页面，如果尚未获取，则在相应的可见性映射页上
 * 获取一个 pin。
 *
 * 为避免调用者中的复杂性，如果只涉及一个缓冲区，则 buffer1 或 buffer2
 * 可能为 InvalidBuffer。出于同样的原因，block2 可能小于 block1。
 *
 * 返回是否临时释放了缓冲区锁。
 */
static bool
GetVisibilityMapPins(Relation relation, Buffer buffer1, Buffer buffer2,
					 BlockNumber block1, BlockNumber block2,
					 Buffer *vmbuffer1, Buffer *vmbuffer2)
{
	bool		need_to_pin_buffer1;
	bool		need_to_pin_buffer2;
	bool		released_locks = false;

	/*
	 * 交换缓冲区以处理单个块/缓冲区的情况，并用于处理如果锁排序规则
	 * 要求先锁定 block2 的情况。
	 */
	if (!BufferIsValid(buffer1) ||
		(BufferIsValid(buffer2) && block1 > block2))
	{
		Buffer		tmpbuf = buffer1;
		Buffer	   *tmpvmbuf = vmbuffer1;
		BlockNumber tmpblock = block1;

		buffer1 = buffer2;
		vmbuffer1 = vmbuffer2;
		block1 = block2;

		buffer2 = tmpbuf;
		vmbuffer2 = tmpvmbuf;
		block2 = tmpblock;
	}

	Assert(BufferIsValid(buffer1));
	Assert(buffer2 == InvalidBuffer || block1 <= block2);

	while (1)
	{
		/* 找出我们需要但尚未获取的 pin。 */
		need_to_pin_buffer1 = PageIsAllVisible(BufferGetPage(buffer1))
			&& !visibilitymap_pin_ok(block1, *vmbuffer1);
		need_to_pin_buffer2 = buffer2 != InvalidBuffer
			&& PageIsAllVisible(BufferGetPage(buffer2))
			&& !visibilitymap_pin_ok(block2, *vmbuffer2);
		if (!need_to_pin_buffer1 && !need_to_pin_buffer2)
			break;

		/* 在进行任何 I/O 之前，我们必须先解锁两个缓冲区。 */
		released_locks = true;
		LockBuffer(buffer1, BUFFER_LOCK_UNLOCK);
		if (buffer2 != InvalidBuffer && buffer2 != buffer1)
			LockBuffer(buffer2, BUFFER_LOCK_UNLOCK);

		/* 获取 pin。 */
		if (need_to_pin_buffer1)
			visibilitymap_pin(relation, block1, vmbuffer1);
		if (need_to_pin_buffer2)
			visibilitymap_pin(relation, block2, vmbuffer2);

		/* 重新锁定缓冲区。 */
		LockBuffer(buffer1, BUFFER_LOCK_EXCLUSIVE);
		if (buffer2 != InvalidBuffer && buffer2 != buffer1)
			LockBuffer(buffer2, BUFFER_LOCK_EXCLUSIVE);

		/*
		 * 如果涉及两个缓冲区且我们只 pin 了其中一个，那么在我们忙于
		 * pin 第一个缓冲区时，第二个缓冲区有可能变成了全可见的。
		 * 如果看起来是这种情况，我们需要再循环一次来处理。
		 */
		if (buffer2 == InvalidBuffer || buffer1 == buffer2
			|| (need_to_pin_buffer1 && need_to_pin_buffer2))
			break;
	}

	return released_locks;
}

/*
 * 扩展关系。如果有利，则扩展多个页面。
 *
 * 如果调用者需要多个页面（num_pages > 1），我们总是尝试至少扩展那么多页面。
 *
 * 如果扩展锁上存在争用，我们不仅为自己扩展，还会尝试帮助他人。我们可以通过
 * 将空页面加入 FSM 来实现这一点。通常，当我们无法使用 FSM 时，不存在争用。
 *
 * 我们确实需要将扩展的页面数量限制在某个值，因为所有被扩展页面的缓冲区
 * 都需要临时被 pin。目前我们将 MAX_BUFFERS_TO_EXTEND_BY 定义为 64 个缓冲区，
 * 更高的数值似乎难以看到收益。这部分是因为 copyfrom.c 的
 * MAX_BUFFERED_TUPLES / MAX_BUFFERED_BYTES 阻止了更大的 multi_insert。
 *
 * 返回一个新扩展块的缓冲区。如果可能，该缓冲区以独占锁方式返回。
 * *did_unlock 设置为 true 表示锁不得不被释放，否则为 false。
 *
 *
 * XXX: 对于某些工作负载，更激进地扩展可能是有益的，例如使用基于关系
 * 大小的启发式方法。
 */
static Buffer
RelationAddBlocks(Relation relation, BulkInsertState bistate,
				  int num_pages, bool use_fsm, bool *did_unlock)
{
#define MAX_BUFFERS_TO_EXTEND_BY 64
	Buffer		victim_buffers[MAX_BUFFERS_TO_EXTEND_BY];
	BlockNumber first_block = InvalidBlockNumber;
	BlockNumber last_block = InvalidBlockNumber;
	uint32		extend_by_pages;
	uint32		not_in_fsm_pages;
	Buffer		buffer;
	Page		page;

	/*
	 * 确定尝试扩展多少个页面。
	 */
	if (bistate == NULL && !use_fsm)
	{
		/*
		 * 如果我们既没有 bistate，也无法使用 FSM，我们就无法进行批量
		 * 扩展 - 将没有找到额外页面的方法。
		 */
		extend_by_pages = 1;
	}
	else
	{
		uint32		waitcount;

		/*
		 * 尝试至少按调用者需要的页面数进行扩展。我们可以记住额外的
		 * 页面（通过 FSM 或 bistate）。
		 */
		extend_by_pages = num_pages;

		if (!RELATION_IS_LOCAL(relation))
			waitcount = RelationExtensionLockWaiterCount(relation);
		else
			waitcount = 0;

		/*
		 * 将扩展的页面数乘以等待者的数量。即使我们没有使用 FSM 也要
		 * 这样做，因为它仍然可以通过推迟此后端下一次需要扩展的时间来
		 * 缓解争用。在这种情况下，扩展的页面将通过 bistate->next_free
		 * 找到。
		 */
		extend_by_pages += extend_by_pages * waitcount;

		/* ---
		 * 如果我们之前使用相同的 bistate 进行过扩展，很可能我们还会
		 * 继续扩展。尝试按之前那么多页面进行扩展。这对于性能可能很
		 * 重要，原因包括：
		 *
		 * - 它可以防止 mdzeroextend() 在以不同方式扩展关系之间切换，
		 *   这对于某些文件系统来说效率低下。
		 *
		 * - 争用往往是间歇性的。即使我们当前没有看到其他等待者（见
		 *   上文），以更大的数量扩展可以防止未来的争用。
		 * ---
		 */
		if (bistate)
			extend_by_pages = Max(extend_by_pages, bistate->already_extended_by);

		/*
		 * 不能超过 MAX_BUFFERS_TO_EXTEND_BY，我们需要并发地 pin 它们
		 * 全部。
		 */
		extend_by_pages = Min(extend_by_pages, MAX_BUFFERS_TO_EXTEND_BY);
	}

	/*
	 * 应该将多少扩展页面录入 FSM？
	 *
	 * 如果我们有 bistate，只将我们自己不需要的页面录入 FSM。否则，
	 * 其他每个后端都会立即尝试使用此后端自己需要的页面，导致不必要
	 * 的争用。如果我们没有 bistate，我们就无法避免使用 FSM。
	 *
	 * 永远不要将返回的页面录入 FSM，我们会立即使用它。
	 */
	if (num_pages > 1 && bistate == NULL)
		not_in_fsm_pages = 1;
	else
		not_in_fsm_pages = num_pages;

	/* 准备将另一个缓冲区放入 bistate */
	if (bistate && bistate->current_buf != InvalidBuffer)
	{
		ReleaseBuffer(bistate->current_buf);
		bistate->current_buf = InvalidBuffer;
	}

	/*
	 * 扩展关系。我们要求第一个返回的页面被锁定，以便确保没有人
	 * 并发地插入到该页面中。
	 *
	 * 在当前的 MAX_BUFFERS_TO_EXTEND_BY 下，不存在 [auto]vacuum
	 * 尝试截断后续页面的危险，因为 REL_TRUNCATE_MINIMUM 要大得多。
	 */
	first_block = ExtendBufferedRelBy(BMR_REL(relation), MAIN_FORKNUM,
									  bistate ? bistate->strategy : NULL,
									  EB_LOCK_FIRST,
									  extend_by_pages,
									  victim_buffers,
									  &extend_by_pages);
	buffer = victim_buffers[0]; /* 该函数将返回的缓冲区 */
	last_block = first_block + (extend_by_pages - 1);
	Assert(first_block == BufferGetBlockNumber(buffer));

	/*
	 * 关系现在已扩展。初始化页面。我们在这里做这件事，在可能释放
	 * 页面锁之前，因为它允许我们双重检查页面内容是否为空（这应该
	 * 永远不会发生，但如果发生了，我们不想冒险清除有效数据）。
	 */
	page = BufferGetPage(buffer);
	if (!PageIsNew(page))
		elog(ERROR, "page %u of relation \"%s\" should be empty but is not",
			 first_block,
			 RelationGetRelationName(relation));

	PageInit(page, BufferGetPageSize(buffer), 0);
	MarkBufferDirty(buffer);

	/*
	 * 如果我们决定将页面放入 FSM，则释放缓冲区锁（但不释放 pin），
	 * 我们不想在持有缓冲区锁时进行 IO。这将需要在调用者中进行
	 * 更进一步的检查。
	 */
	if (use_fsm && not_in_fsm_pages < extend_by_pages)
	{
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		*did_unlock = true;
	}
	else
		*did_unlock = false;

	/*
	 * 关系现在已扩展。释放所有缓冲区的 pin，除了第一个（我们将返回
	 * 它）。如果我们决定将页面放入 FSM，我们可以将其作为同一个循环
	 * 的一部分来完成。
	 */
	for (uint32 i = 1; i < extend_by_pages; i++)
	{
		BlockNumber curBlock = first_block + i;

		Assert(curBlock == BufferGetBlockNumber(victim_buffers[i]));
		Assert(BlockNumberIsValid(curBlock));

		ReleaseBuffer(victim_buffers[i]);

		if (use_fsm && i >= not_in_fsm_pages)
		{
			Size		freespace = BufferGetPageSize(victim_buffers[i]) -
				SizeOfPageHeaderData;

			RecordPageWithFreeSpace(relation, curBlock, freespace);
		}
	}

	if (use_fsm && not_in_fsm_pages < extend_by_pages)
	{
		BlockNumber first_fsm_block = first_block + not_in_fsm_pages;

		FreeSpaceMapVacuumRange(relation, first_fsm_block, last_block);
	}

	if (bistate)
	{
		/*
		 * 记住我们扩展的额外页面，以便我们以后可以在不查看 FSM 的
		 * 情况下使用它们。
		 */
		if (extend_by_pages > 1)
		{
			bistate->next_free = first_block + 1;
			bistate->last_free = last_block;
		}
		else
		{
			bistate->next_free = InvalidBlockNumber;
			bistate->last_free = InvalidBlockNumber;
		}

		/* 维护 bistate->current_buf */
		IncrBufferRefCount(buffer);
		bistate->current_buf = buffer;
		bistate->already_extended_by += extend_by_pages;
	}

	return buffer;
#undef MAX_BUFFERS_TO_EXTEND_BY
}

/*
 * RelationGetBufferForTuple
 *
 *	返回给定关系中一个页面的已 pin 且独占锁定的缓冲区，
 *	其空闲空间 >= 给定的 len。
 *
 *	如果 num_pages > 1，当我们决定扩展关系时，我们将尝试至少扩展那么多
 *	页面。这对于知道自己将需要多个页面的调用者（例如 heap_multi_insert()）
 *	来说效率更高。
 *
 *	如果 otherBuffer 不是 InvalidBuffer，则它引用同一关系中另一个页面的
 *	先前已 pin 的缓冲区；返回时，该缓冲区也将被独占锁定。（这种情况由
 *	heap_update 使用；otherBuffer 包含正在被更新的元组。）
 *
 *	传递 otherBuffer 的原因是，如果两个后端正在执行并发的 heap_update
 *	操作，如果它们试图以相反的顺序锁定相同的两个缓冲区，则可能发生
 *	死锁。为确保这不会发生，我们强制执行这样的规则：关系的缓冲区必须
 *	按页面号递增的顺序锁定。最方便的做法是让 RelationGetBufferForTuple
 *	以适当的顺序小心地锁定它们两个。
 *
 *	注意：otherBuffer 与我们为新元组插入而选择的缓冲区相同，这不太可能，
 *	但并非完全不可能（这只有在 heap_update 发现该页面空间不足后，该页面
 *	中释放了空间时才会发生）。在这种情况下，页面只会被 pin 并锁定一次。
 *
 *	我们还处理以下可能性：全可见标志可能需要在一个或两个页面上被清除。
 *	如果是这样，必须在获取缓冲区锁之前获取关联可见性映射页上的 pin，
 *	以避免在持有缓冲区锁时可能进行 I/O。这些 pin 使用输入输出参数
 *	vmbuffer 和 vmbuffer_other 传递回给调用者。请注意，在某些情况下，
 *	调用者可能已经获取了这样的 pin，这由这些参数在进入时不为 InvalidBuffer
 *	来指示。
 *
 *	我们通常使 FSM 来帮助我们查找空闲空间。但是，如果指定了
 *	HEAP_INSERT_SKIP_FSM，我们只需在元组无法放入当前目标页面时，将一个
 *	新的空页面追加到关系的末尾。当我们知道关系是新的且不包含有用的
 *	空闲空间量时，这可以节省一些周期。
 *
 *	HEAP_INSERT_SKIP_FSM 对于关系的非 WAL 日志记录添加也很有用，如果
 *	调用者持有独占锁并小心地在第一次插入之前使关系的 smgr_targblock
 *	失效 --- 这确保所有插入都发生在新添加的页面中，而不会与来自其他
 *	事务的元组混合。这样，崩溃就不会冒丢失任何其他事务已提交数据
 *	的风险。（有关安全使用此行为所需的额外约束，请参见 heap_insert
 *	的注释。）
 *
 *	调用者还可以提供 BulkInsertState 对象来优化对同一关系的多次插入。
 *	这会保持在当前插入目标页面上的 pin（以节省 pin/unpin 周期），并将
 *	一个 BULKWRITE 缓冲区选择策略对象传递给缓冲区管理器。为 bistate
 *	传递 NULL 选择默认行为。
 *
 *	我们不会将现有页面填充超过 fillfactor，除了近乎空页面中的大元组。
 *	这是可以的，因为在更新元组并使其保持在同一个页面上时不会咨询此
 *	例程，而这正是 fillfactor 旨在为其保留空间的场景。
 *
 *	此处允许 ereport(ERROR)，因此必须在对缓冲区池进行任何（未记录的）
 *	更改之前调用此例程。
 */
Buffer
RelationGetBufferForTuple(Relation relation, Size len,
						  Buffer otherBuffer, int options,
						  BulkInsertState bistate,
						  Buffer *vmbuffer, Buffer *vmbuffer_other,
						  int num_pages)
{
	bool		use_fsm = !(options & HEAP_INSERT_SKIP_FSM);
	Buffer		buffer = InvalidBuffer;
	Page		page;
	Size		nearlyEmptyFreeSpace,
				pageFreeSpace = 0,
				saveFreeSpace = 0,
				targetFreeSpace = 0;
	BlockNumber targetBlock,
				otherBlock;
	bool		unlockedTargetBuffer;
	bool		recheckVmPins;

	len = MAXALIGN(len);		/* 保守起见 */

	/* 如果调用者不知道要扩展多少个页面，则扩展 1 个 */
	if (num_pages <= 0)
		num_pages = 1;

	/* 批量插入不支持更新，只支持插入。 */
	Assert(otherBuffer == InvalidBuffer || !bistate);

	/*
	 * 如果我们将因超大元组而失败，立即失败
	 */
	if (len > MaxHeapTupleSize)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("row is too big: size %zu, maximum size %zu",
						len, MaxHeapTupleSize)));

	/* 计算由于 fillfactor 选项所需的额外空闲空间 */
	saveFreeSpace = RelationGetTargetPageFreeSpace(relation,
												   HEAP_DEFAULT_FILLFACTOR);

	/*
	 * 由于没有元组的页面仍然可以有行指针，我们将页面视为 "空" 当
	 * 不可用空间很小时。这个阈值是有些随意的，但它应该能在向低
	 * fillfactor 表中插入大元组时防止大多数不必要的关关系扩展。
	 */
	nearlyEmptyFreeSpace = MaxHeapTupleSize -
		(MaxHeapTuplesPerPage / 8 * sizeof(ItemIdData));
	if (len + saveFreeSpace > nearlyEmptyFreeSpace)
		targetFreeSpace = Max(len, nearlyEmptyFreeSpace);
	else
		targetFreeSpace = len + saveFreeSpace;

	if (otherBuffer != InvalidBuffer)
		otherBlock = BufferGetBlockNumber(otherBuffer);
	else
		otherBlock = InvalidBlockNumber;	/* 仅为让编译器安静 */

	/*
	 * 我们首先尝试将元组放在上一次插入元组的同一页面上，该页面缓存于
	 * BulkInsertState 或 relcache 条目中。如果那不起作用，我们会请求
	 * 可用空间映射来定位一个合适的页面。由于 FSM 的信息可能已过时，
	 * 我们必须准备好循环并重试多次。（为确保这不是一个无限循环，我们
	 * 必须用每个被证明不合适的页面上的正确空闲空间量来更新 FSM。）
	 * 如果 FSM 没有记录具有足够空闲空间的页面，我们放弃并扩展关系。
	 *
	 * 当 use_fsm 为 false 时，我们要么将元组放在现有目标页面上，要么
	 * 扩展关系。
	 */
	if (bistate && bistate->current_buf != InvalidBuffer)
		targetBlock = BufferGetBlockNumber(bistate->current_buf);
	else
		targetBlock = RelationGetTargetBlock(relation);

	if (targetBlock == InvalidBlockNumber && use_fsm)
	{
		/*
		 * 我们没有缓存的目标页面，因此向 FSM 请求一个初始目标。
		 */
		targetBlock = GetPageWithFreeSpace(relation, targetFreeSpace);
	}

	/*
	 * 如果 FSM 对该关系一无所知，在我们放弃并扩展之前尝试最后一个页面。
	 * 这可以避免在引导期间或最近启动的系统中出现每页一个元组的问题。
	 */
	if (targetBlock == InvalidBlockNumber)
	{
		BlockNumber nblocks = RelationGetNumberOfBlocks(relation);

		if (nblocks > 0)
			targetBlock = nblocks - 1;
	}

loop:
	while (targetBlock != InvalidBlockNumber)
	{
		/*
		 * 读取并以独占方式锁定目标块，以及另一个块（如果给定了的话），
		 * 适当注意锁排序以及它们是同一个块的可能性。
		 *
		 * 如果页面级全可见标志被设置，调用者将需要清除该标志以及
		 * 相应的可见性映射位。但是，到我们返回时，我们将已经 x 锁定了
		 * 缓冲区，而我们不想在那种状态下进行任何 I/O。所以我们在此处、
		 * 在获取锁之前检查该位，并在看起来必要时 pin 该页面。在没有
		 * 锁的情况下检查存在得到错误答案的风险，因此我们将不得不在
		 * 获取锁之后重新检查。
		 */
		if (otherBuffer == InvalidBuffer)
		{
			/* 简单情况 */
			buffer = ReadBufferBI(relation, targetBlock, RBM_NORMAL, bistate);
			if (PageIsAllVisible(BufferGetPage(buffer)))
				visibilitymap_pin(relation, targetBlock, vmbuffer);

			/*
			 * 如果页面为空，pin vmbuffer 以便稍后设置 all_frozen 位。
			 */
			if ((options & HEAP_INSERT_FROZEN) &&
				(PageGetMaxOffsetNumber(BufferGetPage(buffer)) == 0))
				visibilitymap_pin(relation, targetBlock, vmbuffer);

			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		}
		else if (otherBlock == targetBlock)
		{
			/* 同样简单的情况 */
			buffer = otherBuffer;
			if (PageIsAllVisible(BufferGetPage(buffer)))
				visibilitymap_pin(relation, targetBlock, vmbuffer);
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		}
		else if (otherBlock < targetBlock)
		{
			/* 先锁定另一个缓冲区 */
			buffer = ReadBuffer(relation, targetBlock);
			if (PageIsAllVisible(BufferGetPage(buffer)))
				visibilitymap_pin(relation, targetBlock, vmbuffer);
			LockBuffer(otherBuffer, BUFFER_LOCK_EXCLUSIVE);
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		}
		else
		{
			/* 先锁定目标缓冲区 */
			buffer = ReadBuffer(relation, targetBlock);
			if (PageIsAllVisible(BufferGetPage(buffer)))
				visibilitymap_pin(relation, targetBlock, vmbuffer);
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
			LockBuffer(otherBuffer, BUFFER_LOCK_EXCLUSIVE);
		}

		/*
		 * 我们现在已经将目标页面（以及另一个缓冲区，如果有的话）pin 并
		 * 锁定。然而，由于我们最初的 PageIsAllVisible 检查是在获取锁之前
		 * 执行的，结果现在可能已经过时，无论是对于选定的牺牲缓冲区，
		 * 还是对于调用者传递的另一个缓冲区。在这种情况下，我们将需要
		 * 放弃我们的锁，去获取我们之前未能获取的 pin，然后重新锁定。
		 * 这相当麻烦，但希望不会经常发生。
		 *
		 * 注意，有一个小可能性是我们上面没有 pin 该页面，但仍然已经
		 * 正确地 pin 了该页面，要么是因为我们已经通过此循环进行了
		 * 先前的一遍，要么是因为调用者无论如何都将正确的页面传递给了
		 * 我们。
		 *
		 * 还要注意，有可能当我们获取 pin 并重新获取缓冲区锁时，可见性
		 * 映射位已经被其他后端清除了。在这种情况下，我们将已经做了一些
		 * 额外的工作而没有收益，但并没有造成真正的危害。
		 */
		GetVisibilityMapPins(relation, buffer, otherBuffer,
							 targetBlock, otherBlock, vmbuffer,
							 vmbuffer_other);

		/*
		 * 现在我们可以检查这里是否有足够的空闲空间。如果有，我们就
		 * 完成了。
		 */
		page = BufferGetPage(buffer);

		/*
		 * 如有必要初始化页面，它将很快被使用。我们可以避免在这里弄脏
		 * 缓冲区，并依赖调用者在其将元组放入页面时这样做，但这样做
		 * 似乎没有太大好处。
		 */
		if (PageIsNew(page))
		{
			PageInit(page, BufferGetPageSize(buffer), 0);
			MarkBufferDirty(buffer);
		}

		pageFreeSpace = PageGetHeapFreeSpace(page);
		if (targetFreeSpace <= pageFreeSpace)
		{
			/* 也使用此页面作为将来的插入目标 */
			RelationSetTargetBlock(relation, targetBlock);
			return buffer;
		}

		/*
		 * 空间不足，因此我们必须放弃我们的页面锁和 pin（如果有），并
		 * 准备在别处查找。我们不在乎以什么顺序解锁两个缓冲区，所以这
		 * 可以比上面的代码稍微简单一些。
		 */
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		if (otherBuffer == InvalidBuffer)
			ReleaseBuffer(buffer);
		else if (otherBlock != targetBlock)
		{
			LockBuffer(otherBuffer, BUFFER_LOCK_UNLOCK);
			ReleaseBuffer(buffer);
		}

		/* 是否正在进行批量扩展？ */
		if (bistate && bistate->next_free != InvalidBlockNumber)
		{
			Assert(bistate->next_free <= bistate->last_free);

			/*
			 * 我们之前批量扩展了关系，并且该扩展还有一些未使用的页面，
			 * 因此我们无需到 FSM 中去查找新页面。但要记录最后一个页面
			 * 的空闲空间，某人以后可能会插入更窄的元组。
			 */
			if (use_fsm)
				RecordPageWithFreeSpace(relation, targetBlock, pageFreeSpace);

			targetBlock = bistate->next_free;
			if (bistate->next_free >= bistate->last_free)
			{
				bistate->next_free = InvalidBlockNumber;
				bistate->last_free = InvalidBlockNumber;
			}
			else
				bistate->next_free++;
		}
		else if (!use_fsm)
		{
			/* 没有 FSM 时，总是跳出循环并扩展 */
			break;
		}
		else
		{
			/*
			 * 向 FSM 更新此页面的状况，并请求另一个页面进行尝试。
			 */
			targetBlock = RecordAndGetPageWithFreeSpace(relation,
														targetBlock,
														pageFreeSpace,
														targetFreeSpace);
		}
	}

	/* 必须扩展关系 */
	buffer = RelationAddBlocks(relation, bistate, num_pages, use_fsm,
							   &unlockedTargetBuffer);

	targetBlock = BufferGetBlockNumber(buffer);
	page = BufferGetPage(buffer);

	/*
	 * 页面为空，pin vmbuffer 以设置 all_frozen 位。我们不想在缓冲区被
	 * 锁定时进行 IO，因此如果需要 IO（需要下面的检查），我们先解锁页面。
	 */
	if (options & HEAP_INSERT_FROZEN)
	{
		Assert(PageGetMaxOffsetNumber(page) == 0);

		if (!visibilitymap_pin_ok(targetBlock, *vmbuffer))
		{
			if (!unlockedTargetBuffer)
				LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
			unlockedTargetBuffer = true;
			visibilitymap_pin(relation, targetBlock, vmbuffer);
		}
	}

	/*
	 * 如有必要，重新获取锁。
	 *
	 * 如果目标缓冲区在上面被解锁，或者在下面重新获取 otherBuffer 锁时被
	 * 解锁，这不太可能，但有可能，另一个后端在此页面上使用了空间。我们
	 * 在下面检查这一点，并在必要时重试。
	 */
	recheckVmPins = false;
	if (unlockedTargetBuffer)
	{
		/* 在上面释放了目标缓冲区的锁 */
		if (otherBuffer != InvalidBuffer)
			LockBuffer(otherBuffer, BUFFER_LOCK_EXCLUSIVE);
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		recheckVmPins = true;
	}
	else if (otherBuffer != InvalidBuffer)
	{
		/*
		 * 我们没有释放目标缓冲区，并且 otherBuffer 有效，需要锁定另一个
		 * 缓冲区。可以保证它的页面号比新页面低。为了符合死锁预防规则，
		 * 我们应当先锁定 otherBuffer，但那样会给予其他后端在我们的页面
		 * 上放置元组的机会。为了降低这种可能性，尝试以条件方式锁定另一个
		 * 缓冲区，这非常有可能会成功。
		 *
		 * 或者，我们可以在扩展关系之前获取 otherBuffer 上的锁，但这需要
		 * 在执行 IO 时持有锁，这似乎比不太可能重试更糟糕。
		 */
		Assert(otherBuffer != buffer);
		Assert(targetBlock > otherBlock);

		if (unlikely(!ConditionalLockBuffer(otherBuffer)))
		{
			unlockedTargetBuffer = true;
			LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
			LockBuffer(otherBuffer, BUFFER_LOCK_EXCLUSIVE);
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		}
		recheckVmPins = true;
	}

	/*
	 * 如果其中一个缓冲区被解锁（如果 otherBuffer 有效则总是这种情况），
	 * 一个全可见标志有可能被设置，虽然不太可能。我们可以使用
	 * GetVisibilityMapPins 来处理。GetVisibilityMapPins() 有可能需要临时
	 * 释放缓冲区锁，在这种情况下我们将需要在下面检查页面上是否仍有足够的
	 * 空间。
	 */
	if (recheckVmPins)
	{
		if (GetVisibilityMapPins(relation, otherBuffer, buffer,
								 otherBlock, targetBlock, vmbuffer_other,
								 vmbuffer))
			unlockedTargetBuffer = true;
	}

	/*
	 * 如果自关系扩展以来目标缓冲区被临时解锁，则页面上的所有空间有可能
	 * 已经被使用，虽然不太可能。如果是这样，我们只需从头重试。如果我们
	 * 没有解锁，那么如果没有足够的空间就出了问题 - 顶部的测试应该已经
	 * 防止到达这种情况。
	 */
	pageFreeSpace = PageGetHeapFreeSpace(page);
	if (len > pageFreeSpace)
	{
		if (unlockedTargetBuffer)
		{
			if (otherBuffer != InvalidBuffer)
				LockBuffer(otherBuffer, BUFFER_LOCK_UNLOCK);
			UnlockReleaseBuffer(buffer);

			goto loop;
		}
		elog(PANIC, "tuple is too big: size %zu", len);
	}

	/*
	 * 记住新页面作为我们将来插入的目标。
	 *
	 * XXX 我们应该立即将新页面录入空闲空间映射，还是只在短期内（直到
	 * VACUUM 看到它）为此后端的独占使用保留它？这似乎取决于您是否期望
	 * 当前后端进行更多插入，这在大多数时候可能是一个不错的赌注。所以
	 * 目前，不要将它加入 FSM。
	 */
	RelationSetTargetBlock(relation, targetBlock);

	return buffer;
}
