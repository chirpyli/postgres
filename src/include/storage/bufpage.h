/*-------------------------------------------------------------------------
 *
 * bufpage.h
 *	  POSTGRES 标准缓冲区页定义。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/bufpage.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BUFPAGE_H
#define BUFPAGE_H

#include "access/xlogdefs.h"
#include "storage/block.h"
#include "storage/item.h"
#include "storage/off.h"

/* GUC 变量 */
extern PGDLLIMPORT bool ignore_checksum_failure;

/*
 * postgres 磁盘页是建立在 postgres 磁盘块（即单纯的 I/O 单位，参见 block.h）
 * 之上的一层抽象。
 *
 * 具体来说，虽然磁盘块可以是未格式化的，但 postgres 磁盘页始终是如下形式的槽式页面：
 *
 * +----------------+---------------------------------+
 * | PageHeaderData | linp1 linp2 linp3 ...           |
 * +-----------+----+---------------------------------+
 * | ... linpN |									  |
 * +-----------+--------------------------------------+
 * |		   ^ pd_lower							  |
 * |												  |
 * |			 v pd_upper							  |
 * +-------------+------------------------------------+
 * |			 | tupleN ...                         |
 * +-------------+------------------+-----------------+
 * |	   ... tuple3 tuple2 tuple1 | "special space" |
 * +--------------------------------+-----------------+
 *									^ pd_special
 *
 * 当 pd_lower 和 pd_upper 之间无法添加任何内容时，页面即为满。
 *
 * 访问方法写入的所有块都必须是磁盘页。
 *
 * 例外情况：
 *
 * 显然，页面在调用 PageInit 初始化之前是没有格式化的。
 *
 * 注意事项：
 *
 * linp1..N 构成 ItemId（行指针）数组。ItemPointer 指向一个物理块号和该块/页
 * 中的逻辑偏移量（行指针编号）。注意，OffsetNumber 按惯例从 1 开始，而非 0。
 *
 * tuple1..N 在页面上是"从后往前"添加的。由于 ItemPointer 偏移量用于访问 ItemId
 * 条目而非实际的字节偏移位置，元组可以在页面上按需物理重排。这种间接方式也使
 * 崩溃恢复相对简单，因为页面空间管理的底层细节可以由标准缓冲区页代码在日志记录
 * 和恢复期间统一控制。
 *
 * 与访问方法无关的通用每页信息保存在 PageHeaderData 中。
 *
 * 与访问方法相关的每页数据（如果有的话）保存在标记为 "special space" 的区域中；
 * 每种访问方法都有一个在别处定义的 "opaque" 结构体，作为页尾存储。访问方法应始终
 * 使用 PageInit 初始化其页面，然后设置自己的 opaque 字段。
 */

typedef char PageData;
typedef PageData *Page;


/*
 * 页面内的位置（字节偏移量）。
 *
 * 注意，这实际上被限制为 2^15，因为我们已将 ItemIdData.lp_off 和 ItemIdData.lp_len
 * 限制为 15 位（参见 itemid.h）。
 */
typedef uint16 LocationIndex;


/*
 * 由于历史原因，64 位的 LSN 值存储为两个 32 位的值。
 */
typedef struct
{
	uint32		xlogid;			/* 高位 */
	uint32		xrecoff;		/* 低位 */
} PageXLogRecPtr;

static inline XLogRecPtr
PageXLogRecPtrGet(PageXLogRecPtr val)
{
	return (uint64) val.xlogid << 32 | val.xrecoff;
}

#define PageXLogRecPtrSet(ptr, lsn) \
	((ptr).xlogid = (uint32) ((lsn) >> 32), (ptr).xrecoff = (uint32) (lsn))

