/*-------------------------------------------------------------------------
 *
 * heap.c
 *	  用于创建和销毁 POSTGRES 堆关系的代码
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/heap.c
 *
 *
 * INTERFACE ROUTINES
 *		heap_create()			- 创建未编目的堆关系
 *		heap_create_with_catalog() - 创建已编目的关系
 *		heap_drop_with_catalog() - 从系统目录中移除指定名称的关系
 *
 * NOTES
 *	  本代码取自 access/heap/create.c，其中包含了旧的
 *	  heap_create_with_catalog、amcreate 和 amdestroy。
 *	  这些例程很快会通过函数管理器来调用这些新例程，
 *	  就像那些命名拙劣的 "NewXXX" 例程所做的那样。而
 *	  "New" 系列例程终将彻底消亡，一劳永逸！
 *		-cim 1/13/91
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/multixact.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/binary_upgrade.h"
#include "catalog/catalog.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/objectaccess.h"
#include "catalog/partition.h"
#include "catalog/pg_am.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_partitioned_table.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_subscription_rel.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_type.h"
#include "catalog/storage.h"
#include "commands/tablecmds.h"
#include "commands/typecmds.h"
#include "common/int.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_coerce.h"
#include "parser/parse_collate.h"
#include "parser/parse_expr.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "partitioning/partdesc.h"
#include "pgstat.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"


/* 可能被 pg_upgrade_support 函数设置 */
Oid			binary_upgrade_next_heap_pg_class_oid = InvalidOid;
Oid			binary_upgrade_next_toast_pg_class_oid = InvalidOid;
RelFileNumber binary_upgrade_next_heap_pg_class_relfilenumber = InvalidRelFileNumber;
RelFileNumber binary_upgrade_next_toast_pg_class_relfilenumber = InvalidRelFileNumber;

static void AddNewRelationTuple(Relation pg_class_desc,
								Relation new_rel_desc,
								Oid new_rel_oid,
								Oid new_type_oid,
								Oid reloftype,
								Oid relowner,
								char relkind,
								TransactionId relfrozenxid,
								TransactionId relminmxid,
								Datum relacl,
								Datum reloptions);
static ObjectAddress AddNewRelationType(const char *typeName,
										Oid typeNamespace,
										Oid new_rel_oid,
										char new_rel_kind,
										Oid ownerid,
										Oid new_row_type,
										Oid new_array_type);
static void RelationRemoveInheritance(Oid relid);
static Oid	StoreRelCheck(Relation rel, const char *ccname, Node *expr,
						  bool is_enforced, bool is_validated, bool is_local,
						  int16 inhcount, bool is_no_inherit, bool is_internal);
static void StoreConstraints(Relation rel, List *cooked_constraints,
							 bool is_internal);
static bool MergeWithExistingConstraint(Relation rel, const char *ccname, Node *expr,
										bool allow_merge, bool is_local,
										bool is_enforced,
										bool is_initially_valid,
										bool is_no_inherit);
static void SetRelationNumChecks(Relation rel, int numchecks);
static Node *cookConstraint(ParseState *pstate,
							Node *raw_constraint,
							char *relname);


/* ----------------------------------------------------------------
 *				XXX 丑陋的硬编码不良代码自此开始 XXX
 *
 *		这些最好全部移到 lib/catalog 模块的某个位置，
 *		或者干脆先彻底删除。
 * ----------------------------------------------------------------
 */


/*
 * 注意：
 *		今后系统是否应该对这些属性做特殊处理？
 *		优点：在 ATTRIBUTE 关系中占用更少的空间。
 *		缺点：特例会散落得到处都是。
 */

/*
 * 下方的初始化器未包含尾随的变长字段，
 * 但这没有问题——我们无论如何都不会引用该结构固定大小
 * 部分之外的任何内容。可以默认为零的字段也未在此列出。
 */

static const FormData_pg_attribute a1 = {
	.attname = {"ctid"},
	.atttypid = TIDOID,
	.attlen = sizeof(ItemPointerData),
	.attnum = SelfItemPointerAttributeNumber,
	.atttypmod = -1,
	.attbyval = false,
	.attalign = TYPALIGN_SHORT,
	.attstorage = TYPSTORAGE_PLAIN,
	.attnotnull = true,
	.attislocal = true,
};

static const FormData_pg_attribute a2 = {
	.attname = {"xmin"},
	.atttypid = XIDOID,
	.attlen = sizeof(TransactionId),
	.attnum = MinTransactionIdAttributeNumber,
	.atttypmod = -1,
	.attbyval = true,
	.attalign = TYPALIGN_INT,
	.attstorage = TYPSTORAGE_PLAIN,
	.attnotnull = true,
	.attislocal = true,
};

static const FormData_pg_attribute a3 = {
	.attname = {"cmin"},
	.atttypid = CIDOID,
	.attlen = sizeof(CommandId),
	.attnum = MinCommandIdAttributeNumber,
	.atttypmod = -1,
	.attbyval = true,
	.attalign = TYPALIGN_INT,
	.attstorage = TYPSTORAGE_PLAIN,
	.attnotnull = true,
	.attislocal = true,
};

static const FormData_pg_attribute a4 = {
	.attname = {"xmax"},
	.atttypid = XIDOID,
	.attlen = sizeof(TransactionId),
	.attnum = MaxTransactionIdAttributeNumber,
	.atttypmod = -1,
	.attbyval = true,
	.attalign = TYPALIGN_INT,
	.attstorage = TYPSTORAGE_PLAIN,
	.attnotnull = true,
	.attislocal = true,
};

static const FormData_pg_attribute a5 = {
	.attname = {"cmax"},
	.atttypid = CIDOID,
	.attlen = sizeof(CommandId),
	.attnum = MaxCommandIdAttributeNumber,
	.atttypmod = -1,
	.attbyval = true,
	.attalign = TYPALIGN_INT,
	.attstorage = TYPSTORAGE_PLAIN,
	.attnotnull = true,
	.attislocal = true,
};

/*
 * 我们决定将本属性命名为 "tableoid" 而非 "classoid"，
 * 其理由是未来某个特定类/类型可能会有不止一个表。
 * 无论如何，SQL 中使用的词仍然是 table。
 */
static const FormData_pg_attribute a6 = {
	.attname = {"tableoid"},
	.atttypid = OIDOID,
	.attlen = sizeof(Oid),
	.attnum = TableOidAttributeNumber,
	.atttypmod = -1,
	.attbyval = true,
	.attalign = TYPALIGN_INT,
	.attstorage = TYPSTORAGE_PLAIN,
	.attnotnull = true,
	.attislocal = true,
};

static const FormData_pg_attribute *const SysAtt[] = {&a1, &a2, &a3, &a4, &a5, &a6};

/*
 * 本函数返回一个指向系统属性的 Form_pg_attribute 指针。
 * 注意，若传入的 attno 无效我们会 elog，而这只会在
 * 上游出现问题时才会发生。
 */
const FormData_pg_attribute *
SystemAttributeDefinition(AttrNumber attno)
{
	if (attno >= 0 || attno < -(int) lengthof(SysAtt))
		elog(ERROR, "invalid system attribute number %d", attno);
	return SysAtt[-attno - 1];
}

/*
 * 若给定名称是一个系统属性名，则返回用于原型定义的
 * Form_pg_attribute 指针；否则返回 NULL。
 */
const FormData_pg_attribute *
SystemAttributeByName(const char *attname)
{
	int			j;

	for (j = 0; j < (int) lengthof(SysAtt); j++)
	{
		const FormData_pg_attribute *att = SysAtt[j];

		if (strcmp(NameStr(att->attname), attname) == 0)
			return att;
	}

	return NULL;
}


/* ----------------------------------------------------------------
 *				XXX 丑陋硬编码不良代码至此结束 XXX
 * ---------------------------------------------------------------- */


/* ----------------------------------------------------------------
 *		heap_create		- 创建未编目的堆关系
 *
 *		注意 API 变更：调用者现在必须始终提供用于该关系的 OID。
 *		relfilenumber 可以不指定(在最简单的情况下确实如此)。
 *
 *		create_storage 指示是否要创建存储。
 *		不过，即便 create_storage 为 true，若 relkind 属于
 *		不具有存储的类型，也不会创建任何存储。
 *
 *		rel->rd_rel 由 RelationBuildLocalRelation 初始化，
 *		返回时基本全为零。
 * ----------------------------------------------------------------
 */
Relation
heap_create(const char *relname,
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
			bool create_storage)
{
	Relation	rel;

	/* 调用者必须已经为该关系提供了一个 OID。 */
	Assert(OidIsValid(relid));

	/*
	 * 不允许直接在 pg_catalog 中创建关系，尽管允许将用户定义的
	 * 关系移动到那里。包含 pg_catalog 的搜索路径在语义上目前
	 * 过于混乱。
	 *
	 * 但允许在 pg_catalog 中的关系上创建索引，即使
	 * allow_system_table_mods = off，上层已经保证它位于一个
	 * 用户定义的关系上，而非系统关系。
	 */
	if (!allow_system_table_mods &&
		((IsCatalogNamespace(relnamespace) && relkind != RELKIND_INDEX) ||
		 IsToastNamespace(relnamespace)) &&
		IsNormalProcessingMode())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to create \"%s.%s\"",
						get_namespace_name(relnamespace), relname),
				 errdetail("System catalog modifications are currently disallowed.")));

	*relfrozenxid = InvalidTransactionId;
	*relminmxid = InvalidMultiXactId;

	/*
	 * 若该关系种类不支持表空间，则强制将 reltablespace 置零。
	 * 这主要是为了整洁起见。
	 */
	if (!RELKIND_HAS_TABLESPACE(relkind))
		reltablespace = InvalidOid;

	/* 不要为没有物理存储的 relkind 创建存储。 */
	if (!RELKIND_HAS_STORAGE(relkind))
		create_storage = false;
	else
	{
			/*
			 * 若调用者未指定 relfilenumber，则以与 relid 相同的
			 * oid 创建存储。
			 */
		if (!RelFileNumberIsValid(relfilenumber))
			relfilenumber = relid;
	}

	/*
	 * 绝不允许 pg_class 条目在 reltablespace 中显式指定数据库的
	 * 默认表空间；而是将其强制置零。这确保当数据库以不同的默认
	 * 表空间被克隆时，pg_class 条目仍能匹配 CREATE DATABASE
	 * 放置物理复制关系的位置。
	 *
	 * 是的，这有点像个 hack。
	 */
	if (reltablespace == MyDatabaseTableSpace)
		reltablespace = InvalidOid;

	/*
	 * 构建 relcache 条目。
	 */
	rel = RelationBuildLocalRelation(relname,
									 relnamespace,
									 tupDesc,
									 relid,
									 accessmtd,
									 relfilenumber,
									 reltablespace,
									 shared_relation,
									 mapped_relation,
									 relpersistence,
									 relkind);

	/*
	 * 若需要，让存储管理器创建该关系的磁盘文件。
	 *
	 * 对于表，AM 回调会同时创建主 fork 和 init fork。
	 * 对于其他关系，只创建主 fork；其余 fork 将按需创建。
	 */
	if (create_storage)
	{
		if (RELKIND_HAS_TABLE_AM(rel->rd_rel->relkind))
			table_relation_set_new_filelocator(rel, &rel->rd_locator,
											   relpersistence,
											   relfrozenxid, relminmxid);
		else if (RELKIND_HAS_STORAGE(rel->rd_rel->relkind))
			RelationCreateStorage(rel->rd_locator, relpersistence, true);
		else
			Assert(false);
	}

	/*
	 * 若指定了表空间，通常该表空间的移除会由物理文件的存在来保护；
	 * 但对于没有文件的关系，需要添加一条 pg_shdepend 条目来记录这一点。
	 */
	if (!create_storage && reltablespace != InvalidOid)
		recordDependencyOnTablespace(RelationRelationId, relid,
									 reltablespace);

	/* 确保若事务中止则丢弃统计信息 */
	pgstat_create_relation(rel);

	return rel;
}

/* ----------------------------------------------------------------
 *		heap_create_with_catalog		- 创建已编目的关系
 *
 *		这分多个步骤完成：
 *
 *		1) 使用 CheckAttributeNamesTypes() 确保元组
 *		   描述符包含一组有效的属性名与类型
 *
 *		2) 打开 pg_class，并由 get_relname_relid()
 *		   执行扫描以确保不存在同名关系。
 *
 *		3) 调用 heap_create() 在磁盘上创建新关系。
 *
 *		4) 调用 TypeCreate() 定义对应于新关系的
 *		   一个新类型。
 *
 *		5) 调用 AddNewRelationTuple() 将该关系
 *		   登记到 pg_class 中。
 *
 *		6) 调用 AddNewAttributeTuples() 将该
 *		   新关系的模式登记到 pg_attribute 中。
 *
 *		7) StoreConstraints() is called			- vadim 08/22/97
 *
 *		8) 关系被关闭，并返回新关系的 oid。
 *
 * ----------------------------------------------------------------
 */

/* --------------------------------
 *		CheckAttributeNamesTypes
 *
 *		用于确保元组描述符包含一组有效的属性名与数据类型。
 *		一旦发现问题，直接生成 ereport(ERROR) 中止当前事务。
 *
 *		relkind 为待创建关系的 relkind。
 *		flags 控制允许哪些数据类型，参见 CheckAttributeType。
 * --------------------------------
 */
void
CheckAttributeNamesTypes(TupleDesc tupdesc, char relkind,
						 int flags)
{
	int			i;
	int			j;
	int			natts = tupdesc->natts;

	/* 对列数做合理性检查 */
	if (natts < 0 || natts > MaxHeapAttributeNumber)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_COLUMNS),
				 errmsg("tables can have at most %d columns",
						MaxHeapAttributeNumber)));

	/*
	 * 首先检查是否与系统属性名冲突
	 *
	 * 对于视图或类型关系跳过此步，因为它们没有系统属性。
	 */
	if (relkind != RELKIND_VIEW && relkind != RELKIND_COMPOSITE_TYPE)
	{
		for (i = 0; i < natts; i++)
		{
			Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

			if (SystemAttributeByName(NameStr(attr->attname)) != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_COLUMN),
						 errmsg("column name \"%s\" conflicts with a system column name",
								NameStr(attr->attname))));
		}
	}

	/*
	 * 接下来检查重复的属性名
	 */
	for (i = 1; i < natts; i++)
	{
		for (j = 0; j < i; j++)
		{
			if (strcmp(NameStr(TupleDescAttr(tupdesc, j)->attname),
					   NameStr(TupleDescAttr(tupdesc, i)->attname)) == 0)
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_COLUMN),
						 errmsg("column name \"%s\" specified more than once",
								NameStr(TupleDescAttr(tupdesc, j)->attname))));
		}
	}

	/*
	 * 接下来检查属性的类型
	 */
	for (i = 0; i < natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

		if (attr->attisdropped)
			continue;
		CheckAttributeType(NameStr(attr->attname),
						   attr->atttypid,
						   attr->attcollation,
						   NIL, /* assume we're creating a new rowtype */
						   flags | (attr->attgenerated == ATTRIBUTE_GENERATED_VIRTUAL ? CHKATYPE_IS_VIRTUAL : 0));
	}
}

/* --------------------------------
 *		CheckAttributeType
 *
 *		验证属性提议使用的数据类型是否合法。
 *		这主要是因为在目录中存在一些(伪)类型，我们并不支持
 *		将它们作为真实元组的元素。我们还会检查一个表列所需的
 *		其他一些属性。
 *
 * 若该属性是被提议添加到已有的表或组合类型上，请传入一个
 * 仅含该 rowtype OID 的单元素列表作为 containing_rowtypes。
 * 当检查一个待创建的行类型时，传入 NIL 即可，因为不可能存在
 * 对尚未存在的行类型的递归引用。
 *
 * flags 是一个位掩码，控制允许哪些数据类型。在大多数情况下，
 * 伪类型不允许作为属性类型，但也有一些例外：ANYARRAYOID、
 * RECORDOID 和 RECORDARRAYOID 在某些情况下可以被允许。
 * (这之所以可行，是因为这些类型类的值在某种程度上是自描述的。
 * 不过 RECORDOID 和 RECORDARRAYOID 仅在会话内部才能可靠识别，
 * 因为其标识信息可能使用了仅在本地分配的 typmod。调用者应当
 * 了解这些情况是否安全。)
 *
 * flags 还可以控制错误消息的措辞。若指定了 CHKATYPE_IS_PARTKEY，
 * 则 "attname" 应为分区键列号(文本形式)，而非真实的列名。
 * --------------------------------
 */
