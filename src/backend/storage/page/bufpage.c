/*-------------------------------------------------------------------------
 *
 * bufpage.c
 *	  POSTGRES standard buffer page code.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/page/bufpage.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/itup.h"
#include "access/xlog.h"
#include "pgstat.h"
#include "storage/checksum.h"
#include "utils/memdebug.h"
#include "utils/memutils.h"


/* GUC 变量 */
bool		ignore_checksum_failure = false;


/* ----------------------------------------------------------------
 *						页面支持函数
 * ----------------------------------------------------------------
 */

/*
 * PageInit
 *		初始化一个页面的内容。
 *		注意，我们在此并不计算初始校验和；校验和要等到写入时才计算。
 */
void
PageInit(Page page, Size pageSize, Size specialSize)
{
	PageHeader	p = (PageHeader) page;

	specialSize = MAXALIGN(specialSize);

	Assert(pageSize == BLCKSZ);
	Assert(pageSize > specialSize + SizeOfPageHeaderData);

	/* 确保页面的所有字段以及未使用的空间都为零 */
	MemSet(p, 0, pageSize);

	p->pd_flags = 0;
	p->pd_lower = SizeOfPageHeaderData;
	p->pd_upper = pageSize - specialSize;
	p->pd_special = pageSize - specialSize;
	PageSetPageSizeAndVersion(page, pageSize, PG_PAGE_LAYOUT_VERSION);
	/* p->pd_prune_xid = InvalidTransactionId;		由上面的 MemSet 完成 */
}


/*
 * PageIsVerified
 *		检查页面头部与校验和（如果有）是否看起来有效。
 *
 * 当一个页面刚从磁盘读入时会调用本函数。其意图是在我们顺着伪造的行指针
 * 乱跑、测试无效的当事务标识符等之前，廉价地检测出被毁坏的页面。
 *
 * 事实证明，这里也有必要允许全零的页面。尽管在刻意给关系添加一个页面时
 * *不会*调用本例程，但仍存在可能在表中发现全零页面的场景。（例如：一个
 * 后端扩展了某个关系，然后在还没写出任何关于新页面的 WAL 记录时就崩溃了。
 * 内核在该文件中已经拥有这个全零页面，并且在重启后它依然保持那样。）因此
 * 我们在这里允许全零页面，并且小心地让页面访问宏把这样的页面视为空页面、
 * 没有空闲空间。最终，VACUUM 会清理这类页面并使其可用。
 *
 * 如果设置了标志 PIV_LOG_WARNING/PIV_LOG_LOG，则在发生校验和失败时会记录
 * 一条 WARNING/LOG 消息。
 *
 * 如果设置了标志 PIV_IGNORE_CHECKSUM_FAILURE，校验和失败会导致发出一条关于
 * 该失败的消息，但不会导致 PageIsVerified() 返回 false。
 *
 * 为了允许调用者报告关于校验和失败的统计信息，可以传入 *checksum_failure_p。
 * 注意，即便本函数返回 true，也可能存在校验和失败，原因是
 * PIV_IGNORE_CHECKSUM_FAILURE。
 */
bool
PageIsVerified(PageData *page, BlockNumber blkno, int flags, bool *checksum_failure_p)
{
	const PageHeaderData *p = (const PageHeaderData *) page;
	size_t	   *pagebytes;
	bool		checksum_failure = false;
	bool		header_sane = false;
	uint16		checksum = 0;

	if (checksum_failure_p)
		*checksum_failure_p = false;

	/*
	 * 除非页面通过了基本的非零测试，否则不验证页面数据
	 */
	if (!PageIsNew(page))
	{
		if (DataChecksumsEnabled())
		{
			checksum = pg_checksum_page(page, blkno);

			if (checksum != p->pd_checksum)
			{
				checksum_failure = true;
				if (checksum_failure_p)
					*checksum_failure_p = true;
			}
		}

		/*
		 * 以下检查并不能证明头部是正确的，只能说明它看起来足够正常、可以
		 * 放入缓冲池。该块在后续使用中仍可能暴露出问题，这正是我们提供
		 * 校验和选项的原因。
		 */
		if ((p->pd_flags & ~PD_VALID_FLAG_BITS) == 0 &&
			p->pd_lower <= p->pd_upper &&
			p->pd_upper <= p->pd_special &&
			p->pd_special <= BLCKSZ &&
			p->pd_special == MAXALIGN(p->pd_special))
			header_sane = true;

		if (header_sane && !checksum_failure)
			return true;
	}

	/* 检查全零的情况 */
	pagebytes = (size_t *) page;

	if (pg_memory_is_all_zeros(pagebytes, BLCKSZ))
		return true;

	/*
	 * 按照 PIV_LOG_* 的指示，在校验和失败时抛出 WARNING/LOG，但只能在
	 * 完成全零情况检查之后进行。
	 */
	if (checksum_failure)
	{
		if ((flags & (PIV_LOG_WARNING | PIV_LOG_LOG)) != 0)
			ereport(flags & PIV_LOG_WARNING ? WARNING : LOG,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("page verification failed, calculated checksum %u but expected %u",
							checksum, p->pd_checksum)));

		if (header_sane && (flags & PIV_IGNORE_CHECKSUM_FAILURE))
			return true;
	}

	return false;
}


/*
 *	PageAddItemExtended
 *
 *	向页面添加一个项。返回值是它插入处的偏移号；如果该项因任何原因未被
 *	插入，则返回 InvalidOffsetNumber。会发出一条 WARNING 以说明被拒绝的
 *	原因。
 *
 *	offsetNumber 必须为 InvalidOffsetNumber（表示寻找一个空闲的行指针），
 *	或者是介于 FirstOffsetNumber 与最后一个现存项之后一个之间的值（表示
 *	使用那个特定的行指针）。
 *
 *	如果 offsetNumber 有效且设置了标志 PAI_OVERWRITE，我们就把该项存储在
 *	指定的 offsetNumber 处，该位置必须是一个当前未使用的行指针，或者是
 *	最后一个现存项之后的一个位置。
 *
 *	如果 offsetNumber 有效但未设置标志 PAI_OVERWRITE，则在指定的 offsetNumber
 *	处插入该项，并把数组中现有的项向后移动以腾出空间。
 *
 *	如果 offsetNumber 无效，则通过寻找第一个既未使用又已释放的槽位来分配
 *	一个槽位。
 *
 *	如果设置了标志 PAI_IS_HEAP，我们会强制页面上的行指针数量不能超过
 *	MaxHeapTuplesPerPage。
 *
 *	!!! 这里禁止使用 EREPORT(ERROR) !!!
 */
