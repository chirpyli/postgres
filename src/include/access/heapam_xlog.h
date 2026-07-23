/*-------------------------------------------------------------------------
 *
 * heapam_xlog.h
 *	  POSTGRES 堆访问 XLOG 定义。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/heapam_xlog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef HEAPAM_XLOG_H
#define HEAPAM_XLOG_H

#include "access/htup.h"
#include "access/xlogreader.h"
#include "lib/stringinfo.h"
#include "storage/buf.h"
#include "storage/bufpage.h"
#include "storage/relfilelocator.h"
#include "storage/sinval.h"
#include "utils/relcache.h"


/*
 * heapam.c 的 WAL 操作的记录定义
 *
 * XLOG 允许将一些信息存放在日志记录 xl_info 字段的高 4 位中。
 * 我们用其中 3 位作为操作码，1 位作为 init 位。
 */
#define XLOG_HEAP_INSERT		0x00
#define XLOG_HEAP_DELETE		0x10
#define XLOG_HEAP_UPDATE		0x20
#define XLOG_HEAP_TRUNCATE		0x30
#define XLOG_HEAP_HOT_UPDATE	0x40
#define XLOG_HEAP_CONFIRM		0x50
#define XLOG_HEAP_LOCK			0x60
#define XLOG_HEAP_INPLACE		0x70

#define XLOG_HEAP_OPMASK		0x70
/*
 * 当我们在 INSERT、UPDATE、HOT_UPDATE 或 MULTI_INSERT 中向新页面
 * 插入第一个元组时，可以（并且确实会）在 redo 时恢复整个页面
 */
#define XLOG_HEAP_INIT_PAGE		0x80
/*
 * 操作码已经用尽，因此 heapam.c 现在有了第二个 RmgrId。这些操作码
 * 与 RM_HEAP2_ID 相关联，但在逻辑上与上面和 RM_HEAP_ID 相关联的
 * 那些并无不同。XLOG_HEAP_OPMASK 同样适用于它们。
 *
 * XLOG_HEAP2_PRUNE_ON_ACCESS、XLOG_HEAP2_PRUNE_VACUUM_SCAN 与
 * XLOG_HEAP2_PRUNE_VACUUM_CLEANUP 这几种记录之间并无区别。它们
 * 之所以有各自独立的操作码，仅仅是为了调试和分析目的，以表明
 * 该 WAL 记录是因何而发出的。
 */
#define XLOG_HEAP2_REWRITE		0x00
#define XLOG_HEAP2_PRUNE_ON_ACCESS		0x10
#define XLOG_HEAP2_PRUNE_VACUUM_SCAN	0x20
#define XLOG_HEAP2_PRUNE_VACUUM_CLEANUP	0x30
#define XLOG_HEAP2_VISIBLE		0x40
#define XLOG_HEAP2_MULTI_INSERT 0x50
#define XLOG_HEAP2_LOCK_UPDATED 0x60
#define XLOG_HEAP2_NEW_CID		0x70

/*
 * xl_heap_insert / xl_heap_multi_insert 的标志值，共有 8 位可用。
 */
/* PD_ALL_VISIBLE 已被清除 */
#define XLH_INSERT_ALL_VISIBLE_CLEARED			(1<<0)
#define XLH_INSERT_LAST_IN_MULTI				(1<<1)
#define XLH_INSERT_IS_SPECULATIVE				(1<<2)
#define XLH_INSERT_CONTAINS_NEW_TUPLE			(1<<3)
#define XLH_INSERT_ON_TOAST_RELATION			(1<<4)

/* all_frozen_set 总是隐含 all_visible_set */
#define XLH_INSERT_ALL_FROZEN_SET				(1<<5)

/*
 * xl_heap_update 的标志值，共有 8 位可用。
 */
/* PD_ALL_VISIBLE 已被清除 */
#define XLH_UPDATE_OLD_ALL_VISIBLE_CLEARED		(1<<0)
/* 第 2 个页面中的 PD_ALL_VISIBLE 已被清除 */
#define XLH_UPDATE_NEW_ALL_VISIBLE_CLEARED		(1<<1)
#define XLH_UPDATE_CONTAINS_OLD_TUPLE			(1<<2)
#define XLH_UPDATE_CONTAINS_OLD_KEY				(1<<3)
#define XLH_UPDATE_CONTAINS_NEW_TUPLE			(1<<4)
#define XLH_UPDATE_PREFIX_FROM_OLD				(1<<5)
#define XLH_UPDATE_SUFFIX_FROM_OLD				(1<<6)