void
CheckAttributeType(const char *attname,
				   Oid atttypid, Oid attcollation,
				   List *containing_rowtypes,
				   int flags)
{
	char		att_typtype = get_typtype(atttypid);
	Oid			att_typelem;

	/* 由于本函数会递归，可能被驱动到栈溢出 */
	check_stack_depth();

	if (att_typtype == TYPTYPE_PSEUDO)
	{
		/*
		 * 我们禁止伪类型列，但 ANYARRAY、RECORD 和 RECORD[] 在调用者
		 * 声明允许的情况下是例外。
		 *
		 * 对于 RECORD 和 RECORD[]，我们不必担心递归包含，因为
		 * (a) 不允许任何命名的组合类型包含它们，(b) 两个“匿名”记录类型
		 * 不可能被视为同一类型，因此不可能出现无限递归。
		 */
		if (!((atttypid == ANYARRAYOID && (flags & CHKATYPE_ANYARRAY)) ||
			  (atttypid == RECORDOID && (flags & CHKATYPE_ANYRECORD)) ||
			  (atttypid == RECORDARRAYOID && (flags & CHKATYPE_ANYRECORD))))
		{
			if (flags & CHKATYPE_IS_PARTKEY)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				/* translator: first %s is an integer not a name */
						 errmsg("partition key column %s has pseudo-type %s",
								attname, format_type_be(atttypid))));
			else
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("column \"%s\" has pseudo-type %s",
								attname, format_type_be(atttypid))));
		}
	}
	else if (att_typtype == TYPTYPE_DOMAIN)
	{
		/*
		 * 防止虚拟生成列使用域类型。否则当生成列底层的列发生变化时，
		 * 我们必须强制实施域约束。这或许可以实现，但目前没有。
		 */
		if (flags & CHKATYPE_IS_VIRTUAL)
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("virtual generated column \"%s\" cannot have a domain type", attname));

		/*
		 * 若是域，则递归检查其基类型。
		 */
		CheckAttributeType(attname, getBaseType(atttypid), attcollation,
						   containing_rowtypes,
						   flags);
	}
	else if (att_typtype == TYPTYPE_COMPOSITE)
	{
		/*
		 * 对于组合类型，递归检查其属性。
		 */
		Relation	relation;
		TupleDesc	tupdesc;
		int			i;

		/*
		 * 检查自包含。将来我们或许允许这种情况(若如此则直接无错误地
		 * 返回)，但目前尚不清楚在允许表包含自身行类型之前，还有多少
		 * 其他位置需要防御性的反递归保护才能确保安全。
		 */
		if (list_member_oid(containing_rowtypes, atttypid))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("composite type %s cannot be made a member of itself",
							format_type_be(atttypid))));

		containing_rowtypes = lappend_oid(containing_rowtypes, atttypid);

		relation = relation_open(get_typ_typrelid(atttypid), AccessShareLock);

		tupdesc = RelationGetDescr(relation);

		for (i = 0; i < tupdesc->natts; i++)
		{
			Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

			if (attr->attisdropped)
				continue;
			CheckAttributeType(NameStr(attr->attname),
							   attr->atttypid, attr->attcollation,
							   containing_rowtypes,
							   flags & ~CHKATYPE_IS_PARTKEY);
		}

		relation_close(relation, AccessShareLock);

		containing_rowtypes = list_delete_last(containing_rowtypes);
	}
	else if (att_typtype == TYPTYPE_RANGE)
	{
		/*
		 * 若是范围类型，递归检查其子类型。
		 */
		CheckAttributeType(attname, get_range_subtype(atttypid),
						   get_range_collation(atttypid),
						   containing_rowtypes,
						   flags);
	}
	else if (att_typtype == TYPTYPE_MULTIRANGE)
	{
		/*
		 * 若是多范围类型，递归检查其普通范围类型。
		 */
		CheckAttributeType(attname, get_multirange_range(atttypid),
						   InvalidOid,	/* range types are not collatable */
						   containing_rowtypes,
						   flags);
	}
	else if (OidIsValid((att_typelem = get_element_type(atttypid))))
	{
		/*
		 * 也必须递归进入数组类型，以防它们是组合类型。
		 */
		CheckAttributeType(attname, att_typelem, attcollation,
						   containing_rowtypes,
						   flags);
	}

	/*
	 * 为了与 check_virtual_generated_security() 保持一致。
	 */
	if ((flags & CHKATYPE_IS_VIRTUAL) && atttypid >= FirstUnpinnedObjectId)
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("virtual generated column \"%s\" cannot have a user-defined type", attname),
				errdetail("Virtual generated columns that make use of user-defined types are not yet supported."));

	/*
	 * 按 SQL 标准这未必严格非法，但它相当无用，且无法被转储，
	 * 因此我们必须禁止它。
	 */
	if (!OidIsValid(attcollation) && type_is_collatable(atttypid))
	{
		if (flags & CHKATYPE_IS_PARTKEY)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
			/* translator: first %s is an integer not a name */
					 errmsg("no collation was derived for partition key column %s with collatable type %s",
							attname, format_type_be(atttypid)),
					 errhint("Use the COLLATE clause to set the collation explicitly.")));
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("no collation was derived for column \"%s\" with collatable type %s",
							attname, format_type_be(atttypid)),
					 errhint("Use the COLLATE clause to set the collation explicitly.")));
	}
}

/*
 * InsertPgAttributeTuples
 *		在 pg_attribute 中构造并插入一组元组。
 *
 * 调用者已打开并锁定 pg_attribute。tupdesc 包含要插入的属性。
 * tupdesc_extra 为某些变长/可空 pg_attribute 字段提供取值，它必须与
 * tupdesc 包含相同数量的元素，或为 NULL。pg_attribute 的其他变长字段
 * 始终被初始化为 null 值。
 *
 * indstate 是供 CatalogTupleInsertWithInfo 使用的索引状态。它可以被
 * 传入 NULL，此时我们会自行获取必要信息。(插入多个属性时不要这样做，
 * 因为那样会稍微昂贵一些。)
 *
 * new_rel_oid 是分配给被插入属性的关系 OID。
 * 若设为 InvalidOid，则改用 tupdesc 中的关系 OID。
 */
void
InsertPgAttributeTuples(Relation pg_attribute_rel,
						TupleDesc tupdesc,
						Oid new_rel_oid,
						const FormExtraData_pg_attribute tupdesc_extra[],
						CatalogIndexState indstate)
{
	TupleTableSlot **slot;
	TupleDesc	td;
	int			nslots;
	int			natts = 0;
	int			slotCount = 0;
	bool		close_index = false;

	td = RelationGetDescr(pg_attribute_rel);

	/* 初始化要使用的槽数量 */
	nslots = Min(tupdesc->natts,
				 (MAX_CATALOG_MULTI_INSERT_BYTES / sizeof(FormData_pg_attribute)));
	slot = palloc(sizeof(TupleTableSlot *) * nslots);
	for (int i = 0; i < nslots; i++)
		slot[i] = MakeSingleTupleTableSlot(td, &TTSOpsHeapTuple);

	while (natts < tupdesc->natts)
	{
		Form_pg_attribute attrs = TupleDescAttr(tupdesc, natts);
		const FormExtraData_pg_attribute *attrs_extra = tupdesc_extra ? &tupdesc_extra[natts] : NULL;

		ExecClearTuple(slot[slotCount]);

		memset(slot[slotCount]->tts_isnull, false,
			   slot[slotCount]->tts_tupleDescriptor->natts * sizeof(bool));

		if (new_rel_oid != InvalidOid)
			slot[slotCount]->tts_values[Anum_pg_attribute_attrelid - 1] = ObjectIdGetDatum(new_rel_oid);
		else
			slot[slotCount]->tts_values[Anum_pg_attribute_attrelid - 1] = ObjectIdGetDatum(attrs->attrelid);

		slot[slotCount]->tts_values[Anum_pg_attribute_attname - 1] = NameGetDatum(&attrs->attname);
		slot[slotCount]->tts_values[Anum_pg_attribute_atttypid - 1] = ObjectIdGetDatum(attrs->atttypid);
		slot[slotCount]->tts_values[Anum_pg_attribute_attlen - 1] = Int16GetDatum(attrs->attlen);
		slot[slotCount]->tts_values[Anum_pg_attribute_attnum - 1] = Int16GetDatum(attrs->attnum);
		slot[slotCount]->tts_values[Anum_pg_attribute_atttypmod - 1] = Int32GetDatum(attrs->atttypmod);
		slot[slotCount]->tts_values[Anum_pg_attribute_attndims - 1] = Int16GetDatum(attrs->attndims);
		slot[slotCount]->tts_values[Anum_pg_attribute_attbyval - 1] = BoolGetDatum(attrs->attbyval);
		slot[slotCount]->tts_values[Anum_pg_attribute_attalign - 1] = CharGetDatum(attrs->attalign);
		slot[slotCount]->tts_values[Anum_pg_attribute_attstorage - 1] = CharGetDatum(attrs->attstorage);
		slot[slotCount]->tts_values[Anum_pg_attribute_attcompression - 1] = CharGetDatum(attrs->attcompression);
		slot[slotCount]->tts_values[Anum_pg_attribute_attnotnull - 1] = BoolGetDatum(attrs->attnotnull);
		slot[slotCount]->tts_values[Anum_pg_attribute_atthasdef - 1] = BoolGetDatum(attrs->atthasdef);
		slot[slotCount]->tts_values[Anum_pg_attribute_atthasmissing - 1] = BoolGetDatum(attrs->atthasmissing);
		slot[slotCount]->tts_values[Anum_pg_attribute_attidentity - 1] = CharGetDatum(attrs->attidentity);
		slot[slotCount]->tts_values[Anum_pg_attribute_attgenerated - 1] = CharGetDatum(attrs->attgenerated);
		slot[slotCount]->tts_values[Anum_pg_attribute_attisdropped - 1] = BoolGetDatum(attrs->attisdropped);
		slot[slotCount]->tts_values[Anum_pg_attribute_attislocal - 1] = BoolGetDatum(attrs->attislocal);
		slot[slotCount]->tts_values[Anum_pg_attribute_attinhcount - 1] = Int16GetDatum(attrs->attinhcount);
		slot[slotCount]->tts_values[Anum_pg_attribute_attcollation - 1] = ObjectIdGetDatum(attrs->attcollation);
		if (attrs_extra)
		{
			slot[slotCount]->tts_values[Anum_pg_attribute_attstattarget - 1] = attrs_extra->attstattarget.value;
			slot[slotCount]->tts_isnull[Anum_pg_attribute_attstattarget - 1] = attrs_extra->attstattarget.isnull;

			slot[slotCount]->tts_values[Anum_pg_attribute_attoptions - 1] = attrs_extra->attoptions.value;
			slot[slotCount]->tts_isnull[Anum_pg_attribute_attoptions - 1] = attrs_extra->attoptions.isnull;
		}
		else
		{
			slot[slotCount]->tts_isnull[Anum_pg_attribute_attstattarget - 1] = true;
			slot[slotCount]->tts_isnull[Anum_pg_attribute_attoptions - 1] = true;
		}

		/*
		 * 其余字段不会为新列设置。
		 */
		slot[slotCount]->tts_isnull[Anum_pg_attribute_attacl - 1] = true;
		slot[slotCount]->tts_isnull[Anum_pg_attribute_attfdwoptions - 1] = true;
		slot[slotCount]->tts_isnull[Anum_pg_attribute_attmissingval - 1] = true;

		ExecStoreVirtualTuple(slot[slotCount]);
		slotCount++;

		/*
		 * 若槽已满或处理已到达末尾，则插入一批元组。
		 */
		if (slotCount == nslots || natts == tupdesc->natts - 1)
		{
			/* 只在确知需要时才获取索引信息 */
			if (!indstate)
			{
				indstate = CatalogOpenIndexes(pg_attribute_rel);
				close_index = true;
			}

			/* 插入新元组并更新索引 */
			CatalogTuplesMultiInsertWithInfo(pg_attribute_rel, slot, slotCount,
											 indstate);
			slotCount = 0;
		}

		natts++;
	}

	if (close_index)
		CatalogCloseIndexes(indstate);
	for (int i = 0; i < nslots; i++)
		ExecDropSingleTupleTableSlot(slot[i]);
	pfree(slot);
}

/* --------------------------------
 *		AddNewAttributeTuples
 *
 *		通过向 pg_attribute 添加元组，登记新关系的模式。
 * --------------------------------
 */
static void
AddNewAttributeTuples(Oid new_rel_oid,
					  TupleDesc tupdesc,
					  char relkind)
{
	Relation	rel;
	CatalogIndexState indstate;
	int			natts = tupdesc->natts;
	ObjectAddress myself,
				referenced;

	/*
	 * 打开 pg_attribute 及其索引。
	 */
	rel = table_open(AttributeRelationId, RowExclusiveLock);

	indstate = CatalogOpenIndexes(rel);

	InsertPgAttributeTuples(rel, tupdesc, new_rel_oid, NULL, indstate);

	/* 添加对数据类型与排序规则的依赖 */
	for (int i = 0; i < natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

		if (attr->attisdropped)
			continue;

		/* 添加依赖信息 */
		ObjectAddressSubSet(myself, RelationRelationId, new_rel_oid, i + 1);
		ObjectAddressSet(referenced, TypeRelationId, attr->atttypid);
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

		/* 默认排序规则是固定的，无需费心记录它 */
		if (OidIsValid(attr->attcollation) &&
			attr->attcollation != DEFAULT_COLLATION_OID)
		{
			ObjectAddressSet(referenced, CollationRelationId,
							 attr->attcollation);
			recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
		}
	}

	/*
	 * 接下来添加系统属性。对于视图或类型关系则全部跳过。
	 * 这里不必建立数据类型依赖，因为这些类型大概都是固定的。
	 */
	if (relkind != RELKIND_VIEW && relkind != RELKIND_COMPOSITE_TYPE)
	{
		TupleDesc	td;

		td = CreateTupleDesc(lengthof(SysAtt), (FormData_pg_attribute **) &SysAtt);

		InsertPgAttributeTuples(rel, td, new_rel_oid, NULL, indstate);
		FreeTupleDesc(td);
	}

	/*
	 * 清理
	 */
	CatalogCloseIndexes(indstate);

	table_close(rel, RowExclusiveLock);
}

/* --------------------------------
 *		InsertPgClassTuple
 *
 *		在 pg_class 中构造并插入一个新元组。
 *
 * 调用者已打开并锁定 pg_class。
 * 元组数据取自 new_rel_desc->rd_rel，但缓存的 reldesc 中不存在的
 * 变宽字段除外。relacl 和 reloptions 以 Datum 形式传入(以避免
 * 在 heap.h 中引用这些数据类型)。传入 (Datum) 0 可将其置为 NULL。
 * --------------------------------
 */
void
InsertPgClassTuple(Relation pg_class_desc,
				   Relation new_rel_desc,
				   Oid new_rel_oid,
				   Datum relacl,
				   Datum reloptions)
{
	Form_pg_class rd_rel = new_rel_desc->rd_rel;
	Datum		values[Natts_pg_class];
	bool		nulls[Natts_pg_class];
	HeapTuple	tup;

	/* 这有点繁琐，但比我们过去的做法要干净得多... */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	values[Anum_pg_class_oid - 1] = ObjectIdGetDatum(new_rel_oid);
	values[Anum_pg_class_relname - 1] = NameGetDatum(&rd_rel->relname);
	values[Anum_pg_class_relnamespace - 1] = ObjectIdGetDatum(rd_rel->relnamespace);
	values[Anum_pg_class_reltype - 1] = ObjectIdGetDatum(rd_rel->reltype);
	values[Anum_pg_class_reloftype - 1] = ObjectIdGetDatum(rd_rel->reloftype);
	values[Anum_pg_class_relowner - 1] = ObjectIdGetDatum(rd_rel->relowner);
	values[Anum_pg_class_relam - 1] = ObjectIdGetDatum(rd_rel->relam);
	values[Anum_pg_class_relfilenode - 1] = ObjectIdGetDatum(rd_rel->relfilenode);
	values[Anum_pg_class_reltablespace - 1] = ObjectIdGetDatum(rd_rel->reltablespace);
	values[Anum_pg_class_relpages - 1] = Int32GetDatum(rd_rel->relpages);
	values[Anum_pg_class_reltuples - 1] = Float4GetDatum(rd_rel->reltuples);
	values[Anum_pg_class_relallvisible - 1] = Int32GetDatum(rd_rel->relallvisible);
	values[Anum_pg_class_relallfrozen - 1] = Int32GetDatum(rd_rel->relallfrozen);
	values[Anum_pg_class_reltoastrelid - 1] = ObjectIdGetDatum(rd_rel->reltoastrelid);
	values[Anum_pg_class_relhasindex - 1] = BoolGetDatum(rd_rel->relhasindex);
	values[Anum_pg_class_relisshared - 1] = BoolGetDatum(rd_rel->relisshared);
	values[Anum_pg_class_relpersistence - 1] = CharGetDatum(rd_rel->relpersistence);
	values[Anum_pg_class_relkind - 1] = CharGetDatum(rd_rel->relkind);
	values[Anum_pg_class_relnatts - 1] = Int16GetDatum(rd_rel->relnatts);
	values[Anum_pg_class_relchecks - 1] = Int16GetDatum(rd_rel->relchecks);
	values[Anum_pg_class_relhasrules - 1] = BoolGetDatum(rd_rel->relhasrules);
	values[Anum_pg_class_relhastriggers - 1] = BoolGetDatum(rd_rel->relhastriggers);
	values[Anum_pg_class_relrowsecurity - 1] = BoolGetDatum(rd_rel->relrowsecurity);
	values[Anum_pg_class_relforcerowsecurity - 1] = BoolGetDatum(rd_rel->relforcerowsecurity);
	values[Anum_pg_class_relhassubclass - 1] = BoolGetDatum(rd_rel->relhassubclass);
	values[Anum_pg_class_relispopulated - 1] = BoolGetDatum(rd_rel->relispopulated);
	values[Anum_pg_class_relreplident - 1] = CharGetDatum(rd_rel->relreplident);
	values[Anum_pg_class_relispartition - 1] = BoolGetDatum(rd_rel->relispartition);
	values[Anum_pg_class_relrewrite - 1] = ObjectIdGetDatum(rd_rel->relrewrite);
	values[Anum_pg_class_relfrozenxid - 1] = TransactionIdGetDatum(rd_rel->relfrozenxid);
	values[Anum_pg_class_relminmxid - 1] = MultiXactIdGetDatum(rd_rel->relminmxid);
	if (relacl != (Datum) 0)
		values[Anum_pg_class_relacl - 1] = relacl;
	else
		nulls[Anum_pg_class_relacl - 1] = true;
	if (reloptions != (Datum) 0)
		values[Anum_pg_class_reloptions - 1] = reloptions;
	else
		nulls[Anum_pg_class_reloptions - 1] = true;

	/* 如有必要，relpartbound 通过更新本元组来设置 */
	nulls[Anum_pg_class_relpartbound - 1] = true;

	tup = heap_form_tuple(RelationGetDescr(pg_class_desc), values, nulls);

	/* 最后插入新元组，更新索引，并清理 */
	CatalogTupleInsert(pg_class_desc, tup);

	heap_freetuple(tup);
}

