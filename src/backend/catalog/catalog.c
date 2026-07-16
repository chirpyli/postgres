/*-------------------------------------------------------------------------
 *
 * catalog.c
 *		与目录命名约定及其他硬编码知识有关的例程
 *		硬编码的知识
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/catalog.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <fcntl.h>
#include <unistd.h>

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/transam.h"
#include "catalog/catalog.h"
#include "catalog/namespace.h"
#include "catalog/pg_auth_members.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_database.h"
#include "catalog/pg_db_role_setting.h"
#include "catalog/pg_largeobject.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_parameter_acl.h"
#include "catalog/pg_replication_origin.h"
#include "catalog/pg_seclabel.h"
#include "catalog/pg_shdepend.h"
#include "catalog/pg_shdescription.h"
#include "catalog/pg_shseclabel.h"
#include "catalog/pg_subscription.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

/*
 * 决定在 GetNewOidWithIndex() 中何时发出日志消息的参数
 * GetNewOidWithIndex()
 */
#define GETNEWOID_LOG_THRESHOLD 1000000
#define GETNEWOID_LOG_MAX_INTERVAL 128000000

/*
 * IsSystemRelation
 *		当且仅当该关系是一个系统目录或一个 TOAST 表时为 true。
 *		有关系统目录的确切定义，请参见 IsCatalogRelation。
 *
 *		出于保护目的，我们将用户关系的 TOAST 表视为“system relations”，
 *		例如，没有特殊权限就无法修改它们的模式。
 *		因此，该函数的大多数用途都是在
 *		检查 allow_system_table_mods 限制是否适用。
 *		出于其他目的，请考虑是否应该改用
 *		IsCatalogRelation。
 *
 *		该函数不执行任何目录访问。
 *		某些调用者依赖于这一点！
 */
bool
IsSystemRelation(Relation relation)
{
	return IsSystemClass(RelationGetRelid(relation), relation->rd_rel);
}

/*
 * IsSystemClass
 *		与上面类似，但接受 Form_pg_class 作为参数。
 *		用于我们不想打开关系、而必须直接
 *		搜索 pg_class 的情形。
 */
bool
IsSystemClass(Oid relid, Form_pg_class reltuple)
{
	/* IsCatalogRelationOid 略快一些，因此先测试它 */
	return (IsCatalogRelationOid(relid) || IsToastClass(reltuple));
}

/*
 * IsCatalogRelation
 *		当且仅当该关系是一个系统目录时为 true。
 *
 *		所谓系统目录，我们指的是在 initdb 的引导（bootstrap）
 *		阶段创建的关系。这不仅包括目录本身，还包括
 *		它们的索引，以及（若有）TOAST 表和索引。
 *
 *		该函数不执行任何目录访问。
 *		某些调用者依赖于这一点！
 */
bool
IsCatalogRelation(Relation relation)
{
	return IsCatalogRelationOid(RelationGetRelid(relation));
}

/*
 * IsCatalogRelationOid
 *		当且仅当此 OID 所标识的关系是一个系统目录时为 true。
 *
 *		所谓系统目录，我们指的是在 initdb 的引导（bootstrap）
 *		阶段创建的关系。这不仅包括目录本身，还包括
 *		它们的索引，以及（若有）TOAST 表和索引。
 *
 *		该函数不执行任何目录访问。
 *		某些调用者依赖于这一点！
 */
bool
IsCatalogRelationOid(Oid relid)
{
	/*
	 * 我们认为，若一个关系拥有一个被固定的 OID，则它是一个系统目录。
	 * 这包括所有已定义的目录、它们的索引，以及它们的 TOAST
	 * 表和索引。
	 *
	 * 此规则排除了 information_schema 中的关系，它们并非
	 * 系统的核心组成部分，可以像用户关系一样对待。
	 * （由于删除并重建 information_schema 是合法的，任何不这样做的规则
	 * 都是错误的。）
	 *
	 * 此测试是可靠的，因为 OID 回绕时会跳过这一范围的
	 * OID；参见 GetNewObjectId()。
	 */
	return (relid < (Oid) FirstUnpinnedObjectId);
}

