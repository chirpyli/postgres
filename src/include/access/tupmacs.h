/*-------------------------------------------------------------------------
 *
 * tupmacs.h
 *	  同时供索引元组与堆元组使用的元组宏。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/tupmacs.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TUPMACS_H
#define TUPMACS_H

#include "catalog/pg_type_d.h"	/* 用于 TYPALIGN 宏 */


/*
 * 检查元组的 NULL 位图以确定某个属性是否为 NULL。
 * 注意，NULL 位图中的 0 表示 NULL，而 1 表示非 NULL。
 */
static inline bool
att_isnull(int ATT, const bits8 *BITS)
{
	return !(BITS[ATT >> 3] & (1 << (ATT & 0x07)));
}

#ifndef FRONTEND
/*
 * 给定一个取自 Form_pg_attribute 或 CompactAttribute 的 attbyval 与 attlen，
 * 以及指向元组数据区的指针，返回正确的值或指针。
 *
 * 在所有情况下我们都返回一个 Datum 值。若 attbyval 为假，则返回传入的、
 * 指向元组数据区的同一个指针。否则，我们返回从数据区中取出的正确字节数，
 * 并扩展为 Datum 形式。
 *
 * 在 Datum 为 8 字节的机器上，我们支持取出 8 字节的按值传递属性；
 * 否则仅支持 1、2 和 4 字节的值。
 *
 * 注意，T 必须已经正确对齐，本函数才能正常工作。
 */
#define fetchatt(A,T) fetch_att(T, (A)->attbyval, (A)->attlen)

/*
 * 同上，但使用 byval/len 参数而非 Form_pg_attribute 工作。
 */
static inline Datum
fetch_att(const void *T, bool attbyval, int attlen)
{
	if (attbyval)
	{
		switch (attlen)
		{
			case sizeof(char):
				return CharGetDatum(*((const char *) T));
			case sizeof(int16):
				return Int16GetDatum(*((const int16 *) T));
			case sizeof(int32):
				return Int32GetDatum(*((const int32 *) T));
#if SIZEOF_DATUM == 8
			case sizeof(Datum):
				return *((const Datum *) T);
#endif
			default:
				elog(ERROR, "unsupported byval length: %d", attlen);
				return 0;
		}
	}
	else
		return PointerGetDatum(T);
}
#endif							/* FRONTEND */

/*
 * att_align_datum 会依据对齐要求 attalign 与类型长度 attlen，
 * 按需对齐给定的偏移量。attdatum 是我们打算打包进元组的 Datum 变量
 * （仅当处理 varlena 类型时才会被访问）。注意，这假设 Datum 将被原样存储；
 * 打算把非短格式的 varlena Datum 转换为短格式的调用方，必须自行处理这一点。
 */
#define att_align_datum(cur_offset, attalign, attlen, attdatum) \
( \
	((attlen) == -1 && VARATT_IS_SHORT(DatumGetPointer(attdatum))) ? \
	(uintptr_t) (cur_offset) : \
	att_align_nominal(cur_offset, attalign) \
)

/*
 * 与 att_align_datum 类似，但接受以字节数为单位的对齐量，
 * 通常取自 CompactAttribute.attalignby，用以对齐 Datum。
 */
#define att_datum_alignby(cur_offset, attalignby, attlen, attdatum) \
	( \
	((attlen) == -1 && VARATT_IS_SHORT(DatumGetPointer(attdatum))) ? \
	(uintptr_t) (cur_offset) : \
	TYPEALIGN(attalignby, cur_offset))

/*
 * att_align_pointer 执行与 att_align_datum 相同的计算，
 * 但用于遍历元组之时。attptr 是当前实际的数据指针；
 * 当访问 varlena 字段时，我们必须“窥探”以判断当前看到的是填充字节，
 * 还是 1 字节头 datum 的首字节。
 * （零字节必定要么是一个填充字节，要么是一个正确对齐的 4 字节长度字的首字节；
 * 无论哪种情况我们都可以安全对齐。非零字节必定要么是 1 字节长度字，
 * 要么是正确对齐的 4 字节长度字的首字节；无论哪种情况我们都无需对齐。）
 *
 * 注意：有些调用方会传入一个 "char *" 指针作为 cur_offset。
 * 这有点像个 hack，但只要 uintptr_t 的宽度正确，就应当能正常工作。
 */
