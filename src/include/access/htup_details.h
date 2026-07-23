/*-------------------------------------------------------------------------
 *
 * htup_details.h
 *	  POSTGRES 堆元组头部定义。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/htup_details.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef HTUP_DETAILS_H
#define HTUP_DETAILS_H

#include "access/htup.h"
#include "access/transam.h"
#include "access/tupdesc.h"
#include "access/tupmacs.h"
#include "storage/bufpage.h"
#include "varatt.h"

/*
 * MaxTupleAttributeNumber 限制了元组中（用户）列的数量。
 * 该值的关键限制在于：元组的固定开销大小，加上 null 值位图的大小
 * （每列 1 比特），再加上 MAXALIGN 对齐，必须能放进 t_hoff 中，
 * 而 t_hoff 是 uint8。在大多数机器上，在不加宽 t_hoff 的情况下，
 * 上限会略高于 1700。这里以及 MaxHeapAttributeNumber 处我们取整，
 * 这样 HeapTupleHeaderData 布局的变动不会改变所支持的最大列数。
 */
#define MaxTupleAttributeNumber 1664	/* 8 * 208 */

/*
 * MaxHeapAttributeNumber 限制了表中的（用户）列数量。
 * 它应该比 MaxTupleAttributeNumber 略小。它必须至少小 1，否则我们将
 * 无法对最大宽度的表执行 UPDATE（因为 UPDATE 必须构造包含 CTID 的
 * 工作元组）。在实践中我们希望留出一定的余量，以便能够优雅地支持
 * 添加隐藏"resjunk"列的操作，例如
 * SELECT * FROM wide_table ORDER BY foo, bar, baz。
 * 无论如何，取决于列的数据类型，如果你有超过一千列左右，很可能会
 * 撞上基于磁盘块的元组总大小限制。TOAST 对此无能为力。
 */
#define MaxHeapAttributeNumber	1600	/* 8 * 200 */

/*
 * 堆元组头部。为了避免浪费空间，字段的布局方式应避免结构体填充（padding）。
 *
 * 组合类型（行类型）的 Datum 与磁盘元组共享相同的通用结构，因此可以使用
 * 相同的例程来构造和检查它们。但需求略有不同：Datum 不需要任何事务可见性
 * 信息，而它确实需要一个长度字和一些内嵌的类型信息。我们可以通过将堆元组
 * 的 xmin/cmin/xmax/cmax/xvac 字段与 Datum 情况下所需的字段进行重叠（overlay）
 * 来实现这一点。通常，所有在内存中构造的元组都会用 Datum 字段来初始化；
 * 但当元组即将被插入表中时，事务字段会被填充，从而覆盖掉 Datum 字段。
 *
 * 堆元组的整体结构如下：
 *			固定字段（HeapTupleHeaderData 结构体）
 *			null 位图（如果 t_infomask 中设置了 HEAP_HASNULL）
 *			对齐填充（按需使UserData 按 MAXALIGN 对齐）
 *			对象 ID（如果 t_infomask 中设置了 HEAP_HASOID_OLD，已不再创建）
 *			用户数据字段
 *
 * 我们将五个"虚拟"字段 Xmin、Cmin、Xmax、Cmax 和 Xvac 存放在三个物理字段中。
 * Xmin 和 Xmax 总是真正存储的，但 Cmin、Cmax 和 Xvac 共享一个字段。这之所以
 * 可行，是因为我们知道 Cmin 和 Cmax 分别只在插入事务和删除事务的生命周期内有意义。
 * 如果一个元组在同一事务中被插入又删除，我们会存储一个"组合"命令 ID，它可以被
 * 映射回真正的 cmin 和 cmax，但这只能通过发起事务的后端进程中的本地状态来实现。
 * 详见 combocid.c。同时，Xvac 仅由旧式 VACUUM FULL 设置，它没有任何命令子结构，
 * 因此既不需要 Cmin 也不需要 Cmax。（这要求旧式 VACUUM FULL 绝不尝试移动一个
 * Cmin 或 Cmax 仍有意义的元组，即仍在插入中或删除中的元组。）
 *
 * 关于 t_ctid：每当一个新元组被存入磁盘，它的 t_ctid 会用自身的 TID（位置）
 * 初始化。如果元组被更新，它的 t_ctid 会被改为指向该元组的替换版本。或者，如果
 * 由于分区键被更新，元组从一个分区移动到另一个分区，t_ctid 会被设为一个特殊值
 * 以表示这一点（见 ItemPointerSetMovedPartitions）。因此，一个元组是其行的最新版本，
 * 当且仅当 XMAX 无效，或 t_ctid 指向自身（在后一种情况下，如果 XMAX 有效，
 * 则该元组要么被锁定，要么被删除）。人们可以沿着 t_ctid 链接链找到该行的最新版本，
 * 除非它被移动到了不同的分区。但是要注意，VACUUM 可能会在被指向的（较新）元组
 * 之前擦除指向它的（较旧）元组。因此，在跟随 t_ctid 链接时，有必要检查
 * 被引用的槽位是否为空或包含一个无关的元组。需检查被引用元组的 XMIN 是否
 * 等于引用元组的 XMAX，以验证它确实是后代版本，而非一个填入最近被 VACUUM 释放的
 * 槽位中的无关元组。如果其中任一检查失败，可以认为不存在存活的后代版本。
 *
 * t_ctid 有时用于存储一个推测插入令牌（speculative insertion token），而不是
 * 真正的 TID。推测令牌会被设置在一个正在被插入的元组上，直到插入者确定它
 * 想要继续完成插入。因此令牌只应出现在一个 XMAX 仍处于进行中、或无效/已中止的
 * 元组上。当插入被确认时，令牌会被替换为该元组的真实 TID。在跟随 t_ctid 链接链时
 * 绝不应看到推测插入令牌，因为它们不用于更新，只用于插入。
 *
 * 在固定头部字段之后，存储 null 位图（从 t_bits 开始）。如果 t_infomask 表明
 * 元组中没有 null，则不存储该位图。如果存在 OID 字段（由 t_infomask 指示），
 * 则它存储在UserData 之前，而用户数据从 t_hoff 所示偏移处开始。注意 t_hoff 必须
 * 是 MAXALIGN 的倍数。
 */

