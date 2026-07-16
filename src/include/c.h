/*-------------------------------------------------------------------------
 *
 * c.h
 *	  基础的 C 定义。PostgreSQL 中的每个 .c 文件都通过 postgres.h 或
 *	  postgres_fe.h（视情况而定）间接包含此文件。
 *
 *	  注意，此处的定义不打算暴露给前端接口库的客户端——因此我们不太担心
 *	  用大量东西污染命名空间……
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/c.h
 *
 *-------------------------------------------------------------------------
 */
/* IWYU pragma: always_keep */
/*
 *----------------------------------------------------------------
 *	 目录
 *
 *		当向本文件添加内容时，请尽量将其放入相关的节中，或酌情添加新的节。
 *
 *	  节号		描述
 *	  -------	------------------------------------------------
 *		0)		pg_config.h 和标准系统头文件
 *		1)		编译器特性
 *		2)		bool, true, false
 *		3)		标准系统类型
 *		4)		系统类型的 IsValid 宏
 *		5)		lengthof, 对齐
 *		6)		断言
 *		7)		广泛适用的宏
 *		8)		杂项
 *		9)		系统相关 hack
 *
 * 注意：由于本文件同时被前端和后端模块包含，通常不应在此放置 "extern"
 * 声明，除非用 #ifdef 限定其只在一侧可见。typedef 和宏是适合放在这里的内容。
 *
 *----------------------------------------------------------------
 */
#ifndef C_H
#define C_H

/* IWYU pragma: begin_exports */

/*
 * 这些头文件必须在任何系统头文件之前包含，因为在某些平台上它们会影响
 * 系统头文件的行为（例如，通过定义 _FILE_OFFSET_BITS）。
 */
#include "pg_config.h"
#include "pg_config_manual.h"	/* 必须在 pg_config.h 之后 */
#include "pg_config_os.h"		/* 来自 include/port/PORTNAME.h 的配置 */

/* Postgres 中所有地方都应当可用的系统头文件 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdarg.h>
#ifdef HAVE_STRINGS_H
#include <strings.h>
#endif
#include <stdint.h>
#include <sys/types.h>
#include <errno.h>
#if defined(WIN32) || defined(__CYGWIN__)
#include <fcntl.h>				/* 确保 O_BINARY 可用 */
#endif
#include <locale.h>
#ifdef HAVE_XLOCALE_H
#include <xlocale.h>
#endif
#ifdef ENABLE_NLS
#include <libintl.h>
#endif

 /* 拉入我们也向应用程序暴露的基本符号 */
#include "postgres_ext.h"

/* 在包含 zlib.h 之前定义，为 zlib API 添加 const 修饰 */
#ifdef HAVE_LIBZ
#define ZLIB_CONST
#endif


/* ----------------------------------------------------------------
 *				第 1 节: 编译器特性
 *
 * 类型前缀（const、signed、volatile、inline）在 pg_config.h 中处理。
 * ----------------------------------------------------------------
 */

/*
 * 如果定义了 PG_FORCE_DISABLE_INLINE，则禁用 "inline"。
 * 这用于绕过编译器 bug，也可能用于调査目的。
 */
#ifdef PG_FORCE_DISABLE_INLINE
#undef inline
#define inline
#endif

/*
 * 属性宏
 *
 * GCC: https://gcc.gnu.org/onlinedocs/gcc/Function-Attributes.html
 * GCC: https://gcc.gnu.org/onlinedocs/gcc/Type-Attributes.html
 * Clang: https://clang.llvm.org/docs/AttributeReference.html
 * Sunpro: https://docs.oracle.com/cd/E18659_01/html/821-1384/gjzke.html
 */

/*
 * 对于不支持 __has_attribute 的编译器，我们将 __has_attribute(x) 定义为 0，
 * 以便更轻松地在下面为各种 __attribute__ 定义宏。
 */
#ifndef __has_attribute
#define __has_attribute(attribute) 0
#endif

/* 只有 GCC 支持 unused 属性 */
#ifdef __GNUC__
#define pg_attribute_unused() __attribute__((unused))
#else
#define pg_attribute_unused()
#endif

/*
 * pg_nodiscard 表示如果函数调用的结果被忽略，编译器应发出警告。
 * 名称 "nodiscard" 与 C23 标准中的同名属性保持一致。为了最大程度地向前兼容，
 * 请将其放在声明之前。
 */
#ifdef __GNUC__
#define pg_nodiscard __attribute__((warn_unused_result))
#else
#define pg_nodiscard
#endif

/*
 * pg_noreturn 对应于 C11 的 noreturn/_Noreturn 函数修饰符。
 * 我们不能使用标准名称 "noreturn"，因为某些第三方代码在头文件中使用了
 * __attribute__((noreturn))，如果 "noreturn" 被定义为 "_Noreturn"（如
 * <stdnoreturn.h> 所做的那样），就会产生混淆。
 *
 * 在声明中，函数修饰符位于函数名之前。常见风格是将其放在返回类型之前。
 * （MSVC 的 fallback 有相同要求。GCC 的 fallback 更灵活。）
 */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define pg_noreturn _Noreturn
#elif defined(__GNUC__) || defined(__SUNPRO_C)
#define pg_noreturn __attribute__((noreturn))
#elif defined(_MSC_VER)
#define pg_noreturn __declspec(noreturn)
#else
#define pg_noreturn
#endif

/*
 * 此宏将禁用函数的地址安全检查（address sanitizer），当使用
 * "-fsanitize=address" 编译时生效。使用前请三思！
 */
#if defined(__clang__) || __GNUC__ >= 8
#define pg_attribute_no_sanitize_address() __attribute__((no_sanitize("address")))
#elif __has_attribute(no_sanitize_address)
/* 这对 clang 也有效，但已弃用。 */
#define pg_attribute_no_sanitize_address() __attribute__((no_sanitize_address))
#else
#define pg_attribute_no_sanitize_address()
#endif

/*
 * 将此宏放在允许进行非对齐访问的函数之前。在非 x86 专用代码上使用前请三思！
 * 测试可用 clang 的 "-fsanitize=alignment -fsanitize-trap=alignment"
 * 或 gcc 的 "-fsanitize=alignment -fno-sanitize-recover=alignment"。
 */
#if __clang_major__ >= 7 || __GNUC__ >= 8
#define pg_attribute_no_sanitize_alignment() __attribute__((no_sanitize("alignment")))
#else
#define pg_attribute_no_sanitize_alignment()
#endif

/*
 * pg_attribute_nonnull 表示如果函数被调用时列出的参数为 NULL，
 * 编译器应发出警告。如果没有列出参数，则任何指针参数为 NULL 时编译器都应警告。
 */
