/*-------------------------------------------------------------------------
 *
 * postgres.h
 *	  PostgreSQL 服务端 .c 文件的主要包含文件
 *
 * 这应该是 PostgreSQL 后端模块包含的第一个文件。
 * 客户端代码应改为包含 postgres_fe.h。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1995, Regents of the University of California
 *
 * src/include/postgres.h
 *
 *-------------------------------------------------------------------------
 */
/* IWYU pragma: always_keep */
/*
 *----------------------------------------------------------------
 *	 目录
 *
 *		向此文件添加内容时，请尽量将内容放入相关的节，或者视情况
 *		新增节。
 *
 *	  节		描述
 *	  -------	------------------------------------------------
 *		1)		Datum 类型 + 支持函数
 *		2)		杂项
 *
 *	 说明
 *
 *	一般来说，此文件应包含后端环境中广泛需要、但在后端之外
 *	无关紧要的声明。
 *
 *	简单的类型定义位于 c.h 中，并在那里与 postgres_fe.h 共享。
 *	我们这样做是因为那些需要处理与后端的二进制数据传输的
 *	前端模块需要这些类型定义。此文件中的类型定义应当用于那些
 *	永远不会离开后端的表示形式，例如 Datum。
 *
 *----------------------------------------------------------------
 */
#ifndef POSTGRES_H
#define POSTGRES_H

/* IWYU pragma: begin_exports */

#include "c.h"
#include "utils/elog.h"
#include "utils/palloc.h"

/* IWYU pragma: end_exports */

/* ----------------------------------------------------------------
 *				第 1 节：Datum 类型 + 支持函数
 * ----------------------------------------------------------------
 */

/*
 * 一个 Datum 要么包含一个传值类型的值，要么包含一个指向传引用类型
 * 的值的指针。因此，我们要求：
 *
 * sizeof(Datum) == sizeof(void *) == 4 或 8
 *
 * 下面的这些函数，以及其他类型的类似函数，应当用于在 Datum 和
 * 相应的 C 类型之间进行转换。
 */

typedef uintptr_t Datum;

/*
 * NullableDatum 用于需要同时存储一个 Datum 及其是否为空的地方。由于
 * 更好的空间局部性，这比将 datum 和空值信息分别存储在不同的数组中
 * 更高效，即便可能因填充而浪费更多空间。
 */
typedef struct NullableDatum
{
#define FIELDNO_NULLABLE_DATUM_DATUM 0
	Datum		value;
#define FIELDNO_NULLABLE_DATUM_ISNULL 1
	bool		isnull;
	/* 由于对齐填充，这里可以免费用于存放标志位 */
} NullableDatum;

#define SIZEOF_DATUM SIZEOF_VOID_P

/*
 * DatumGetBool
 *		返回 datum 的布尔值。
 *
 * 注意：任何非零值都将被视为 true。
 */
static inline bool
DatumGetBool(Datum X)
{
	return (X != 0);
}

/*
 * BoolGetDatum
 *		返回布尔值的 datum 表示。
 *
 * 注意：任何非零值都将被视为 true。
 */
static inline Datum
BoolGetDatum(bool X)
{
	return (Datum) (X ? 1 : 0);
}

/*
 * DatumGetChar
 *		返回 datum 的字符值。
 */
static inline char
DatumGetChar(Datum X)
{
	return (char) X;
}

/*
 * CharGetDatum
 *		返回字符的 datum 表示。
 */
static inline Datum
CharGetDatum(char X)
{
	return (Datum) X;
}

/*
 * Int8GetDatum
 *		返回 8 位整数的 datum 表示。
 */
static inline Datum
Int8GetDatum(int8 X)
{
	return (Datum) X;
}

/*
 * DatumGetUInt8
 *		返回 datum 的 8 位无符号整数值。
 */
static inline uint8
DatumGetUInt8(Datum X)
{
	return (uint8) X;
}

/*
 * UInt8GetDatum
 *		返回 8 位无符号整数的 datum 表示。
 */
static inline Datum
UInt8GetDatum(uint8 X)
{
	return (Datum) X;
}

/*
 * DatumGetInt16
 *		返回 datum 的 16 位整数值。
 */
static inline int16
DatumGetInt16(Datum X)
{
	return (int16) X;
}

/*
 * Int16GetDatum
 *		返回 16 位整数的 datum 表示。
 */
static inline Datum
Int16GetDatum(int16 X)
{
	return (Datum) X;
}

/*
 * DatumGetUInt16
 *		返回 datum 的 16 位无符号整数值。
 */
static inline uint16
DatumGetUInt16(Datum X)
{
	return (uint16) X;
}

/*
 * UInt16GetDatum
 *		返回 16 位无符号整数的 datum 表示。
 */
