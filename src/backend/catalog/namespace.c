/*-------------------------------------------------------------------------
 *
 * namespace.c
 *	  支持访问和搜索命名空间的代码
 *
 * 本文件与 pg_namespace.c 相互独立，后者包含直接操作 pg_namespace 系统目录
 * 的例程。本模块提供与定义"命名空间搜索路径"以及实现基于搜索路径的
 * 搜索相关的例程。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/catalog/namespace.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/parallel.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/dependency.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_conversion.h"
#include "catalog/pg_database.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_ts_config.h"
#include "catalog/pg_ts_dict.h"
#include "catalog/pg_ts_parser.h"
#include "catalog/pg_ts_template.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "common/hashfn_unstable.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/guc_hooks.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/varlena.h"


/*
 * 命名空间搜索路径是一个可能为空的命名空间 OID 列表。
 * 除了显式列表之外，还可以包含隐式搜索的命名空间：
 *
 * 1. 如果本次会话中已经初始化了一个 TEMP 表命名空间，则会首先隐式搜索它。
 *
 * 2. 系统目录命名空间总是会被搜索。如果系统命名空间出现在显式路径中，
 * 则会按照指定的顺序搜索；否则会在 TEMP 表之后、显式列表*之前*搜索。
 * （系统命名空间看起来应该隐式排在最后，但 SQL99 似乎要求这种行为。
 * 此外，这也提供了一种优先搜索系统命名空间的方法，而无需因此将其
 * 设为默认的创建目标命名空间。）
 *
 * 出于安全原因，使用搜索路径进行搜索时，除了关系和类型之外，搜索任何
 * 其他对象类型都会忽略临时命名空间。（我们必须允许类型，因为临时表
 * 具有行类型。）
 *
 * 默认的创建目标命名空间始终是显式列表的第一个元素。如果显式列表为空，
 * 则没有默认目标。
 *
 * search_path 的文本规范可以包含 "$user"，以引用与当前用户同名的命名空间
 * （如果存在的话）。（如果不存在这样的命名空间，则会被忽略。）另外，它
 * 可以包含 "pg_temp"，以引用当前后端的临时命名空间。如果临时命名空间
 * 尚未建立，这通常也可以忽略，但有一个特殊情况：如果 "pg_temp" 排在第一位，
 * 则它应该是默认的创建目标。我们对这种情况做了一点 hack，使得临时命名空间
 * 直到第一次尝试在其中创建对象时才建立。（之所以这样 hack，是因为我们无法
 * 在事务之外创建临时命名空间，但 search_path 的初始 GUC 处理发生在事务之外。）
 * 如果 "pg_temp" 出现在字符串的首位、但因命名空间尚未建立而未反映到
 * activeCreationNamespace 中，则 activeTempCreationPending 为 true。
 *
 * 在 bootstrap 模式下，搜索路径被设为 "pg_catalog"，这样系统命名空间就是
 * 唯一被搜索或插入的命名空间。initdb 也会注意在其后引导（post-bootstrap）
 * 的独立后端运行中将 search_path 设为 "pg_catalog"。除此之外，默认搜索路径
 * 由 GUC 决定。出厂默认路径包含 PUBLIC 命名空间（如果存在），其前面是
 * 用户的个人命名空间（如果存在）。
 *
 * activeSearchPath 始终是实际活动的路径；它指向 baseSearchPath，
 * 即由 namespace_search_path 派生出的列表。
 *
 * 如果 baseSearchPathValid 为 false，则 baseSearchPath（以及其他派生变量）
 * 需要从 namespace_search_path 重新计算，或者在没有任何系统缓存失效的情况下
 * 从搜索路径缓存中检索。我们在对 namespace_search_path 赋值或收到
 * pg_namespace 或 pg_authid 的系统缓存失效事件时将其标记为无效。
 * 重新计算将在下一次查找尝试时进行。
 *
 * 在 namespace_search_path 中提到的、当前用户 ID 不可读取的任何命名空间
 * 都会简单地被排除在 baseSearchPath 之外；因此当当前用户 ID 发生变化时，
 * 我们必须愿意重新计算路径。namespaceUser 是已经为其计算出路径的用户 ID。
 *
 * 注意：这些 List 变量所指向的所有数据都位于 TopMemoryContext 中。
 *
 * 每当 activeSearchPath/activeCreationNamespace/activeTempCreationPending
 * 的有效值发生变化时，activePathGeneration 都会递增。这可用于快速检测
 * 自上次检查搜索路径状态以来是否发生过任何变化。
 */

/* 以下变量定义了实际活动的状态： */

static List *activeSearchPath = NIL;

/* 创建对象的默认位置；如果为 InvalidOid，则没有默认目标 */
static Oid	activeCreationNamespace = InvalidOid;

/* 如果为 true，则 activeCreationNamespace 有误，应为临时命名空间 */
static bool activeTempCreationPending = false;

/* current generation counter; make sure this is never zero */
static uint64 activePathGeneration = 1;

/* 以下变量是最近一次从 namespace_search_path 派生出的值： */

static List *baseSearchPath = NIL;

static Oid	baseCreationNamespace = InvalidOid;

static bool baseTempCreationPending = false;

static Oid	namespaceUser = InvalidOid;

/* 上述四个值仅在 baseSearchPathValid 为真时才有效 */
static bool baseSearchPathValid = true;

/*
 * 搜索路径缓存的存储。将 searchPathCacheValid 清除，作为一种简单地
 * 使*所有*缓存项（而不仅仅是活动项）失效的方法。
 */
static bool searchPathCacheValid = false;
static MemoryContext SearchPathCacheContext = NULL;

typedef struct SearchPathCacheKey
{
	const char *searchPath;
	Oid			roleid;
} SearchPathCacheKey;

typedef struct SearchPathCacheEntry
{
	SearchPathCacheKey key;
	List	   *oidlist;		/* namespace OIDs that pass ACL checks */
	List	   *finalPath;		/* cached final computed search path */
	Oid			firstNS;		/* first explicitly-listed namespace */
	bool		temp_missing;
	bool		forceRecompute; /* force recompute of finalPath */

	/* needed for simplehash */
	char		status;
} SearchPathCacheEntry;

/*
 * myTempNamespace 在建立 TEMP 命名空间之前（或除非建立）为 InvalidOid，
 * 这种情况发生在第一次执行 CREATE TEMP TABLE 命令时。此后它即为临时
 * 命名空间的 OID。
 *
 * myTempToastNamespace 是我的临时表的 TOAST 表所属命名空间的 OID。
 * 它与 myTempNamespace 同时被设置，在此之前为 InvalidOid。
 *
 * myTempNamespaceSubID 表示我们是否在当前子事务中创建了 TEMP 命名空间。
 * 该标志会向上传播到子事务树中，因此如果所有中间子事务都提交，主事务
 * 将正确识别该标志。当它为 InvalidSubTransactionId 时，我们要么尚未
 * 建立 TEMP 命名空间，要么已经成功提交了其创建，具体取决于
 * myTempNamespace 是否有效。
 */
static Oid	myTempNamespace = InvalidOid;

static Oid	myTempToastNamespace = InvalidOid;

static SubTransactionId myTempNamespaceSubID = InvalidSubTransactionId;

/*
 * 这是用户的文本搜索路径规范 --- 即 GUC 变量 'search_path' 的值。
 */
char	   *namespace_search_path = NULL;


/* 局部函数 */
static bool RelationIsVisibleExt(Oid relid, bool *is_missing);
static bool TypeIsVisibleExt(Oid typid, bool *is_missing);
static bool FunctionIsVisibleExt(Oid funcid, bool *is_missing);
static bool OperatorIsVisibleExt(Oid oprid, bool *is_missing);
static bool OpclassIsVisibleExt(Oid opcid, bool *is_missing);
static bool OpfamilyIsVisibleExt(Oid opfid, bool *is_missing);
static bool CollationIsVisibleExt(Oid collid, bool *is_missing);
static bool ConversionIsVisibleExt(Oid conid, bool *is_missing);
static bool StatisticsObjIsVisibleExt(Oid stxid, bool *is_missing);
static bool TSParserIsVisibleExt(Oid prsId, bool *is_missing);
static bool TSDictionaryIsVisibleExt(Oid dictId, bool *is_missing);
static bool TSTemplateIsVisibleExt(Oid tmplId, bool *is_missing);
static bool TSConfigIsVisibleExt(Oid cfgid, bool *is_missing);
static void recomputeNamespacePath(void);
static void AccessTempTableNamespace(bool force);
static void InitTempTableNamespace(void);
static void RemoveTempRelations(Oid tempNamespaceId);
static void RemoveTempRelationsCallback(int code, Datum arg);
static void InvalidationCallback(Datum arg, int cacheid, uint32 hashvalue);
static bool MatchNamedCall(HeapTuple proctup, int nargs, List *argnames,
						   bool include_out_arguments, int pronargs,
						   int **argnumbers);

/*
 * 频繁地重新计算命名空间路径开销很大，例如当某个函数在 proconfig 中
 * 设置了 search_path 时。新增一个搜索路径缓存，供 recomputeNamespacePath()
 * 使用。
 *
 * 该缓存还用于在 check_search_path() 中记住已经校验过的字符串，从而避免
 * 反复调用 SplitIdentifierString()。
 *
 * 搜索路径缓存基于一个对 simplehash 哈希表（nsphash，定义如下）的封装。
 * spcache 封装器在尝试初始化键时处理 OOM，优化对同一键的重复查找，
 * 并且提供了更方便的 API。
 */

static inline uint32
spcachekey_hash(SearchPathCacheKey key)
{
	fasthash_state hs;
	int			sp_len;

	fasthash_init(&hs, 0);

	hs.accum = key.roleid;
	fasthash_combine(&hs);

	/*
	 * Combine search path into the hash and save the length for tweaking the
	 * final mix.
	 */
	sp_len = fasthash_accum_cstring(&hs, key.searchPath);

	return fasthash_final32(&hs, sp_len);
}

static inline bool
spcachekey_equal(SearchPathCacheKey a, SearchPathCacheKey b)
{
	return a.roleid == b.roleid &&
		strcmp(a.searchPath, b.searchPath) == 0;
}

#define SH_PREFIX		nsphash
#define SH_ELEMENT_TYPE	SearchPathCacheEntry
#define SH_KEY_TYPE		SearchPathCacheKey
#define SH_KEY			key
#define SH_HASH_KEY(tb, key)   	spcachekey_hash(key)
#define SH_EQUAL(tb, a, b)		spcachekey_equal(a, b)
#define SH_SCOPE		static inline
#define SH_DECLARE
#define SH_DEFINE
#include "lib/simplehash.h"

/*
 * 我们只预期会使用少量的唯一 search_path 字符串。如果此缓存增长到
 * 不合理的规模，则将其重置，以避免内存的稳态增长。很可能只有其中少数
 * 条目会从缓存中获益，而缓存会很快被这类条目重新填满。
 */
#define SPCACHE_RESET_THRESHOLD		256

static nsphash_hash *SearchPathCache = NULL;
static SearchPathCacheEntry *LastSearchPathCacheEntry = NULL;

/*
 * 根据需要创建或重置 search_path 缓存。
 */
static void
spcache_init(void)
{
	if (SearchPathCache && searchPathCacheValid &&
		SearchPathCache->members < SPCACHE_RESET_THRESHOLD)
		return;

	searchPathCacheValid = false;
	baseSearchPathValid = false;

	/*
	 * 确保在初始化过程中如果发生失败，不会留下悬空指针。
	 */
	SearchPathCache = NULL;
	LastSearchPathCacheEntry = NULL;

	if (SearchPathCacheContext == NULL)
	{
		/* 创建用于保存搜索路径缓存哈希表的上下文 */
		SearchPathCacheContext = AllocSetContextCreate(TopMemoryContext,
													   "search_path processing cache",
													   ALLOCSET_DEFAULT_SIZES);
	}
	else
	{
		MemoryContextReset(SearchPathCacheContext);
	}

	/* 任意指定的初始大小：16 个元素 */
	SearchPathCache = nsphash_create(SearchPathCacheContext, 16, NULL);
	searchPathCacheValid = true;
}

/*
 * 在搜索路径缓存中查找条目但不插入。如果不存在则返回 NULL。
 */
static SearchPathCacheEntry *
spcache_lookup(const char *searchPath, Oid roleid)
{
	if (LastSearchPathCacheEntry &&
		LastSearchPathCacheEntry->key.roleid == roleid &&
		strcmp(LastSearchPathCacheEntry->key.searchPath, searchPath) == 0)
	{
		return LastSearchPathCacheEntry;
	}
	else
	{
		SearchPathCacheEntry *entry;
		SearchPathCacheKey cachekey = {
			.searchPath = searchPath,
			.roleid = roleid
		};

		entry = nsphash_lookup(SearchPathCache, cachekey);
		if (entry)
			LastSearchPathCacheEntry = entry;
		return entry;
	}
}

/*
 * 在搜索路径缓存中查找或插入条目。
 *
 * 安全地初始化键，使得 OOM 不会留下一个没有有效键的条目。
 * 调用方必须确保非键内容已被正确初始化。
 */
static SearchPathCacheEntry *
spcache_insert(const char *searchPath, Oid roleid)
{
	if (LastSearchPathCacheEntry &&
		LastSearchPathCacheEntry->key.roleid == roleid &&
		strcmp(LastSearchPathCacheEntry->key.searchPath, searchPath) == 0)
	{
		return LastSearchPathCacheEntry;
	}
	else
	{
		SearchPathCacheEntry *entry;
		SearchPathCacheKey cachekey = {
			.searchPath = searchPath,
			.roleid = roleid
		};

		/*
		 * searchPath 并未保存在 SearchPathCacheContext 中。先执行一次查找，
		 * 仅当我们需要创建一个新条目时才复制 searchPath。
		 */
		entry = nsphash_lookup(SearchPathCache, cachekey);

		if (!entry)
		{
			bool		found;

			cachekey.searchPath = MemoryContextStrdup(SearchPathCacheContext, searchPath);
			entry = nsphash_insert(SearchPathCache, cachekey, &found);
			Assert(!found);

			entry->oidlist = NIL;
			entry->finalPath = NIL;
			entry->firstNS = InvalidOid;
			entry->temp_missing = false;
			entry->forceRecompute = false;
			/* do not touch entry->status, used by simplehash */
		}

		LastSearchPathCacheEntry = entry;
		return entry;
	}
}

/*
 * RangeVarGetRelidExtended
 *		给定一个描述已存在关系的 RangeVar，
 *		选择合适的命名空间并查找该关系的 OID。
 *
 * 如果未找到模式或关系，当 flags 包含 RVR_MISSING_OK 时返回 InvalidOid，
 * 否则报错。
 *
 * 如果 flags 包含 RVR_NOWAIT，则当我们不得不等待一个锁时会报错。
 *
 * 如果 flags 包含 RVR_SKIP_LOCKED，则当我们不得不等待一个锁时返回 InvalidOid。
 *
 * flags 不能同时包含 RVR_NOWAIT 和 RVR_SKIP_LOCKED。
 *
 * 注意，如果同时指定了 RVR_MISSING_OK 和 RVR_SKIP_LOCKED，那么
 * InvalidOid 的返回值可能表示该关系缺失，也可能表示它无法被加锁。
 *
 * 回调允许调用方在获取关系锁之前检查权限或获取额外的锁。
 */
Oid
RangeVarGetRelidExtended(const RangeVar *relation, LOCKMODE lockmode,
						 uint32 flags,
						 RangeVarGetRelidCallback callback, void *callback_arg)
{
	uint64		inval_count;
	Oid			relId;
	Oid			oldRelId = InvalidOid;
	bool		retry = false;
	bool		missing_ok = (flags & RVR_MISSING_OK) != 0;

	/* 验证这些标志之间没有冲突 */
	Assert(!((flags & RVR_NOWAIT) && (flags & RVR_SKIP_LOCKED)));

	/*
	 * 我们先检查目录名，然后忽略它。
	 */
	if (relation->catalogname)
	{
		if (strcmp(relation->catalogname, get_database_name(MyDatabaseId)) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cross-database references are not implemented: \"%s.%s.%s\"",
							relation->catalogname, relation->schemaname,
							relation->relname)));
	}

	/*
	 * DDL 操作可能会改变名称查找的结果。由于所有此类操作都会生成失效消息，
	 * 我们会跟踪在执行操作期间是否出现了任何此类消息，并不断重试，直到
	 * (1) 不再出现失效消息，或 (2) 答案不再改变。
	 *
	 * 但如果 lockmode = NoLock，则我们假设调用方要么可以接受答案在
	 * 其之下发生变化，要么已经持有了某种适当的锁，因此直接返回我们得到的
	 * 第一个答案，而不检查失效消息。此外，如果所请求的锁已经被持有，
	 * LockRelationOid 不会调用 AcceptInvalidationMessages，因此我们可能
	 * 无法察觉变化。我们本可以在开始这个循环之前调用
	 * AcceptInvalidationMessages() 来防范这种情况，但那样会增加可观的
	 * 开销，所以目前我们不这样做。
	 */
	for (;;)
	{
		/*
		 * 记住这个值，以便在查找关系名称并锁定其 OID 之后，能够检查
		 * 是否处理过任何可能需要重做的失效消息。
		 */
		inval_count = SharedInvalidMessageCounter;

		/*
		 * 可能指定了某个非默认的 relpersistence 值。解析器在简单的 DML 中
		 * 永远不会生成这样的 RangeVar，但在诸如
		 * "CREATE TEMP TABLE foo (f1 int PRIMARY KEY)" 这样的上下文中
		 * 可能会发生。这样的命令会生成一个额外的 CREATE INDEX 操作，
		 * 该操作必须小心地找到临时表，即使在搜索路径中 pg_temp 不排在
		 * 第一位时也是如此。
		 */
		if (relation->relpersistence == RELPERSISTENCE_TEMP)
		{
			if (!OidIsValid(myTempNamespace))
				relId = InvalidOid; /* 这大概不会发生？ */
			else
			{
				if (relation->schemaname)
				{
					Oid			namespaceId;

					namespaceId = LookupExplicitNamespace(relation->schemaname, missing_ok);

			/*
			 * 对于 missing_ok，允许不存在的模式名返回 InvalidOid。
			 */
					if (namespaceId != myTempNamespace)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
								 errmsg("temporary tables cannot specify a schema name")));
				}

				relId = get_relname_relid(relation->relname, myTempNamespace);
			}
		}
		else if (relation->schemaname)
		{
			Oid			namespaceId;

			/* 使用给定的确切模式 */
			namespaceId = LookupExplicitNamespace(relation->schemaname, missing_ok);
			if (missing_ok && !OidIsValid(namespaceId))
				relId = InvalidOid;
			else
				relId = get_relname_relid(relation->relname, namespaceId);
		}
		else
		{
			/* 搜索命名空间路径 */
			relId = RelnameGetRelid(relation->relname);
		}

		/*
		 * 调用调用方提供的回调（如果有）。
		 *
		 * 这个回调是检查权限的好地方：我们还没有获取表锁（而且最好在任何
		 * 加锁之前检查权限！），但我们已经推进到足以知道我们认为应当锁定的
		 * OID。当然，并发的 DDL 可能在等待锁的过程中改变情况，但在那种
		 * 情况下，回调会以新的 OID 再次被调用。
		 */
		if (callback)
			callback(relation, relId, oldRelId, callback_arg);

		/*
		 * 如果没有请求锁，我们假设调用方清楚自己在做什么。他们应该已经在本
		 * 语句的更早处理阶段获取了该关系上的重量级锁，因此在这里调用
		 * AcceptInvalidationMessages() 是不合适的，因为那可能会把地毯从
		 * 他们脚下抽走（使其前功尽弃）。
		 */
		if (lockmode == NoLock)
			break;

		/*
		 * 如果在重试时，我们得到了与上一次相同的 OID，那么我们所处理的
		 * 失效消息并没有改变最终的答案。因此我们就完成了。
		 *
		 * 如果我们得到了不同的 OID，那么我们锁定的是曾经拥有这个名字的
		 * 关系，而不是现在拥有这个名字的关系。因此释放该锁。
		 */
		if (retry)
		{
			if (relId == oldRelId)
				break;
			if (OidIsValid(oldRelId))
				UnlockRelationOid(oldRelId, lockmode);
		}

		/*
		 * 锁定关系。这也会接受任何挂起的失效消息。如果我们得到了
		 * InvalidOid（表示未找到），则没有东西可锁，但我们仍然接受失效
		 * 消息，以冲刷任何可能残留的否定 catcache 条目。
		 */
		if (!OidIsValid(relId))
			AcceptInvalidationMessages();
		else if (!(flags & (RVR_NOWAIT | RVR_SKIP_LOCKED)))
			LockRelationOid(relId, lockmode);
		else if (!ConditionalLockRelationOid(relId, lockmode))
		{
			int			elevel = (flags & RVR_SKIP_LOCKED) ? DEBUG1 : ERROR;

			if (relation->schemaname)
				ereport(elevel,
						(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
						 errmsg("could not obtain lock on relation \"%s.%s\"",
								relation->schemaname, relation->relname)));
			else
				ereport(elevel,
						(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
						 errmsg("could not obtain lock on relation \"%s\"",
								relation->relname)));

			return InvalidOid;
		}

		/*
		 * 如果没有处理任何失效消息，我们就完成了！
		 */
		if (inval_count == SharedInvalidMessageCounter)
			break;

		/*
		 * 可能有东西发生了变化。让我们重复这次名称查找，以确保这个名字
		 * 仍然引用着与它之前所引用相同的那个关系。
		 */
		retry = true;
		oldRelId = relId;
	}

	if (!OidIsValid(relId))
	{
		int			elevel = missing_ok ? DEBUG1 : ERROR;

		if (relation->schemaname)
			ereport(elevel,
					(errcode(ERRCODE_UNDEFINED_TABLE),
					 errmsg("relation \"%s.%s\" does not exist",
							relation->schemaname, relation->relname)));
		else
			ereport(elevel,
					(errcode(ERRCODE_UNDEFINED_TABLE),
					 errmsg("relation \"%s\" does not exist",
							relation->relname)));
	}
	return relId;
}

