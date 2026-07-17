/*-------------------------------------------------------------------------
 *
 * datum.h
 *	  POSTGRES Datum（抽象数据类型）的操作例程。
 *
 * 这些例程由 'typbyval' 和 'typlen' 信息驱动，调用方须事先针对该 Datum
 * 的数据类型取得这些信息。（我们采用这种方式，是因为在大多数情况下调用方
 * 只需查询一次该信息，即可用于多次针对单个 datum 的操作。）
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/datum.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef DATUM_H
#define DATUM_H

/*
 * datumGetSize - 求取一个 datum 的"实际"长度
 */
extern Size datumGetSize(Datum value, bool typByVal, int typLen);

/*
 * datumCopy - 复制一个非 NULL 的 datum。
 *
 * 如果数据类型是按引用传递的，则使用 palloc() 分配内存。
 */
extern Datum datumCopy(Datum value, bool typByVal, int typLen);

/*
 * datumTransfer - 将一个非 NULL 的 datum 转移到当前内存上下文。
 *
 * 与 datumCopy() 的不同之处在于它对可读写扩展对象的处理方式。
 */
extern Datum datumTransfer(Datum value, bool typByVal, int typLen);

/*
 * datumIsEqual
 * 如果两个同类型 datum 相等则返回 true，否则返回 false。
 *
 * XXX：限制条件参见代码中的注释！
 */
extern bool datumIsEqual(Datum value1, Datum value2,
						 bool typByVal, int typLen);

/*
 * datum_image_eq
 *
 * 基于字节映像比较两个 datum 的内容是否完全相同。若两个 datum 相等则返回
 * true，否则返回 false。
 */
extern bool datum_image_eq(Datum value1, Datum value2,
						   bool typByVal, int typLen);

/*
 * datum_image_hash
 *
 * 为 'value' 生成哈希值，基于其二进制位而非逻辑值。
 */
extern uint32 datum_image_hash(Datum value, bool typByVal, int typLen);

/*
 * 对 datum 进行序列化与反序列化，以便将其传递给并行工作进程。
 */
extern Size datumEstimateSpace(Datum value, bool isnull, bool typByVal,
							   int typLen);
extern void datumSerialize(Datum value, bool isnull, bool typByVal,
						   int typLen, char **start_address);
extern Datum datumRestore(char **start_address, bool *isnull);

#endif							/* DATUM_H */
