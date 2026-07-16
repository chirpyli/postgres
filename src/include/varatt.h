/*-------------------------------------------------------------------------
 *
 * varatt.h
 *	  变长数据类型（TOAST 支持）
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1995, Regents of the University of California
 *
 * src/include/varatt.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef VARATT_H
#define VARATT_H

/*
 * varatt_external 结构体是一种传统的“TOAST 指针”，即用于从 TOAST 表中
 * 获取存储在行外（out-of-line）的 Datum 所需的信息。当且仅当 va_extinfo 中
 * 存储的外部大小小于 va_rawsize - VARHDRSZ 时，数据才被压缩。
 *
 * 该结构体不得包含任何填充字节，因为我们有时会使用 memcmp 来比较这些指针。
 *
 * 注意，这些信息在真实元组内部是未对齐（unaligned）存储的，因此在查看这些
 * 字段之前，需要先通过 memcpy 从元组中复制到本地的结构体变量里！
 * （我们使用 memcmp 的原因，正是为了避免仅仅为了判断两个 TOAST 指针是否相等
 * 就去做这样的复制……）
 */
typedef struct varatt_external
{
	int32		va_rawsize;		/* 原始数据大小（含头部） */
	uint32		va_extinfo;		/* 外部保存的大小（不含头部）以及
								 * 压缩方法 */
	Oid			va_valueid;		/* TOAST 表中该值的唯一 ID */
	Oid			va_toastrelid;	/* 包含该值的 TOAST 表的 RelID */
}			varatt_external;

/*
 * 以下宏定义了 va_extinfo 中“已保存大小”的部分。其剩余的两个高位用于标识
 * 压缩方法。
 */
#define VARLENA_EXTSIZE_BITS	30
#define VARLENA_EXTSIZE_MASK	((1U << VARLENA_EXTSIZE_BITS) - 1)

/*
 * varatt_indirect 是一种“TOAST 指针”，代表存储在内存中（而非外部 TOAST
 * 关系中）的行外 Datum。此类 Datum 的创建者全权负责保证被引用的存储持续时间
 * 不短于引用它的指针 Datum 可能存在的时间。
 *
 * 注意，与 struct varatt_external 相同，该结构体在任意包含它的元组内部也是
 * 未对齐存储的。
 */
typedef struct varatt_indirect
{
	struct varlena *pointer;	/* 指向内存中 varlena 的指针 */
}			varatt_indirect;

/*
 * varatt_expanded 是一种“TOAST 指针”，代表以内存形式存储的行外 Datum，采用
 * 某种类型相关的、不一定物理连续、便于计算而非便于存储的格式。相关的 API
 * （尤其是 struct ExpandedObjectHeader 的定义）位于
 * src/include/utils/expandeddatum.h。
 *
 * 注意，与 struct varatt_external 相同，该结构体在任意包含它的元组内部也是
 * 未对齐存储的。
 */
typedef struct ExpandedObjectHeader ExpandedObjectHeader;

typedef struct varatt_expanded
{
	ExpandedObjectHeader *eohptr;
} varatt_expanded;

/*
 * 各类“TOAST 指针” Datum 的类型标签。VARTAG_ONDISK 的特殊取值源于对磁盘兼容
 * 性的要求——早期曾认为标签字段就是指针 Datum 的长度。
 */
typedef enum vartag_external
{
	VARTAG_INDIRECT = 1,
	VARTAG_EXPANDED_RO = 2,
	VARTAG_EXPANDED_RW = 3,
	VARTAG_ONDISK = 18
} vartag_external;

/* 该测试依赖于上面这些特定的标签取值 */
#define VARTAG_IS_EXPANDED(tag) \
	(((tag) & ~1) == VARTAG_EXPANDED_RO)

#define VARTAG_SIZE(tag) \
	((tag) == VARTAG_INDIRECT ? sizeof(varatt_indirect) : \
	 VARTAG_IS_EXPANDED(tag) ? sizeof(varatt_expanded) : \
	 (tag) == VARTAG_ONDISK ? sizeof(varatt_external) : \
	 (AssertMacro(false), 0))

