/*-------------------------------------------------------------------------
 *
 * tupdesc.h
 *	  POSTGRES 元组描述符定义。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/tupdesc.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TUPDESC_H
#define TUPDESC_H

#include "access/attnum.h"
#include "catalog/pg_attribute.h"
#include "nodes/pg_list.h"


typedef struct AttrDefault
{
	AttrNumber	adnum;
	char	   *adbin;			/* 表达式的 nodeToString 表示形式 */
} AttrDefault;

typedef struct ConstrCheck
{
	char	   *ccname;
	char	   *ccbin;			/* 表达式的 nodeToString 表示形式 */
	bool		ccenforced;
	bool		ccvalid;
	bool		ccnoinherit;	/* 这是一个不可继承的约束 */
} ConstrCheck;

/* 本结构体包含元组的约束 */
typedef struct TupleConstr
{
	AttrDefault *defval;		/* 数组 */
	ConstrCheck *check;			/* 数组 */
	struct AttrMissing *missing;	/* 缺失属性值，若无则为 NULL */
	uint16		num_defval;
	uint16		num_check;
	bool		has_not_null;	/* 任意非空约束，包括尚未生效的 */
	bool		has_generated_stored;
	bool		has_generated_virtual;
} TupleConstr;

/*
 * CompactAttribute（紧凑属性）
 *		为了更快速地访问（例如元组解构）等任务而对 FormData_pg_attribute 的精简版本。
 *		本结构体的字段由 populate_compact_attribute() 函数填充，该函数必须在
 *		FormData_pg_attribute 结构体被填充或以任何方式修改之后立即调用。
 * 目前本结构体大小为 16 字节。任何会增大该结构体的代码改动都应非常慎重地考虑。
 *
 * 必须访问 TupleDesc 属性数据的代码，在所需字段于此可用时，应当始终使用本结构体的字段。
 * 由于 CompactAttribute 是 FormData_pg_attribute 更紧凑的表示形式，
 * 且访问 FormData_pg_attribute 还需要额外的计算来获取 TupleDesc 内数组的
 * 基地址，访问 CompactAttribute 中的内存效率更高。
 */
typedef struct CompactAttribute
{
	int32		attcacheoff;	/* 元组中已知的固定偏移，未知则为 -1 */
	int16		attlen;			/* 属性长度（字节）；-1 表示变长，-2 表示 cstring */
	bool		attbyval;		/* 同 FormData_pg_attribute.attbyval */
	bool		attispackable;	/* 同 FormData_pg_attribute.attstorage != TYPSTORAGE_PLAIN */
	bool		atthasmissing;	/* 同 FormData_pg_attribute.atthasmissing */
	bool		attisdropped;	/* 同 FormData_pg_attribute.attisdropped */
	bool		attgenerated;	/* 同 FormData_pg_attribute.attgenerated != '\0' */
	char		attnullability; /* 非空约束的状态，见下文 */
	uint8		attalignby;		/* 对齐要求（字节数） */
} CompactAttribute;

/* CompactAttribute->attnullability 的有效取值 */
#define	ATTNULLABLE_UNRESTRICTED 'f'	/* 不存在任何约束 */
#define	ATTNULLABLE_UNKNOWN		'u' /* 存在约束，但有效性未知 */
#define	ATTNULLABLE_VALID		'v' /* 存在有效约束 */
#define	ATTNULLABLE_INVALID		'i' /* 存在约束，但被标记为无效 */

/*
 * 本结构体在后端内部被传递，用于描述元组的结构。
 * 对于来自磁盘关系的元组，其信息收集自 pg_attribute、pg_attrdef 和 pg_constraint 系统目录。

 * 瞬态行类型（例如连接查询的结果）拥有匿名的 TupleDesc 结构体，通常会省略任何约束信息；
 * 因此本结构被设计成可以高效地省略这些约束。

 *
 * 注意，TupleDesc 中只提及用户属性，而不提及系统属性。

 *
 * 如果 tupdesc 已知对应于某个具名行类型（例如表的行类型），则 tdtypeid 标识该类型，且 tdtypmod 为 -1。
 * 否则 tdtypeid 为 RECORDOID，而 tdtypmod 可以是 -1（表示完全匿名的行类型），
 * 或是 >= 0 的值，以便在 typcache.c 的类型缓存中查找该行类型。


 *
 * 注意，tdtypeid 永远不会是"复合之上的域"的 OID，即便我们处理的是（在更高层面上）
 * 已知属于"复合之上域"类型的值。这是因为 tdtypeid/tdtypmod 需要与组合 Datums 的
 * 类型标注相匹配，而那些 Datums 也从未被显式标记为某个域类型。
 *
 * 当前，存活于缓存（relcache 或 typcache）中的元组描述符采用引用计数：当引用计数
 * 降为零时，它们就可以被删除。然而，由执行器创建的元组描述符不需要引用计数：它们
 * 只是在适当的内存上下文中创建，并在该上下文被释放时随之消失。我们将此类描述符的
 * tdrefcount 字段置为 -1，而采用引用计数的描述符其 tdrefcount 总是 >= 0。
 *
 * 在 compact_attrs 变长数组之外，TupleDesc 还存储了一个 FormData_pg_attribute 数组。
 * 下文定义的 TupleDescAttr() 函数负责计算 FormData_pg_attribute 数组元素的地址。
 *
 * CompactAttribute 数组实际上是 FormData_pg_attribute 数组的精简版本。由于
 * CompactAttribute 比 FormData_pg_attribute 小得多，代码（尤其是性能关键的代码）应当优先使用 CompactAttribute 中的字段，而非 FormData_pg_attribute 中的等价字段。
 *
 * 任何代码在手动修改 FormData_pg_attribute 数组中的字段之后，必须随后调用 populate_compact_attribute() 以将这些修改刷新到对应的 'compact_attrs' 元素中。
 */