typedef struct HeapTupleFields
{
	TransactionId t_xmin;		/* 插入该元组的事务 ID */
	TransactionId t_xmax;		/* 删除或锁定该元组的事务 ID */

	union
	{
		CommandId	t_cid;		/* 插入或删除的命令 ID，或两者兼有 */
		TransactionId t_xvac;	/* 旧式 VACUUM FULL 的事务 ID */
	}			t_field3;
} HeapTupleFields;

typedef struct DatumTupleFields
{
	int32		datum_len_;		/* varlena 头部（不要直接修改！） */

	int32		datum_typmod;	/* -1，或记录类型的标识符 */

	Oid			datum_typeid;	/* 组合类型的 OID，或 RECORDOID */

	/*
	 * datum_typeid 不能是组合类型之上的域，只能是普通组合类型，
	 * 即使该 Datum 本意是 domain-over-composite 类型的值。
	 * 这与 CoerceToDomain 不改变基础类型值的物理表示这一通用原则一致。
	 *
	 * 注意：字段顺序的选择考虑到了 Oid 将来某天可能扩展到 64 位。
	 */
} DatumTupleFields;

struct HeapTupleHeaderData
{
	union
	{
		HeapTupleFields t_heap;
		DatumTupleFields t_datum;
	}			t_choice;

	ItemPointerData t_ctid;		/* 本元组或更新元组的当前 TID（或
								 * 一个推测插入令牌） */

	/* 此行以下的字段必须与 MinimalTupleData 匹配！ */

#define FIELDNO_HEAPTUPLEHEADERDATA_INFOMASK2 2
	uint16		t_infomask2;	/* 属性数量 + 各种标志位 */

#define FIELDNO_HEAPTUPLEHEADERDATA_INFOMASK 3
	uint16		t_infomask;		/* 各种标志位，见下文 */

#define FIELDNO_HEAPTUPLEHEADERDATA_HOFF 4
	uint8		t_hoff;			/* 包含位图和填充的头部大小 */

	/* ^ - 23 字节 - ^ */

#define FIELDNO_HEAPTUPLEHEADERDATA_BITS 5
	bits8		t_bits[FLEXIBLE_ARRAY_MEMBER];	/* NULL 位图 */

	/* 结构体末尾后还有更多数据 */
};

/* 类型定义在 htup.h 中 */

#define SizeofHeapTupleHeader offsetof(HeapTupleHeaderData, t_bits)

/*
 * 存放在 t_infomask 中的信息：
 */
#define HEAP_HASNULL			0x0001	/* 含有 null 属性 */
#define HEAP_HASVARWIDTH		0x0002	/* 含有变宽属性 */
#define HEAP_HASEXTERNAL		0x0004	/* 含有外部存储的属性 */
#define HEAP_HASOID_OLD			0x0008	/* 含有一个对象 ID 字段 */
#define HEAP_XMAX_KEYSHR_LOCK	0x0010	/* xmax 是一个键共享锁持有者 */
#define HEAP_COMBOCID			0x0020	/* t_cid 是一个组合 CID */
#define HEAP_XMAX_EXCL_LOCK		0x0040	/* xmax 是一个排他锁持有者 */
#define HEAP_XMAX_LOCK_ONLY		0x0080	/* xmax 如果有效，则仅仅是一个锁持有者 */

 /* xmax 是一个共享锁持有者 */
#define HEAP_XMAX_SHR_LOCK	(HEAP_XMAX_EXCL_LOCK | HEAP_XMAX_KEYSHR_LOCK)

#define HEAP_LOCK_MASK	(HEAP_XMAX_SHR_LOCK | HEAP_XMAX_EXCL_LOCK | \
						 HEAP_XMAX_KEYSHR_LOCK)
