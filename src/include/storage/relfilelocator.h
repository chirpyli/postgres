/*-------------------------------------------------------------------------
 *
 * relfilelocator.h
 *	  关系的物理访问信息。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/relfilelocator.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RELFILELOCATOR_H
#define RELFILELOCATOR_H

#include "common/relpath.h"
#include "storage/procnumber.h"

/*
 * RelFileLocator 必须提供我们物理访问一个关系所需的全部信息，
 * 但后端进程号（proc number）除外，后者可以另行提供。不过请注意，
 * 一个“物理”关系在文件系统上由多个文件组成，因为每个 fork 都作为
 * 一个单独的文件存储，且每个 fork 还可被划分为多个段。参见 md.c。
 *
 * spcOid 标识该关系所在的表空间，对应 pg_tablespace.oid。
 *
 * dbOid 标识该关系所属的数据库。对于“共享”关系
 * （即集群中所有数据库共有的那些关系），它为零。
 * 非零的 dbOid 值对应 pg_database.oid。
 *
 * relNumber 标识具体的关系。relNumber 对应 pg_class.relfilenode
 * （而非 pg_class.oid，因为我们需要在某些情况下为关系分配新的
 * 物理文件）。注意，relNumber 仅在特定表空间内的某个数据库中
 * 唯一。
 *
 * 注意：当且仅当 dbOid 为零时，spcOid 必须为
 * GLOBALTABLESPACE_OID。我们只支持将共享关系放在 “global”
 * 表空间中。
 *
 * 注意：在 pg_class 中，我们允许 reltablespace == 0 表示该关系
 * 存储在其数据库的 “默认” 表空间中（由 pg_database.dattablespace
 * 标识）。但这种简写形式在 RelFileLocator 结构体中是不被允许的——
 * 设置 spcOid 时必须提供真实的表空间 ID。
 *
 * 注意：在 pg_class 中，relfilenode 可以为零，表示该关系是一个
 * “被映射的”关系，其当前真实文件节点号可从 relmapper.c 获取。
 * 同样，这种情况在 RelFileLocator 中也是不允许的。
 *
 * 注意：有多处将 RelFileLocator 用作哈希表的键。因此，该结构体中
 * *不得* 存在任何未使用的填充字节。只要所有字段都是 Oid 类型，
 * 这应当是安全的。
 */
typedef struct RelFileLocator
{
	Oid			spcOid;			/* tablespace */
	Oid			dbOid;			/* database */
	RelFileNumber relNumber;	/* relation */
} RelFileLocator;

/*
 * 为 relfilelocator 补充后端的进程号，就提供了定位物理存储所需的
 * 全部信息。'backend' 对于普通关系（可被多个后端访问的那些）为
 * INVALID_PROC_NUMBER，而对于后端本地关系，则是其拥有者后端的
 * 进程号。后端本地关系总是临时性的，在数据库崩溃时会被清除；
 * 它们永远不会被 WAL 记录或 fsync。
 */
typedef struct RelFileLocatorBackend
{
	RelFileLocator locator;
	ProcNumber	backend;
} RelFileLocatorBackend;

#define RelFileLocatorBackendIsTemp(rlocator) \
	((rlocator).backend != INVALID_PROC_NUMBER)

/*
 * 注意：RelFileLocatorEquals 与 RelFileLocatorBackendEquals 会先比较
 * relNumber，因为在两个不相等的 RelFileLocator 中，relNumber 最有可能
 * 是不同的。若其他字段都相等，再比较 spcOid 或许是多余的，但为了
 * 保险仍照做。RelFileLocatorBackendEquals 中对后端号的比较亦然。
 */
#define RelFileLocatorEquals(locator1, locator2) \
	((locator1).relNumber == (locator2).relNumber && \
	 (locator1).dbOid == (locator2).dbOid && \
	 (locator1).spcOid == (locator2).spcOid)

#define RelFileLocatorBackendEquals(locator1, locator2) \
	((locator1).locator.relNumber == (locator2).locator.relNumber && \
	 (locator1).locator.dbOid == (locator2).locator.dbOid && \
	 (locator1).backend == (locator2).backend && \
	 (locator1).locator.spcOid == (locator2).locator.spcOid)

#endif							/* RELFILELOCATOR_H */