/*
 * 磁盘页组织结构
 *
 * 适用于任何页面的空间管理通用信息
 *
 *		pd_lsn			- 记录对该页最后一次变更的 xlog 记录。
 *		pd_checksum		- 页面校验和（如果设置）。
 *		pd_flags		- 标志位。
 *		pd_lower		- 空闲空间起始偏移量。
 *		pd_upper		- 空闲空间结束偏移量。
 *		pd_special		- 特殊空间起始偏移量。
 *		pd_pagesize_version	- 页面大小（字节）和页布局版本号。
 *		pd_prune_xid	- 页面上潜在可清理元组中最老的 XID。
 *
 * LSN 被缓冲区管理器用于强制执行 WAL 的基本规则：
 * "数据写入前必须先写 xlog"。脏缓冲区不能刷到磁盘，直到 xlog 至少刷到
 * 该页的 LSN 为止。
 *
 * pd_checksum 存储该页的校验和（如果已设置）；校验和为 0 是合法值。
 * 如果不使用校验和，则该字段保持未设置状态。这通常意味着该字段为 0，
 * 但非零值也可能出现，例如从 9.3 之前版本通过 pg_upgrade 升级的数据库，
 * 当时该字节偏移量用于存储页面最后更新时的 current timelineid。
 * 注意，页面上没有标记表明校验和是否有效，这是一个刻意的设计选择，
 * 以避免依赖页面内容来决定是否验证校验和的问题。因此没有与校验和相关的标志位。
 *
 * pd_prune_xid 是一个提示字段，用于帮助判断清理操作是否有用。
 * 目前在索引页中未使用。
 *
 * 页版本号和页大小打包成一个 uint16 字段。这是历史原因：
 * 在 PostgreSQL 7.3 之前，没有页版本号的概念，这样做可以让我们假装 7.3 之前的
 * 数据库页版本号为 0。我们限制页大小为 256 的倍数，从而将低八位留给版本号。
 *
 * 最小可能的页大小约为 64B，以容纳页头、opaque 空间和一个最小元组；
 * 当然，实际使用中希望更大的页，因此页大小 mod 256 的限制并不是一个重要约束。
 * 在上限方面，由于 lp_off/lp_len 是 15 位，我们仅支持最大 32KB 的页面。
 */

typedef struct PageHeaderData
{
	/* XXX LSN 是任何块的成员，不仅限于页组织块 */
	PageXLogRecPtr pd_lsn;		/* LSN：对页最后一次变更的 xlog 记录最后字节
								 * 的下一个字节 */
	uint16		pd_checksum;	/* 校验和 */
	uint16		pd_flags;		/* 标志位，见下文 */
	LocationIndex pd_lower;		/* 空闲空间起始偏移量 */
	LocationIndex pd_upper;		/* 空闲空间结束偏移量 */
	LocationIndex pd_special;	/* 特殊空间起始偏移量 */
	uint16		pd_pagesize_version;
	TransactionId pd_prune_xid; /* 最老的可清理 XID，无则为零 */
	ItemIdData	pd_linp[FLEXIBLE_ARRAY_MEMBER]; /* 行指针数组 */
} PageHeaderData;

typedef PageHeaderData *PageHeader;

/*
 * pd_flags 包含以下标志位。未定义的位被初始化为零，可能在未来使用。
 *
 * PD_HAS_FREE_LINES 在 pd_lower 之前存在任何 LP_UNUSED 行指针时设置。
 * 这应被视为提示而非事实，因为对其的变更不记入 WAL。
 *
 * PD_PAGE_FULL 在 UPDATE 在页面上找不到足够空闲空间容纳新元组版本时设置；
 * 这表明需要执行清理。同样，这只是一个提示。
 */
#define PD_HAS_FREE_LINES	0x0001	/* 是否存在未使用的行指针？ */
#define PD_PAGE_FULL		0x0002	/* 新元组的空闲空间不足？ */
#define PD_ALL_VISIBLE		0x0004	/* 页面上所有元组对所有事务可见 */

#define PD_VALID_FLAG_BITS	0x0007	/* 所有合法 pd_flags 位的 OR */

/*
 * 页布局版本号 0 对应于 7.3 之前的 Postgres 版本。
 * 7.3 和 7.4 版本使用 1，表示新的 HeapTupleHeader 布局。
 * 8.0 版本使用 2；再次修改了 HeapTupleHeader 布局。
 * 8.1 版本使用 3；重新定义了 HeapTupleHeader 的 infomask 位。
 * 8.3 版本使用 4；再次修改了 HeapTupleHeader 布局，
 *		并增加了 pd_flags 字段（从 pd_tli 借用了一些位），
 *		还增加了 pd_prune_xid 字段（扩大了头部大小）。
 *
 * 从 9.3 版本起，处理页面时还必须考虑校验和版本。
 */
#define PG_PAGE_LAYOUT_VERSION		4
#define PG_DATA_CHECKSUM_VERSION	1

/* ----------------------------------------------------------------
 *						页面辅助函数
 * ----------------------------------------------------------------
 */

/*
 * 行指针不计入头部大小
 */
#define SizeOfPageHeaderData (offsetof(PageHeaderData, pd_linp))

/*
 * PageIsEmpty
 *		页面上尚未分配任何 itemid 时返回真
 */
static inline bool
PageIsEmpty(const PageData *page)
{
	return ((const PageHeaderData *) page)->pd_lower <= SizeOfPageHeaderData;
}

/*
 * PageIsNew
 *		页面尚未初始化（通过 PageInit）时返回真
 */
static inline bool
PageIsNew(const PageData *page)
{
	return ((const PageHeaderData *) page)->pd_upper == 0;
}