OffsetNumber
PageAddItemExtended(Page page,
					Item item,
					Size size,
					OffsetNumber offsetNumber,
					int flags)
{
	PageHeader	phdr = (PageHeader) page;
	Size		alignedSize;
	int			lower;
	int			upper;
	ItemId		itemId;
	OffsetNumber limit;
	bool		needshuffle = false;

	/*
	 * 当心损坏的页面指针
	 */
	if (phdr->pd_lower < SizeOfPageHeaderData ||
		phdr->pd_lower > phdr->pd_upper ||
		phdr->pd_upper > phdr->pd_special ||
		phdr->pd_special > BLCKSZ)
		ereport(PANIC,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupted page pointers: lower = %u, upper = %u, special = %u",
						phdr->pd_lower, phdr->pd_upper, phdr->pd_special)));

	/*
	 * 选择放置新项的 offsetNumber
	 */
	limit = OffsetNumberNext(PageGetMaxOffsetNumber(page));

	/* 是否传入了 offsetNumber？ */
	if (OffsetNumberIsValid(offsetNumber))
	{
		/* 是，检查它 */
		if ((flags & PAI_OVERWRITE) != 0)
		{
			if (offsetNumber < limit)
			{
				itemId = PageGetItemId(page, offsetNumber);
				if (ItemIdIsUsed(itemId) || ItemIdHasStorage(itemId))
				{
					elog(WARNING, "will not overwrite a used ItemId");
					return InvalidOffsetNumber;
				}
			}
		}
		else
		{
			if (offsetNumber < limit)
				needshuffle = true; /* 需要移动现有的 linp */
		}
	}
	else
	{
		/* 没有传入 offsetNumber，因此寻找一个空闲槽位 */
		/* 如果没有空闲槽位，就放到 limit 处（第一个开放槽位） */
		if (PageHasFreeLinePointers(page))
		{
			/*
			 * 扫描行指针数组，定位一个"可回收"（未使用）的 ItemId。
			 *
			 * 总是优先使用更靠前的项。PageTruncateLinePointerArray 只能在
			 * 未使用的项作为连续的一组出现在行指针数组末尾时，才能将其
			 * 截断。
			 */
			for (offsetNumber = FirstOffsetNumber;
				 offsetNumber < limit;	/* limit 即 maxoff+1 */
				 offsetNumber++)
			{
				itemId = PageGetItemId(page, offsetNumber);

				/*
				 * 我们同时检查是否没有存储，只是出于谨慎；未使用的项永远
				 * 不应该有存储。也用 Assert() 来确认这一不变式得到遵守。
				 */
				Assert(ItemIdIsUsed(itemId) || !ItemIdHasStorage(itemId));

				if (!ItemIdIsUsed(itemId) && !ItemIdHasStorage(itemId))
					break;
			}
			if (offsetNumber >= limit)
			{
			/* 该提示是错误的，因此重置它 */
			PageClearHasFreeLinePointers(page);
			}
		}
		else
		{
			/* 如果提示说没有空闲槽位，就不必搜索了 */
			offsetNumber = limit;
		}
	}

	/* 拒绝把项放置到第一个未使用的行指针之后 */
	if (offsetNumber > limit)
	{
		elog(WARNING, "specified item offset is too large");
		return InvalidOffsetNumber;
	}

	/* 若是堆，则拒绝把项放置到堆边界之外 */
	if ((flags & PAI_IS_HEAP) != 0 && offsetNumber > MaxHeapTuplesPerPage)
	{
		elog(WARNING, "can't put more than MaxHeapTuplesPerPage items in a heap page");
		return InvalidOffsetNumber;
	}

	/*
	 * 计算页面新的 lower 和 upper 指针，看是否能放下。
	 *
	 * 注意：用有符号整数进行运算，以避免诸如 alignedSize > pd_upper 时
	 * 出错。
	 */
	if (offsetNumber == limit || needshuffle)
		lower = phdr->pd_lower + sizeof(ItemIdData);
	else
		lower = phdr->pd_lower;

	alignedSize = MAXALIGN(size);

	upper = (int) phdr->pd_upper - (int) alignedSize;

	if (lower > upper)
		return InvalidOffsetNumber;

	/*
	 * 可以插入该项了。首先，在需要时对现有的指针进行搬移。
	 */
	itemId = PageGetItemId(page, offsetNumber);

	if (needshuffle)
		memmove(itemId + 1, itemId,
				(limit - offsetNumber) * sizeof(ItemIdData));

	/* 设置行指针 */
	ItemIdSetNormal(itemId, upper, size);

	/*
	 * 项通常不包含未初始化的字节。核心的 bufpage 使用者都遵守这一点，但这
	 * 并非一条必需的编码规则；一个新的索引访问方法（AM）可以选择不遵守它。
	 * 不过，数据类型输入函数以及其他合成 datum 的 C 语言函数应当初始化所有
	 * 字节；datumIsEqual() 依赖于这一点。这里的检查，连同 printtup() 中类似的
	 * 检查，有助于捕获此类错误。
	 *
	 * 通过仅索引扫描（index-only scan）检索到的 "name" 类型的值可能含有未
	 * 初始化的字节；参见 btrescan() 中的注释。Valgrind 会将此报告为错误，
	 * 但忽略它是安全的。
	 */
	VALGRIND_CHECK_MEM_IS_DEFINED(item, size);

	/* 将该项的数据复制到页面上 */
	memcpy((char *) page + upper, item, size);

	/* 调整页面头部 */
	phdr->pd_lower = (LocationIndex) lower;
	phdr->pd_upper = (LocationIndex) upper;

	return offsetNumber;
}


/*
 * PageGetTempPage
 *		在本地内存中获取一个临时页面用于特殊处理。
 *		返回的页面完全未被初始化；调用者必须自行初始化。
 */
Page
PageGetTempPage(const PageData *page)
{
	Size		pageSize;
	Page		temp;

	pageSize = PageGetPageSize(page);
	temp = (Page) palloc(pageSize);

	return temp;
}

/*
 * PageGetTempPageCopy
 *		在本地内存中获取一个临时页面用于特殊处理。
 *		该页面通过复制给定页面的内容进行初始化。
 */