/*
 * RangeVarGetCreationNamespace
 *\t\t给定一个描述待创建关系的 RangeVar，
 *\t\t选择应在哪个命名空间中创建它。
 *
 * 注意：调用本函数可能会导致一次 CommandCounterIncrement 操作。
 * 这种情况会在任何特定后端运行中首次请求临时表时发生；我们将需要
 * 创建或清空临时模式。
 */
Oid
RangeVarGetCreationNamespace(const RangeVar *newRelation)
{
	Oid			namespaceId;

	/*
	 * 我们先检查目录名，然后忽略它。
	 */
	if (newRelation->catalogname)
	{
		if (strcmp(newRelation->catalogname, get_database_name(MyDatabaseId)) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cross-database references are not implemented: \"%s.%s.%s\"",
							newRelation->catalogname, newRelation->schemaname,
							newRelation->relname)));
	}

	if (newRelation->schemaname)
	{
		/* 检查 pg_temp 别名 */
		if (strcmp(newRelation->schemaname, "pg_temp") == 0)
		{
			/* 初始化临时命名空间 */
			AccessTempTableNamespace(false);
			return myTempNamespace;
		}
		/* 使用给定的确切模式 */
		namespaceId = get_namespace_oid(newRelation->schemaname, false);
		/* 我们在这里不检查 USAGE 权限！ */
	}
	else if (newRelation->relpersistence == RELPERSISTENCE_TEMP)
	{
		/* 初始化临时命名空间 */
		AccessTempTableNamespace(false);
		return myTempNamespace;
	}
	else
	{
		/* 使用默认的创建命名空间 */
		recomputeNamespacePath();
		if (activeTempCreationPending)
		{
			/* 需要初始化临时命名空间 */
			AccessTempTableNamespace(true);
			return myTempNamespace;
		}
		namespaceId = activeCreationNamespace;
		if (!OidIsValid(namespaceId))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_SCHEMA),
					 errmsg("no schema has been selected to create in")));
	}

	/* 注意：调用方会在适当的时候检查 CREATE 权限 */

	return namespaceId;
}

/*
 * RangeVarGetAndCheckCreationNamespace
 *
 * 本函数返回应当在其中创建给定名称的新关系的命名空间的 OID。如果用户
 * 对目标命名空间没有 CREATE 权限，本函数会改为发出一个 ERROR。
 *
 * 如果非 NULL，*existing_relation_id 会被设置为在该命名空间中已存在的、
 * 同名的任何关系的 OID；如果不存在这样的关系，则被设置为 InvalidOid。
 *
 * 如果 lockmode != NoLock，则会在已存在的关系（如果有）上获取指定的锁模式，
 * 前提是当前用户拥有该目标关系。然而，如果 lockmode != NoLock 且用户
 * 并不拥有该目标关系，我们会发出 ERROR，因为我们绝不能尝试锁定用户
 * 没有权限的关系。
 *
 * 作为一个副作用，本函数会在目标命名空间上获取 AccessShareLock。如果没有
 * 它，该命名空间可能会在我们的事务提交之前被删除，从而留下 relnamespace
 * 指向一个已不存在的命名空间的关系。
 *
 * 作为进一步的副作用，如果所选的命名空间是一个临时命名空间，我们会将
 * RangeVar 标记为 RELPERSISTENCE_TEMP。
 */
Oid
RangeVarGetAndCheckCreationNamespace(RangeVar *relation,
									 LOCKMODE lockmode,
									 Oid *existing_relation_id)
{
	uint64		inval_count;
	Oid			relid;
	Oid			oldrelid = InvalidOid;
	Oid			nspid;
	Oid			oldnspid = InvalidOid;
	bool		retry = false;

	/*
	 * 我们先检查目录名，然后忽略它。
	 */
	if (relation->catalogname)
	{
		if (strcmp(relation->catalogname, get_database_name(MyDatabaseId)) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cross-database references are not implemented: \"%s.%s.%s\"",
							relation->catalogname, relation->schemaname,
							relation->relname)));
	}

	/*
	 * 与 RangeVarGetRelidExtended() 中一样，我们通过跟踪在执行名称查找
	 * 和获取锁期间是否处理了任何失效消息，来防范并发的 DDL 操作。有关
	 * 该逻辑的更详细说明，请参阅那个函数中的注释。
	 */
	for (;;)
	{
		AclResult	aclresult;

		inval_count = SharedInvalidMessageCounter;

		/* 查找创建命名空间并检查是否已存在关系。 */
		nspid = RangeVarGetCreationNamespace(relation);
		Assert(OidIsValid(nspid));
		if (existing_relation_id != NULL)
			relid = get_relname_relid(relation->relname, nspid);
		else
			relid = InvalidOid;

		/*
		 * 在 bootstrap 处理模式下，我们不去操心权限或加锁。权限可能尚未
		 * 生效，而且加锁也是不必要的。
		 */
		if (IsBootstrapProcessingMode())
			break;

		/* 检查命名空间权限。 */
		aclresult = object_aclcheck(NamespaceRelationId, nspid, GetUserId(), ACL_CREATE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_SCHEMA,
						   get_namespace_name(nspid));

		if (retry)
		{
			/* 如果没有变化，我们就完成了。 */
			if (relid == oldrelid && nspid == oldnspid)
				break;
			/* 如果创建命名空间发生了变化，则放弃旧的锁。 */
			if (nspid != oldnspid)
				UnlockDatabaseObject(NamespaceRelationId, oldnspid, 0,
									 AccessShareLock);
			/* 如果名称指向了不同的东西，则放弃旧的锁。 */
			if (relid != oldrelid && OidIsValid(oldrelid) && lockmode != NoLock)
				UnlockRelationOid(oldrelid, lockmode);
		}

		/* 锁定命名空间。 */
		if (nspid != oldnspid)
			LockDatabaseObject(NamespaceRelationId, nspid, 0, AccessShareLock);

		/* 如果需要且我们有权限，则锁定关系。 */
		if (lockmode != NoLock && OidIsValid(relid))
		{
			if (!object_ownercheck(RelationRelationId, relid, GetUserId()))
				aclcheck_error(ACLCHECK_NOT_OWNER, get_relkind_objtype(get_rel_relkind(relid)),
							   relation->relname);
			if (relid != oldrelid)
				LockRelationOid(relid, lockmode);
		}

		/* 如果没有处理任何失效消息，我们就完成了！ */
		if (inval_count == SharedInvalidMessageCounter)
			break;

		/* 可能有东西发生了变化，因此重新检查我们的工作。 */
		retry = true;
		oldrelid = relid;
		oldnspid = nspid;
	}

	RangeVarAdjustRelationPersistence(relation, nspid);
	if (existing_relation_id != NULL)
		*existing_relation_id = relid;
	return nspid;
}

/*
 * 根据创建命名空间调整一个即将被创建的关系的 relpersistence，
 * 并对无效的组合报错。
 */
void
RangeVarAdjustRelationPersistence(RangeVar *newRelation, Oid nspid)
{
	switch (newRelation->relpersistence)
	{
		case RELPERSISTENCE_TEMP:
			if (!isTempOrTempToastNamespace(nspid))
			{
				if (isAnyTempNamespace(nspid))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("cannot create relations in temporary schemas of other sessions")));
				else
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
							 errmsg("cannot create temporary relation in non-temporary schema")));
			}
			break;
		case RELPERSISTENCE_PERMANENT:
			if (isTempOrTempToastNamespace(nspid))
				newRelation->relpersistence = RELPERSISTENCE_TEMP;
			else if (isAnyTempNamespace(nspid))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("cannot create relations in temporary schemas of other sessions")));
			break;
		default:
			if (isAnyTempNamespace(nspid))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						 errmsg("only temporary relations may be created in temporary schemas")));
	}
}

/*
 * RelnameGetRelid
 *\t\t尝试解析一个非限定关系名。
 *\t\t如果在搜索路径中找到关系则返回其 OID，否则返回 InvalidOid。
 */
Oid
RelnameGetRelid(const char *relname)
{
	Oid			relid;
	ListCell   *l;

	recomputeNamespacePath();

	foreach(l, activeSearchPath)
	{
		Oid			namespaceId = lfirst_oid(l);

		relid = get_relname_relid(relname, namespaceId);
		if (OidIsValid(relid))
			return relid;
	}

	/* 在路径中未找到 */
	return InvalidOid;
}


/*
 * RelationIsVisible
 *\t\t判断一个关系（由 OID 标识）在当前搜索路径中是否可见。
 *\t\t“可见”是指“通过搜索非限定关系名可以找到它”。
 */
bool
RelationIsVisible(Oid relid)
{
	return RelationIsVisibleExt(relid, NULL);
}

/*
 * RelationIsVisibleExt
 *\t\t与上面相同，但如果关系未找到且 is_missing 不为 NULL，则
 *\t\t将 *is_missing 置为 true 并返回 false，而不是抛出一个错误。
 *\t\t（调用方必须将 *is_missing 初始化为 false。）
 */
static bool
RelationIsVisibleExt(Oid relid, bool *is_missing)
{
	HeapTuple	reltup;
	Form_pg_class relform;
	Oid			relnamespace;
	bool		visible;

	reltup = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(reltup))
	{
		if (is_missing != NULL)
		{
			*is_missing = true;
			return false;
		}
		elog(ERROR, "cache lookup failed for relation %u", relid);
	}
	relform = (Form_pg_class) GETSTRUCT(reltup);

	recomputeNamespacePath();

/*
 * 快速检查：如果它根本不在搜索路径中，那它肯定不可见。系统命名空间中的
 * 项肯定在路径中，因此我们甚至不需要对它们调用 list_member_oid()。
 */
	relnamespace = relform->relnamespace;
	if (relnamespace != PG_CATALOG_NAMESPACE &&
		!list_member_oid(activeSearchPath, relnamespace))
		visible = false;
	else
	{
		/*
		 * 如果它在路径中，它可能仍然不可见；它可能被路径中更早出现的、
		 * 同名的另一个关系所遮蔽。因此我们必须做一次较慢的检查，以查找
		 * 冲突的关系。
		 */
		char	   *relname = NameStr(relform->relname);
		ListCell   *l;

		visible = false;
		foreach(l, activeSearchPath)
		{
			Oid			namespaceId = lfirst_oid(l);

			if (namespaceId == relnamespace)
			{
				/* Found it first in path */
				visible = true;
				break;
			}
			if (OidIsValid(get_relname_relid(relname, namespaceId)))
			{
				/* Found something else first in path */
				break;
			}
		}
	}

	ReleaseSysCache(reltup);

	return visible;
}


/*
 * TypenameGetTypid
 *\t\t为二进制兼容性提供的包装函数。
 */
Oid
TypenameGetTypid(const char *typname)
{
	return TypenameGetTypidExtended(typname, true);
}

/*
 * TypenameGetTypidExtended
 *\t\t尝试解析一个非限定数据类型名。
 *\t\t如果在搜索路径中找到类型则返回其 OID，否则返回 InvalidOid。
 *
 * 这与 RelnameGetRelid 本质上相同。
 */
Oid
TypenameGetTypidExtended(const char *typname, bool temp_ok)
{
	Oid			typid;
	ListCell   *l;

	recomputeNamespacePath();

	foreach(l, activeSearchPath)
	{
		Oid			namespaceId = lfirst_oid(l);

		if (!temp_ok && namespaceId == myTempNamespace)
			continue;			/* 不要在临时命名空间中查找 */

		typid = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
								PointerGetDatum(typname),
								ObjectIdGetDatum(namespaceId));
		if (OidIsValid(typid))
			return typid;
	}

	/* 在路径中未找到 */
	return InvalidOid;
}

/*
 * TypeIsVisible
 *\t\t判断一个类型（由 OID 标识）在当前搜索路径中是否可见。
 *\t\t“可见”是指“通过搜索非限定类型名可以找到它”。
 */
bool
TypeIsVisible(Oid typid)
{
	return TypeIsVisibleExt(typid, NULL);
}

/*
 * TypeIsVisibleExt
 *\t\t与上面相同，但如果类型未找到且 is_missing 不为 NULL，则
 *\t\t将 *is_missing 置为 true 并返回 false，而不是抛出一个错误。
 *\t\t（调用方必须将 *is_missing 初始化为 false。）
 */
static bool
TypeIsVisibleExt(Oid typid, bool *is_missing)
{
	HeapTuple	typtup;
	Form_pg_type typform;
	Oid			typnamespace;
	bool		visible;

	typtup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (!HeapTupleIsValid(typtup))
	{
		if (is_missing != NULL)
		{
			*is_missing = true;
			return false;
		}
		elog(ERROR, "cache lookup failed for type %u", typid);
	}
	typform = (Form_pg_type) GETSTRUCT(typtup);

	recomputeNamespacePath();

/*
 * 快速检查：如果它根本不在搜索路径中，那它肯定不可见。系统命名空间中的
 * 项肯定在路径中，因此我们甚至不需要对它们调用 list_member_oid()。
 */
	typnamespace = typform->typnamespace;
	if (typnamespace != PG_CATALOG_NAMESPACE &&
		!list_member_oid(activeSearchPath, typnamespace))
		visible = false;
	else
	{
		/*
		 * 如果它在路径中，它可能仍然不可见；它可能被路径中更早出现的、
		 * 同名的另一个类型所遮蔽。因此我们必须做一次较慢的检查，以查找
		 * 冲突的类型。
		 */
		char	   *typname = NameStr(typform->typname);
		ListCell   *l;

		visible = false;
		foreach(l, activeSearchPath)
		{
			Oid			namespaceId = lfirst_oid(l);

			if (namespaceId == typnamespace)
			{
				/* Found it first in path */
				visible = true;
				break;
			}
			if (SearchSysCacheExists2(TYPENAMENSP,
									  PointerGetDatum(typname),
									  ObjectIdGetDatum(namespaceId)))
			{
				/* Found something else first in path */
				break;
			}
		}
	}

	ReleaseSysCache(typtup);

	return visible;
}


/*
 * FuncnameGetCandidates
 *\t\t给定一个可能限定的函数名以及参数个数，
 *\t\t检索出可能的匹配项列表。
 *
 * 如果 nargs 为 -1，则返回所有匹配给定名称的函数，而不管参数个数如何。
 * （在这种情况下，argnames 必须为 NIL，且 expand_variadic 与 expand_defaults
 * 必须为 false。）
 *
 * 如果 argnames 不为 NIL，我们考虑的是命名表示法或混合表示法的调用，
 * 只会返回拥有所列全部参数名的函数。（我们假设 length(argnames) <= nargs，
 * 且所有传入的名称都是不同的。）返回的结构体将包含一个 argnumbers 数组，
 * 用于显示每个逻辑参数位置对应的实际参数索引。
 *
 * 如果 expand_variadic 为 true，那么参数个数相同或更少的可变参数函数
 * 也会被检索出来，其中可变参数以及任何额外的参数位置都用可变参数的
 * 元素类型填充。返回结构体中的 nvargs 被设置为这类参数的个数。
 * 如果 expand_variadic 为 false，则可变参数不会被特殊对待，返回的 nvargs
 * 将始终为零。
 *
 * 如果 expand_defaults 为 true，那么在插入默认参数值之后能够匹配的函数
 * 也会被检索出来。在这种情况下，返回的结构体可能具有 nargs > 传入的 nargs，
 * 而 ndargs 被设置为额外参数的个数（可以从函数的 proargdefaults 条目中
 * 获取）。
 *
 * 如果 include_out_arguments 为 true，那么 OUT 模式的参数会被视为包含在
 * 参数列表中。它们的类型会被包含在返回的数组中，且 argnumbers 是
 * proallargtypes 中的索引，而非 proargtypes 中的索引。我们还会将
 * nominalnargs 设置为 proallargtypes 的长度，而非 proargtypes 的长度。
 * 否则，OUT 模式的参数会被忽略。
 *
 * 在同一个列表条目中，nvargs 与 ndargs 不可能同时非零，因为默认值的插入
 * 允许匹配参数个数多于 nargs 的函数，而可变参数的变换要求参数个数相同
 * 或更少。
 *
 * 当 argnames 不为 NIL 时，返回的 args[] 类型数组并非按照函数的声明顺序
 * 排列，而是按照调用排列：先是任何位置参数，然后是命名参数，再然后是
 * 默认参数（如果需要且 expand_defaults 允许）。argnumbers[] 数组可用于
 * 将其映射回目录信息。argnumbers[k] 被设置为第 k 个调用参数在 proargtypes
 * 或 proallargtypes 中的索引。
 *
 * 如果函数名是限定的，我们搜索单个命名空间；否则搜索搜索路径中的
 * 所有命名空间。在多命名空间的情况下，我们会让较早命名空间中的条目
 * 遮蔽较晚命名空间中相同的条目。
 *
 * 在展开可变参数时，如果展开后的参数列表相同，我们会让非可变参数函数
 * 遮蔽可变参数函数。不过，不同的可变参数函数之间仍然可能存在冲突。
 *
 * 保证返回的列表永远不会包含多个具有完全相同参数列表的条目。当
 * expand_defaults 为 true 时，条目可能具有多于 nargs 个位置，但我们仍然
 * 保证它们在前 nargs 个位置上是互不相同的。然而，如果 argnames 不为 NIL，
 * 或者 expand_variadic 或 expand_defaults 为 true，则可能存在多个展开后
 * 得到相同参数列表的候选函数。我们不会在此处报错，而是通过返回一个
 * oid = 0 的单一条目来报告这种情况，该条目代表一组相互冲突的候选函数。
 * 调用方最终可能会丢弃这样的条目，但如果它选中了这样的条目，则应当
 * 像遇到歧义调用那样做出反应。
 *
 * 如果 missing_ok 为 true，则当该名称以模式限定、而该模式不存在时，
 * 返回一个空列表（NULL）。同样地，如果由于其他原因没有找到候选函数，
 * 也会返回空列表。
 */