typedef struct TupleDescData
{
	int			natts;			/* 元组中的属性个数 */
	Oid			tdtypeid;		/* 元组类型对应的复合类型 ID */
	int32		tdtypmod;		/* 元组类型的 typmod */
	int			tdrefcount;		/* 引用计数；若不计数为 -1 */
	TupleConstr *constr;		/* 约束，若无则为 NULL */
	/* compact_attrs[N] 是属性号 N+1 的紧凑元数据 */
	CompactAttribute compact_attrs[FLEXIBLE_ARRAY_MEMBER];
}			TupleDescData;
typedef struct TupleDescData *TupleDesc;

extern void populate_compact_attribute(TupleDesc tupdesc, int attnum);

/*
 * 计算 TupleDescData 结构体末尾处 Form_pg_attribute 的基地址。
 */
#define TupleDescAttrAddress(desc) \
	(Form_pg_attribute) ((char *) (desc) + \
	 (offsetof(struct TupleDescData, compact_attrs) + \
	 (desc)->natts * sizeof(CompactAttribute)))

/* 访问 tupdesc 第 i 个 FormData_pg_attribute 元素的访问器。 */
static inline FormData_pg_attribute *
TupleDescAttr(TupleDesc tupdesc, int i)
{
	FormData_pg_attribute *attrs = TupleDescAttrAddress(tupdesc);

	return &attrs[i];
}

#undef TupleDescAttrAddress

extern void verify_compact_attribute(TupleDesc, int attnum);

/*
 * 访问 tupdesc 第 i 个 CompactAttribute 元素的访问器。
 */
static inline CompactAttribute *
TupleDescCompactAttr(TupleDesc tupdesc, int i)
{
	CompactAttribute *cattr = &tupdesc->compact_attrs[i];

#ifdef USE_ASSERT_CHECKING

	/* 检查 CompactAttribute 是否已正确填充 */
	verify_compact_attribute(tupdesc, i);
#endif

	return cattr;
}

extern TupleDesc CreateTemplateTupleDesc(int natts);

extern TupleDesc CreateTupleDesc(int natts, Form_pg_attribute *attrs);

extern TupleDesc CreateTupleDescCopy(TupleDesc tupdesc);

extern TupleDesc CreateTupleDescTruncatedCopy(TupleDesc tupdesc, int natts);

extern TupleDesc CreateTupleDescCopyConstr(TupleDesc tupdesc);

#define TupleDescSize(src) \
	(offsetof(struct TupleDescData, compact_attrs) + \
	 (src)->natts * sizeof(CompactAttribute) + \
	 (src)->natts * sizeof(FormData_pg_attribute))

extern void TupleDescCopy(TupleDesc dst, TupleDesc src);

extern void TupleDescCopyEntry(TupleDesc dst, AttrNumber dstAttno,
							   TupleDesc src, AttrNumber srcAttno);

extern void FreeTupleDesc(TupleDesc tupdesc);

extern void IncrTupleDescRefCount(TupleDesc tupdesc);
extern void DecrTupleDescRefCount(TupleDesc tupdesc);

#define PinTupleDesc(tupdesc) \
	do { \
		if ((tupdesc)->tdrefcount >= 0) \
			IncrTupleDescRefCount(tupdesc); \
	} while (0)

#define ReleaseTupleDesc(tupdesc) \
	do { \
		if ((tupdesc)->tdrefcount >= 0) \
			DecrTupleDescRefCount(tupdesc); \
	} while (0)

extern bool equalTupleDescs(TupleDesc tupdesc1, TupleDesc tupdesc2);
extern bool equalRowTypes(TupleDesc tupdesc1, TupleDesc tupdesc2);
extern uint32 hashRowType(TupleDesc desc);

extern void TupleDescInitEntry(TupleDesc desc,
							   AttrNumber attributeNumber,
							   const char *attributeName,
							   Oid oidtypeid,
							   int32 typmod,
							   int attdim);

extern void TupleDescInitBuiltinEntry(TupleDesc desc,
									  AttrNumber attributeNumber,
									  const char *attributeName,
									  Oid oidtypeid,
									  int32 typmod,
									  int attdim);

extern void TupleDescInitEntryCollation(TupleDesc desc,
										AttrNumber attributeNumber,
										Oid collationid);

extern TupleDesc BuildDescFromLists(const List *names, const List *types, const List *typmods, const List *collations);

extern Node *TupleDescGetDefault(TupleDesc tupdesc, AttrNumber attnum);

#endif							/* TUPDESC_H */