#define HEAP_XMIN_COMMITTED		0x0100	/* t_xmin 已提交 */
#define HEAP_XMIN_INVALID		0x0200	/* t_xmin 无效/已中止 */
#define HEAP_XMIN_FROZEN		(HEAP_XMIN_COMMITTED|HEAP_XMIN_INVALID)
#define HEAP_XMAX_COMMITTED		0x0400	/* t_xmax 已提交 */
#define HEAP_XMAX_INVALID		0x0800	/* t_xmax 无效/已中止 */
#define HEAP_XMAX_IS_MULTI		0x1000	/* t_xmax 是一个 MultiXactId */
#define HEAP_UPDATED			0x2000	/* 这是该行的 UPDATEd 版本 */
#define HEAP_MOVED_OFF			0x4000	/* 被 9.0 之前的 VACUUM FULL 移动到了
										 * 别处；为二进制升级兼容而保留 */
#define HEAP_MOVED_IN			0x8000	/* 被 9.0 之前的 VACUUM FULL 从
										 * 别处移入；为二进制升级兼容而保留 */
#define HEAP_MOVED (HEAP_MOVED_OFF | HEAP_MOVED_IN)

#define HEAP_XACT_MASK			0xFFF0	/* 与可见性相关的位 */

/*
 * 一个元组仅在设置了 HEAP_XMAX_LOCK_ONLY 位时才是被锁定的（即未被其 Xmax
 * 更新）；或者，为了 pg_upgrade 的兼容，如果 Xmax 不是 multi 且设置了
 * EXCL_LOCK 位，也算锁定。
 *
 * 另见 HeapTupleHeaderIsOnlyLocked，它还会检查可能存在的已中止更新者事务。
 */
static inline bool
HEAP_XMAX_IS_LOCKED_ONLY(uint16 infomask)
{
	return (infomask & HEAP_XMAX_LOCK_ONLY) ||
		(infomask & (HEAP_XMAX_IS_MULTI | HEAP_LOCK_MASK)) == HEAP_XMAX_EXCL_LOCK;
}

/*
 * 一个元组同时具有 HEAP_XMAX_IS_MULTI 和 HEAP_XMAX_LOCK_ONLY，但不具有
 * HEAP_XMAX_EXCL_LOCK 也不具有 HEAP_XMAX_KEYSHR_LOCK，必然来自一个在 9.2 或更早
 * 版本中被共享锁定、然后经过 pg_upgrade 的元组。
 *
 * 在 9.2 及更早版本中，HEAP_XMAX_IS_MULTI 仅在该元组有多个 FOR SHARE 锁持有者时
 * 才会被设置。这会设置 HEAP_XMAX_LOCK_ONLY（当时名字不同），但既不设置
 * HEAP_XMAX_EXCL_LOCK 也不设置 HEAP_XMAX_KEYSHR_LOCK。这种组合在 9.3 及之后
 * 不再可能出现，因此如果我们看到这种组合，就可以确定该元组是在更早的版本中
 * 被锁定的；由于所有此类锁持有者都早已消失（它们无法在 pg_upgrade 中存活），
 * 这样的元组可以安全地被视为未被锁定。
 *
 * 我们绝不能本地解析此类 multixact，因为无论它们相对于当前有效 multixact 范围
 * 处于何种位置，结果都是错误的。
 */
static inline bool
HEAP_LOCKED_UPGRADED(uint16 infomask)
{
	return
		(infomask & HEAP_XMAX_IS_MULTI) != 0 &&
		(infomask & HEAP_XMAX_LOCK_ONLY) != 0 &&
		(infomask & (HEAP_XMAX_EXCL_LOCK | HEAP_XMAX_KEYSHR_LOCK)) == 0;
}

/*
 * 使用这些宏来测试某个特定的锁是否应用于元组
 */
static inline bool
HEAP_XMAX_IS_SHR_LOCKED(int16 infomask)
{
	return (infomask & HEAP_LOCK_MASK) == HEAP_XMAX_SHR_LOCK;
}

static inline bool
HEAP_XMAX_IS_EXCL_LOCKED(int16 infomask)
{
	return (infomask & HEAP_LOCK_MASK) == HEAP_XMAX_EXCL_LOCK;
}

static inline bool
HEAP_XMAX_IS_KEYSHR_LOCKED(int16 infomask)
{
	return (infomask & HEAP_LOCK_MASK) == HEAP_XMAX_KEYSHR_LOCK;
}

/* 当 Xmax 即将改变时，将这些位全部关闭 */
#define HEAP_XMAX_BITS (HEAP_XMAX_COMMITTED | HEAP_XMAX_INVALID | \
						HEAP_XMAX_IS_MULTI | HEAP_LOCK_MASK | HEAP_XMAX_LOCK_ONLY)

/*
 * 存放在 t_infomask2 中的信息：
 */
