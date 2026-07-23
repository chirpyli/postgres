/*-------------------------------------------------------------------------
 *
 * pg_attrdef.c
 *	  用于支持对 pg_attrdef 关系进行操作的例程。
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_attrdef.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relation.h"
#include "access/table.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_attrdef.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"
#include "utils/syscache.h"


/*
 * 为关系 rel 的列 attnum 存储一个默认表达式。
 *
 * 返回新 pg_attrdef 元组的 OID。
 */
Oid
StoreAttrDefault(Relation rel, AttrNumber attnum,
				 Node *expr, bool is_internal)
{
	char	   *adbin;
	Relation	adrel;
	HeapTuple	tuple;
	Datum		values[Natts_pg_attrdef];
	static bool nulls[Natts_pg_attrdef] = {false, false, false, false};
	Relation	attrrel;
	HeapTuple	atttup;
	Form_pg_attribute attStruct;
	Datum		valuesAtt[Natts_pg_attribute] = {0};
	bool		nullsAtt[Natts_pg_attribute] = {0};
	bool		replacesAtt[Natts_pg_attribute] = {0};
	char		attgenerated;
	Oid			attrdefOid;
	ObjectAddress colobject,
				defobject;

	adrel = table_open(AttrDefaultRelationId, RowExclusiveLock);

	/*
	 * 将表达式扁平化为字符串形式以便存储。
	 */
	adbin = nodeToString(expr);

	/*
	 * 创建 pg_attrdef 目录项。
	 */
	attrdefOid = GetNewOidWithIndex(adrel, AttrDefaultOidIndexId,
									Anum_pg_attrdef_oid);
	values[Anum_pg_attrdef_oid - 1] = ObjectIdGetDatum(attrdefOid);
	values[Anum_pg_attrdef_adrelid - 1] = ObjectIdGetDatum(RelationGetRelid(rel));
	values[Anum_pg_attrdef_adnum - 1] = Int16GetDatum(attnum);
	values[Anum_pg_attrdef_adbin - 1] = CStringGetTextDatum(adbin);

	tuple = heap_form_tuple(adrel->rd_att, values, nulls);
	CatalogTupleInsert(adrel, tuple);

	defobject.classId = AttrDefaultRelationId;
	defobject.objectId = attrdefOid;
	defobject.objectSubId = 0;

	table_close(adrel, RowExclusiveLock);

	/* 现在可以释放上面分配的部分内容 */
	pfree(DatumGetPointer(values[Anum_pg_attrdef_adbin - 1]));
	heap_freetuple(tuple);
	pfree(adbin);

	/*
	 * 更新该列的 pg_attribute 目录项，以标明已存在默认值。
	 */
	attrrel = table_open(AttributeRelationId, RowExclusiveLock);
	atttup = SearchSysCacheCopy2(ATTNUM,
								 ObjectIdGetDatum(RelationGetRelid(rel)),
								 Int16GetDatum(attnum));
	if (!HeapTupleIsValid(atttup))
		elog(ERROR, "cache lookup failed for attribute %d of relation %u",
			 attnum, RelationGetRelid(rel));
	attStruct = (Form_pg_attribute) GETSTRUCT(atttup);
	attgenerated = attStruct->attgenerated;

	valuesAtt[Anum_pg_attribute_atthasdef - 1] = BoolGetDatum(true);
	replacesAtt[Anum_pg_attribute_atthasdef - 1] = true;

	atttup = heap_modify_tuple(atttup, RelationGetDescr(attrrel),
							   valuesAtt, nullsAtt, replacesAtt);

	CatalogTupleUpdate(attrrel, &atttup->t_self, atttup);

	table_close(attrrel, RowExclusiveLock);
	heap_freetuple(atttup);

	/*
	 * 建立依赖，使得当列（或整张表）被删除时 pg_attrdef 目录项也随之消失。
	 * 对于生成列（generated column），则将其设为内部依赖（internal dependency），
	 * 以防止默认表达式被单独删除。
	 */
	colobject.classId = RelationRelationId;
	colobject.objectId = RelationGetRelid(rel);
	colobject.objectSubId = attnum;

	recordDependencyOn(&defobject, &colobject,
					   attgenerated ? DEPENDENCY_INTERNAL : DEPENDENCY_AUTO);

	/*
	 * 同时记录表达式中用到的对象的依赖。
	 */
	recordDependencyOnSingleRelExpr(&defobject, expr, RelationGetRelid(rel),
									DEPENDENCY_NORMAL,
									DEPENDENCY_NORMAL, false);

	/*
	 * 属性默认值的创建后钩子（post-creation hook）。
	 *
	 * XXX. ALTER TABLE ALTER COLUMN SET/DROP DEFAULT 是通过对列的默认目录项
	 * 进行若干次删除/创建来实现的，因此如果被调用方（callee）需要加以区分，
	 * 应当检查该目录项是否存在较早的版本。
	 */
	InvokeObjectPostCreateHookArg(AttrDefaultRelationId,
								  RelationGetRelid(rel), attnum, is_internal);

	return attrdefOid;
}