#if __has_attribute (nonnull)
#define pg_attribute_nonnull(...) __attribute__((nonnull(__VA_ARGS__)))
#else
#define pg_attribute_nonnull(...)
#endif

/*
 * pg_attribute_target 允许指定函数编译时所使用的不同目标选项
 * （例如，用于使用特殊的 CPU 指令）。
 * 请注意，仍然需要在 configure 时验证编译器是否能理解特定的目标。
 */
#if __has_attribute (target)
#define pg_attribute_target(...) __attribute__((target(__VA_ARGS__)))
#else
#define pg_attribute_target(...)
#endif

/*
 * 将 PG_USED_FOR_ASSERTS_ONLY 添加到仅在启用断言的构建中使用的变量定义上，
 * 以避免在禁用断言的构建中出现"未使用变量"的编译器警告。
 */
#ifdef USE_ASSERT_CHECKING
#define PG_USED_FOR_ASSERTS_ONLY
#else
#define PG_USED_FOR_ASSERTS_ONLY pg_attribute_unused()
#endif

/*
 * 我们的 C 和 C++ 编译器可能对哪种 printf 原型最能代表
 * src/port/snprintf.c 的行为有不同的看法。
 */
#ifndef __cplusplus
#define PG_PRINTF_ATTRIBUTE PG_C_PRINTF_ATTRIBUTE
#else
#define PG_PRINTF_ATTRIBUTE PG_CXX_PRINTF_ATTRIBUTE
#endif

/* GCC 支持 format 属性 */
#if defined(__GNUC__)
#define pg_attribute_format_arg(a) __attribute__((format_arg(a)))
#define pg_attribute_printf(f,a) __attribute__((format(PG_PRINTF_ATTRIBUTE, f, a)))
#else
#define pg_attribute_format_arg(a)
#define pg_attribute_printf(f,a)
#endif

/* GCC 和 Sunpro 支持 aligned 和 packed */
#if defined(__GNUC__) || defined(__SUNPRO_C)
#define pg_attribute_aligned(a) __attribute__((aligned(a)))
#define pg_attribute_packed() __attribute__((packed))
#elif defined(_MSC_VER)
/*
 * MSVC 支持 aligned。
 *
 * Packing 也可用，但只能通过包裹整个结构体定义来实现，
 * 这不适合我们当前的宏声明风格。
 */
#define pg_attribute_aligned(a) __declspec(align(a))
#else
/*
 * 注意：aligned 和 packed 没有提供默认定义，因为它们影响代码功能；
 * 若要使用它们，编译器*必须*实现。
 */
#endif

/*
 * 对于那些我们希望强制内联的函数（即使编译器的启发式算法选择不内联），
 * 使用 "pg_attribute_always_inline" 代替 "inline"。但是，如果可能的话，
 * 不要在未优化的调试构建中强制内联。
 */
#if (defined(__GNUC__) && __GNUC__ > 3 && defined(__OPTIMIZE__)) || defined(__SUNPRO_C)
/* GCC > 3 和 Sunpro 通过 __attribute__ 支持 always_inline */
#define pg_attribute_always_inline __attribute__((always_inline)) inline
#elif defined(_MSC_VER)
/* MSVC 为此提供了特殊关键字 */
#define pg_attribute_always_inline __forceinline
#else
/* 否则，我们最多只能使用 "inline" */
#define pg_attribute_always_inline inline
#endif

/*
 * 强制不内联函数在以下场景中很有用：它是性能关键函数的慢路径，
 * 或者应该在 profile 中可见以进行恰当的成本归属。
 * 注意：与上面的 pg_attribute_XXX 宏不同，此宏应放在函数的返回类型和名称之前。
 */
/* GCC 和 Sunpro 通过 __attribute__ 支持 noinline */
#if (defined(__GNUC__) && __GNUC__ > 2) || defined(__SUNPRO_C)
#define pg_noinline __attribute__((noinline))
/* msvc 通过 declspec 支持 */
#elif defined(_MSC_VER)
#define pg_noinline __declspec(noinline)
#else
#define pg_noinline
#endif

/*
 * 目前，在 minGW 8.1 上将 pg_attribute_cold 和 pg_attribute_hot 定义为
 * 空宏。那里似乎有一个编译器的 bug 会导致编译失败。目前，我们至少还有
 * 一台 buildfarm 机器在运行该编译器，所以这样做应该能让它恢复正常。
 * 这个编译器可能不够普及，不值得永远保留这段代码，所以等最后一台
 * buildfarm 机器升级后，我们就可以直接移除它。
 */
#if defined(__MINGW64__) && __GNUC__ == 8 && __GNUC_MINOR__ == 1

#define pg_attribute_cold
#define pg_attribute_hot

#else
/*
 * 将某些函数标记为 "hot" 或 "cold" 有助于编译器以更高效的方式排列
 * 汇编代码。
 */
#if __has_attribute (cold)
#define pg_attribute_cold __attribute__((cold))
#else
#define pg_attribute_cold
#endif

#if __has_attribute (hot)
#define pg_attribute_hot __attribute__((hot))
#else
#define pg_attribute_hot
#endif

#endif							/* defined(__MINGW64__) && __GNUC__ == 8 &&
								 * __GNUC_MINOR__ == 1 */
/*
 * 以可移植的方式标记一个点不可达。最好让编译器能理解这一点，以辅助
 * 代码生成。在启用断言的构建中，出于调试原因，我们优先使用 abort()。
 */
#if defined(HAVE__BUILTIN_UNREACHABLE) && !defined(USE_ASSERT_CHECKING)
#define pg_unreachable() __builtin_unreachable()
#elif defined(_MSC_VER) && !defined(USE_ASSERT_CHECKING)
#define pg_unreachable() __assume(0)
#else
#define pg_unreachable() abort()
#endif

/*
 * 向编译器提示一条分支的可能性。likely() 和 unlikely() 都返回
 * 所包含表达式的布尔值。
 *
 * 这些只应在极少的情况下、在非常热的代码路径中使用。人们很容易
 * 错误估计可能性。
 */
#if __GNUC__ >= 3
#define likely(x)	__builtin_expect((x) != 0, 1)
#define unlikely(x) __builtin_expect((x) != 0, 0)
#else
#define likely(x)	((x) != 0)
#define unlikely(x) ((x) != 0)
#endif

/*
 * CppAsString
 *		使用 C 预处理器将参数转换为字符串。
 * CppAsString2
 *		将参数经过一轮宏展开后转换为字符串。
 * CppConcat
 *		使用 C 预处理器将两个参数连接在一起。
 *
 * 注意：这里曾经支持不支持 # 和 ## 的前 ANSI C 编译器。如今，这些宏
 * 只是为了保证清晰性和/或与现有 PostgreSQL 代码的向后兼容性。
 */