/* --------------------------------
 *		AddNewRelationTuple
 *
 *		通过向 pg_class 添加元组，将新关系登记到系统目录中。
 * --------------------------------
 */
static void
AddNewRelationTuple(Relation pg_class_desc,
					Relation new_rel_desc,
					Oid new_rel_oid,
					Oid new_type_oid,
					Oid reloftype,
					Oid relowner,
					char relkind,
					TransactionId relfrozenxid,
					TransactionId relminmxid,
					Datum relacl,
					Datum reloptions)
{
	Form_pg_class new_rel_reltup;

	/*
	 * 首先更新我们未编目关系的部分信息于其关系描述符中。
	 */
	new_rel_reltup = new_rel_desc->rd_rel;

	/* 该关系为空 */
	new_rel_reltup->relpages = 0;
	new_rel_reltup->reltuples = -1;
	new_rel_reltup->relallvisible = 0;
	new_rel_reltup->relallfrozen = 0;

	/* 序列总是拥有已知的尺寸 */
	if (relkind == RELKIND_SEQUENCE)
	{
		new_rel_reltup->relpages = 1;
		new_rel_reltup->reltuples = 1;
	}

	new_rel_reltup->relfrozenxid = relfrozenxid;
	new_rel_reltup->relminmxid = relminmxid;
	new_rel_reltup->relowner = relowner;
	new_rel_reltup->reltype = new_type_oid;
	new_rel_reltup->reloftype = reloftype;

	/* relispartition 总是由稍后更新本元组来设置 */
	new_rel_reltup->relispartition = false;

	/* 即便 reltype 为零，也用合理的值填充 rd_att 的类型 ID */
	new_rel_desc->rd_att->tdtypeid = new_type_oid ? new_type_oid : RECORDOID;
	new_rel_desc->rd_att->tdtypmod = -1;

	/* 现在构建并插入元组 */
	InsertPgClassTuple(pg_class_desc, new_rel_desc, new_rel_oid,
					   relacl, reloptions);
}


/* --------------------------------
 *		AddNewRelationType -
 *
 *		定义对应于新关系的组合类型
 * --------------------------------
 */
static ObjectAddress
AddNewRelationType(const char *typeName,
				   Oid typeNamespace,
				   Oid new_rel_oid,
				   char new_rel_kind,
				   Oid ownerid,
				   Oid new_row_type,
				   Oid new_array_type)
{
	return
		TypeCreate(new_row_type,	/* optional predetermined OID */
				   typeName,	/* type name */
				   typeNamespace,	/* type namespace */
				   new_rel_oid, /* relation oid */
				   new_rel_kind,	/* relation kind */
				   ownerid,		/* owner's ID */
				   -1,			/* internal size (varlena) */
				   TYPTYPE_COMPOSITE,	/* type-type (composite) */
				   TYPCATEGORY_COMPOSITE,	/* type-category (ditto) */
				   false,		/* composite types are never preferred */
				   DEFAULT_TYPDELIM,	/* default array delimiter */
				   F_RECORD_IN, /* input procedure */
				   F_RECORD_OUT,	/* output procedure */
				   F_RECORD_RECV,	/* receive procedure */
				   F_RECORD_SEND,	/* send procedure */
				   InvalidOid,	/* typmodin procedure - none */
				   InvalidOid,	/* typmodout procedure - none */
				   InvalidOid,	/* analyze procedure - default */
				   InvalidOid,	/* subscript procedure - none */
				   InvalidOid,	/* array element type - irrelevant */
				   false,		/* this is not an array type */
				   new_array_type,	/* array type if any */
				   InvalidOid,	/* domain base type - irrelevant */
				   NULL,		/* default value - none */
				   NULL,		/* default binary representation */
				   false,		/* passed by reference */
				   TYPALIGN_DOUBLE, /* alignment - must be the largest! */
				   TYPSTORAGE_EXTENDED, /* fully TOASTable */
				   -1,			/* typmod */
				   0,			/* array dimensions for typBaseType */
				   false,		/* Type NOT NULL */
				   InvalidOid); /* rowtypes never have a collation */
}

/* --------------------------------
 *		heap_create_with_catalog
 *
 *		创建一个新编目关系。详见上方注释。
 *
 * 参数:
 *	relname: 赋予新关系的名称
 *	relnamespace: 关系所在命名空间的 OID
 *	reltablespace: 关系所在表空间的 OID
 *	relid: 分配给新关系的 OID，或 InvalidOid 以选择一个新的 OID
 *	reltypeid: 分配给关系行类型的 OID，或 InvalidOid 以选择一个新的
 *	reloftypeid: 若是类型化表，则为底层类型的 OID；否则为 InvalidOid
 *	ownerid: 新关系所有者的 OID
 *	accessmtd: 新关系访问方法的 OID
 *	tupdesc: 元组描述符（列定义的来源）
 *	cooked_constraints: 预加工好的 CHECK 约束与默认值列表
 *	relkind: 新关系的 relkind
 *	relpersistence: 关系的持久化状态（永久、临时或未记录）
 *	shared_relation: 若为共享关系则为 true
 *	mapped_relation: 若关系将使用 relfilenumber 映射则为 true
 *	oncommit: ON COMMIT 标记（仅当它是临时表时才相关）
 *	reloptions: 以 Datum 形式给出的 reloptions，若无则为 (Datum) 0
 *	use_user_acl: 若应查找用户定义的默认权限则为 true；
 *		若为 false，则 relacl 始终设为 NULL
 *	allow_system_table_mods: 若为 true 则允许在系统命名空间中创建
 *	is_internal: 这是否是一个系统生成的编目？
 *	relrewrite: 表重写期间指向原始关系的链接
 *
 * 输出参数:
 *	typaddress: 若非 NULL，则接收新 pg_type 项的对象地址
 *	（若该 relkind 不会获得 pg_type 项，则必须为 NULL）
 *
 * 返回新关系的 OID
 * --------------------------------
 */
Oid
heap_create_with_catalog(const char *relname,
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
						 ObjectAddress *typaddress)
{
	Relation	pg_class_desc;
	Relation	new_rel_desc;
	Acl		   *relacl;
	Oid			existing_relid;
	Oid			old_type_oid;
	Oid			new_type_oid;

	/* 默认设为 InvalidOid，除非被 binary-upgrade 覆盖 */
	RelFileNumber relfilenumber = InvalidRelFileNumber;
	TransactionId relfrozenxid;
	MultiXactId relminmxid;

	pg_class_desc = table_open(RelationRelationId, RowExclusiveLock);

	/*
	 * 合理性检查
	 */
	Assert(IsNormalProcessingMode() || IsBootstrapProcessingMode());

	/*
	 * 为期望的 relkind 校验提议的 tupdesc。若开启了
	 * allow_system_table_mods，则允许使用 ANYARRAY；这是一个
	 * 用于在 VACUUM FULL 期间创建并克隆 pg_statistic 的 hack。
	 */
	CheckAttributeNamesTypes(tupdesc, relkind,
							 allow_system_table_mods ? CHKATYPE_ANYARRAY : 0);

	/*
	 * 若关系已存在，这无论如何稍后也会失败。但在此处捕获它，
	 * 我们便能给出更友好的错误消息。
	 */
	existing_relid = get_relname_relid(relname, relnamespace);
	if (existing_relid != InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_TABLE),
				 errmsg("relation \"%s\" already exists", relname)));

	/*
	 * 由于我们还要创建一个行类型，因此也要检查是否与已有的
	 * 类型名冲突。若冲突对象是一个自动生成的数组类型，我们可以将其
	 * 重命名让开；否则我们至少能给出一条良好的错误消息。
	 */
	old_type_oid = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
								   CStringGetDatum(relname),
								   ObjectIdGetDatum(relnamespace));
	if (OidIsValid(old_type_oid))
	{
		if (!moveArrayTypeName(old_type_oid, relname, relnamespace))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("type \"%s\" already exists", relname),
					 errhint("A relation has an associated type of the same name, "
							 "so you must use a name that doesn't conflict "
							 "with any existing type.")));
	}

	/*
	 * 共享关系必须位于 pg_global 中(最后一道检查)
	 */
	if (shared_relation && reltablespace != GLOBALTABLESPACE_OID)
		elog(ERROR, "shared relations must be placed in pg_global tablespace");

	/*
	 * 为关系分配一个 OID，除非已被告知使用哪个。
	 *
	 * 该 OID 也将作为 relfilenumber，因此要确保它既不与 pg_class 的
	 * OID 冲突，也不与已有的物理文件冲突。
	 */
	if (!OidIsValid(relid))
	{
		/* 为 pg_class.oid 与 relfilenumber 使用 binary-upgrade 覆盖值 */
		if (IsBinaryUpgrade)
		{
			/*
			 * 此处不支持索引；它们使用
			 * binary_upgrade_next_index_pg_class_oid。
			 */
			Assert(relkind != RELKIND_INDEX);
			Assert(relkind != RELKIND_PARTITIONED_INDEX);

			if (relkind == RELKIND_TOASTVALUE)
			{
				/* 可能并不存在 TOAST 表，因此我们必须先测试它是否存在。 */
				if (OidIsValid(binary_upgrade_next_toast_pg_class_oid))
				{
					relid = binary_upgrade_next_toast_pg_class_oid;
					binary_upgrade_next_toast_pg_class_oid = InvalidOid;

					if (!RelFileNumberIsValid(binary_upgrade_next_toast_pg_class_relfilenumber))
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								 errmsg("toast relfilenumber value not set when in binary upgrade mode")));

					relfilenumber = binary_upgrade_next_toast_pg_class_relfilenumber;
					binary_upgrade_next_toast_pg_class_relfilenumber = InvalidRelFileNumber;
				}
			}
			else
			{
				if (!OidIsValid(binary_upgrade_next_heap_pg_class_oid))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("pg_class heap OID value not set when in binary upgrade mode")));

				relid = binary_upgrade_next_heap_pg_class_oid;
				binary_upgrade_next_heap_pg_class_oid = InvalidOid;

				if (RELKIND_HAS_STORAGE(relkind))
				{
					if (!RelFileNumberIsValid(binary_upgrade_next_heap_pg_class_relfilenumber))
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								 errmsg("relfilenumber value not set when in binary upgrade mode")));

					relfilenumber = binary_upgrade_next_heap_pg_class_relfilenumber;
					binary_upgrade_next_heap_pg_class_relfilenumber = InvalidRelFileNumber;
				}
			}
		}

		if (!OidIsValid(relid))
			relid = GetNewRelFileNumber(reltablespace, pg_class_desc,
										relpersistence);
	}

	/*
	 * 在其他会话提交之前，它们的目录扫描都无法找到本关系。因此，
	 * 持有 AccessExclusiveLock 并无害处。在此处加锁，调用者就无法
	 * 在锁模式或获取时机上意外地发生改变。
	 */
	LockRelationOid(relid, AccessExclusiveLock);

	/*
	 * 确定关系的初始权限。
	 */
	if (use_user_acl)
	{
		switch (relkind)
		{
			case RELKIND_RELATION:
			case RELKIND_VIEW:
			case RELKIND_MATVIEW:
			case RELKIND_FOREIGN_TABLE:
			case RELKIND_PARTITIONED_TABLE:
				relacl = get_user_default_acl(OBJECT_TABLE, ownerid,
											  relnamespace);
				break;
			case RELKIND_SEQUENCE:
				relacl = get_user_default_acl(OBJECT_SEQUENCE, ownerid,
											  relnamespace);
				break;
			default:
				relacl = NULL;
				break;
		}
	}
	else
		relacl = NULL;

	/*
	 * 创建 relcache 条目(目前基本是空壳)与物理磁盘文件。
	 * (若后续失败，移除磁盘文件是 smgr 的责任。)
	 *
	 * 注意：即便对于二进制升级，传入 create_storage = true 也是正确的。
	 * 我们在此创建的存储稍后会被替换，但在此期间磁盘上需要有内容。
	 */
	new_rel_desc = heap_create(relname,
							   relnamespace,
							   reltablespace,
							   relid,
							   relfilenumber,
							   accessmtd,
							   tupdesc,
							   relkind,
							   relpersistence,
							   shared_relation,
							   mapped_relation,
							   allow_system_table_mods,
							   &relfrozenxid,
							   &relminmxid,
							   true);

	Assert(relid == RelationGetRelid(new_rel_desc));

	new_rel_desc->rd_rel->relrewrite = relrewrite;

	/*
	 * 决定是否要为关系的行类型创建一个 pg_type 条目。
	 * 这些类型都会被创建，除非关系的使用仅仅是个实现细节：
	 * toast 表、序列和索引。
	 */
	if (!(relkind == RELKIND_SEQUENCE ||
		  relkind == RELKIND_TOASTVALUE ||
		  relkind == RELKIND_INDEX ||
		  relkind == RELKIND_PARTITIONED_INDEX))
	{
		Oid			new_array_oid;
		ObjectAddress new_type_addr;
		char	   *relarrayname;

		/*
		 * 我们也会创建该组合类型之上的数组类型。主要出于
		 * 历史原因，数组类型的 OID 先被分配。
		 */
		new_array_oid = AssignTypeArrayOid();

		/*
		 * 为组合类型创建 pg_type 条目。组合类型的 OID 可由调用者
		 * 预先选定，但若 reltypeid 为 InvalidOid，我们会为其生成
		 * 一个新的 OID。
		 *
		 * 注意：这里可能会遇到唯一索引冲突，因为可能另有某个会话
		 * 在并行创建相同的类型名，而我们在上方检查重名时它尚未提交。
		 */
		new_type_addr = AddNewRelationType(relname,
										   relnamespace,
										   relid,
										   relkind,
										   ownerid,
										   reltypeid,
										   new_array_oid);
		new_type_oid = new_type_addr.objectId;
		if (typaddress)
			*typaddress = new_type_addr;

		/* 现在创建数组类型。 */
		relarrayname = makeArrayTypeName(relname, relnamespace);

		TypeCreate(new_array_oid,	/* force the type's OID to this */
				   relarrayname,	/* Array type name */
				   relnamespace,	/* Same namespace as parent */
				   InvalidOid,	/* Not composite, no relationOid */
				   0,			/* relkind, also N/A here */
				   ownerid,		/* owner's ID */
				   -1,			/* Internal size (varlena) */
				   TYPTYPE_BASE,	/* Not composite - typelem is */
				   TYPCATEGORY_ARRAY,	/* type-category (array) */
				   false,		/* array types are never preferred */
				   DEFAULT_TYPDELIM,	/* default array delimiter */
				   F_ARRAY_IN,	/* array input proc */
				   F_ARRAY_OUT, /* array output proc */
				   F_ARRAY_RECV,	/* array recv (bin) proc */
				   F_ARRAY_SEND,	/* array send (bin) proc */
				   InvalidOid,	/* typmodin procedure - none */
				   InvalidOid,	/* typmodout procedure - none */
				   F_ARRAY_TYPANALYZE,	/* array analyze procedure */
				   F_ARRAY_SUBSCRIPT_HANDLER,	/* array subscript procedure */
				   new_type_oid,	/* array element type - the rowtype */
				   true,		/* yes, this is an array type */
				   InvalidOid,	/* this has no array type */
				   InvalidOid,	/* domain base type - irrelevant */
				   NULL,		/* default value - none */
				   NULL,		/* default binary representation */
				   false,		/* passed by reference */
				   TYPALIGN_DOUBLE, /* alignment - must be the largest! */
				   TYPSTORAGE_EXTENDED, /* fully TOASTable */
				   -1,			/* typmod */
				   0,			/* array dimensions for typBaseType */
				   false,		/* Type NOT NULL */
				   InvalidOid); /* rowtypes never have a collation */

		pfree(relarrayname);
	}
	else
	{
		/* 调用者不应期望会创建一个类型。 */
		Assert(reltypeid == InvalidOid);
		Assert(typaddress == NULL);

		new_type_oid = InvalidOid;
	}

	/*
	 * 现在为关系在 pg_class 中创建一条条目。
	 *
	 * 注意：这里可能会遇到唯一索引冲突，因为可能另有某个会话
	 * 在并行创建相同的关系名，而我们在上方检查重名时它尚未提交。
	 */
	AddNewRelationTuple(pg_class_desc,
						new_rel_desc,
						relid,
						new_type_oid,
						reloftypeid,
						ownerid,
						relkind,
						relfrozenxid,
						relminmxid,
						PointerGetDatum(relacl),
						reloptions);

	/*
	 * 现在为我们新关系的属性向 pg_attribute 添加元组。
	 */
	AddNewAttributeTuples(relid, new_rel_desc->rd_att, relkind);

	/*
	 * 建立依赖链接，使得当其命名空间被删除时，该关系也被强制删除。
	 * 同时建立指向其所有者的依赖链接，以及针对默认 ACL 中
	 * 提到的任何角色的依赖。
	 *
	 * 对于组合类型，这些依赖是针对 pg_type 条目跟踪的，
	 * 因此这里无需记录。同样地，TOAST 表不需要命名空间依赖
	 * (它们位于固定的命名空间中)，也不需要所有者依赖
	 * (它们通过父表间接依赖)，也不应有任何 ACL 条目。
	 * 扩展依赖也同理。
	 *
	 * 另外，在 bootstrap 模式下跳过此步，因为我们不会在
	 * bootstrapping 期间建立依赖。
	 */
	if (relkind != RELKIND_COMPOSITE_TYPE &&
		relkind != RELKIND_TOASTVALUE &&
		!IsBootstrapProcessingMode())
	{
		ObjectAddress myself,
					referenced;
		ObjectAddresses *addrs;

		ObjectAddressSet(myself, RelationRelationId, relid);

		recordDependencyOnOwner(RelationRelationId, relid, ownerid);

		recordDependencyOnNewAcl(RelationRelationId, relid, 0, ownerid, relacl);

		recordDependencyOnCurrentExtension(&myself, false);

		addrs = new_object_addresses();

		ObjectAddressSet(referenced, NamespaceRelationId, relnamespace);
		add_exact_object_address(&referenced, addrs);

		if (reloftypeid)
		{
			ObjectAddressSet(referenced, TypeRelationId, reloftypeid);
			add_exact_object_address(&referenced, addrs);
		}

		/*
		 * 建立依赖链接，使得当关系的访问方法被删除时，
		 * 该关系也被强制删除。
		 *
		 * 无需为 toast 表添加显式依赖，因为主表依赖于它。
		 * 分区表可能没有设置访问方法。
		 */
		if ((RELKIND_HAS_TABLE_AM(relkind) && relkind != RELKIND_TOASTVALUE) ||
			(relkind == RELKIND_PARTITIONED_TABLE && OidIsValid(accessmtd)))
		{
			ObjectAddressSet(referenced, AccessMethodRelationId, accessmtd);
			add_exact_object_address(&referenced, addrs);
		}

		record_object_address_dependencies(&myself, addrs, DEPENDENCY_NORMAL);
		free_object_addresses(addrs);
	}

	/* 针对新关系的创建后钩子 */
	InvokeObjectPostCreateHookArg(RelationRelationId, relid, 0, is_internal);

	/*
	 * 存储任何提供的 CHECK 约束与默认值。
	 *
	 * 注意：这可能会执行 CommandCounterIncrement 并重建 relcache
	 * 条目，因此关系到此必须有效且自洽。尤其重要的是，此刻任何地方
	 * 都还没有约束与默认值。
	 */
	StoreConstraints(new_rel_desc, cooked_constraints, is_internal);

	/*
	 * 若存在特殊的 on-commit 动作，则记录它
	 */
	if (oncommit != ONCOMMIT_NOOP)
		register_on_commit_action(relid, oncommit);

	/*
	 * 好了，关系已被编目，因此关闭我们的关系并返回
	 * 新创建关系的 OID。
	 */
	table_close(new_rel_desc, NoLock);	/* do not unlock till end of xact */
	table_close(pg_class_desc, RowExclusiveLock);

	return relid;
}