/* 用于检查是否记录了任何形式的旧元组的便利宏 */
#define XLH_UPDATE_CONTAINS_OLD						\
	(XLH_UPDATE_CONTAINS_OLD_TUPLE | XLH_UPDATE_CONTAINS_OLD_KEY)

/*
 * xl_heap_delete 的标志值，共有 8 位可用。
 */
/* PD_ALL_VISIBLE 已被清除 */
#define XLH_DELETE_ALL_VISIBLE_CLEARED			(1<<0)
#define XLH_DELETE_CONTAINS_OLD_TUPLE			(1<<1)
#define XLH_DELETE_CONTAINS_OLD_KEY				(1<<2)
#define XLH_DELETE_IS_SUPER						(1<<3)
#define XLH_DELETE_IS_PARTITION_MOVE			(1<<4)

/* 用于检查是否记录了任何形式的旧元组的便利宏 */
#define XLH_DELETE_CONTAINS_OLD						\
	(XLH_DELETE_CONTAINS_OLD_TUPLE | XLH_DELETE_CONTAINS_OLD_KEY)

/* 关于 delete，我们需要知道的信息如下 */
typedef struct xl_heap_delete
{
	TransactionId xmax;			/* 被删除元组的 xmax */
	OffsetNumber offnum;		/* 被删除元组的偏移量 */
	uint8		infobits_set;	/* infomask 位 */
	uint8		flags;
} xl_heap_delete;

#define SizeOfHeapDelete	(offsetof(xl_heap_delete, flags) + sizeof(uint8))

/*
 * xl_heap_truncate 的标志值，共有 8 位可用。
 */
#define XLH_TRUNCATE_CASCADE					(1<<0)
#define XLH_TRUNCATE_RESTART_SEQS				(1<<1)

/*
 * 对于 truncate，我们将所有被截断的 relid 列在一个数组中，随后
 * （如果有的话）列出所有需要重新开始的序列 relid。
 * 所有关系始终位于同一个数据库中，因此我们只需要列出一次 dbid。
 */
typedef struct xl_heap_truncate
{
	Oid			dbId;
	uint32		nrelids;
	uint8		flags;
	Oid			relids[FLEXIBLE_ARRAY_MEMBER];
} xl_heap_truncate;

#define SizeOfHeapTruncate	(offsetof(xl_heap_truncate, relids))

/*
 * 对于被插入或更新的元组，我们不会将其整个固定部分
 * （HeapTupleHeaderData）存入 WAL；我们可以利用 WAL 记录中
 * 其他地方已有的字段来重建某些字段，或者某些字段本来就
 * 无需重建，从而节省几个字节。以下是我们必须存储的字段。
 */
typedef struct xl_heap_header
{
	uint16		t_infomask2;
	uint16		t_infomask;
	uint8		t_hoff;
} xl_heap_header;

#define SizeOfHeapHeader	(offsetof(xl_heap_header, t_hoff) + sizeof(uint8))

/* 关于 insert，我们需要知道的信息如下 */
typedef struct xl_heap_insert
{
	OffsetNumber offnum;		/* 被插入元组的偏移量 */
	uint8		flags;

	/* xl_heap_header 与 TUPLE DATA 位于备份块 0 中 */
} xl_heap_insert;

#define SizeOfHeapInsert	(offsetof(xl_heap_insert, flags) + sizeof(uint8))

/*
 * 关于 multi-insert，我们需要知道的信息如下。
 *
 * 记录的主要数据由这个 xl_heap_multi_insert 头部组成。如果整个
 * 页面被重新初始化（XLOG_HEAP_INIT_PAGE），则会省略 'offsets' 数组。
 *
 * 在块 0 的数据部分中，存在一个 xl_multi_insert_tuple 结构体，
 * 其后紧跟着每个元组的元组数据。结构体之间有填充以对齐每个
 * xl_multi_insert_tuple 结构体。
 */
typedef struct xl_heap_multi_insert
{
	uint8		flags;
	uint16		ntuples;
	OffsetNumber offsets[FLEXIBLE_ARRAY_MEMBER];
} xl_heap_multi_insert;

#define SizeOfHeapMultiInsert	offsetof(xl_heap_multi_insert, offsets)

typedef struct xl_multi_insert_tuple
{
	uint16		datalen;		/* 随后跟随的元组数据的大小 */
	uint16		t_infomask2;
	uint16		t_infomask;
	uint8		t_hoff;
	/* 元组数据位于结构体末尾 */
} xl_multi_insert_tuple;

#define SizeOfMultiInsertTuple	(offsetof(xl_multi_insert_tuple, t_hoff) + sizeof(uint8))