#define HEAP_NATTS_MASK			0x07FF	/* 用于属性数量的 11 位 */
/* 0x1800 位可用 */
#define HEAP_KEYS_UPDATED		0x2000	/* 元组被更新且键列被修改，
										 * 或元组被删除 */
#define HEAP_HOT_UPDATED		0x4000	/* 元组被 HOT 更新 */
#define HEAP_ONLY_TUPLE			0x8000	/* 这是一个 heap-only 元组 */

#define HEAP2_XACT_MASK			0xE000	/* 与可见性相关的位 */

/*
 * HEAP_TUPLE_HAS_MATCH 是哈希连接期间使用的临时标志。它仅用于哈希表中的
 * 元组，而那些元组不需要任何可见性信息，因此我们可以将其叠加在一个可见性
 * 标志上，而不必占用一个专用位。
 */
#define HEAP_TUPLE_HAS_MATCH	HEAP_ONLY_TUPLE /* 元组有连接匹配 */

/*
 * HeapTupleHeader 访问器函数
 */

static bool HeapTupleHeaderXminFrozen(const HeapTupleHeaderData *tup);

/*
 * HeapTupleHeaderGetRawXmin 返回"原始"的 xmin 字段，即最初用于插入该元组的 xid。
 * 不过，该元组实际上可能已被冻结（通过 HeapTupleHeaderSetXminFrozen），在这种
 * 情况下该元组的 xmin 对所有快照都可见。在 PostgreSQL 9.4 之前，我们实际上是
 * 将 xmin 改为 FrozenTransactionId，该值仍可能在磁盘上遇到。
 */
static inline TransactionId
HeapTupleHeaderGetRawXmin(const HeapTupleHeaderData *tup)
{
	return tup->t_choice.t_heap.t_xmin;
}

static inline TransactionId
HeapTupleHeaderGetXmin(const HeapTupleHeaderData *tup)
{
	return HeapTupleHeaderXminFrozen(tup) ?
		FrozenTransactionId : HeapTupleHeaderGetRawXmin(tup);
}

static inline void
HeapTupleHeaderSetXmin(HeapTupleHeaderData *tup, TransactionId xid)
{
	tup->t_choice.t_heap.t_xmin = xid;
}

static inline bool
HeapTupleHeaderXminCommitted(const HeapTupleHeaderData *tup)
{
	return (tup->t_infomask & HEAP_XMIN_COMMITTED) != 0;
}

static inline bool
HeapTupleHeaderXminInvalid(const HeapTupleHeaderData *tup) \
{
	return (tup->t_infomask & (HEAP_XMIN_COMMITTED | HEAP_XMIN_INVALID)) ==
		HEAP_XMIN_INVALID;
}

static inline bool
HeapTupleHeaderXminFrozen(const HeapTupleHeaderData *tup)
{
	return (tup->t_infomask & HEAP_XMIN_FROZEN) == HEAP_XMIN_FROZEN;
}

static inline void
HeapTupleHeaderSetXminCommitted(HeapTupleHeaderData *tup)
{
	Assert(!HeapTupleHeaderXminInvalid(tup));
	tup->t_infomask |= HEAP_XMIN_COMMITTED;
}

static inline void
HeapTupleHeaderSetXminInvalid(HeapTupleHeaderData *tup)
{
	Assert(!HeapTupleHeaderXminCommitted(tup));
	tup->t_infomask |= HEAP_XMIN_INVALID;
}

static inline void
HeapTupleHeaderSetXminFrozen(HeapTupleHeaderData *tup)
{
	Assert(!HeapTupleHeaderXminInvalid(tup));
	tup->t_infomask |= HEAP_XMIN_FROZEN;
}

static inline TransactionId
HeapTupleHeaderGetRawXmax(const HeapTupleHeaderData *tup)
{
	return tup->t_choice.t_heap.t_xmax;
}

static inline void
HeapTupleHeaderSetXmax(HeapTupleHeaderData *tup, TransactionId xid)
{
	tup->t_choice.t_heap.t_xmax = xid;
}

#ifndef FRONTEND
/*
 * HeapTupleHeaderGetRawXmax 返回原始的 Xmax 字段。要找出更新某元组的 Xid，
 * 在某些位被设置的情况下可能需要解析 MultiXactId。HeapTupleHeaderGetUpdateXid
 * 会检查这些位，并在必要时负责解析 MultiXactId。这可能涉及 multixact 的 I/O，
 * 因此应仅在绝对必要时使用。
 */
static inline TransactionId
HeapTupleHeaderGetUpdateXid(const HeapTupleHeaderData *tup)
{
	if (!((tup)->t_infomask & HEAP_XMAX_INVALID) &&
		((tup)->t_infomask & HEAP_XMAX_IS_MULTI) &&
		!((tup)->t_infomask & HEAP_XMAX_LOCK_ONLY))
		return HeapTupleGetUpdateXid(tup);
	else
		return HeapTupleHeaderGetRawXmax(tup);
}
#endif							/* FRONTEND */

