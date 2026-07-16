/*-------------------------------------------------------------------------
 *
 * pg_type.c
 *	  用于支持对 pg_type 关系进行操作的例程
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_type.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/binary_upgrade.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/typecmds.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

/* 可能由 pg_upgrade_support 函数设置 */
Oid			binary_upgrade_next_pg_type_oid = InvalidOid;

/* ----------------------------------------------------------------
 *		TypeShellMake
 *
 *		该过程向 pg_type 关系中插入一个"shell"（空壳）元组。
 *		插入的类型元组拥有合法但虚构的值，其
 *		"typisdefined" 字段为 false，表示它尚未被真正定义。
 *
 *		这样做是为了让目录中先存在一个元组。该类型的 I/O
 *		函数将链接到这个元组。当发出完整的
 *		CREATE TYPE 命令时，这些虚构的值会被正确的取值替换，
 *		并且 "typisdefined" 会被置为 true。
 * ----------------------------------------------------------------
 */
ObjectAddress
TypeShellMake(const char *typeName, Oid typeNamespace, Oid ownerId)
{
	Relation	pg_type_desc;
	TupleDesc	tupDesc;
	int			i;
	HeapTuple	tup;
	Datum		values[Natts_pg_type];
	bool		nulls[Natts_pg_type];
	Oid			typoid;
	NameData	name;
	ObjectAddress address;

	Assert(PointerIsValid(typeName));

	/*
	 * 打开 pg_type
	 */
	pg_type_desc = table_open(TypeRelationId, RowExclusiveLock);
	tupDesc = pg_type_desc->rd_att;

	/*
	 * 初始化 *nulls 和 *values 数组
	 */
	for (i = 0; i < Natts_pg_type; ++i)
	{
		nulls[i] = false;
		values[i] = (Datum) NULL;	/* 多余，但安全 */
	}

	/*
	 * 用类型名和虚构值初始化 *values
	 *
	 * 表示细节与 int4 相同……只要保持一致，具体取值并不重要。
	 * 另外注意，我们将其 typtype 设为 TYPTYPE_PSEUDO，作为额外保险，
	 * 确保它不会被误认为一个可用的类型。
	 */
	namestrcpy(&name, typeName);
	values[Anum_pg_type_typname - 1] = NameGetDatum(&name);
	values[Anum_pg_type_typnamespace - 1] = ObjectIdGetDatum(typeNamespace);
	values[Anum_pg_type_typowner - 1] = ObjectIdGetDatum(ownerId);
	values[Anum_pg_type_typlen - 1] = Int16GetDatum(sizeof(int32));
	values[Anum_pg_type_typbyval - 1] = BoolGetDatum(true);
	values[Anum_pg_type_typtype - 1] = CharGetDatum(TYPTYPE_PSEUDO);
	values[Anum_pg_type_typcategory - 1] = CharGetDatum(TYPCATEGORY_PSEUDOTYPE);
	values[Anum_pg_type_typispreferred - 1] = BoolGetDatum(false);
	values[Anum_pg_type_typisdefined - 1] = BoolGetDatum(false);
	values[Anum_pg_type_typdelim - 1] = CharGetDatum(DEFAULT_TYPDELIM);
	values[Anum_pg_type_typrelid - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_type_typsubscript - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_type_typelem - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_type_typarray - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_type_typinput - 1] = ObjectIdGetDatum(F_SHELL_IN);
	values[Anum_pg_type_typoutput - 1] = ObjectIdGetDatum(F_SHELL_OUT);
	values[Anum_pg_type_typreceive - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_type_typsend - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_type_typmodin - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_type_typmodout - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_type_typanalyze - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_type_typalign - 1] = CharGetDatum(TYPALIGN_INT);
	values[Anum_pg_type_typstorage - 1] = CharGetDatum(TYPSTORAGE_PLAIN);
	values[Anum_pg_type_typnotnull - 1] = BoolGetDatum(false);
	values[Anum_pg_type_typbasetype - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_type_typtypmod - 1] = Int32GetDatum(-1);
	values[Anum_pg_type_typndims - 1] = Int32GetDatum(0);
	values[Anum_pg_type_typcollation - 1] = ObjectIdGetDatum(InvalidOid);
	nulls[Anum_pg_type_typdefaultbin - 1] = true;
	nulls[Anum_pg_type_typdefault - 1] = true;
	nulls[Anum_pg_type_typacl - 1] = true;

	/* 是否对 pg_type.oid 使用二进制升级覆盖？ */
	if (IsBinaryUpgrade)
	{
		if (!OidIsValid(binary_upgrade_next_pg_type_oid))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("pg_type OID value not set when in binary upgrade mode")));

		typoid = binary_upgrade_next_pg_type_oid;
		binary_upgrade_next_pg_type_oid = InvalidOid;
	}
	else
	{
		typoid = GetNewOidWithIndex(pg_type_desc, TypeOidIndexId,
									Anum_pg_type_oid);
	}

	values[Anum_pg_type_oid - 1] = ObjectIdGetDatum(typoid);

	/*
	 * 创建一个新的类型元组
	 */
	tup = heap_form_tuple(tupDesc, values, nulls);

	/*
	 * 将元组插入关系，并获取该元组的 oid。
	 */
	CatalogTupleInsert(pg_type_desc, tup);

	/*
	 * 创建依赖关系。在 bootstrap 模式下我们可以/必须跳过这一步。
	 */
	if (!IsBootstrapProcessingMode())
		GenerateTypeDependencies(tup,
								 pg_type_desc,
								 NULL,
								 NULL,
								 0,
								 false,
								 false,
								 true,	/* 创建扩展依赖 */
								 false);

	/* 新建 shell 类型后的钩子 */
	InvokeObjectPostCreateHook(TypeRelationId, typoid, 0);

	ObjectAddressSet(address, TypeRelationId, typoid);

	/*
	 * 清理并返回类型 oid
	 */
	heap_freetuple(tup);
	table_close(pg_type_desc, RowExclusiveLock);

	return address;
}