/*
 * 关于 update|hot_update，我们需要知道的信息如下
 *
 * 备份块 0：新页面
 *
 * 如果设置了 XLH_UPDATE_PREFIX_FROM_OLD 或 XLH_UPDATE_SUFFIX_FROM_OLD
 * 标志，则前缀和/或后缀会先行出现，各占一个或两个 uint16。
 *
 * 其后跟随 xl_heap_header 与新的元组数据。新的元组数据不包含
 * 前缀和后缀，它们会在重放时从旧元组中复制。
 *
 * 如果给定了 XLH_UPDATE_CONTAINS_NEW_TUPLE 标志，则即便已拍摄了
 * 整页镜像，元组数据也会被包含在内。
 *
 * 备份块 1：旧页面（如果不同）。（无数据，仅是对该块的引用）
 */
typedef struct xl_heap_update
{
	TransactionId old_xmax;		/* 旧元组的 xmax */
	OffsetNumber old_offnum;	/* 旧元组的偏移量 */
	uint8		old_infobits_set;	/* 要在旧元组上设置的 infomask 位 */
	uint8		flags;
	TransactionId new_xmax;		/* 新元组的 xmax */
	OffsetNumber new_offnum;	/* 新元组的偏移量 */

	/*
	 * 如果设置了 XLH_UPDATE_CONTAINS_OLD_TUPLE 或
	 * XLH_UPDATE_CONTAINS_OLD_KEY 标志，则其后跟随旧元组的
	 * xl_heap_header 与元组数据。
	 */
} xl_heap_update;

#define SizeOfHeapUpdate	(offsetof(xl_heap_update, new_offnum) + sizeof(OffsetNumber))

/*
 * 这些结构体与标志用于编码 VACUUM 修剪、冻结以及在访问时进行的
 * 页面修剪修改。
 *
 * xl_heap_prune 是主记录。XLHP_HAS_* 标志指示包含哪些“子记录”，
 * 其他 XLHP_* 标志则提供关于重放条件的额外信息。
 *
 * 块引用 0 的数据包含“子记录”，具体内容取决于哪些 XLHP_HAS_*
 * 标志被设置。参见下面的 xlhp_* 结构体定义。子记录的出现顺序与
 * XLHP_* 标志的顺序一致。下面是包含所有子记录的一个示例记录：
 *
 *-----------------------------------------------------------------------------
 * Main data section:
 *
 *	xl_heap_prune
 *		uint8				flags
 *	TransactionId			snapshot_conflict_horizon
 *
 * Block 0 data section:
 *
 *	xlhp_freeze_plans
 *		uint16				nplans
 *		[2 bytes of padding]
 *		xlhp_freeze_plan	plans[nplans]
 *
 *	xlhp_prune_items
 *		uint16				nredirected
 *		OffsetNumber		redirected[2 * nredirected]
 *
 *	xlhp_prune_items
 *		uint16				ndead
 *		OffsetNumber		nowdead[ndead]
 *
 *	xlhp_prune_items
 *		uint16				nunused
 *		OffsetNumber		nowunused[nunused]
 *
 *	OffsetNumber			frz_offsets[sum([plan.ntuples for plan in plans])]
 *-----------------------------------------------------------------------------
 *
 * 注意：由于记录数据由多个可选部分拼装而成，我们必须密切关注对齐。
 * 在主数据段中，'snapshot_conflict_horizon' 紧接在 'flags' 之后以
 * 未对齐方式存储，以节省空间。在块 0 数据段中，冻结计划排在最先，
 * 因为它们包含需要 4 字节对齐的 TransactionId 字段。其余字段只需
 * 2 字节对齐。这也是 'frz_offsets' 被单独存储、未与 xlhp_freeze_plan
 * 结构体放在一起的原因。
 */
typedef struct xl_heap_prune
{
	uint8		reason;
	uint8		flags;

	/*
	 * 如果设置了 XLHP_HAS_CONFLICT_HORIZON，则其后跟随冲突
	 * 边界 XID，且为未对齐存储。
	 */
} xl_heap_prune;

#define SizeOfHeapPrune (offsetof(xl_heap_prune, flags) + sizeof(uint8))

/* 用于在备机上进行逻辑解码时处理恢复冲突 */
#define		XLHP_IS_CATALOG_REL			(1 << 1)

/*
 * 重放该记录是否需要一个 cleanup 锁？
 *
 * 在 VACUUM 的第一遍扫描中，或在以其他方式访问页面时进行的修剪，
 * 都需要一个 cleanup 锁。而对于冻结，以及 VACUUM 将 LP_DEAD 行
 * 指针标记为未使用、且不移动任何元组数据的第二遍扫描，只需一个
 * 普通的排他锁即可。
 */