/*
 * 这些结构体描述了可能已被 TOAST 的 varlena 对象的头部。一般情况下不要直接
 * 引用这些结构体，而应使用下面的宏。
 *
 * 我们对对齐和未对齐两种情况使用不同的结构体，否则编译器可能会生成假定对齐
 * 的访问代码，而去触碰 1 字节头部 varlena 的字段。
 */
typedef union
{
	struct						/* 普通 varlena（4 字节长度） */
	{
		uint32		va_header;
		char		va_data[FLEXIBLE_ARRAY_MEMBER];
	}			va_4byte;
	struct						/* 行内压缩格式 */
	{
		uint32		va_header;
		uint32		va_tcinfo;	/* 原始数据大小（不含头部）以及
								 * 压缩方法；参见 va_extinfo */
		char		va_data[FLEXIBLE_ARRAY_MEMBER]; /* 压缩后的数据 */
	}			va_compressed;
} varattrib_4b;

typedef struct
{
	uint8		va_header;
	char		va_data[FLEXIBLE_ARRAY_MEMBER]; /* 数据从此处开始 */
} varattrib_1b;

/* TOAST 指针是 varattrib_1b 的一个子集，带有一个用于标识的标签字节 */
typedef struct
{
	uint8		va_header;		/* 总是 0x80 或 0x01 */
	uint8		va_tag;			/* Datum 的类型 */
	char		va_data[FLEXIBLE_ARRAY_MEMBER]; /* 类型相关数据 */
} varattrib_1b_e;

/*
 * 大端机器上 varlena 头部的比特布局：
 *
 * 00xxxxxx 4 字节长度字，对齐，未压缩数据（最大 1G）
 * 01xxxxxx 4 字节长度字，对齐，*压缩*数据（最大 1G）
 * 10000000 1 字节长度字，未对齐，TOAST 指针
 * 1xxxxxxx 1 字节长度字，未对齐，未压缩数据（最大 126 字节）
 *
 * 小端机器上 varlena 头部的比特布局：
 *
 * xxxxxx00 4 字节长度字，对齐，未压缩数据（最大 1G）
 * xxxxxx10 4 字节长度字，对齐，*压缩*数据（最大 1G）
 * 00000001 1 字节长度字，未对齐，TOAST 指针
 * xxxxxxx1 1 字节长度字，未对齐，未压缩数据（最大 126 字节）
 *
 * “xxx” 比特是长度字段（在所有情况下都包含自身）。大端情况下我们通过掩码
 * 提取长度，小端情况下则通过移位。注意在两种情况下标志位都位于物理上的
 * 第一个字节。此外，1 字节长度字不可能为零；这使得我们能够区分对齐填充字节
 * 与未对齐 Datum 的起始。（我们现在*要求*填充字节必须填为零！）
 *
 * 在 TOAST 指针中，va_tag 字段（见 varattrib_1b_e）用于区分指针 Datum 的具体
 * 类型与长度。
 */

/*
 * 与字节序相关的宏。这些被视为内部使用——请改用下方对外暴露的宏，不要直接
 * 使用这些。
 *
 * 注意：对于外部 TOAST 记录，IS_1B 为真，但 VARSIZE_1B 对这类记录会返回 0。
 * 因此通常应先检查 IS_EXTERNAL，再检查 IS_1B。
 */

#ifdef WORDS_BIGENDIAN

#define VARATT_IS_4B(PTR) \
	((((varattrib_1b *) (PTR))->va_header & 0x80) == 0x00)
#define VARATT_IS_4B_U(PTR) \
	((((varattrib_1b *) (PTR))->va_header & 0xC0) == 0x00)
#define VARATT_IS_4B_C(PTR) \
	((((varattrib_1b *) (PTR))->va_header & 0xC0) == 0x40)