/*
 *		RelationRemoveInheritance
 *
 * 以往，本例程会检查子关系，若发现任何子关系则中止删除。
 * 现在我们依赖依赖机制来检查或删除子关系。等到执行到这里时，
 * 已经没有任何子关系，我们只需移除将这个关系链接到其
 * 父关系的任何 pg_inherits 行。
 */
static void
RelationRemoveInheritance(Oid relid)
{
	Relation	catalogRelation;
	SysScanDesc scan;
	ScanKeyData key;
	HeapTuple	tuple;

	catalogRelation = table_open(InheritsRelationId, RowExclusiveLock);

	ScanKeyInit(&key,
				Anum_pg_inherits_inhrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));

	scan = systable_beginscan(catalogRelation, InheritsRelidSeqnoIndexId, true,
							  NULL, 1, &key);

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
		CatalogTupleDelete(catalogRelation, &tuple->t_self);

	systable_endscan(scan);
	table_close(catalogRelation, RowExclusiveLock);
}

/*
 *		DeleteRelationTuple
 *
 * 移除给定 relid 对应的 pg_class 行。
 *
 * 注意：本例程由关系删除与索引删除共用，不打算用于任何其他地方。
 */
void
DeleteRelationTuple(Oid relid)
{
	Relation	pg_class_desc;
	HeapTuple	tup;

	/* 在 pg_class 关系上获取适当的锁 */
	pg_class_desc = table_open(RelationRelationId, RowExclusiveLock);

	tup = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for relation %u", relid);

	/* 从 pg_class 中删除关系元组，并收尾 */
	CatalogTupleDelete(pg_class_desc, &tup->t_self);

	ReleaseSysCache(tup);

	table_close(pg_class_desc, RowExclusiveLock);
}

/*
 *		DeleteAttributeTuples
 *
 * 移除给定 relid 对应的 pg_attribute 行。
 *
 * 注意：本例程由关系删除与索引删除共用，不打算用于任何其他地方。
 */
void
DeleteAttributeTuples(Oid relid)
{
	Relation	attrel;
	SysScanDesc scan;
	ScanKeyData key[1];
	HeapTuple	atttup;

	/* 在 pg_attribute 关系上获取适当的锁 */
	attrel = table_open(AttributeRelationId, RowExclusiveLock);

	/* 使用索引仅扫描目标关系的属性 */
	ScanKeyInit(&key[0],
				Anum_pg_attribute_attrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));

	scan = systable_beginscan(attrel, AttributeRelidNumIndexId, true,
							  NULL, 1, key);

	/* 删除所有匹配的元组 */
	while ((atttup = systable_getnext(scan)) != NULL)
		CatalogTupleDelete(attrel, &atttup->t_self);

	/* 扫描结束后清理 */
	systable_endscan(scan);
	table_close(attrel, RowExclusiveLock);
}

/*
 *		DeleteSystemAttributeTuples
 *
 * 移除给定 relid 的系统列对应的 pg_attribute 行。
 *
 * 注意：本例程仅用于将表转换为视图时使用。视图没有
 * 系统列，因此我们应当将其从 pg_attribute 中移除。
 */