/*
 * IsCatalogTextUniqueIndexOid
 *		当且仅当此 OID 所标识的关系是一个目录的 UNIQUE 索引，
 *		且其包含类型为 text 的列。
 *
 *		relcache 不得使用这些索引。向任何 UNIQUE 索引中插入时，
 *		会在持有 BUFFER_LOCK_EXCLUSIVE 的情况下比较索引键。
 *		bttextcmp() 会搜索 COLLOID 目录缓存（catcache）。取决于并发的
 *		失效流量，catcache 可能触达 relcache 的构建。后端
 *		若 relcache 构建读取了
 *		持有排他锁的缓冲区，则会自己在 LWLocks 上死锁。
 *
 *		为了避免自身成为自我死锁的原因，这里不读取
 *		目录，而是使用一个硬编码的列表，并配有相应的
 *		回归测试。
 */
bool
IsCatalogTextUniqueIndexOid(Oid relid)
{
	switch (relid)
	{
		case ParameterAclParnameIndexId:
		case ReplicationOriginNameIndex:
		case SecLabelObjectIndexId:
		case SharedSecLabelObjectIndexId:
			return true;
	}
	return false;
}

/*
 * IsInplaceUpdateRelation
 *		当且仅当核心代码对该关系执行原地更新时为 true。
 *
 *		这用于断言，并使执行器遵循
 *		该锁协议在 README.tuplock 的 "Locking to write
 *		inplace-updated tables" 一节中描述。扩展也可以原地更新其他堆
 *		表，但对同一表的并发 SQL UPDATE 可能会覆盖
 *		这些修改。
 *
 *		执行器可以假定这些关系既不是分区、也不是被分区表，并且
 *		没有触发器。
 */
bool
IsInplaceUpdateRelation(Relation relation)
{
	return IsInplaceUpdateOid(RelationGetRelid(relation));
}

/*
 * IsInplaceUpdateOid
 *		与上面类似，但接受一个 OID 作为参数。
 */
bool
IsInplaceUpdateOid(Oid relid)
{
	return (relid == RelationRelationId ||
			relid == DatabaseRelationId);
}

/*
 * IsToastRelation
 *		当且仅当该关系是一个 TOAST 支撑关系（或索引）时为 true。
 *
 *		不执行任何目录访问。
 */
bool
IsToastRelation(Relation relation)
{
	/*
	 * 我们实际检查的是该关系是否属于一个 pg_toast
	 * 命名空间。由于在别处强制实施了相关限制，这应当是等价的：
	 * 禁止在 pg_toast 命名空间中创建用户关系，或把关系移入/移出
	 * 该命名空间。同时还要注意，对于属于其他会话临时表的
	 * TOAST 表，这不会返回 "true"。
	 * 我们期望其他机制会阻止对这些表的访问。
	 */
	return IsToastNamespace(RelationGetNamespace(relation));
}

/*
 * IsToastClass
 *		与上面类似，但接受 Form_pg_class 作为参数。
 *		用于我们不想打开关系、而必须直接
 *		搜索 pg_class 的情形。
 */
bool
IsToastClass(Form_pg_class reltuple)
{
	Oid			relnamespace = reltuple->relnamespace;

	return IsToastNamespace(relnamespace);
}

/*
 * IsCatalogNamespace
 *		当且仅当命名空间为 pg_catalog 时为 true。
 *
 *		不执行任何目录访问。
 *
 * 注意：这里不写成宏的原因是为了避免在很多地方
 * 包含 catalog/pg_namespace.h。
 */
bool
IsCatalogNamespace(Oid namespaceId)
{
	return namespaceId == PG_CATALOG_NAMESPACE;
}

/*
 * IsToastNamespace
 *		当且仅当命名空间为 pg_toast 或我的临时 TOAST 表命名空间时为 true。
 *
 *		不执行任何目录访问。
 *
 * 注意：对于属于其他后端的临时 TOAST 表命名空间，这会返回 false。
 * 它们与其他后端的普通
 * 临时表命名空间一视同仁，并在适当的时候阻止访问。
 * 如果需要检查这些，你也许可以使用 isAnyTempNamespace，
 * 但要注意那确实会涉及一次目录访问。
 */
bool
IsToastNamespace(Oid namespaceId)
{
	return (namespaceId == PG_TOAST_NAMESPACE) ||
		isTempToastNamespace(namespaceId);
}


/*
 * IsReservedName
 *		当且仅当名字以 pg_ 前缀开头时为 true。
 *
 *		对于某些类别的对象，前缀 pg_ 是保留给
 *		系统对象专用的。从 8.0 开始，这只对
 *		模式和表空间名成立。到了 9.6，这同样适用于
 *		角色。
 */