FuncCandidateList
FuncnameGetCandidates(List *names, int nargs, List *argnames,
					  bool expand_variadic, bool expand_defaults,
					  bool include_out_arguments, bool missing_ok)
{
	FuncCandidateList resultList = NULL;
	bool		any_special = false;
	char	   *schemaname;
	char	   *funcname;
	Oid			namespaceId;
	CatCList   *catlist;
	int			i;

	/* check for caller error */
	Assert(nargs >= 0 || !(expand_variadic | expand_defaults));

	/* 拆分名称列表 */
	DeconstructQualifiedName(names, &schemaname, &funcname);

	if (schemaname)
	{
		/* 使用给定的确切模式 */
		namespaceId = LookupExplicitNamespace(schemaname, missing_ok);
		if (!OidIsValid(namespaceId))
			return NULL;
	}
	else
	{
		/* flag to indicate we need namespace search */
		namespaceId = InvalidOid;
		recomputeNamespacePath();
	}

	/* Search syscache by name only */
	catlist = SearchSysCacheList1(PROCNAMEARGSNSP, CStringGetDatum(funcname));

	for (i = 0; i < catlist->n_members; i++)
	{
		HeapTuple	proctup = &catlist->members[i]->tuple;
		Form_pg_proc procform = (Form_pg_proc) GETSTRUCT(proctup);
		Oid		   *proargtypes = procform->proargtypes.values;
		int			pronargs = procform->pronargs;
		int			effective_nargs;
		int			pathpos = 0;
		bool		variadic;
		bool		use_defaults;
		Oid			va_elem_type;
		int		   *argnumbers = NULL;
		FuncCandidateList newResult;

		if (OidIsValid(namespaceId))
		{
			/* 只考虑指定命名空间中的过程 */
			if (procform->pronamespace != namespaceId)
				continue;
		}
		else
		{
			/*
			 * 只考虑那些位于搜索路径中、且不在临时命名空间里的过程。
			 */
			ListCell   *nsp;

			foreach(nsp, activeSearchPath)
			{
				if (procform->pronamespace == lfirst_oid(nsp) &&
					procform->pronamespace != myTempNamespace)
					break;
				pathpos++;
			}
			if (nsp == NULL)
				continue;		/* proc is not in search path */
		}

		/*
		 * 如果要求我们匹配 OUT 参数，则使用 proallargtypes 数组（它包含
		 * 那些 OUT 参数）；否则使用 proargtypes（它不包含）。当然，如果
		 * proallargtypes 为 null，则我们总是使用 proargtypes。
		 */
		if (include_out_arguments)
		{
			Datum		proallargtypes;
			bool		isNull;

			proallargtypes = SysCacheGetAttr(PROCNAMEARGSNSP, proctup,
											 Anum_pg_proc_proallargtypes,
											 &isNull);
			if (!isNull)
			{
				ArrayType  *arr = DatumGetArrayTypeP(proallargtypes);

				pronargs = ARR_DIMS(arr)[0];
				if (ARR_NDIM(arr) != 1 ||
					pronargs < 0 ||
					ARR_HASNULL(arr) ||
					ARR_ELEMTYPE(arr) != OIDOID)
					elog(ERROR, "proallargtypes is not a 1-D Oid array or it contains nulls");
				Assert(pronargs >= procform->pronargs);
				proargtypes = (Oid *) ARR_DATA_PTR(arr);
			}
		}

		if (argnames != NIL)
		{
			/*
			 * 调用使用了命名表示法或混合表示法
			 *
			 * 命名或混合表示法只有在 expand_variadic 关闭时才能匹配可变
			 * 参数函数；否则无法匹配从可变参数数组展开出来的那些假定无名的
			 * 参数。
			 */
			if (OidIsValid(procform->provariadic) && expand_variadic)
				continue;
			va_elem_type = InvalidOid;
			variadic = false;

			/*
			 * 检查参数个数。
			 */
			Assert(nargs >= 0); /* -1 not supported with argnames */

			if (pronargs > nargs && expand_defaults)
			{
				/* 如果默认表达式不够则忽略 */
				if (nargs + procform->pronargdefaults < pronargs)
					continue;
				use_defaults = true;
			}
			else
				use_defaults = false;

			/* 如果与所请求的参数个数不匹配则忽略 */
			if (pronargs != nargs && !use_defaults)
				continue;

			/* 检查参数名是否匹配，并生成位置映射 */
			if (!MatchNamedCall(proctup, nargs, argnames,
								include_out_arguments, pronargs,
								&argnumbers))
				continue;

			/* 命名参数匹配始终是“special” */
			any_special = true;
		}
		else
		{
			/*
			 * 调用使用了位置表示法
			 *
			 * 检查函数是否为可变参数，如果是则获取可变参数的元素类型。
			 * 如果 expand_variadic 为 false，则我们只需忽略其可变参数特性。
			 */
			if (pronargs <= nargs && expand_variadic)
			{
				va_elem_type = procform->provariadic;
				variadic = OidIsValid(va_elem_type);
				any_special |= variadic;
			}
			else
			{
				va_elem_type = InvalidOid;
				variadic = false;
			}

			/*
			 * 检查函数是否能通过参数默认值来匹配。
			 */
			if (pronargs > nargs && expand_defaults)
			{
				/* 如果默认表达式不够则忽略 */
				if (nargs + procform->pronargdefaults < pronargs)
					continue;
				use_defaults = true;
				any_special = true;
			}
			else
				use_defaults = false;

			/* 如果与所请求的参数个数不匹配则忽略 */
			if (nargs >= 0 && pronargs != nargs && !variadic && !use_defaults)
				continue;
		}

		/*
		 * 我们必须计算出有效的参数列表，以便能够与先前的结果轻松比较。
		 * 如果它被一个更早的结果所遮蔽，我们会浪费一次 palloc，但这确实
		 * 是一种相当少见的情况，因此不值得为此担心。
		 */
		effective_nargs = Max(pronargs, nargs);
		newResult = (FuncCandidateList)
			palloc(offsetof(struct _FuncCandidateList, args) +
				   effective_nargs * sizeof(Oid));
		newResult->pathpos = pathpos;
		newResult->oid = procform->oid;
		newResult->nominalnargs = pronargs;
		newResult->nargs = effective_nargs;
		newResult->argnumbers = argnumbers;
		if (argnumbers)
		{
			/* 将参数类型重新排序为调用的逻辑顺序 */
			for (int j = 0; j < pronargs; j++)
				newResult->args[j] = proargtypes[argnumbers[j]];
		}
		else
		{
			/* 简单位置参数的情况，直接按原样复制 proargtypes */
			memcpy(newResult->args, proargtypes, pronargs * sizeof(Oid));
		}
		if (variadic)
		{
			newResult->nvargs = effective_nargs - pronargs + 1;
			/* 将可变参数展开为 N 份元素类型的副本 */
			for (int j = pronargs - 1; j < effective_nargs; j++)
				newResult->args[j] = va_elem_type;
		}
		else
			newResult->nvargs = 0;
		newResult->ndargs = use_defaults ? pronargs - nargs : 0;

		/*
		 * 它是否与我们已经接受的某个结果具有相同的参数？如果是，则决定
		 * 如何处理，以避免返回重复的参数列表。对于单命名空间的情况，如果
		 * 还没有产生任何特殊（命名、可变参数或默认值）匹配，我们可以跳过
		 * 此检查，因为那样的话 pg_proc 上的唯一索引保证了所有匹配都具有
		 * 不同的参数列表。
		 */
		if (resultList != NULL &&
			(any_special || !OidIsValid(namespaceId)))
		{
			/*
			 * 如果我们从 SearchSysCacheList 得到的是一个有序列表（正常情况），
			 * 那么任何冲突的过程必定紧邻地排列在这个过程之前，因此我们只需要
			 * 查看最新的结果项。如果我们得到的是无序列表，则必须扫描整个
			 * 结果列表。此外，如果当前候选或任何先前候选是一个特殊匹配，
			 * 我们就不能假定冲突是相邻的。
			 *
			 * 在判断什么是匹配时，我们会忽略带有默认值的参数。
			 */
			FuncCandidateList prevResult;

			if (catlist->ordered && !any_special)
			{
				/* ndargs must be 0 if !any_special */
				if (effective_nargs == resultList->nargs &&
					memcmp(newResult->args,
						   resultList->args,
						   effective_nargs * sizeof(Oid)) == 0)
					prevResult = resultList;
				else
					prevResult = NULL;
			}
			else
			{
				int			cmp_nargs = newResult->nargs - newResult->ndargs;

				for (prevResult = resultList;
					 prevResult;
					 prevResult = prevResult->next)
				{
					if (cmp_nargs == prevResult->nargs - prevResult->ndargs &&
						memcmp(newResult->args,
							   prevResult->args,
							   cmp_nargs * sizeof(Oid)) == 0)
						break;
				}
			}

			if (prevResult)
			{
			/*
			 * 我们找到了与先前结果的匹配。决定保留哪一个，或者在无法决定时
			 * 将其标记为歧义。此处的逻辑是：preference > 0 表示保留旧的
			 * 结果，preference < 0 表示保留新的，preference = 0 表示歧义。
			 */
				int			preference;

				if (pathpos != prevResult->pathpos)
				{
					/*
					 * 优先选择搜索路径中排在前面的那个。
					 */
					preference = pathpos - prevResult->pathpos;
				}
				else if (variadic && prevResult->nvargs == 0)
				{
				/*
				 * 以可变参数函数为例，我们可能会在同一命名空间中同时拥有
				 * foo(numeric) 和 foo(variadic numeric[])；如果是这样，
				 * 出于效率考虑，我们优先选择非可变参数的匹配。
				 */
					preference = 1;
				}
				else if (!variadic && prevResult->nvargs > 0)
				{
					preference = -1;
				}
				else
				{
				/*----------
				 * 我们无法决定。例如，以下情况都可能发生：在同一个命名空间中
				 * 同时存在 foo(numeric, variadic numeric[]) 和
				 * foo(variadic numeric[])；或者同时存在 foo(int) 和
				 * foo (int, int default something)；或者同时存在
				 * foo(a int, b text) 和 foo(b text, a int)。
				 *----------
				 */
					preference = 0;
				}

				if (preference > 0)
				{
					/* 保留先前的结果 */
					pfree(newResult);
					continue;
				}
				else if (preference < 0)
				{
					/* 从列表中移除先前的结果 */
					if (prevResult == resultList)
						resultList = prevResult->next;
					else
					{
						FuncCandidateList prevPrevResult;

						for (prevPrevResult = resultList;
							 prevPrevResult;
							 prevPrevResult = prevPrevResult->next)
						{
							if (prevResult == prevPrevResult->next)
							{
								prevPrevResult->next = prevResult->next;
								break;
							}
						}
						Assert(prevPrevResult); /* assert we found it */
					}
					pfree(prevResult);
					/* fall through to add newResult to list */
				}
				else
				{
					/* 将旧结果标记为歧义，丢弃新的 */
					prevResult->oid = InvalidOid;
					pfree(newResult);
					continue;
				}
			}
		}

		/*
		 * 可以把它加入结果列表
		 */
		newResult->next = resultList;
		resultList = newResult;
	}

	ReleaseSysCacheList(catlist);

	return resultList;
}

/*
 * MatchNamedCall
 *\t\t给定一个 pg_proc 堆元组和一次调用的参数名列表，
 *\t\t检查该函数是否可能匹配该调用。
 *
 * 如果所提供的所有参数名都被该函数接受，且位于最后一个位置参数之后的
 * 位置，并且所有未提供的参数都有默认值，那么该调用就可以匹配。
 *
 * 如果 include_out_arguments 为 true，我们将 OUT 参数视为包含在参数列表中。
 * pronargs 是我们所考虑的参数个数（proargtypes 或 proallargtypes 的长度）。
 *
 * 位置参数的个数为 nargs - list_length(argnames)。注意调用方已经对参数
 * 个数做了基本检查。
 *
 * 如果匹配，则返回 true，并用一个 palloc 出来的数组填充 *argnumbers，
 * 该数组显示了从调用参数位置到实际函数参数编号的映射。默认参数也包含
 * 在此映射中，位于最后一个已提供参数之后的位置。
 */
static bool
MatchNamedCall(HeapTuple proctup, int nargs, List *argnames,
			   bool include_out_arguments, int pronargs,
			   int **argnumbers)
{
	Form_pg_proc procform = (Form_pg_proc) GETSTRUCT(proctup);
	int			numposargs = nargs - list_length(argnames);
	int			pronallargs;
	Oid		   *p_argtypes;
	char	  **p_argnames;
	char	   *p_argmodes;
	bool		arggiven[FUNC_MAX_ARGS];
	bool		isnull;
	int			ap;				/* call args position */
	int			pp;				/* proargs position */
	ListCell   *lc;

	Assert(argnames != NIL);
	Assert(numposargs >= 0);
	Assert(nargs <= pronargs);

	/* 如果其 proargnames 为 null，则忽略这个函数 */
	(void) SysCacheGetAttr(PROCOID, proctup, Anum_pg_proc_proargnames,
						   &isnull);
	if (isnull)
		return false;

	/* 好的，让我们提取参数名和类型 */
	pronallargs = get_func_arg_info(proctup,
									&p_argtypes, &p_argnames, &p_argmodes);
	Assert(p_argnames != NULL);

	Assert(include_out_arguments ? (pronargs == pronallargs) : (pronargs <= pronallargs));

	/* 初始化用于匹配的状态 */
	*argnumbers = (int *) palloc(pronargs * sizeof(int));
	memset(arggiven, false, pronargs * sizeof(bool));

	/* 在命名参数之前有 numposargs 个位置参数 */
	for (ap = 0; ap < numposargs; ap++)
	{
		(*argnumbers)[ap] = ap;
		arggiven[ap] = true;
	}

	/* 现在检查命名参数 */
	foreach(lc, argnames)
	{
		char	   *argname = (char *) lfirst(lc);
		bool		found;
		int			i;

		pp = 0;
		found = false;
		for (i = 0; i < pronallargs; i++)
		{
			/* 只考虑输入参数，除非指定了 include_out_arguments */
			if (!include_out_arguments &&
				p_argmodes &&
				(p_argmodes[i] != FUNC_PARAM_IN &&
				 p_argmodes[i] != FUNC_PARAM_INOUT &&
				 p_argmodes[i] != FUNC_PARAM_VARIADIC))
				continue;
			if (p_argnames[i] && strcmp(p_argnames[i], argname) == 0)
			{
				/* 如果参数名与一个位置参数匹配则失败 */
				if (arggiven[pp])
					return false;
				arggiven[pp] = true;
				(*argnumbers)[ap] = pp;
				found = true;
				break;
			}
			/* 只对考虑到的参数递增 pp */
			pp++;
		}
		/* 如果名称不在 proargnames 中则失败 */
		if (!found)
			return false;
		ap++;
	}

	Assert(ap == nargs);		/* processed all actual parameters */

	/* 检查默认参数 */
	if (nargs < pronargs)
	{
		int			first_arg_with_default = pronargs - procform->pronargdefaults;

		for (pp = numposargs; pp < pronargs; pp++)
		{
			if (arggiven[pp])
				continue;
			/* 如果未提供参数且没有可用的默认值则失败 */
			if (pp < first_arg_with_default)
				return false;
			(*argnumbers)[ap++] = pp;
		}
	}

	Assert(ap == pronargs);		/* processed all function parameters */

	return true;
}

/*
 * FunctionIsVisible
 *\t\t判断一个函数（由 OID 标识）在当前搜索路径中是否可见。
 *\t\t“可见”是指“通过搜索非限定函数名并以精确的参数匹配可以找到它”。
 */
bool
FunctionIsVisible(Oid funcid)
{
	return FunctionIsVisibleExt(funcid, NULL);
}

/*
 * FunctionIsVisibleExt
 *\t\t与上面相同，但如果函数未找到且 is_missing 不为 NULL，则
 *\t\t将 *is_missing 置为 true 并返回 false，而不是抛出一个错误。
 *\t\t（调用方必须将 *is_missing 初始化为 false。）
 */
static bool
FunctionIsVisibleExt(Oid funcid, bool *is_missing)
{
	HeapTuple	proctup;
	Form_pg_proc procform;
	Oid			pronamespace;
	bool		visible;

	proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(proctup))
	{
		if (is_missing != NULL)
		{
			*is_missing = true;
			return false;
		}
		elog(ERROR, "cache lookup failed for function %u", funcid);
	}
	procform = (Form_pg_proc) GETSTRUCT(proctup);

	recomputeNamespacePath();

/*
 * 快速检查：如果它根本不在搜索路径中，那它肯定不可见。系统命名空间中的
 * 项肯定在路径中，因此我们甚至不需要对它们调用 list_member_oid()。
 */
	pronamespace = procform->pronamespace;
	if (pronamespace != PG_CATALOG_NAMESPACE &&
		!list_member_oid(activeSearchPath, pronamespace))
		visible = false;
	else
	{
		/*
		 * 如果它在路径中，它可能仍然不可见；它可能被路径中更早出现的、
		 * 同名同参数的另一个过程所遮蔽。因此我们必须做一次较慢的检查，
		 * 以确认这是否就是 FuncnameGetCandidates 会找到的那个过程。
		 */
		char	   *proname = NameStr(procform->proname);
		int			nargs = procform->pronargs;
		FuncCandidateList clist;

		visible = false;

		clist = FuncnameGetCandidates(list_make1(makeString(proname)),
									  nargs, NIL, false, false, false, false);

		for (; clist; clist = clist->next)
		{
			if (memcmp(clist->args, procform->proargtypes.values,
					   nargs * sizeof(Oid)) == 0)
			{
				/* Found the expected entry; is it the right proc? */
				visible = (clist->oid == funcid);
				break;
			}
		}
	}

	ReleaseSysCache(proctup);

	return visible;
}