/*
 *		RemoveAttrDefault
 *
 * 如果指定的关系/属性存在默认值，则将其移除。
 * （若不存在默认值，则在 complain 为真时报错，否则静默返回。）
 */
void
RemoveAttrDefault(Oid relid, AttrNumber attnum,
				  DropBehavior behavior, bool complain, bool internal)
{
	Relation	attrdef_rel;
	ScanKeyData scankeys[2];
	SysScanDesc scan;
	HeapTuple	tuple;
	bool		found = false;

	attrdef_rel = table_open(AttrDefaultRelationId, RowExclusiveLock);

	ScanKeyInit(&scankeys[0],
				Anum_pg_attrdef_adrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	ScanKeyInit(&scankeys[1],
				Anum_pg_attrdef_adnum,
				BTEqualStrategyNumber, F_INT2EQ,
				Int16GetDatum(attnum));

	scan = systable_beginscan(attrdef_rel, AttrDefaultIndexId, true,
							  NULL, 2, scankeys);

	/* 至多应有一个匹配的元组，但无论如何我们都循环处理 */
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		ObjectAddress object;
		Form_pg_attrdef attrtuple = (Form_pg_attrdef) GETSTRUCT(tuple);

		object.classId = AttrDefaultRelationId;
		object.objectId = attrtuple->oid;
		object.objectSubId = 0;

		performDeletion(&object, behavior,
						internal ? PERFORM_DELETION_INTERNAL : 0);

		found = true;
	}

	systable_endscan(scan);
	table_close(attrdef_rel, RowExclusiveLock);

	if (complain && !found)
		elog(ERROR, "could not find attrdef tuple for relation %u attnum %d",
			 relid, attnum);
}

/*
 *		RemoveAttrDefaultById
 *
 * 移除由 OID 指定的 pg_attrdef 目录项。这是属性默认值移除的核心实现。
 * 注意：它应当经由 performDeletion 调用，而非直接调用。
 */