/*
 * HeapTupleHeaderGetRawCommandId 会给出头部中的内容，无论它是否有用。
 * 大多数代码应使用 HeapTupleHeaderGetCmin 或 HeapTupleHeaderGetCmax 代替，
 * 但要注意那些函数会断言你能得到一个合法的结果，即你正处于发起事务中！
 */
static inline CommandId
HeapTupleHeaderGetRawCommandId(const HeapTupleHeaderData *tup)
{
	return tup->t_choice.t_heap.t_field3.t_cid;
}

/* SetCmin 相当简单，因为我们从不需要组合 CID */static inline void
HeapTupleHeaderSetCmin(HeapTupleHeaderData *tup, CommandId cid)
{
	Assert(!(tup->t_infomask & HEAP_MOVED));
	tup->t_choice.t_heap.t_field3.t_cid = cid;
	tup->t_infomask &= ~HEAP_COMBOCID;
}

/* SetCmax 必须在 HeapTupleHeaderAdjustCmax 之后使用；见 combocid.c */
static inline void
HeapTupleHeaderSetCmax(HeapTupleHeaderData *tup, CommandId cid, bool iscombo)
{
	Assert(!((tup)->t_infomask & HEAP_MOVED));
	tup->t_choice.t_heap.t_field3.t_cid = cid;
	if (iscombo)
		tup->t_infomask |= HEAP_COMBOCID;
	else
		tup->t_infomask &= ~HEAP_COMBOCID;
}

static inline TransactionId
HeapTupleHeaderGetXvac(const HeapTupleHeaderData *tup)
{
	if (tup->t_infomask & HEAP_MOVED)
		return tup->t_choice.t_heap.t_field3.t_xvac;
	else
		return InvalidTransactionId;
}

static inline void
HeapTupleHeaderSetXvac(HeapTupleHeaderData *tup, TransactionId xid)
{
	Assert(tup->t_infomask & HEAP_MOVED);
	tup->t_choice.t_heap.t_field3.t_xvac = xid;
}

StaticAssertDecl(MaxOffsetNumber < SpecTokenOffsetNumber,
				 "invalid speculative token constant");

static inline bool
HeapTupleHeaderIsSpeculative(const HeapTupleHeaderData *tup)
{
	return ItemPointerGetOffsetNumberNoCheck(&tup->t_ctid) == SpecTokenOffsetNumber;
}

static inline BlockNumber
HeapTupleHeaderGetSpeculativeToken(const HeapTupleHeaderData *tup)
{
	Assert(HeapTupleHeaderIsSpeculative(tup));
	return ItemPointerGetBlockNumber(&tup->t_ctid);
}

static inline void
HeapTupleHeaderSetSpeculativeToken(HeapTupleHeaderData *tup, BlockNumber token)
{
	ItemPointerSet(&tup->t_ctid, token, SpecTokenOffsetNumber);
}

static inline bool
HeapTupleHeaderIndicatesMovedPartitions(const HeapTupleHeaderData *tup)
{
	return ItemPointerIndicatesMovedPartitions(&tup->t_ctid);
}

static inline void
HeapTupleHeaderSetMovedPartitions(HeapTupleHeaderData *tup)
{
	ItemPointerSetMovedPartitions(&tup->t_ctid);
}

static inline uint32
HeapTupleHeaderGetDatumLength(const HeapTupleHeaderData *tup)
{
	return VARSIZE(tup);
}

static inline void
HeapTupleHeaderSetDatumLength(HeapTupleHeaderData *tup, uint32 len)
{
	SET_VARSIZE(tup, len);
}

static inline Oid
HeapTupleHeaderGetTypeId(const HeapTupleHeaderData *tup)
{
	return tup->t_choice.t_datum.datum_typeid;
}

static inline void
HeapTupleHeaderSetTypeId(HeapTupleHeaderData *tup, Oid datum_typeid)
{
	tup->t_choice.t_datum.datum_typeid = datum_typeid;
}

static inline int32
HeapTupleHeaderGetTypMod(const HeapTupleHeaderData *tup)
{
	return tup->t_choice.t_datum.datum_typmod;
}

static inline void
HeapTupleHeaderSetTypMod(HeapTupleHeaderData *tup, int32 typmod)
{
	tup->t_choice.t_datum.datum_typmod = typmod;
}

/*
 * 注意：一旦得知元组已中止，或本应更新它的事务已知中止，我们就不再将其视为
 * HOT 更新。为了获得最佳效率，在使用本函数前先检查元组的可见性，以使
 * INVALID 位尽可能保持最新。
 */
static inline bool
HeapTupleHeaderIsHotUpdated(const HeapTupleHeaderData *tup)
{
	return
		(tup->t_infomask2 & HEAP_HOT_UPDATED) != 0 &&
		(tup->t_infomask & HEAP_XMAX_INVALID) == 0 &&
		!HeapTupleHeaderXminInvalid(tup);
}

static inline void
HeapTupleHeaderSetHotUpdated(HeapTupleHeaderData *tup)
{
	tup->t_infomask2 |= HEAP_HOT_UPDATED;
}

