/*-------------------------------------------------------------------------
 *
 * relation.c
 *	  通用的关系相关例程。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/common/relation.c
 *
 * 注意
 *	  本文件包含实现关系（表、索引等）访问的 relation_ 例程。
 *	  针对关系子类型的特定支持应放在它们各自的文件中，而不是这里。
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/relation.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "pgstat.h"
#include "storage/lmgr.h"
#include "utils/inval.h"
#include "utils/syscache.h"


/* ----------------
 *		relation_open - open any relation by relation OID
 *
 *		If lockmode is not "NoLock", the specified kind of lock is
 *		obtained on the relation.  (Generally, NoLock should only be
 *		used if the caller knows it has some appropriate lock on the
 *		relation already.)
 *
 *		An error is raised if the relation does not exist.
 *
 *		NB: a "relation" is anything with a pg_class entry.  The caller is
 *		expected to check whether the relkind is something it can handle.
 * ----------------
 */
Relation
relation_open(Oid relationId, LOCKMODE lockmode)
{
	Relation	r;

	Assert(lockmode >= NoLock && lockmode < MAX_LOCKMODES);

	/* 在尝试打开 relcache 条目之前先获取锁 */
	if (lockmode != NoLock)
		LockRelationOid(relationId, lockmode);

	/* relcache 完成真正的工作…… */
	r = RelationIdGetRelation(relationId);

	if (!RelationIsValid(r))
		elog(ERROR, "could not open relation with OID %u", relationId);

	/*
	 * 如果我们自己没有获取锁，则断言调用方持有该锁，
	 * 但在引导（bootstrap）模式下没有使用任何锁时除外。
	 */
	Assert(lockmode != NoLock ||
		   IsBootstrapProcessingMode() ||
		   CheckRelationLockedByMe(r, AccessShareLock, true));

	/* 记录我们访问了一个临时关系 */
	if (RelationUsesLocalBuffers(r))
		MyXactFlags |= XACT_FLAGS_ACCESSEDTEMPNAMESPACE;

	pgstat_init_relation(r);

	return r;
}

/* ----------------
 *		try_relation_open - open any relation by relation OID
 *
 *		Same as relation_open, except return NULL instead of failing
 *		if the relation does not exist.
 * ----------------
 */
Relation
try_relation_open(Oid relationId, LOCKMODE lockmode)
{
	Relation	r;

	Assert(lockmode >= NoLock && lockmode < MAX_LOCKMODES);

	/* 先获取锁 */
	if (lockmode != NoLock)
		LockRelationOid(relationId, lockmode);

	/*
	 * 既然已经持有锁，探测该关系是否真的存在。
	 */
	if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(relationId)))
	{
		/* 释放无用的锁 */
		if (lockmode != NoLock)
			UnlockRelationOid(relationId, lockmode);

		return NULL;
	}

	/* 此时做 relcache 加载应当是安全的 */
	r = RelationIdGetRelation(relationId);

	if (!RelationIsValid(r))
		elog(ERROR, "could not open relation with OID %u", relationId);

	/* 如果我们自己没有获取锁，断言调用方持有该锁 */
	Assert(lockmode != NoLock ||
		   CheckRelationLockedByMe(r, AccessShareLock, true));

	/* 记录我们访问了一个临时关系 */
	if (RelationUsesLocalBuffers(r))
		MyXactFlags |= XACT_FLAGS_ACCESSEDTEMPNAMESPACE;

	pgstat_init_relation(r);

	return r;
}

/* ----------------
 *		relation_openrv - open any relation specified by a RangeVar
 *
 *		Same as relation_open, but the relation is specified by a RangeVar.
 * ----------------
 */
Relation
relation_openrv(const RangeVar *relation, LOCKMODE lockmode)
{
	Oid			relOid;

	/*
	 * Check for shared-cache-inval messages before trying to open the
	 * relation.  This is needed even if we already hold a lock on the
	 * relation, because GRANT/REVOKE are executed without taking any lock on
	 * the target relation, and we want to be sure we see current ACL
	 * information.  We can skip this if asked for NoLock, on the assumption
	 * that such a call is not the first one in the current command, and so we
	 * should be reasonably up-to-date already.  (XXX this all could stand to
	 * be redesigned, but for the moment we'll keep doing this like it's been
	 * done historically.)
	 */
	if (lockmode != NoLock)
		AcceptInvalidationMessages();

	/* 通过命名空间搜索查找并锁定相应的关系 */
	relOid = RangeVarGetRelid(relation, lockmode, false);

	/* 其余工作交给 relation_open 完成 */
	return relation_open(relOid, NoLock);
}

/* ----------------
 *		relation_openrv_extended - open any relation specified by a RangeVar
 *
 *		Same as relation_openrv, but with an additional missing_ok argument
 *		allowing a NULL return rather than an error if the relation is not
 *		found.  (Note that some other causes, such as permissions problems,
 *		will still result in an ereport.)
 * ----------------
 */
Relation
relation_openrv_extended(const RangeVar *relation, LOCKMODE lockmode,
						 bool missing_ok)
{
	Oid			relOid;

	/*
	 * Check for shared-cache-inval messages before trying to open the
	 * relation.  See comments in relation_openrv().
	 */
	if (lockmode != NoLock)
		AcceptInvalidationMessages();

	/* 通过命名空间搜索查找并锁定相应的关系 */
	relOid = RangeVarGetRelid(relation, lockmode, missing_ok);

	/* 未找到时返回 NULL */
	if (!OidIsValid(relOid))
		return NULL;

	/* 其余工作交给 relation_open 完成 */
	return relation_open(relOid, NoLock);
}

/* ----------------
 *		relation_close - close any relation
 *
 *		If lockmode is not "NoLock", we then release the specified lock.
 *
 *		Note that it is often sensible to hold a lock beyond relation_close;
 *		in that case, the lock is released automatically at xact end.
 * ----------------
 */
void
relation_close(Relation relation, LOCKMODE lockmode)
{
	LockRelId	relid = relation->rd_lockInfo.lockRelId;

	Assert(lockmode >= NoLock && lockmode < MAX_LOCKMODES);

	/* The relcache does the real work... */
	RelationClose(relation);

	if (lockmode != NoLock)
		UnlockRelationId(&relid, lockmode);
}