#define		XLHP_CLEANUP_LOCK	       (1 << 2)

/*
 * 如果我们移除或冻结了任何包含 xid 的条目，就需要包含一个
 * 快照冲突边界。它在 Hot Standby 模式下用于确保没有查询仍在
 * 访问那些被移除的元组（即这些元组对其仍可见），或仍将那些
 * 被冻结的 XID 视为正在运行。
 */
#define		XLHP_HAS_CONFLICT_HORIZON   (1 << 3)

/*
 * 指示存在一个 xlhp_freeze_plans 子记录以及一个或多个
 * xlhp_freeze_plan 子记录。
 */
#define		XLHP_HAS_FREEZE_PLANS		(1 << 4)

/*
 * XLHP_HAS_REDIRECTIONS、XLHP_HAS_DEAD_ITEMS 与
 * XLHP_HAS_NOW_UNUSED_ITEMS 指示存在带有重定向、死亡以及
 * 未使用项偏移量的 xlhp_prune_items 子记录。
 */
#define		XLHP_HAS_REDIRECTIONS		(1 << 5)
#define		XLHP_HAS_DEAD_ITEMS	        (1 << 6)
#define		XLHP_HAS_NOW_UNUSED_ITEMS   (1 << 7)

/*
 * xlhp_freeze_plan 描述如何冻结一组（一个或多个）堆元组
 * （出现在 xl_heap_prune 的 xlhp_freeze_plans 子记录中）
 */
/* 0x01 曾是 XLH_FREEZE_XMIN */
#define		XLH_FREEZE_XVAC		0x02
#define		XLH_INVALID_XVAC	0x04

typedef struct xlhp_freeze_plan
{
	TransactionId xmax;
	uint16		t_infomask2;
	uint16		t_infomask;
	uint8		frzflags;

	/* 本计划所对应的各页面偏移号数组的长度 */
	uint16		ntuples;
} xlhp_freeze_plan;

/*
 * 关于 VACUUM 期间被冻结的块，我们需要知道的信息如下
 *
 * 备份块的数据包含一个 xlhp_freeze_plan 结构体数组（共
 * nplans 个元素）。各个项的偏移量位于整条记录末尾的一个数组中，
 * 该数组有 nplans *（每个计划的 ntuples）个成员。这些偏移量
 * 的顺序与各计划一致。REDO 例程利用这些偏移量来冻结相应的
 * 堆元组。
 *
 * （自 PostgreSQL 17 起，XLOG_HEAP2_PRUNE_VACUUM_SCAN 记录取代了
 * 原先独立的 XLOG_HEAP2_FREEZE_PAGE 记录。）
 */
typedef struct xlhp_freeze_plans
{
	uint16		nplans;
	xlhp_freeze_plan plans[FLEXIBLE_ARRAY_MEMBER];
} xlhp_freeze_plans;

/*
 * 通用子记录类型，包含在 xl_heap_prune 记录的块引用 0 中，用于
 * 重定向、死亡以及未使用的项——前提是有任何一个
 * XLHP_HAS_REDIRECTIONS / XLHP_HAS_DEAD_ITEMS / XLHP_HAS_NOW_UNUSED_ITEMS
 * 标志被设置。注意，在 XLHP_HAS_REDIRECTIONS 变体中，数据里实际上
 * 有 2 *（项个数）个 OffsetNumber。
 */
typedef struct xlhp_prune_items
{
	uint16		ntargets;
	OffsetNumber data[FLEXIBLE_ARRAY_MEMBER];
} xlhp_prune_items;


/* infobits_set 的标志 */
#define XLHL_XMAX_IS_MULTI		0x01
#define XLHL_XMAX_LOCK_ONLY		0x02
#define XLHL_XMAX_EXCL_LOCK		0x04
#define XLHL_XMAX_KEYSHR_LOCK	0x08
#define XLHL_KEYS_UPDATED		0x10

/* xl_heap_lock / xl_heap_lock_updated 的 flag 字段的位 */
#define XLH_LOCK_ALL_FROZEN_CLEARED		0x01

/* 关于 lock，我们需要知道的信息如下 */
typedef struct xl_heap_lock
{
	TransactionId xmax;			/* 可能是一个 MultiXactId */
	OffsetNumber offnum;		/* 被锁定元组在页面上的偏移量 */
	uint8		infobits_set;	/* 要设置的 infomask 与 infomask2 位 */
	uint8		flags;			/* XLH_LOCK_* 标志位 */
} xl_heap_lock;