#define CppAsString(identifier) #identifier
#define CppAsString2(x)			CppAsString(x)
#define CppConcat(x, y)			x##y

/*
 * VA_ARGS_NARGS
 *		返回传递给它的宏参数的个数。
 *
 * 空参数也算一个参数，实际上，这意味着返回值是"参数列表中逗号的数量加一"。
 *
 * 该宏最多支持 63 个参数。在内部，VA_ARGS_NARGS_() 被传入 64+N 个参数，
 * 而 C99 标准只要求宏最多允许 127 个参数，因此我们无法可移植地支持更多。
 * 实现非常直观：VA_ARGS_NARGS_() 返回其第 64 个参数，我们通过设置调用
 * 使得该参数恰好是常量列表中对应的那一个。这个想法来自 Laurent Deniau。
 *
 * MSVC 对 __VA_ARGS__ 的实现不符合标准，除非使用 /Zc:preprocessor
 * 编译器标志，但该标志在 Visual Studio 2019 之前不可用。目前，我们对
 * 旧编译器使用一个不同的定义。
 */
#ifdef _MSC_VER
#define EXPAND(args) args
#define VA_ARGS_NARGS(...) \
	VA_ARGS_NARGS_ EXPAND((__VA_ARGS__, \
				   63,62,61,60,                   \
				   59,58,57,56,55,54,53,52,51,50, \
				   49,48,47,46,45,44,43,42,41,40, \
				   39,38,37,36,35,34,33,32,31,30, \
				   29,28,27,26,25,24,23,22,21,20, \
				   19,18,17,16,15,14,13,12,11,10, \
				   9, 8, 7, 6, 5, 4, 3, 2, 1, 0))
#else

#define VA_ARGS_NARGS(...) \
	VA_ARGS_NARGS_(__VA_ARGS__, \
				   63,62,61,60,                   \
				   59,58,57,56,55,54,53,52,51,50, \
				   49,48,47,46,45,44,43,42,41,40, \
				   39,38,37,36,35,34,33,32,31,30, \
				   29,28,27,26,25,24,23,22,21,20, \
				   19,18,17,16,15,14,13,12,11,10, \
				   9, 8, 7, 6, 5, 4, 3, 2, 1, 0)
#endif

#define VA_ARGS_NARGS_( \
	_01,_02,_03,_04,_05,_06,_07,_08,_09,_10, \
	_11,_12,_13,_14,_15,_16,_17,_18,_19,_20, \
	_21,_22,_23,_24,_25,_26,_27,_28,_29,_30, \
	_31,_32,_33,_34,_35,_36,_37,_38,_39,_40, \
	_41,_42,_43,_44,_45,_46,_47,_48,_49,_50, \
	_51,_52,_53,_54,_55,_56,_57,_58,_59,_60, \
	_61,_62,_63,  N, ...) \
	(N)

/*
 * 通用函数指针。可用于那些极少数需要将函数指针强制转换为看似不兼容的
 * 函数指针类型，同时避免 gcc 的 -Wcast-function-type 警告的场景。
 */
typedef void (*pg_funcptr_t) (void);

/*
 * 我们要求 C99，因此编译器应能理解柔性数组成员。但是，为了文档目的，
 * 我们仍然认为编写 "field[FLEXIBLE_ARRAY_MEMBER]" 而非 "field[]"
 * 是项目风格。在计算此类对象的大小时，请使用 "offsetof(struct s, f)"
 * 以保证可移植性。不要使用 "offsetof(struct s, f[0])"，因为这在
 * MSVC 和 C++ 编译器上不工作。
 */
#define FLEXIBLE_ARRAY_MEMBER	/* 空 */

/*
 * 编译器是否支持 #pragma GCC system_header？我们可选用它来避免那些
 * 我们无法修复的警告（例如在 perl 头文件中）。
 * 参见 https://gcc.gnu.org/onlinedocs/cpp/System-Headers.html
 *
 * 对于那些我们不希望显示编译器警告的头文件，可以有条件地使用
 * #pragma GCC system_header 来避免警告。显然，这只应用于我们无法控制的
 * 外部头文件。
 *
 * 对该 pragma 的支持在这里测试，而不是在 configure 阶段，因为 gcc
 * 也会对在 .c 文件中使用该 pragma 发出警告。让 autoconf 使用 .h 作为
 * 文件后缀出奇地困难。看起来 gcc 自 2000 年起就已实现该 pragma，
 * 所以这个测试应该足够。
 *
 *
 * 另一种方案是将有问题的头文件的 include 路径使用 -isystem 添加，
 * 但那是更粗暴的做法，而且更难搜索追溯。
 *
 * 更细粒度的替代方法是使用 #pragma GCC diagnostic push/ignored/pop，
 * 但 gcc 会对未知的被忽略警告发出警告，因此每个需要临时忽略的编译器警告
 * 都需要自己的 pg_config.h 符号和 #ifdef。
 */
#ifdef __GNUC__
#define HAVE_PRAGMA_GCC_SYSTEM_HEADER	1
#endif


/* ----------------------------------------------------------------
 *				第 2 节:	bool, true, false
 * ----------------------------------------------------------------
 */

/*
 * bool
 *		布尔值，true 或 false。
 *
 * PostgreSQL 目前无法处理大小不为 1 的 bool；代码中有静态断言来防止此情况。
 */

#include <stdbool.h>


/* ----------------------------------------------------------------
 *				第 3 节: 标准系统类型
 * ----------------------------------------------------------------
 */

/*
 * Pointer
 *		保存任意内存驻留对象地址的变量。
 *
 *		XXX 指针算术是用它完成的，因此在"真正的" ANSI 编译器下不能是 void *。
 */
typedef char *Pointer;

/* <stdint.h> 中类型的历史命名。 */
typedef int8_t int8;
typedef int16_t int16;
typedef int32_t int32;
typedef int64_t int64;
typedef uint8_t uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef uint64_t uint64;

/*
 * bitsN
 *		按位运算的单位，至少 N 位大小。
 */
typedef uint8 bits8;			/* >= 8 位 */
typedef uint16 bits16;			/* >= 16 位 */
typedef uint32 bits32;			/* >= 32 位 */

/*
 * 64 位整数
 */
#define INT64CONST(x)  INT64_C(x)
#define UINT64CONST(x) UINT64_C(x)

/* 用于 64 位整数的 snprintf 格式字符串 */
#define INT64_FORMAT "%" PRId64
#define UINT64_FORMAT "%" PRIu64