/*
 * PageGetItemId
 *		返回页面的一项标识符。
 */
static inline ItemId
PageGetItemId(Page page, OffsetNumber offsetNumber)
{
	return &((PageHeader) page)->pd_linp[offsetNumber - 1];
}

/*
 * PageGetContents
 *		用于页面不包含行指针的情况。
 *
 * 注意：8.3 之前，不能保证返回 MAXALIGN 对齐的结果。
 * 现在可以保证。请小心那些认为内容偏移量只是 SizeOfPageHeaderData
 * 而非 MAXALIGN(SizeOfPageHeaderData) 的旧代码。
 */
static inline char *
PageGetContents(Page page)
{
	return (char *) page + MAXALIGN(SizeOfPageHeaderData);
}

/* ----------------
 *		访问页面大小信息的函数
 * ----------------
 */

/*
 * PageGetPageSize
 *		返回页面的页大小。
 *
 * 只能在已格式化的页面上调用（不同于 BufferGetPageSize，
 * 后者可以在未格式化的页面上调用）。
 * 但可以在不存储在缓冲区中的页面上调用。
 */
static inline Size
PageGetPageSize(const PageData *page)
{
	return (Size) (((const PageHeaderData *) page)->pd_pagesize_version & (uint16) 0xFF00);
}

/*
 * PageGetPageLayoutVersion
 *		返回页面的页布局版本。
 */
static inline uint8
PageGetPageLayoutVersion(const PageData *page)
{
	return (((const PageHeaderData *) page)->pd_pagesize_version & 0x00FF);
}

/*
 * PageSetPageSizeAndVersion
 *		设置页面的页大小和页布局版本号。
 *
 * 我们可以支持分别设置这两个值，但目前没有实际需求。
 */
static inline void
PageSetPageSizeAndVersion(Page page, Size size, uint8 version)
{
	Assert((size & 0xFF00) == size);
	Assert((version & 0x00FF) == version);

	((PageHeader) page)->pd_pagesize_version = size | version;
}

/* ----------------
 *		页面特殊空间数据函数
 * ----------------
 */
/*
 * PageGetSpecialSize
 *		返回页面上特殊空间的大小。
 */
static inline uint16
PageGetSpecialSize(const PageData *page)
{
	return (PageGetPageSize(page) - ((const PageHeaderData *) page)->pd_special);
}

/*
 * 使用断言验证页面特殊空间指针是否合法。
 *
 * 用于在页面初始化之前捕获对特殊空间指针的访问。
 */
static inline void
PageValidateSpecialPointer(const PageData *page)
{
	Assert(page);
	Assert(((const PageHeaderData *) page)->pd_special <= BLCKSZ);
	Assert(((const PageHeaderData *) page)->pd_special >= SizeOfPageHeaderData);
}

/*
 * PageGetSpecialPointer
 *		返回指向页面上特殊空间的指针。
 */
#define PageGetSpecialPointer(page) \
( \
	PageValidateSpecialPointer(page), \
	((page) + ((PageHeader) (page))->pd_special) \
)

/*
 * PageGetItem
 *		检索给定页面上的指定项。
 *
 * 注意：
 *		这不会改变任何传入资源的状态。
 *		语义可能在未来发生变化。
 */
static inline Item
PageGetItem(const PageData *page, const ItemIdData *itemId)
{
	Assert(page);
	Assert(ItemIdHasStorage(itemId));

	return (Item) (((const char *) page) + ItemIdGetOffset(itemId));
}

/*
 * PageGetMaxOffsetNumber
 *		返回给定页面使用的最大偏移号。
 *		由于偏移号是从 1 开始的，这也是页面上的项数。
 *
 *		注意：如果页面未初始化（pd_lower == 0），必须返回零
 *		以确保行为正确。
 */
static inline OffsetNumber
PageGetMaxOffsetNumber(const PageData *page)
{
	const PageHeaderData *pageheader = (const PageHeaderData *) page;

	if (pageheader->pd_lower <= SizeOfPageHeaderData)
		return 0;
	else
		return (pageheader->pd_lower - SizeOfPageHeaderData) / sizeof(ItemIdData);
}

/*
 * 访问页面头部的附加函数。
 */
static inline XLogRecPtr
PageGetLSN(const PageData *page)
{
	return PageXLogRecPtrGet(((const PageHeaderData *) page)->pd_lsn);
}
static inline void
PageSetLSN(Page page, XLogRecPtr lsn)
{
	PageXLogRecPtrSet(((PageHeader) page)->pd_lsn, lsn);
}