/*
 * OpernameGetOprid
 *\t\t给定一个可能限定的操作符名以及精确的输入数据类型，
 *\t\t查找该操作符。如果未找到则返回 InvalidOid。
 *
 * 对于前缀操作符，传入 oprleft = InvalidOid。
 *
 * 如果操作符名不是模式限定的，则会在当前的命名空间搜索路径中查找。
 * 如果名称是模式限定的、而给定的模式不存在，则返回 InvalidOid。
 */
Oid
OpernameGetOprid(List *names, Oid oprleft, Oid oprright)
{
	char	   *schemaname;
	char	   *opername;
	CatCList   *catlist;
	ListCell   *l;

	/* 拆分名称列表 */
	DeconstructQualifiedName(names, &schemaname, &opername);

	if (schemaname)
	{
		/* 只在给定的确切模式中搜索 */
		Oid			namespaceId;

		namespaceId = LookupExplicitNamespace(schemaname, true);
		if (OidIsValid(namespaceId))
		{
			HeapTuple	opertup;

			opertup = SearchSysCache4(OPERNAMENSP,
									  CStringGetDatum(opername),
									  ObjectIdGetDatum(oprleft),
									  ObjectIdGetDatum(oprright),
									  ObjectIdGetDatum(namespaceId));
			if (HeapTupleIsValid(opertup))
			{
				Form_pg_operator operclass = (Form_pg_operator) GETSTRUCT(opertup);
				Oid			result = operclass->oid;

				ReleaseSysCache(opertup);
				return result;
			}
		}

		return InvalidOid;
	}

	/* Search syscache by name and argument types */
	catlist = SearchSysCacheList3(OPERNAMENSP,
								  CStringGetDatum(opername),
								  ObjectIdGetDatum(oprleft),
								  ObjectIdGetDatum(oprright));

	if (catlist->n_members == 0)
	{
		/* 没有希望，提前退出 */
		ReleaseSysCacheList(catlist);
		return InvalidOid;
	}

	/*
	 * We have to find the list member that is first in the search path, if
	 * there's more than one.  This doubly-nested loop looks ugly, but in
	 * practice there should usually be few catlist members.
	 */
	recomputeNamespacePath();

	foreach(l, activeSearchPath)
	{
		Oid			namespaceId = lfirst_oid(l);
		int			i;

		if (namespaceId == myTempNamespace)
			continue;			/* 不要在临时命名空间中查找 */

		for (i = 0; i < catlist->n_members; i++)
		{
			HeapTuple	opertup = &catlist->members[i]->tuple;
			Form_pg_operator operform = (Form_pg_operator) GETSTRUCT(opertup);

			if (operform->oprnamespace == namespaceId)
			{
				Oid			result = operform->oid;

				ReleaseSysCacheList(catlist);
				return result;
			}
		}
	}

	ReleaseSysCacheList(catlist);
	return InvalidOid;
}

/*
 * OpernameGetCandidates
 *\t\t给定一个可能限定的操作符名以及操作符种类，
 *\t\t检索出可能的匹配项列表。
 *
 * 如果 oprkind 为 '\0'，则返回所有匹配给定名称的操作符，
 * 而不管其参数如何。
 *
 * 如果操作符名是限定的，我们搜索单个命名空间；否则搜索搜索路径中的
 * 所有命名空间。返回的列表永远不会包含多个具有完全相同参数列表的条目
 * --- 在多命名空间的情况下，我们会让较早命名空间中的条目遮蔽
 * 较晚命名空间中相同的条目。
 *
 * 返回的项总是有两个 args[] 条目 --- 对于前缀操作符种类，第一个
 * 将为 InvalidOid。nargs 也总是 2。
 */
FuncCandidateList
OpernameGetCandidates(List *names, char oprkind, bool missing_schema_ok)
{
	FuncCandidateList resultList = NULL;
	char	   *resultSpace = NULL;
	int			nextResult = 0;
	char	   *schemaname;
	char	   *opername;
	Oid			namespaceId;
	CatCList   *catlist;
	int			i;

	/* 拆分名称列表 */
	DeconstructQualifiedName(names, &schemaname, &opername);

	if (schemaname)
	{
		/* 使用给定的确切模式 */
		namespaceId = LookupExplicitNamespace(schemaname, missing_schema_ok);
		if (missing_schema_ok && !OidIsValid(namespaceId))
			return NULL;
	}
	else
	{
		/* flag to indicate we need namespace search */
		namespaceId = InvalidOid;
		recomputeNamespacePath();
	}

	/* Search syscache by name only */
	catlist = SearchSysCacheList1(OPERNAMENSP, CStringGetDatum(opername));

	/*
	 * In typical scenarios, most if not all of the operators found by the
	 * catcache search will end up getting returned; and there can be quite a
	 * few, for common operator names such as '=' or '+'.  To reduce the time
	 * spent in palloc, we allocate the result space as an array large enough
	 * to hold all the operators.  The original coding of this routine did a
	 * separate palloc for each operator, but profiling revealed that the
	 * pallocs used an unreasonably large fraction of parsing time.
	 */
#define SPACE_PER_OP MAXALIGN(offsetof(struct _FuncCandidateList, args) + \
							  2 * sizeof(Oid))

	if (catlist->n_members > 0)
		resultSpace = palloc(catlist->n_members * SPACE_PER_OP);

	for (i = 0; i < catlist->n_members; i++)
	{
		HeapTuple	opertup = &catlist->members[i]->tuple;
		Form_pg_operator operform = (Form_pg_operator) GETSTRUCT(opertup);
		int			pathpos = 0;
		FuncCandidateList newResult;

		/* Ignore operators of wrong kind, if specific kind requested */
		if (oprkind && operform->oprkind != oprkind)
			continue;

		if (OidIsValid(namespaceId))
		{
			/* Consider only opers in specified namespace */
			if (operform->oprnamespace != namespaceId)
				continue;
			/* No need to check args, they must all be different */
		}
		else
		{
			/*
			 * Consider only opers that are in the search path and are not in
			 * the temp namespace.
			 */
			ListCell   *nsp;

			foreach(nsp, activeSearchPath)
			{
				if (operform->oprnamespace == lfirst_oid(nsp) &&
					operform->oprnamespace != myTempNamespace)
					break;
				pathpos++;
			}
			if (nsp == NULL)
				continue;		/* oper is not in search path */

			/*
			 * Okay, it's in the search path, but does it have the same
			 * arguments as something we already accepted?	If so, keep only
			 * the one that appears earlier in the search path.
			 *
			 * If we have an ordered list from SearchSysCacheList (the normal
			 * case), then any conflicting oper must immediately adjoin this
			 * one in the list, so we only need to look at the newest result
			 * item.  If we have an unordered list, we have to scan the whole
			 * result list.
			 */
			if (resultList)
			{
				FuncCandidateList prevResult;

				if (catlist->ordered)
				{
					if (operform->oprleft == resultList->args[0] &&
						operform->oprright == resultList->args[1])
						prevResult = resultList;
					else
						prevResult = NULL;
				}
				else
				{
					for (prevResult = resultList;
						 prevResult;
						 prevResult = prevResult->next)
					{
						if (operform->oprleft == prevResult->args[0] &&
							operform->oprright == prevResult->args[1])
							break;
					}
				}
				if (prevResult)
				{
					/* We have a match with a previous result */
					Assert(pathpos != prevResult->pathpos);
					if (pathpos > prevResult->pathpos)
						continue;	/* 保留先前的结果 */
					/* replace previous result */
					prevResult->pathpos = pathpos;
					prevResult->oid = operform->oid;
					continue;	/* args are same, of course */
				}
			}
		}

		/*
		 * 可以把它加入结果列表
		 */
		newResult = (FuncCandidateList) (resultSpace + nextResult);
		nextResult += SPACE_PER_OP;

		newResult->pathpos = pathpos;
		newResult->oid = operform->oid;
		newResult->nominalnargs = 2;
		newResult->nargs = 2;
		newResult->nvargs = 0;
		newResult->ndargs = 0;
		newResult->argnumbers = NULL;
		newResult->args[0] = operform->oprleft;
		newResult->args[1] = operform->oprright;
		newResult->next = resultList;
		resultList = newResult;
	}

	ReleaseSysCacheList(catlist);

	return resultList;
}

/*
 * OperatorIsVisible
 *		Determine whether an operator (identified by OID) is visible in the
 *		current search path.  Visible means "would be found by searching
 *		for the unqualified operator name with exact argument matches".
 */
bool
OperatorIsVisible(Oid oprid)
{
	return OperatorIsVisibleExt(oprid, NULL);
}

/*
 * OperatorIsVisibleExt
 *		As above, but if the operator isn't found and is_missing is not NULL,
 *		then set *is_missing = true and return false instead of throwing
 *		an error.  (Caller must initialize *is_missing = false.)
 */
static bool
OperatorIsVisibleExt(Oid oprid, bool *is_missing)
{
	HeapTuple	oprtup;
	Form_pg_operator oprform;
	Oid			oprnamespace;
	bool		visible;

	oprtup = SearchSysCache1(OPEROID, ObjectIdGetDatum(oprid));
	if (!HeapTupleIsValid(oprtup))
	{
		if (is_missing != NULL)
		{
			*is_missing = true;
			return false;
		}
		elog(ERROR, "cache lookup failed for operator %u", oprid);
	}
	oprform = (Form_pg_operator) GETSTRUCT(oprtup);

	recomputeNamespacePath();

/*
 * 快速检查：如果它根本不在搜索路径中，那它肯定不可见。系统命名空间中的
 * 项肯定在路径中，因此我们甚至不需要对它们调用 list_member_oid()。
 */
	oprnamespace = oprform->oprnamespace;
	if (oprnamespace != PG_CATALOG_NAMESPACE &&
		!list_member_oid(activeSearchPath, oprnamespace))
		visible = false;
	else
	{
		/*
		 * If it is in the path, it might still not be visible; it could be
		 * hidden by another operator of the same name and arguments earlier
		 * in the path.  So we must do a slow check to see if this is the same
		 * operator that would be found by OpernameGetOprid.
		 */
		char	   *oprname = NameStr(oprform->oprname);

		visible = (OpernameGetOprid(list_make1(makeString(oprname)),
									oprform->oprleft, oprform->oprright)
				   == oprid);
	}

	ReleaseSysCache(oprtup);

	return visible;
}


/*
 * OpclassnameGetOpcid
 *		Try to resolve an unqualified index opclass name.
 *		Returns OID if opclass found in search path, else InvalidOid.
 *
 * This is essentially the same as TypenameGetTypid, but we have to have
 * an extra argument for the index AM OID.
 */
Oid
OpclassnameGetOpcid(Oid amid, const char *opcname)
{
	Oid			opcid;
	ListCell   *l;

	recomputeNamespacePath();

	foreach(l, activeSearchPath)
	{
		Oid			namespaceId = lfirst_oid(l);

		if (namespaceId == myTempNamespace)
			continue;			/* 不要在临时命名空间中查找 */

		opcid = GetSysCacheOid3(CLAAMNAMENSP, Anum_pg_opclass_oid,
								ObjectIdGetDatum(amid),
								PointerGetDatum(opcname),
								ObjectIdGetDatum(namespaceId));
		if (OidIsValid(opcid))
			return opcid;
	}

	/* 在路径中未找到 */
	return InvalidOid;
}

/*
 * OpclassIsVisible
 *		Determine whether an opclass (identified by OID) is visible in the
 *		current search path.  Visible means "would be found by searching
 *		for the unqualified opclass name".
 */
bool
OpclassIsVisible(Oid opcid)
{
	return OpclassIsVisibleExt(opcid, NULL);
}

/*
 * OpclassIsVisibleExt
 *		As above, but if the opclass isn't found and is_missing is not NULL,
 *		then set *is_missing = true and return false instead of throwing
 *		an error.  (Caller must initialize *is_missing = false.)
 */
static bool
OpclassIsVisibleExt(Oid opcid, bool *is_missing)
{
	HeapTuple	opctup;
	Form_pg_opclass opcform;
	Oid			opcnamespace;
	bool		visible;

	opctup = SearchSysCache1(CLAOID, ObjectIdGetDatum(opcid));
	if (!HeapTupleIsValid(opctup))
	{
		if (is_missing != NULL)
		{
			*is_missing = true;
			return false;
		}
		elog(ERROR, "cache lookup failed for opclass %u", opcid);
	}
	opcform = (Form_pg_opclass) GETSTRUCT(opctup);

	recomputeNamespacePath();

/*
 * 快速检查：如果它根本不在搜索路径中，那它肯定不可见。系统命名空间中的
 * 项肯定在路径中，因此我们甚至不需要对它们调用 list_member_oid()。
 */
	opcnamespace = opcform->opcnamespace;
	if (opcnamespace != PG_CATALOG_NAMESPACE &&
		!list_member_oid(activeSearchPath, opcnamespace))
		visible = false;
	else
	{
		/*
		 * If it is in the path, it might still not be visible; it could be
		 * hidden by another opclass of the same name earlier in the path. So
		 * we must do a slow check to see if this opclass would be found by
		 * OpclassnameGetOpcid.
		 */
		char	   *opcname = NameStr(opcform->opcname);

		visible = (OpclassnameGetOpcid(opcform->opcmethod, opcname) == opcid);
	}

	ReleaseSysCache(opctup);

	return visible;
}

/*
 * OpfamilynameGetOpfid
 *		Try to resolve an unqualified index opfamily name.
 *		Returns OID if opfamily found in search path, else InvalidOid.
 *
 * This is essentially the same as TypenameGetTypid, but we have to have
 * an extra argument for the index AM OID.
 */
Oid
OpfamilynameGetOpfid(Oid amid, const char *opfname)
{
	Oid			opfid;
	ListCell   *l;

	recomputeNamespacePath();

	foreach(l, activeSearchPath)
	{
		Oid			namespaceId = lfirst_oid(l);

		if (namespaceId == myTempNamespace)
			continue;			/* 不要在临时命名空间中查找 */

		opfid = GetSysCacheOid3(OPFAMILYAMNAMENSP, Anum_pg_opfamily_oid,
								ObjectIdGetDatum(amid),
								PointerGetDatum(opfname),
								ObjectIdGetDatum(namespaceId));
		if (OidIsValid(opfid))
			return opfid;
	}

	/* 在路径中未找到 */
	return InvalidOid;
}

/*
 * OpfamilyIsVisible
 *		Determine whether an opfamily (identified by OID) is visible in the
 *		current search path.  Visible means "would be found by searching
 *		for the unqualified opfamily name".
 */
bool
OpfamilyIsVisible(Oid opfid)
{
	return OpfamilyIsVisibleExt(opfid, NULL);
}

/*
 * OpfamilyIsVisibleExt
 *		As above, but if the opfamily isn't found and is_missing is not NULL,
 *		then set *is_missing = true and return false instead of throwing
 *		an error.  (Caller must initialize *is_missing = false.)
 */
static bool
OpfamilyIsVisibleExt(Oid opfid, bool *is_missing)
{
	HeapTuple	opftup;
	Form_pg_opfamily opfform;
	Oid			opfnamespace;
	bool		visible;

	opftup = SearchSysCache1(OPFAMILYOID, ObjectIdGetDatum(opfid));
	if (!HeapTupleIsValid(opftup))
	{
		if (is_missing != NULL)
		{
			*is_missing = true;
			return false;
		}
		elog(ERROR, "cache lookup failed for opfamily %u", opfid);
	}
	opfform = (Form_pg_opfamily) GETSTRUCT(opftup);

	recomputeNamespacePath();

/*
 * 快速检查：如果它根本不在搜索路径中，那它肯定不可见。系统命名空间中的
 * 项肯定在路径中，因此我们甚至不需要对它们调用 list_member_oid()。
 */
	opfnamespace = opfform->opfnamespace;
	if (opfnamespace != PG_CATALOG_NAMESPACE &&
		!list_member_oid(activeSearchPath, opfnamespace))
		visible = false;
	else
	{
		/*
		 * If it is in the path, it might still not be visible; it could be
		 * hidden by another opfamily of the same name earlier in the path. So
		 * we must do a slow check to see if this opfamily would be found by
		 * OpfamilynameGetOpfid.
		 */
		char	   *opfname = NameStr(opfform->opfname);

		visible = (OpfamilynameGetOpfid(opfform->opfmethod, opfname) == opfid);
	}

	ReleaseSysCache(opftup);

	return visible;
}

/*
 * lookup_collation
 *		If there's a collation of the given name/namespace, and it works
 *		with the given encoding, return its OID.  Else return InvalidOid.
 */
static Oid
lookup_collation(const char *collname, Oid collnamespace, int32 encoding)
{
	Oid			collid;
	HeapTuple	colltup;
	Form_pg_collation collform;

	/* Check for encoding-specific entry (exact match) */
	collid = GetSysCacheOid3(COLLNAMEENCNSP, Anum_pg_collation_oid,
							 PointerGetDatum(collname),
							 Int32GetDatum(encoding),
							 ObjectIdGetDatum(collnamespace));
	if (OidIsValid(collid))
		return collid;

	/*
	 * Check for any-encoding entry.  This takes a bit more work: while libc
	 * collations with collencoding = -1 do work with all encodings, ICU
	 * collations only work with certain encodings, so we have to check that
	 * aspect before deciding it's a match.
	 */
	colltup = SearchSysCache3(COLLNAMEENCNSP,
							  PointerGetDatum(collname),
							  Int32GetDatum(-1),
							  ObjectIdGetDatum(collnamespace));
	if (!HeapTupleIsValid(colltup))
		return InvalidOid;
	collform = (Form_pg_collation) GETSTRUCT(colltup);
	if (collform->collprovider == COLLPROVIDER_ICU)
	{
		if (is_encoding_supported_by_icu(encoding))
			collid = collform->oid;
		else
			collid = InvalidOid;
	}
	else
	{
		collid = collform->oid;
	}
	ReleaseSysCache(colltup);
	return collid;
}

/*
 * CollationGetCollid
 *		Try to resolve an unqualified collation name.
 *		Returns OID if collation found in search path, else InvalidOid.
 *
 * Note that this will only find collations that work with the current
 * database's encoding.
 */
Oid
CollationGetCollid(const char *collname)
{
	int32		dbencoding = GetDatabaseEncoding();
	ListCell   *l;

	recomputeNamespacePath();

	foreach(l, activeSearchPath)
	{
		Oid			namespaceId = lfirst_oid(l);
		Oid			collid;

		if (namespaceId == myTempNamespace)
			continue;			/* 不要在临时命名空间中查找 */

		collid = lookup_collation(collname, namespaceId, dbencoding);
		if (OidIsValid(collid))
			return collid;
	}

	/* 在路径中未找到 */
	return InvalidOid;
}