#define att_align_pointer(cur_offset, attalign, attlen, attptr) \
( \
	((attlen) == -1 && VARATT_NOT_PAD_BYTE(attptr)) ? \
	(uintptr_t) (cur_offset) : \
	att_align_nominal(cur_offset, attalign) \
)

/*
 * 与 att_align_pointer 类似，但接受以字节数为单位的对齐量，
 * 通常取自 CompactAttribute.attalignby，用以对齐指针。
 */
#define att_pointer_alignby(cur_offset, attalignby, attlen, attptr) \
	( \
	((attlen) == -1 && VARATT_NOT_PAD_BYTE(attptr)) ? \
	(uintptr_t) (cur_offset) : \
	TYPEALIGN(attalignby, cur_offset))

/*
 * att_align_nominal 会依据对齐要求 attalign 按需对齐给定的偏移量，
 * 同时忽略对打包 varlena datum 的任何考量。直接使用本宏有三种主要场景：
 *	* 我们知道相关属性并非 varlena（attlen != -1）；
 *	  此时它比上面的宏更廉价，且同样正确。
 *	* 我们需要抽象地（即不参考真实元组）估算对齐填充的代价。
 *	  此时我们必须假设最坏情况，即所有 varlena 都已被对齐。
 *	* 在数组与 multirange 内部，我们无条件对齐 varlena（XXX 这一点
 *	  大概应该重新审视）。
 *
 * 各种 attalign 情形的测试顺序，但愿是按其出现频率排列的。
 */
#define att_align_nominal(cur_offset, attalign) \
( \
	((attalign) == TYPALIGN_INT) ? INTALIGN(cur_offset) : \
	 (((attalign) == TYPALIGN_CHAR) ? (uintptr_t) (cur_offset) : \
	  (((attalign) == TYPALIGN_DOUBLE) ? DOUBLEALIGN(cur_offset) : \
	   ( \
			AssertMacro((attalign) == TYPALIGN_SHORT), \
			SHORTALIGN(cur_offset) \
	   ))) \
)

/*
 * 与 att_align_nominal 类似，但接受以字节数为单位的对齐量，
 * 通常取自 CompactAttribute.attalignby，用以对齐偏移量。
 */
#define att_nominal_alignby(cur_offset, attalignby) \
	TYPEALIGN(attalignby, cur_offset)

/*
 * att_addlength_datum 会把给定偏移量按所需 Datum 变量占用的空间递增。
 * 仅当处理变长属性时才会访问 attdatum。
 */
#define att_addlength_datum(cur_offset, attlen, attdatum) \
	att_addlength_pointer(cur_offset, attlen, DatumGetPointer(attdatum))

/*
 * att_addlength_pointer 执行与 att_addlength_datum 相同的计算，
 * 但用于遍历元组之时 —— attptr 是指向元组内该字段的指针。
 *
 * 注意：有些调用方会传入一个 "char *" 指针作为 cur_offset。
 * 这实际上完全没问题，但大概应当连同 att_align_pointer 中的相同做法一起清理掉。
 */
#define att_addlength_pointer(cur_offset, attlen, attptr) \
( \
	((attlen) > 0) ? \
	( \
		(cur_offset) + (attlen) \
	) \
	: (((attlen) == -1) ? \
	( \
		(cur_offset) + VARSIZE_ANY(attptr) \
	) \
	: \
	( \
		AssertMacro((attlen) == -2), \
		(cur_offset) + (strlen((char *) (attptr)) + 1) \
	)) \
)

#ifndef FRONTEND
/*
 * store_att_byval 是 fetch_att 的部分逆操作：将一个给定的 Datum
 * 值存入元组数据区中指定的地址。不过，它只处理按值传递的情况，
 * 因为在典型用法中调用方本就需要区分按值传递与按引用传递两种情况，
 * 因此一个“包办一切”的函数并不方便。
 */
static inline void
store_att_byval(void *T, Datum newdatum, int attlen)
{
	switch (attlen)
	{
		case sizeof(char):
			*(char *) T = DatumGetChar(newdatum);
			break;
		case sizeof(int16):
			*(int16 *) T = DatumGetInt16(newdatum);
			break;
		case sizeof(int32):
			*(int32 *) T = DatumGetInt32(newdatum);
			break;
#if SIZEOF_DATUM == 8
		case sizeof(Datum):
			*(Datum *) T = newdatum;
			break;
#endif
		default:
			elog(ERROR, "unsupported byval length: %d", attlen);
	}
}
#endif							/* FRONTEND */

#endif							/* TUPMACS_H */
