/*-------------------------------------------------------------------------
 *
 * relpath.h
 *		GetRelationPath() 及相关函数的声明
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/common/relpath.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RELPATH_H
#define RELPATH_H

/*
 *	此处需要包含；注意如果符号未定义，CppAsString2() 不会报错。
 */
#include "catalog/catversion.h"

/*
 * RelFileNumber 数据类型用于标识特定的关系文件名。
 */
typedef Oid RelFileNumber;
#define InvalidRelFileNumber		((RelFileNumber) InvalidOid)
#define RelFileNumberIsValid(relnumber) \
				((bool) ((relnumber) != InvalidRelFileNumber))

/*
 * 主版本相关的表空间子目录名称
 */
#define TABLESPACE_VERSION_DIRECTORY	"PG_" PG_MAJORVERSION "_" \
									CppAsString2(CATALOG_VERSION_NO)

/*
 * 表空间路径（相对于安装目录的 $PGDATA）。
 *
 * 这些值不应被修改，因为许多工具都依赖于它。
 */
#define PG_TBLSPC_DIR "pg_tblspc"
#define PG_TBLSPC_DIR_SLASH "pg_tblspc/"	/* 字符串比较时需要用到 */

/* 关系路径中一个 OID 所占用的字符数 */
#define OIDCHARS		10		/* %u 所能打印的最大字符数 */

/*
 * fork 名称相关定义。
 *
 * 一个关系的物理存储由一个或多个 fork 组成。
 * 主 fork（main fork）总是会被创建，除此之外还可以有
 * 额外的 fork 用于存储各种元数据。当我们需要在关系中引用
 * 某个特定的 fork 时，使用 ForkNumber 来表示。
 */
typedef enum ForkNumber
{
	InvalidForkNumber = -1,
	MAIN_FORKNUM = 0,
	FSM_FORKNUM,
	VISIBILITYMAP_FORKNUM,
	INIT_FORKNUM,

	/*
	 * 注意：如果新增一个 fork，需要修改下方的 MAX_FORKNUM，
	 * 可能还要修改 FORKNAMECHARS，并更新 src/common/relpath.c
	 * 中的 forkNames 数组
	 */
} ForkNumber;

#define MAX_FORKNUM		INIT_FORKNUM

#define FORKNAMECHARS	4		/* fork 名称的最大字符数 */

extern PGDLLIMPORT const char *const forkNames[];

extern ForkNumber forkname_to_number(const char *forkName);
extern int	forkname_chars(const char *str, ForkNumber *fork);


/*
 * 遗憾的是，无法简单地从 MAX_BACKENDS 推导出 PROCNUMBER_CHARS。
 * MAX_BACKENDS 为 2^18-1。该值在 test_relpath() 中进行了交叉校验。
 */
#define PROCNUMBER_CHARS	6

/*
 * 关系路径的最大可能长度来源于以下格式：
 * sprintf(rp.path, "%s/%u/%s/%u/t%d_%u",
 *         PG_TBLSPC_DIR, spcOid,
 *         TABLESPACE_VERSION_DIRECTORY,
 *         dbOid, procNumber, relNumber);
 *
 * 注意这里*不*包含结尾的空字节，以便于与其他长度进行拼接。
 */
#define REL_PATH_STR_MAXLEN \
	( \
		sizeof(PG_TBLSPC_DIR) - 1 \
		+ sizeof((char)'/') \
		+ OIDCHARS /* spcOid */ \
		+ sizeof((char)'/') \
		+ sizeof(TABLESPACE_VERSION_DIRECTORY) - 1 \
		+ sizeof((char)'/') \
		+ OIDCHARS /* dbOid */ \
		+ sizeof((char)'/') \
		+ sizeof((char)'t') /* 临时表标识 */ \
		+ PROCNUMBER_CHARS /* procNumber */ \
		+ sizeof((char)'_') \
		+ OIDCHARS /* relNumber */ \
		+ sizeof((char)'_') \
		+ FORKNAMECHARS /* forkNames[forkNumber] */ \
	)

/*
 * 表示关系路径所需的精确长度的字符串。我们返回这个结构体，
 * 而不是 char[REL_PATH_STR_MAXLEN + 1]，是因为指针很容易退化为
 * 普通的 char *，从而可能使编译器无法检测出对 GetRelationPath()
 * 返回值的栈上引用是否合法。
 */
typedef struct RelPathStr
{
	char		str[REL_PATH_STR_MAXLEN + 1];
} RelPathStr;


/*
 * 计算关系文件系统路径名相关定义。
 */
extern char *GetDatabasePath(Oid dbOid, Oid spcOid);

extern RelPathStr GetRelationPath(Oid dbOid, Oid spcOid, RelFileNumber relNumber,
								  int procNumber, ForkNumber forkNumber);

/*
 * GetRelationPath 的包装宏。注意 RelFileLocator 或
 * RelFileLocatorBackend 参数会被多次求值，使用时需谨慎！
 */

/* 第一个参数为 RelFileLocator */
#define relpathbackend(rlocator, backend, forknum) \
	GetRelationPath((rlocator).dbOid, (rlocator).spcOid, (rlocator).relNumber, \
					backend, forknum)

/* 第一个参数为 RelFileLocator */
#define relpathperm(rlocator, forknum) \
	relpathbackend(rlocator, INVALID_PROC_NUMBER, forknum)

/* 第一个参数为 RelFileLocatorBackend */
#define relpath(rlocator, forknum) \
	relpathbackend((rlocator).locator, (rlocator).backend, forknum)

#endif							/* RELPATH_H */