static inline void
HeapTupleHeaderClearHotUpdated(HeapTupleHeaderData *tup)
{
	tup->t_infomask2 &= ~HEAP_HOT_UPDATED;
}

static inline bool
HeapTupleHeaderIsHeapOnly(const HeapTupleHeaderData *tup) \
{
	return (tup->t_infomask2 & HEAP_ONLY_TUPLE) != 0;
}

static inline void
HeapTupleHeaderSetHeapOnly(HeapTupleHeaderData *tup)
{
	tup->t_infomask2 |= HEAP_ONLY_TUPLE;
}

static inline void
HeapTupleHeaderClearHeapOnly(HeapTupleHeaderData *tup)
{
	tup->t_infomask2 &= ~HEAP_ONLY_TUPLE;
}

/*
 * 这些同时用于 HeapTuple 和 MinimalTuple，因此它们必须是宏。
 */

#define HeapTupleHeaderGetNatts(tup) \
	((tup)->t_infomask2 & HEAP_NATTS_MASK)

#define HeapTupleHeaderSetNatts(tup, natts) \
( \
	(tup)->t_infomask2 = ((tup)->t_infomask2 & ~HEAP_NATTS_MASK) | (natts) \
)

#define HeapTupleHeaderHasExternal(tup) \
		(((tup)->t_infomask & HEAP_HASEXTERNAL) != 0)


/*
 * BITMAPLEN(NATTS) -
 *		根据数据列的数量计算 null 位图的大小。
 */
static inline int
BITMAPLEN(int NATTS)
{
	return (NATTS + 7) / 8;
}

/*
 * MaxHeapTupleSize 是堆元组允许的最大尺寸，包含头部和 MAXALIGN 对齐填充。
 * 基本上它是 BLCKSZ 减去磁盘页上必须存在的其他东西。由于堆页面不使用
 * "特殊空间"（special space），因此无需为此扣除。
 *
 * 注意：我们为必须指向该元组的 ItemId 预留了空间，以确保一个原本为空的页
 * 确实能容纳下这个尺寸的元组。由于 ItemId 和元组有不同的对齐要求，不要假设
 * 例如可以在同一页上放得下 2 个大小为 MaxHeapTupleSize/2 的元组。
 */
#define MaxHeapTupleSize  (BLCKSZ - MAXALIGN(SizeOfPageHeaderData + sizeof(ItemIdData)))
#define MinHeapTupleSize  MAXALIGN(SizeofHeapTupleHeader)

/*
 * MaxHeapTuplesPerPage 是单个堆页所能容纳元组数量的上界。（注意索引可以有
 * 更多，因为它们使用更小的元组头部。）分母的得出是因为每个元组必须
 * 按 MAXALIGN 对齐，并且必须有一个相关联的行指针。
 *
 * 注意：在 HOT 情况下，理论上堆页上的行指针（而非实际元组）数量可能多于
 * 此值。但我们仍将行指针数量约束为此值，以避免过度的行指针膨胀，并且
 * 不需要增大工作数组的尺寸。
 */
#define MaxHeapTuplesPerPage	\
	((int) ((BLCKSZ - SizeOfPageHeaderData) / \
			(MAXALIGN(SizeofHeapTupleHeader) + sizeof(ItemIdData))))

/*
 * MaxAttrSize 是对 char(n) 及类似类型数据字段声明大小的一个有些随意的上限。
 * 它无需与 varlena 值的*实际*上限有任何直接关系，后者目前是 1Gb
 * （见 postgres.h 中的 TOAST 结构）。我将其设为 10Mb，这看起来是个合理的
 * 数值 --- tgl 2000/8/6。
 */
#define MaxAttrSize		(10 * 1024 * 1024)


/*
 * MinimalTuple 是一种替代表示，用于执行器内部的临时元组，适用于不需要事务状态
 * 信息、元组行类型已知、并且由于需要存储大量元组而值得省下几个字节的场景。
 * 这种表示方式的选择使得元组访问例程可以通过 HeapTupleData 指针结构来操作
 * 完整体元组或最小元组的任一种。访问例程看不出区别，只是它们不能访问事务状态
 * 或 t_ctid 字段，因为那些字段不存在。
 *
 * 在大多数情况下，MinimalTuple 应通过 TupleTableSlot 例程来访问。这些例程会
 * 阻止访问"系统列"，从而防止意外使用那些不存在的字段。
 *
 * MinimalTupleData 包含一个长度字、一些填充，以及从 t_infomask2 开始与
 * HeapTupleHeaderData 匹配的字段。填充的选择使得 offsetof(t_infomask2) 在两个
 * 结构体中按 MAXIMUM_ALIGNOF 取模后相同。这使得两种情况下数据对齐规则一致。
 *
 * 当通过 HeapTupleData 指针访问最小元组时，t_data 被设为指向实际最小元组起始
 * 位置之前 MINIMAL_TUPLE_OFFSET 字节处 --- 也就是与一个最小元组数据相匹配的
 * 完整体元组的起始位置。正是这个技巧让这两个结构看起来是等价的。
 *
 * 注意 t_hoff 的计算方式与完整体元组相同，因此它包含了 MINIMAL_TUPLE_OFFSET
 * 这段距离。但 t_len 不包含这段距离。
 *
 * MINIMAL_TUPLE_DATA_OFFSET 是第一个有用（非填充）数据的偏移量，不包括长度字。
 * tuplesort.c 和 tuplestore.c 使用它来避免将填充写入磁盘。
 */