#define VARATT_IS_1B(PTR) \
	((((varattrib_1b *) (PTR))->va_header & 0x80) == 0x80)
#define VARATT_IS_1B_E(PTR) \
	((((varattrib_1b *) (PTR))->va_header) == 0x80)
#define VARATT_NOT_PAD_BYTE(PTR) \
	(*((uint8 *) (PTR)) != 0)

/* VARSIZE_4B() 只应在已知对齐的数据上使用 */
#define VARSIZE_4B(PTR) \
	(((varattrib_4b *) (PTR))->va_4byte.va_header & 0x3FFFFFFF)
#define VARSIZE_1B(PTR) \
	(((varattrib_1b *) (PTR))->va_header & 0x7F)
#define VARTAG_1B_E(PTR) \
	(((varattrib_1b_e *) (PTR))->va_tag)

#define SET_VARSIZE_4B(PTR,len) \
	(((varattrib_4b *) (PTR))->va_4byte.va_header = (len) & 0x3FFFFFFF)
#define SET_VARSIZE_4B_C(PTR,len) \
	(((varattrib_4b *) (PTR))->va_4byte.va_header = ((len) & 0x3FFFFFFF) | 0x40000000)
#define SET_VARSIZE_1B(PTR,len) \
	(((varattrib_1b *) (PTR))->va_header = (len) | 0x80)
#define SET_VARTAG_1B_E(PTR,tag) \
	(((varattrib_1b_e *) (PTR))->va_header = 0x80, \
	 ((varattrib_1b_e *) (PTR))->va_tag = (tag))

#else							/* !WORDS_BIGENDIAN */

#define VARATT_IS_4B(PTR) \
	((((varattrib_1b *) (PTR))->va_header & 0x01) == 0x00)
#define VARATT_IS_4B_U(PTR) \
	((((varattrib_1b *) (PTR))->va_header & 0x03) == 0x00)
#define VARATT_IS_4B_C(PTR) \
	((((varattrib_1b *) (PTR))->va_header & 0x03) == 0x02)
#define VARATT_IS_1B(PTR) \
	((((varattrib_1b *) (PTR))->va_header & 0x01) == 0x01)
#define VARATT_IS_1B_E(PTR) \
	((((varattrib_1b *) (PTR))->va_header) == 0x01)
#define VARATT_NOT_PAD_BYTE(PTR) \
	(*((uint8 *) (PTR)) != 0)

/* VARSIZE_4B() 只应在已知对齐的数据上使用 */
#define VARSIZE_4B(PTR) \
	((((varattrib_4b *) (PTR))->va_4byte.va_header >> 2) & 0x3FFFFFFF)
#define VARSIZE_1B(PTR) \
	((((varattrib_1b *) (PTR))->va_header >> 1) & 0x7F)
#define VARTAG_1B_E(PTR) \
	(((varattrib_1b_e *) (PTR))->va_tag)

#define SET_VARSIZE_4B(PTR,len) \
	(((varattrib_4b *) (PTR))->va_4byte.va_header = (((uint32) (len)) << 2))
#define SET_VARSIZE_4B_C(PTR,len) \
	(((varattrib_4b *) (PTR))->va_4byte.va_header = (((uint32) (len)) << 2) | 0x02)
#define SET_VARSIZE_1B(PTR,len) \
	(((varattrib_1b *) (PTR))->va_header = (((uint8) (len)) << 1) | 0x01)
#define SET_VARTAG_1B_E(PTR,tag) \
	(((varattrib_1b_e *) (PTR))->va_header = 0x01, \
	 ((varattrib_1b_e *) (PTR))->va_tag = (tag))

#endif							/* WORDS_BIGENDIAN */

#define VARDATA_4B(PTR)		(((varattrib_4b *) (PTR))->va_4byte.va_data)
#define VARDATA_4B_C(PTR)	(((varattrib_4b *) (PTR))->va_compressed.va_data)
#define VARDATA_1B(PTR)		(((varattrib_1b *) (PTR))->va_data)
#define VARDATA_1B_E(PTR)	(((varattrib_1b_e *) (PTR))->va_data)