/* ----------------------------------------------------------------
 *		TypeCreate
 *
 *		该函数完成定义一个新类型所需的所有工作。
 *
 *		返回分配给新类型的 ObjectAddress。
 *		如果 newTypeOid 为零（通常情况），则创建一个新的 OID；
 *		否则我们就精确地使用该 OID。
 * ----------------------------------------------------------------
 */
ObjectAddress
TypeCreate(Oid newTypeOid,
		   const char *typeName,
		   Oid typeNamespace,
		   Oid relationOid,		/* 仅用于关系行类型 */
		   char relationKind,	/* 同上 */
		   Oid ownerId,
		   int16 internalSize,
		   char typeType,
		   char typeCategory,
		   bool typePreferred,
		   char typDelim,
		   Oid inputProcedure,
		   Oid outputProcedure,
		   Oid receiveProcedure,
		   Oid sendProcedure,
		   Oid typmodinProcedure,
		   Oid typmodoutProcedure,
		   Oid analyzeProcedure,
		   Oid subscriptProcedure,
		   Oid elementType,
		   bool isImplicitArray,
		   Oid arrayType,
		   Oid baseType,
		   const char *defaultTypeValue,	/* 人类可读表示 */
		   char *defaultTypeBin,	/* 加工后的表示 */
		   bool passedByValue,
		   char alignment,
		   char storage,
		   int32 typeMod,
		   int32 typNDims,		/* baseType 的数组维度 */
		   bool typeNotNull,
		   Oid typeCollation)
{
	Relation	pg_type_desc;
	Oid			typeObjectId;
	bool		isDependentType;
	bool		rebuildDeps = false;
	Acl		   *typacl;
	HeapTuple	tup;
	bool		nulls[Natts_pg_type];
	bool		replaces[Natts_pg_type];
	Datum		values[Natts_pg_type];
	NameData	name;
	int			i;
	ObjectAddress address;

	/*
	 * 我们假设调用方已分别校验过各个参数，但尚未
	 * 检查它们之间的不良组合。
	 *
	 * 校验大小规格：要么为正数（定长），要么为 -1
	 * （varlena），要么为 -2（cstring）。
	 */
	if (!(internalSize > 0 ||
		  internalSize == -1 ||
		  internalSize == -2))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("invalid type internal size %d",
						internalSize)));

	if (passedByValue)
	{
		/*
		 * 按值传递的类型必须具有固定长度，且该长度必须是
		 * fetch_att() 和 store_att_byval() 所支持的取值之一；其
		 * 对齐方式也最好与之相符。所有这些代码必须与
		 * access/tupmacs.h 保持一致！
		 */
		if (internalSize == (int16) sizeof(char))
		{
			if (alignment != TYPALIGN_CHAR)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("alignment \"%c\" is invalid for passed-by-value type of size %d",
								alignment, internalSize)));
		}
		else if (internalSize == (int16) sizeof(int16))
		{
			if (alignment != TYPALIGN_SHORT)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("alignment \"%c\" is invalid for passed-by-value type of size %d",
								alignment, internalSize)));
		}
		else if (internalSize == (int16) sizeof(int32))
		{
			if (alignment != TYPALIGN_INT)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("alignment \"%c\" is invalid for passed-by-value type of size %d",
								alignment, internalSize)));
		}