/*
 * CollationIsVisible
 *		Determine whether a collation (identified by OID) is visible in the
 *		current search path.  Visible means "would be found by searching
 *		for the unqualified collation name".
 *
 * Note that only collations that work with the current database's encoding
 * will be considered visible.
 */
bool
CollationIsVisible(Oid collid)
{
	return CollationIsVisibleExt(collid, NULL);
}

/*
 * CollationIsVisibleExt
 *		As above, but if the collation isn't found and is_missing is not NULL,
 *		then set *is_missing = true and return false instead of throwing
 *		an error.  (Caller must initialize *is_missing = false.)
 */
static bool
CollationIsVisibleExt(Oid collid, bool *is_missing)
{
	HeapTuple	colltup;
	Form_pg_collation collform;
	Oid			collnamespace;
	bool		visible;

	colltup = SearchSysCache1(COLLOID, ObjectIdGetDatum(collid));
	if (!HeapTupleIsValid(colltup))
	{
		if (is_missing != NULL)
		{
			*is_missing = true;
			return false;
		}
		elog(ERROR, "cache lookup failed for collation %u", collid);
	}
	collform = (Form_pg_collation) GETSTRUCT(colltup);

	recomputeNamespacePath();

/*
 * 快速检查：如果它根本不在搜索路径中，那它肯定不可见。系统命名空间中的
 * 项肯定在路径中，因此我们甚至不需要对它们调用 list_member_oid()。
 */
	collnamespace = collform->collnamespace;
	if (collnamespace != PG_CATALOG_NAMESPACE &&
		!list_member_oid(activeSearchPath, collnamespace))
		visible = false;
	else
	{
		/*
		 * If it is in the path, it might still not be visible; it could be
		 * hidden by another collation of the same name earlier in the path,
		 * or it might not work with the current DB encoding.  So we must do a
		 * slow check to see if this collation would be found by
		 * CollationGetCollid.
		 */
		char	   *collname = NameStr(collform->collname);

		visible = (CollationGetCollid(collname) == collid);
	}

	ReleaseSysCache(colltup);

	return visible;
}


/*
 * ConversionGetConid
 *		Try to resolve an unqualified conversion name.
 *		Returns OID if conversion found in search path, else InvalidOid.
 *
 * This is essentially the same as RelnameGetRelid.
 */
Oid
ConversionGetConid(const char *conname)
{
	Oid			conid;
	ListCell   *l;

	recomputeNamespacePath();

	foreach(l, activeSearchPath)
	{
		Oid			namespaceId = lfirst_oid(l);

		if (namespaceId == myTempNamespace)
			continue;			/* 不要在临时命名空间中查找 */

		conid = GetSysCacheOid2(CONNAMENSP, Anum_pg_conversion_oid,
								PointerGetDatum(conname),
								ObjectIdGetDatum(namespaceId));
		if (OidIsValid(conid))
			return conid;
	}

	/* 在路径中未找到 */
	return InvalidOid;
}

/*
 * ConversionIsVisible
 *		Determine whether a conversion (identified by OID) is visible in the
 *		current search path.  Visible means "would be found by searching
 *		for the unqualified conversion name".
 */
bool
ConversionIsVisible(Oid conid)
{
	return ConversionIsVisibleExt(conid, NULL);
}

/*
 * ConversionIsVisibleExt
 *		As above, but if the conversion isn't found and is_missing is not NULL,
 *		then set *is_missing = true and return false instead of throwing
 *		an error.  (Caller must initialize *is_missing = false.)
 */
static bool
ConversionIsVisibleExt(Oid conid, bool *is_missing)
{
	HeapTuple	contup;
	Form_pg_conversion conform;
	Oid			connamespace;
	bool		visible;

	contup = SearchSysCache1(CONVOID, ObjectIdGetDatum(conid));
	if (!HeapTupleIsValid(contup))
	{
		if (is_missing != NULL)
		{
			*is_missing = true;
			return false;
		}
		elog(ERROR, "cache lookup failed for conversion %u", conid);
	}
	conform = (Form_pg_conversion) GETSTRUCT(contup);

	recomputeNamespacePath();

/*
 * 快速检查：如果它根本不在搜索路径中，那它肯定不可见。系统命名空间中的
 * 项肯定在路径中，因此我们甚至不需要对它们调用 list_member_oid()。
 */
	connamespace = conform->connamespace;
	if (connamespace != PG_CATALOG_NAMESPACE &&
		!list_member_oid(activeSearchPath, connamespace))
		visible = false;
	else
	{
		/*
		 * If it is in the path, it might still not be visible; it could be
		 * hidden by another conversion of the same name earlier in the path.
		 * So we must do a slow check to see if this conversion would be found
		 * by ConversionGetConid.
		 */
		char	   *conname = NameStr(conform->conname);

		visible = (ConversionGetConid(conname) == conid);
	}

	ReleaseSysCache(contup);

	return visible;
}

/*
 * get_statistics_object_oid - find a statistics object by possibly qualified name
 *
 * If not found, returns InvalidOid if missing_ok, else throws error
 */
Oid
get_statistics_object_oid(List *names, bool missing_ok)
{
	char	   *schemaname;
	char	   *stats_name;
	Oid			namespaceId;
	Oid			stats_oid = InvalidOid;
	ListCell   *l;

	/* 拆分名称列表 */
	DeconstructQualifiedName(names, &schemaname, &stats_name);

	if (schemaname)
	{
		/* 使用给定的确切模式 */
		namespaceId = LookupExplicitNamespace(schemaname, missing_ok);
		if (missing_ok && !OidIsValid(namespaceId))
			stats_oid = InvalidOid;
		else
			stats_oid = GetSysCacheOid2(STATEXTNAMENSP, Anum_pg_statistic_ext_oid,
										PointerGetDatum(stats_name),
										ObjectIdGetDatum(namespaceId));
	}
	else
	{
		/* search for it in search path */
		recomputeNamespacePath();

		foreach(l, activeSearchPath)
		{
			namespaceId = lfirst_oid(l);

			if (namespaceId == myTempNamespace)
				continue;		/* 不要在临时命名空间中查找 */
			stats_oid = GetSysCacheOid2(STATEXTNAMENSP, Anum_pg_statistic_ext_oid,
										PointerGetDatum(stats_name),
										ObjectIdGetDatum(namespaceId));
			if (OidIsValid(stats_oid))
				break;
		}
	}

	if (!OidIsValid(stats_oid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("statistics object \"%s\" does not exist",
						NameListToString(names))));

	return stats_oid;
}

/*
 * StatisticsObjIsVisible
 *		Determine whether a statistics object (identified by OID) is visible in
 *		the current search path.  Visible means "would be found by searching
 *		for the unqualified statistics object name".
 */
bool
StatisticsObjIsVisible(Oid stxid)
{
	return StatisticsObjIsVisibleExt(stxid, NULL);
}

/*
 * StatisticsObjIsVisibleExt
 *		As above, but if the statistics object isn't found and is_missing is
 *		not NULL, then set *is_missing = true and return false instead of
 *		throwing an error.  (Caller must initialize *is_missing = false.)
 */
static bool
StatisticsObjIsVisibleExt(Oid stxid, bool *is_missing)
{
	HeapTuple	stxtup;
	Form_pg_statistic_ext stxform;
	Oid			stxnamespace;
	bool		visible;

	stxtup = SearchSysCache1(STATEXTOID, ObjectIdGetDatum(stxid));
	if (!HeapTupleIsValid(stxtup))
	{
		if (is_missing != NULL)
		{
			*is_missing = true;
			return false;
		}
		elog(ERROR, "cache lookup failed for statistics object %u", stxid);
	}
	stxform = (Form_pg_statistic_ext) GETSTRUCT(stxtup);

	recomputeNamespacePath();

/*
 * 快速检查：如果它根本不在搜索路径中，那它肯定不可见。系统命名空间中的
 * 项肯定在路径中，因此我们甚至不需要对它们调用 list_member_oid()。
 */
	stxnamespace = stxform->stxnamespace;
	if (stxnamespace != PG_CATALOG_NAMESPACE &&
		!list_member_oid(activeSearchPath, stxnamespace))
		visible = false;
	else
	{
		/*
		 * If it is in the path, it might still not be visible; it could be
		 * hidden by another statistics object of the same name earlier in the
		 * path. So we must do a slow check for conflicting objects.
		 */
		char	   *stxname = NameStr(stxform->stxname);
		ListCell   *l;

		visible = false;
		foreach(l, activeSearchPath)
		{
			Oid			namespaceId = lfirst_oid(l);

			if (namespaceId == myTempNamespace)
				continue;		/* 不要在临时命名空间中查找 */

			if (namespaceId == stxnamespace)
			{
				/* Found it first in path */
				visible = true;
				break;
			}
			if (SearchSysCacheExists2(STATEXTNAMENSP,
									  PointerGetDatum(stxname),
									  ObjectIdGetDatum(namespaceId)))
			{
				/* Found something else first in path */
				break;
			}
		}
	}

	ReleaseSysCache(stxtup);

	return visible;
}

/*
 * get_ts_parser_oid - find a TS parser by possibly qualified name
 *
 * If not found, returns InvalidOid if missing_ok, else throws error
 */
Oid
get_ts_parser_oid(List *names, bool missing_ok)
{
	char	   *schemaname;
	char	   *parser_name;
	Oid			namespaceId;
	Oid			prsoid = InvalidOid;
	ListCell   *l;

	/* 拆分名称列表 */
	DeconstructQualifiedName(names, &schemaname, &parser_name);

	if (schemaname)
	{
		/* 使用给定的确切模式 */
		namespaceId = LookupExplicitNamespace(schemaname, missing_ok);
		if (missing_ok && !OidIsValid(namespaceId))
			prsoid = InvalidOid;
		else
			prsoid = GetSysCacheOid2(TSPARSERNAMENSP, Anum_pg_ts_parser_oid,
									 PointerGetDatum(parser_name),
									 ObjectIdGetDatum(namespaceId));
	}
	else
	{
		/* search for it in search path */
		recomputeNamespacePath();

		foreach(l, activeSearchPath)
		{
			namespaceId = lfirst_oid(l);

			if (namespaceId == myTempNamespace)
				continue;		/* 不要在临时命名空间中查找 */

			prsoid = GetSysCacheOid2(TSPARSERNAMENSP, Anum_pg_ts_parser_oid,
									 PointerGetDatum(parser_name),
									 ObjectIdGetDatum(namespaceId));
			if (OidIsValid(prsoid))
				break;
		}
	}

	if (!OidIsValid(prsoid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("text search parser \"%s\" does not exist",
						NameListToString(names))));

	return prsoid;
}

/*
 * TSParserIsVisible
 *		Determine whether a parser (identified by OID) is visible in the
 *		current search path.  Visible means "would be found by searching
 *		for the unqualified parser name".
 */
bool
TSParserIsVisible(Oid prsId)
{
	return TSParserIsVisibleExt(prsId, NULL);
}

/*
 * TSParserIsVisibleExt
 *		As above, but if the parser isn't found and is_missing is not NULL,
 *		then set *is_missing = true and return false instead of throwing
 *		an error.  (Caller must initialize *is_missing = false.)
 */
static bool
TSParserIsVisibleExt(Oid prsId, bool *is_missing)
{
	HeapTuple	tup;
	Form_pg_ts_parser form;
	Oid			namespace;
	bool		visible;

	tup = SearchSysCache1(TSPARSEROID, ObjectIdGetDatum(prsId));
	if (!HeapTupleIsValid(tup))
	{
		if (is_missing != NULL)
		{
			*is_missing = true;
			return false;
		}
		elog(ERROR, "cache lookup failed for text search parser %u", prsId);
	}
	form = (Form_pg_ts_parser) GETSTRUCT(tup);

	recomputeNamespacePath();

/*
 * 快速检查：如果它根本不在搜索路径中，那它肯定不可见。系统命名空间中的
 * 项肯定在路径中，因此我们甚至不需要对它们调用 list_member_oid()。
 */
	namespace = form->prsnamespace;
	if (namespace != PG_CATALOG_NAMESPACE &&
		!list_member_oid(activeSearchPath, namespace))
		visible = false;
	else
	{
		/*
		 * If it is in the path, it might still not be visible; it could be
		 * hidden by another parser of the same name earlier in the path. So
		 * we must do a slow check for conflicting parsers.
		 */
		char	   *name = NameStr(form->prsname);
		ListCell   *l;

		visible = false;
		foreach(l, activeSearchPath)
		{
			Oid			namespaceId = lfirst_oid(l);

			if (namespaceId == myTempNamespace)
				continue;		/* 不要在临时命名空间中查找 */

			if (namespaceId == namespace)
			{
				/* Found it first in path */
				visible = true;
				break;
			}
			if (SearchSysCacheExists2(TSPARSERNAMENSP,
									  PointerGetDatum(name),
									  ObjectIdGetDatum(namespaceId)))
			{
				/* Found something else first in path */
				break;
			}
		}
	}

	ReleaseSysCache(tup);

	return visible;
}

/*
 * get_ts_dict_oid - find a TS dictionary by possibly qualified name
 *
 * If not found, returns InvalidOid if missing_ok, else throws error
 */
Oid
get_ts_dict_oid(List *names, bool missing_ok)
{
	char	   *schemaname;
	char	   *dict_name;
	Oid			namespaceId;
	Oid			dictoid = InvalidOid;
	ListCell   *l;

	/* 拆分名称列表 */
	DeconstructQualifiedName(names, &schemaname, &dict_name);

	if (schemaname)
	{
		/* 使用给定的确切模式 */
		namespaceId = LookupExplicitNamespace(schemaname, missing_ok);
		if (missing_ok && !OidIsValid(namespaceId))
			dictoid = InvalidOid;
		else
			dictoid = GetSysCacheOid2(TSDICTNAMENSP, Anum_pg_ts_dict_oid,
									  PointerGetDatum(dict_name),
									  ObjectIdGetDatum(namespaceId));
	}
	else
	{
		/* search for it in search path */
		recomputeNamespacePath();

		foreach(l, activeSearchPath)
		{
			namespaceId = lfirst_oid(l);

			if (namespaceId == myTempNamespace)
				continue;		/* 不要在临时命名空间中查找 */

			dictoid = GetSysCacheOid2(TSDICTNAMENSP, Anum_pg_ts_dict_oid,
									  PointerGetDatum(dict_name),
									  ObjectIdGetDatum(namespaceId));
			if (OidIsValid(dictoid))
				break;
		}
	}

	if (!OidIsValid(dictoid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("text search dictionary \"%s\" does not exist",
						NameListToString(names))));

	return dictoid;
}

/*
 * TSDictionaryIsVisible
 *		Determine whether a dictionary (identified by OID) is visible in the
 *		current search path.  Visible means "would be found by searching
 *		for the unqualified dictionary name".
 */
bool
TSDictionaryIsVisible(Oid dictId)
{
	return TSDictionaryIsVisibleExt(dictId, NULL);
}

/*
 * TSDictionaryIsVisibleExt
 *		As above, but if the dictionary isn't found and is_missing is not NULL,
 *		then set *is_missing = true and return false instead of throwing
 *		an error.  (Caller must initialize *is_missing = false.)
 */
static bool
TSDictionaryIsVisibleExt(Oid dictId, bool *is_missing)
{
	HeapTuple	tup;
	Form_pg_ts_dict form;
	Oid			namespace;
	bool		visible;

	tup = SearchSysCache1(TSDICTOID, ObjectIdGetDatum(dictId));
	if (!HeapTupleIsValid(tup))
	{
		if (is_missing != NULL)
		{
			*is_missing = true;
			return false;
		}
		elog(ERROR, "cache lookup failed for text search dictionary %u",
			 dictId);
	}
	form = (Form_pg_ts_dict) GETSTRUCT(tup);

	recomputeNamespacePath();

/*
 * 快速检查：如果它根本不在搜索路径中，那它肯定不可见。系统命名空间中的
 * 项肯定在路径中，因此我们甚至不需要对它们调用 list_member_oid()。
 */
	namespace = form->dictnamespace;
	if (namespace != PG_CATALOG_NAMESPACE &&
		!list_member_oid(activeSearchPath, namespace))
		visible = false;
	else
	{
		/*
		 * If it is in the path, it might still not be visible; it could be
		 * hidden by another dictionary of the same name earlier in the path.
		 * So we must do a slow check for conflicting dictionaries.
		 */
		char	   *name = NameStr(form->dictname);
		ListCell   *l;

		visible = false;
		foreach(l, activeSearchPath)
		{
			Oid			namespaceId = lfirst_oid(l);

			if (namespaceId == myTempNamespace)
				continue;		/* 不要在临时命名空间中查找 */

			if (namespaceId == namespace)
			{
				/* Found it first in path */
				visible = true;
				break;
			}
			if (SearchSysCacheExists2(TSDICTNAMENSP,
									  PointerGetDatum(name),
									  ObjectIdGetDatum(namespaceId)))
			{
				/* Found something else first in path */
				break;
			}
		}
	}

	ReleaseSysCache(tup);

	return visible;
}

/*
 * get_ts_template_oid - find a TS template by possibly qualified name
 *
 * If not found, returns InvalidOid if missing_ok, else throws error
 */
Oid
get_ts_template_oid(List *names, bool missing_ok)
{
	char	   *schemaname;
	char	   *template_name;
	Oid			namespaceId;
	Oid			tmploid = InvalidOid;
	ListCell   *l;

	/* 拆分名称列表 */
	DeconstructQualifiedName(names, &schemaname, &template_name);

	if (schemaname)
	{
		/* 使用给定的确切模式 */
		namespaceId = LookupExplicitNamespace(schemaname, missing_ok);
		if (missing_ok && !OidIsValid(namespaceId))
			tmploid = InvalidOid;
		else
			tmploid = GetSysCacheOid2(TSTEMPLATENAMENSP, Anum_pg_ts_template_oid,
									  PointerGetDatum(template_name),
									  ObjectIdGetDatum(namespaceId));
	}
	else
	{
		/* search for it in search path */
		recomputeNamespacePath();

		foreach(l, activeSearchPath)
		{
			namespaceId = lfirst_oid(l);

			if (namespaceId == myTempNamespace)
				continue;		/* 不要在临时命名空间中查找 */

			tmploid = GetSysCacheOid2(TSTEMPLATENAMENSP, Anum_pg_ts_template_oid,
									  PointerGetDatum(template_name),
									  ObjectIdGetDatum(namespaceId));
			if (OidIsValid(tmploid))
				break;
		}
	}

	if (!OidIsValid(tmploid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("text search template \"%s\" does not exist",
						NameListToString(names))));

	return tmploid;
}

/*
 * TSTemplateIsVisible
 *		Determine whether a template (identified by OID) is visible in the
 *		current search path.  Visible means "would be found by searching
 *		for the unqualified template name".
 */
bool
TSTemplateIsVisible(Oid tmplId)
{
	return TSTemplateIsVisibleExt(tmplId, NULL);
}

/*
 * TSTemplateIsVisibleExt
 *		As above, but if the template isn't found and is_missing is not NULL,
 *		then set *is_missing = true and return false instead of throwing
 *		an error.  (Caller must initialize *is_missing = false.)
 */