Page
PageGetTempPageCopy(const PageData *page)
{
	Size		pageSize;
	Page		temp;

	pageSize = PageGetPageSize(page);
	temp = (Page) palloc(pageSize);

	memcpy(temp, page, pageSize);

	return temp;
}

/*
 * PageGetTempPageCopySpecial
 *		在本地内存中获取一个临时页面用于特殊处理。
 *		该页面以与给定页面相同的 special space 大小进行 PageInit，并且
 *		special space 会从给定页面复制过来。
 */
Page
PageGetTempPageCopySpecial(const PageData *page)
{
	Size		pageSize;
	Page		temp;

	pageSize = PageGetPageSize(page);
	temp = (Page) palloc(pageSize);

	PageInit(temp, pageSize, PageGetSpecialSize(page));
	memcpy(PageGetSpecialPointer(temp),
		   PageGetSpecialPointer(page),
		   PageGetSpecialSize(page));

	return temp;
}

/*
 * PageRestoreTempPage
 *		在特殊处理之后，将临时页面复制回永久页面，并释放临时页面。
 */
void
PageRestoreTempPage(Page tempPage, Page oldPage)
{
	Size		pageSize;

	pageSize = PageGetPageSize(tempPage);
	memcpy(oldPage, tempPage, pageSize);

	pfree(tempPage);
}

/*
 * 为 PageRepairFragmentation 和 PageIndexMultiDelete 提供的元组碎片整理支持
 */
typedef struct itemIdCompactData
{
	uint16		offsetindex;	/* linp 数组索引 */
	int16		itemoff;		/* 项数据的页面偏移 */
	uint16		alignedlen;		/* MAXALIGN(项数据长度) */
} itemIdCompactData;
typedef itemIdCompactData *itemIdCompact;

/*
 * 在移除或标记部分行指针为未使用之后，移动元组以消除被移除项造成的空隙，
 * 并将它们重新排序回页面中的逆序行指针顺序。
 *
 * 这个函数经常会非常热（hot），因此采取一些措施使其尽可能最优是值得的。
 *
 * 如果 'itemidbase' 数组是按 itemoff 降序排列的，调用者可以把 'presorted'
 * 传为 true。当如此时，我们只需把元组 memmove() 向页面末尾方向移动即可。这是
 * 相当常见的情况，因为元组最初插入页面时就是这个顺序。当我们调用本函数
 * 来整理页面中的元组碎片时，页面上添加的任何新行指针都会保持该预排序顺序，
 * 因此对于经常更新的表，命中这种情况依然非常常见。
 *
 * 当 'itemidbase' 数组并非预排序时，我们就不能自由地随意 memmove() 元组。
 * 那样做可能会导致我们覆盖掉尚未移动的元组所属的内存。在这种情况下，我们
 * 把所有需要移动的元组复制到一个临时缓冲区中。然后我们只需从该临时缓冲区
 * memcpy() 回页面中的正确位置。元组以与 'itemidbase' 数组相同的顺序被复制回
 * 页面，因此我们最终把元组重新排序回逆序的行指针顺序。这会增加下一次命中
 * 预排序情况的几率。
 *
 * 调用者必须确保 nitems > 0
 */