static inline Datum
UInt16GetDatum(uint16 X)
{
	return (Datum) X;
}

/*
 * DatumGetInt32
 *		返回 datum 的 32 位整数值。
 */
static inline int32
DatumGetInt32(Datum X)
{
	return (int32) X;
}

/*
 * Int32GetDatum
 *		返回 32 位整数的 datum 表示。
 */
static inline Datum
Int32GetDatum(int32 X)
{
	return (Datum) X;
}

/*
 * DatumGetUInt32
 *		返回 datum 的 32 位无符号整数值。
 */
static inline uint32
DatumGetUInt32(Datum X)
{
	return (uint32) X;
}

/*
 * UInt32GetDatum
 *		返回 32 位无符号整数的 datum 表示。
 */
static inline Datum
UInt32GetDatum(uint32 X)
{
	return (Datum) X;
}

/*
 * DatumGetObjectId
 *		返回 datum 的对象标识符值。
 */
static inline Oid
DatumGetObjectId(Datum X)
{
	return (Oid) X;
}

/*
 * ObjectIdGetDatum
 *		返回对象标识符的 datum 表示。
 */
static inline Datum
ObjectIdGetDatum(Oid X)
{
	return (Datum) X;
}

/*
 * DatumGetTransactionId
 *		返回 datum 的事务标识符值。
 */
static inline TransactionId
DatumGetTransactionId(Datum X)
{
	return (TransactionId) X;
}

/*
 * TransactionIdGetDatum
 *		返回事务标识符的 datum 表示。
 */
static inline Datum
TransactionIdGetDatum(TransactionId X)
{
	return (Datum) X;
}

/*
 * MultiXactIdGetDatum
 *		返回 multixact 标识符的 datum 表示。
 */
static inline Datum
MultiXactIdGetDatum(MultiXactId X)
{
	return (Datum) X;
}

/*
 * DatumGetCommandId
 *		返回 datum 的命令标识符值。
 */
static inline CommandId
DatumGetCommandId(Datum X)
{
	return (CommandId) X;
}

/*
 * CommandIdGetDatum
 *		返回命令标识符的 datum 表示。
 */
static inline Datum
CommandIdGetDatum(CommandId X)
{
	return (Datum) X;
}

/*
 * DatumGetPointer
 *		返回 datum 的指针值。
 */
static inline Pointer
DatumGetPointer(Datum X)
{
	return (Pointer) X;
}

/*
 * PointerGetDatum
 *		返回指针的 datum 表示。
 */
static inline Datum
PointerGetDatum(const void *X)
{
	return (Datum) X;
}

/*
 * DatumGetCString
 *		返回 datum 的 C 字符串（以 null 结尾的字符串）值。
 *
 * 注意：C 字符串目前并不是 PostgreSQL 的一个完整类型，
 * 但类型输入函数会利用此转换来处理它们的输入。
 */
static inline char *
DatumGetCString(Datum X)
{
	return (char *) DatumGetPointer(X);
}

/*
 * CStringGetDatum
 *		返回 C 字符串（以 null 结尾的字符串）的 datum 表示。
 *
 * 注意：C 字符串目前并不是 PostgreSQL 的一个完整类型，
 * 但类型输出函数会利用此转换来处理它们的输出。
 * 注意：CString 是传引用的；调用者必须确保所指向的值具有足够的生命周期。
 */
static inline Datum
CStringGetDatum(const char *X)
{
	return PointerGetDatum(X);
}

/*
 * DatumGetName
 *		返回 datum 的 name 值。
 */
static inline Name
DatumGetName(Datum X)
{
	return (Name) DatumGetPointer(X);
}

/*
 * NameGetDatum
 *		返回 name 的 datum 表示。
 *
 * 注意：Name 是传引用的；调用者必须确保所指向的值具有足够的生命周期。
 */
static inline Datum
NameGetDatum(const NameData *X)
{
	return CStringGetDatum(NameStr(*X));
}

/*
 * DatumGetInt64
 *		返回 datum 的 64 位整数值。
 *
 * 注意：此函数隐藏了 int64 是传值还是传引用的细节。
 */
static inline int64
DatumGetInt64(Datum X)
{
#ifdef USE_FLOAT8_BYVAL
	return (int64) X;
#else
	return *((int64 *) DatumGetPointer(X));
#endif
}

/*
 * Int64GetDatum
 *		返回 64 位整数的 datum 表示。
 *
 * 注意：如果 int64 是传引用的，此函数返回的是一个指向 palloc
 * 分配的内存的引用。
 */
#ifdef USE_FLOAT8_BYVAL
static inline Datum
Int64GetDatum(int64 X)
{
	return (Datum) X;
}
#else
extern Datum Int64GetDatum(int64 X);
#endif