#if SIZEOF_DATUM == 8
		else if (internalSize == (int16) sizeof(Datum))
		{
			if (alignment != TYPALIGN_DOUBLE)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("alignment \"%c\" is invalid for passed-by-value type of size %d",
								alignment, internalSize)));
		}
#endif
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("internal size %d is invalid for passed-by-value type",
							internalSize)));
	}
	else
	{
		/* varlena 类型必须具有 INT 对齐或更严格的对齐 */
		if (internalSize == -1 &&
			!(alignment == TYPALIGN_INT || alignment == TYPALIGN_DOUBLE))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("alignment \"%c\" is invalid for variable-length type",
							alignment)));
		/* cstring 必须具有 CHAR 对齐 */
		if (internalSize == -2 && !(alignment == TYPALIGN_CHAR))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("alignment \"%c\" is invalid for variable-length type",
							alignment)));
	}

	/* 只有 varlena 类型才能被 TOAST */
	if (storage != TYPSTORAGE_PLAIN && internalSize != -1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("fixed-size types must have storage PLAIN")));

	/*
	 * 如果这是一个隐式创建的数组类型或
	 * 多范围（multirange）类型，或者是一个非复合类型的
	 * 关系行类型，那么它就是一个依赖类型。对于此类类型，
	 * 我们将 ACL 置空，并跳过创建某些依赖记录，
	 * 因为通过所依赖的类型或关系已经存在相应依赖。
	 * （注意：这与 GenerateTypeDependencies 中的某些行为紧密相关。）
	 */
	isDependentType = isImplicitArray ||
		typeType == TYPTYPE_MULTIRANGE ||
		(OidIsValid(relationOid) && relationKind != RELKIND_COMPOSITE_TYPE);

	/*
	 * 初始化 heap_form_tuple 或 heap_modify_tuple 所需的数组
	 */
	for (i = 0; i < Natts_pg_type; ++i)
	{
		nulls[i] = false;
		replaces[i] = true;
		values[i] = (Datum) 0;
	}

	/*
	 * 插入数据值
	 */
	namestrcpy(&name, typeName);
	values[Anum_pg_type_typname - 1] = NameGetDatum(&name);
	values[Anum_pg_type_typnamespace - 1] = ObjectIdGetDatum(typeNamespace);
	values[Anum_pg_type_typowner - 1] = ObjectIdGetDatum(ownerId);
	values[Anum_pg_type_typlen - 1] = Int16GetDatum(internalSize);
	values[Anum_pg_type_typbyval - 1] = BoolGetDatum(passedByValue);
	values[Anum_pg_type_typtype - 1] = CharGetDatum(typeType);
	values[Anum_pg_type_typcategory - 1] = CharGetDatum(typeCategory);
	values[Anum_pg_type_typispreferred - 1] = BoolGetDatum(typePreferred);
	values[Anum_pg_type_typisdefined - 1] = BoolGetDatum(true);
	values[Anum_pg_type_typdelim - 1] = CharGetDatum(typDelim);
	values[Anum_pg_type_typrelid - 1] = ObjectIdGetDatum(relationOid);
	values[Anum_pg_type_typsubscript - 1] = ObjectIdGetDatum(subscriptProcedure);
	values[Anum_pg_type_typelem - 1] = ObjectIdGetDatum(elementType);
	values[Anum_pg_type_typarray - 1] = ObjectIdGetDatum(arrayType);
	values[Anum_pg_type_typinput - 1] = ObjectIdGetDatum(inputProcedure);
	values[Anum_pg_type_typoutput - 1] = ObjectIdGetDatum(outputProcedure);
	values[Anum_pg_type_typreceive - 1] = ObjectIdGetDatum(receiveProcedure);
	values[Anum_pg_type_typsend - 1] = ObjectIdGetDatum(sendProcedure);
	values[Anum_pg_type_typmodin - 1] = ObjectIdGetDatum(typmodinProcedure);
	values[Anum_pg_type_typmodout - 1] = ObjectIdGetDatum(typmodoutProcedure);
	values[Anum_pg_type_typanalyze - 1] = ObjectIdGetDatum(analyzeProcedure);
	values[Anum_pg_type_typalign - 1] = CharGetDatum(alignment);
	values[Anum_pg_type_typstorage - 1] = CharGetDatum(storage);
	values[Anum_pg_type_typnotnull - 1] = BoolGetDatum(typeNotNull);
	values[Anum_pg_type_typbasetype - 1] = ObjectIdGetDatum(baseType);
	values[Anum_pg_type_typtypmod - 1] = Int32GetDatum(typeMod);
	values[Anum_pg_type_typndims - 1] = Int32GetDatum(typNDims);
	values[Anum_pg_type_typcollation - 1] = ObjectIdGetDatum(typeCollation);

	/*
	 * 初始化该类型的默认二进制值。当然，要注意检查空值。
	 */
	if (defaultTypeBin)
		values[Anum_pg_type_typdefaultbin - 1] = CStringGetTextDatum(defaultTypeBin);
	else
		nulls[Anum_pg_type_typdefaultbin - 1] = true;

	/*
	 * 初始化该类型的默认值。
	 */
	if (defaultTypeValue)
		values[Anum_pg_type_typdefault - 1] = CStringGetTextDatum(defaultTypeValue);
	else
		nulls[Anum_pg_type_typdefault - 1] = true;

	/*
	 * 同时也要初始化该类型的 ACL。但依赖类型不拥有 ACL。
	 */
	if (isDependentType)
		typacl = NULL;
	else
		typacl = get_user_default_acl(OBJECT_TYPE, ownerId,
									  typeNamespace);
	if (typacl != NULL)
		values[Anum_pg_type_typacl - 1] = PointerGetDatum(typacl);
	else
		nulls[Anum_pg_type_typacl - 1] = true;

	/*
	 * 打开 pg_type 并准备插入或更新一行。
	 *
	 * 注意：更新在 bootstrap 模式下无法正确工作；
	 * 但我们不预期在 bootstrap 模式下会覆盖任何 shell 类型。
	 */
	pg_type_desc = table_open(TypeRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy2(TYPENAMENSP,
							  CStringGetDatum(typeName),
							  ObjectIdGetDatum(typeNamespace));
	if (HeapTupleIsValid(tup))
	{
		Form_pg_type typform = (Form_pg_type) GETSTRUCT(tup);

		/*
		 * 检查该类型是否尚未被定义。不过，它可能以
		 * shell 类型的形式存在。
		 */
		if (typform->typisdefined)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("type \"%s\" already exists", typeName)));

		/*
		 * shell 类型必须由同一所有者创建
		 */
		if (typform->typowner != ownerId)
			aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_TYPE, typeName);

		/* 如果调用方想强制指定 OID 则会出问题 */
		if (OidIsValid(newTypeOid))
			elog(ERROR, "cannot assign new OID to existing shell type");

		replaces[Anum_pg_type_oid - 1] = false;

		/*
		 * 可以更新已有的 shell 类型元组
		 */
		tup = heap_modify_tuple(tup,
								RelationGetDescr(pg_type_desc),
								values,
								nulls,
								replaces);

		CatalogTupleUpdate(pg_type_desc, &tup->t_self, tup);

		typeObjectId = typform->oid;

		rebuildDeps = true;		/* 清除 shell 类型的依赖 */
	}
	else
	{
		/* 如果调用方要求，则强制使用指定 OID */
		if (OidIsValid(newTypeOid))
			typeObjectId = newTypeOid;
		/* 如果提供了，则使用二进制升级覆盖来指定 pg_type.oid。 */
		else if (IsBinaryUpgrade)
		{
			if (!OidIsValid(binary_upgrade_next_pg_type_oid))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("pg_type OID value not set when in binary upgrade mode")));

			typeObjectId = binary_upgrade_next_pg_type_oid;
			binary_upgrade_next_pg_type_oid = InvalidOid;
		}
		else
		{
			typeObjectId = GetNewOidWithIndex(pg_type_desc, TypeOidIndexId,
											  Anum_pg_type_oid);
		}

		values[Anum_pg_type_oid - 1] = ObjectIdGetDatum(typeObjectId);

		tup = heap_form_tuple(RelationGetDescr(pg_type_desc),
							  values, nulls);

		CatalogTupleInsert(pg_type_desc, tup);
	}

	/*
	 * 创建依赖关系。在 bootstrap 模式下我们可以/必须跳过这一步。
	 */
	if (!IsBootstrapProcessingMode())
		GenerateTypeDependencies(tup,
								 pg_type_desc,
								 (defaultTypeBin ?
								  stringToNode(defaultTypeBin) :
								  NULL),
								 typacl,
								 relationKind,
								 isImplicitArray,
								 isDependentType,
								 true,	/* 创建扩展依赖 */
								 rebuildDeps);

	/* 新建类型后的钩子 */
	InvokeObjectPostCreateHook(TypeRelationId, typeObjectId, 0);

	ObjectAddressSet(address, TypeRelationId, typeObjectId);

	/*
	 * 收尾
	 */
	table_close(pg_type_desc, RowExclusiveLock);

	return address;
}