/*
 * 128 位有符号和无符号整数
 *		目前对这些类型的支持有限。
 *		例如，不支持 128 位字面量和 snprintf；但数学运算是支持的。
 *		此外，因为我们在选择 MAXIMUM_ALIGNOF 时排除了这样的类型，所以必须
 *		能使编译器以不超过 MAXALIGN 边界的方式分配它们。
 */
#if defined(PG_INT128_TYPE)
#if defined(pg_attribute_aligned) || ALIGNOF_PG_INT128_TYPE <= MAXIMUM_ALIGNOF
#define HAVE_INT128 1

typedef PG_INT128_TYPE int128
#if defined(pg_attribute_aligned)
			pg_attribute_aligned(MAXIMUM_ALIGNOF)
#endif
		   ;

typedef unsigned PG_INT128_TYPE uint128
#if defined(pg_attribute_aligned)
			pg_attribute_aligned(MAXIMUM_ALIGNOF)
#endif
		   ;

#endif
#endif

/* <stdint.h> 中限制值的历史命名。 */
#define PG_INT8_MIN		INT8_MIN
#define PG_INT8_MAX		INT8_MAX
#define PG_UINT8_MAX	UINT8_MAX
#define PG_INT16_MIN	INT16_MIN
#define PG_INT16_MAX	INT16_MAX
#define PG_UINT16_MAX	UINT16_MAX
#define PG_INT32_MIN	INT32_MIN
#define PG_INT32_MAX	INT32_MAX
#define PG_UINT32_MAX	UINT32_MAX
#define PG_INT64_MIN	INT64_MIN
#define PG_INT64_MAX	INT64_MAX
#define PG_UINT64_MAX	UINT64_MAX

/*
 * 我们现在始终使用 int64 时间戳，但保留此符号以便可能测试它的外部代码使用。
 */
#define HAVE_INT64_TIMESTAMP

/*
 * Size
 *		任意内存驻留对象的大小，由 sizeof 返回。
 */
typedef size_t Size;

/*
 * Index
 *		任意内存驻留数组的索引。
 *
 * 注意：
 *		索引是非负的。
 */
typedef unsigned int Index;

/*
 * Offset
 *		任意内存驻留数组的偏移量。
 *
 * 注意：
 *		这与 Index 不同，Index 始终非负，而 Offset 可以为负。
 */
typedef signed int Offset;

/*
 * 常见的 Postgres 数据类型名称（如系统表中使用的）
 */
typedef float float4;
typedef double float8;

#ifdef USE_FLOAT8_BYVAL
#define FLOAT8PASSBYVAL true
#else
#define FLOAT8PASSBYVAL false
#endif

/*
 * Oid, RegProcedure, TransactionId, SubTransactionId, MultiXactId,
 * CommandId
 */

/* typedef Oid 位于 postgres_ext.h 中 */

/*
 * regproc 是 include/catalog 头文件中使用的类型名，但 RegProcedure
 * 是 C 代码中的首选名称。
 */
typedef Oid regproc;
typedef regproc RegProcedure;

typedef uint32 TransactionId;

typedef uint32 LocalTransactionId;

typedef uint32 SubTransactionId;

#define InvalidSubTransactionId		((SubTransactionId) 0)
#define TopSubTransactionId			((SubTransactionId) 1)

/* MultiXactId 必须与 TransactionId 等价，以便放入 t_xmax */
typedef TransactionId MultiXactId;

typedef uint32 MultiXactOffset;

typedef uint32 CommandId;

#define FirstCommandId	((CommandId) 0)
#define InvalidCommandId	(~(CommandId)0)


/* ----------------
 *		变长数据类型都共享 'struct varlena' 头部。
 *
 * 注意：对于可 TOAST 的类型，这是一个过度简化，因为值可能被压缩或移到行外。
 * 然而，特定数据类型的例程大多只满足于处理已 de-TOAST 的值，当然，
 * 客户端例程永远不应该看到 TOAST 状态的值。但即使在 de-TOAST 的值中，
 * 也要注意不要直接接触 vl_len_，因为其表示形式已经不再方便直接使用。
 * 建议代码始终使用宏 VARDATA_ANY、VARSIZE_ANY、VARSIZE_ANY_EXHDR、
 * VARDATA、VARSIZE 以及 SET_VARSIZE，而不是直接引用结构体字段。
 * 参见 postgres.h 以了解 TOAST 形式的详细信息。
 * ----------------
 */
struct varlena
{
	char		vl_len_[4];		/* 不要直接触及此字段！ */
	char		vl_dat[FLEXIBLE_ARRAY_MEMBER];	/* 数据内容在此 */
};

#define VARHDRSZ		((int32) sizeof(int32))

/*
 * 这些广泛使用的数据类型只是一个 varlena 头部加上数据字节。
 * 没有终止 null 或类似的东西——数据长度始终为 VARSIZE_ANY_EXHDR(ptr)。
 */
typedef struct varlena bytea;
typedef struct varlena text;
typedef struct varlena BpChar;	/* 空白填充的 char，即 SQL char(n) */
typedef struct varlena VarChar; /* 变长 char，即 SQL varchar(n) */

/*
 * 专用的数组类型。这些类型在物理布局上与常规数组完全相同
 * （以便常规的数组下标代码能与它们一起工作）。它们作为独立类型存在
 * 主要是出于历史原因：它们具有非标准的 I/O 行为，我们不想改变，
 * 以免破坏那些查看系统表的应用程序。此外，oidvector 还有一个实现问题：
 * 它是 pg_proc 的主键的一部分，而我们不能用正常的 btree 数组支持例程，
 * 否则会产生循环依赖。
 */
typedef struct
{
	int32		vl_len_;		/* 这些字段必须匹配 ArrayType！ */
	int			ndim;			/* 对于 int2vector 始终为 1 */
	int32		dataoffset;		/* 对于 int2vector 始终为 0 */
	Oid			elemtype;
	int			dim1;
	int			lbound1;
	int16		values[FLEXIBLE_ARRAY_MEMBER];
} int2vector;

typedef struct
{
	int32		vl_len_;		/* 这些字段必须匹配 ArrayType！ */
	int			ndim;			/* 对于 oidvector 始终为 1 */
	int32		dataoffset;		/* 对于 oidvector 始终为 0 */
	Oid			elemtype;
	int			dim1;
	int			lbound1;
	Oid			values[FLEXIBLE_ARRAY_MEMBER];
} oidvector;

/*
 * Name 的表示：实际上只是一个 C 字符串，但用 null 填充到恰好 NAMEDATALEN 字节。
 * 使用结构体是历史原因。
 */
typedef struct nameData
{
	char		data[NAMEDATALEN];
} NameData;
typedef NameData *Name;