static bool
TSTemplateIsVisibleExt(Oid tmplId, bool *is_missing)
{
	HeapTuple	tup;
	Form_pg_ts_template form;
	Oid			namespace;
	bool		visible;

	tup = SearchSysCache1(TSTEMPLATEOID, ObjectIdGetDatum(tmplId));
	if (!HeapTupleIsValid(tup))
	{
		if (is_missing != NULL)
		{
			*is_missing = true;
			return false;
		}
		elog(ERROR, "cache lookup failed for text search template %u", tmplId);
	}
	form = (Form_pg_ts_template) GETSTRUCT(tup);

	recomputeNamespacePath();

/*
 * 快速检查：如果它根本不在搜索路径中，那它肯定不可见。系统命名空间中的
 * 项肯定在路径中，因此我们甚至不需要对它们调用 list_member_oid()。
 */
	namespace = form->tmplnamespace;
	if (namespace != PG_CATALOG_NAMESPACE &&
		!list_member_oid(activeSearchPath, namespace))
		visible = false;
	else
	{
		/*
		 * If it is in the path, it might still not be visible; it could be
		 * hidden by another template of the same name earlier in the path. So
		 * we must do a slow check for conflicting templates.
		 */
		char	   *name = NameStr(form->tmplname);
		ListCell   *l;

		visible = false;
		foreach(l, activeSearchPath)
		{
			Oid			namespaceId = lfirst_oid(l);

			if (namespaceId == myTempNamespace)
				continue;		/* 不要在临时命名空间中查找 */

			if (namespaceId == namespace)
			{
				/* Found it first in path */
				visible = true;
				break;
			}
			if (SearchSysCacheExists2(TSTEMPLATENAMENSP,
									  PointerGetDatum(name),
									  ObjectIdGetDatum(namespaceId)))
			{
				/* Found something else first in path */
				break;
			}
		}
	}

	ReleaseSysCache(tup);

	return visible;
}

/*
 * get_ts_config_oid - find a TS config by possibly qualified name
 *
 * If not found, returns InvalidOid if missing_ok, else throws error
 */
Oid
get_ts_config_oid(List *names, bool missing_ok)
{
	char	   *schemaname;
	char	   *config_name;
	Oid			namespaceId;
	Oid			cfgoid = InvalidOid;
	ListCell   *l;

	/* 拆分名称列表 */
	DeconstructQualifiedName(names, &schemaname, &config_name);

	if (schemaname)
	{
		/* 使用给定的确切模式 */
		namespaceId = LookupExplicitNamespace(schemaname, missing_ok);
		if (missing_ok && !OidIsValid(namespaceId))
			cfgoid = InvalidOid;
		else
			cfgoid = GetSysCacheOid2(TSCONFIGNAMENSP, Anum_pg_ts_config_oid,
									 PointerGetDatum(config_name),
									 ObjectIdGetDatum(namespaceId));
	}
	else
	{
		/* search for it in search path */
		recomputeNamespacePath();

		foreach(l, activeSearchPath)
		{
			namespaceId = lfirst_oid(l);

			if (namespaceId == myTempNamespace)
				continue;		/* 不要在临时命名空间中查找 */

			cfgoid = GetSysCacheOid2(TSCONFIGNAMENSP, Anum_pg_ts_config_oid,
									 PointerGetDatum(config_name),
									 ObjectIdGetDatum(namespaceId));
			if (OidIsValid(cfgoid))
				break;
		}
	}

	if (!OidIsValid(cfgoid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("text search configuration \"%s\" does not exist",
						NameListToString(names))));

	return cfgoid;
}

/*
 * TSConfigIsVisible
 *		Determine whether a text search configuration (identified by OID)
 *		is visible in the current search path.  Visible means "would be found
 *		by searching for the unqualified text search configuration name".
 */
bool
TSConfigIsVisible(Oid cfgid)
{
	return TSConfigIsVisibleExt(cfgid, NULL);
}

/*
 * TSConfigIsVisibleExt
 *		As above, but if the configuration isn't found and is_missing is not
 *		NULL, then set *is_missing = true and return false instead of throwing
 *		an error.  (Caller must initialize *is_missing = false.)
 */
static bool
TSConfigIsVisibleExt(Oid cfgid, bool *is_missing)
{
	HeapTuple	tup;
	Form_pg_ts_config form;
	Oid			namespace;
	bool		visible;

	tup = SearchSysCache1(TSCONFIGOID, ObjectIdGetDatum(cfgid));
	if (!HeapTupleIsValid(tup))
	{
		if (is_missing != NULL)
		{
			*is_missing = true;
			return false;
		}
		elog(ERROR, "cache lookup failed for text search configuration %u",
			 cfgid);
	}
	form = (Form_pg_ts_config) GETSTRUCT(tup);

	recomputeNamespacePath();

/*
 * 快速检查：如果它根本不在搜索路径中，那它肯定不可见。系统命名空间中的
 * 项肯定在路径中，因此我们甚至不需要对它们调用 list_member_oid()。
 */
	namespace = form->cfgnamespace;
	if (namespace != PG_CATALOG_NAMESPACE &&
		!list_member_oid(activeSearchPath, namespace))
		visible = false;
	else
	{
		/*
		 * If it is in the path, it might still not be visible; it could be
		 * hidden by another configuration of the same name earlier in the
		 * path. So we must do a slow check for conflicting configurations.
		 */
		char	   *name = NameStr(form->cfgname);
		ListCell   *l;

		visible = false;
		foreach(l, activeSearchPath)
		{
			Oid			namespaceId = lfirst_oid(l);

			if (namespaceId == myTempNamespace)
				continue;		/* 不要在临时命名空间中查找 */

			if (namespaceId == namespace)
			{
				/* Found it first in path */
				visible = true;
				break;
			}
			if (SearchSysCacheExists2(TSCONFIGNAMENSP,
									  PointerGetDatum(name),
									  ObjectIdGetDatum(namespaceId)))
			{
				/* Found something else first in path */
				break;
			}
		}
	}

	ReleaseSysCache(tup);

	return visible;
}


/*
 * DeconstructQualifiedName
 *		Given a possibly-qualified name expressed as a list of String nodes,
 *		extract the schema name and object name.
 *
 * *nspname_p is set to NULL if there is no explicit schema name.
 */
void
DeconstructQualifiedName(const List *names,
						 char **nspname_p,
						 char **objname_p)
{
	char	   *catalogname;
	char	   *schemaname = NULL;
	char	   *objname = NULL;

	switch (list_length(names))
	{
		case 1:
			objname = strVal(linitial(names));
			break;
		case 2:
			schemaname = strVal(linitial(names));
			objname = strVal(lsecond(names));
			break;
		case 3:
			catalogname = strVal(linitial(names));
			schemaname = strVal(lsecond(names));
			objname = strVal(lthird(names));

			/*
			 * 我们先检查目录名，然后忽略它。
			 */
			if (strcmp(catalogname, get_database_name(MyDatabaseId)) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cross-database references are not implemented: %s",
								NameListToString(names))));
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("improper qualified name (too many dotted names): %s",
							NameListToString(names))));
			break;
	}

	*nspname_p = schemaname;
	*objname_p = objname;
}

/*
 * LookupNamespaceNoError
 *		Look up a schema name.
 *
 * Returns the namespace OID, or InvalidOid if not found.
 *
 * Note this does NOT perform any permissions check --- callers are
 * responsible for being sure that an appropriate check is made.
 * In the majority of cases LookupExplicitNamespace is preferable.
 */
Oid
LookupNamespaceNoError(const char *nspname)
{
	/* 检查 pg_temp 别名 */
	if (strcmp(nspname, "pg_temp") == 0)
	{
		if (OidIsValid(myTempNamespace))
		{
			InvokeNamespaceSearchHook(myTempNamespace, true);
			return myTempNamespace;
		}

		/*
		 * Since this is used only for looking up existing objects, there is
		 * no point in trying to initialize the temp namespace here; and doing
		 * so might create problems for some callers. Just report "not found".
		 */
		return InvalidOid;
	}

	return get_namespace_oid(nspname, true);
}

/*
 * LookupExplicitNamespace
 *		Process an explicitly-specified schema name: look up the schema
 *		and verify we have USAGE (lookup) rights in it.
 *
 * Returns the namespace OID
 */
Oid
LookupExplicitNamespace(const char *nspname, bool missing_ok)
{
	Oid			namespaceId;
	AclResult	aclresult;

	/* 检查 pg_temp 别名 */
	if (strcmp(nspname, "pg_temp") == 0)
	{
		if (OidIsValid(myTempNamespace))
			return myTempNamespace;

		/*
		 * Since this is used only for looking up existing objects, there is
		 * no point in trying to initialize the temp namespace here; and doing
		 * so might create problems for some callers --- just fall through.
		 */
	}

	namespaceId = get_namespace_oid(nspname, missing_ok);
	if (missing_ok && !OidIsValid(namespaceId))
		return InvalidOid;

	aclresult = object_aclcheck(NamespaceRelationId, namespaceId, GetUserId(), ACL_USAGE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA,
					   nspname);
	/* Schema search hook for this lookup */
	InvokeNamespaceSearchHook(namespaceId, true);

	return namespaceId;
}

/*
 * LookupCreationNamespace
 *		Look up the schema and verify we have CREATE rights on it.
 *
 * This is just like LookupExplicitNamespace except for the different
 * permission check, and that we are willing to create pg_temp if needed.
 *
 * Note: calling this may result in a CommandCounterIncrement operation,
 * if we have to create or clean out the temp namespace.
 */
Oid
LookupCreationNamespace(const char *nspname)
{
	Oid			namespaceId;
	AclResult	aclresult;

	/* 检查 pg_temp 别名 */
	if (strcmp(nspname, "pg_temp") == 0)
	{
		/* 初始化临时命名空间 */
		AccessTempTableNamespace(false);
		return myTempNamespace;
	}

	namespaceId = get_namespace_oid(nspname, false);

	aclresult = object_aclcheck(NamespaceRelationId, namespaceId, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA,
					   nspname);

	return namespaceId;
}

/*
 * Common checks on switching namespaces.
 *
 * We complain if either the old or new namespaces is a temporary schema
 * (or temporary toast schema), or if either the old or new namespaces is the
 * TOAST schema.
 */
void
CheckSetNamespace(Oid oldNspOid, Oid nspOid)
{
	/* disallow renaming into or out of temp schemas */
	if (isAnyTempNamespace(nspOid) || isAnyTempNamespace(oldNspOid))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot move objects into or out of temporary schemas")));

	/* same for TOAST schema */
	if (nspOid == PG_TOAST_NAMESPACE || oldNspOid == PG_TOAST_NAMESPACE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot move objects into or out of TOAST schema")));
}

/*
 * QualifiedNameGetCreationNamespace
 *		Given a possibly-qualified name for an object (in List-of-Strings
 *		format), determine what namespace the object should be created in.
 *		Also extract and return the object name (last component of list).
 *
 * Note: this does not apply any permissions check.  Callers must check
 * for CREATE rights on the selected namespace when appropriate.
 *
 * Note: calling this may result in a CommandCounterIncrement operation,
 * if we have to create or clean out the temp namespace.
 */
Oid
QualifiedNameGetCreationNamespace(const List *names, char **objname_p)
{
	char	   *schemaname;
	Oid			namespaceId;

	/* 拆分名称列表 */
	DeconstructQualifiedName(names, &schemaname, objname_p);

	if (schemaname)
	{
		/* 检查 pg_temp 别名 */
		if (strcmp(schemaname, "pg_temp") == 0)
		{
			/* 初始化临时命名空间 */
			AccessTempTableNamespace(false);
			return myTempNamespace;
		}
		/* 使用给定的确切模式 */
		namespaceId = get_namespace_oid(schemaname, false);
		/* 我们在这里不检查 USAGE 权限！ */
	}
	else
	{
		/* 使用默认的创建命名空间 */
		recomputeNamespacePath();
		if (activeTempCreationPending)
		{
			/* 需要初始化临时命名空间 */
			AccessTempTableNamespace(true);
			return myTempNamespace;
		}
		namespaceId = activeCreationNamespace;
		if (!OidIsValid(namespaceId))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_SCHEMA),
					 errmsg("no schema has been selected to create in")));
	}

	return namespaceId;
}

/*
 * get_namespace_oid - given a namespace name, look up the OID
 *
 * If missing_ok is false, throw an error if namespace name not found.  If
 * true, just return InvalidOid.
 */
Oid
get_namespace_oid(const char *nspname, bool missing_ok)
{
	Oid			oid;

	oid = GetSysCacheOid1(NAMESPACENAME, Anum_pg_namespace_oid,
						  CStringGetDatum(nspname));
	if (!OidIsValid(oid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_SCHEMA),
				 errmsg("schema \"%s\" does not exist", nspname)));

	return oid;
}

/*
 * makeRangeVarFromNameList
 *		Utility routine to convert a qualified-name list into RangeVar form.
 */
RangeVar *
makeRangeVarFromNameList(const List *names)
{
	RangeVar   *rel = makeRangeVar(NULL, NULL, -1);

	switch (list_length(names))
	{
		case 1:
			rel->relname = strVal(linitial(names));
			break;
		case 2:
			rel->schemaname = strVal(linitial(names));
			rel->relname = strVal(lsecond(names));
			break;
		case 3:
			rel->catalogname = strVal(linitial(names));
			rel->schemaname = strVal(lsecond(names));
			rel->relname = strVal(lthird(names));
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("improper relation name (too many dotted names): %s",
							NameListToString(names))));
			break;
	}

	return rel;
}

/*
 * NameListToString
 *		Utility routine to convert a qualified-name list into a string.
 *
 * This is used primarily to form error messages, and so we do not quote
 * the list elements, for the sake of legibility.
 *
 * In most scenarios the list elements should always be String values,
 * but we also allow A_Star for the convenience of ColumnRef processing.
 */
char *
NameListToString(const List *names)
{
	StringInfoData string;
	ListCell   *l;

	initStringInfo(&string);

	foreach(l, names)
	{
		Node	   *name = (Node *) lfirst(l);

		if (l != list_head(names))
			appendStringInfoChar(&string, '.');

		if (IsA(name, String))
			appendStringInfoString(&string, strVal(name));
		else if (IsA(name, A_Star))
			appendStringInfoChar(&string, '*');
		else
			elog(ERROR, "unexpected node type in name list: %d",
				 (int) nodeTag(name));
	}

	return string.data;
}

/*
 * NameListToQuotedString
 *		Utility routine to convert a qualified-name list into a string.
 *
 * Same as above except that names will be double-quoted where necessary,
 * so the string could be re-parsed (eg, by textToQualifiedNameList).
 */
char *
NameListToQuotedString(const List *names)
{
	StringInfoData string;
	ListCell   *l;

	initStringInfo(&string);

	foreach(l, names)
	{
		if (l != list_head(names))
			appendStringInfoChar(&string, '.');
		appendStringInfoString(&string, quote_identifier(strVal(lfirst(l))));
	}

	return string.data;
}

/*
 * isTempNamespace - is the given namespace my temporary-table namespace?
 */
bool
isTempNamespace(Oid namespaceId)
{
	if (OidIsValid(myTempNamespace) && myTempNamespace == namespaceId)
		return true;
	return false;
}

/*
 * isTempToastNamespace - is the given namespace my temporary-toast-table
 *		namespace?
 */
bool
isTempToastNamespace(Oid namespaceId)
{
	if (OidIsValid(myTempToastNamespace) && myTempToastNamespace == namespaceId)
		return true;
	return false;
}

/*
 * isTempOrTempToastNamespace - is the given namespace my temporary-table
 *		namespace or my temporary-toast-table namespace?
 */
bool
isTempOrTempToastNamespace(Oid namespaceId)
{
	if (OidIsValid(myTempNamespace) &&
		(myTempNamespace == namespaceId || myTempToastNamespace == namespaceId))
		return true;
	return false;
}

/*
 * isAnyTempNamespace - is the given namespace a temporary-table namespace
 * (either my own, or another backend's)?  Temporary-toast-table namespaces
 * are included, too.
 */
bool
isAnyTempNamespace(Oid namespaceId)
{
	bool		result;
	char	   *nspname;

	/* True if the namespace name starts with "pg_temp_" or "pg_toast_temp_" */
	nspname = get_namespace_name(namespaceId);
	if (!nspname)
		return false;			/* no such namespace? */
	result = (strncmp(nspname, "pg_temp_", 8) == 0) ||
		(strncmp(nspname, "pg_toast_temp_", 14) == 0);
	pfree(nspname);
	return result;
}

/*
 * isOtherTempNamespace - is the given namespace some other backend's
 * temporary-table namespace (including temporary-toast-table namespaces)?
 *
 * Note: for most purposes in the C code, this function is obsolete.  Use
 * RELATION_IS_OTHER_TEMP() instead to detect non-local temp relations.
 */
bool
isOtherTempNamespace(Oid namespaceId)
{
	/* If it's my own temp namespace, say "false" */
	if (isTempOrTempToastNamespace(namespaceId))
		return false;
	/* Else, if it's any temp namespace, say "true" */
	return isAnyTempNamespace(namespaceId);
}

/*
 * checkTempNamespaceStatus - is the given namespace owned and actively used
 * by a backend?
 *
 * Note: this can be used while scanning relations in pg_class to detect
 * orphaned temporary tables or namespaces with a backend connected to a
 * given database.  The result may be out of date quickly, so the caller
 * must be careful how to handle this information.
 */
TempNamespaceStatus
checkTempNamespaceStatus(Oid namespaceId)
{
	PGPROC	   *proc;
	ProcNumber	procNumber;

	Assert(OidIsValid(MyDatabaseId));

	procNumber = GetTempNamespaceProcNumber(namespaceId);

	/* No such namespace, or its name shows it's not temp? */
	if (procNumber == INVALID_PROC_NUMBER)
		return TEMP_NAMESPACE_NOT_TEMP;

	/* Is the backend alive? */
	proc = ProcNumberGetProc(procNumber);
	if (proc == NULL)
		return TEMP_NAMESPACE_IDLE;

	/* Is the backend connected to the same database we are looking at? */
	if (proc->databaseId != MyDatabaseId)
		return TEMP_NAMESPACE_IDLE;

	/* Does the backend own the temporary namespace? */
	if (proc->tempNamespaceId != namespaceId)
		return TEMP_NAMESPACE_IDLE;

	/* Yup, so namespace is busy */
	return TEMP_NAMESPACE_IN_USE;
}

/*
 * GetTempNamespaceProcNumber - if the given namespace is a temporary-table
 * namespace (either my own, or another backend's), return the proc number
 * that owns it.  Temporary-toast-table namespaces are included, too.
 * If it isn't a temp namespace, return INVALID_PROC_NUMBER.
 */
ProcNumber
GetTempNamespaceProcNumber(Oid namespaceId)
{
	int			result;
	char	   *nspname;

	/* See if the namespace name starts with "pg_temp_" or "pg_toast_temp_" */
	nspname = get_namespace_name(namespaceId);
	if (!nspname)
		return INVALID_PROC_NUMBER; /* no such namespace? */
	if (strncmp(nspname, "pg_temp_", 8) == 0)
		result = atoi(nspname + 8);
	else if (strncmp(nspname, "pg_toast_temp_", 14) == 0)
		result = atoi(nspname + 14);
	else
		result = INVALID_PROC_NUMBER;
	pfree(nspname);
	return result;
}

