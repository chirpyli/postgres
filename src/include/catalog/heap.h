/*-------------------------------------------------------------------------
 *
 * heap.h
 *		backend/catalog/heap.c 中各函数的原型声明
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/heap.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef HEAP_H
#define HEAP_H

#include "catalog/indexing.h"
#include "catalog/objectaddress.h"
#include "parser/parse_node.h"


/* CheckAttributeType/CheckAttributeNamesTypes 的标志位 */
#define CHKATYPE_ANYARRAY		0x01	/* 允许 ANYARRAY */
#define CHKATYPE_ANYRECORD		0x02	/* 允许 RECORD 和 RECORD[] */
#define CHKATYPE_IS_PARTKEY		0x04	/* attname 是分区键编号而非列名 */
#define CHKATYPE_IS_VIRTUAL		0x08	/* 是否为虚拟生成列 */

typedef struct RawColumnDefault
{
	AttrNumber	attnum;			/* 要附加默认值的属性 */
	Node	   *raw_default;	/* 默认值（未经转换的解析树） */
	char		generated;		/* attgenerated 设置 */
} RawColumnDefault;

typedef struct CookedConstraint
{
	ConstrType	contype;		/* 约束类型：CONSTR_DEFAULT、CONSTR_CHECK、
								 * CONSTR_NOTNULL */
	Oid			conoid;			/* 若已创建则为约束 OID，否则为 Invalid */
	char	   *name;			/* 名称，没有则为 NULL */
	AttrNumber	attnum;			/* 对应哪个属性（仅用于 NOTNULL、DEFAULT） */
	Node	   *expr;			/* 转换后的默认值或 CHECK 表达式 */
	bool		is_enforced;	/* 是否强制执行？（仅用于 CHECK） */
	bool		skip_validation;	/* 是否跳过校验？（仅用于 CHECK） */
	bool		is_local;		/* 约束具有本地（非继承）定义 */
	int16		inhcount;		/* 约束被继承的次数 */
	bool		is_no_inherit;	/* 约束具有本地定义且不能被
								 * 继承 */
} CookedConstraint;

extern Relation heap_create(const char *relname,
							Oid relnamespace,
							Oid reltablespace,
							Oid relid,
							RelFileNumber relfilenumber,
							Oid accessmtd,
							TupleDesc tupDesc,
							char relkind,
							char relpersistence,
							bool shared_relation,
							bool mapped_relation,
							bool allow_system_table_mods,
							TransactionId *relfrozenxid,
							MultiXactId *relminmxid,
							bool create_storage);

extern Oid	heap_create_with_catalog(const char *relname,
									 Oid relnamespace,
									 Oid reltablespace,
									 Oid relid,
									 Oid reltypeid,
									 Oid reloftypeid,
									 Oid ownerid,
									 Oid accessmtd,
									 TupleDesc tupdesc,
									 List *cooked_constraints,
									 char relkind,
									 char relpersistence,
									 bool shared_relation,
									 bool mapped_relation,
									 OnCommitAction oncommit,
									 Datum reloptions,
									 bool use_user_acl,
									 bool allow_system_table_mods,
									 bool is_internal,
									 Oid relrewrite,
									 ObjectAddress *typaddress);

extern void heap_drop_with_catalog(Oid relid);

extern void heap_truncate(List *relids);

extern void heap_truncate_one_rel(Relation rel);

extern void heap_truncate_check_FKs(List *relations, bool tempTables);

extern List *heap_truncate_find_FKs(List *relationIds);

extern void InsertPgAttributeTuples(Relation pg_attribute_rel,
									TupleDesc tupdesc,
									Oid new_rel_oid,
									const FormExtraData_pg_attribute tupdesc_extra[],
									CatalogIndexState indstate);

extern void InsertPgClassTuple(Relation pg_class_desc,
							   Relation new_rel_desc,
							   Oid new_rel_oid,
							   Datum relacl,
							   Datum reloptions);

extern List *AddRelationNewConstraints(Relation rel,
									   List *newColDefaults,
									   List *newConstraints,
									   bool allow_merge,
									   bool is_local,
									   bool is_internal,
									   const char *queryString);
extern List *AddRelationNotNullConstraints(Relation rel,
										   List *constraints,
										   List *old_notnulls,
										   List *existing_constraints);

extern void RelationClearMissing(Relation rel);

extern void StoreAttrMissingVal(Relation rel, AttrNumber attnum,
								Datum missingval);
extern void SetAttrMissing(Oid relid, char *attname, char *value);

extern Node *cookDefault(ParseState *pstate,
						 Node *raw_default,
						 Oid atttypid,
						 int32 atttypmod,
						 const char *attname,
						 char attgenerated);

extern void DeleteRelationTuple(Oid relid);
extern void DeleteAttributeTuples(Oid relid);
extern void DeleteSystemAttributeTuples(Oid relid);
extern void RemoveAttributeById(Oid relid, AttrNumber attnum);

extern void CopyStatistics(Oid fromrelid, Oid torelid);
extern void RemoveStatistics(Oid relid, AttrNumber attnum);

extern const FormData_pg_attribute *SystemAttributeDefinition(AttrNumber attno);

extern const FormData_pg_attribute *SystemAttributeByName(const char *attname);

extern void CheckAttributeNamesTypes(TupleDesc tupdesc, char relkind,
									 int flags);

extern void CheckAttributeType(const char *attname,
							   Oid atttypid, Oid attcollation,
							   List *containing_rowtypes,
							   int flags);

/* pg_partitioned_table 系统表操作函数 */
extern void StorePartitionKey(Relation rel,
							  char strategy,
							  int16 partnatts,
							  AttrNumber *partattrs,
							  List *partexprs,
							  Oid *partopclass,
							  Oid *partcollation);
extern void RemovePartitionKeyByRelId(Oid relid);
extern void StorePartitionBound(Relation rel, Relation parent,
								PartitionBoundSpec *bound);

#endif							/* HEAP_H */