static void
compactify_tuples(itemIdCompact itemidbase, int nitems, Page page, bool presorted)
{
	PageHeader	phdr = (PageHeader) page;
	Offset		upper;
	Offset		copy_tail;
	Offset		copy_head;
	itemIdCompact itemidptr;
	int			i;

	/* 如果 nitems == 0，下面的代码将无法正确工作 */
	Assert(nitems > 0);

	if (presorted)
	{

#ifdef USE_ASSERT_CHECKING
		{
			/*
			 * 验证我们没有遇到任何错误地传入 true presorted 值的新调用者。
			 */
			Offset		lastoff = phdr->pd_special;

			for (i = 0; i < nitems; i++)
			{
				itemidptr = &itemidbase[i];

				Assert(lastoff > itemidptr->itemoff);

				lastoff = itemidptr->itemoff;
			}
		}
#endif							/* USE_ASSERT_CHECKING */

		/*
		 * 'itemidbase' 已经处于最优顺序，即较矮的项指针具有更高的偏移。
		 * 这使我们能够将元组 memmove() 到页面末尾，而无需担心覆盖那些
		 * 尚未移动的元组。
		 *
		 * 很有可能会有一些元组已经恰好位于页面末尾，我们可以直接跳过它们，
		 * 因为它们在页面中已经处于正确的位置。我们先做这件事……
		 */
		upper = phdr->pd_special;
		i = 0;
		do
		{
			itemidptr = &itemidbase[i];
			if (upper != itemidptr->itemoff + itemidptr->alignedlen)
				break;
			upper -= itemidptr->alignedlen;

			i++;
		} while (i < nitems);

		/*
		 * 既然我们已经找到了第一个需要移动的元组，就可以进行元组紧凑化了。
		 * 我们尽量少做 memmove() 调用，并且只在出现空隙时才调用 memmove()。
		 * 当发现空隙时，我们就把空隙之后、直到上一次搬移点的所有元组都搬移
		 * 过去。
		 */
		copy_tail = copy_head = itemidptr->itemoff + itemidptr->alignedlen;
		for (; i < nitems; i++)
		{
			ItemId		lp;

			itemidptr = &itemidbase[i];
			lp = PageGetItemId(page, itemidptr->offsetindex + 1);

			if (copy_head != itemidptr->itemoff + itemidptr->alignedlen)
			{
				memmove((char *) page + upper,
						page + copy_head,
						copy_tail - copy_head);

				/*
				 * 我们现在已经搬移了所有已见过的元组，但还没搬移当前元组，
				 * 因此把 copy_tail 设为该元组的末尾，以便它在循环的下一次
				 * 迭代中被搬移。
				 */
				copy_tail = itemidptr->itemoff + itemidptr->alignedlen;
			}
			/* 把目标偏移向下移动本元组的长度 */
			upper -= itemidptr->alignedlen;
			/* 把 copy_head 指向本元组的起始位置 */
			copy_head = itemidptr->itemoff;

			/* 更新行指针以引用新的偏移 */
			lp->lp_off = upper;
		}

		/* 搬移剩余的元组。 */
		memmove((char *) page + upper,
				page + copy_head,
				copy_tail - copy_head);
	}
	else
	{
		PGAlignedBlock scratch;
		char	   *scratchptr = scratch.data;

		/*
		 * 非预排序情况：itemidbase 数组中的元组可能是任意顺序。因此，为了
		 * 把它们移动到页面末尾，我们必须先为每个需要移动的元组制作一份
		 * 临时副本，然后再把它们复制回页面中的新偏移处。
		 *
		 * 如果很大部分（>75%）的元组已被剪枝（pruned），我们就逐个元组地
		 * 把它们复制到临时缓冲区中；否则，我们就对所有需要移动的元组只做
		 * 一次 memcpy()。当如此多的元组被移除时，很可能会出现很多空隙，而且
		 * 页面末尾不太可能剩下很多不可移动的元组。
		 */
		if (nitems < PageGetMaxOffsetNumber(page) / 4)
		{
			i = 0;
			do
			{
				itemidptr = &itemidbase[i];
				memcpy(scratchptr + itemidptr->itemoff, page + itemidptr->itemoff,
					   itemidptr->alignedlen);
				i++;
			} while (i < nitems);

			/* 为下面的紧凑化代码做好准备 */
			i = 0;
			itemidptr = &itemidbase[0];
			upper = phdr->pd_special;
		}
		else
		{
			upper = phdr->pd_special;

			/*
			 * 很多元组很可能已经处于正确的位置。没有必要把它们复制到临时
			 * 缓冲区中。相反，我们只需在 itemidbase 数组中向前跳到确实需要
			 * 从中移动元组的位置，这样下面的代码就会让这些元组保持不动。
			 */
			i = 0;
			do
			{
				itemidptr = &itemidbase[i];
				if (upper != itemidptr->itemoff + itemidptr->alignedlen)
					break;
				upper -= itemidptr->alignedlen;

				i++;
			} while (i < nitems);

			/* 把所有需要移动的元组复制到临时缓冲区中 */
			memcpy(scratchptr + phdr->pd_upper,
				   page + phdr->pd_upper,
				   upper - phdr->pd_upper);
		}

		/*
		 * 进行元组紧凑化。itemidptr 已经指向我们要移动的第一个元组。在这里
		 * 我们把相邻元组的 memcpy 调用合并为一次调用。做法是将 memcpy 调用
		 * 推迟，直到我们发现需要闭合的空隙为止。
		 */
		copy_tail = copy_head = itemidptr->itemoff + itemidptr->alignedlen;
		for (; i < nitems; i++)
		{
			ItemId		lp;

			itemidptr = &itemidbase[i];
			lp = PageGetItemId(page, itemidptr->offsetindex + 1);

			/* 当检测到空隙时，复制待处理的元组 */
			if (copy_head != itemidptr->itemoff + itemidptr->alignedlen)
			{
				memcpy((char *) page + upper,
					   scratchptr + copy_head,
					   copy_tail - copy_head);

				/*
				 * 我们现在已经复制了所有已见过的元组，但还没复制当前元组，
				 * 因此把 copy_tail 设为该元组的末尾。
				 */
				copy_tail = itemidptr->itemoff + itemidptr->alignedlen;
			}
			/* 把目标偏移向下移动本元组的长度 */
			upper -= itemidptr->alignedlen;
			/* 把 copy_head 指向本元组的起始位置 */
			copy_head = itemidptr->itemoff;

			/* 更新行指针以引用新的偏移 */
			lp->lp_off = upper;
		}

		/* 复制剩余的块 */
		memcpy((char *) page + upper,
			   scratchptr + copy_head,
			   copy_tail - copy_head);
	}

	phdr->pd_upper = upper;
}

/*
 * PageRepairFragmentation
 *
 * 在剪枝（pruning）之后，释放堆页面上的碎片空间。
 *
 * 本例程仅可用于堆页面，但另请参见 PageIndexMultiDelete。
 *
 * 本例程会从行指针数组的末尾移除未使用的行指针。当只存于堆中的死元组（dead
 * heap-only tuples）被剪枝移除时，这是可能的，尤其是当之前存在每条链含有
 * 多个元组的 HOT 链时。
 *
 * 调用者最好持有页面缓冲区上的一个完整清理锁（cleanup lock）。作为副作用，
 * 页面上的 PD_HAS_FREE_LINES 提示位会按需要被置位或清零。调用者可能还需要
 * 考虑数组截断后行指针数组长度的减少。
 */
void
PageRepairFragmentation(Page page)
{
	Offset		pd_lower = ((PageHeader) page)->pd_lower;
	Offset		pd_upper = ((PageHeader) page)->pd_upper;
	Offset		pd_special = ((PageHeader) page)->pd_special;
	Offset		last_offset;
	itemIdCompactData itemidbase[MaxHeapTuplesPerPage];
	itemIdCompact itemidptr;
	ItemId		lp;
	int			nline,
				nstorage,
				nunused;
	OffsetNumber finalusedlp = InvalidOffsetNumber;
	int			i;
	Size		totallen;
	bool		presorted = true;	/* 暂定 */

	/*
	 * 这里比在大多数地方更值得费心保持多疑，因为我们即将（通常是在）一个
	 * 共享磁盘缓冲区中重新整理数据。如果我们不小心，损坏的指针、长度等等
	 * 可能会导致我们破坏相邻的磁盘缓冲区，使数据丢失进一步扩散。因此，要
	 * 检查一切。
	 */
	if (pd_lower < SizeOfPageHeaderData ||
		pd_lower > pd_upper ||
		pd_upper > pd_special ||
		pd_special > BLCKSZ ||
		pd_special != MAXALIGN(pd_special))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupted page pointers: lower = %u, upper = %u, special = %u",
						pd_lower, pd_upper, pd_special)));

	/*
	 * 遍历行指针数组，收集关于存活项（live items）的数据。
	 */
	nline = PageGetMaxOffsetNumber(page);
	itemidptr = itemidbase;
	nunused = totallen = 0;
	last_offset = pd_special;
	for (i = FirstOffsetNumber; i <= nline; i++)
	{
		lp = PageGetItemId(page, i);
		if (ItemIdIsUsed(lp))
		{
			if (ItemIdHasStorage(lp))
			{
				itemidptr->offsetindex = i - 1;
				itemidptr->itemoff = ItemIdGetOffset(lp);

				if (last_offset > itemidptr->itemoff)
					last_offset = itemidptr->itemoff;
				else
					presorted = false;

				if (unlikely(itemidptr->itemoff < (int) pd_upper ||
							 itemidptr->itemoff >= (int) pd_special))
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("corrupted line pointer: %u",
									itemidptr->itemoff)));
				itemidptr->alignedlen = MAXALIGN(ItemIdGetLength(lp));
				totallen += itemidptr->alignedlen;
				itemidptr++;
			}

			finalusedlp = i;	/* 可能是最后一个非 LP_UNUSED 项 */
		}
		else
		{
		/* 未使用的条目应当 lp_len = 0，但还是要确认一下 */
		Assert(!ItemIdHasStorage(lp));
			ItemIdSetUnused(lp);
			nunused++;
		}
	}

	nstorage = itemidptr - itemidbase;
	if (nstorage == 0)
	{
		/* 页面完全为空，因此快速重置它即可 */
		((PageHeader) page)->pd_upper = pd_special;
	}
	else
	{
		/* 需要费力地紧凑化页面 */
		if (totallen > (Size) (pd_special - pd_lower))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("corrupted item lengths: total %u, available space %u",
							(unsigned int) totallen, pd_special - pd_lower)));

		compactify_tuples(itemidbase, nstorage, page, presorted);
	}

	if (finalusedlp != nline)
	{
		/* 最后一个行指针并不是最后一个被使用的行指针 */
		int			nunusedend = nline - finalusedlp;

		Assert(nunused >= nunusedend && nunusedend > 0);

		/* 从计数中移除末尾的未使用行指针 */
		nunused -= nunusedend;
		/* 截断行指针数组 */
		((PageHeader) page)->pd_lower -= (sizeof(ItemIdData) * nunusedend);
	}

	/* 为 PageAddItemExtended 设置提示位 */
	if (nunused > 0)
		PageSetHasFreeLinePointers(page);
	else
		PageClearHasFreeLinePointers(page);
}