/*
 * GenerateTypeDependencies: 构建类型所需的依赖关系
 *
 * 该函数需要了解的关于类型的绝大部分信息，都通过新的 pg_type 行
 * typeTuple 传入。我们还要求调用方传入 pg_type 的 Relation，
 * 以便我们能方便地获取该行的元组描述符。
 *
 * 虽然本函数能够从元组中提取 defaultExpr 和 typacl，
 * 但这样做相对开销较大，而调用方可能手里已有这些值。
 * 如果方便就传入它们，否则传入 NULL。（typacl 本质上是
 * "Acl *"，但我们将其声明为 "void *" 以避免在 pg_type.h 中包含 acl.h。）
 *
 * relationKind 和 isImplicitArray 同样较难从元组推断，
 * 因此我们要求调用方传入它们（它们不是可选的）。
 *
 * 如果这是一个隐式数组、多范围（multirange）或
 * 关系行类型，则 isDependentType 为 true；这意味着它不需要
 * 自己建立对所有者等的依赖。
 *
 * 如果我们正处于扩展脚本中且 makeExtensionDep 为 true，
 * 则会创建一个扩展成员依赖。
 * 在创建新类型或替换 shell 类型时，makeExtensionDep 应为 true，
 * 但对于已有类型的 ALTER TYPE 则不应为 true。传入 false 会
 * 让该类型的扩展成员关系保持不变。
 *
 * 如果这是一个已有的类型，则 rebuild 应为 true。我们会移除
 * 现有依赖并从头重建它们。ALTER TYPE 以及替换 shell 类型时
 * 都需要这样做。不过我们不会移除任何已有的扩展依赖；因此，
 * 如果 makeExtensionDep 同样为 true 且我们正处于扩展脚本中，
 * 除非该类型已经属于当前扩展，否则会发生错误。这正是我们
 * 在替换 shell 类型时所期望的行为，也是两个标志同时为 true
 * 的唯一情形。
 */