#define SizeOfHeapLock	(offsetof(xl_heap_lock, flags) + sizeof(uint8))

/* 关于锁定某行已更新版本，我们需要知道的信息如下 */
typedef struct xl_heap_lock_updated
{
	TransactionId xmax;
	OffsetNumber offnum;
	uint8		infobits_set;
	uint8		flags;
} xl_heap_lock_updated;

#define SizeOfHeapLockUpdated	(offsetof(xl_heap_lock_updated, flags) + sizeof(uint8))

/* 关于推测性插入的确认，我们需要知道的信息如下 */
typedef struct xl_heap_confirm
{
	OffsetNumber offnum;		/* 被确认元组在页面上的偏移量 */
} xl_heap_confirm;

#define SizeOfHeapConfirm	(offsetof(xl_heap_confirm, offnum) + sizeof(OffsetNumber))

/* 关于就地更新，我们需要知道的信息如下 */
typedef struct xl_heap_inplace
{
	OffsetNumber offnum;		/* 被更新元组在页面上的偏移量 */
	Oid			dbId;			/* MyDatabaseId */
	Oid			tsId;			/* MyDatabaseTableSpace */
	bool		relcacheInitFileInval;	/* 使 relcache 初始化文件失效 */
	int			nmsgs;			/* 共享失效消息的数量 */
	SharedInvalidationMessage msgs[FLEXIBLE_ARRAY_MEMBER];
} xl_heap_inplace;

#define MinSizeOfHeapInplace	(offsetof(xl_heap_inplace, nmsgs) + sizeof(int))

/*
 * 关于设置可见性映射位，我们需要知道的信息如下
 *
 * 备份块 0：可见性映射缓冲区
 * 备份块 1：堆缓冲区
 */
typedef struct xl_heap_visible
{
	TransactionId snapshotConflictHorizon;
	uint8		flags;
} xl_heap_visible;

#define SizeOfHeapVisible (offsetof(xl_heap_visible, flags) + sizeof(uint8))

typedef struct xl_heap_new_cid
{
	/*
	 * 存储顶层 xid，这样就不必从不同事务合并 cids
	 */
	TransactionId top_xid;
	CommandId	cmin;
	CommandId	cmax;
	CommandId	combocid;		/* 仅用于调试 */

	/*
	 * 存储 relfilelocator/ctid 对，以便于查找。
	 */
	RelFileLocator target_locator;
	ItemPointerData target_tid;
} xl_heap_new_cid;

#define SizeOfHeapNewCid (offsetof(xl_heap_new_cid, target_tid) + sizeof(ItemPointerData))

/* 逻辑重写 xlog 记录头 */
typedef struct xl_heap_rewrite_mapping
{
	TransactionId mapped_xid;	/* 可能需要看到该行的 xid */
	Oid			mapped_db;		/* 共享关系的 DbOid 或 InvalidOid */
	Oid			mapped_rel;		/* 被映射关系的 Oid */
	off_t		offset;			/* 目前已写入到的位置 */
	uint32		num_mappings;	/* 内存中映射的数量 */
	XLogRecPtr	start_lsn;		/* 重写开始时的插入 LSN */
} xl_heap_rewrite_mapping;

extern void HeapTupleHeaderAdvanceConflictHorizon(HeapTupleHeader tuple,
												  TransactionId *snapshotConflictHorizon);

extern void heap_redo(XLogReaderState *record);
extern void heap_desc(StringInfo buf, XLogReaderState *record);
extern const char *heap_identify(uint8 info);
extern void heap_mask(char *pagedata, BlockNumber blkno);
extern void heap2_redo(XLogReaderState *record);
extern void heap2_desc(StringInfo buf, XLogReaderState *record);
extern const char *heap2_identify(uint8 info);
extern void heap_xlog_logical_rewrite(XLogReaderState *r);

extern XLogRecPtr log_heap_visible(Relation rel, Buffer heap_buffer,
								   Buffer vm_buffer,
								   TransactionId snapshotConflictHorizon,
								   uint8 vmflags);

/* 位于 heapdesc.c，以便前端/后端代码可以共享 */
extern void heap_xlog_deserialize_prune_and_freeze(char *cursor, uint8 flags,
												   int *nplans, xlhp_freeze_plan **plans,
												   OffsetNumber **frz_offsets,
												   int *nredirected, OffsetNumber **redirected,
												   int *ndead, OffsetNumber **nowdead,
												   int *nunused, OffsetNumber **nowunused);

#endif							/* HEAPAM_XLOG_H */