/*
 * DatumGetUInt64
 *		返回 datum 的 64 位无符号整数值。
 *
 * 注意：此函数隐藏了 int64 是传值还是传引用的细节。
 */
static inline uint64
DatumGetUInt64(Datum X)
{
#ifdef USE_FLOAT8_BYVAL
	return (uint64) X;
#else
	return *((uint64 *) DatumGetPointer(X));
#endif
}

/*
 * UInt64GetDatum
 *		返回 64 位无符号整数的 datum 表示。
 *
 * 注意：如果 int64 是传引用的，此函数返回的是一个指向 palloc
 * 分配的内存的引用。
 */
static inline Datum
UInt64GetDatum(uint64 X)
{
#ifdef USE_FLOAT8_BYVAL
	return (Datum) X;
#else
	return Int64GetDatum((int64) X);
#endif
}

/*
 * Float 与 Datum 之间的转换
 *
 * 在传值时，这些必须以内联函数而非宏来实现，因为许多机器传递 int 和
 * float 的函数参数/返回值的方式不同；因此我们需要用联合体来玩一些
 * 取巧的手段。
 */

/*
 * DatumGetFloat4
 *		返回 datum 的 4 字节浮点值。
 */
static inline float4
DatumGetFloat4(Datum X)
{
	union
	{
		int32		value;
		float4		retval;
	}			myunion;

	myunion.value = DatumGetInt32(X);
	return myunion.retval;
}

/*
 * Float4GetDatum
 *		返回 4 字节浮点数的 datum 表示。
 */
static inline Datum
Float4GetDatum(float4 X)
{
	union
	{
		float4		value;
		int32		retval;
	}			myunion;

	myunion.value = X;
	return Int32GetDatum(myunion.retval);
}

/*
 * DatumGetFloat8
 *		返回 datum 的 8 字节浮点值。
 *
 * 注意：此函数隐藏了 float8 是传值还是传引用的细节。
 */
static inline float8
DatumGetFloat8(Datum X)
{
#ifdef USE_FLOAT8_BYVAL
	union
	{
		int64		value;
		float8		retval;
	}			myunion;

	myunion.value = DatumGetInt64(X);
	return myunion.retval;
#else
	return *((float8 *) DatumGetPointer(X));
#endif
}

/*
 * Float8GetDatum
 *		返回 8 字节浮点数的 datum 表示。
 *
 * 注意：如果 float8 是传引用的，此函数返回的是一个指向 palloc
 * 分配的内存的引用。
 */
#ifdef USE_FLOAT8_BYVAL
static inline Datum
Float8GetDatum(float8 X)
{
	union
	{
		float8		value;
		int64		retval;
	}			myunion;

	myunion.value = X;
	return Int64GetDatum(myunion.retval);
}
#else
extern Datum Float8GetDatum(float8 X);
#endif


/*
 * Int64GetDatumFast
 * Float8GetDatumFast
 *
 * 这些宏旨在允许编写不依赖于 int64 和 float8 是否为传引用类型的代码，
 * 同时又不牺牲它们作为传引用类型时的性能。参数必须是一个变量，该变量
 * 在 Datum 被需要期间一直存在且保持相同的值。在传引用的情况下，会取
 * 该变量的地址作为 Datum 使用。在传值的情况下，这些宏与非 Fast 版本
 * 的函数相同，只是会断言变量具有正确的类型。
 */

#ifdef USE_FLOAT8_BYVAL
#define Int64GetDatumFast(X) \
	(AssertVariableIsOfTypeMacro(X, int64), Int64GetDatum(X))
#define Float8GetDatumFast(X) \
	(AssertVariableIsOfTypeMacro(X, double), Float8GetDatum(X))
#else
#define Int64GetDatumFast(X) \
	(AssertVariableIsOfTypeMacro(X, int64), PointerGetDatum(&(X)))
#define Float8GetDatumFast(X) \
	(AssertVariableIsOfTypeMacro(X, double), PointerGetDatum(&(X)))
#endif


/* ----------------------------------------------------------------
 *				第 2 节：杂项
 * ----------------------------------------------------------------
 */

/*
 * NON_EXEC_STATIC：有时定义一个通常是 static、但在使用 EXEC_BACKEND 时
 * 为 extern 的变量或函数会比较有用（参见 pg_config_manual.h）。通常
 * 在 postmaster.c 中会有些代码利用这些 extern 符号在进程之间传递状态，
 * 或在 EXEC_BACKEND 模式下做它需要做的其他事情。
 */
#ifdef EXEC_BACKEND
#define NON_EXEC_STATIC
#else
#define NON_EXEC_STATIC static
#endif

#endif							/* POSTGRES_H */