/*
 * PageTruncateLinePointerArray
 *
 * 移除行指针数组末尾未使用的行指针。
 *
 * 本例程仅可用于堆页面。它由 VACUUM 在第二次遍历堆时调用。我们期望页面上
 * 至少有一个 LP_UNUSED 行指针（如果 VACUUM 在页面上没有 LP_DEAD 项可被它
 * 置为 LP_UNUSED，那它就不应该调用到这里）。
 *
 * 我们避免把行指针数组截断为 0 个项，如有必要，会保留一个剩余的 LP_UNUSED
 * 项。这有点武断，但为了避免留下一个 PageIsEmpty() 页面，这似乎是个好主意。
 *
 * 调用者可以持有页面缓冲区的排他锁或完整的清理锁。页面的 PD_HAS_FREE_LINES
 * 提示位会根据我们是否留下了任何剩余的 LP_UNUSED 项而被置位或清零。
 */
void
PageTruncateLinePointerArray(Page page)
{
	PageHeader	phdr = (PageHeader) page;
	bool		countdone = false,
				sethint = false;
	int			nunusedend = 0;

	/* 从后向前扫描行指针数组 */
	for (int i = PageGetMaxOffsetNumber(page); i >= FirstOffsetNumber; i--)
	{
		ItemId		lp = PageGetItemId(page, i);

		if (!countdone && i > FirstOffsetNumber)
		{
			/*
			 * 仍在确定数组末尾的哪些行指针将被截断掉。要么把另一个行指针
			 * 计为可以安全截断，要么注意到额外的行指针不再能安全截断（停止
			 * 计数行指针）。
			 */
			if (!ItemIdIsUsed(lp))
				nunusedend++;
			else
				countdone = true;
		}
		else
		{
			/*
			 * 一旦我们停止计数，仍然需要弄清楚在数组更靠前的位置是否还有
			 * 任何剩余的 LP_UNUSED 行指针。
			 */
			if (!ItemIdIsUsed(lp))
			{
				/*
				 * 这是一个我们不会截断掉的未使用行指针——因此至少有一个。
				 * 在页面上设置提示。
				 */
				sethint = true;
				break;
			}
		}
	}

	if (nunusedend > 0)
	{
		phdr->pd_lower -= sizeof(ItemIdData) * nunusedend;

#ifdef CLOBBER_FREED_MEMORY
		memset((char *) page + phdr->pd_lower, 0x7F,
			   sizeof(ItemIdData) * nunusedend);
#endif
	}
	else
		Assert(sethint);

	/* 为 PageAddItemExtended 设置提示位 */
	if (sethint)
		PageSetHasFreeLinePointers(page);
	else
		PageClearHasFreeLinePointers(page);
}

/*
 * PageGetFreeSpace
 *		返回页面上空闲（可分配）空间的大小，扣除了一个新行指针所需的空间。
 *
 * 注意：这通常只应被用于索引页面。在堆页面上请使用 PageGetHeapFreeSpace。
 */
Size
PageGetFreeSpace(const PageData *page)
{
	const PageHeaderData *phdr = (const PageHeaderData *) page;
	int			space;

	/*
	 * 这里使用有符号算术，以便在 pd_lower > pd_upper 时行为仍然合理。
	 */
	space = (int) phdr->pd_upper - (int) phdr->pd_lower;

	if (space < (int) sizeof(ItemIdData))
		return 0;
	space -= sizeof(ItemIdData);

	return (Size) space;
}

/*
 * PageGetFreeSpaceForMultipleTuples
 *		返回页面上空闲（可分配）空间的大小，扣除了多个新行指针所需的空间。
 *
 * 注意：这通常只应被用于索引页面。在堆页面上请使用 PageGetHeapFreeSpace。
 */
Size
PageGetFreeSpaceForMultipleTuples(const PageData *page, int ntups)
{
	const PageHeaderData *phdr = (const PageHeaderData *) page;
	int			space;

	/*
	 * 这里使用有符号算术，以便在 pd_lower > pd_upper 时行为仍然合理。
	 */
	space = (int) phdr->pd_upper - (int) phdr->pd_lower;

	if (space < (int) (ntups * sizeof(ItemIdData)))
		return 0;
	space -= ntups * sizeof(ItemIdData);

	return (Size) space;
}

/*
 * PageGetExactFreeSpace
 *		返回页面上空闲（可分配）空间的大小，不考虑添加/移除行指针。
 */
Size
PageGetExactFreeSpace(const PageData *page)
{
	const PageHeaderData *phdr = (const PageHeaderData *) page;
	int			space;

	/*
	 * 这里使用有符号算术，以便在 pd_lower > pd_upper 时行为仍然合理。
	 */
	space = (int) phdr->pd_upper - (int) phdr->pd_lower;

	if (space < 0)
		return 0;

	return (Size) space;
}