/*
 * GetTempToastNamespace - get the OID of my temporary-toast-table namespace,
 * which must already be assigned.  (This is only used when creating a toast
 * table for a temp table, so we must have already done InitTempTableNamespace)
 */
Oid
GetTempToastNamespace(void)
{
	Assert(OidIsValid(myTempToastNamespace));
	return myTempToastNamespace;
}


/*
 * GetTempNamespaceState - fetch status of session's temporary namespace
 *
 * This is used for conveying state to a parallel worker, and is not meant
 * for general-purpose access.
 */
void
GetTempNamespaceState(Oid *tempNamespaceId, Oid *tempToastNamespaceId)
{
	/* Return namespace OIDs, or 0 if session has not created temp namespace */
	*tempNamespaceId = myTempNamespace;
	*tempToastNamespaceId = myTempToastNamespace;
}

/*
 * SetTempNamespaceState - set status of session's temporary namespace
 *
 * This is used for conveying state to a parallel worker, and is not meant for
 * general-purpose access.  By transferring these namespace OIDs to workers,
 * we ensure they will have the same notion of the search path as their leader
 * does.
 */
void
SetTempNamespaceState(Oid tempNamespaceId, Oid tempToastNamespaceId)
{
	/* Worker should not have created its own namespaces ... */
	Assert(myTempNamespace == InvalidOid);
	Assert(myTempToastNamespace == InvalidOid);
	Assert(myTempNamespaceSubID == InvalidSubTransactionId);

	/* Assign same namespace OIDs that leader has */
	myTempNamespace = tempNamespaceId;
	myTempToastNamespace = tempToastNamespaceId;

	/*
	 * It's fine to leave myTempNamespaceSubID == InvalidSubTransactionId.
	 * Even if the namespace is new so far as the leader is concerned, it's
	 * not new to the worker, and we certainly wouldn't want the worker trying
	 * to destroy it.
	 */

	baseSearchPathValid = false;	/* may need to rebuild list */
	searchPathCacheValid = false;
}


/*
 * GetSearchPathMatcher - fetch current search path definition.
 *
 * The result structure is allocated in the specified memory context
 * (which might or might not be equal to CurrentMemoryContext); but any
 * junk created by revalidation calculations will be in CurrentMemoryContext.
 */
SearchPathMatcher *
GetSearchPathMatcher(MemoryContext context)
{
	SearchPathMatcher *result;
	List	   *schemas;
	MemoryContext oldcxt;

	recomputeNamespacePath();

	oldcxt = MemoryContextSwitchTo(context);

	result = (SearchPathMatcher *) palloc0(sizeof(SearchPathMatcher));
	schemas = list_copy(activeSearchPath);
	while (schemas && linitial_oid(schemas) != activeCreationNamespace)
	{
		if (linitial_oid(schemas) == myTempNamespace)
			result->addTemp = true;
		else
		{
			Assert(linitial_oid(schemas) == PG_CATALOG_NAMESPACE);
			result->addCatalog = true;
		}
		schemas = list_delete_first(schemas);
	}
	result->schemas = schemas;
	result->generation = activePathGeneration;

	MemoryContextSwitchTo(oldcxt);

	return result;
}

/*
 * CopySearchPathMatcher - copy the specified SearchPathMatcher.
 *
 * The result structure is allocated in CurrentMemoryContext.
 */
SearchPathMatcher *
CopySearchPathMatcher(SearchPathMatcher *path)
{
	SearchPathMatcher *result;

	result = (SearchPathMatcher *) palloc(sizeof(SearchPathMatcher));
	result->schemas = list_copy(path->schemas);
	result->addCatalog = path->addCatalog;
	result->addTemp = path->addTemp;
	result->generation = path->generation;

	return result;
}

/*
 * SearchPathMatchesCurrentEnvironment - does path match current environment?
 *
 * This is tested over and over in some common code paths, and in the typical
 * scenario where the active search path seldom changes, it'll always succeed.
 * We make that case fast by keeping a generation counter that is advanced
 * whenever the active search path changes.
 */
bool
SearchPathMatchesCurrentEnvironment(SearchPathMatcher *path)
{
	ListCell   *lc,
			   *lcp;

	recomputeNamespacePath();

	/* Quick out if already known equal to active path. */
	if (path->generation == activePathGeneration)
		return true;

	/* We scan down the activeSearchPath to see if it matches the input. */
	lc = list_head(activeSearchPath);

	/* If path->addTemp, first item should be my temp namespace. */
	if (path->addTemp)
	{
		if (lc && lfirst_oid(lc) == myTempNamespace)
			lc = lnext(activeSearchPath, lc);
		else
			return false;
	}
	/* If path->addCatalog, next item should be pg_catalog. */
	if (path->addCatalog)
	{
		if (lc && lfirst_oid(lc) == PG_CATALOG_NAMESPACE)
			lc = lnext(activeSearchPath, lc);
		else
			return false;
	}
	/* We should now be looking at the activeCreationNamespace. */
	if (activeCreationNamespace != (lc ? lfirst_oid(lc) : InvalidOid))
		return false;
	/* The remainder of activeSearchPath should match path->schemas. */
	foreach(lcp, path->schemas)
	{
		if (lc && lfirst_oid(lc) == lfirst_oid(lcp))
			lc = lnext(activeSearchPath, lc);
		else
			return false;
	}
	if (lc)
		return false;

	/*
	 * Update path->generation so that future tests will return quickly, so
	 * long as the active search path doesn't change.
	 */
	path->generation = activePathGeneration;

	return true;
}

/*
 * get_collation_oid - find a collation by possibly qualified name
 *
 * Note that this will only find collations that work with the current
 * database's encoding.
 */
Oid
get_collation_oid(List *collname, bool missing_ok)
{
	char	   *schemaname;
	char	   *collation_name;
	int32		dbencoding = GetDatabaseEncoding();
	Oid			namespaceId;
	Oid			colloid;
	ListCell   *l;

	/* 拆分名称列表 */
	DeconstructQualifiedName(collname, &schemaname, &collation_name);

	if (schemaname)
	{
		/* 使用给定的确切模式 */
		namespaceId = LookupExplicitNamespace(schemaname, missing_ok);
		if (missing_ok && !OidIsValid(namespaceId))
			return InvalidOid;

		colloid = lookup_collation(collation_name, namespaceId, dbencoding);
		if (OidIsValid(colloid))
			return colloid;
	}
	else
	{
		/* search for it in search path */
		recomputeNamespacePath();

		foreach(l, activeSearchPath)
		{
			namespaceId = lfirst_oid(l);

			if (namespaceId == myTempNamespace)
				continue;		/* 不要在临时命名空间中查找 */

			colloid = lookup_collation(collation_name, namespaceId, dbencoding);
			if (OidIsValid(colloid))
				return colloid;
		}
	}

	/* 在路径中未找到 */
	if (!missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("collation \"%s\" for encoding \"%s\" does not exist",
						NameListToString(collname), GetDatabaseEncodingName())));
	return InvalidOid;
}

/*
 * get_conversion_oid - find a conversion by possibly qualified name
 */
Oid
get_conversion_oid(List *conname, bool missing_ok)
{
	char	   *schemaname;
	char	   *conversion_name;
	Oid			namespaceId;
	Oid			conoid = InvalidOid;
	ListCell   *l;

	/* 拆分名称列表 */
	DeconstructQualifiedName(conname, &schemaname, &conversion_name);

	if (schemaname)
	{
		/* 使用给定的确切模式 */
		namespaceId = LookupExplicitNamespace(schemaname, missing_ok);
		if (missing_ok && !OidIsValid(namespaceId))
			conoid = InvalidOid;
		else
			conoid = GetSysCacheOid2(CONNAMENSP, Anum_pg_conversion_oid,
									 PointerGetDatum(conversion_name),
									 ObjectIdGetDatum(namespaceId));
	}
	else
	{
		/* search for it in search path */
		recomputeNamespacePath();

		foreach(l, activeSearchPath)
		{
			namespaceId = lfirst_oid(l);

			if (namespaceId == myTempNamespace)
				continue;		/* 不要在临时命名空间中查找 */

			conoid = GetSysCacheOid2(CONNAMENSP, Anum_pg_conversion_oid,
									 PointerGetDatum(conversion_name),
									 ObjectIdGetDatum(namespaceId));
			if (OidIsValid(conoid))
				return conoid;
		}
	}

	/* 在路径中未找到 */
	if (!OidIsValid(conoid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("conversion \"%s\" does not exist",
						NameListToString(conname))));
	return conoid;
}

/*
 * FindDefaultConversionProc - find default encoding conversion proc
 */
Oid
FindDefaultConversionProc(int32 for_encoding, int32 to_encoding)
{
	Oid			proc;
	ListCell   *l;

	recomputeNamespacePath();

	foreach(l, activeSearchPath)
	{
		Oid			namespaceId = lfirst_oid(l);

		if (namespaceId == myTempNamespace)
			continue;			/* 不要在临时命名空间中查找 */

		proc = FindDefaultConversion(namespaceId, for_encoding, to_encoding);
		if (OidIsValid(proc))
			return proc;
	}

	/* 在路径中未找到 */
	return InvalidOid;
}

/*
 * Look up namespace IDs and perform ACL checks. Return newly-allocated list.
 */
static List *
preprocessNamespacePath(const char *searchPath, Oid roleid,
						bool *temp_missing)
{
	char	   *rawname;
	List	   *namelist;
	List	   *oidlist;
	ListCell   *l;

	/* Need a modifiable copy */
	rawname = pstrdup(searchPath);

	/* Parse string into list of identifiers */
	if (!SplitIdentifierString(rawname, ',', &namelist))
	{
		/* syntax error in name list */
		/* this should not happen if GUC checked check_search_path */
		elog(ERROR, "invalid list syntax");
	}

	/*
	 * Convert the list of names to a list of OIDs.  If any names are not
	 * recognizable or we don't have read access, just leave them out of the
	 * list.  (We can't raise an error, since the search_path setting has
	 * already been accepted.)	Don't make duplicate entries, either.
	 */
	oidlist = NIL;
	*temp_missing = false;
	foreach(l, namelist)
	{
		char	   *curname = (char *) lfirst(l);
		Oid			namespaceId;

		if (strcmp(curname, "$user") == 0)
		{
			/* $user --- substitute namespace matching user name, if any */
			HeapTuple	tuple;

			tuple = SearchSysCache1(AUTHOID, ObjectIdGetDatum(roleid));
			if (HeapTupleIsValid(tuple))
			{
				char	   *rname;

				rname = NameStr(((Form_pg_authid) GETSTRUCT(tuple))->rolname);
				namespaceId = get_namespace_oid(rname, true);
				ReleaseSysCache(tuple);
				if (OidIsValid(namespaceId) &&
					object_aclcheck(NamespaceRelationId, namespaceId, roleid,
									ACL_USAGE) == ACLCHECK_OK)
					oidlist = lappend_oid(oidlist, namespaceId);
			}
		}
		else if (strcmp(curname, "pg_temp") == 0)
		{
			/* pg_temp --- substitute temp namespace, if any */
			if (OidIsValid(myTempNamespace))
				oidlist = lappend_oid(oidlist, myTempNamespace);
			else
			{
				/* If it ought to be the creation namespace, set flag */
				if (oidlist == NIL)
					*temp_missing = true;
			}
		}
		else
		{
			/* normal namespace reference */
			namespaceId = get_namespace_oid(curname, true);
			if (OidIsValid(namespaceId) &&
				object_aclcheck(NamespaceRelationId, namespaceId, roleid,
								ACL_USAGE) == ACLCHECK_OK)
				oidlist = lappend_oid(oidlist, namespaceId);
		}
	}

	pfree(rawname);
	list_free(namelist);

	return oidlist;
}

/*
 * Remove duplicates, run namespace search hooks, and prepend
 * implicitly-searched namespaces. Return newly-allocated list.
 *
 * If an object_access_hook is present, this must always be recalculated. It
 * may seem that duplicate elimination is not dependent on the result of the
 * hook, but if a hook returns different results on different calls for the
 * same namespace ID, then it could affect the order in which that namespace
 * appears in the final list.
 */
static List *
finalNamespacePath(List *oidlist, Oid *firstNS)
{
	List	   *finalPath = NIL;
	ListCell   *lc;

	foreach(lc, oidlist)
	{
		Oid			namespaceId = lfirst_oid(lc);

		if (!list_member_oid(finalPath, namespaceId))
		{
			if (InvokeNamespaceSearchHook(namespaceId, false))
				finalPath = lappend_oid(finalPath, namespaceId);
		}
	}

	/*
	 * Remember the first member of the explicit list.  (Note: this is
	 * nominally wrong if temp_missing, but we need it anyway to distinguish
	 * explicit from implicit mention of pg_catalog.)
	 */
	if (finalPath == NIL)
		*firstNS = InvalidOid;
	else
		*firstNS = linitial_oid(finalPath);

	/*
	 * Add any implicitly-searched namespaces to the list.  Note these go on
	 * the front, not the back; also notice that we do not check USAGE
	 * permissions for these.
	 */
	if (!list_member_oid(finalPath, PG_CATALOG_NAMESPACE))
		finalPath = lcons_oid(PG_CATALOG_NAMESPACE, finalPath);

	if (OidIsValid(myTempNamespace) &&
		!list_member_oid(finalPath, myTempNamespace))
		finalPath = lcons_oid(myTempNamespace, finalPath);

	return finalPath;
}

/*
 * Retrieve search path information from the cache; or if not there, fill
 * it. The returned entry is valid only until the next call to this function.
 */
static const SearchPathCacheEntry *
cachedNamespacePath(const char *searchPath, Oid roleid)
{
	MemoryContext oldcxt;
	SearchPathCacheEntry *entry;

	spcache_init();

	entry = spcache_insert(searchPath, roleid);

	/*
	 * An OOM may have resulted in a cache entry with missing 'oidlist' or
	 * 'finalPath', so just compute whatever is missing.
	 */

	if (entry->oidlist == NIL)
	{
		oldcxt = MemoryContextSwitchTo(SearchPathCacheContext);
		entry->oidlist = preprocessNamespacePath(searchPath, roleid,
												 &entry->temp_missing);
		MemoryContextSwitchTo(oldcxt);
	}

	/*
	 * If a hook is set, we must recompute finalPath from the oidlist each
	 * time, because the hook may affect the result. This is still much faster
	 * than recomputing from the string (and doing catalog lookups and ACL
	 * checks).
	 */
	if (entry->finalPath == NIL || object_access_hook ||
		entry->forceRecompute)
	{
		list_free(entry->finalPath);
		entry->finalPath = NIL;

		oldcxt = MemoryContextSwitchTo(SearchPathCacheContext);
		entry->finalPath = finalNamespacePath(entry->oidlist,
											  &entry->firstNS);
		MemoryContextSwitchTo(oldcxt);

		/*
		 * If an object_access_hook is set when finalPath is calculated, the
		 * result may be affected by the hook. Force recomputation of
		 * finalPath the next time this cache entry is used, even if the
		 * object_access_hook is not set at that time.
		 */
		entry->forceRecompute = object_access_hook ? true : false;
	}

	return entry;
}

/*
 * recomputeNamespacePath - recompute path derived variables if needed.
 */
static void
recomputeNamespacePath(void)
{
	Oid			roleid = GetUserId();
	bool		pathChanged;
	const SearchPathCacheEntry *entry;

	/* Do nothing if path is already valid. */
	if (baseSearchPathValid && namespaceUser == roleid)
		return;

	entry = cachedNamespacePath(namespace_search_path, roleid);

	if (baseCreationNamespace == entry->firstNS &&
		baseTempCreationPending == entry->temp_missing &&
		equal(entry->finalPath, baseSearchPath))
	{
		pathChanged = false;
	}
	else
	{
		MemoryContext oldcxt;
		List	   *newpath;

		pathChanged = true;

		/* Must save OID list in permanent storage. */
		oldcxt = MemoryContextSwitchTo(TopMemoryContext);
		newpath = list_copy(entry->finalPath);
		MemoryContextSwitchTo(oldcxt);

		/* Now safe to assign to state variables. */
		list_free(baseSearchPath);
		baseSearchPath = newpath;
		baseCreationNamespace = entry->firstNS;
		baseTempCreationPending = entry->temp_missing;
	}

	/* Mark the path valid. */
	baseSearchPathValid = true;
	namespaceUser = roleid;

	/* And make it active. */
	activeSearchPath = baseSearchPath;
	activeCreationNamespace = baseCreationNamespace;
	activeTempCreationPending = baseTempCreationPending;

	/*
	 * Bump the generation only if something actually changed.  (Notice that
	 * what we compared to was the old state of the base path variables.)
	 */
	if (pathChanged)
		activePathGeneration++;
}

/*
 * AccessTempTableNamespace
 *		Provide access to a temporary namespace, potentially creating it
 *		if not present yet.  This routine registers if the namespace gets
 *		in use in this transaction.  'force' can be set to true to allow
 *		the caller to enforce the creation of the temporary namespace for
 *		use in this backend, which happens if its creation is pending.
 */
static void
AccessTempTableNamespace(bool force)
{
	/*
	 * Make note that this temporary namespace has been accessed in this
	 * transaction.
	 */
	MyXactFlags |= XACT_FLAGS_ACCESSEDTEMPNAMESPACE;

	/*
	 * If the caller attempting to access a temporary schema expects the
	 * creation of the namespace to be pending and should be enforced, then go
	 * through the creation.
	 */
	if (!force && OidIsValid(myTempNamespace))
		return;

	/*
	 * The temporary tablespace does not exist yet and is wanted, so
	 * initialize it.
	 */
	InitTempTableNamespace();
}

/*
 * InitTempTableNamespace
 *		Initialize temp table namespace on first use in a particular backend
 */