void
GenerateTypeDependencies(HeapTuple typeTuple,
						 Relation typeCatalog,
						 Node *defaultExpr,
						 void *typacl,
						 char relationKind, /* 仅用于关系行类型 */
						 bool isImplicitArray,
						 bool isDependentType,
						 bool makeExtensionDep,
						 bool rebuild)
{
	Form_pg_type typeForm = (Form_pg_type) GETSTRUCT(typeTuple);
	Oid			typeObjectId = typeForm->oid;
	Datum		datum;
	bool		isNull;
	ObjectAddress myself,
				referenced;
	ObjectAddresses *addrs_normal;

	/* 如果调用方没有传入 defaultExpr，则从中提取 */
	if (defaultExpr == NULL)
	{
		datum = heap_getattr(typeTuple, Anum_pg_type_typdefaultbin,
							 RelationGetDescr(typeCatalog), &isNull);
		if (!isNull)
			defaultExpr = stringToNode(TextDatumGetCString(datum));
	}
	/* 如果调用方没有传入 typacl，则从中提取 */
	if (typacl == NULL)
	{
		datum = heap_getattr(typeTuple, Anum_pg_type_typacl,
							 RelationGetDescr(typeCatalog), &isNull);
		if (!isNull)
			typacl = DatumGetAclPCopy(datum);
	}

	/* 如果是重建，则先清除旧的依赖（扩展依赖除外） */
	if (rebuild)
	{
		deleteDependencyRecordsFor(TypeRelationId, typeObjectId, true);
		deleteSharedDependencyRecordsFor(TypeRelationId, typeObjectId, 0);
	}

	ObjectAddressSet(myself, TypeRelationId, typeObjectId);

	/*
	 * 创建对命名空间、所有者、ACL 的依赖。
	 *
	 * 对于依赖类型则跳过这些，因为它会通过所依赖的类型或关系
	 * 间接拥有这些依赖。一个例外是：多范围类型需要拥有自己的
	 * 命名空间依赖，因为我们并不强制它们与其范围类型处于同一模式。
	 */

	/* 收集普通依赖以便批量记录 */
	addrs_normal = new_object_addresses();

	if (!isDependentType || typeForm->typtype == TYPTYPE_MULTIRANGE)
	{
		ObjectAddressSet(referenced, NamespaceRelationId,
						 typeForm->typnamespace);
		add_exact_object_address(&referenced, addrs_normal);
	}

	if (!isDependentType)
	{
		recordDependencyOnOwner(TypeRelationId, typeObjectId,
								typeForm->typowner);

		recordDependencyOnNewAcl(TypeRelationId, typeObjectId, 0,
								 typeForm->typowner, typacl);
	}

	/*
	 * 如果请求，则创建扩展依赖。
	 *
	 * 我们过去对依赖类型会跳过这一步，但显式记录它们的
	 * 扩展成员关系似乎更好；否则像 postgres_fdw 的
	 * 可下推性（shippability）测试之类的代码会被误导。
	 */
	if (makeExtensionDep)
		recordDependencyOnCurrentExtension(&myself, rebuild);

	/* 对 I/O 及支持函数的普通依赖 */
	if (OidIsValid(typeForm->typinput))
	{
		ObjectAddressSet(referenced, ProcedureRelationId, typeForm->typinput);
		add_exact_object_address(&referenced, addrs_normal);
	}

	if (OidIsValid(typeForm->typoutput))
	{
		ObjectAddressSet(referenced, ProcedureRelationId, typeForm->typoutput);
		add_exact_object_address(&referenced, addrs_normal);
	}

	if (OidIsValid(typeForm->typreceive))
	{
		ObjectAddressSet(referenced, ProcedureRelationId, typeForm->typreceive);
		add_exact_object_address(&referenced, addrs_normal);
	}

	if (OidIsValid(typeForm->typsend))
	{
		ObjectAddressSet(referenced, ProcedureRelationId, typeForm->typsend);
		add_exact_object_address(&referenced, addrs_normal);
	}

	if (OidIsValid(typeForm->typmodin))
	{
		ObjectAddressSet(referenced, ProcedureRelationId, typeForm->typmodin);
		add_exact_object_address(&referenced, addrs_normal);
	}

	if (OidIsValid(typeForm->typmodout))
	{
		ObjectAddressSet(referenced, ProcedureRelationId, typeForm->typmodout);
		add_exact_object_address(&referenced, addrs_normal);
	}

	if (OidIsValid(typeForm->typanalyze))
	{
		ObjectAddressSet(referenced, ProcedureRelationId, typeForm->typanalyze);
		add_exact_object_address(&referenced, addrs_normal);
	}

	if (OidIsValid(typeForm->typsubscript))
	{
		ObjectAddressSet(referenced, ProcedureRelationId, typeForm->typsubscript);
		add_exact_object_address(&referenced, addrs_normal);
	}

	/* 域对其基类型的普通依赖。 */
	if (OidIsValid(typeForm->typbasetype))
	{
		ObjectAddressSet(referenced, TypeRelationId, typeForm->typbasetype);
		add_exact_object_address(&referenced, addrs_normal);
	}

	/*
	 * 域对其排序规则（collation）的普通依赖。我们知道
	 * 默认排序规则是固定的（pinned），因此无需记录它。
	 */
	if (OidIsValid(typeForm->typcollation) &&
		typeForm->typcollation != DEFAULT_COLLATION_OID)
	{
		ObjectAddressSet(referenced, CollationRelationId, typeForm->typcollation);
		add_exact_object_address(&referenced, addrs_normal);
	}

	record_object_address_dependencies(&myself, addrs_normal, DEPENDENCY_NORMAL);
	free_object_addresses(addrs_normal);

	/* 对默认表达式的普通依赖。 */
	if (defaultExpr)
		recordDependencyOnExpr(&myself, defaultExpr, NIL, DEPENDENCY_NORMAL);

	/*
	 * 如果该类型是某个关系的行类型，则将其标记为在内部
	 * 依赖于该关系，*除非*它是一个独立的复合类型关系。
	 * 对于后一种情况，我们必须反转依赖方向。
	 *
	 * 在前一种情况下，这允许该类型在关系被删除时自动被删除，
	 * 反之则不会。而在后一种情况下，我们自然得到相反的效果。
	 */
	if (OidIsValid(typeForm->typrelid))
	{
		ObjectAddressSet(referenced, RelationRelationId, typeForm->typrelid);

		if (relationKind != RELKIND_COMPOSITE_TYPE)
			recordDependencyOn(&myself, &referenced, DEPENDENCY_INTERNAL);
		else
			recordDependencyOn(&referenced, &myself, DEPENDENCY_INTERNAL);
	}

	/*
	 * 如果该类型是一个隐式创建的数组类型，则将其标记为在内部
	 * 依赖于其元素类型。否则，如果它具有元素类型，
	 * 则该依赖是一个普通依赖。
	 */
	if (OidIsValid(typeForm->typelem))
	{
		ObjectAddressSet(referenced, TypeRelationId, typeForm->typelem);
		recordDependencyOn(&myself, &referenced,
						   isImplicitArray ? DEPENDENCY_INTERNAL : DEPENDENCY_NORMAL);
	}

	/*
	 * 注意：你可能会认为，按照上面的类比，我们应该在这里记录
	 * 多范围类型对其范围类型的内部依赖。但实际上，这一步是由
	 * RangeCreate() 完成的，它同时还负责记录其他范围类型特有的依赖。
	 * 这相当不优雅。目前这样做没问题，因为我们不存在需要重新生成
	 * 范围或多范围类型依赖的场景。但将来也许需要把那段逻辑
	 * 移到这里，以支持此类重新生成。
	 */
}