#define NameStr(name)	((name).data)


/* ----------------------------------------------------------------
 *				第 4 节: 系统类型的 IsValid 宏
 * ----------------------------------------------------------------
 */
/*
 * BoolIsValid
 *		当 bool 有效时为真。
 */
#define BoolIsValid(boolean)	((boolean) == false || (boolean) == true)

/*
 * PointerIsValid
 *		当指针有效时为真。
 */
#define PointerIsValid(pointer) ((const void*)(pointer) != NULL)

/*
 * PointerIsAligned
 *		当指针已针对指向给定类型做了适当对齐时为真。
 */
#define PointerIsAligned(pointer, type) \
		(((uintptr_t)(pointer) % (sizeof (type))) == 0)

#define OffsetToPointer(base, offset) \
		((void *)((char *) base + offset))

#define OidIsValid(objectId)  ((bool) ((objectId) != InvalidOid))

#define RegProcedureIsValid(p)	OidIsValid(p)


/* ----------------------------------------------------------------
 *				第 5 节: lengthof, 对齐
 * ----------------------------------------------------------------
 */
/*
 * lengthof
 *		数组中元素的个数。
 */
#define lengthof(array) (sizeof (array) / sizeof ((array)[0]))

/* ----------------
 * 对齐宏：为给定类型适当对齐长度或地址。
 * fooALIGN() 宏向上舍入到所需对齐的倍数，而 fooALIGN_DOWN() 宏向下舍入。
 * 后者对于诸如"一页中能容纳多少个 X 大小的结构体？"之类的问题更有用。
 *
 * 注意：如果 ALIGNVAL 不是 2 的幂，TYPEALIGN[_DOWN] 将无法工作。
 * 不过，这种情况在实际中几乎不可能遇到。
 *
 * 注意：MAXIMUM_ALIGNOF，以及由此派生的 MAXALIGN()，故意排除了编译器
 * 可能有的任何大于 8 字节的类型。
 * ----------------
 */

#define TYPEALIGN(ALIGNVAL,LEN)  \
	(((uintptr_t) (LEN) + ((ALIGNVAL) - 1)) & ~((uintptr_t) ((ALIGNVAL) - 1)))

#define SHORTALIGN(LEN)			TYPEALIGN(ALIGNOF_SHORT, (LEN))
#define INTALIGN(LEN)			TYPEALIGN(ALIGNOF_INT, (LEN))
#define LONGALIGN(LEN)			TYPEALIGN(ALIGNOF_LONG, (LEN))
#define DOUBLEALIGN(LEN)		TYPEALIGN(ALIGNOF_DOUBLE, (LEN))
#define MAXALIGN(LEN)			TYPEALIGN(MAXIMUM_ALIGNOF, (LEN))
/* MAXALIGN 仅涵盖内置类型，不涵盖缓冲区 */
#define BUFFERALIGN(LEN)		TYPEALIGN(ALIGNOF_BUFFER, (LEN))
#define CACHELINEALIGN(LEN)		TYPEALIGN(PG_CACHE_LINE_SIZE, (LEN))

#define TYPEALIGN_DOWN(ALIGNVAL,LEN)  \
	(((uintptr_t) (LEN)) & ~((uintptr_t) ((ALIGNVAL) - 1)))

#define SHORTALIGN_DOWN(LEN)	TYPEALIGN_DOWN(ALIGNOF_SHORT, (LEN))
#define INTALIGN_DOWN(LEN)		TYPEALIGN_DOWN(ALIGNOF_INT, (LEN))
#define LONGALIGN_DOWN(LEN)		TYPEALIGN_DOWN(ALIGNOF_LONG, (LEN))
#define DOUBLEALIGN_DOWN(LEN)	TYPEALIGN_DOWN(ALIGNOF_DOUBLE, (LEN))
#define MAXALIGN_DOWN(LEN)		TYPEALIGN_DOWN(MAXIMUM_ALIGNOF, (LEN))
#define BUFFERALIGN_DOWN(LEN)	TYPEALIGN_DOWN(ALIGNOF_BUFFER, (LEN))

/*
 * 上述宏不能用于比 uintptr_t 更宽的类型，例如 32 位平台上的 uint64。
 * 这对通常情况下对齐指针或长度没问题，但在需要对（可能）更宽的内容做对齐时，
 * 请使用 TYPEALIGN64。
 */
#define TYPEALIGN64(ALIGNVAL,LEN)  \
	(((uint64) (LEN) + ((ALIGNVAL) - 1)) & ~((uint64) ((ALIGNVAL) - 1)))

/* 我们目前不需要其他 ALIGN 宏的更宽版本 */
#define MAXALIGN64(LEN)			TYPEALIGN64(MAXIMUM_ALIGNOF, (LEN))


/* ----------------------------------------------------------------
 *				第 6 节: 断言
 * ----------------------------------------------------------------
 */

/*
 * USE_ASSERT_CHECKING，如果定义，则打开所有断言。
 * - plai  9/5/90
 *
 * 它 _不_ 应该在发布版本或基准测试副本中定义。
 */

/*
 * Assert() 可同时用于前端和后端代码。在前端代码中，如果标准 assert 可用，
 * 则直接调用它。如果没有配置使用断言，则什么都不做。
 */
#ifndef USE_ASSERT_CHECKING

#define Assert(condition)	((void)true)
#define AssertMacro(condition)	((void)true)

#elif defined(FRONTEND)

#include <assert.h>
#define Assert(p) assert(p)
#define AssertMacro(p)	((void) assert(p))

#else							/* USE_ASSERT_CHECKING && !FRONTEND */

/*
 * Assert
 *		如果给定条件为 false，则产生一个致命异常。
 */