void
DeleteSystemAttributeTuples(Oid relid)
{
	Relation	attrel;
	SysScanDesc scan;
	ScanKeyData key[2];
	HeapTuple	atttup;

	/* 在 pg_attribute 关系上获取适当的锁 */
	attrel = table_open(AttributeRelationId, RowExclusiveLock);

	/* 使用索引仅扫描目标关系的系统属性 */
	ScanKeyInit(&key[0],
				Anum_pg_attribute_attrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	ScanKeyInit(&key[1],
				Anum_pg_attribute_attnum,
				BTLessEqualStrategyNumber, F_INT2LE,
				Int16GetDatum(0));

	scan = systable_beginscan(attrel, AttributeRelidNumIndexId, true,
							  NULL, 2, key);

	/* 删除所有匹配的元组 */
	while ((atttup = systable_getnext(scan)) != NULL)
		CatalogTupleDelete(attrel, &atttup->t_self);

	/* 扫描结束后清理 */
	systable_endscan(scan);
	table_close(attrel, RowExclusiveLock);
}

/*
 *		RemoveAttributeById
 *
 * 这是 ALTER TABLE DROP COLUMN 的核心：真正在 pg_attribute 中
 * 将该属性标记为已删除。我们还会移除它的 pg_statistic 条目。
 * (其余所需工作，例如清除任何 pg_attrdef 条目，
 * 由 dependency.c 处理。)
 */
void
RemoveAttributeById(Oid relid, AttrNumber attnum)
{
	Relation	rel;
	Relation	attr_rel;
	HeapTuple	tuple;
	Form_pg_attribute attStruct;
	char		newattname[NAMEDATALEN];
	Datum		valuesAtt[Natts_pg_attribute] = {0};
	bool		nullsAtt[Natts_pg_attribute] = {0};
	bool		replacesAtt[Natts_pg_attribute] = {0};

	/*
	 * 在目标表上获取排他锁，我们将一直持有到事务结束。
	 * (在直接删除本列的简单情况下，ATExecDropColumn 已经做过这件事……；
	 * 但当从删除其他对象级联而来时，我们可能没有任何锁。)
	 */
	rel = relation_open(relid, AccessExclusiveLock);

	attr_rel = table_open(AttributeRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopy2(ATTNUM,
								ObjectIdGetDatum(relid),
								Int16GetDatum(attnum));
	if (!HeapTupleIsValid(tuple))	/* shouldn't happen */
		elog(ERROR, "cache lookup failed for attribute %d of relation %u",
			 attnum, relid);
	attStruct = (Form_pg_attribute) GETSTRUCT(tuple);

	/* 将该属性标记为已删除 */
	attStruct->attisdropped = true;

	/*
	 * 将类型 OID 设为无效。被删除属性的类型链接不可信赖
	 * (一旦属性被删除，其类型也可能随之消失)。所幸我们并不需要
	 * 类型行——唯一真正必要的信息是类型的 typlen 和 typalign，
	 * 它们保存在属性的 attlen 和 attalign 中。我们在此将 atttypid
	 * 置零，以此捕获那些错误地期望它仍然有效的代码。
	 */
	attStruct->atttypid = InvalidOid;

	/* 移除该列可能带有的任何 NOT NULL 约束 */
	attStruct->attnotnull = false;

	/* 清除此项，以免有人尝试查找生成表达式 */
	attStruct->attgenerated = '\0';

	/*
	 * 将列名改为一个不太可能冲突的名称
	 */
	snprintf(newattname, sizeof(newattname),
			 "........pg.dropped.%d........", attnum);
	namestrcpy(&(attStruct->attname), newattname);

	/* 清除缺失值 */
	attStruct->atthasmissing = false;
	nullsAtt[Anum_pg_attribute_attmissingval - 1] = true;
	replacesAtt[Anum_pg_attribute_attmissingval - 1] = true;

	/*
	 * 清除其他可空字段。这能在 pg_attribute 中节省一些空间，
	 * 并移除不再有用的信息。
	 */
	nullsAtt[Anum_pg_attribute_attstattarget - 1] = true;
	replacesAtt[Anum_pg_attribute_attstattarget - 1] = true;
	nullsAtt[Anum_pg_attribute_attacl - 1] = true;
	replacesAtt[Anum_pg_attribute_attacl - 1] = true;
	nullsAtt[Anum_pg_attribute_attoptions - 1] = true;
	replacesAtt[Anum_pg_attribute_attoptions - 1] = true;
	nullsAtt[Anum_pg_attribute_attfdwoptions - 1] = true;
	replacesAtt[Anum_pg_attribute_attfdwoptions - 1] = true;

	tuple = heap_modify_tuple(tuple, RelationGetDescr(attr_rel),
							  valuesAtt, nullsAtt, replacesAtt);

	CatalogTupleUpdate(attr_rel, &tuple->t_self, tuple);

	/*
	 * 因为更新 pg_attribute 行会触发目标关系的 relcache 刷新，
	 * 我们无需再做其他事情来通知其他后端这一变更。
	 */

	table_close(attr_rel, RowExclusiveLock);

	RemoveStatistics(relid, attnum);

	relation_close(rel, NoLock);
}

/*
 * heap_drop_with_catalog	- 从系统目录中移除指定关系
 *
 * 注意，本例程不负责删除那些通过依赖链接到 pg_class 条目的对象
 * (例如索引和约束)。这些对象在控制权到达这里之前，已由
 * dependency.c 中的依赖追踪逻辑删除。因此一般而言，本例程
 * 不应被直接调用；请改为经由 performDeletion() 调用。
 */
void
heap_drop_with_catalog(Oid relid)
{
	Relation	rel;
	HeapTuple	tuple;
	Oid			parentOid = InvalidOid,
				defaultPartOid = InvalidOid;

	/*
	 * 为了安全地删除一个分区，我们必须在它的父表上获取排他锁，
	 * 因为另一个后端可能正要在父表上执行查询。若它依赖之前缓存的
	 * 分区描述符，就可能试图访问刚刚删除的关系作为其分区。
	 * 因此我们必须在提交之前获取足够强的表锁，阻止该表上的所有查询
	 * 继续进行，直到我们发出共享缓存失效通知，使它们更新各自的分区
	 * 描述符。
	 */
	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relid);
	if (((Form_pg_class) GETSTRUCT(tuple))->relispartition)
	{
		/*
		 * 若分区正在被 detach，我们必须锁定其父表，
		 * 因为可能仍有某个查询持有包含本分区的
		 * 分区描述符。
		 */
		parentOid = get_partition_parent(relid, true);
		LockRelationOid(parentOid, AccessExclusiveLock);

		/*
		 * 若这不是默认分区，删除它会改变默认分区的
		 * 分区约束，因此我们必须锁定它。
		 */
		defaultPartOid = get_default_partition_oid(parentOid);
		if (OidIsValid(defaultPartOid) && relid != defaultPartOid)
			LockRelationOid(defaultPartOid, AccessExclusiveLock);
	}

	ReleaseSysCache(tuple);

	/*
	 * 打开并锁定该关系。
	 */
	rel = relation_open(relid, AccessExclusiveLock);

	/*
	 * 不会再有其他会话触碰该关系，但在我们自己的会话中
	 * 仍可能有打开的查询或游标，或待处理的触发器事件。
	 */
	CheckTableNotInUse(rel, "DROP TABLE");

	/*
	 * 这会有效地删除表中的所有行，且可能在一个可串行化事务中执行。
	 * 在这种情况下，我们必须为每个持有该表谓词锁的事务，
	 * 记录一条指向本事务的读写冲突。
	 */
	CheckTableForSerializableConflictIn(rel);

	/*
	 * 首先删除 pg_foreign_table 元组。
	 */
	if (rel->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
	{
		Relation	ftrel;
		HeapTuple	fttuple;

		ftrel = table_open(ForeignTableRelationId, RowExclusiveLock);

		fttuple = SearchSysCache1(FOREIGNTABLEREL, ObjectIdGetDatum(relid));
		if (!HeapTupleIsValid(fttuple))
			elog(ERROR, "cache lookup failed for foreign table %u", relid);

		CatalogTupleDelete(ftrel, &fttuple->t_self);

		ReleaseSysCache(fttuple);
		table_close(ftrel, RowExclusiveLock);
	}

	/*
	 * 若是分区表，删除 pg_partitioned_table 元组。
	 */
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		RemovePartitionKeyByRelId(relid);

	/*
	 * 若被删除的关系本身就是默认分区，
	 * 则使其 pg_partitioned_table 中的条目失效。
	 */
	if (relid == defaultPartOid)
		update_default_partition_oid(parentOid, InvalidOid);

	/*
	 * 安排在提交时解除该关系物理文件的链接。
	 */
	if (RELKIND_HAS_STORAGE(rel->rd_rel->relkind))
		RelationDropStorage(rel);

	/* 确保若事务提交则丢弃统计信息 */
	pgstat_drop_relation(rel);

	/*
	 * 关闭 relcache 条目，但*保留*该关系上的 AccessExclusiveLock
	 * 直到事务提交。这确保不会有其他会话试图对这个注定被删除的
	 * 关系做些什么。
	 */
	relation_close(rel, NoLock);

	/*
	 * 移除任何关联的 relation 同步状态。
	 */
	RemoveSubscriptionRel(InvalidOid, relid);

	/*
	 * 忘记该关系的任何 ON COMMIT 动作
	 */
	remove_on_commit_action(relid);

	/*
	 * 将关系从 relcache 中刷除。我们希望在着手移除目录条目之前
	 * 完成此操作，以确信不会在过程中途发生 relcache 条目重建。
	 * (这其实本应无关紧要，因为我们在此不做
	 * CommandCounterIncrement，但为了安全还是这么做。)
	 */
	RelationForgetRelation(relid);

	/*
	 * 移除继承信息
	 */
	RelationRemoveInheritance(relid);

	/*
	 * 删除统计信息
	 */
	RemoveStatistics(relid, 0);

	/*
	 * 删除属性元组
	 */
	DeleteAttributeTuples(relid);

	/*
	 * 删除关系元组
	 */
	DeleteRelationTuple(relid);

	if (OidIsValid(parentOid))
	{
		/*
		 * 若这不是默认分区，默认分区的分区约束已经改变，
		 * 以纳入原本由被删除分区覆盖的那部分键空间。
		 */
		if (OidIsValid(defaultPartOid) && relid != defaultPartOid)
			CacheInvalidateRelcacheByRelid(defaultPartOid);

		/*
		 * 使父表的 relcache 失效，使该分区不再
		 * 被包含在其分区描述符中。
		 */
		CacheInvalidateRelcacheByRelid(parentOid);
		/* 保留该锁 */
	}
}


/*
 * RelationClearMissing
 *
 * 对所有当前已设置 atthasmissing 和 attmissingval 的属性，将它们
 * 设为 false/null。若表被重写(例如经 VACUUM FULL 或 CLUSTER)，
 * 我们可以安全且有效地这样做，因为我们知道不会再有行具有
 * 少于完整数量的属性。
 *
 * 调用者必须持有该关系的 AccessExclusive 锁。
 */
void
RelationClearMissing(Relation rel)
{
	Relation	attr_rel;
	Oid			relid = RelationGetRelid(rel);
	int			natts = RelationGetNumberOfAttributes(rel);
	int			attnum;
	Datum		repl_val[Natts_pg_attribute];
	bool		repl_null[Natts_pg_attribute];
	bool		repl_repl[Natts_pg_attribute];
	Form_pg_attribute attrtuple;
	HeapTuple	tuple,
				newtuple;

	memset(repl_val, 0, sizeof(repl_val));
	memset(repl_null, false, sizeof(repl_null));
	memset(repl_repl, false, sizeof(repl_repl));

	repl_val[Anum_pg_attribute_atthasmissing - 1] = BoolGetDatum(false);
	repl_null[Anum_pg_attribute_attmissingval - 1] = true;

	repl_repl[Anum_pg_attribute_atthasmissing - 1] = true;
	repl_repl[Anum_pg_attribute_attmissingval - 1] = true;


	/* 获取 pg_attribute 上的锁 */
	attr_rel = table_open(AttributeRelationId, RowExclusiveLock);

	/* 处理每个非系统属性，包括任何已删除的列 */
	for (attnum = 1; attnum <= natts; attnum++)
	{
		tuple = SearchSysCache2(ATTNUM,
								ObjectIdGetDatum(relid),
								Int16GetDatum(attnum));
		if (!HeapTupleIsValid(tuple))	/* shouldn't happen */
			elog(ERROR, "cache lookup failed for attribute %d of relation %u",
				 attnum, relid);

		attrtuple = (Form_pg_attribute) GETSTRUCT(tuple);

		/* 忽略任何 atthasmissing 不为 true 的属性 */
		if (attrtuple->atthasmissing)
		{
			newtuple = heap_modify_tuple(tuple, RelationGetDescr(attr_rel),
										 repl_val, repl_null, repl_repl);

			CatalogTupleUpdate(attr_rel, &newtuple->t_self, newtuple);

			heap_freetuple(newtuple);
		}

		ReleaseSysCache(tuple);
	}

	/*
	 * 我们对 pg_attribute 行的更新会强制进行一次 relcache 重建，
	 * 因此这里无需再做其他事情。
	 */
	table_close(attr_rel, RowExclusiveLock);
}

/*
 * StoreAttrMissingVal
 *
 * 设置单个属性的缺失值。
 */
void
StoreAttrMissingVal(Relation rel, AttrNumber attnum, Datum missingval)
{
	Datum		valuesAtt[Natts_pg_attribute] = {0};
	bool		nullsAtt[Natts_pg_attribute] = {0};
	bool		replacesAtt[Natts_pg_attribute] = {0};
	Relation	attrrel;
	Form_pg_attribute attStruct;
	HeapTuple	atttup,
				newtup;

	/* 仅支持普通表 */
	Assert(rel->rd_rel->relkind == RELKIND_RELATION);

	/* 获取 pg_attribute 行 */
	attrrel = table_open(AttributeRelationId, RowExclusiveLock);

	atttup = SearchSysCache2(ATTNUM,
							 ObjectIdGetDatum(RelationGetRelid(rel)),
							 Int16GetDatum(attnum));
	if (!HeapTupleIsValid(atttup))	/* shouldn't happen */
		elog(ERROR, "cache lookup failed for attribute %d of relation %u",
			 attnum, RelationGetRelid(rel));
	attStruct = (Form_pg_attribute) GETSTRUCT(atttup);

	/* 构造一个包含该值的单元素数组 */
	missingval = PointerGetDatum(construct_array(&missingval,
												 1,
												 attStruct->atttypid,
												 attStruct->attlen,
												 attStruct->attbyval,
												 attStruct->attalign));

	/* 更新 pg_attribute 行 */
	valuesAtt[Anum_pg_attribute_atthasmissing - 1] = BoolGetDatum(true);
	replacesAtt[Anum_pg_attribute_atthasmissing - 1] = true;

	valuesAtt[Anum_pg_attribute_attmissingval - 1] = missingval;
	replacesAtt[Anum_pg_attribute_attmissingval - 1] = true;

	newtup = heap_modify_tuple(atttup, RelationGetDescr(attrrel),
							   valuesAtt, nullsAtt, replacesAtt);
	CatalogTupleUpdate(attrrel, &newtup->t_self, newtup);

	/* 清理 */
	ReleaseSysCache(atttup);
	table_close(attrrel, RowExclusiveLock);
}

/*
 * SetAttrMissing
 *
 * 设置单个属性的缺失值。这应仅由二进制升级使用。会对
 * 拥有该属性的关系取 AccessExclusive 锁。
 */
void
SetAttrMissing(Oid relid, char *attname, char *value)
{
	Datum		valuesAtt[Natts_pg_attribute] = {0};
	bool		nullsAtt[Natts_pg_attribute] = {0};
	bool		replacesAtt[Natts_pg_attribute] = {0};
	Datum		missingval;
	Form_pg_attribute attStruct;
	Relation	attrrel,
				tablerel;
	HeapTuple	atttup,
				newtup;

	/* 锁定该属性所属的表 */
	tablerel = table_open(relid, AccessExclusiveLock);

	/* 除非是普通表，否则什么都不做 */
	if (tablerel->rd_rel->relkind != RELKIND_RELATION)
	{
		table_close(tablerel, AccessExclusiveLock);
		return;
	}

	/* 锁定属性行并获取数据 */
	attrrel = table_open(AttributeRelationId, RowExclusiveLock);
	atttup = SearchSysCacheAttName(relid, attname);
	if (!HeapTupleIsValid(atttup))
		elog(ERROR, "cache lookup failed for attribute %s of relation %u",
			 attname, relid);
	attStruct = (Form_pg_attribute) GETSTRUCT(atttup);

	/* 从值字符串获取一个数组值 */
	missingval = OidFunctionCall3(F_ARRAY_IN,
								  CStringGetDatum(value),
								  ObjectIdGetDatum(attStruct->atttypid),
								  Int32GetDatum(attStruct->atttypmod));

	/* 更新元组 - 设置 atthasmissing 与 attmissingval */
	valuesAtt[Anum_pg_attribute_atthasmissing - 1] = BoolGetDatum(true);
	replacesAtt[Anum_pg_attribute_atthasmissing - 1] = true;
	valuesAtt[Anum_pg_attribute_attmissingval - 1] = missingval;
	replacesAtt[Anum_pg_attribute_attmissingval - 1] = true;

	newtup = heap_modify_tuple(atttup, RelationGetDescr(attrrel),
							   valuesAtt, nullsAtt, replacesAtt);
	CatalogTupleUpdate(attrrel, &newtup->t_self, newtup);

	/* 清理 */
	ReleaseSysCache(atttup);
	table_close(attrrel, RowExclusiveLock);
	table_close(tablerel, AccessExclusiveLock);
}

/*
 * 为给定关系存储一个 CHECK 约束表达式。
 * 为给定关系存储一个 CHECK 约束表达式。
 *
 * 调用者负责更新该关系的 pg_class 条目中
 * 约束的数量。
 *
 * 返回新约束的 OID。
 */
static Oid
StoreRelCheck(Relation rel, const char *ccname, Node *expr,
			  bool is_enforced, bool is_validated, bool is_local,
			  int16 inhcount, bool is_no_inherit, bool is_internal)
{
	char	   *ccbin;
	List	   *varList;
	int			keycount;
	int16	   *attNos;
	Oid			constrOid;

	/*
	 * 将表达式扁平化为字符串形式以便存储。
	 */
	ccbin = nodeToString(expr);

	/*
	 * 找出 expr 中使用的 rel 的列
	 *
	 * 注意：pull_var_clause 在此处可用，仅因为我们不允许 CHECK 约束中
	 * 出现子查询；否则它将无法检查子查询的内容。
	 */
	varList = pull_var_clause(expr, 0);
	keycount = list_length(varList);

	if (keycount > 0)
	{
		ListCell   *vl;
		int			i = 0;

		attNos = (int16 *) palloc(keycount * sizeof(int16));
		foreach(vl, varList)
		{
			Var		   *var = (Var *) lfirst(vl);
			int			j;

			for (j = 0; j < i; j++)
				if (attNos[j] == var->varattno)
					break;
			if (j == i)
				attNos[i++] = var->varattno;
		}
		keycount = i;
	}
	else
		attNos = NULL;

	/*
	 * 分区表自身不包含任何行，因此 NO INHERIT
	 * 约束毫无意义。
	 */
	if (is_no_inherit &&
		rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("cannot add NO INHERIT constraint to partitioned table \"%s\"",
						RelationGetRelationName(rel))));

	/*
	 * 创建 CHECK 约束
	 */
	constrOid =
		CreateConstraintEntry(ccname,	/* Constraint Name */
							  RelationGetNamespace(rel),	/* namespace */
							  CONSTRAINT_CHECK, /* Constraint Type */
							  false,	/* Is Deferrable */
							  false,	/* Is Deferred */
							  is_enforced,	/* Is Enforced */
							  is_validated,
							  InvalidOid,	/* no parent constraint */
							  RelationGetRelid(rel),	/* relation */
							  attNos,	/* attrs in the constraint */
							  keycount, /* # key attrs in the constraint */
							  keycount, /* # total attrs in the constraint */
							  InvalidOid,	/* not a domain constraint */
							  InvalidOid,	/* no associated index */
							  InvalidOid,	/* Foreign key fields */
							  NULL,
							  NULL,
							  NULL,
							  NULL,
							  0,
							  ' ',
							  ' ',
							  NULL,
							  0,
							  ' ',
							  NULL, /* not an exclusion constraint */
							  expr, /* Tree form of check constraint */
							  ccbin,	/* Binary form of check constraint */
							  is_local, /* conislocal */
							  inhcount, /* coninhcount */
							  is_no_inherit,	/* connoinherit */
							  false,	/* conperiod */
							  is_internal); /* internally constructed? */

	pfree(ccbin);

	return constrOid;
}

/*
 * 为给定关系存储一个 NOT NULL 约束。
 * 为给定关系存储一个 NOT NULL 约束。
 *
 * 返回新约束的 OID。
 */
static Oid
StoreRelNotNull(Relation rel, const char *nnname, AttrNumber attnum,
				bool is_validated, bool is_local, int inhcount,
				bool is_no_inherit)
{
	Oid			constrOid;

	Assert(attnum > InvalidAttrNumber);

	constrOid =
		CreateConstraintEntry(nnname,
							  RelationGetNamespace(rel),
							  CONSTRAINT_NOTNULL,
							  false,
							  false,
							  true, /* Is Enforced */
							  is_validated,
							  InvalidOid,
							  RelationGetRelid(rel),
							  &attnum,
							  1,
							  1,
							  InvalidOid,	/* not a domain constraint */
							  InvalidOid,	/* no associated index */
							  InvalidOid,	/* Foreign key fields */
							  NULL,
							  NULL,
							  NULL,
							  NULL,
							  0,
							  ' ',
							  ' ',
							  NULL,
							  0,
							  ' ',
							  NULL, /* not an exclusion constraint */
							  NULL,
							  NULL,
							  is_local,
							  inhcount,
							  is_no_inherit,
							  false,
							  false);
	return constrOid;
}

/*
 * 存储默认值与 CHECK 约束(以 CookedConstraint 列表形式传入)。
 *
 * 每个 CookedConstraint 结构体都会被修改，以存入新的目录元组 OID。
 *
 * 注意：仅预加工的(cooked)表达式会以这种方式传入，也就是说，
 * 是从已有关系继承而来的表达式。新解析的表达式可以稍后通过
 * 直接调用 StoreAttrDefault 和 StoreRelCheck 来添加
 * (见 AddRelationNewConstraints())。
 */
static void
StoreConstraints(Relation rel, List *cooked_constraints, bool is_internal)
{
	int			numchecks = 0;
	ListCell   *lc;

	if (cooked_constraints == NIL)
		return;					/* nothing to do */

	/*
	 * 除非让刚创建的 pg_attribute 元组对本关系可见，否则约束表达式的
	 * 反解析会失败。因此，递增命令计数器。注意：这将导致 relcache
	 * 条目重建。
	 */
	CommandCounterIncrement();

	foreach(lc, cooked_constraints)
	{
		CookedConstraint *con = (CookedConstraint *) lfirst(lc);

		switch (con->contype)
		{
			case CONSTR_DEFAULT:
				con->conoid = StoreAttrDefault(rel, con->attnum, con->expr,
											   is_internal);
				break;
			case CONSTR_CHECK:
				con->conoid =
					StoreRelCheck(rel, con->name, con->expr,
								  con->is_enforced, !con->skip_validation,
								  con->is_local, con->inhcount,
								  con->is_no_inherit, is_internal);
				numchecks++;
				break;

			default:
				elog(ERROR, "unrecognized constraint type: %d",
					 (int) con->contype);
		}
	}

	if (numchecks > 0)
		SetRelationNumChecks(rel, numchecks);
}

/*
 * AddRelationNewConstraints
 *
 * 向一个已有关系添加新的列默认表达式和/或约束检查表达式。
 * 这里定义为两者都做，是为了在 DefineRelation 中提高效率，
 * 但当然你可以通过传入空列表而只做其中一件事。
 *
 * rel: 待修改的关系
 * newColDefaults: RawColumnDefault 结构体列表
 * newConstraints: Constraint 节点列表
 * allow_merge: 若为 true，CHECK 约束可与其已有的约束合并
 * is_local: 若定义为本地则为 true，若为继承则为 false
 * is_internal: 若为某些内部过程(而非用户请求)的结果则为 true
 * queryString: 在默认值和已加工的 CHECK 约束的表达式转换期间使用
 *
 * newColDefaults 中的所有条目都会被处理。newConstraints 中的条目
 * 仅当其为 CONSTR_CHECK 或 CONSTR_NOTNULL 类型时才会被处理。
 *
 * 返回一个 CookedConstraint 节点列表，展示添加到该关系的
 * 默认与约束表达式的加工后形式。
 *
 * 注意：调用者应当以某种自冲突的锁模式打开 rel，并在事务结束前
 * 一直持有该锁；普通情况下应为 AccessExclusiveLock，但如果调用者
 * 知道该约束已由其他手段强制执行，则可以是 ShareUpdateExclusiveLock。
 * 此外，我们假设调用者已在必要时执行了 CommandCounterIncrement，
 * 以使该关系的目录元组可见。
 */
List *
AddRelationNewConstraints(Relation rel,
						  List *newColDefaults,
						  List *newConstraints,
						  bool allow_merge,
						  bool is_local,
						  bool is_internal,
						  const char *queryString)
{
	List	   *cookedConstraints = NIL;
	TupleDesc	tupleDesc;
	TupleConstr *oldconstr;
	int			numoldchecks;
	ParseState *pstate;
	ParseNamespaceItem *nsitem;
	int			numchecks;
	List	   *checknames;
	List	   *nnnames;
	Node	   *expr;
	CookedConstraint *cooked;

	/*
	 * 获取已有约束的信息。
	 */
	tupleDesc = RelationGetDescr(rel);
	oldconstr = tupleDesc->constr;
	if (oldconstr)
		numoldchecks = oldconstr->num_check;
	else
		numoldchecks = 0;

	/*
	 * 创建一个虚拟的 ParseState，并将目标关系作为它唯一的
	 * 范围表项插入。transformExpr 需要一个 ParseState。
	 */
	pstate = make_parsestate(NULL);
	pstate->p_sourcetext = queryString;
	nsitem = addRangeTableEntryForRelation(pstate,
										   rel,
										   AccessShareLock,
										   NULL,
										   false,
										   true);
	addNSItemToQuery(pstate, nsitem, true, true, true);

	/*
	 * 处理列默认表达式。
	 */
	foreach_ptr(RawColumnDefault, colDef, newColDefaults)
	{
		Form_pg_attribute atp = TupleDescAttr(rel->rd_att, colDef->attnum - 1);
		Oid			defOid;

		expr = cookDefault(pstate, colDef->raw_default,
						   atp->atttypid, atp->atttypmod,
						   NameStr(atp->attname),
						   atp->attgenerated);

		/*
		 * 若表达式仅仅是一个 NULL 常量，我们就不必去创建一个显式的
		 * pg_attrdef 条目，因为默认行为与之等价。这适用于列默认值，
		 * 但不适用于生成表达式。
		 *
		 * 注意本测试一个不显见的性质：若列的类型是域类型，我们得到的
		 * 就不是一个裸的 null Const，而是一个 CoerceToDomain 表达式，
		 * 因此我们不会丢弃该默认值。这很关键，因为列的默认值需要被保留，
		 * 以覆盖该域可能拥有的任何默认值。
		 */
		if (expr == NULL ||
			(!colDef->generated &&
			 IsA(expr, Const) &&
			 castNode(Const, expr)->constisnull))
			continue;

		defOid = StoreAttrDefault(rel, colDef->attnum, expr, is_internal);

		cooked = (CookedConstraint *) palloc(sizeof(CookedConstraint));
		cooked->contype = CONSTR_DEFAULT;
		cooked->conoid = defOid;
		cooked->name = NULL;
		cooked->attnum = colDef->attnum;
		cooked->expr = expr;
		cooked->is_enforced = true;
		cooked->skip_validation = false;
		cooked->is_local = is_local;
		cooked->inhcount = is_local ? 0 : 1;
		cooked->is_no_inherit = false;
		cookedConstraints = lappend(cookedConstraints, cooked);
	}

	/*
	 * 处理约束表达式。
	 */
	numchecks = numoldchecks;
	checknames = NIL;
	nnnames = NIL;
	foreach_node(Constraint, cdef, newConstraints)
	{
		Oid			constrOid;

		if (cdef->contype == CONSTR_CHECK)
		{
			char	   *ccname;

			if (cdef->raw_expr != NULL)
			{
				Assert(cdef->cooked_expr == NULL);

				/*
				 * 将原始解析树转换为可执行表达式，并
				 * 验证其作为 CHECK 约束的有效性。
				 */
				expr = cookConstraint(pstate, cdef->raw_expr,
									  RelationGetRelationName(rel));
			}
			else
			{
				Assert(cdef->cooked_expr != NULL);

				/*
				 * 在这里，我们假设解析器只会向我们传入有效的 CHECK
				 * 表达式，因此不做特别的检查。
				 */
				expr = stringToNode(cdef->cooked_expr);
			}

		/*
		 * 检查名称唯一性，若未给定名称则生成一个。
		 */
		if (cdef->conname != NULL)
			{
				ccname = cdef->conname;
				/* 与其它新约束进行比对 */
				/* 需要这样做，因为循环中我们没有执行 CommandCounterIncrement */
				foreach_ptr(char, chkname, checknames)
				{
					if (strcmp(chkname, ccname) == 0)
						ereport(ERROR,
								(errcode(ERRCODE_DUPLICATE_OBJECT),
								 errmsg("check constraint \"%s\" already exists",
										ccname)));
				}

				/* 保存名称以备后续检查 */
				checknames = lappend(checknames, ccname);

				/*
				 * 与已有的约束进行比对。若允许与某个已有约束合并，
				 * 则此处无需再做其他事情。(我们会从结果中省略这条重复的
				 * 约束，这正是 ATAddCheckNNConstraint 所期望的。)
				 */
				if (MergeWithExistingConstraint(rel, ccname, expr,
												allow_merge, is_local,
												cdef->is_enforced,
												cdef->initially_valid,
												cdef->is_no_inherit))
					continue;
			}
			else
			{
				/*
				 * 生成名称时，我们希望为列约束创建 "tab_col_check"，
				 * 为表约束创建 "tab_check"。我们不再拥有关于约束短语
				 * 语法位置的任何信息，因此通过观察表达式是否引用了
				 * 多个列来近似判断。(如果用户遵守规则，结果是一样的……)
				 *
				 * 注意：pull_var_clause() 不会下钻到子链接中，但我们在
				 * 上面已经消除了那些；而且无论如何这里只需要一个
				 * 近似的答案。
				 */
				List	   *vars;
				char	   *colname;

				vars = pull_var_clause(expr, 0);

				/* 消除重复项 */
				vars = list_union(NIL, vars);

				if (list_length(vars) == 1)
					colname = get_attname(RelationGetRelid(rel),
										  ((Var *) linitial(vars))->varattno,
										  true);
				else
					colname = NULL;

				ccname = ChooseConstraintName(RelationGetRelationName(rel),
											  colname,
											  "check",
											  RelationGetNamespace(rel),
											  checknames);

				/* 保存名称以备后续检查 */
				checknames = lappend(checknames, ccname);
			}

			/*
			 * 好了，存储它。
			 */
			constrOid =
				StoreRelCheck(rel, ccname, expr, cdef->is_enforced,
							  cdef->initially_valid, is_local,
							  is_local ? 0 : 1, cdef->is_no_inherit,
							  is_internal);

			numchecks++;

			cooked = (CookedConstraint *) palloc(sizeof(CookedConstraint));
			cooked->contype = CONSTR_CHECK;
			cooked->conoid = constrOid;
			cooked->name = ccname;
			cooked->attnum = 0;
			cooked->expr = expr;
			cooked->is_enforced = cdef->is_enforced;
			cooked->skip_validation = cdef->skip_validation;
			cooked->is_local = is_local;
			cooked->inhcount = is_local ? 0 : 1;
			cooked->is_no_inherit = cdef->is_no_inherit;
			cookedConstraints = lappend(cookedConstraints, cooked);
		}
		else if (cdef->contype == CONSTR_NOTNULL)
		{
			CookedConstraint *nncooked;
			AttrNumber	colnum;
			int16		inhcount = is_local ? 0 : 1;
			char	   *nnname;

			/* 确定要修改哪一列 */
			colnum = get_attnum(RelationGetRelid(rel), strVal(linitial(cdef->keys)));
			if (colnum == InvalidAttrNumber)
				ereport(ERROR,
						errcode(ERRCODE_UNDEFINED_COLUMN),
						errmsg("column \"%s\" of relation \"%s\" does not exist",
							   strVal(linitial(cdef->keys)), RelationGetRelationName(rel)));
			if (colnum < InvalidAttrNumber)
				ereport(ERROR,
						errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("cannot add not-null constraint on system column \"%s\"",
							   strVal(linitial(cdef->keys))));

			Assert(cdef->initially_valid != cdef->skip_validation);

			/*
			 * 若该列已经有一个 NOT NULL 约束，我们不想
			 * 再添加另一个；按需调整继承状态。这也会检查
			 * 已有约束是否与所请求的有效性相匹配。
			 */
			if (AdjustNotNullInheritance(RelationGetRelid(rel), colnum,
										 cdef->conname,
										 is_local, cdef->is_no_inherit,
										 cdef->skip_validation))
				continue;

			/*
			 * 若指定了约束名，则检查它尚未被使用。
			 * 否则，我们自己选一个不冲突的名称。
			 */
			if (cdef->conname)
			{
				if (ConstraintNameIsUsed(CONSTRAINT_RELATION,
										 RelationGetRelid(rel),
										 cdef->conname))
					ereport(ERROR,
							errcode(ERRCODE_DUPLICATE_OBJECT),
							errmsg("constraint \"%s\" for relation \"%s\" already exists",
								   cdef->conname, RelationGetRelationName(rel)));
				nnname = cdef->conname;
			}
			else
				nnname = ChooseConstraintName(RelationGetRelationName(rel),
											  strVal(linitial(cdef->keys)),
											  "not_null",
											  RelationGetNamespace(rel),
											  nnnames);
			nnnames = lappend(nnnames, nnname);

			constrOid =
				StoreRelNotNull(rel, nnname, colnum,
								cdef->initially_valid,
								is_local,
								inhcount,
								cdef->is_no_inherit);

			nncooked = (CookedConstraint *) palloc(sizeof(CookedConstraint));
			nncooked->contype = CONSTR_NOTNULL;
			nncooked->conoid = constrOid;
			nncooked->name = nnname;
			nncooked->attnum = colnum;
			nncooked->expr = NULL;
			nncooked->is_enforced = true;
			nncooked->skip_validation = cdef->skip_validation;
			nncooked->is_local = is_local;
			nncooked->inhcount = inhcount;
			nncooked->is_no_inherit = cdef->is_no_inherit;

			cookedConstraints = lappend(cookedConstraints, nncooked);
		}
	}

	/*
	 * 更新该关系 pg_class 元组中的约束数量。即使没有任何变化我们也这样做，
	 * 以确保为 pg_class 元组发出一条 SI 更新消息，从而强制其他后端
	 * 重建它们对该关系的 relcache 条目。(如果我们添加了默认值但没添加
	 * 约束，这一点至关重要。)
	 */
	SetRelationNumChecks(rel, numchecks);

	return cookedConstraints;
}

/*
 * 检查是否存在与提议的新 CHECK 约束冲突的已有 CHECK 约束，
 * 并视情况调整其 conislocal/coninhcount 设置或抛出错误。
 *
 * 若合并成功(约束为重复)则返回 true，若得到一个迄今唯一的名称
 * 则返回 false，若冲突则抛出错误。
 *
 * 注意：若你修改这段代码，也请一并查看 MergeConstraintsIntoExisting。
 */
static bool
MergeWithExistingConstraint(Relation rel, const char *ccname, Node *expr,
							bool allow_merge, bool is_local,
							bool is_enforced,
							bool is_initially_valid,
							bool is_no_inherit)
{
	bool		found;
	Relation	conDesc;
	SysScanDesc conscan;
	ScanKeyData skey[3];
	HeapTuple	tup;

	/* 查找同名且同关系的 pg_constraint 条目 */
	conDesc = table_open(ConstraintRelationId, RowExclusiveLock);

	found = false;

	ScanKeyInit(&skey[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));
	ScanKeyInit(&skey[1],
				Anum_pg_constraint_contypid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(InvalidOid));
	ScanKeyInit(&skey[2],
				Anum_pg_constraint_conname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(ccname));

	conscan = systable_beginscan(conDesc, ConstraintRelidTypidNameIndexId, true,
								 NULL, 3, skey);

	/* 最多只能有一个匹配的行 */
	if (HeapTupleIsValid(tup = systable_getnext(conscan)))
	{
		Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tup);

		/* 找到了。若不是相同的 CHECK 约束则冲突 */
		if (con->contype == CONSTRAINT_CHECK)
		{
			Datum		val;
			bool		isnull;

			val = fastgetattr(tup,
							  Anum_pg_constraint_conbin,
							  conDesc->rd_att, &isnull);
			if (isnull)
				elog(ERROR, "null conbin for rel %s",
					 RelationGetRelationName(rel));
			if (equal(expr, stringToNode(TextDatumGetCString(val))))
				found = true;
		}

		/*
		 * 若已有约束纯粹是继承而来的(没有本地定义)，则将添加一个
		 * 本地约束解释为一次合法的合并。这允许对父表和子表
		 * 的 ALTER ADD CONSTRAINT 以任意顺序给出，最终状态相同。
		 * 但如果关系是分区，所有继承来的约束始终是非本地的，
		 * 包括那些已被合并的。
		 */
		if (is_local && !con->conislocal && !rel->rd_rel->relispartition)
			allow_merge = true;

		if (!found || !allow_merge)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("constraint \"%s\" for relation \"%s\" already exists",
							ccname, RelationGetRelationName(rel))));

		/* 若子约束是 "no inherit"，则不能合并 */
		if (con->connoinherit)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("constraint \"%s\" conflicts with non-inherited constraint on relation \"%s\"",
							ccname, RelationGetRelationName(rel))));

		/*
		 * 绝不能将已有的继承约束改为 "no inherit" 状态。
		 * 这是因为继承约束应当能够传播到更低层级的子表。
		 */
		if (con->coninhcount > 0 && is_no_inherit)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("constraint \"%s\" conflicts with inherited constraint on relation \"%s\"",
							ccname, RelationGetRelationName(rel))));

		/*
		 * 若子约束是 "not valid"，则不能与有效的父约束合并。
		 */
		if (is_initially_valid && con->conenforced && !con->convalidated)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("constraint \"%s\" conflicts with NOT VALID constraint on relation \"%s\"",
							ccname, RelationGetRelationName(rel))));

		/*
		 * 非强制的子约束不能与强制的父约束合并。但反向是允许的，
		 * 即子约束为强制时。
		 */
		if ((!is_local && is_enforced && !con->conenforced) ||
			(is_local && !is_enforced && con->conenforced))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("constraint \"%s\" conflicts with NOT ENFORCED constraint on relation \"%s\"",
							ccname, RelationGetRelationName(rel))));

		/* 可以更新该元组 */
		ereport(NOTICE,
				(errmsg("merging constraint \"%s\" with inherited definition",
						ccname)));

		tup = heap_copytuple(tup);
		con = (Form_pg_constraint) GETSTRUCT(tup);

		/*
		 * 对于分区，继承约束必须只被继承一次，因为它不可能有多个
		 * 父表，且永远不会被视为本地的。
		 */
		if (rel->rd_rel->relispartition)
		{
			con->coninhcount = 1;
			con->conislocal = false;
		}
		else
		{
			if (is_local)
				con->conislocal = true;
			else if (pg_add_s16_overflow(con->coninhcount, 1,
										 &con->coninhcount))
				ereport(ERROR,
						errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						errmsg("too many inheritance parents"));
		}

		if (is_no_inherit)
		{
			Assert(is_local);
			con->connoinherit = true;
		}

		/*
		 * 若要求子约束被强制，而父约束未被强制，则应当允许，
		 * 方法是将子约束标记为强制。反向的情况则会在到达此点之前
		 * 就已经抛出错误。
		 */
		if (is_enforced && !con->conenforced)
		{
			Assert(is_local);
			con->conenforced = true;
			con->convalidated = true;
		}

		CatalogTupleUpdate(conDesc, &tup->t_self, tup);
	}

	systable_endscan(conscan);
	table_close(conDesc, RowExclusiveLock);

	return found;
}