static inline bool
PageHasFreeLinePointers(const PageData *page)
{
	return ((const PageHeaderData *) page)->pd_flags & PD_HAS_FREE_LINES;
}
static inline void
PageSetHasFreeLinePointers(Page page)
{
	((PageHeader) page)->pd_flags |= PD_HAS_FREE_LINES;
}
static inline void
PageClearHasFreeLinePointers(Page page)
{
	((PageHeader) page)->pd_flags &= ~PD_HAS_FREE_LINES;
}

static inline bool
PageIsFull(const PageData *page)
{
	return ((const PageHeaderData *) page)->pd_flags & PD_PAGE_FULL;
}
static inline void
PageSetFull(Page page)
{
	((PageHeader) page)->pd_flags |= PD_PAGE_FULL;
}
static inline void
PageClearFull(Page page)
{
	((PageHeader) page)->pd_flags &= ~PD_PAGE_FULL;
}

static inline bool
PageIsAllVisible(const PageData *page)
{
	return ((const PageHeaderData *) page)->pd_flags & PD_ALL_VISIBLE;
}
static inline void
PageSetAllVisible(Page page)
{
	((PageHeader) page)->pd_flags |= PD_ALL_VISIBLE;
}
static inline void
PageClearAllVisible(Page page)
{
	((PageHeader) page)->pd_flags &= ~PD_ALL_VISIBLE;
}

/*
 * 以下两个需要 access/transam.h，因此保持为宏。
 */
#define PageSetPrunable(page, xid) \
do { \
	Assert(TransactionIdIsNormal(xid)); \
	if (!TransactionIdIsValid(((PageHeader) (page))->pd_prune_xid) || \
		TransactionIdPrecedes(xid, ((PageHeader) (page))->pd_prune_xid)) \
		((PageHeader) (page))->pd_prune_xid = (xid); \
} while (0)
#define PageClearPrunable(page) \
	(((PageHeader) (page))->pd_prune_xid = InvalidTransactionId)


/* ----------------------------------------------------------------
 *		外部函数声明
 * ----------------------------------------------------------------
 */

/* PageAddItemExtended() 的标志 */
#define PAI_OVERWRITE			(1 << 0)
#define PAI_IS_HEAP				(1 << 1)

/* PageIsVerified() 的标志 */
#define PIV_LOG_WARNING			(1 << 0)
#define PIV_LOG_LOG				(1 << 1)
#define PIV_IGNORE_CHECKSUM_FAILURE (1 << 2)

#define PageAddItem(page, item, size, offsetNumber, overwrite, is_heap) \
	PageAddItemExtended(page, item, size, offsetNumber, \
						((overwrite) ? PAI_OVERWRITE : 0) | \
						((is_heap) ? PAI_IS_HEAP : 0))

/*
 * 检查 BLCKSZ 是 sizeof(size_t) 的倍数。在 PageIsVerified() 中，使用本机字长
 * 检查页面是否为全零要快得多。注意，这个断言保留在头文件中，以确保
 * StaticAssertDecl() 能在各种平台和编译器组合下正常工作。
 */
StaticAssertDecl(BLCKSZ == ((BLCKSZ / sizeof(size_t)) * sizeof(size_t)),
				 "BLCKSZ has to be a multiple of sizeof(size_t)");

extern void PageInit(Page page, Size pageSize, Size specialSize);
extern bool PageIsVerified(PageData *page, BlockNumber blkno, int flags,
						   bool *checksum_failure_p);
extern OffsetNumber PageAddItemExtended(Page page, Item item, Size size,
										OffsetNumber offsetNumber, int flags);
extern Page PageGetTempPage(const PageData *page);
extern Page PageGetTempPageCopy(const PageData *page);
extern Page PageGetTempPageCopySpecial(const PageData *page);
extern void PageRestoreTempPage(Page tempPage, Page oldPage);
extern void PageRepairFragmentation(Page page);
extern void PageTruncateLinePointerArray(Page page);
extern Size PageGetFreeSpace(const PageData *page);
extern Size PageGetFreeSpaceForMultipleTuples(const PageData *page, int ntups);
extern Size PageGetExactFreeSpace(const PageData *page);
extern Size PageGetHeapFreeSpace(const PageData *page);
extern void PageIndexTupleDelete(Page page, OffsetNumber offnum);
extern void PageIndexMultiDelete(Page page, OffsetNumber *itemnos, int nitems);
extern void PageIndexTupleDeleteNoCompact(Page page, OffsetNumber offnum);
extern bool PageIndexTupleOverwrite(Page page, OffsetNumber offnum,
									Item newtup, Size newsize);
extern char *PageSetChecksumCopy(Page page, BlockNumber blkno);
extern void PageSetChecksumInplace(Page page, BlockNumber blkno);

#endif							/* BUFPAGE_H */