void
RemoveAttrDefaultById(Oid attrdefId)
{
	Relation	attrdef_rel;
	Relation	attr_rel;
	Relation	myrel;
	ScanKeyData scankeys[1];
	SysScanDesc scan;
	HeapTuple	tuple;
	Oid			myrelid;
	AttrNumber	myattnum;

	/* 在 pg_attrdef 关系上获取适当的锁 */
	attrdef_rel = table_open(AttrDefaultRelationId, RowExclusiveLock);

	/* 查找 pg_attrdef 元组 */
	ScanKeyInit(&scankeys[0],
				Anum_pg_attrdef_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(attrdefId));

	scan = systable_beginscan(attrdef_rel, AttrDefaultOidIndexId, true,
							  NULL, 1, scankeys);

	tuple = systable_getnext(scan);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "could not find tuple for attrdef %u", attrdefId);

	myrelid = ((Form_pg_attrdef) GETSTRUCT(tuple))->adrelid;
	myattnum = ((Form_pg_attrdef) GETSTRUCT(tuple))->adnum;

	/* 对拥有该属性的关系获取排他锁 */
	myrel = relation_open(myrelid, AccessExclusiveLock);

	/* 现在可以删除 pg_attrdef 行了 */
	CatalogTupleDelete(attrdef_rel, &tuple->t_self);

	systable_endscan(scan);
	table_close(attrdef_rel, RowExclusiveLock);

	/* 修正 pg_attribute 行 */
	attr_rel = table_open(AttributeRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopy2(ATTNUM,
								ObjectIdGetDatum(myrelid),
								Int16GetDatum(myattnum));
	if (!HeapTupleIsValid(tuple))	/* 不应发生 */
		elog(ERROR, "cache lookup failed for attribute %d of relation %u",
			 myattnum, myrelid);

	((Form_pg_attribute) GETSTRUCT(tuple))->atthasdef = false;

	CatalogTupleUpdate(attr_rel, &tuple->t_self, tuple);

	/*
	 * 我们对 pg_attribute 行的更新会强制引发 relcache 重建，因此这里无需再做其他事情。
	 */
	table_close(attr_rel, RowExclusiveLock);

	/* 将属性的关系锁保持到事务结束 */
	relation_close(myrel, NoLock);
}


/*
 * 获取由关系 OID 与列号所标识的列的默认表达式对应的 pg_attrdef OID。
 *
 * 若不存在这样的 pg_attrdef 目录项，则返回 InvalidOid。
 */
Oid
GetAttrDefaultOid(Oid relid, AttrNumber attnum)
{
	Oid			result = InvalidOid;
	Relation	attrdef;
	ScanKeyData keys[2];
	SysScanDesc scan;
	HeapTuple	tup;

	attrdef = table_open(AttrDefaultRelationId, AccessShareLock);
	ScanKeyInit(&keys[0],
				Anum_pg_attrdef_adrelid,
				BTEqualStrategyNumber,
				F_OIDEQ,
				ObjectIdGetDatum(relid));
	ScanKeyInit(&keys[1],
				Anum_pg_attrdef_adnum,
				BTEqualStrategyNumber,
				F_INT2EQ,
				Int16GetDatum(attnum));
	scan = systable_beginscan(attrdef, AttrDefaultIndexId, true,
							  NULL, 2, keys);

	if (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_attrdef atdform = (Form_pg_attrdef) GETSTRUCT(tup);

		result = atdform->oid;
	}

	systable_endscan(scan);
	table_close(attrdef, AccessShareLock);

	return result;
}

/*
 * 给定一个 pg_attrdef OID，返回其所属列的关系 OID 与列号
 * （为方便起见，以 ObjectAddress 形式表示）。
 *
 * 若不存在这样的 pg_attrdef 目录项，则返回 InvalidObjectAddress。
 */
ObjectAddress
GetAttrDefaultColumnAddress(Oid attrdefoid)
{
	ObjectAddress result = InvalidObjectAddress;
	Relation	attrdef;
	ScanKeyData skey[1];
	SysScanDesc scan;
	HeapTuple	tup;

	attrdef = table_open(AttrDefaultRelationId, AccessShareLock);
	ScanKeyInit(&skey[0],
				Anum_pg_attrdef_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(attrdefoid));
	scan = systable_beginscan(attrdef, AttrDefaultOidIndexId, true,
							  NULL, 1, skey);

	if (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_attrdef atdform = (Form_pg_attrdef) GETSTRUCT(tup);

		result.classId = RelationRelationId;
		result.objectId = atdform->adrelid;
		result.objectSubId = atdform->adnum;
	}

	systable_endscan(scan);
	table_close(attrdef, AccessShareLock);

	return result;
}