/*
 * 对外可见的 TOAST 宏从此处开始。
 */

#define VARHDRSZ_EXTERNAL		offsetof(varattrib_1b_e, va_data)
#define VARHDRSZ_COMPRESSED		offsetof(varattrib_4b, va_compressed.va_data)
#define VARHDRSZ_SHORT			offsetof(varattrib_1b, va_data)

#define VARATT_SHORT_MAX		0x7F
#define VARATT_CAN_MAKE_SHORT(PTR) \
	(VARATT_IS_4B_U(PTR) && \
	 (VARSIZE(PTR) - VARHDRSZ + VARHDRSZ_SHORT) <= VARATT_SHORT_MAX)
#define VARATT_CONVERTED_SHORT_SIZE(PTR) \
	(VARSIZE(PTR) - VARHDRSZ + VARHDRSZ_SHORT)

/*
 * 在那些不关心数据对齐的调用方中，应使用 PG_DETOAST_DATUM_PACKED()、
 * VARDATA_ANY()、VARSIZE_ANY() 与 VARSIZE_ANY_EXHDR()。其他场合应使用
 * PG_DETOAST_DATUM()、VARDATA() 与 VARSIZE()。在表示 Datum 布局的结构体中
 * 直接获取 int16、int32 或更宽字段需要数据对齐。memcpy() 不关心对齐，大多数
 * 数据类型上的操作（例如只包含 char 字段的 text）也是如此。
 *
 * 组装一个新 Datum 的代码应当调用 VARDATA() 和 SET_VARSIZE()。
 * （Datum 诞生时都是未 TOAST 的。）
 *
 * 这里的其它宏通常只应由元组组装/拆解代码，以及那些专门处理“仍为 TOAST 状态”
 * 的 Datum 的代码使用。
 */
#define VARDATA(PTR)						VARDATA_4B(PTR)
#define VARSIZE(PTR)						VARSIZE_4B(PTR)

#define VARSIZE_SHORT(PTR)					VARSIZE_1B(PTR)
#define VARDATA_SHORT(PTR)					VARDATA_1B(PTR)

#define VARTAG_EXTERNAL(PTR)				VARTAG_1B_E(PTR)
#define VARSIZE_EXTERNAL(PTR)				(VARHDRSZ_EXTERNAL + VARTAG_SIZE(VARTAG_EXTERNAL(PTR)))
#define VARDATA_EXTERNAL(PTR)				VARDATA_1B_E(PTR)

#define VARATT_IS_COMPRESSED(PTR)			VARATT_IS_4B_C(PTR)
#define VARATT_IS_EXTERNAL(PTR)				VARATT_IS_1B_E(PTR)
#define VARATT_IS_EXTERNAL_ONDISK(PTR) \
	(VARATT_IS_EXTERNAL(PTR) && VARTAG_EXTERNAL(PTR) == VARTAG_ONDISK)
#define VARATT_IS_EXTERNAL_INDIRECT(PTR) \
	(VARATT_IS_EXTERNAL(PTR) && VARTAG_EXTERNAL(PTR) == VARTAG_INDIRECT)
#define VARATT_IS_EXTERNAL_EXPANDED_RO(PTR) \
	(VARATT_IS_EXTERNAL(PTR) && VARTAG_EXTERNAL(PTR) == VARTAG_EXPANDED_RO)
#define VARATT_IS_EXTERNAL_EXPANDED_RW(PTR) \
	(VARATT_IS_EXTERNAL(PTR) && VARTAG_EXTERNAL(PTR) == VARTAG_EXPANDED_RW)
#define VARATT_IS_EXTERNAL_EXPANDED(PTR) \
	(VARATT_IS_EXTERNAL(PTR) && VARTAG_IS_EXPANDED(VARTAG_EXTERNAL(PTR)))