/*
 * RenameTypeInternal
 *		该函数重命名一个类型，以及任何关联的数组类型。
 *
 *		调用方必须已经检查过权限。
 *
 *		目前它用于重命名表行类型，以及
 *		ALTER TYPE RENAME TO 命令。
 */
void
RenameTypeInternal(Oid typeOid, const char *newTypeName, Oid typeNamespace)
{
	Relation	pg_type_desc;
	HeapTuple	tuple;
	Form_pg_type typ;
	Oid			arrayOid;
	Oid			oldTypeOid;

	pg_type_desc = table_open(TypeRelationId, RowExclusiveLock);

	tuple = SearchSysCacheCopy1(TYPEOID, ObjectIdGetDatum(typeOid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for type %u", typeOid);
	typ = (Form_pg_type) GETSTRUCT(tuple);

	/* 这里我们不应更改模式（schema） */
	Assert(typeNamespace == typ->typnamespace);

	arrayOid = typ->typarray;

	/* 检查是否存在冲突的类型名。 */
	oldTypeOid = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
								 CStringGetDatum(newTypeName),
								 ObjectIdGetDatum(typeNamespace));

	/*
	 * 如果存在，则检查它是否是一个自动生成的数组类型，
	 * 如果是，则将其重命名以让出位置。（但对于 shell 类型我们必须跳过这一步，
	 * 因为在这种情况下 moveArrayTypeName 会做错事。）
	 * 否则，我们至少可以给出一个比唯一索引冲突更友好的错误。
	 */
	if (OidIsValid(oldTypeOid))
	{
		if (get_typisdefined(oldTypeOid) &&
			moveArrayTypeName(oldTypeOid, newTypeName, typeNamespace))
			 /* 成功避开该问题 */ ;
		else
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("type \"%s\" already exists", newTypeName)));
	}

	/* 好，执行重命名 —— 元组是一份副本，因此可以直接在上面修改 */
	namestrcpy(&(typ->typname), newTypeName);

	CatalogTupleUpdate(pg_type_desc, &tuple->t_self, tuple);

	InvokeObjectPostAlterHook(TypeRelationId, typeOid, 0);

	heap_freetuple(tuple);
	table_close(pg_type_desc, RowExclusiveLock);

	/*
	 * 如果该类型拥有数组类型，则递归处理它。但如果上面
	 * 已经重命名过该数组类型，我们就无需再做任何事
	 * （例如将 "foo" 重命名为 "_foo" 时就会发生这种情况）。
	 */
	if (OidIsValid(arrayOid) && arrayOid != oldTypeOid)
	{
		char	   *arrname = makeArrayTypeName(newTypeName, typeNamespace);

		RenameTypeInternal(arrayOid, arrname, typeNamespace);
		pfree(arrname);
	}
}