bool
IsReservedName(const char *name)
{
	/* 为了速度而写的丑陋代码 */
	return (name[0] == 'p' &&
			name[1] == 'g' &&
			name[2] == '_');
}


/*
 * IsSharedRelation
 *		给定一个关系的 OID，判断它是否应该
 *		在整个数据库集簇中共享。
 *
 * 在较早的版本中，这必须是硬编码的，以便我们能够计算关系的
 * 锁标签（locktag），并在检查其目录项之前将其锁定。
 * 既然我们现在有了 MVCC 目录访问，使得这一要求成立的竞态条件
 * 已经不复存在，因此我们或许可以考虑放宽这一限制。
 * 然而，如果我们扫描 pg_class 项来查找 relisshared，并且只在
 * 那时才锁定关系，pg_class 可能会在此期间被更新，
 * 迫使我们再次扫描该关系，这肯定会相当复杂，
 * 并可能带来不希望出现的性能后果。所幸，共享关系的集合
 * 相当稳定，因此手工维护一份它们的 OID 列表
 * 并非完全不切实际。
 */
bool
IsSharedRelation(Oid relationId)
{
	/* 这些是共享的目录（查找 BKI_SHARED_RELATION） */
	if (relationId == AuthIdRelationId ||
		relationId == AuthMemRelationId ||
		relationId == DatabaseRelationId ||
		relationId == DbRoleSettingRelationId ||
		relationId == ParameterAclRelationId ||
		relationId == ReplicationOriginRelationId ||
		relationId == SharedDependRelationId ||
		relationId == SharedDescriptionRelationId ||
		relationId == SharedSecLabelRelationId ||
		relationId == SubscriptionRelationId ||
		relationId == TableSpaceRelationId)
		return true;
	/* 这些是它们的索引 */
	if (relationId == AuthIdOidIndexId ||
		relationId == AuthIdRolnameIndexId ||
		relationId == AuthMemMemRoleIndexId ||
		relationId == AuthMemRoleMemIndexId ||
		relationId == AuthMemOidIndexId ||
		relationId == AuthMemGrantorIndexId ||
		relationId == DatabaseNameIndexId ||
		relationId == DatabaseOidIndexId ||
		relationId == DbRoleSettingDatidRolidIndexId ||
		relationId == ParameterAclOidIndexId ||
		relationId == ParameterAclParnameIndexId ||
		relationId == ReplicationOriginIdentIndex ||
		relationId == ReplicationOriginNameIndex ||
		relationId == SharedDependDependerIndexId ||
		relationId == SharedDependReferenceIndexId ||
		relationId == SharedDescriptionObjIndexId ||
		relationId == SharedSecLabelObjectIndexId ||
		relationId == SubscriptionNameIndexId ||
		relationId == SubscriptionObjectIndexId ||
		relationId == TablespaceNameIndexId ||
		relationId == TablespaceOidIndexId)
		return true;
	/* 这些是它们的 TOAST 表和 TOAST 索引 */
	if (relationId == PgDatabaseToastTable ||
		relationId == PgDatabaseToastIndex ||
		relationId == PgDbRoleSettingToastTable ||
		relationId == PgDbRoleSettingToastIndex ||
		relationId == PgParameterAclToastTable ||
		relationId == PgParameterAclToastIndex ||
		relationId == PgShdescriptionToastTable ||
		relationId == PgShdescriptionToastIndex ||
		relationId == PgShseclabelToastTable ||
		relationId == PgShseclabelToastIndex ||
		relationId == PgSubscriptionToastTable ||
		relationId == PgSubscriptionToastIndex ||
		relationId == PgTablespaceToastTable ||
		relationId == PgTablespaceToastIndex)
		return true;
	return false;
}

/*
 * IsPinnedObject
 *		给定一个数据库对象的 class + OID 标识，报告它是否
 *		被“pinned”，即因系统需要而不可删除。
 *
 * 我们以前在 pg_depend 中显式地表示这一点，但事实证明那
 * 会带来不必要的开销，因此现在我们依赖于 OID 范围测试。
 */