/*
 * 在创建新关系时创建 NOT NULL 约束
 *
 * 这些约束来自两个来源：'constraints' 列表(Constraint)由用户
 * 直接指定；'old_notnulls' 列表(CookedConstraint)来自继承。
 * 我们为每一列创建一个约束，优先采用用户指定的，并按导致
 * 每一列获得 NOT NULL 约束的父表数量设置 inhcount。
 * 若用户指定的名称与另一个用户指定的名称冲突，则抛出错误。
 * 'existing_constraints' 是已定义约束名称的列表，在生成
 * 更多约束时应避免使用这些名称。
 *
 * 返回一个 AttrNumber 列表，列出需要设置 attnotnull 标志的列。
 */
List *
AddRelationNotNullConstraints(Relation rel, List *constraints,
							  List *old_notnulls, List *existing_constraints)
{
	List	   *givennames;
	List	   *nnnames;
	List	   *nncols = NIL;

	/*
	 * 我们维护两个名称列表：nnnames 保存所有约束名，
	 * givennames 跟踪用户生成的名称。这一区分很重要，
	 * 因为对于用户生成的名称冲突我们必须报错，而对于
	 * 系统生成的名称冲突，我们只需再生成一个。
	 */
	nnnames = list_copy(existing_constraints);	/* don't scribble on input */
	givennames = NIL;

	/*
	 * 首先，创建所有由用户直接指定的 NOT NULL 约束。注意，继承可能
	 * 为每一个都提供了另一个来源，因此我们必须扫描 old_notnulls 列表，
	 * 并为每个具有相同 attnum 的元素递增 inhcount。我们会从那里
	 * 删除任何已被处理的元素。
	 *
	 * 这里不使用 foreach()，因为我们有两层嵌套循环遍历约束列表，
	 * 内层可能会删除元素。如果我们使用 foreach_delete_current()，
	 * 它只能修复其中一个循环的状态，因此对于两个循环都使用基于
	 * 列表索引的循环看起来更清晰。注意任何删除都会发生在外层循环
	 * 当前位置之后，因此外层循环的索引永远无需调整。
	 */
	for (int outerpos = 0; outerpos < list_length(constraints); outerpos++)
	{
		Constraint *constr;
		AttrNumber	attnum;
		char	   *conname;
		int			inhcount = 0;

		constr = list_nth_node(Constraint, constraints, outerpos);

		Assert(constr->contype == CONSTR_NOTNULL);

		attnum = get_attnum(RelationGetRelid(rel),
							strVal(linitial(constr->keys)));
		if (attnum == InvalidAttrNumber)
			ereport(ERROR,
					errcode(ERRCODE_UNDEFINED_COLUMN),
					errmsg("column \"%s\" of relation \"%s\" does not exist",
						   strVal(linitial(constr->keys)),
						   RelationGetRelationName(rel)));
		if (attnum < InvalidAttrNumber)
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("cannot add not-null constraint on system column \"%s\"",
						   strVal(linitial(constr->keys))));

		/*
		 * 一列只能有一个 NOT NULL 约束，因此丢弃任何针对我们
		 * 已见过列的额外约束；但要检查 NO INHERIT 标志是否匹配。
		 */
		for (int restpos = outerpos + 1; restpos < list_length(constraints);)
		{
			Constraint *other;

			other = list_nth_node(Constraint, constraints, restpos);
			if (strcmp(strVal(linitial(constr->keys)),
					   strVal(linitial(other->keys))) == 0)
			{
				if (other->is_no_inherit != constr->is_no_inherit)
					ereport(ERROR,
							errcode(ERRCODE_SYNTAX_ERROR),
							errmsg("conflicting NO INHERIT declaration for not-null constraint on column \"%s\"",
								   strVal(linitial(constr->keys))));

		/*
		 * 若指定了约束名则保留它，但若指定了相互冲突的
		 * 名称则抛出错误。
		 */
				if (other->conname)
				{
					if (!constr->conname)
						constr->conname = pstrdup(other->conname);
					else if (strcmp(constr->conname, other->conname) != 0)
						ereport(ERROR,
								errcode(ERRCODE_SYNTAX_ERROR),
								errmsg("conflicting not-null constraint names \"%s\" and \"%s\"",
									   constr->conname, other->conname));
				}

				/* 我们还需要验证其他字段吗？ */
				constraints = list_delete_nth_cell(constraints, restpos);
			}
			else
				restpos++;
		}

		/*
		 * 在继承约束列表中搜索同一列上的任何条目；并据此确定一个
		 * 继承计数。此外，若至少有一个父表对该列有约束，我们就不能
		 * 接受用户关于 NO INHERIT 的指定。我们在此处理的任何来自
		 * 父表的约束都会从列表中删除：我们不再需要在下面的循环中
		 * 处理它。
		 */
		foreach_ptr(CookedConstraint, old, old_notnulls)
		{
			if (old->attnum == attnum)
			{
			/*
			 * 若我们从父表得到一个约束，那么拥有本地的 NO
			 * INHERIT 约束是行不通的。
			 */
			if (constr->is_no_inherit)
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("cannot define not-null constraint with NO INHERIT on column \"%s\"",
									strVal(linitial(constr->keys))),
							 errdetail("The column has an inherited not-null constraint.")));

				inhcount++;
				old_notnulls = foreach_delete_current(old_notnulls, old);
			}
		}

		/*
		 * 确定一个约束名称，它可能已由用户指定；若与另一个
		 * 用户指定的名称存在冲突，则报错。
		 */
		if (constr->conname)
		{
			foreach_ptr(char, thisname, givennames)
			{
				if (strcmp(thisname, constr->conname) == 0)
					ereport(ERROR,
							errcode(ERRCODE_DUPLICATE_OBJECT),
							errmsg("constraint \"%s\" for relation \"%s\" already exists",
								   constr->conname,
								   RelationGetRelationName(rel)));
			}

			conname = constr->conname;
			givennames = lappend(givennames, conname);
		}
		else
			conname = ChooseConstraintName(RelationGetRelationName(rel),
										   get_attname(RelationGetRelid(rel),
													   attnum, false),
										   "not_null",
										   RelationGetNamespace(rel),
										   nnnames);
		nnnames = lappend(nnnames, conname);

		StoreRelNotNull(rel, conname,
						attnum, true, true,
						inhcount, constr->is_no_inherit);

		nncols = lappend_int(nncols, attnum);
	}

	/*
	 * 若 old_notnulls 列表中仍有任何列残留，我们必须为那一列创建一个
	 * 标记为 non-local 的 NOT NULL 约束。因为多个父表都可能对同一列
	 * 指定 NOT NULL 约束，我们必须统计有多少个，并设置相应的
	 * inhcount，同时删除已经处理过的元素。
	 *
	 * 这里不使用 foreach()，因为我们有两层嵌套循环遍历约束列表，
	 * 内层可能会删除元素。如果我们使用 foreach_delete_current()，
	 * 它只能修复其中一个循环的状态，因此对于两个循环都使用基于
	 * 列表索引的循环看起来更清晰。注意任何删除都会发生在外层循环
	 * 当前位置之后，因此外层循环的索引永远无需调整。
	 */
	for (int outerpos = 0; outerpos < list_length(old_notnulls); outerpos++)
	{
		CookedConstraint *cooked;
		char	   *conname = NULL;
		int			inhcount = 1;

		cooked = (CookedConstraint *) list_nth(old_notnulls, outerpos);
		Assert(cooked->contype == CONSTR_NOTNULL);
		Assert(cooked->name);

		/*
		 * 保留我们遇到的第一个不冲突的约束名称。
		 */
		if (conname == NULL)
			conname = cooked->name;

		for (int restpos = outerpos + 1; restpos < list_length(old_notnulls);)
		{
			CookedConstraint *other;

			other = (CookedConstraint *) list_nth(old_notnulls, restpos);
			Assert(other->name);
			if (other->attnum == cooked->attnum)
			{
				if (conname == NULL)
					conname = other->name;

				inhcount++;
				old_notnulls = list_delete_nth_cell(old_notnulls, restpos);
			}
			else
				restpos++;
		}

		/* 若我们得到了一个名称，确保它不是已经用过的 */
		if (conname != NULL)
		{
			foreach_ptr(char, thisname, nnnames)
			{
				if (strcmp(thisname, conname) == 0)
				{
					conname = NULL;
					break;
				}
			}
		}

		/* 并在需要时选择一个名称 */
		if (conname == NULL)
			conname = ChooseConstraintName(RelationGetRelationName(rel),
										   get_attname(RelationGetRelid(rel),
													   cooked->attnum, false),
										   "not_null",
										   RelationGetNamespace(rel),
										   nnnames);
		nnnames = lappend(nnnames, conname);

		/* 忽略源约束的 is_local 与 inhcount */
		StoreRelNotNull(rel, conname, cooked->attnum, true,
						false, inhcount, false);

		nncols = lappend_int(nncols, cooked->attnum);
	}

	return nncols;
}