/*
 * PageGetHeapFreeSpace
 *		返回页面上空闲（可分配）空间的大小，扣除了一个新行指针所需的空间。
 *
 * 本函数与 PageGetFreeSpace 的区别在于：如果页面上已经有 MaxHeapTuplesPerPage
 * 个行指针且没有空闲的，本函数会返回零。我们用它来确保堆页面上创建的
 * 行指针数量不超过 MaxHeapTuplesPerPage。（尽管无论如何也放不下比那更多的
 * 元组，但在存在重定向或死行指针的情况下，可能会出现过多的行指针。为了避免
 * 破坏那些假设 MaxHeapTuplesPerPage 是行指针数量硬上限的代码，我们做了这
 * 次额外的检查。）
 */
Size
PageGetHeapFreeSpace(const PageData *page)
{
	Size		space;

	space = PageGetFreeSpace(page);
	if (space > 0)
	{
		OffsetNumber offnum,
					nline;

			/*
			 * 页面上是否已经有 MaxHeapTuplesPerPage 个行指针了？
			 */
		nline = PageGetMaxOffsetNumber(page);
		if (nline >= MaxHeapTuplesPerPage)
		{
			if (PageHasFreeLinePointers(page))
			{
				/*
				 * 由于这只是一个提示，我们必须确认那里确实存在一个空闲的
				 * 行指针
				 */
				for (offnum = FirstOffsetNumber; offnum <= nline; offnum = OffsetNumberNext(offnum))
				{
					ItemId		lp = PageGetItemId(unconstify(PageData *, page), offnum);

					if (!ItemIdIsUsed(lp))
						break;
				}

				if (offnum > nline)
				{
				/*
				 * 该提示是错误的，但我们无法在这里清除它，因为我们没有
				 * 将页面标记为脏的能力。
				 */
					space = 0;
				}
			}
			else
			{
			/*
			 * 尽管该提示可能是错的，但 PageAddItem 无论如何都会相信它，
			 * 因此我们也必须相信它。
			 */
				space = 0;
			}
		}
	}
	return space;
}


/*
 * PageIndexTupleDelete
 *
 * 本例程完成从索引页面移除一个元组的工作。
 *
 * 与堆页面不同，我们会紧凑化掉被移除元组的行指针。
 */
void
PageIndexTupleDelete(Page page, OffsetNumber offnum)
{
	PageHeader	phdr = (PageHeader) page;
	char	   *addr;
	ItemId		tup;
	Size		size;
	unsigned	offset;
	int			nbytes;
	int			offidx;
	int			nline;

	/*
	 * 与 PageRepairFragmentation 一样，这里保持多疑是值得的。
	 */
	if (phdr->pd_lower < SizeOfPageHeaderData ||
		phdr->pd_lower > phdr->pd_upper ||
		phdr->pd_upper > phdr->pd_special ||
		phdr->pd_special > BLCKSZ ||
		phdr->pd_special != MAXALIGN(phdr->pd_special))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupted page pointers: lower = %u, upper = %u, special = %u",
						phdr->pd_lower, phdr->pd_upper, phdr->pd_special)));

	nline = PageGetMaxOffsetNumber(page);
	if ((int) offnum <= 0 || (int) offnum > nline)
		elog(ERROR, "invalid index offnum: %u", offnum);

	/* 将偏移号转换为偏移索引 */
	offidx = offnum - 1;

	tup = PageGetItemId(page, offnum);
	Assert(ItemIdHasStorage(tup));
	size = ItemIdGetLength(tup);
	offset = ItemIdGetOffset(tup);

	if (offset < phdr->pd_upper || (offset + size) > phdr->pd_special ||
		offset != MAXALIGN(offset))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupted line pointer: offset = %u, size = %u",
						offset, (unsigned int) size)));

	/* 实际需要删除的空间大小 */
	size = MAXALIGN(size);

	/*
	 * 首先，我们想去掉该索引元组的 pd_linp 条目。我们把数组中所有后续的
	 * linp 向后挪一个槽位。我们不使用 PageGetItemId，因为我们操纵的是
	 * _数组_，而不是单个的 linp。
	 */
	nbytes = phdr->pd_lower -
		((char *) &phdr->pd_linp[offidx + 1] - (char *) phdr);

	if (nbytes > 0)
		memmove(&(phdr->pd_linp[offidx]),
				&(phdr->pd_linp[offidx + 1]),
				nbytes);

	/*
	 * 现在把旧的上界（元组空间的起始位置）与被删除元组的起始位置之间的
	 * 所有内容向前移动，从而让页面中间的空闲空间留出来。如果我们刚删除的
	 * 就是元组空间起始处的元组，那就不需要做这次复制。
	 */

	/* 元组空间的起始位置 */
	addr = (char *) page + phdr->pd_upper;

	if (offset > phdr->pd_upper)
		memmove(addr + size, addr, offset - phdr->pd_upper);

	/* 调整空闲空间的边界指针 */
	phdr->pd_upper += size;
	phdr->pd_lower -= sizeof(ItemIdData);

	/*
	 * 最后，我们需要调整剩余的行指针条目。
	 *
	 * 任何原本位于被删除元组数据之前的内容，都已向前移动了被删除元组的大小。
	 */
	if (!PageIsEmpty(page))
	{
		int			i;

		nline--;				/* 比开始时少了一个 */
		for (i = 1; i <= nline; i++)
		{
			ItemId		ii = PageGetItemId(page, i);

			Assert(ItemIdHasStorage(ii));
			if (ItemIdGetOffset(ii) <= offset)
				ii->lp_off += size;
		}
	}
}


/*
 * PageIndexMultiDelete
 *
 * 本例程处理一次性从索引页面删除多个元组的情况。它比围绕 PageIndexTupleDelete
 * 的循环要快得多……不过，调用者*必须*按项号顺序提供待删除的项号数组！
 */