bool
IsPinnedObject(Oid classId, Oid objectId)
{
	/*
	 * OID 大于 FirstUnpinnedObjectId 的对象永远不会被固定。由于
	 * OID 生成器在回绕时会跳过此范围，这一检查
	 * 保证了用户定义的对象永远不会被视为已固定。
	 */
	if (objectId >= FirstUnpinnedObjectId)
		return false;

	/*
	 * 大对象永远不会被固定。我们需要这个特例，是因为
	 * 它们的 OID 可以由用户指定。
	 */
	if (classId == LargeObjectRelationId)
		return false;

	/*
	 * 在目录的 .dat 文件中定义了一些对象，按照策略我们倾向于
	 * 不把它们视为已固定。我们以前通过在 pg_depend 中排除它们来处理，
	 * 但现在在代码中硬编码它们的 OID 同样简单。（如果用户确实删除了
	 * 并重建了它们，它们将拥有新的、但肯定未被固定的 OID，因此没有问题。）
	 * 并重建了它们，它们将拥有新的、但肯定未被固定的 OID，因此没有问题。）
	 *
	 * 同时检查 classId 和 objectId 属于画蛇添足，因为低于
	 * FirstGenbkiObjectId 的 OID 应当是全局唯一的，但出于
	 * 健壮性仍这样做。
	 */

	/* 公共命名空间未被固定 */
	if (classId == NamespaceRelationId &&
		objectId == PG_PUBLIC_NAMESPACE)
		return false;

	/*
	 * 数据库永远不会被固定。看起来把至少 template0 固定起来是谨慎的；
	 * 但我们是有意为之，以便 template0 和
	 * template1 能够相互重建，从而让它们充当
	 * 相互备份（只要你没有修改过 template1 就行）。
	 */
	if (classId == DatabaseRelationId)
		return false;

	/*
	 * 所有其他由 initdb 创建的对象都是被固定的。这有点过度（系统
	 * 并不真的依赖于拥有每一个古怪的数据类型，例如），但只生成
	 * 最少的必需依赖集合似乎很难，而强制维护一份精确的列表
	 * 要比
	 * 这里使用的简单范围检查昂贵得多。
	 */
	return true;
}


/*
 * GetNewOidWithIndex
 *		生成一个在系统关系内唯一的 OID。
 *
 * 由于 OID 不会立即插入表中，这里存在
 * 竞态条件；但只有其他人
 * 在我们完成插入之前，设法循环遍历了 2^32 个 OID 并生成了相同的 OID，
 * 才可能出问题。这似乎不太可能成为问题。注意
 * 如果我们必须 *commit* 该行来结束竞态条件，风险
 * 会相当高；因此我们在测试中使用 SnapshotAny，以便
 * 能看到未提交的行。（我们以前使用 SnapshotDirty，但它有
 * 忽略最近删除的行的缺点，会制造一种风险：
 * 只要我们自己的 MVCC 快照认为某个
 * 最近删除的行仍然存活，就会出现瞬时冲突。在选择 TOAST 的
 * OID 时风险要高得多，因为 SnapshotToast 会把死行无限期地视为活跃。）
 *
 * 注意，我们实际上是在假定该表的条目数量相对较少
 * （远小于 2^32），并且不会存在很长的
 * 连续已有 OID 序列。对于
 * 系统目录而言，这是一个基本合理的假设。
 *
 * 调用者必须对该关系持有适当的锁。
 */