/*
 * 更新该关系 pg_class 元组中的约束数量。
 *
 * 调用者最好持有该关系的排他锁。
 *
 * 一个重要的副作用是，会为 pg_class 元组发出一条 SI 更新消息，
 * 从而强制其他后端重建它们对该关系的 relcache 条目。此外，
 * 本后端会在下一次 CommandCounterIncrement 时重建它自己的
 * relcache 条目。
 */
static void
SetRelationNumChecks(Relation rel, int numchecks)
{
	Relation	relrel;
	HeapTuple	reltup;
	Form_pg_class relStruct;

	relrel = table_open(RelationRelationId, RowExclusiveLock);
	reltup = SearchSysCacheCopy1(RELOID,
								 ObjectIdGetDatum(RelationGetRelid(rel)));
	if (!HeapTupleIsValid(reltup))
		elog(ERROR, "cache lookup failed for relation %u",
			 RelationGetRelid(rel));
	relStruct = (Form_pg_class) GETSTRUCT(reltup);

	if (relStruct->relchecks != numchecks)
	{
		relStruct->relchecks = numchecks;

		CatalogTupleUpdate(relrel, &reltup->t_self, reltup);
	}
	else
	{
		/* 跳过磁盘更新，但仍强制令 relcache 失效 */
		CacheInvalidateRelcache(rel);
	}

	heap_freetuple(reltup);
	table_close(relrel, RowExclusiveLock);
}

/*
 * 检查对生成列的引用
 */
static bool
check_nested_generated_walker(Node *node, void *context)
{
	ParseState *pstate = context;

	if (node == NULL)
		return false;
	else if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;
		Oid			relid;
		AttrNumber	attnum;

		relid = rt_fetch(var->varno, pstate->p_rtable)->relid;
		if (!OidIsValid(relid))
			return false;		/* 我们是否应该抛出错误？ */

		attnum = var->varattno;

		if (attnum > 0 && get_attgenerated(relid, attnum))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("cannot use generated column \"%s\" in column generation expression",
							get_attname(relid, attnum, false)),
					 errdetail("A generated column cannot reference another generated column."),
					 parser_errposition(pstate, var->location)));
		/* 整行 Var 必然是自我引用的，因此禁止它 */
		if (attnum == 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("cannot use whole-row variable in column generation expression"),
					 errdetail("This would cause the generated column to depend on its own value."),
					 parser_errposition(pstate, var->location)));
		/* 系统列已在解析器中检查过 */

		return false;
	}
	else
		return expression_tree_walker(node, check_nested_generated_walker,
									  context);
}

static void
check_nested_generated(ParseState *pstate, Node *node)
{
	check_nested_generated_walker(node, pstate);
}

/*
 * 检查虚拟生成列表达式的安全性。
 *
 * 就像从视图中选择数据可能被利用(CVE-2024-7348)一样，从带有
 * 虚拟生成列的表中选择数据也可能被利用。关注此问题的用户可以
 * 避免从视图中选择，但告诉他们避免从表中选择则不切实际。
 *
 * 为了解决这个问题，这里将虚拟生成列的生成表达式限制为只能
 * 使用内置函数和类型。我们假设内置函数和类型无法被用于此目的。
 * 注意，整体的安全性还要求所使用的所有函数都是不可变的。
 * (例如，存在一些可以执行任意 SQL 的内置非不可变函数。)
 * 不可变性在别处检查，因为这是一种与安全考量无关、
 * 必须成立的属性。
 *
 * 将来，可以通过某种新机制来声明其他函数与类型在此用途下是
 * 安全或可信的，从而扩展本功能，但这尚待设计。
 */

/*
 * 供 check_functions_in_node() 使用的回调，用于判断一个函数
 * 是否为用户定义的。
 */
static bool
contains_user_functions_checker(Oid func_id, void *context)
{
	return (func_id >= FirstUnpinnedObjectId);
}

/*
 * 出于安全原因，检查虚拟生成列生成表达式中所有我们不希望
 * 出现的内容。一旦发现就抛出错误。
 */
static bool
check_virtual_generated_security_walker(Node *node, void *context)
{
	ParseState *pstate = context;

	if (node == NULL)
		return false;

	if (!IsA(node, List))
	{
		if (check_functions_in_node(node, contains_user_functions_checker, NULL))
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("generation expression uses user-defined function"),
					errdetail("Virtual generated columns that make use of user-defined functions are not yet supported."),
					parser_errposition(pstate, exprLocation(node)));

		/*
		 * check_functions_in_node() 不检查某些节点类型(参见
		 * 那里的注释)。我们通过检查内置类型来处理 CoerceToDomain
		 * 和 MinMaxExpr。其他列出的节点类型无法调用可由用户定义的
		 * SQL 可见函数。
		 *
		 * 此外，我们还需要这一类型检查来处理诸如 array_eq() 这样的
		 * 内置、不可变的多态函数。
		 */
		if (exprType(node) >= FirstUnpinnedObjectId)
			ereport(ERROR,
					errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("generation expression uses user-defined type"),
					errdetail("Virtual generated columns that make use of user-defined types are not yet supported."),
					parser_errposition(pstate, exprLocation(node)));
	}

	return expression_tree_walker(node, check_virtual_generated_security_walker, context);
}

static void
check_virtual_generated_security(ParseState *pstate, Node *node)
{
	check_virtual_generated_security_walker(node, pstate);
}

/*
 * 取一个原始默认值，并将其转换为可用于存储的加工后格式。
 *
 * 解析状态应当设置好，以识别表达式中可能出现的任何变量。
 * (尽管我们计划拒绝变量，但给出正确的错误消息比"未知变量"
 * 更加对用户友好。)
 *
 * 若 atttypid 不是 InvalidOid，则将表达式强制转换为指定的
 * 类型(以及 typmod atttypmod)。attname 仅在此情况下需要：
 * 它用于(若有)错误消息中。
 */
Node *
cookDefault(ParseState *pstate,
			Node *raw_default,
			Oid atttypid,
			int32 atttypmod,
			const char *attname,
			char attgenerated)
{
	Node	   *expr;

	Assert(raw_default != NULL);

	/*
	 * 将原始解析树转换为可执行表达式。
	 */
	expr = transformExpr(pstate, raw_default, attgenerated ? EXPR_KIND_GENERATED_COLUMN : EXPR_KIND_COLUMN_DEFAULT);

	if (attgenerated)
	{
		/* 禁止引用其他生成列 */
		check_nested_generated(pstate, expr);

		/* 禁止可变函数 */
		if (contain_mutable_functions_after_planning((Expr *) expr))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("generation expression is not immutable")));

		/* 检查虚拟生成列表达式的安全性 */
		if (attgenerated == ATTRIBUTE_GENERATED_VIRTUAL)
			check_virtual_generated_security(pstate, expr);
	}
	else
	{
		/*
		 * 对于默认表达式，transformExpr() 应当已经拒绝了
		 * 列引用。
		 */
		Assert(!contain_var_clause(expr));
	}

	/*
	 * 若给定，将表达式强制转换为正确的类型与 typmod。这应当
	 * 与解析器对非默认值表达式的处理保持一致——参见
	 * transformAssignedExpr()。
	 */
	if (OidIsValid(atttypid))
	{
		Oid			type_id = exprType(expr);

		expr = coerce_to_target_type(pstate, expr, type_id,
									 atttypid, atttypmod,
									 COERCION_ASSIGNMENT,
									 COERCE_IMPLICIT_CAST,
									 -1);
		if (expr == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("column \"%s\" is of type %s"
							" but default expression is of type %s",
							attname,
							format_type_be(atttypid),
							format_type_be(type_id)),
					 errhint("You will need to rewrite or cast the expression.")));
	}

	/*
	 * 最后，处理已完成表达式中的排序规则。
	 */
	assign_expr_collations(pstate, expr);

	return expr;
}

/*
 * 取一个原始 CHECK 约束表达式，并将其转换为可用于存储的
 * 加工后格式。
 *
 * 解析状态必须设置好，以识别表达式中可能出现的任何变量。
 */
static Node *
cookConstraint(ParseState *pstate,
			   Node *raw_constraint,
			   char *relname)
{
	Node	   *expr;

	/*
	 * 将原始解析树转换为可执行表达式。
	 */
	expr = transformExpr(pstate, raw_constraint, EXPR_KIND_CHECK_CONSTRAINT);

	/*
	 * 确保它产生一个布尔结果。
	 */
	expr = coerce_to_boolean(pstate, expr, "CHECK");

	/*
	 * 处理排序规则。
	 */
	assign_expr_collations(pstate, expr);

	/*
	 * 确保没有引用外部关系(既然 add_missing_from 已成为历史，
	 * 这大概已经是死代码了)。
	 */
	if (list_length(pstate->p_rtable) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("only table \"%s\" can be referenced in check constraint",
						relname)));

	return expr;
}

/*
 * CopyStatistics --- 将 pg_statistic 中的条目从一关系复制到另一关系
 */
void
CopyStatistics(Oid fromrelid, Oid torelid)
{
	HeapTuple	tup;
	SysScanDesc scan;
	ScanKeyData key[1];
	Relation	statrel;
	CatalogIndexState indstate = NULL;

	statrel = table_open(StatisticRelationId, RowExclusiveLock);

	/* 现在查找统计记录 */
	ScanKeyInit(&key[0],
				Anum_pg_statistic_starelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(fromrelid));

	scan = systable_beginscan(statrel, StatisticRelidAttnumInhIndexId,
							  true, NULL, 1, key);

	while (HeapTupleIsValid((tup = systable_getnext(scan))))
	{
		Form_pg_statistic statform;

		/* 制作一个可修改的副本 */
		tup = heap_copytuple(tup);
		statform = (Form_pg_statistic) GETSTRUCT(tup);

		/* 更新元组的副本并插入它 */
		statform->starelid = torelid;

		/* 在确知需要时才获取索引信息 */
		if (indstate == NULL)
			indstate = CatalogOpenIndexes(statrel);

		CatalogTupleInsertWithInfo(statrel, tup, indstate);

		heap_freetuple(tup);
	}

	systable_endscan(scan);

	if (indstate != NULL)
		CatalogCloseIndexes(indstate);
	table_close(statrel, RowExclusiveLock);
}

/*
 * RemoveStatistics --- 移除某关系或某列的 pg_statistic 条目
 *
 * 若 attnum 为零，移除该关系的所有条目；否则仅移除该列的条目。
 */
void
RemoveStatistics(Oid relid, AttrNumber attnum)
{
	Relation	pgstatistic;
	SysScanDesc scan;
	ScanKeyData key[2];
	int			nkeys;
	HeapTuple	tuple;

	pgstatistic = table_open(StatisticRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_statistic_starelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));

	if (attnum == 0)
		nkeys = 1;
	else
	{
		ScanKeyInit(&key[1],
					Anum_pg_statistic_staattnum,
					BTEqualStrategyNumber, F_INT2EQ,
					Int16GetDatum(attnum));
		nkeys = 2;
	}

	scan = systable_beginscan(pgstatistic, StatisticRelidAttnumInhIndexId, true,
							  NULL, nkeys, key);

	/* 即便 attnum != 0 我们也必须循环，以防存在继承的统计信息 */
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
		CatalogTupleDelete(pgstatistic, &tuple->t_self);

	systable_endscan(scan);

	table_close(pgstatistic, RowExclusiveLock);
}


/*
 * RelationTruncateIndexes --- 将与堆关系关联的所有索引截断为零元组。
 *
 * 该例程会截断并随后重建指定关系上的索引。
 * 调用者必须持有该关系的排他锁。
 */
static void
RelationTruncateIndexes(Relation heapRelation)
{
	ListCell   *indlist;

	/* 请 relcache 生成该关系的索引列表 */
	foreach(indlist, RelationGetIndexList(heapRelation))
	{
		Oid			indexId = lfirst_oid(indlist);
		Relation	currentIndex;
		IndexInfo  *indexInfo;

		/* 打开索引关系；为稳妥起见使用排他锁 */
		currentIndex = index_open(indexId, AccessExclusiveLock);

		/*
		 * 获取 index_build 所需的信息。由于我们知道没有真正需要建立索引的
		 * 元组，因此可以使用一个虚拟的 IndexInfo。这样构建略微廉价一些，
		 * 但真正的目的是避免可能运行索引表达式或谓词中的用户定义代码。
		 * 我们可能会在 ON COMMIT 处理期间被调用，而那时我们不想运行任何
		 * 此类代码。
		 */
		indexInfo = BuildDummyIndexInfo(currentIndex);

		/*
		 * 现在截断实际文件（并丢弃缓冲区）。
		 */
		RelationTruncate(currentIndex, 0);

		/* 初始化索引并重建 */
		/* 注意：我们不需要重新设置主键设置 */
		index_build(heapRelation, currentIndex, indexInfo, true, false);

		/* 该索引处理完毕 */
		index_close(currentIndex, NoLock);
	}
}

/*
 *	 heap_truncate
 *
 *	 该例程删除所有指定关系内的全部数据。
 *
 * 这不是事务安全的！在 commands/tablecmds.c 中存在另一个事务安全的
 * 实现。我们现在仅将其用于临时表的 ON COMMIT 截断，此时是否事务安全
 * 无关紧要。
 */