void
PageIndexMultiDelete(Page page, OffsetNumber *itemnos, int nitems)
{
	PageHeader	phdr = (PageHeader) page;
	Offset		pd_lower = phdr->pd_lower;
	Offset		pd_upper = phdr->pd_upper;
	Offset		pd_special = phdr->pd_special;
	Offset		last_offset;
	itemIdCompactData itemidbase[MaxIndexTuplesPerPage];
	ItemIdData	newitemids[MaxIndexTuplesPerPage];
	itemIdCompact itemidptr;
	ItemId		lp;
	int			nline,
				nused;
	Size		totallen;
	Size		size;
	unsigned	offset;
	int			nextitm;
	OffsetNumber offnum;
	bool		presorted = true;	/* 暂定 */

	Assert(nitems <= MaxIndexTuplesPerPage);

	/*
	 * 如果待删除的项不太多，那么逐条调用 PageIndexTupleDelete 就是最好的
	 * 方式。以逆序删除各项，这样我们就不必考虑为前面已做的删除调整项号。
	 *
	 * TODO: 调整这里魔法数字
	 */
	if (nitems <= 2)
	{
		while (--nitems >= 0)
			PageIndexTupleDelete(page, itemnos[nitems]);
		return;
	}

	/*
	 * 与 PageRepairFragmentation 一样，这里保持多疑是值得的。
	 */
	if (pd_lower < SizeOfPageHeaderData ||
		pd_lower > pd_upper ||
		pd_upper > pd_special ||
		pd_special > BLCKSZ ||
		pd_special != MAXALIGN(pd_special))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupted page pointers: lower = %u, upper = %u, special = %u",
						pd_lower, pd_upper, pd_special)));

	/*
	 * 扫描行指针数组，构建一个仅包含我们要保留的那些项的列表。注意我们
	 * 尚未修改页面，因为我们仍在进行有效性检查。
	 */
	nline = PageGetMaxOffsetNumber(page);
	itemidptr = itemidbase;
	totallen = 0;
	nused = 0;
	nextitm = 0;
	last_offset = pd_special;
	for (offnum = FirstOffsetNumber; offnum <= nline; offnum = OffsetNumberNext(offnum))
	{
		lp = PageGetItemId(page, offnum);
		Assert(ItemIdHasStorage(lp));
		size = ItemIdGetLength(lp);
		offset = ItemIdGetOffset(lp);
		if (offset < pd_upper ||
			(offset + size) > pd_special ||
			offset != MAXALIGN(offset))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("corrupted line pointer: offset = %u, size = %u",
							offset, (unsigned int) size)));

		if (nextitm < nitems && offnum == itemnos[nextitm])
		{
			/* 跳过待删除的项 */
			nextitm++;
		}
		else
		{
			itemidptr->offsetindex = nused; /* 它将被放置的位置 */
			itemidptr->itemoff = offset;

			if (last_offset > itemidptr->itemoff)
				last_offset = itemidptr->itemoff;
			else
				presorted = false;

			itemidptr->alignedlen = MAXALIGN(size);
			totallen += itemidptr->alignedlen;
			newitemids[nused] = *lp;
			itemidptr++;
			nused++;
		}
	}

	/* 这会捕获无效或乱序的 itemnos[] */
	if (nextitm != nitems)
		elog(ERROR, "incorrect index offsets supplied");

	if (totallen > (Size) (pd_special - pd_lower))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupted item lengths: total %u, available space %u",
						(unsigned int) totallen, pd_special - pd_lower)));

	/*
	 * 看起来没问题。用这份副本覆盖行指针，其中我们已经移除了所有未使用的
	 * 项。
	 */
	memcpy(phdr->pd_linp, newitemids, nused * sizeof(ItemIdData));
	phdr->pd_lower = SizeOfPageHeaderData + nused * sizeof(ItemIdData);

	/* 然后紧凑化元组数据 */
	if (nused > 0)
		compactify_tuples(itemidbase, nused, page, presorted);
	else
		phdr->pd_upper = pd_special;
}


/*
 * PageIndexTupleDeleteNoCompact
 *
 * 从索引页面移除指定的元组，但将其行指针置为"未使用"，而非紧凑化掉它；
 * 例外情况是，如果它是页面上最后一个行指针，则可以被移除。
 *
 * 这用于那些要求存活元组的现有 TID 保持不变、并且愿意允许未使用行指针
 * 的索引访问方法（AM）。
 */
void
PageIndexTupleDeleteNoCompact(Page page, OffsetNumber offnum)
{
	PageHeader	phdr = (PageHeader) page;
	char	   *addr;
	ItemId		tup;
	Size		size;
	unsigned	offset;
	int			nline;

	/*
	 * 与 PageRepairFragmentation 一样，这里保持多疑是值得的。
	 */
	if (phdr->pd_lower < SizeOfPageHeaderData ||
		phdr->pd_lower > phdr->pd_upper ||
		phdr->pd_upper > phdr->pd_special ||
		phdr->pd_special > BLCKSZ ||
		phdr->pd_special != MAXALIGN(phdr->pd_special))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupted page pointers: lower = %u, upper = %u, special = %u",
						phdr->pd_lower, phdr->pd_upper, phdr->pd_special)));

	nline = PageGetMaxOffsetNumber(page);
	if ((int) offnum <= 0 || (int) offnum > nline)
		elog(ERROR, "invalid index offnum: %u", offnum);

	tup = PageGetItemId(page, offnum);
	Assert(ItemIdHasStorage(tup));
	size = ItemIdGetLength(tup);
	offset = ItemIdGetOffset(tup);

	if (offset < phdr->pd_upper || (offset + size) > phdr->pd_special ||
		offset != MAXALIGN(offset))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupted line pointer: offset = %u, size = %u",
						offset, (unsigned int) size)));

	/* 实际需要删除的空间大小 */
	size = MAXALIGN(size);

	/*
	 * 要么把行指针置为"未使用"，要么如果是最后一个就把它抹掉。（注意：
	 * 倒数第二个（或多个）可能已经是未使用的，但即便如此我们也不费心
	 * 去尝试把它们紧凑化掉。）
	 */
	if ((int) offnum < nline)
		ItemIdSetUnused(tup);
	else
	{
		phdr->pd_lower -= sizeof(ItemIdData);
		nline--;				/* 比开始时少了一个 */
	}

	/*
	 * 现在把旧的上界（元组空间的起始位置）与被删除元组的起始位置之间的
	 * 所有内容向前移动，从而让页面中间的空闲空间留出来。如果我们刚删除的
	 * 就是元组空间起始处的元组，那就不需要做这次复制。
	 */

	/* 元组空间的起始位置 */
	addr = (char *) page + phdr->pd_upper;

	if (offset > phdr->pd_upper)
		memmove(addr + size, addr, offset - phdr->pd_upper);

	/* 调整空闲空间的边界指针 */
	phdr->pd_upper += size;

	/*
	 * 最后，我们需要调整剩余的行指针条目。
	 *
	 * 任何原本位于被删除元组数据之前的内容，都已向前移动了被删除元组的大小。
	 */
	if (!PageIsEmpty(page))
	{
		int			i;

		for (i = 1; i <= nline; i++)
		{
			ItemId		ii = PageGetItemId(page, i);

			if (ItemIdHasStorage(ii) && ItemIdGetOffset(ii) <= offset)
				ii->lp_off += size;
		}
	}
}