Oid
GetNewOidWithIndex(Relation relation, Oid indexId, AttrNumber oidcolumn)
{
	Oid			newOid;
	SysScanDesc scan;
	ScanKeyData key;
	bool		collides;
	uint64		retries = 0;
	uint64		retries_before_log = GETNEWOID_LOG_THRESHOLD;

	/* 仅支持系统关系 */
	Assert(IsSystemRelation(relation));

	/* 在引导（bootstrap）模式下，我们没有任何索引可用 */
	if (IsBootstrapProcessingMode())
		return GetNewObjectId();

	/*
	 * 我们绝不应被要求在
	 * pg_upgrade 期间生成新的 pg_type OID；这样做会与它想要
	 * 分配的 OID 发生冲突。触发这个断言意味着存在某条路径，在其中我们未能
	 * 确保类型 OID 由转储脚本中的命令决定。
	 */
	Assert(!IsBinaryUpgrade || RelationGetRelid(relation) != TypeRelationId);

	/* 不断生成新的 OID，直到找到一个不在表中的为止 */
	do
	{
		CHECK_FOR_INTERRUPTS();

		newOid = GetNewObjectId();

		ScanKeyInit(&key,
					oidcolumn,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(newOid));

		/* 参见上面关于使用 SnapshotAny 的说明 */
		scan = systable_beginscan(relation, indexId, true,
								  SnapshotAny, 1, &key);

		collides = HeapTupleIsValid(systable_getnext(scan));

		systable_endscan(scan);

		/*
		 * 记录：我们迭代次数已超过 GETNEWOID_LOG_THRESHOLD，但尚未
		 * 在关系中找到未使用的 OID。然后以
		 * 指数增长的间隔重复记录，直到迭代次数超过
		 * GETNEWOID_LOG_MAX_INTERVAL。最后每隔
		 * GETNEWOID_LOG_MAX_INTERVAL 重复记录一次，除非找到了未使用的 OID。这一
		 * 逻辑是必要的，以免用类似的消息
		 * 填满服务器日志。
		 */
		if (retries >= retries_before_log)
		{
			ereport(LOG,
					(errmsg("still searching for an unused OID in relation \"%s\"",
							RelationGetRelationName(relation)),
					 errdetail_plural("OID candidates have been checked %" PRIu64 " time, but no unused OID has been found yet.",
									  "OID candidates have been checked %" PRIu64 " times, but no unused OID has been found yet.",
									  retries,
									  retries)));

			/*
			 * 将下次记录前要执行的重试次数翻倍，直到它
			 * 达到 GETNEWOID_LOG_MAX_INTERVAL。
			 */
			if (retries_before_log * 2 <= GETNEWOID_LOG_MAX_INTERVAL)
				retries_before_log *= 2;
			else
				retries_before_log += GETNEWOID_LOG_MAX_INTERVAL;
		}

		retries++;
	} while (collides);

	/*
	 * 如果至少发出了一条日志消息，则也记录 OID 分配的
	 * 完成。
	 */
	if (retries > GETNEWOID_LOG_THRESHOLD)
	{
		ereport(LOG,
				(errmsg_plural("new OID has been assigned in relation \"%s\" after %" PRIu64 " retry",
							   "new OID has been assigned in relation \"%s\" after %" PRIu64 " retries",
							   retries,
							   RelationGetRelationName(relation), retries)));
	}

	return newOid;
}

/*
 * GetNewRelFileNumber
 *		生成一个在给定的
 *		表空间的数据库内唯一的 relfilenumber。
 *
 * 如果该 relfilenumber 也将用作关系的 OID，则传入
 * 已打开的 pg_class 目录，本例程将保证结果
 * 同时也是 pg_class 中一个未使用的 OID。如果结果仅用于
 * 作为已有关系的 relfilenumber，则为 pg_class 传入 NULL。
 *
 * 与 GetNewOidWithIndex() 类似，这里存在某种理论上的竞态
 * 条件风险，但似乎不值得担心。
 *
 * 注意：我们不支持在引导（bootstrap）模式下使用本函数。所有由
 * 引导创建的关系都预先分配了 OID，因此没有这个必要。
 */