/*
 * makeArrayTypeName
 *		- 给定一个基类型名，为其生成一个数组类型名
 *
 *		调用方负责释放（pfree）返回的结果
 */
char *
makeArrayTypeName(const char *typeName, Oid typeNamespace)
{
	char	   *arr_name;
	int			pass = 0;
	char		suffix[NAMEDATALEN];

	/*
	 * 按照 PostgreSQL 古老的传统，数组类型名是在基类型名前
	 * 加一个下划线构成的。许多客户端代码都知道这个约定，
	 * 因此不要去改动它。不过，对于生成的名字过长或与已有名字
	 * 冲突这类边界情况，这一传统并未给出清晰的处理方式。
	 * 我们目前的规则是：(1) 按需从右侧截断基类型名；
	 * (2) 如果存在冲突，则追加另一个下划线以及若干用于
	 * 保证唯一性的数字。这与 ChooseRelationName() 的做法类似。
	 *
	 * 实际的名字生成可以交给 makeObjectName() 完成，
	 * 只需传给它一个空的第一名称分量即可。
	 */

	/* 首先，尝试不带数字后缀 */
	arr_name = makeObjectName("", typeName, NULL);

	for (;;)
	{
		if (!SearchSysCacheExists2(TYPENAMENSP,
								   CStringGetDatum(arr_name),
								   ObjectIdGetDatum(typeNamespace)))
			break;

		/* 那次尝试发生了冲突。准备一个带数字的新名字。 */
		pfree(arr_name);
		snprintf(suffix, sizeof(suffix), "%d", ++pass);
		arr_name = makeObjectName("", typeName, suffix);
	}

	return arr_name;
}