#define MINIMAL_TUPLE_OFFSET \
	((offsetof(HeapTupleHeaderData, t_infomask2) - sizeof(uint32)) / MAXIMUM_ALIGNOF * MAXIMUM_ALIGNOF)
#define MINIMAL_TUPLE_PADDING \
	((offsetof(HeapTupleHeaderData, t_infomask2) - sizeof(uint32)) % MAXIMUM_ALIGNOF)
#define MINIMAL_TUPLE_DATA_OFFSET \
	offsetof(MinimalTupleData, t_infomask2)

struct MinimalTupleData
{
	uint32		t_len;			/* 最小元组的实际长度 */

	char		mt_padding[MINIMAL_TUPLE_PADDING];

	/* 此行以下的字段必须与 HeapTupleHeaderData 匹配！ */

	uint16		t_infomask2;	/* 属性数量 + 各种标志位 */

	uint16		t_infomask;		/* 各种标志位，见下文 */

	uint8		t_hoff;			/* 包含位图和填充的头部大小 */

	/* ^ - 23 字节 - ^ */

	bits8		t_bits[FLEXIBLE_ARRAY_MEMBER];	/* NULL 位图 */

	/* 结构体末尾后还有更多数据 */
};

/* 类型定义在 htup.h 中 */

#define SizeofMinimalTupleHeader offsetof(MinimalTupleData, t_bits)

/*
 * MinimalTuple 访问器函数
 */

static inline bool
HeapTupleHeaderHasMatch(const MinimalTupleData *tup)
{
	return (tup->t_infomask2 & HEAP_TUPLE_HAS_MATCH) != 0;
}

static inline void
HeapTupleHeaderSetMatch(MinimalTupleData *tup)
{
	tup->t_infomask2 |= HEAP_TUPLE_HAS_MATCH;
}

static inline void
HeapTupleHeaderClearMatch(MinimalTupleData *tup)
{
	tup->t_infomask2 &= ~HEAP_TUPLE_HAS_MATCH;
}


/*
 * GETSTRUCT - 给定一个 HeapTuple 指针，返回用户数据的地址
 */
static inline void *
GETSTRUCT(const HeapTupleData *tuple)
{
	return ((char *) (tuple->t_data) + tuple->t_data->t_hoff);
}

/*
 * 用于 HeapTuple 指针的访问器函数。
 */

static inline bool
HeapTupleHasNulls(const HeapTupleData *tuple)
{
	return (tuple->t_data->t_infomask & HEAP_HASNULL) != 0;
}

static inline bool
HeapTupleNoNulls(const HeapTupleData *tuple)
{
	return !HeapTupleHasNulls(tuple);
}

static inline bool
HeapTupleHasVarWidth(const HeapTupleData *tuple)
{
	return (tuple->t_data->t_infomask & HEAP_HASVARWIDTH) != 0;
}

static inline bool
HeapTupleAllFixed(const HeapTupleData *tuple)
{
	return !HeapTupleHasVarWidth(tuple);
}

static inline bool
HeapTupleHasExternal(const HeapTupleData *tuple)
{
	return (tuple->t_data->t_infomask & HEAP_HASEXTERNAL) != 0;
}

static inline bool
HeapTupleIsHotUpdated(const HeapTupleData *tuple)
{
	return HeapTupleHeaderIsHotUpdated(tuple->t_data);
}

static inline void
HeapTupleSetHotUpdated(const HeapTupleData *tuple)
{
	HeapTupleHeaderSetHotUpdated(tuple->t_data);
}

static inline void
HeapTupleClearHotUpdated(const HeapTupleData *tuple)
{
	HeapTupleHeaderClearHotUpdated(tuple->t_data);
}

static inline bool
HeapTupleIsHeapOnly(const HeapTupleData *tuple)
{
	return HeapTupleHeaderIsHeapOnly(tuple->t_data);
}

static inline void
HeapTupleSetHeapOnly(const HeapTupleData *tuple)
{
	HeapTupleHeaderSetHeapOnly(tuple->t_data);
}

static inline void
HeapTupleClearHeapOnly(const HeapTupleData *tuple)
{
	HeapTupleHeaderClearHeapOnly(tuple->t_data);
}

/* common/heaptuple.c 中函数的原型声明 */
extern Size heap_compute_data_size(TupleDesc tupleDesc,
								   const Datum *values, const bool *isnull);