RelFileNumber
GetNewRelFileNumber(Oid reltablespace, Relation pg_class, char relpersistence)
{
	RelFileLocatorBackend rlocator;
	RelPathStr	rpath;
	bool		collides;
	ProcNumber	procNumber;

	/*
	 * 如果我们竟然在 pg_upgrade 期间到达这里，说明出了问题；所有
	 * 在二进制升级运行期间的 relfilenumber 分配都应当
	 * 由转储脚本中的命令决定。
	 */
	Assert(!IsBinaryUpgrade);

	switch (relpersistence)
	{
		case RELPERSISTENCE_TEMP:
			procNumber = ProcNumberForTempRelations();
			break;
		case RELPERSISTENCE_UNLOGGED:
		case RELPERSISTENCE_PERMANENT:
			procNumber = INVALID_PROC_NUMBER;
			break;
		default:
			elog(ERROR, "invalid relpersistence: %c", relpersistence);
			return InvalidRelFileNumber;	/* 让编译器保持安静（避免告警） */
	}

	/* 这段逻辑应与 RelationInitPhysicalAddr 保持一致 */
	rlocator.locator.spcOid = reltablespace ? reltablespace : MyDatabaseTableSpace;
	rlocator.locator.dbOid =
		(rlocator.locator.spcOid == GLOBALTABLESPACE_OID) ?
		InvalidOid : MyDatabaseId;

	/*
	 * relpath 会根据后端编号而变化，因此我们必须
	 * 在这里正确地初始化它，以确保任何基于
	 * 文件名的冲突都能被正确检测到。
	 */
	rlocator.backend = procNumber;

	do
	{
		CHECK_FOR_INTERRUPTS();

		/* 生成 OID */
		if (pg_class)
			rlocator.locator.relNumber = GetNewOidWithIndex(pg_class, ClassOidIndexId,
															Anum_pg_class_oid);
		else
			rlocator.locator.relNumber = GetNewObjectId();

		/* 检查是否存在同名文件 */
		rpath = relpath(rlocator, MAIN_FORKNUM);

		if (access(rpath.str, F_OK) == 0)
		{
			/* 确定发生了冲突 */
			collides = true;
		}
		else
		{
			/*
			 * 这里我们有点左右为难：如果 errno 是
			 * ENOENT 以外的其它值，我们是否应该宣告冲突并循环？在
			 * 实践中，无论 errno 为何似乎最好都继续。如果
			 * 存在冲突的文件，当我们
			 * 尝试创建新的关系文件时会得到一个 smgr 失败。
			 */
			collides = false;
		}
	} while (collides);

	return rlocator.locator.relNumber;
}

/*
 * GetNewOidWithIndex() 的 SQL 可调用接口。除了 initdb 直接
 * 向目录表插入数据，以及从损坏中恢复之外，这
 * 很少会需要用到。
 *
 * 该函数有意不在面向用户的文档中记录。
 */
Datum
pg_nextoid(PG_FUNCTION_ARGS)
{
	Oid			reloid = PG_GETARG_OID(0);
	Name		attname = PG_GETARG_NAME(1);
	Oid			idxoid = PG_GETARG_OID(2);
	Relation	rel;
	Relation	idx;
	HeapTuple	atttuple;
	Form_pg_attribute attform;
	AttrNumber	attno;
	Oid			newoid;

	/*
	 * 由于此函数并非设计用于正常运行期间，并且
	 * 只支持系统目录（修改它们需要超级用户权限），
	 * 仅仅检查超级用户权限应当不会妨碍有效的
	 * 使用场景。
	 */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to call %s()",
						"pg_nextoid")));

	rel = table_open(reloid, RowExclusiveLock);
	idx = index_open(idxoid, RowExclusiveLock);

	if (!IsSystemRelation(rel))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("pg_nextoid() can only be used on system catalogs")));

	if (idx->rd_index->indrelid != RelationGetRelid(rel))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("index \"%s\" does not belong to table \"%s\"",
						RelationGetRelationName(idx),
						RelationGetRelationName(rel))));

	atttuple = SearchSysCacheAttName(reloid, NameStr(*attname));
	if (!HeapTupleIsValid(atttuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						NameStr(*attname), RelationGetRelationName(rel))));

	attform = ((Form_pg_attribute) GETSTRUCT(atttuple));
	attno = attform->attnum;

	if (attform->atttypid != OIDOID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("column \"%s\" is not of type oid",
						NameStr(*attname))));

	if (IndexRelationGetNumberOfKeyAttributes(idx) != 1 ||
		idx->rd_index->indkey.values[0] != attno)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("index \"%s\" is not the index for column \"%s\"",
						RelationGetRelationName(idx),
						NameStr(*attname))));

	newoid = GetNewOidWithIndex(rel, idxoid, attno);

	ReleaseSysCache(atttuple);
	table_close(rel, RowExclusiveLock);
	index_close(idx, RowExclusiveLock);

	PG_RETURN_OID(newoid);
}

/*
 * StopGeneratingPinnedObjectIds() 的 SQL 可调用接口。
 *
 * 这仅供 initdb 使用，因此有意不在
 * 面向用户的文档中记录。
 */
Datum
pg_stop_making_pinned_objects(PG_FUNCTION_ARGS)
{
	/*
	 * 双保险式的检查，因为 StopGeneratingPinnedObjectIds 在非
	 * 单用户模式下终究会失败。
	 */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to call %s()",
						"pg_stop_making_pinned_objects")));

	StopGeneratingPinnedObjectIds();

	PG_RETURN_VOID();
}