/*
 * moveArrayTypeName
 *		- 尝试重新分配用户想要使用的数组类型名。
 *
 * 给定的类型名被发现已经存在（具有给定的 OID）。如果它是一个
 * 自动生成的数组类型，则修改该数组类型的名字以消除冲突。
 * 这允许用户先创建类型 "foo"、再创建类型 "_foo" 而不会出问题。
 * （当然，如果两个后端并发地尝试创建名称相似的类型，会存在竞争条件，
 * 但最坏情况不过是一次不必要的失败 —— 如果由于名字冲突导致类型创建失败，
 * 我们在这里所做的任何改动都会被回滚。）
 *
 * 注意，必须在调用 makeArrayTypeName 以确定新类型自身的数组类型名
 * *之前*调用本函数；否则后者必然会选到相同的名字。
 *
 * 如果成功移动了该类型则返回 true，否则返回 false。
 *
 * 如果给定的类型是一个 shell 类型，我们同样返回 true。在这种情况下，
 * 该类型并未被重命名让位，但依然可以预期 TypeCreate 会成功。
 * 这一行为对大多数调用方而言很方便 —— 那些需要区分 shell 类型情形的
 * 调用方，必须自己做 typisdefined 测试。
 */
bool
moveArrayTypeName(Oid typeOid, const char *typeName, Oid typeNamespace)
{
	Oid			elemOid;
	char	   *newname;

	/* 如果它是一个 shell 类型，我们无需做任何事。 */
	if (!get_typisdefined(typeOid))
		return true;

	/* 如果它不是自动生成的数组类型，则无法更改它。 */
	elemOid = get_element_type(typeOid);
	if (!OidIsValid(elemOid) ||
		get_array_type(elemOid) != typeOid)
		return false;

	/*
	 * 好，使用 makeArrayTypeName 来选出一个未被使用的名字变体。
	 * 注意，由于 makeArrayTypeName 是一个迭代过程，这里产生的名字
	 * 与"如果我们要创建的冲突类型事先就已存在时，它第一次
	 * 所可能产生的名字"是一致的。
	 */
	newname = makeArrayTypeName(typeName, typeNamespace);

	/* 执行重命名 */
	RenameTypeInternal(typeOid, newname, typeNamespace);

	/*
	 * 我们必须递增命令计数器，以便后续对 makeArrayTypeName 的
	 * 调用能看到我们刚刚做的改动，从而不会选到相同的名字。
	 */
	CommandCounterIncrement();

	pfree(newname);

	return true;
}


/*
 * makeMultirangeTypeName
 *		- 给定一个范围类型名，为其生成一个多范围类型名
 *
 *		调用方负责释放（pfree）返回的结果
 */
char *
makeMultirangeTypeName(const char *rangeTypeName, Oid typeNamespace)
{
	char	   *buf;
	const char *rangestr;

	/*
	 * 如果范围类型名包含 "range"，则将其改为
	 * "multirange"。否则在末尾加上 "_multirange"。
	 */
	rangestr = strstr(rangeTypeName, "range");
	if (rangestr)
	{
		char	   *prefix = pnstrdup(rangeTypeName, rangestr - rangeTypeName);

		buf = psprintf("%s%s%s", prefix, "multi", rangestr);
	}
	else
		buf = psprintf("%s_multirange", pnstrdup(rangeTypeName, NAMEDATALEN - 12));

	/* 将其截断到 NAMEDATALEN-1 字节 */
	buf[pg_mbcliplen(buf, strlen(buf), NAMEDATALEN - 1)] = '\0';

	if (SearchSysCacheExists2(TYPENAMENSP,
							  CStringGetDatum(buf),
							  ObjectIdGetDatum(typeNamespace)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("type \"%s\" already exists", buf),
				 errdetail("Failed while creating a multirange type for type \"%s\".", rangeTypeName),
				 errhint("You can manually specify a multirange type name using the \"multirange_type_name\" attribute.")));

	return pstrdup(buf);
}