void
heap_truncate(List *relids)
{
	List	   *relations = NIL;
	ListCell   *cell;

	/* 打开关系以进行处理，并对每个关系获取排他访问 */
	foreach(cell, relids)
	{
		Oid			rid = lfirst_oid(cell);
		Relation	rel;

		rel = table_open(rid, AccessExclusiveLock);
		relations = lappend(relations, rel);
	}

	/* 不允许对已被外键引用的表执行截断 */
	heap_truncate_check_FKs(relations, true);

	/* 可以执行了 */
	foreach(cell, relations)
	{
		Relation	rel = lfirst(cell);

		/* 截断该关系 */
		heap_truncate_one_rel(rel);

		/* 关闭关系，但保留其上的排他锁直到提交 */
		table_close(rel, NoLock);
	}
}

/*
 *	 heap_truncate_one_rel
 *
 *	 该例程删除指定关系内的全部数据。
 *
 * 这不是事务安全的，因为截断是立即执行的，且之后无法回滚。
 * 调用者负责已完成权限等检查，并且必须已经获取了 AccessExclusiveLock。
 */
void
heap_truncate_one_rel(Relation rel)
{
	Oid			toastrelid;

	/*
	 * 截断该关系。分区表没有存储，因此此处对它们无需做任何操作。
	 */
	if (rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		return;

	/* 截断底层关系 */
	table_relation_nontransactional_truncate(rel);

	/* 若关系有索引，则也截断这些索引 */
	RelationTruncateIndexes(rel);

	/* 若存在 toast 表，也截断它 */
	toastrelid = rel->rd_rel->reltoastrelid;
	if (OidIsValid(toastrelid))
	{
		Relation	toastrel = table_open(toastrelid, AccessExclusiveLock);

		table_relation_nontransactional_truncate(toastrel);
		RelationTruncateIndexes(toastrel);
		/* 保留锁... */
		table_close(toastrel, NoLock);
	}
}

/*
 * heap_truncate_check_FKs
 *		检查引用待截断关系列表的外键，若存在则报错
 *
 * 我们不允许此类外键（自引用除外），因为 TRUNCATE 的全部意义就在于
 * 不去扫描将被丢弃的个别行。
 *
 * 此函数被拆分出来，以便两种截断实现都能共享。调用者应当已经持有
 * 关系上的适当锁。
 *
 * tempTables 仅用于选择合适的错误消息。
 */
void
heap_truncate_check_FKs(List *relations, bool tempTables)
{
	List	   *oids = NIL;
	List	   *dependents;
	ListCell   *cell;

	/*
	 * 构建相关关系的 OID 列表。
	 *
	 * 若一个关系没有触发器，则它既不可能拥有外键，也不可能被另一个
	 * 表的外键引用，因此可以忽略它。对于分区表，外键没有触发器，
	 * 因此无论如何都必须将其包含进来。
	 */
	foreach(cell, relations)
	{
		Relation	rel = lfirst(cell);

		if (rel->rd_rel->relhastriggers ||
			rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
			oids = lappend_oid(oids, RelationGetRelid(rel));
	}

	/*
	 * 快速路径：若无关系拥有触发器，则也无关系拥有外键。
	 */
	if (oids == NIL)
		return;

	/*
	 * 否则，必须扫描 pg_constraint。我们用所有关系一起做一次遍历；
	 * 若未找到任何内容，则一切正常。
	 */
	dependents = heap_truncate_find_FKs(oids);
	if (dependents == NIL)
		return;

	/*
	 * 否则，我们对每个关系重复扫描一次，以找出要报错的一对特定关系。
	 * 这种方式相当缓慢，但在失败路径中性能并不重要。采用这种做法的原因
	 * 是为了确保所产生的消息不依赖于 pg_constraint 中行的偶然位置。
	 */
	foreach(cell, oids)
	{
		Oid			relid = lfirst_oid(cell);
		ListCell   *cell2;

		dependents = heap_truncate_find_FKs(list_make1_oid(relid));

		foreach(cell2, dependents)
		{
			Oid			relid2 = lfirst_oid(cell2);

			if (!list_member_oid(oids, relid2))
			{
				char	   *relname = get_rel_name(relid);
				char	   *relname2 = get_rel_name(relid2);

				if (tempTables)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("unsupported ON COMMIT and foreign key combination"),
							 errdetail("Table \"%s\" references \"%s\", but they do not have the same ON COMMIT setting.",
									   relname2, relname)));
				else
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot truncate a table referenced in a foreign key constraint"),
							 errdetail("Table \"%s\" references \"%s\".",
									   relname2, relname),
							 errhint("Truncate table \"%s\" at the same time, "
									 "or use TRUNCATE ... CASCADE.",
									 relname2)));
			}
		}
	}
}

/*
 * heap_truncate_find_FKs
 *		查找拥有引用给定任一关系的外键的关系
 *
 * 输入与结果都是关系 OID 的列表。结果不包含重复项，*不* 包含输入列表
 * 中已有的任何关系，并且按 OID 顺序排列。（最后这个属性主要是为了
 * 保证回归测试中的行为一致；我们不希望行为依赖于 pg_constraint 中
 * 行的偶然位置而发生变化。）
 *
 * 注意：调用者应当已经持有 relationIds 中提到的所有关系的适当锁。
 * 由于新增或删除外键需要对两个关系都持有排他锁，这保证了结果的稳定。
 */
List *
heap_truncate_find_FKs(List *relationIds)
{
	List	   *result = NIL;
	List	   *oids;
	List	   *parent_cons;
	ListCell   *cell;
	ScanKeyData key;
	Relation	fkeyRel;
	SysScanDesc fkeyScan;
	HeapTuple	tuple;
	bool		restart;

	oids = list_copy(relationIds);

	/*
	 * 必须扫描 pg_constraint。目前这是一次顺序扫描，因为在 confrelid
	 * 上没有可用的索引。
	 */
	fkeyRel = table_open(ConstraintRelationId, AccessShareLock);

restart:
	restart = false;
	parent_cons = NIL;

	fkeyScan = systable_beginscan(fkeyRel, InvalidOid, false,
								  NULL, 0, NULL);

	while (HeapTupleIsValid(tuple = systable_getnext(fkeyScan)))
	{
		Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tuple);

		/* 不是外键 */
		if (con->contype != CONSTRAINT_FOREIGN)
			continue;

		/* 未引用我们表列表中任一表 */
		if (!list_member_oid(oids, con->confrelid))
			continue;

		/*
		 * 若该约束拥有一个我们尚未见过的父约束，则将其记录下来以供
		 * 下方的第二次循环使用。跟踪父约束使我们能够向上追溯到顶层约束，
		 * 并查找所有引用该分区表的可能关系。
		 */
		if (OidIsValid(con->conparentid) &&
			!list_member_oid(parent_cons, con->conparentid))
			parent_cons = lappend_oid(parent_cons, con->conparentid);

		/*
		 * 将引用者加入结果，除非它已出现在输入列表中。（不必担心
		 * 重复：我们会在下方修正。）
		 */
		if (!list_member_oid(relationIds, con->conrelid))
			result = lappend_oid(result, con->conrelid);
	}

	systable_endscan(fkeyScan);

	/*
	 * 处理我们找到的每个父约束，将其所引用的关系列表加入 oids 列表。
	 * 若确实加入了任何此类新关系，则重做上方的第一次循环。此外，
	 * 若发现父约束自身也有父约束，则将其也加入，以便我们在一次额外
	 * 的遍历中处理所有关系。
	 */
	foreach(cell, parent_cons)
	{
		Oid			parent = lfirst_oid(cell);

		ScanKeyInit(&key,
					Anum_pg_constraint_oid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(parent));

		fkeyScan = systable_beginscan(fkeyRel, ConstraintOidIndexId,
									  true, NULL, 1, &key);

		tuple = systable_getnext(fkeyScan);
		if (HeapTupleIsValid(tuple))
		{
			Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tuple);

			/*
		 * pg_constraint 行在分区层次结构中总是以这种方式出现：在约束的
		 * 每一侧，为每个指向另一侧最顶层表的分区各出现一行。
		 *
		 * 由于这种安排，我们可以通过将具有有效 conparentid 的所有行
		 * 加入 'parent_cons'，并将 conparentid 为零的所有行加入 'oids'
		 * 列表，来正确地捕获所有相关关系。若向 'oids' 加入了任何 OID，
		 * 则通过设置 'restart' 重做上方的第一次循环。
			 */
			if (OidIsValid(con->conparentid))
				parent_cons = list_append_unique_oid(parent_cons,
													 con->conparentid);
			else if (!list_member_oid(oids, con->confrelid))
			{
				oids = lappend_oid(oids, con->confrelid);
				restart = true;
			}
		}

		systable_endscan(fkeyScan);
	}

	list_free(parent_cons);
	if (restart)
		goto restart;

	table_close(fkeyRel, AccessShareLock);
	list_free(oids);

	/* 现在对结果列表排序并去重 */
	list_sort(result, list_oid_cmp);
	list_deduplicate_oid(result);

	return result;
}

/*
 * StorePartitionKey
 *		将分区键关系的相关信息存入系统目录
 */
void
StorePartitionKey(Relation rel,
				  char strategy,
				  int16 partnatts,
				  AttrNumber *partattrs,
				  List *partexprs,
				  Oid *partopclass,
				  Oid *partcollation)
{
	int			i;
	int2vector *partattrs_vec;
	oidvector  *partopclass_vec;
	oidvector  *partcollation_vec;
	Datum		partexprDatum;
	Relation	pg_partitioned_table;
	HeapTuple	tuple;
	Datum		values[Natts_pg_partitioned_table];
	bool		nulls[Natts_pg_partitioned_table] = {0};
	ObjectAddress myself;
	ObjectAddress referenced;
	ObjectAddresses *addrs;

	Assert(rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE);

	/* 将分区属性编号、操作符类 OID 复制到数组中 */
	partattrs_vec = buildint2vector(partattrs, partnatts);
	partopclass_vec = buildoidvector(partopclass, partnatts);
	partcollation_vec = buildoidvector(partcollation, partnatts);

	/* 将表达式（若有）转换为 text datum */
	if (partexprs)
	{
		char	   *exprString;

		exprString = nodeToString(partexprs);
		partexprDatum = CStringGetTextDatum(exprString);
		pfree(exprString);
	}
	else
		partexprDatum = (Datum) 0;

	pg_partitioned_table = table_open(PartitionedRelationId, RowExclusiveLock);

	/* 只有这个可能为 NULL */
	if (!partexprDatum)
		nulls[Anum_pg_partitioned_table_partexprs - 1] = true;

	values[Anum_pg_partitioned_table_partrelid - 1] = ObjectIdGetDatum(RelationGetRelid(rel));
	values[Anum_pg_partitioned_table_partstrat - 1] = CharGetDatum(strategy);
	values[Anum_pg_partitioned_table_partnatts - 1] = Int16GetDatum(partnatts);
	values[Anum_pg_partitioned_table_partdefid - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_partitioned_table_partattrs - 1] = PointerGetDatum(partattrs_vec);
	values[Anum_pg_partitioned_table_partclass - 1] = PointerGetDatum(partopclass_vec);
	values[Anum_pg_partitioned_table_partcollation - 1] = PointerGetDatum(partcollation_vec);
	values[Anum_pg_partitioned_table_partexprs - 1] = partexprDatum;

	tuple = heap_form_tuple(RelationGetDescr(pg_partitioned_table), values, nulls);

	CatalogTupleInsert(pg_partitioned_table, tuple);
	table_close(pg_partitioned_table, RowExclusiveLock);

	/* 将该关系标记为依赖于以下几项 */
	addrs = new_object_addresses();
	ObjectAddressSet(myself, RelationRelationId, RelationGetRelid(rel));

	/* 每个键列的操作符类与排序规则 */
	for (i = 0; i < partnatts; i++)
	{
		ObjectAddressSet(referenced, OperatorClassRelationId, partopclass[i]);
		add_exact_object_address(&referenced, addrs);

		/* 默认排序规则是固定的，无需费心记录它 */
		if (OidIsValid(partcollation[i]) &&
			partcollation[i] != DEFAULT_COLLATION_OID)
		{
			ObjectAddressSet(referenced, CollationRelationId, partcollation[i]);
			add_exact_object_address(&referenced, addrs);
		}
	}

	record_object_address_dependencies(&myself, addrs, DEPENDENCY_NORMAL);
	free_object_addresses(addrs);

	/*
	 * 分区列在内部被设置为依赖于该表，因为若不丢弃整张表就无法丢弃
	 * 其中任何一列。（ATExecDropColumn 会独立地强制这一点，但它并非
	 * 无懈可击，因此我们也需要这些依赖。）
	 */
	for (i = 0; i < partnatts; i++)
	{
		if (partattrs[i] == 0)
			continue;			/* 此处忽略表达式 */

		ObjectAddressSubSet(referenced, RelationRelationId,
							RelationGetRelid(rel), partattrs[i]);
		recordDependencyOn(&referenced, &myself, DEPENDENCY_INTERNAL);
	}

	/*
	 * 还要考虑分区表达式中提及的任何对象。外部引用（例如函数）获得
	 * NORMAL 依赖。表达式中提及的表列与普通的分类键列处理方式相同，
	 * 即它们在内部依赖于整张表。
	 */
	if (partexprs)
		recordDependencyOnSingleRelExpr(&myself,
										(Node *) partexprs,
										RelationGetRelid(rel),
										DEPENDENCY_NORMAL,
										DEPENDENCY_INTERNAL,
										true /* reverse the self-deps */ );

	/*
	 * 我们必须令 relcache 失效，以便下一次 CommandCounterIncrement()
	 * 会使用刚刚创建的系统目录项中的信息将其重建。
	 */
	CacheInvalidateRelcache(rel);
}

/*
 *	RemovePartitionKeyByRelId
 *		移除某关系的 pg_partitioned_table 项
 */
void
RemovePartitionKeyByRelId(Oid relid)
{
	Relation	rel;
	HeapTuple	tuple;

	rel = table_open(PartitionedRelationId, RowExclusiveLock);

	tuple = SearchSysCache1(PARTRELID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for partition key of relation %u",
			 relid);

	CatalogTupleDelete(rel, &tuple->t_self);

	ReleaseSysCache(tuple);
	table_close(rel, RowExclusiveLock);
}

/*
 * StorePartitionBound
 *		更新 rel 的 pg_class 元组以存储分区边界，并将 relispartition
 *		设为 true
 *
 * 若这是默认分区，则同时更新 pg_partitioned_table 中的默认分区 OID。
 *
 * 此外，令父关系的 relcache 失效，以便下一次重建会将新分区的信息
 * 加载进其分区描述符。若存在默认分区，我们也必须令其 relcache 项失效。
 */
void
StorePartitionBound(Relation rel, Relation parent, PartitionBoundSpec *bound)
{
	Relation	classRel;
	HeapTuple	tuple,
				newtuple;
	Datum		new_val[Natts_pg_class];
	bool		new_null[Natts_pg_class],
				new_repl[Natts_pg_class];
	Oid			defaultPartOid;

	/* 更新 pg_class 元组 */
	classRel = table_open(RelationRelationId, RowExclusiveLock);
	tuple = SearchSysCacheCopy1(RELOID,
								ObjectIdGetDatum(RelationGetRelid(rel)));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u",
			 RelationGetRelid(rel));

#ifdef USE_ASSERT_CHECKING
	{
		Form_pg_class classForm;
		bool		isnull;

		classForm = (Form_pg_class) GETSTRUCT(tuple);
		Assert(!classForm->relispartition);
		(void) SysCacheGetAttr(RELOID, tuple, Anum_pg_class_relpartbound,
							   &isnull);
		Assert(isnull);
	}
#endif

	/* 填入 relpartbound 值 */
	memset(new_val, 0, sizeof(new_val));
	memset(new_null, false, sizeof(new_null));
	memset(new_repl, false, sizeof(new_repl));
	new_val[Anum_pg_class_relpartbound - 1] = CStringGetTextDatum(nodeToString(bound));
	new_null[Anum_pg_class_relpartbound - 1] = false;
	new_repl[Anum_pg_class_relpartbound - 1] = true;
	newtuple = heap_modify_tuple(tuple, RelationGetDescr(classRel),
								 new_val, new_null, new_repl);
	/* 同时设置该标志 */
	((Form_pg_class) GETSTRUCT(newtuple))->relispartition = true;

	/*
	 * 我们已经检查过没有继承子表，但仍需重置 relhassubclass，
	 * 以防它是遗留下来的值。
	 */
	if (rel->rd_rel->relkind == RELKIND_RELATION && rel->rd_rel->relhassubclass)
		((Form_pg_class) GETSTRUCT(newtuple))->relhassubclass = false;

	CatalogTupleUpdate(classRel, &newtuple->t_self, newtuple);
	heap_freetuple(newtuple);
	table_close(classRel, RowExclusiveLock);

	/*
	 * 若我们正在为默认分区存储边界，则同时更新 pg_partitioned_table。
	 */
	if (bound->is_default)
		update_default_partition_oid(RelationGetRelid(parent),
									 RelationGetRelid(rel));

	/* 使这些更新可见 */
	CommandCounterIncrement();

	/*
	 * 默认分区的分区约束依赖于其他每个分区的分区边界，因此每当
	 * 新增或删除一个分区时，我们都必须令该分区的 relcache 项失效。
	 */
	defaultPartOid =
		get_default_oid_from_partdesc(RelationGetPartitionDesc(parent, true));
	if (OidIsValid(defaultPartOid))
		CacheInvalidateRelcacheByRelid(defaultPartOid);

	CacheInvalidateRelcache(parent);
}
