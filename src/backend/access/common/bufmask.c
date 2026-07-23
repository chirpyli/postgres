/*-------------------------------------------------------------------------
 *
 * bufmask.c
 *	  用于缓冲区掩码处理的例程。用于屏蔽页面中某些在 WAL 生成时
 *	  与 WAL 应用时不相同的位。
 *
 * Portions Copyright (c) 2016-2025, PostgreSQL Global Development Group
 *
 * 包含屏蔽页面所需的通用例程。
 *
 * IDENTIFICATION
 *	  src/backend/access/common/bufmask.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/bufmask.h"

/*
 * mask_page_lsn_and_checksum
 *
 * 在一致性检查中，被比较的两个页面的 LSN 很可能不同，因为 WAL 生成时
 * 存在并发操作，而 WAL 应用时页面状态已变。同时屏蔽掉校验和，因为
 * 屏蔽页面上其它任何内容都意味着校验和也将无法匹配。
 */
void
mask_page_lsn_and_checksum(Page page)
{
	PageHeader	phdr = (PageHeader) page;

	PageXLogRecPtrSet(phdr->pd_lsn, (uint64) MASK_MARKER);
	phdr->pd_checksum = MASK_MARKER;
}

/*
 * mask_page_hint_bits
 *
 * 屏蔽 PageHeader 中的提示位。我们希望忽略提示位上的差异，
 * 因为它们可以在不写任何 WAL 的情况下被设置。
 */
void
mask_page_hint_bits(Page page)
{
	PageHeader	phdr = (PageHeader) page;

	/* 忽略 prune_xid（它类似于一个提示位） */
	phdr->pd_prune_xid = MASK_MARKER;

	/* 忽略 PD_PAGE_FULL 和 PD_HAS_FREE_LINES 标志，它们只是提示位。 */
	PageClearFull(page);
	PageClearHasFreeLinePointers(page);

	/*
	 * 在回放期间，如果页面 LSN 已超过我们 XLOG 记录的 LSN，
	 * 我们不会将页面标记为全部可见。详见 heap_xlog_visible()。
	 */
	PageClearAllVisible(page);
}

/*
 * mask_unused_space
 *
 * 屏蔽页面上 pd_lower 与 pd_upper 之间的未使用空间。
 */
void
mask_unused_space(Page page)
{
	int			pd_lower = ((PageHeader) page)->pd_lower;
	int			pd_upper = ((PageHeader) page)->pd_upper;
	int			pd_special = ((PageHeader) page)->pd_special;

	/* 合理性检查 */
	if (pd_lower > pd_upper || pd_special < pd_upper ||
		pd_lower < SizeOfPageHeaderData || pd_special > BLCKSZ)
	{
		elog(ERROR, "invalid page pd_lower %u pd_upper %u pd_special %u",
			 pd_lower, pd_upper, pd_special);
	}

	memset(page + pd_lower, MASK_MARKER, pd_upper - pd_lower);
}

/*
 * mask_lp_flags
 *
 * 在某些索引访问方法中，行指针标志可以在主表上被修改，
 * 而不写任何 WAL 记录。
 */
void
mask_lp_flags(Page page)
{
	OffsetNumber offnum,
				maxoff;

	maxoff = PageGetMaxOffsetNumber(page);
	for (offnum = FirstOffsetNumber;
		 offnum <= maxoff;
		 offnum = OffsetNumberNext(offnum))
	{
		ItemId		itemId = PageGetItemId(page, offnum);

		if (ItemIdIsUsed(itemId))
			itemId->lp_flags = LP_UNUSED;
	}
}

/*
 * mask_page_content
 *
 * 在某些索引访问方法中，被删除页面的内容几乎需要被完全忽略。
 */
void
mask_page_content(Page page)
{
	/* 屏蔽页面内容 */
	memset(page + SizeOfPageHeaderData, MASK_MARKER,
		   BLCKSZ - SizeOfPageHeaderData);

	/* 屏蔽 pd_lower 和 pd_upper */
	memset(&((PageHeader) page)->pd_lower, MASK_MARKER,
		   sizeof(uint16));
	memset(&((PageHeader) page)->pd_upper, MASK_MARKER,
		   sizeof(uint16));
}