extern void heap_fill_tuple(TupleDesc tupleDesc,
							const Datum *values, const bool *isnull,
							char *data, Size data_size,
							uint16 *infomask, bits8 *bit);
extern bool heap_attisnull(HeapTuple tup, int attnum, TupleDesc tupleDesc);
extern Datum nocachegetattr(HeapTuple tup, int attnum,
							TupleDesc tupleDesc);
extern Datum heap_getsysattr(HeapTuple tup, int attnum, TupleDesc tupleDesc,
							 bool *isnull);
extern Datum getmissingattr(TupleDesc tupleDesc,
							int attnum, bool *isnull);
extern HeapTuple heap_copytuple(HeapTuple tuple);
extern void heap_copytuple_with_tuple(HeapTuple src, HeapTuple dest);
extern Datum heap_copy_tuple_as_datum(HeapTuple tuple, TupleDesc tupleDesc);
extern HeapTuple heap_form_tuple(TupleDesc tupleDescriptor,
								 const Datum *values, const bool *isnull);
extern HeapTuple heap_modify_tuple(HeapTuple tuple,
								   TupleDesc tupleDesc,
								   const Datum *replValues,
								   const bool *replIsnull,
								   const bool *doReplace);
extern HeapTuple heap_modify_tuple_by_cols(HeapTuple tuple,
										   TupleDesc tupleDesc,
										   int nCols,
										   const int *replCols,
										   const Datum *replValues,
										   const bool *replIsnull);
extern void heap_deform_tuple(HeapTuple tuple, TupleDesc tupleDesc,
							  Datum *values, bool *isnull);
extern void heap_freetuple(HeapTuple htup);
extern MinimalTuple heap_form_minimal_tuple(TupleDesc tupleDescriptor,
											const Datum *values, const bool *isnull,
											Size extra);
extern void heap_free_minimal_tuple(MinimalTuple mtup);
extern MinimalTuple heap_copy_minimal_tuple(MinimalTuple mtup, Size extra);
extern HeapTuple heap_tuple_from_minimal_tuple(MinimalTuple mtup);
extern MinimalTuple minimal_tuple_from_heap_tuple(HeapTuple htup, Size extra);
extern size_t varsize_any(void *p);
extern HeapTuple heap_expand_tuple(HeapTuple sourceTuple, TupleDesc tupleDesc);
extern MinimalTuple minimal_expand_tuple(HeapTuple sourceTuple, TupleDesc tupleDesc);

#ifndef FRONTEND
/*
 *	fastgetattr
 *		获取一个用户属性的值，以 Datum 形式返回（可能是一个值，
 *		也可能是指向元组数据区的指针）。
 *
 *		当可能请求系统属性时，不得使用本函数。此外，传入的 attnum
 *		必须是有效的。如有疑问，请改用 heap_getattr()。
 *
 *		本函数被频繁调用，因此我们将可缓存的查找和 NULL 查找
 *		做了宏内联优化，其余情况调用 nocachegetattr()。
 */
static inline Datum
fastgetattr(HeapTuple tup, int attnum, TupleDesc tupleDesc, bool *isnull)
{
	Assert(attnum > 0);

	*isnull = false;
	if (HeapTupleNoNulls(tup))
	{
		CompactAttribute *att;

		att = TupleDescCompactAttr(tupleDesc, attnum - 1);
		if (att->attcacheoff >= 0)
			return fetchatt(att, (char *) tup->t_data + tup->t_data->t_hoff +
							att->attcacheoff);
		else
			return nocachegetattr(tup, attnum, tupleDesc);
	}
	else
	{
		if (att_isnull(attnum - 1, tup->t_data->t_bits))
		{
			*isnull = true;
			return (Datum) NULL;
		}
		else
			return nocachegetattr(tup, attnum, tupleDesc);
	}
}

/*
 *	heap_getattr
 *		提取堆元组的一个属性并以 Datum 形式返回。
 *		这同时适用于系统属性和用户属性。给定的 attnum 会经过
 *		恰当的范围检查。
 *
 *		如果相关字段的值为 NULL，我们返回一个零 Datum 并将 *isnull 设为 true。
 *		否则，我们将 *isnull 设为 false。
 *
 *		<tup> 是指向堆元组的指针。<attnum> 是调用方想要的列（字段）的
 *		属性编号。<tupleDesc> 是指向描述该行及其所有字段的结构的指针。
 *
 */
static inline Datum
heap_getattr(HeapTuple tup, int attnum, TupleDesc tupleDesc, bool *isnull)
{
	if (attnum > 0)
	{
		if (attnum > (int) HeapTupleHeaderGetNatts(tup->t_data))
			return getmissingattr(tupleDesc, attnum, isnull);
		else
			return fastgetattr(tup, attnum, tupleDesc, isnull);
	}
	else
		return heap_getsysattr(tup, attnum, tupleDesc, isnull);
}
#endif							/* FRONTEND */

#endif							/* HTUP_DETAILS_H */