static void
InitTempTableNamespace(void)
{
	char		namespaceName[NAMEDATALEN];
	Oid			namespaceId;
	Oid			toastspaceId;

	Assert(!OidIsValid(myTempNamespace));

	/*
	 * First, do permission check to see if we are authorized to make temp
	 * tables.  We use a nonstandard error message here since "databasename:
	 * permission denied" might be a tad cryptic.
	 *
	 * Note that ACL_CREATE_TEMP rights are rechecked in pg_namespace_aclmask;
	 * that's necessary since current user ID could change during the session.
	 * But there's no need to make the namespace in the first place until a
	 * temp table creation request is made by someone with appropriate rights.
	 */
	if (object_aclcheck(DatabaseRelationId, MyDatabaseId, GetUserId(),
						ACL_CREATE_TEMP) != ACLCHECK_OK)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to create temporary tables in database \"%s\"",
						get_database_name(MyDatabaseId))));

	/*
	 * Do not allow a Hot Standby session to make temp tables.  Aside from
	 * problems with modifying the system catalogs, there is a naming
	 * conflict: pg_temp_N belongs to the session with proc number N on the
	 * primary, not to a hot standby session with the same proc number.  We
	 * should not be able to get here anyway due to XactReadOnly checks, but
	 * let's just make real sure.  Note that this also backstops various
	 * operations that allow XactReadOnly transactions to modify temp tables;
	 * they'd need RecoveryInProgress checks if not for this.
	 */
	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
				 errmsg("cannot create temporary tables during recovery")));

	/* Parallel workers can't create temporary tables, either. */
	if (IsParallelWorker())
		ereport(ERROR,
				(errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION),
				 errmsg("cannot create temporary tables during a parallel operation")));

	snprintf(namespaceName, sizeof(namespaceName), "pg_temp_%d", MyProcNumber);

	namespaceId = get_namespace_oid(namespaceName, true);
	if (!OidIsValid(namespaceId))
	{
		/*
		 * First use of this temp namespace in this database; create it. The
		 * temp namespaces are always owned by the superuser.  We leave their
		 * permissions at default --- i.e., no access except to superuser ---
		 * to ensure that unprivileged users can't peek at other backends'
		 * temp tables.  This works because the places that access the temp
		 * namespace for my own backend skip permissions checks on it.
		 */
		namespaceId = NamespaceCreate(namespaceName, BOOTSTRAP_SUPERUSERID,
									  true);
		/* Advance command counter to make namespace visible */
		CommandCounterIncrement();
	}
	else
	{
		/*
		 * If the namespace already exists, clean it out (in case the former
		 * owner crashed without doing so).
		 */
		RemoveTempRelations(namespaceId);
	}

	/*
	 * If the corresponding toast-table namespace doesn't exist yet, create
	 * it. (We assume there is no need to clean it out if it does exist, since
	 * dropping a parent table should make its toast table go away.)
	 */
	snprintf(namespaceName, sizeof(namespaceName), "pg_toast_temp_%d",
			 MyProcNumber);

	toastspaceId = get_namespace_oid(namespaceName, true);
	if (!OidIsValid(toastspaceId))
	{
		toastspaceId = NamespaceCreate(namespaceName, BOOTSTRAP_SUPERUSERID,
									   true);
		/* Advance command counter to make namespace visible */
		CommandCounterIncrement();
	}

	/*
	 * Okay, we've prepared the temp namespace ... but it's not committed yet,
	 * so all our work could be undone by transaction rollback.  Set flag for
	 * AtEOXact_Namespace to know what to do.
	 */
	myTempNamespace = namespaceId;
	myTempToastNamespace = toastspaceId;

	/*
	 * Mark MyProc as owning this namespace which other processes can use to
	 * decide if a temporary namespace is in use or not.  We assume that
	 * assignment of namespaceId is an atomic operation.  Even if it is not,
	 * the temporary relation which resulted in the creation of this temporary
	 * namespace is still locked until the current transaction commits, and
	 * its pg_namespace row is not visible yet.  However it does not matter:
	 * this flag makes the namespace as being in use, so no objects created on
	 * it would be removed concurrently.
	 */
	MyProc->tempNamespaceId = namespaceId;

	/* It should not be done already. */
	Assert(myTempNamespaceSubID == InvalidSubTransactionId);
	myTempNamespaceSubID = GetCurrentSubTransactionId();

	baseSearchPathValid = false;	/* need to rebuild list */
	searchPathCacheValid = false;
}

/*
 * End-of-transaction cleanup for namespaces.
 */
void
AtEOXact_Namespace(bool isCommit, bool parallel)
{
	/*
	 * If we abort the transaction in which a temp namespace was selected,
	 * we'll have to do any creation or cleanout work over again.  So, just
	 * forget the namespace entirely until next time.  On the other hand, if
	 * we commit then register an exit callback to clean out the temp tables
	 * at backend shutdown.  (We only want to register the callback once per
	 * session, so this is a good place to do it.)
	 */
	if (myTempNamespaceSubID != InvalidSubTransactionId && !parallel)
	{
		if (isCommit)
			before_shmem_exit(RemoveTempRelationsCallback, 0);
		else
		{
			myTempNamespace = InvalidOid;
			myTempToastNamespace = InvalidOid;
			baseSearchPathValid = false;	/* need to rebuild list */
			searchPathCacheValid = false;

			/*
			 * Reset the temporary namespace flag in MyProc.  We assume that
			 * this operation is atomic.
			 *
			 * Because this transaction is aborting, the pg_namespace row is
			 * not visible to anyone else anyway, but that doesn't matter:
			 * it's not a problem if objects contained in this namespace are
			 * removed concurrently.
			 */
			MyProc->tempNamespaceId = InvalidOid;
		}
		myTempNamespaceSubID = InvalidSubTransactionId;
	}

}

/*
 * AtEOSubXact_Namespace
 *
 * At subtransaction commit, propagate the temp-namespace-creation
 * flag to the parent subtransaction.
 *
 * At subtransaction abort, forget the flag if we set it up.
 */
void
AtEOSubXact_Namespace(bool isCommit, SubTransactionId mySubid,
					  SubTransactionId parentSubid)
{

	if (myTempNamespaceSubID == mySubid)
	{
		if (isCommit)
			myTempNamespaceSubID = parentSubid;
		else
		{
			myTempNamespaceSubID = InvalidSubTransactionId;
			/* TEMP namespace creation failed, so reset state */
			myTempNamespace = InvalidOid;
			myTempToastNamespace = InvalidOid;
			baseSearchPathValid = false;	/* need to rebuild list */
			searchPathCacheValid = false;

			/*
			 * Reset the temporary namespace flag in MyProc.  We assume that
			 * this operation is atomic.
			 *
			 * Because this subtransaction is aborting, the pg_namespace row
			 * is not visible to anyone else anyway, but that doesn't matter:
			 * it's not a problem if objects contained in this namespace are
			 * removed concurrently.
			 */
			MyProc->tempNamespaceId = InvalidOid;
		}
	}
}

/*
 * Remove all relations in the specified temp namespace.
 *
 * This is called at backend shutdown (if we made any temp relations).
 * It is also called when we begin using a pre-existing temp namespace,
 * in order to clean out any relations that might have been created by
 * a crashed backend.
 */
static void
RemoveTempRelations(Oid tempNamespaceId)
{
	ObjectAddress object;

	/*
	 * We want to get rid of everything in the target namespace, but not the
	 * namespace itself (deleting it only to recreate it later would be a
	 * waste of cycles).  Hence, specify SKIP_ORIGINAL.  It's also an INTERNAL
	 * deletion, and we want to not drop any extensions that might happen to
	 * own temp objects.
	 */
	object.classId = NamespaceRelationId;
	object.objectId = tempNamespaceId;
	object.objectSubId = 0;

	performDeletion(&object, DROP_CASCADE,
					PERFORM_DELETION_INTERNAL |
					PERFORM_DELETION_QUIETLY |
					PERFORM_DELETION_SKIP_ORIGINAL |
					PERFORM_DELETION_SKIP_EXTENSIONS);
}

/*
 * Callback to remove temp relations at backend exit.
 */
static void
RemoveTempRelationsCallback(int code, Datum arg)
{
	if (OidIsValid(myTempNamespace))	/* should always be true */
	{
		/* Need to ensure we have a usable transaction. */
		AbortOutOfAnyTransaction();
		StartTransactionCommand();
		PushActiveSnapshot(GetTransactionSnapshot());

		RemoveTempRelations(myTempNamespace);

		PopActiveSnapshot();
		CommitTransactionCommand();
	}
}

/*
 * Remove all temp tables from the temporary namespace.
 */
void
ResetTempTableNamespace(void)
{
	if (OidIsValid(myTempNamespace))
		RemoveTempRelations(myTempNamespace);
}


/*
 * Routines for handling the GUC variable 'search_path'.
 */

/* check_hook: validate new search_path value */
bool
check_search_path(char **newval, void **extra, GucSource source)
{
	Oid			roleid = InvalidOid;
	const char *searchPath = *newval;
	char	   *rawname;
	List	   *namelist;
	bool		use_cache = (SearchPathCacheContext != NULL);

	/*
	 * We used to try to check that the named schemas exist, but there are
	 * many valid use-cases for having search_path settings that include
	 * schemas that don't exist; and often, we are not inside a transaction
	 * here and so can't consult the system catalogs anyway.  So now, the only
	 * requirement is syntactic validity of the identifier list.
	 *
	 * Checking only the syntactic validity also allows us to use the search
	 * path cache (if available) to avoid calling SplitIdentifierString() on
	 * the same string repeatedly.
	 */
	if (use_cache)
	{
		spcache_init();

		roleid = GetUserId();

		if (spcache_lookup(searchPath, roleid) != NULL)
			return true;
	}

	/*
	 * Ensure validity check succeeds before creating cache entry.
	 */

	rawname = pstrdup(searchPath);	/* need a modifiable copy */

	/* Parse string into list of identifiers */
	if (!SplitIdentifierString(rawname, ',', &namelist))
	{
		/* syntax error in name list */
		GUC_check_errdetail("List syntax is invalid.");
		pfree(rawname);
		list_free(namelist);
		return false;
	}
	pfree(rawname);
	list_free(namelist);

	/* OK to create empty cache entry */
	if (use_cache)
		(void) spcache_insert(searchPath, roleid);

	return true;
}

/* assign_hook: do extra actions as needed */
void
assign_search_path(const char *newval, void *extra)
{
	/* don't access search_path during bootstrap */
	Assert(!IsBootstrapProcessingMode());

	/*
	 * We mark the path as needing recomputation, but don't do anything until
	 * it's needed.  This avoids trying to do database access during GUC
	 * initialization, or outside a transaction.
	 *
	 * This does not invalidate the search path cache, so if this value had
	 * been previously set and no syscache invalidations happened,
	 * recomputation may not be necessary.
	 */
	baseSearchPathValid = false;
}

/*
 * InitializeSearchPath: initialize module during InitPostgres.
 *
 * This is called after we are up enough to be able to do catalog lookups.
 */
void
InitializeSearchPath(void)
{
	if (IsBootstrapProcessingMode())
	{
		/*
		 * In bootstrap mode, the search path must be 'pg_catalog' so that
		 * tables are created in the proper namespace; ignore the GUC setting.
		 */
		MemoryContext oldcxt;

		oldcxt = MemoryContextSwitchTo(TopMemoryContext);
		baseSearchPath = list_make1_oid(PG_CATALOG_NAMESPACE);
		MemoryContextSwitchTo(oldcxt);
		baseCreationNamespace = PG_CATALOG_NAMESPACE;
		baseTempCreationPending = false;
		baseSearchPathValid = true;
		namespaceUser = GetUserId();
		activeSearchPath = baseSearchPath;
		activeCreationNamespace = baseCreationNamespace;
		activeTempCreationPending = baseTempCreationPending;
		activePathGeneration++; /* pro forma */
	}
	else
	{
		/*
		 * In normal mode, arrange for a callback on any syscache invalidation
		 * that will affect the search_path cache.
		 */

		/* namespace name or ACLs may have changed */
		CacheRegisterSyscacheCallback(NAMESPACEOID,
									  InvalidationCallback,
									  (Datum) 0);

		/* role name may affect the meaning of "$user" */
		CacheRegisterSyscacheCallback(AUTHOID,
									  InvalidationCallback,
									  (Datum) 0);

		/* role membership may affect ACLs */
		CacheRegisterSyscacheCallback(AUTHMEMROLEMEM,
									  InvalidationCallback,
									  (Datum) 0);

		/* database owner may affect ACLs */
		CacheRegisterSyscacheCallback(DATABASEOID,
									  InvalidationCallback,
									  (Datum) 0);

		/* Force search path to be recomputed on next use */
		baseSearchPathValid = false;
		searchPathCacheValid = false;
	}
}

/*
 * InvalidationCallback
 *		Syscache inval callback function
 */
static void
InvalidationCallback(Datum arg, int cacheid, uint32 hashvalue)
{
	/*
	 * Force search path to be recomputed on next use, also invalidating the
	 * search path cache (because namespace names, ACLs, or role names may
	 * have changed).
	 */
	baseSearchPathValid = false;
	searchPathCacheValid = false;
}

/*
 * Fetch the active search path. The return value is a palloc'ed list
 * of OIDs; the caller is responsible for freeing this storage as
 * appropriate.
 *
 * The returned list includes the implicitly-prepended namespaces only if
 * includeImplicit is true.
 *
 * Note: calling this may result in a CommandCounterIncrement operation,
 * if we have to create or clean out the temp namespace.
 */
List *
fetch_search_path(bool includeImplicit)
{
	List	   *result;

	recomputeNamespacePath();

	/*
	 * If the temp namespace should be first, force it to exist.  This is so
	 * that callers can trust the result to reflect the actual default
	 * creation namespace.  It's a bit bogus to do this here, since
	 * current_schema() is supposedly a stable function without side-effects,
	 * but the alternatives seem worse.
	 */
	if (activeTempCreationPending)
	{
		AccessTempTableNamespace(true);
		recomputeNamespacePath();
	}

	result = list_copy(activeSearchPath);
	if (!includeImplicit)
	{
		while (result && linitial_oid(result) != activeCreationNamespace)
			result = list_delete_first(result);
	}

	return result;
}

/*
 * Fetch the active search path into a caller-allocated array of OIDs.
 * Returns the number of path entries.  (If this is more than sarray_len,
 * then the data didn't fit and is not all stored.)
 *
 * The returned list always includes the implicitly-prepended namespaces,
 * but never includes the temp namespace.  (This is suitable for existing
 * users, which would want to ignore the temp namespace anyway.)  This
 * definition allows us to not worry about initializing the temp namespace.
 */
int
fetch_search_path_array(Oid *sarray, int sarray_len)
{
	int			count = 0;
	ListCell   *l;

	recomputeNamespacePath();

	foreach(l, activeSearchPath)
	{
		Oid			namespaceId = lfirst_oid(l);

		if (namespaceId == myTempNamespace)
			continue;			/* do not include temp namespace */

		if (count < sarray_len)
			sarray[count] = namespaceId;
		count++;
	}

	return count;
}


/*
 * Export the FooIsVisible functions as SQL-callable functions.
 *
 * Note: as of Postgres 8.4, these will silently return NULL if called on
 * a nonexistent object OID, rather than failing.  This is to avoid race
 * condition errors when a query that's scanning a catalog using an MVCC
 * snapshot uses one of these functions.  The underlying IsVisible functions
 * always use an up-to-date snapshot and so might see the object as already
 * gone when it's still visible to the transaction snapshot.
 */

Datum
pg_table_is_visible(PG_FUNCTION_ARGS)
{
	Oid			oid = PG_GETARG_OID(0);
	bool		result;
	bool		is_missing = false;

	result = RelationIsVisibleExt(oid, &is_missing);

	if (is_missing)
		PG_RETURN_NULL();
	PG_RETURN_BOOL(result);
}

Datum
pg_type_is_visible(PG_FUNCTION_ARGS)
{
	Oid			oid = PG_GETARG_OID(0);
	bool		result;
	bool		is_missing = false;

	result = TypeIsVisibleExt(oid, &is_missing);

	if (is_missing)
		PG_RETURN_NULL();
	PG_RETURN_BOOL(result);
}

Datum
pg_function_is_visible(PG_FUNCTION_ARGS)
{
	Oid			oid = PG_GETARG_OID(0);
	bool		result;
	bool		is_missing = false;

	result = FunctionIsVisibleExt(oid, &is_missing);

	if (is_missing)
		PG_RETURN_NULL();
	PG_RETURN_BOOL(result);
}

Datum
pg_operator_is_visible(PG_FUNCTION_ARGS)
{
	Oid			oid = PG_GETARG_OID(0);
	bool		result;
	bool		is_missing = false;

	result = OperatorIsVisibleExt(oid, &is_missing);

	if (is_missing)
		PG_RETURN_NULL();
	PG_RETURN_BOOL(result);
}

Datum
pg_opclass_is_visible(PG_FUNCTION_ARGS)
{
	Oid			oid = PG_GETARG_OID(0);
	bool		result;
	bool		is_missing = false;

	result = OpclassIsVisibleExt(oid, &is_missing);

	if (is_missing)
		PG_RETURN_NULL();
	PG_RETURN_BOOL(result);
}

Datum
pg_opfamily_is_visible(PG_FUNCTION_ARGS)
{
	Oid			oid = PG_GETARG_OID(0);
	bool		result;
	bool		is_missing = false;

	result = OpfamilyIsVisibleExt(oid, &is_missing);

	if (is_missing)
		PG_RETURN_NULL();
	PG_RETURN_BOOL(result);
}

Datum
pg_collation_is_visible(PG_FUNCTION_ARGS)
{
	Oid			oid = PG_GETARG_OID(0);
	bool		result;
	bool		is_missing = false;

	result = CollationIsVisibleExt(oid, &is_missing);

	if (is_missing)
		PG_RETURN_NULL();
	PG_RETURN_BOOL(result);
}

Datum
pg_conversion_is_visible(PG_FUNCTION_ARGS)
{
	Oid			oid = PG_GETARG_OID(0);
	bool		result;
	bool		is_missing = false;

	result = ConversionIsVisibleExt(oid, &is_missing);

	if (is_missing)
		PG_RETURN_NULL();
	PG_RETURN_BOOL(result);
}

Datum
pg_statistics_obj_is_visible(PG_FUNCTION_ARGS)
{
	Oid			oid = PG_GETARG_OID(0);
	bool		result;
	bool		is_missing = false;

	result = StatisticsObjIsVisibleExt(oid, &is_missing);

	if (is_missing)
		PG_RETURN_NULL();
	PG_RETURN_BOOL(result);
}

Datum
pg_ts_parser_is_visible(PG_FUNCTION_ARGS)
{
	Oid			oid = PG_GETARG_OID(0);
	bool		result;
	bool		is_missing = false;

	result = TSParserIsVisibleExt(oid, &is_missing);

	if (is_missing)
		PG_RETURN_NULL();
	PG_RETURN_BOOL(result);
}

Datum
pg_ts_dict_is_visible(PG_FUNCTION_ARGS)
{
	Oid			oid = PG_GETARG_OID(0);
	bool		result;
	bool		is_missing = false;

	result = TSDictionaryIsVisibleExt(oid, &is_missing);

	if (is_missing)
		PG_RETURN_NULL();
	PG_RETURN_BOOL(result);
}

Datum
pg_ts_template_is_visible(PG_FUNCTION_ARGS)
{
	Oid			oid = PG_GETARG_OID(0);
	bool		result;
	bool		is_missing = false;

	result = TSTemplateIsVisibleExt(oid, &is_missing);

	if (is_missing)
		PG_RETURN_NULL();
	PG_RETURN_BOOL(result);
}

Datum
pg_ts_config_is_visible(PG_FUNCTION_ARGS)
{
	Oid			oid = PG_GETARG_OID(0);
	bool		result;
	bool		is_missing = false;

	result = TSConfigIsVisibleExt(oid, &is_missing);

	if (is_missing)
		PG_RETURN_NULL();
	PG_RETURN_BOOL(result);
}

Datum
pg_my_temp_schema(PG_FUNCTION_ARGS)
{
	PG_RETURN_OID(myTempNamespace);
}

Datum
pg_is_other_temp_schema(PG_FUNCTION_ARGS)
{
	Oid			oid = PG_GETARG_OID(0);

	PG_RETURN_BOOL(isOtherTempNamespace(oid));
}