#define Assert(condition) \
	do { \
		if (!(condition)) \
			ExceptionalCondition(#condition, __FILE__, __LINE__); \
	} while (0)

/*
 * AssertMacro 与 Assert 相同，但适用于类似表达式的宏，例如：
 *
 *		#define foo(x) (AssertMacro(x != 0), bar(x))
 */
#define AssertMacro(condition) \
	((void) ((condition) || \
			 (ExceptionalCondition(#condition, __FILE__, __LINE__), 0)))

#endif							/* USE_ASSERT_CHECKING && !FRONTEND */

/*
 * 检查 `ptr' 是否为 `bndr' 对齐。
 */
#define AssertPointerAlignment(ptr, bndr) \
	Assert(TYPEALIGN(bndr, (uintptr_t)(ptr)) == (uintptr_t)(ptr))

/*
 * 无论是否定义了 USE_ASSERT_CHECKING，ExceptionalCondition 都会编译进
 * 后端，以支持用该 #define 编译的扩展与未用该 #define 编译的后端一起使用。
 * 因此，只要 !FRONTEND，我们就应声明它。
 */
#ifndef FRONTEND
pg_noreturn extern void ExceptionalCondition(const char *conditionName,
											 const char *fileName, int lineNumber);
#endif

/*
 * 支持编译时断言检查的宏。
 *
 * 如果 "condition"（一个编译时常量表达式）求值为 false，则使用
 * "errmessage"（字符串字面量）抛出一个编译错误。
 *
 * C11 有 _Static_assert()，且大多数 C99 编译器已经支持它。为了可移植性，
 * 我们将其包装为 StaticAssertDecl()。_Static_assert() 是一个"声明"，
 * 因此必须放在例如变量声明有效的位置。只要我们以
 * -Wno-declaration-after-statement 编译，这也意味着不能在函数中的语句之后
 * 放置它。StaticAssertStmt() 和 StaticAssertExpr() 宏分别使其安全地用作
 * 语句或表达式。
 *
 * 对于没有 _Static_assert() 的编译器，我们回退到一个权宜方案：假定编译器
 * 会对结构体位域的负宽度发出抱怨。这不会包含有用的错误消息，但总比完全
 * 得不到错误要好。
 */
#ifndef __cplusplus
#ifdef HAVE__STATIC_ASSERT
#define StaticAssertDecl(condition, errmessage) \
	_Static_assert(condition, errmessage)
#define StaticAssertStmt(condition, errmessage) \
	do { _Static_assert(condition, errmessage); } while(0)
#define StaticAssertExpr(condition, errmessage) \
	((void) ({ StaticAssertStmt(condition, errmessage); true; }))
#else							/* !HAVE__STATIC_ASSERT */
#define StaticAssertDecl(condition, errmessage) \
	extern void static_assert_func(int static_assert_failure[(condition) ? 1 : -1])
#define StaticAssertStmt(condition, errmessage) \
	((void) sizeof(struct { int static_assert_failure : (condition) ? 1 : -1; }))
#define StaticAssertExpr(condition, errmessage) \
	StaticAssertStmt(condition, errmessage)
#endif							/* HAVE__STATIC_ASSERT */
#else							/* C++ */
#if defined(__cpp_static_assert) && __cpp_static_assert >= 200410
#define StaticAssertDecl(condition, errmessage) \
	static_assert(condition, errmessage)
#define StaticAssertStmt(condition, errmessage) \
	static_assert(condition, errmessage)
#define StaticAssertExpr(condition, errmessage) \
	({ static_assert(condition, errmessage); })
#else							/* !__cpp_static_assert */
#define StaticAssertDecl(condition, errmessage) \
	extern void static_assert_func(int static_assert_failure[(condition) ? 1 : -1])
#define StaticAssertStmt(condition, errmessage) \
	do { struct static_assert_struct { int static_assert_failure : (condition) ? 1 : -1; }; } while(0)
#define StaticAssertExpr(condition, errmessage) \
	((void) ({ StaticAssertStmt(condition, errmessage); }))
#endif							/* __cpp_static_assert */
#endif							/* C++ */


/*
 * 编译时检查一个变量（或表达式）具有指定的类型。
 *
 * AssertVariableIsOfType() 可作为语句使用。
 * AssertVariableIsOfTypeMacro() 用于宏中，例如：
 *		#define foo(x) (AssertVariableIsOfTypeMacro(x, int), bar(x))
 *
 * 如果没有 __builtin_types_compatible_p，我们仍然可以断言类型具有相同的大小。
 * 这远非理想（尤其在 32 位平台上），但至少提供了一些覆盖。
 */
#ifdef HAVE__BUILTIN_TYPES_COMPATIBLE_P
#define AssertVariableIsOfType(varname, typename) \
	StaticAssertStmt(__builtin_types_compatible_p(__typeof__(varname), typename), \
	CppAsString(varname) " does not have type " CppAsString(typename))
#define AssertVariableIsOfTypeMacro(varname, typename) \
	(StaticAssertExpr(__builtin_types_compatible_p(__typeof__(varname), typename), \
	 CppAsString(varname) " does not have type " CppAsString(typename)))
#else							/* !HAVE__BUILTIN_TYPES_COMPATIBLE_P */
#define AssertVariableIsOfType(varname, typename) \
	StaticAssertStmt(sizeof(varname) == sizeof(typename), \
	CppAsString(varname) " does not have type " CppAsString(typename))
#define AssertVariableIsOfTypeMacro(varname, typename) \
	(StaticAssertExpr(sizeof(varname) == sizeof(typename), \
	 CppAsString(varname) " does not have type " CppAsString(typename)))
#endif							/* HAVE__BUILTIN_TYPES_COMPATIBLE_P */


/* ----------------------------------------------------------------
 *				第 7 节: 广泛适用的宏
 * ----------------------------------------------------------------
 */
/*
 * Max
 *		返回两个数中的最大值。
 */
#define Max(x, y)		((x) > (y) ? (x) : (y))

/*
 * Min
 *		返回两个数中的最小值。
 */
#define Min(x, y)		((x) < (y) ? (x) : (y))


/* 获取非 long 对齐地址中设置的位的掩码 */
#define LONG_ALIGN_MASK (sizeof(long) - 1)

/*
 * MemSet
 *	与标准库函数 memset() 完全相同，但对于清零小的字对齐结构体
 *	（如解析树节点）要快得多。这必须是宏，因为主要目的是避免函数调用
 *	开销。然而，我们还发现在某些平台上，即使那些平台有汇编版的
 *	memset() 函数，这个循环也比原生 libc memset() 更快。
 *	需要做更多研究，也许可以通过 configure 中的 MEMSET_LOOP_LIMIT 测试。
 */
#define MemSet(start, val, len) \
	do \
	{ \
		/* 必须是 void*，因为尚不知道是否整数对齐 */ \
		void   *_vstart = (void *) (start); \
		int		_val = (val); \
		Size	_len = (len); \
\
		if ((((uintptr_t) _vstart) & LONG_ALIGN_MASK) == 0 && \
			(_len & LONG_ALIGN_MASK) == 0 && \
			_val == 0 && \
			_len <= MEMSET_LOOP_LIMIT && \
			/* \
			 *	如果 MEMSET_LOOP_LIMIT == 0，编译器应发现整个 \
			 *	"if" 在编译时为 false。 \
			 */ \
			MEMSET_LOOP_LIMIT != 0) \
		{ \
			long *_start = (long *) _vstart; \
			long *_stop = (long *) ((char *) _start + _len); \
			while (_start < _stop) \
				*_start++ = 0; \
		} \
		else \
			memset(_vstart, _val, _len); \
	} while (0)

/*
 * MemSetAligned 与 MemSet 相同，只是省略了检查 "start" 是否字对齐的测试。
 * 如果调用者先验地知道指针已适当对齐（通常是因为刚从 palloc() 获得，而
 * palloc() 总是返回最大对齐的指针），则可以使用此宏。
 */
#define MemSetAligned(start, val, len) \
	do \
	{ \
		long   *_start = (long *) (start); \
		int		_val = (val); \
		Size	_len = (len); \
\
		if ((_len & LONG_ALIGN_MASK) == 0 && \
			_val == 0 && \
			_len <= MEMSET_LOOP_LIMIT && \
			MEMSET_LOOP_LIMIT != 0) \
		{ \
			long *_stop = (long *) ((char *) _start + _len); \
			while (_start < _stop) \
				*_start++ = 0; \
		} \
		else \
			memset(_start, _val, _len); \
	} while (0)


/*
 * 在将浮点值转换为整数之前进行范围检查的宏。
 * 我们必须注意，这里的边界值要在浮点域中精确表示。PG_INTnn_MIN 是 2 的
 * 精确幂，因此可以精确表示；但 PG_INTnn_MAX 不是，可能会被舍入，所以要
 * 避免使用它。
 * 输入必须事先舍入到整数（通常用 rint()），否则可能会对接近极限的值得出
 * 错误结论。
 * 这些宏对 Inf 能够正确处理，但对 NaN 不一定，因此如果可能存在 NaN，
 * 请先检查 isnan(num)。
 */
#define FLOAT4_FITS_IN_INT16(num) \
	((num) >= (float4) PG_INT16_MIN && (num) < -((float4) PG_INT16_MIN))
#define FLOAT4_FITS_IN_INT32(num) \
	((num) >= (float4) PG_INT32_MIN && (num) < -((float4) PG_INT32_MIN))
#define FLOAT4_FITS_IN_INT64(num) \
	((num) >= (float4) PG_INT64_MIN && (num) < -((float4) PG_INT64_MIN))
#define FLOAT8_FITS_IN_INT16(num) \
	((num) >= (float8) PG_INT16_MIN && (num) < -((float8) PG_INT16_MIN))
#define FLOAT8_FITS_IN_INT32(num) \
	((num) >= (float8) PG_INT32_MIN && (num) < -((float8) PG_INT32_MIN))
#define FLOAT8_FITS_IN_INT64(num) \
	((num) >= (float8) PG_INT64_MIN && (num) < -((float8) PG_INT64_MIN))


/* ----------------------------------------------------------------
 *				第 8 节: 杂项
 * ----------------------------------------------------------------
 */

/*
 * 反转 qsort 风格比较结果的符号，即交换负值和正值，同时注意避免对
 * INT_MIN 得出错误答案。参数应是一个整型变量。
 */
#define INVERT_COMPARE_RESULT(var) \
	((var) = ((var) < 0) ? 1 : -(var))

/*
 * 使用此宏（而非 "char buf[BLCKSZ]"）来声明持有页缓冲区的字段或局部变量，
 * 前提是该页可能作为页来访问。否则变量可能对齐不足，在对齐敏感的硬件上
 * 造成问题。我们在 union 中同时包含了 "double" 和 "int64"，以确保编译器知道
 * 该值必须 MAXALIGN 对齐（参见 configure 中 MAXIMUM_ALIGNOF 的计算）。
 */
typedef union PGAlignedBlock
{
	char		data[BLCKSZ];
	double		force_align_d;
	int64		force_align_i64;
} PGAlignedBlock;

/*
 * 使用此宏声明持有页缓冲区的字段或局部变量，前提是该页可能作为页来访问，
 * 或者要传递给 SMgr I/O 函数。如果使用 MemoryContext API 分配，应当使用
 * 对齐分配函数，并以 PG_IO_ALIGN_SIZE 为大小。这种对齐在一般情况下可能使
 * I/O 更高效，但在某些平台上使用 direct I/O 时可能是强制要求的。
 */
typedef union PGIOAlignedBlock
{
#ifdef pg_attribute_aligned
	pg_attribute_aligned(PG_IO_ALIGN_SIZE)
#endif
	char		data[BLCKSZ];
	double		force_align_d;
	int64		force_align_i64;
} PGIOAlignedBlock;

/* 同理，用于 XLOG_BLCKSZ 大小的缓冲区 */
typedef union PGAlignedXLogBlock
{
#ifdef pg_attribute_aligned
	pg_attribute_aligned(PG_IO_ALIGN_SIZE)
#endif
	char		data[XLOG_BLCKSZ];
	double		force_align_d;
	int64		force_align_i64;
} PGAlignedXLogBlock;

/* char 的最高位 */
#define HIGHBIT					(0x80)
#define IS_HIGHBIT_SET(ch)		((unsigned char)(ch) & HIGHBIT)

/*
 * 字符串转义的辅助宏。escape_backslash 在生成非标准合规字符串时应为 true。
 * 在字符串前加上 ESCAPE_STRING_SYNTAX 可以保证它是非标准合规的。
 * 注意 "ch" 参数会被多次求值！
 */
#define SQL_STR_DOUBLE(ch, escape_backslash)	\
	((ch) == '\'' || ((ch) == '\\' && (escape_backslash)))

#define ESCAPE_STRING_SYNTAX	'E'


#define STATUS_OK				(0)
#define STATUS_ERROR			(-1)
#define STATUS_EOF				(-2)

/*
 * gettext 支持
 */

#ifndef ENABLE_NLS
/* 我们本来会从 <libintl.h> 获取的内容 */
#define gettext(x) (x)
#define dgettext(d,x) (x)
#define ngettext(s,p,n) ((n) == 1 ? (s) : (p))
#define dngettext(d,s,p,n) ((n) == 1 ? (s) : (p))
#endif

#define _(x) gettext(x)

/*
 *	使用此宏来标记字符串常量需要在将来的某个时刻（而非立即）进行翻译。
 *	适用于需要同时访问原始字符串和翻译后字符串的场景，以及那些无法立即
 *	翻译的场景，例如初始化全局变量时。
 *
 *	https://www.gnu.org/software/gettext/manual/html_node/Special-cases.html
 */
#define gettext_noop(x) (x)

/*
 * 为了更好地支持 PostgreSQL 主版本的并行安装，以及主程序库 soname 版本的
 * 并行安装，我们通过在 gettext 域名后添加这些版本号来修改域名。
 * 编码规则是：无论何处，只要域名作为字面量提及，就必须包裹在
 * PG_TEXTDOMAIN() 中。下面的宏不适用于非字面量；但这在某种程度上是有意为之，
 * 因为这样可以避免在值传递过程中处理"前缀"和"后缀"的多种状态。
 *
 * 请确保这与 nls-global.mk 中的安装规则匹配。
 */
#ifdef SO_MAJOR_VERSION
#define PG_TEXTDOMAIN(domain) (domain CppAsString2(SO_MAJOR_VERSION) "-" PG_MAJORVERSION)
#else
#define PG_TEXTDOMAIN(domain) (domain "-" PG_MAJORVERSION)
#endif

/*
 * 允许从表达式中去除 const 和 volatile 修饰，但不允许改变底层类型的宏。
 * 后者的强制执行目前只对 gcc 类编译器有效。
 *
 * 请注意：如果结果将会被修改，去除 const 修饰是*不安全的*（这将是未定义
 * 行为）。这样做仍然可能导致编译器错误优化或运行时崩溃（修改只读内存）。
 * 仅当结果不会被修改，但 API 设计或语言限制阻止你声明这一点时
 * （例如，因为某个函数同时返回 const 和非 const 变量），才安全使用。
 *
 * 注意，这仅在函数作用域内有效，对全局变量无效（改进这一点虽然不平凡，
 * 但会很不错）。
 */
#if defined(__cplusplus)
#define unconstify(underlying_type, expr) const_cast<underlying_type>(expr)
#define unvolatize(underlying_type, expr) const_cast<underlying_type>(expr)
#elif defined(HAVE__BUILTIN_TYPES_COMPATIBLE_P)
#define unconstify(underlying_type, expr) \
	(StaticAssertExpr(__builtin_types_compatible_p(__typeof(expr), const underlying_type), \
					  "wrong cast"), \
	 (underlying_type) (expr))
#define unvolatize(underlying_type, expr) \
	(StaticAssertExpr(__builtin_types_compatible_p(__typeof(expr), volatile underlying_type), \
					  "wrong cast"), \
	 (underlying_type) (expr))
#else
#define unconstify(underlying_type, expr) \
	((underlying_type) (expr))
#define unvolatize(underlying_type, expr) \
	((underlying_type) (expr))
#endif

/* ----------------------------------------------------------------
 *				第 9 节: 系统相关 hack
 *
 *		此节应仅限于绝对必须包含在每个源文件中的内容。
 *		特定平台的端口头文件通常是放置此类内容的更好位置。
 * ----------------------------------------------------------------
 */

/*
 *	注意：这也用于打开文本文件。
 *	WIN32 将文本模式打开的文件中的 Control-Z 视为 EOF。
 *	因此，我们在 Win32 上以二进制模式打开文件，以便读取字面量 control-Z。
 *	另一个影响是我们会看到 CRLF，但这没有问题，因为我们已经能干净地处理它们。
 */
#if defined(WIN32) || defined(__CYGWIN__)
#define PG_BINARY	O_BINARY
#define PG_BINARY_A "ab"
#define PG_BINARY_R "rb"
#define PG_BINARY_W "wb"
#else
#define PG_BINARY	0
#define PG_BINARY_A "a"
#define PG_BINARY_R "r"
#define PG_BINARY_W "w"
#endif

/*
 * 为特定机器的标准 C 库中不存在的例程提供原型。
 */

#if !HAVE_DECL_FDATASYNC
extern int	fdatasync(int fildes);
#endif

/*
 * 将字符串精确转换为 64 位整数的薄包装，匹配我们对 int64 的定义。
 * （关于命名，可比较 POSIX 的 strtoimax()/strtoumax()，它们返回
 * intmax_t/uintmax_t。）
 */
#if SIZEOF_LONG == 8
#define strtoi64(str, endptr, base) ((int64) strtol(str, endptr, base))
#define strtou64(str, endptr, base) ((uint64) strtoul(str, endptr, base))
#elif SIZEOF_LONG_LONG == 8
#define strtoi64(str, endptr, base) ((int64) strtoll(str, endptr, base))
#define strtou64(str, endptr, base) ((uint64) strtoull(str, endptr, base))
#else
#error "cannot find integer type of the same size as int64_t"
#endif

/*
 * 类似地，匹配 int64 的 labs()/llabs() 包装。
 */
#if SIZEOF_LONG == 8
#define i64abs(i) ((int64) labs(i))
#elif SIZEOF_LONG_LONG == 8
#define i64abs(i) ((int64) llabs(i))
#else
#error "cannot find integer type of the same size as int64_t"
#endif

/*
 * 使用 "extern PGDLLIMPORT ..." 来声明在核心后端中定义、需要被可加载模块
 * 访问的变量。在大多数平台上不需要特殊标记。
 */
#ifndef PGDLLIMPORT
#define PGDLLIMPORT
#endif

/*
 * 使用 "extern PGDLLEXPORT ..." 来声明在可加载模块中定义、需要被核心后端
 * 或其他可加载模块调用的函数。
 * 如果编译器知道 __attribute__((visibility("*")))，我们就使用它，
 * 除非已有平台相关的定义。否则，不需要特殊标记。
 */
#ifndef PGDLLEXPORT
#ifdef HAVE_VISIBILITY_ATTRIBUTE
#define PGDLLEXPORT __attribute__((visibility("default")))
#else
#define PGDLLEXPORT
#endif
#endif

/*
 * 以下内容用作信号处理函数的参数列表。任何接收非 int 参数的端口应在其
 * pg_config_os.h 文件中覆盖此定义。注意需要变量名，因为它同时用于原型和
 * 定义中。还要注意这个名称很长。我们期望这不会与其他名称冲突而导致
 * 编译器警告。
 */

#ifndef SIGNAL_ARGS
#define SIGNAL_ARGS  int postgres_signal_arg
#endif

/*
 * 当没有 sigsetjmp 时，其功能由普通的 setjmp 提供。
 * 我们现在仅在 Windows 上支持这种情况。然而，MinGW-64 的 setjmp 支持
 * 似乎存在一些长期存在的问题，因此在该工具链上，我们取巧使用 gcc 的内置函数。
 */
#ifdef WIN32
#ifdef __MINGW64__
typedef intptr_t sigjmp_buf[5];
#define sigsetjmp(x,y) __builtin_setjmp(x)
#define siglongjmp __builtin_longjmp
#else							/* !__MINGW64__ */
#define sigjmp_buf jmp_buf
#define sigsetjmp(x,y) setjmp(x)
#define siglongjmp longjmp
#endif							/* __MINGW64__ */
#endif							/* WIN32 */

/* /port 兼容函数 */
#include "port.h"

/* IWYU pragma: end_exports */

#endif							/* C_H */