#define VARATT_IS_EXTERNAL_NON_EXPANDED(PTR) \
	(VARATT_IS_EXTERNAL(PTR) && !VARTAG_IS_EXPANDED(VARTAG_EXTERNAL(PTR)))
#define VARATT_IS_SHORT(PTR)				VARATT_IS_1B(PTR)
#define VARATT_IS_EXTENDED(PTR)				(!VARATT_IS_4B_U(PTR))

#define SET_VARSIZE(PTR, len)				SET_VARSIZE_4B(PTR, len)
#define SET_VARSIZE_SHORT(PTR, len)			SET_VARSIZE_1B(PTR, len)
#define SET_VARSIZE_COMPRESSED(PTR, len)	SET_VARSIZE_4B_C(PTR, len)

#define SET_VARTAG_EXTERNAL(PTR, tag)		SET_VARTAG_1B_E(PTR, tag)

#define VARSIZE_ANY(PTR) \
	(VARATT_IS_1B_E(PTR) ? VARSIZE_EXTERNAL(PTR) : \
	 (VARATT_IS_1B(PTR) ? VARSIZE_1B(PTR) : \
	  VARSIZE_4B(PTR)))

/* 一个 varlena 数据的大小，不含头部 */
#define VARSIZE_ANY_EXHDR(PTR) \
	(VARATT_IS_1B_E(PTR) ? VARSIZE_EXTERNAL(PTR)-VARHDRSZ_EXTERNAL : \
	 (VARATT_IS_1B(PTR) ? VARSIZE_1B(PTR)-VARHDRSZ_SHORT : \
	  VARSIZE_4B(PTR)-VARHDRSZ))

/* 注意：该宏对行外或行内压缩的 Datum 无效 */
/* 注意：该宏可能返回一个未对齐的指针 */
#define VARDATA_ANY(PTR) \
	 (VARATT_IS_1B(PTR) ? VARDATA_1B(PTR) : VARDATA_4B(PTR))

/* 行内压缩 Datum 的解压后大小与压缩方法 */
#define VARDATA_COMPRESSED_GET_EXTSIZE(PTR) \
	(((varattrib_4b *) (PTR))->va_compressed.va_tcinfo & VARLENA_EXTSIZE_MASK)
#define VARDATA_COMPRESSED_GET_COMPRESS_METHOD(PTR) \
	(((varattrib_4b *) (PTR))->va_compressed.va_tcinfo >> VARLENA_EXTSIZE_BITS)

/* 行外 Datum 同理；但注意参数是一个 struct varatt_external */
#define VARATT_EXTERNAL_GET_EXTSIZE(toast_pointer) \
	((toast_pointer).va_extinfo & VARLENA_EXTSIZE_MASK)
#define VARATT_EXTERNAL_GET_COMPRESS_METHOD(toast_pointer) \
	((toast_pointer).va_extinfo >> VARLENA_EXTSIZE_BITS)

#define VARATT_EXTERNAL_SET_SIZE_AND_COMPRESS_METHOD(toast_pointer, len, cm) \
	do { \
		Assert((cm) == TOAST_PGLZ_COMPRESSION_ID || \
			   (cm) == TOAST_LZ4_COMPRESSION_ID); \
		((toast_pointer).va_extinfo = \
			(len) | ((uint32) (cm) << VARLENA_EXTSIZE_BITS)); \
	} while (0)

/*
 * 现在判断一个行外存储的值是否被压缩，需要比较 va_extinfo 中存储的大小
 * （外部数据的实际长度）与 rawsize（原始未压缩 Datum 的大小）。后者包含
 * VARHDRSZ 开销，前者不包含。我们只有在真正节省空间时才会启用压缩，
 * 因此预期两者相等或前者更小。
 */
#define VARATT_EXTERNAL_IS_COMPRESSED(toast_pointer) \
	(VARATT_EXTERNAL_GET_EXTSIZE(toast_pointer) < \
	 (toast_pointer).va_rawsize - VARHDRSZ)

#endif