/*
 * PageIndexTupleOverwrite
 *
 * 替换索引页面上的指定元组。
 *
 * 新元组被放置在旧元组原本所在的确切位置，按需将其他元组的数据向上或向下
 * 移动，以保持页面的紧凑。这比删除再重新插入该元组更好，因为元组大小
 * 不变时它避免了任何数据搬移；即便大小变了，我们也避免了搬移行指针。
 * 这可被那些不希望在 LP_DEAD 位被设置时清除它的索引 AM 使用。它或许也能被
 * 那些既关心元组物理顺序、也关心其逻辑/ItemId 顺序的索引 AM 使用。
 *
 * 如果没有足够的空间容纳新元组，返回 false。其他错误代表数据损坏问题，
 * 因此我们只是 elog。
 */
bool
PageIndexTupleOverwrite(Page page, OffsetNumber offnum,
						Item newtup, Size newsize)
{
	PageHeader	phdr = (PageHeader) page;
	ItemId		tupid;
	int			oldsize;
	unsigned	offset;
	Size		alignednewsize;
	int			size_diff;
	int			itemcount;

	/*
	 * 与 PageRepairFragmentation 一样，这里保持多疑是值得的。
	 */
	if (phdr->pd_lower < SizeOfPageHeaderData ||
		phdr->pd_lower > phdr->pd_upper ||
		phdr->pd_upper > phdr->pd_special ||
		phdr->pd_special > BLCKSZ ||
		phdr->pd_special != MAXALIGN(phdr->pd_special))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupted page pointers: lower = %u, upper = %u, special = %u",
						phdr->pd_lower, phdr->pd_upper, phdr->pd_special)));

	itemcount = PageGetMaxOffsetNumber(page);
	if ((int) offnum <= 0 || (int) offnum > itemcount)
		elog(ERROR, "invalid index offnum: %u", offnum);

	tupid = PageGetItemId(page, offnum);
	Assert(ItemIdHasStorage(tupid));
	oldsize = ItemIdGetLength(tupid);
	offset = ItemIdGetOffset(tupid);

	if (offset < phdr->pd_upper || (offset + oldsize) > phdr->pd_special ||
		offset != MAXALIGN(offset))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupted line pointer: offset = %u, size = %u",
						offset, (unsigned int) oldsize)));

	/*
	 * 确定空间需求的实际变化量，检查页面是否溢出。
	 */
	oldsize = MAXALIGN(oldsize);
	alignednewsize = MAXALIGN(newsize);
	if (alignednewsize > oldsize + (phdr->pd_upper - phdr->pd_lower))
		return false;

	/*
	 * 重新定位现有数据并更新行指针，除非新元组与旧元组（对齐后）大小相同，
	 * 那样就没有什么可做的。注意，我们需要重新定位的是目标元组之前的数据，
	 * 而不是之后的数据，因此把 size_diff 表达为元组大小减少的量是很方便的，
	 * 这样它就是需要加到 pd_upper 和受影响的行指针上的增量。
	 */
	size_diff = oldsize - (int) alignednewsize;
	if (size_diff != 0)
	{
		char	   *addr = (char *) page + phdr->pd_upper;
		int			i;

		/* 重新定位目标元组之前的所有元组数据 */
		memmove(addr + size_diff, addr, offset - phdr->pd_upper);

		/* 调整空闲空间的边界指针 */
		phdr->pd_upper += size_diff;

		/* 也调整受影响的行指针 */
		for (i = FirstOffsetNumber; i <= itemcount; i++)
		{
			ItemId		ii = PageGetItemId(page, i);

			/* 允许没有存储的项；目前只有 BRIN 需要这样 */
			if (ItemIdHasStorage(ii) && ItemIdGetOffset(ii) <= offset)
				ii->lp_off += size_diff;
		}
	}

	/* 更新该项的元组长度，但不改变其 lp_flags 字段 */
	tupid->lp_off = offset + size_diff;
	tupid->lp_len = newsize;

	/* 将新元组的数据复制到页面上 */
	memcpy(PageGetItem(page, tupid), newtup, newsize);

	return true;
}


/*
 * 为共享缓冲区中的一个页面设置校验和。
 *
 * 如果校验和已禁用，或者页面尚未初始化，就直接返回输入。否则，在计算校验和
 * 之前我们必须制作一份页面的副本，以防止并发修改（例如设置提示位）使最终
 * 的校验和失效。在复制期间我们是否包含或排除提示位都无关紧要，只要写出的是
 * 一个有效的页面及其关联的校验和即可。
 *
 * 返回一个指向需要写入的、块大小数据的指针。它使用静态分配的内存，因此
 * 调用者必须立即写出返回的页面，并且不再引用它。
 */
char *
PageSetChecksumCopy(Page page, BlockNumber blkno)
{
	static char *pageCopy = NULL;

	/* 如果不需要校验和，就直接返回传入的数据 */
	if (PageIsNew(page) || !DataChecksumsEnabled())
		return page;

	/*
	 * 我们一次性分配这份副本空间，并在之后每次调用时复用它。这里用 palloc
	 * 分配，而不是用一个静态 char 数组，其目的一是确保校验和代码拥有足够的
	 * 对齐，二是避免在那些从不调用本函数的进程中浪费空间。
	 */
	if (pageCopy == NULL)
		pageCopy = MemoryContextAllocAligned(TopMemoryContext,
											 BLCKSZ,
											 PG_IO_ALIGN_SIZE,
											 0);

	memcpy(pageCopy, page, BLCKSZ);
	((PageHeader) pageCopy)->pd_checksum = pg_checksum_page(pageCopy, blkno);
	return pageCopy;
}

/*
 * 为私有内存中的一个页面设置校验和。
 *
 * 这只能在我们确信没有其他进程会修改该页面缓冲区时使用。
 */
void
PageSetChecksumInplace(Page page, BlockNumber blkno)
{
	/* 如果不需要校验和，就直接返回 */
	if (PageIsNew(page) || !DataChecksumsEnabled())
		return;

	((PageHeader) page)->pd_checksum = pg_checksum_page(page, blkno);
}
