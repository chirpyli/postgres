/*-------------------------------------------------------------------------
 *
 * reloptions.c
 *	  关系选项（pg_class.reloptions）的核心支持
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/common/reloptions.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <float.h>

#include "access/gist_private.h"
#include "access/hash.h"
#include "access/heaptoast.h"
#include "access/htup_details.h"
#include "access/nbtree.h"
#include "access/reloptions.h"
#include "access/spgist_private.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/tablespace.h"
#include "nodes/makefuncs.h"
#include "utils/array.h"
#include "utils/attoptcache.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/rel.h"

/*
 * pg_class.reloptions 的内容
 *
 * 要新增一个选项：
 *
 * (i) 确定其类型（bool、integer、real、enum、string）、名称、默认值、
 *     上下界（如适用）；对于字符串，考虑一个校验例程。
 * (ii) 在下方添加一条记录（或使用 add_<type>_reloption）。
 * (iii) 将其加入相应的选项结构体（也许是 StdRdOptions）。
 * (iv) 将其加入相应的处理例程（也许是 default_reloptions）。
 * (v) 确保为该操作设置了正确的锁级别。
 * (vi) 别忘了为这个选项编写文档。
 *
 * 任何新选项的默认选择都应该是 AccessExclusiveLock。
 * 在某些情况下，锁级别可以从此降低，但所选的锁级别必须始终与自身冲突，
 * 以确保在我们尝试并发修改时，多个修改不会丢失。
 * 锁级别的选择完全取决于该参数在服务器内部的使用方式，而不是取决于
 * 你希望如何以及何时修改它。安全第一。已有的选择记录在本文档以及
 * 使用该参数的后端代码中的其他地方。
 *
 * 一般而言，任何会影响 SELECT 所得结果的内容都必须用
 * AccessExclusiveLock 加以保护。
 *
 * 与 Autovacuum 相关的参数可以在 ShareUpdateExclusiveLock 下设置，
 * 因为它们仅由 AV 进程使用，且不会改变当前正在执行的任何东西。
 *
 * fillfactor 可以在 ShareUpdateExclusiveLock 下设置，因为它只应用于
 * 后续对数据块的修改，正如 hio.c 中所记载的那样。
 *
 * n_distinct 选项可以在 ShareUpdateExclusiveLock 下设置，因为它们
 * 仅在 ANALYZE 期间使用，而 ANALYZE 使用 ShareUpdateExclusiveLock，
 * 因此 ANALYZE 不会受到进行中（in-flight）修改的影响。修改这些值
 * 直到下一次 ANALYZE 才会生效，因此不需要更强的锁。
 *
 * 与规划器（planner）相关的参数可以在 ShareUpdateExclusiveLock 下设置，
 * 因为它们只影响规划而不影响执行的正确性。计划无法在运行过程中改变，
 * 因此这里的修改无论如何都不容易带来新的改进计划。所以我们允许现有
 * 查询继续执行、现有计划继续存活，这是为了让更好的计划能够在不干扰
 * 用户的情况下并发引入而付出的小小代价。
 *
 * 在 ShareUpdateExclusiveLock 下设置 parallel_workers 是安全的，因为
 * 它的作用与 max_parallel_workers_per_gather 相同，后者是一个 USERSET
 * 参数，不会影响现有的计划或查询。
 *
 * vacuum_truncate 可以在 ShareUpdateExclusiveLock 下设置，因为它
 * 仅在 VACUUM 期间使用，而 VACUUM 使用 ShareUpdateExclusiveLock，
 * 因此 VACUUM 不会受到进行中修改的影响。修改其值直到下一次 VACUUM
 * 才会生效，因此不需要更强的锁。
 */

static relopt_bool boolRelOpts[] =
{
	{
		{
			"autosummarize",
			"Enables automatic summarization on this BRIN index",
			RELOPT_KIND_BRIN,
			AccessExclusiveLock
		},
		false
	},
	{
		{
			"autovacuum_enabled",
			"Enables autovacuum in this relation",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		},
		true
	},
	{
		{
			"user_catalog_table",
			"Declare a table as an additional catalog table, e.g. for the purpose of logical replication",
			RELOPT_KIND_HEAP,
			AccessExclusiveLock
		},
		false
	},
	{
		{
			"fastupdate",
			"Enables \"fast update\" feature for this GIN index",
			RELOPT_KIND_GIN,
			AccessExclusiveLock
		},
		true
	},
	{
		{
			"security_barrier",
			"View acts as a row security barrier",
			RELOPT_KIND_VIEW,
			AccessExclusiveLock
		},
		false
	},
	{
		{
			"security_invoker",
			"Privileges on underlying relations are checked as the invoking user, not the view owner",
			RELOPT_KIND_VIEW,
			AccessExclusiveLock
		},
		false
	},
	{
		{
			"vacuum_truncate",
			"Enables vacuum to truncate empty pages at the end of this table",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		},
		true
	},
	{
		{
			"deduplicate_items",
			"Enables \"deduplicate items\" feature for this btree index",
			RELOPT_KIND_BTREE,
			ShareUpdateExclusiveLock	/* 因为它只适用于后续的插入 */
		},
		true
	},
	/* 列表终止符 */
	{{NULL}}
};

static relopt_int intRelOpts[] =
{
	{
		{
			"fillfactor",
			"Packs table pages only to this percentage",
			RELOPT_KIND_HEAP,
			ShareUpdateExclusiveLock	/* 因为它只适用于后续的插入 */
		},
		HEAP_DEFAULT_FILLFACTOR, HEAP_MIN_FILLFACTOR, 100
	},
	{
		{
			"fillfactor",
			"Packs btree index pages only to this percentage",
			RELOPT_KIND_BTREE,
			ShareUpdateExclusiveLock	/* 因为它只适用于后续的插入 */
		},
		BTREE_DEFAULT_FILLFACTOR, BTREE_MIN_FILLFACTOR, 100
	},
	{
		{
			"fillfactor",
			"Packs hash index pages only to this percentage",
			RELOPT_KIND_HASH,
			ShareUpdateExclusiveLock	/* 因为它只适用于后续的插入 */
		},
		HASH_DEFAULT_FILLFACTOR, HASH_MIN_FILLFACTOR, 100
	},
	{
		{
			"fillfactor",
			"Packs gist index pages only to this percentage",
			RELOPT_KIND_GIST,
			ShareUpdateExclusiveLock	/* 因为它只适用于后续的插入 */
		},
		GIST_DEFAULT_FILLFACTOR, GIST_MIN_FILLFACTOR, 100
	},
	{
		{
			"fillfactor",
			"Packs spgist index pages only to this percentage",
			RELOPT_KIND_SPGIST,
			ShareUpdateExclusiveLock	/* 因为它只适用于后续的插入 */
		},
		SPGIST_DEFAULT_FILLFACTOR, SPGIST_MIN_FILLFACTOR, 100
	},
	{
		{
			"autovacuum_vacuum_threshold",
			"Minimum number of tuple updates or deletes prior to vacuum",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		},
		-1, 0, INT_MAX
	},
	{
		{
			"autovacuum_vacuum_max_threshold",
			"Maximum number of tuple updates or deletes prior to vacuum",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		},
		-2, -1, INT_MAX
	},
	{
		{
			"autovacuum_vacuum_insert_threshold",
			"Minimum number of tuple inserts prior to vacuum, or -1 to disable insert vacuums",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		},
		-2, -1, INT_MAX
	},
	{
		{
			"autovacuum_analyze_threshold",
			"Minimum number of tuple inserts, updates or deletes prior to analyze",
			RELOPT_KIND_HEAP,
			ShareUpdateExclusiveLock
		},
		-1, 0, INT_MAX
	},
	{
		{
			"autovacuum_vacuum_cost_limit",
			"Vacuum cost amount available before napping, for autovacuum",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		},
		-1, 1, 10000
	},
	{
		{
			"autovacuum_freeze_min_age",
			"Minimum age at which VACUUM should freeze a table row, for autovacuum",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		},
		-1, 0, 1000000000
	},
	{
		{
			"autovacuum_multixact_freeze_min_age",
			"Minimum multixact age at which VACUUM should freeze a row multixact's, for autovacuum",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		},
		-1, 0, 1000000000
	},
	{
		{
			"autovacuum_freeze_max_age",
			"Age at which to autovacuum a table to prevent transaction ID wraparound",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		},
		-1, 100000, 2000000000
	},
	{
		{
			"autovacuum_multixact_freeze_max_age",
			"Multixact age at which to autovacuum a table to prevent multixact wraparound",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		},
		-1, 10000, 2000000000
	},
	{
		{
			"autovacuum_freeze_table_age",
			"Age at which VACUUM should perform a full table sweep to freeze row versions",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		}, -1, 0, 2000000000
	},
	{
		{
			"autovacuum_multixact_freeze_table_age",
			"Age of multixact at which VACUUM should perform a full table sweep to freeze row versions",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		}, -1, 0, 2000000000
	},
	{
		{
			"log_autovacuum_min_duration",
			"Sets the minimum execution time above which autovacuum actions will be logged",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		},
		-1, -1, INT_MAX
	},
	{
		{
			"toast_tuple_target",
			"Sets the target tuple length at which external columns will be toasted",
			RELOPT_KIND_HEAP,
			ShareUpdateExclusiveLock
		},
		TOAST_TUPLE_TARGET, 128, TOAST_TUPLE_TARGET_MAIN
	},
	{
		{
			"pages_per_range",
			"Number of pages that each page range covers in a BRIN index",
			RELOPT_KIND_BRIN,
			AccessExclusiveLock
		}, 128, 1, 131072
	},
	{
		{
			"gin_pending_list_limit",
			"Maximum size of the pending list for this GIN index, in kilobytes.",
			RELOPT_KIND_GIN,
			AccessExclusiveLock
		},
		-1, 64, MAX_KILOBYTES
	},
	{
		{
			"effective_io_concurrency",
			"Number of simultaneous requests that can be handled efficiently by the disk subsystem.",
			RELOPT_KIND_TABLESPACE,
			ShareUpdateExclusiveLock
		},
		-1, 0, MAX_IO_CONCURRENCY
	},
	{
		{
			"maintenance_io_concurrency",
			"Number of simultaneous requests that can be handled efficiently by the disk subsystem for maintenance work.",
			RELOPT_KIND_TABLESPACE,
			ShareUpdateExclusiveLock
		},
		-1, 0, MAX_IO_CONCURRENCY
	},
	{
		{
			"parallel_workers",
			"Number of parallel processes that can be used per executor node for this relation.",
			RELOPT_KIND_HEAP,
			ShareUpdateExclusiveLock
		},
		-1, 0, 1024
	},

	/* 列表终止符 */
	{{NULL}}
};

static relopt_real realRelOpts[] =
{
	{
		{
			"autovacuum_vacuum_cost_delay",
			"Vacuum cost delay in milliseconds, for autovacuum",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		},
		-1, 0.0, 100.0
	},
	{
		{
			"autovacuum_vacuum_scale_factor",
			"Number of tuple updates or deletes prior to vacuum as a fraction of reltuples",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		},
		-1, 0.0, 100.0
	},
	{
		{
			"autovacuum_vacuum_insert_scale_factor",
			"Number of tuple inserts prior to vacuum as a fraction of reltuples",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		},
		-1, 0.0, 100.0
	},
	{
		{
			"autovacuum_analyze_scale_factor",
			"Number of tuple inserts, updates or deletes prior to analyze as a fraction of reltuples",
			RELOPT_KIND_HEAP,
			ShareUpdateExclusiveLock
		},
		-1, 0.0, 100.0
	},
	{
		{
			"vacuum_max_eager_freeze_failure_rate",
			"Fraction of pages in a relation vacuum can scan and fail to freeze before disabling eager scanning.",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		},
		-1, 0.0, 1.0
	},

	{
		{
			"seq_page_cost",
			"Sets the planner's estimate of the cost of a sequentially fetched disk page.",
			RELOPT_KIND_TABLESPACE,
			ShareUpdateExclusiveLock
		},
		-1, 0.0, DBL_MAX
	},
	{
		{
			"random_page_cost",
			"Sets the planner's estimate of the cost of a nonsequentially fetched disk page.",
			RELOPT_KIND_TABLESPACE,
			ShareUpdateExclusiveLock
		},
		-1, 0.0, DBL_MAX
	},
	{
		{
			"n_distinct",
			"Sets the planner's estimate of the number of distinct values appearing in a column (excluding child relations).",
			RELOPT_KIND_ATTRIBUTE,
			ShareUpdateExclusiveLock
		},
		0, -1.0, DBL_MAX
	},
	{
		{
			"n_distinct_inherited",
			"Sets the planner's estimate of the number of distinct values appearing in a column (including child relations).",
			RELOPT_KIND_ATTRIBUTE,
			ShareUpdateExclusiveLock
		},
		0, -1.0, DBL_MAX
	},
	{
		{
			"vacuum_cleanup_index_scale_factor",
			"Deprecated B-Tree parameter.",
			RELOPT_KIND_BTREE,
			ShareUpdateExclusiveLock
		},
		-1, 0.0, 1e10
	},
	/* 列表终止符 */
	{{NULL}}
};

/* 取自 StdRdOptIndexCleanup 的值 */
static relopt_enum_elt_def StdRdOptIndexCleanupValues[] =
{
	{"auto", STDRD_OPTION_VACUUM_INDEX_CLEANUP_AUTO},
	{"on", STDRD_OPTION_VACUUM_INDEX_CLEANUP_ON},
	{"off", STDRD_OPTION_VACUUM_INDEX_CLEANUP_OFF},
	{"true", STDRD_OPTION_VACUUM_INDEX_CLEANUP_ON},
	{"false", STDRD_OPTION_VACUUM_INDEX_CLEANUP_OFF},
	{"yes", STDRD_OPTION_VACUUM_INDEX_CLEANUP_ON},
	{"no", STDRD_OPTION_VACUUM_INDEX_CLEANUP_OFF},
	{"1", STDRD_OPTION_VACUUM_INDEX_CLEANUP_ON},
	{"0", STDRD_OPTION_VACUUM_INDEX_CLEANUP_OFF},
	{(const char *) NULL}		/* 列表终止符 */
};

/* 取自 GistOptBufferingMode 的值 */
static relopt_enum_elt_def gistBufferingOptValues[] =
{
	{"auto", GIST_OPTION_BUFFERING_AUTO},
	{"on", GIST_OPTION_BUFFERING_ON},
	{"off", GIST_OPTION_BUFFERING_OFF},
	{(const char *) NULL}		/* 列表终止符 */
};

/* 取自 ViewOptCheckOption 的值 */
static relopt_enum_elt_def viewCheckOptValues[] =
{
	/* NOT_SET 没有对应的值 */
	{"local", VIEW_OPTION_CHECK_OPTION_LOCAL},
	{"cascaded", VIEW_OPTION_CHECK_OPTION_CASCADED},
	{(const char *) NULL}		/* 列表终止符 */
};

static relopt_enum enumRelOpts[] =
{
	{
		{
			"vacuum_index_cleanup",
			"Controls index vacuuming and index cleanup",
			RELOPT_KIND_HEAP | RELOPT_KIND_TOAST,
			ShareUpdateExclusiveLock
		},
		StdRdOptIndexCleanupValues,
		STDRD_OPTION_VACUUM_INDEX_CLEANUP_AUTO,
		gettext_noop("Valid values are \"on\", \"off\", and \"auto\".")
	},
	{
		{
			"buffering",
			"Enables buffering build for this GiST index",
			RELOPT_KIND_GIST,
			AccessExclusiveLock
		},
		gistBufferingOptValues,
		GIST_OPTION_BUFFERING_AUTO,
		gettext_noop("Valid values are \"on\", \"off\", and \"auto\".")
	},
	{
		{
			"check_option",
			"View has WITH CHECK OPTION defined (local or cascaded).",
			RELOPT_KIND_VIEW,
			AccessExclusiveLock
		},
		viewCheckOptValues,
		VIEW_OPTION_CHECK_OPTION_NOT_SET,
		gettext_noop("Valid values are \"local\" and \"cascaded\".")
	},
	/* 列表终止符 */
	{{NULL}}
};

static relopt_string stringRelOpts[] =
{
	/* 列表终止符 */
	{{NULL}}
};

static relopt_gen **relOpts = NULL;
static bits32 last_assigned_kind = RELOPT_KIND_LAST_DEFAULT;

static int	num_custom_options = 0;
static relopt_gen **custom_options = NULL;
static bool need_initialization = true;

static void initialize_reloptions(void);
static void parse_one_reloption(relopt_value *option, char *text_str,
								int text_len, bool validate);

/*
 * 获取一个字符串类型 reloption 的长度（无论是默认值还是用户定义的值）。
 * 这用于在构建一组关系选项时进行内存分配。
 */
#define GET_STRING_RELOPTION_LEN(option) \
	((option).isset ? strlen((option).values.string_val) : \
	 ((relopt_string *) (option).gen)->default_len)

/*
 * initialize_reloptions
 *		初始化例程，必须在解析之前调用
 *
 * 初始化 relOpts 数组，并填充每个变量的类型与名称长度。
 */
static void
initialize_reloptions(void)
{
	int			i;
	int			j;

	j = 0;
	for (i = 0; boolRelOpts[i].gen.name; i++)
	{
		Assert(DoLockModesConflict(boolRelOpts[i].gen.lockmode,
								   boolRelOpts[i].gen.lockmode));
		j++;
	}
	for (i = 0; intRelOpts[i].gen.name; i++)
	{
		Assert(DoLockModesConflict(intRelOpts[i].gen.lockmode,
								   intRelOpts[i].gen.lockmode));
		j++;
	}
	for (i = 0; realRelOpts[i].gen.name; i++)
	{
		Assert(DoLockModesConflict(realRelOpts[i].gen.lockmode,
								   realRelOpts[i].gen.lockmode));
		j++;
	}
	for (i = 0; enumRelOpts[i].gen.name; i++)
	{
		Assert(DoLockModesConflict(enumRelOpts[i].gen.lockmode,
								   enumRelOpts[i].gen.lockmode));
		j++;
	}
	for (i = 0; stringRelOpts[i].gen.name; i++)
	{
		Assert(DoLockModesConflict(stringRelOpts[i].gen.lockmode,
								   stringRelOpts[i].gen.lockmode));
		j++;
	}
	j += num_custom_options;

	if (relOpts)
		pfree(relOpts);
	relOpts = MemoryContextAlloc(TopMemoryContext,
								 (j + 1) * sizeof(relopt_gen *));

	j = 0;
	for (i = 0; boolRelOpts[i].gen.name; i++)
	{
		relOpts[j] = &boolRelOpts[i].gen;
		relOpts[j]->type = RELOPT_TYPE_BOOL;
		relOpts[j]->namelen = strlen(relOpts[j]->name);
		j++;
	}

	for (i = 0; intRelOpts[i].gen.name; i++)
	{
		relOpts[j] = &intRelOpts[i].gen;
		relOpts[j]->type = RELOPT_TYPE_INT;
		relOpts[j]->namelen = strlen(relOpts[j]->name);
		j++;
	}

	for (i = 0; realRelOpts[i].gen.name; i++)
	{
		relOpts[j] = &realRelOpts[i].gen;
		relOpts[j]->type = RELOPT_TYPE_REAL;
		relOpts[j]->namelen = strlen(relOpts[j]->name);
		j++;
	}

	for (i = 0; enumRelOpts[i].gen.name; i++)
	{
		relOpts[j] = &enumRelOpts[i].gen;
		relOpts[j]->type = RELOPT_TYPE_ENUM;
		relOpts[j]->namelen = strlen(relOpts[j]->name);
		j++;
	}

	for (i = 0; stringRelOpts[i].gen.name; i++)
	{
		relOpts[j] = &stringRelOpts[i].gen;
		relOpts[j]->type = RELOPT_TYPE_STRING;
		relOpts[j]->namelen = strlen(relOpts[j]->name);
		j++;
	}

	for (i = 0; i < num_custom_options; i++)
	{
		relOpts[j] = custom_options[i];
		j++;
	}

	/* 添加一个列表终止符 */
	relOpts[j] = NULL;

	/* 标记工作已完成 */
	need_initialization = false;
}

/*
 * add_reloption_kind
 *		创建一个新的 relopt_kind 值，供用户自定义的 AM 在自定义
 *		reloption 中使用。
 */
relopt_kind
add_reloption_kind(void)
{
	/* 不要把最后一位分配出去，以保证枚举的行为在跨平台时保持不变 */
	if (last_assigned_kind >= RELOPT_KIND_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("user-defined relation parameter types limit exceeded")));
	last_assigned_kind <<= 1;
	return (relopt_kind) last_assigned_kind;
}

/*
 * add_reloption
 *		将一个已创建好的自定义 reloption 加入列表，并重新计算主解析表。
 */
static void
add_reloption(relopt_gen *newoption)
{
	static int	max_custom_options = 0;

	if (num_custom_options >= max_custom_options)
	{
		MemoryContext oldcxt;

		oldcxt = MemoryContextSwitchTo(TopMemoryContext);

		if (max_custom_options == 0)
		{
			max_custom_options = 8;
			custom_options = palloc(max_custom_options * sizeof(relopt_gen *));
		}
		else
		{
			max_custom_options *= 2;
			custom_options = repalloc(custom_options,
									  max_custom_options * sizeof(relopt_gen *));
		}
		MemoryContextSwitchTo(oldcxt);
	}
	custom_options[num_custom_options++] = newoption;

	need_initialization = true;
}

/*
 * init_local_reloptions
 *		初始化本地 reloption，它们将被解析进大小为
 * 		'relopt_struct_size' 的 bytea 结构体中。
 */
void
init_local_reloptions(local_relopts *relopts, Size relopt_struct_size)
{
	relopts->options = NIL;
	relopts->validators = NIL;
	relopts->relopt_struct_size = relopt_struct_size;
}

/*
 * register_reloptions_validator
 *		注册一个自定义的校验回调函数，该回调将在
 *		build_local_reloptions() 结束时被调用。
 */
void
register_reloptions_validator(local_relopts *relopts, relopts_validator validator)
{
	relopts->validators = lappend(relopts->validators, validator);
}

/*
 * add_local_reloption
 *		将一个已创建好的自定义 reloption 加入本地列表。
 */
static void
add_local_reloption(local_relopts *relopts, relopt_gen *newoption, int offset)
{
	local_relopt *opt = palloc(sizeof(*opt));

	Assert(offset < relopts->relopt_struct_size);

	opt->option = newoption;
	opt->offset = offset;

	relopts->options = lappend(relopts->options, opt);
}

/*
 * allocate_reloption
 *		分配一个新的 reloption，并初始化与类型无关的字段
 *		（针对除 string 之外的类型）
 */
static relopt_gen *
allocate_reloption(bits32 kinds, int type, const char *name, const char *desc,
				   LOCKMODE lockmode)
{
	MemoryContext oldcxt;
	size_t		size;
	relopt_gen *newoption;

	if (kinds != RELOPT_KIND_LOCAL)
		oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	else
		oldcxt = NULL;

	switch (type)
	{
		case RELOPT_TYPE_BOOL:
			size = sizeof(relopt_bool);
			break;
		case RELOPT_TYPE_INT:
			size = sizeof(relopt_int);
			break;
		case RELOPT_TYPE_REAL:
			size = sizeof(relopt_real);
			break;
		case RELOPT_TYPE_ENUM:
			size = sizeof(relopt_enum);
			break;
		case RELOPT_TYPE_STRING:
			size = sizeof(relopt_string);
			break;
		default:
			elog(ERROR, "unsupported reloption type %d", type);
			return NULL;		/* 让编译器安静（避免告警） */
	}

	newoption = palloc(size);

	newoption->name = pstrdup(name);
	if (desc)
		newoption->desc = pstrdup(desc);
	else
		newoption->desc = NULL;
	newoption->kinds = kinds;
	newoption->namelen = strlen(name);
	newoption->type = type;
	newoption->lockmode = lockmode;

	if (oldcxt != NULL)
		MemoryContextSwitchTo(oldcxt);

	return newoption;
}

/*
 * init_bool_reloption
 *		分配并初始化一个新的布尔（boolean）类型 reloption
 */
static relopt_bool *
init_bool_reloption(bits32 kinds, const char *name, const char *desc,
					bool default_val, LOCKMODE lockmode)
{
	relopt_bool *newoption;

	newoption = (relopt_bool *) allocate_reloption(kinds, RELOPT_TYPE_BOOL,
												   name, desc, lockmode);
	newoption->default_val = default_val;

	return newoption;
}

/*
 * add_bool_reloption
 *		添加一个新的布尔（boolean）类型 reloption
 */
void
add_bool_reloption(bits32 kinds, const char *name, const char *desc,
				   bool default_val, LOCKMODE lockmode)
{
	relopt_bool *newoption = init_bool_reloption(kinds, name, desc,
												 default_val, lockmode);

	add_reloption((relopt_gen *) newoption);
}

/*
 * add_local_bool_reloption
 *		添加一个新的本地布尔（boolean）类型 reloption
 *
 * 'offset' 是 bool 类型字段的偏移量。
 */
void
add_local_bool_reloption(local_relopts *relopts, const char *name,
						 const char *desc, bool default_val, int offset)
{
	relopt_bool *newoption = init_bool_reloption(RELOPT_KIND_LOCAL,
												 name, desc,
												 default_val, 0);

	add_local_reloption(relopts, (relopt_gen *) newoption, offset);
}


/*
 * init_int_reloption
 *		分配并初始化一个新的整数（integer）类型 reloption
 */
static relopt_int *
init_int_reloption(bits32 kinds, const char *name, const char *desc,
				   int default_val, int min_val, int max_val,
				   LOCKMODE lockmode)
{
	relopt_int *newoption;

	newoption = (relopt_int *) allocate_reloption(kinds, RELOPT_TYPE_INT,
												  name, desc, lockmode);
	newoption->default_val = default_val;
	newoption->min = min_val;
	newoption->max = max_val;

	return newoption;
}

/*
 * add_int_reloption
 *		添加一个新的整数（integer）类型 reloption
 */
void
add_int_reloption(bits32 kinds, const char *name, const char *desc, int default_val,
				  int min_val, int max_val, LOCKMODE lockmode)
{
	relopt_int *newoption = init_int_reloption(kinds, name, desc,
											   default_val, min_val,
											   max_val, lockmode);

	add_reloption((relopt_gen *) newoption);
}

/*
 * add_local_int_reloption
 *		Add a new local integer reloption
 *
 * 'offset' 是 int 类型字段的偏移量。
 */
void
add_local_int_reloption(local_relopts *relopts, const char *name,
						const char *desc, int default_val, int min_val,
						int max_val, int offset)
{
	relopt_int *newoption = init_int_reloption(RELOPT_KIND_LOCAL,
											   name, desc, default_val,
											   min_val, max_val, 0);

	add_local_reloption(relopts, (relopt_gen *) newoption, offset);
}

/*
 * init_real_reloption
 *		分配并初始化一个新的实数（real）类型 reloption
 */
static relopt_real *
init_real_reloption(bits32 kinds, const char *name, const char *desc,
					double default_val, double min_val, double max_val,
					LOCKMODE lockmode)
{
	relopt_real *newoption;

	newoption = (relopt_real *) allocate_reloption(kinds, RELOPT_TYPE_REAL,
												   name, desc, lockmode);
	newoption->default_val = default_val;
	newoption->min = min_val;
	newoption->max = max_val;

	return newoption;
}

/*
 * add_real_reloption
 *		添加一个新的实数（float）类型 reloption
 */
void
add_real_reloption(bits32 kinds, const char *name, const char *desc,
				   double default_val, double min_val, double max_val,
				   LOCKMODE lockmode)
{
	relopt_real *newoption = init_real_reloption(kinds, name, desc,
												 default_val, min_val,
												 max_val, lockmode);

	add_reloption((relopt_gen *) newoption);
}

/*
 * add_local_real_reloption
 *		添加一个新的本地实数（float）类型 reloption
 *
 * 'offset' 是 double 类型字段的偏移量。
 */
void
add_local_real_reloption(local_relopts *relopts, const char *name,
						 const char *desc, double default_val,
						 double min_val, double max_val, int offset)
{
	relopt_real *newoption = init_real_reloption(RELOPT_KIND_LOCAL,
												 name, desc,
												 default_val, min_val,
												 max_val, 0);

	add_local_reloption(relopts, (relopt_gen *) newoption, offset);
}

/*
 * init_enum_reloption
 *		分配并初始化一个新的枚举（enum）类型 reloption
 */
static relopt_enum *
init_enum_reloption(bits32 kinds, const char *name, const char *desc,
					relopt_enum_elt_def *members, int default_val,
					const char *detailmsg, LOCKMODE lockmode)
{
	relopt_enum *newoption;

	newoption = (relopt_enum *) allocate_reloption(kinds, RELOPT_TYPE_ENUM,
												   name, desc, lockmode);
	newoption->members = members;
	newoption->default_val = default_val;
	newoption->detailmsg = detailmsg;

	return newoption;
}


/*
 * add_enum_reloption
 *		添加一个新的枚举（enum）类型 reloption
 *
 * members 数组必须带有一个以 NULL 结尾的终止项。
 *
 * 当传入不受支持的值时，将显示 detailmsg，其形式如下：
 *   "Valid values are \"foo\", \"bar\", and \"bar\"."
 *
 * members 数组与 detailmsg 不会被复制 —— 调用方必须保证它们在
 * 进程的整个生命周期内都有效。
 */
void
add_enum_reloption(bits32 kinds, const char *name, const char *desc,
				   relopt_enum_elt_def *members, int default_val,
				   const char *detailmsg, LOCKMODE lockmode)
{
	relopt_enum *newoption = init_enum_reloption(kinds, name, desc,
												 members, default_val,
												 detailmsg, lockmode);

	add_reloption((relopt_gen *) newoption);
}

/*
 * add_local_enum_reloption
 *		添加一个新的本地枚举（enum）类型 reloption
 *
 * 'offset' 是 int 类型字段的偏移量。
 */
void
add_local_enum_reloption(local_relopts *relopts, const char *name,
						 const char *desc, relopt_enum_elt_def *members,
						 int default_val, const char *detailmsg, int offset)
{
	relopt_enum *newoption = init_enum_reloption(RELOPT_KIND_LOCAL,
												 name, desc,
												 members, default_val,
												 detailmsg, 0);

	add_local_reloption(relopts, (relopt_gen *) newoption, offset);
}

/*
 * init_string_reloption
 *		分配并初始化一个新的字符串（string）类型 reloption
 */
static relopt_string *
init_string_reloption(bits32 kinds, const char *name, const char *desc,
					  const char *default_val,
					  validate_string_relopt validator,
					  fill_string_relopt filler,
					  LOCKMODE lockmode)
{
	relopt_string *newoption;

	/* 确保校验器（validator）与默认值的组合是合理的 */
	if (validator)
		(validator) (default_val);

	newoption = (relopt_string *) allocate_reloption(kinds, RELOPT_TYPE_STRING,
													 name, desc, lockmode);
	newoption->validate_cb = validator;
	newoption->fill_cb = filler;
	if (default_val)
	{
		if (kinds == RELOPT_KIND_LOCAL)
			newoption->default_val = strdup(default_val);
		else
			newoption->default_val = MemoryContextStrdup(TopMemoryContext, default_val);
		newoption->default_len = strlen(default_val);
		newoption->default_isnull = false;
	}
	else
	{
		newoption->default_val = "";
		newoption->default_len = 0;
		newoption->default_isnull = true;
	}

	return newoption;
}

/*
 * add_string_reloption
 *		添加一个新的字符串（string）类型 reloption
 *
 * "validator" 是一个可选的函数指针，可用于检验值的合法性。当参数
 * 字符串对该变量不可接受时，它必须通过 elog(ERROR) 报错。注意：默认值
 * 必须通过该校验。
 */
void
add_string_reloption(bits32 kinds, const char *name, const char *desc,
					 const char *default_val, validate_string_relopt validator,
					 LOCKMODE lockmode)
{
	relopt_string *newoption = init_string_reloption(kinds, name, desc,
													 default_val,
													 validator, NULL,
													 lockmode);

	add_reloption((relopt_gen *) newoption);
}

/*
 * add_local_string_reloption
 *		添加一个新的本地字符串（string）类型 reloption
 *
 * 'offset' 是 int 类型字段的偏移量，该字段将存储字符串值在最终
 * 生成的 bytea 结构体中的偏移量。
 */
void
add_local_string_reloption(local_relopts *relopts, const char *name,
						   const char *desc, const char *default_val,
						   validate_string_relopt validator,
						   fill_string_relopt filler, int offset)
{
	relopt_string *newoption = init_string_reloption(RELOPT_KIND_LOCAL,
													 name, desc,
													 default_val,
													 validator, filler,
													 0);

	add_local_reloption(relopts, (relopt_gen *) newoption, offset);
}

/*
 * 将一个关系选项列表（DefElem 的列表）转换为保存在
 * pg_class.reloptions 中的文本数组格式，且只包含位于所传入命名空间
 * 内的那些选项。输出值不包含命名空间。
 *
 * 这用于三种情形：CREATE TABLE/INDEX、ALTER TABLE SET，以及
 * ALTER TABLE RESET。在 ALTER 的情形下，oldOptions 是已有的
 * reloptions 值（可能为 NULL），我们会根据需要替换或移除其中的条目。
 *
 * 如果 acceptOidsOff 为真，则允许 oids = false，但在为 on 时报错。
 * 这纯粹是为了向后兼容而需要的。
 *
 * 注意，本函数并不负责判断这些选项是否合法，但它确实会检查所有给定
 * 选项的命名空间是否都列在 validnsps 中。NULL 命名空间始终合法，
 * 无需显式列出。传入 NULL 指针意味着只有 NULL 命名空间是合法的。
 *
 * oldOptions 与结果都是文本数组（对于 "default" 则为 NULL），但我们
 * 将它们声明为 Datum，以避免在 reloptions.h 中包含 array.h。
 */
Datum
transformRelOptions(Datum oldOptions, List *defList, const char *namspace,
					const char *const validnsps[], bool acceptOidsOff, bool isReset)
{
	Datum		result;
	ArrayBuildState *astate;
	ListCell   *cell;

	/* 空列表则不改变 */
	if (defList == NIL)
		return oldOptions;

	/* 我们使用 accumArrayResult 来构建新数组 */
	astate = NULL;

	/* 复制任何不需要被替换的 oldOptions */
	if (PointerIsValid(DatumGetPointer(oldOptions)))
	{
		ArrayType  *array = DatumGetArrayTypeP(oldOptions);
		Datum	   *oldoptions;
		int			noldoptions;
		int			i;

		deconstruct_array_builtin(array, TEXTOID, &oldoptions, NULL, &noldoptions);

		for (i = 0; i < noldoptions; i++)
		{
			char	   *text_str = VARDATA(oldoptions[i]);
			int			text_len = VARSIZE(oldoptions[i]) - VARHDRSZ;

			/* 在 defList 中查找匹配项 */
			foreach(cell, defList)
			{
				DefElem    *def = (DefElem *) lfirst(cell);
				int			kw_len;

				/* 如果不在同一命名空间中，则忽略 */
				if (namspace == NULL)
				{
					if (def->defnamespace != NULL)
						continue;
				}
				else if (def->defnamespace == NULL)
					continue;
				else if (strcmp(def->defnamespace, namspace) != 0)
					continue;

				kw_len = strlen(def->defname);
				if (text_len > kw_len && text_str[kw_len] == '=' &&
					strncmp(text_str, def->defname, kw_len) == 0)
					break;
			}
			if (!cell)
			{
				/* 没有匹配项，因此保留旧选项 */
				astate = accumArrayResult(astate, oldoptions[i],
										  false, TEXTOID,
										  CurrentMemoryContext);
			}
		}
	}

	/*
	 * 如果是 CREATE/SET，则将新选项加入数组；如果是 RESET，则只需
	 * 检查用户没有写成 RESET (option=val)。（必须这样做，因为语法解析
	 * 器并不强制约束这一点。）
	 */
	foreach(cell, defList)
	{
		DefElem    *def = (DefElem *) lfirst(cell);

		if (isReset)
		{
			if (def->arg != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("RESET must not include values for parameters")));
		}
		else
		{
			const char *name;
			const char *value;
			text	   *t;
			Size		len;

			/*
			 * 如果命名空间不合法则报错。NULL 命名空间始终合法。
			 */
			if (def->defnamespace != NULL)
			{
				bool		valid = false;
				int			i;

				if (validnsps)
				{
					for (i = 0; validnsps[i]; i++)
					{
						if (strcmp(def->defnamespace, validnsps[i]) == 0)
						{
							valid = true;
							break;
						}
					}
				}

				if (!valid)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("unrecognized parameter namespace \"%s\"",
									def->defnamespace)));
			}

			/* 如果不在同一命名空间中，则忽略 */
			if (namspace == NULL)
			{
				if (def->defnamespace != NULL)
					continue;
			}
			else if (def->defnamespace == NULL)
				continue;
			else if (strcmp(def->defnamespace, namspace) != 0)
				continue;

			/*
			 * 将 DefElem 扁平化为形如 "name=arg" 的文本字符串。如果
			 * 只有 "name"，则假定其含义为 "name=true"。注意：命名空间
			 * 不会被输出。
			 */
			name = def->defname;
			if (def->arg != NULL)
				value = defGetString(def);
			else
				value = "true";

			/* 坚持要求名称中不能包含 "="，否则 "a=b=c" 会产生歧义 */
			if (strchr(name, '=') != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("invalid option name \"%s\": must not contain \"=\"",
								name)));

			/*
			 * 这里并不是做这个检查的最佳位置，但也没有其他方便的地方
			 * 可以过滤掉这个选项。由于 WITH (oids = false) 终有一天会被
			 * 移除，因此这点难看似乎尚属可以接受。
			 */
			if (acceptOidsOff && def->defnamespace == NULL &&
				strcmp(name, "oids") == 0)
			{
				if (defGetBoolean(def))
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("tables declared WITH OIDS are not supported")));
				/* 跳过该选项，reloptions 机制并不认识它 */
				continue;
			}

			len = VARHDRSZ + strlen(name) + 1 + strlen(value);
			/* +1 为 sprintf 的结尾空字符留出空间 */
			t = (text *) palloc(len + 1);
			SET_VARSIZE(t, len);
			sprintf(VARDATA(t), "%s=%s", name, value);

			astate = accumArrayResult(astate, PointerGetDatum(t),
									  false, TEXTOID,
									  CurrentMemoryContext);
		}
	}

	if (astate)
		result = makeArrayResult(astate, CurrentMemoryContext);
	else
		result = (Datum) 0;

	return result;
}


/*
 * 将 reloptions 的文本数组格式转换为 DefElem 的列表。
 * 这是 transformRelOptions() 的逆操作。
 */
List *
untransformRelOptions(Datum options)
{
	List	   *result = NIL;
	ArrayType  *array;
	Datum	   *optiondatums;
	int			noptions;
	int			i;

	/* 如果没有选项，则无事可做 */
	if (!PointerIsValid(DatumGetPointer(options)))
		return result;

	array = DatumGetArrayTypeP(options);

	deconstruct_array_builtin(array, TEXTOID, &optiondatums, NULL, &noptions);

	for (i = 0; i < noptions; i++)
	{
		char	   *s;
		char	   *p;
		Node	   *val = NULL;

		s = TextDatumGetCString(optiondatums[i]);
		p = strchr(s, '=');
		if (p)
		{
			*p++ = '\0';
			val = (Node *) makeString(p);
		}
		result = lappend(result, makeDefElem(s, val, -1));
	}

	return result;
}

/*
 * 从 pg_class 元组中抽取并解析 reloptions。
 *
 * 这是一个底层例程，预期由 relcache 代码以及没有表 relcache 条目（例如
 * autovacuum）的调用方使用。对于其他用途，请考虑直接从 relcache 条目
 * 中获取 rd_options 指针。
 *
 * tupdesc 是 pg_class 的元组描述符。对于对应于索引的元组，amoptions 是指向
 * 索引 AM 的 options 解析函数的指针，否则为 NULL。
 */
bytea *
extractRelOptions(HeapTuple tuple, TupleDesc tupdesc,
				  amoptions_function amoptions)
{
	bytea	   *options;
	bool		isnull;
	Datum		datum;
	Form_pg_class classForm;

	datum = fastgetattr(tuple,
						Anum_pg_class_reloptions,
						tupdesc,
						&isnull);
	if (isnull)
		return NULL;

	classForm = (Form_pg_class) GETSTRUCT(tuple);

	/* 解析为适当的格式；此处不报错 */
	switch (classForm->relkind)
	{
		case RELKIND_RELATION:
		case RELKIND_TOASTVALUE:
		case RELKIND_MATVIEW:
			options = heap_reloptions(classForm->relkind, datum, false);
			break;
		case RELKIND_PARTITIONED_TABLE:
			options = partitioned_table_reloptions(datum, false);
			break;
		case RELKIND_VIEW:
			options = view_reloptions(datum, false);
			break;
		case RELKIND_INDEX:
		case RELKIND_PARTITIONED_INDEX:
			options = index_reloptions(amoptions, datum, false);
			break;
		case RELKIND_FOREIGN_TABLE:
			options = NULL;
			break;
		default:
			Assert(false);		/* 不可能到达此处 */
			options = NULL;		/* 让编译器安静（避免告警） */
			break;
	}

	return options;
}

static void
parseRelOptionsInternal(Datum options, bool validate,
						relopt_value *reloptions, int numoptions)
{
	ArrayType  *array = DatumGetArrayTypeP(options);
	Datum	   *optiondatums;
	int			noptions;
	int			i;

	deconstruct_array_builtin(array, TEXTOID, &optiondatums, NULL, &noptions);

	for (i = 0; i < noptions; i++)
	{
		char	   *text_str = VARDATA(optiondatums[i]);
		int			text_len = VARSIZE(optiondatums[i]) - VARHDRSZ;
		int			j;

		/* 在 reloptions 中查找匹配项 */
		for (j = 0; j < numoptions; j++)
		{
			int			kw_len = reloptions[j].gen->namelen;

			if (text_len > kw_len && text_str[kw_len] == '=' &&
				strncmp(text_str, reloptions[j].gen->name, kw_len) == 0)
			{
				parse_one_reloption(&reloptions[j], text_str, text_len,
									validate);
				break;
			}
		}

		if (j >= numoptions && validate)
		{
			char	   *s;
			char	   *p;

			s = TextDatumGetCString(optiondatums[i]);
			p = strchr(s, '=');
			if (p)
				*p = '\0';
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("unrecognized parameter \"%s\"", s)));
		}
	}

	/* 在本函数中避免内存泄漏是值得的 */
	pfree(optiondatums);

	if (((void *) array) != DatumGetPointer(options))
		pfree(array);
}

/*
 * 解析以文本数组格式给出的 reloptions。
 *
 * options 是由 transformRelOptions 构建的 reloption 文本数组。
 * kind 指定了要处理的选项族。
 *
 * 返回值是一个 relopt_value * 数组，其中在 options 数组中实际被设置的
 * 选项会被标记为 isset=true。该数组的长度通过 *numrelopts 返回。未被
 * 设置的选项也会出现在数组中；这样调用方便于轻松地定位默认值。
 *
 * 如果不存在给定类型的选项，则将 numrelopts 设为 0 并返回 NULL（除非
 * 在没有任何选项被定义的情况下仍然非法地提供了选项，此时会发生错误）。
 *
 * 注意：int、bool 和 real 类型的值会作为返回数组的一部分被分配；
 * string 类型的值则单独分配，必须由调用方释放。
 */
static relopt_value *
parseRelOptions(Datum options, bool validate, relopt_kind kind,
				int *numrelopts)
{
	relopt_value *reloptions = NULL;
	int			numoptions = 0;
	int			i;
	int			j;

	if (need_initialization)
		initialize_reloptions();

	/* 基于 kind 构建一组预期选项列表 */

	for (i = 0; relOpts[i]; i++)
		if (relOpts[i]->kinds & kind)
			numoptions++;

	if (numoptions > 0)
	{
		reloptions = palloc(numoptions * sizeof(relopt_value));

		for (i = 0, j = 0; relOpts[i]; i++)
		{
			if (relOpts[i]->kinds & kind)
			{
				reloptions[j].gen = relOpts[i];
				reloptions[j].isset = false;
				j++;
			}
		}
	}

	/* 没有选项则结束 */
	if (PointerIsValid(DatumGetPointer(options)))
		parseRelOptionsInternal(options, validate, reloptions, numoptions);

	*numrelopts = numoptions;
	return reloptions;
}

/* 解析本地的未注册选项。 */
static relopt_value *
parseLocalRelOptions(local_relopts *relopts, Datum options, bool validate)
{
	int			nopts = list_length(relopts->options);
	relopt_value *values = palloc(sizeof(*values) * nopts);
	ListCell   *lc;
	int			i = 0;

	foreach(lc, relopts->options)
	{
		local_relopt *opt = lfirst(lc);

		values[i].gen = opt->option;
		values[i].isset = false;

		i++;
	}

	if (options != (Datum) 0)
		parseRelOptionsInternal(options, validate, values, nopts);

	return values;
}

/*
 * parseRelOptions 的子例程，用于解析并校验单个选项的值
 */
static void
parse_one_reloption(relopt_value *option, char *text_str, int text_len,
					bool validate)
{
	char	   *value;
	int			value_len;
	bool		parsed;
	bool		nofree = false;

	if (option->isset && validate)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("parameter \"%s\" specified more than once",
						option->gen->name)));

	value_len = text_len - option->gen->namelen - 1;
	value = (char *) palloc(value_len + 1);
	memcpy(value, text_str + option->gen->namelen + 1, value_len);
	value[value_len] = '\0';

	switch (option->gen->type)
	{
		case RELOPT_TYPE_BOOL:
			{
				parsed = parse_bool(value, &option->values.bool_val);
				if (validate && !parsed)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("invalid value for boolean option \"%s\": %s",
									option->gen->name, value)));
			}
			break;
		case RELOPT_TYPE_INT:
			{
				relopt_int *optint = (relopt_int *) option->gen;

				parsed = parse_int(value, &option->values.int_val, 0, NULL);
				if (validate && !parsed)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("invalid value for integer option \"%s\": %s",
									option->gen->name, value)));
				if (validate && (option->values.int_val < optint->min ||
								 option->values.int_val > optint->max))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("value %s out of bounds for option \"%s\"",
									value, option->gen->name),
							 errdetail("Valid values are between \"%d\" and \"%d\".",
									   optint->min, optint->max)));
			}
			break;
		case RELOPT_TYPE_REAL:
			{
				relopt_real *optreal = (relopt_real *) option->gen;

				parsed = parse_real(value, &option->values.real_val, 0, NULL);
				if (validate && !parsed)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("invalid value for floating point option \"%s\": %s",
									option->gen->name, value)));
				if (validate && (option->values.real_val < optreal->min ||
								 option->values.real_val > optreal->max))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("value %s out of bounds for option \"%s\"",
									value, option->gen->name),
							 errdetail("Valid values are between \"%f\" and \"%f\".",
									   optreal->min, optreal->max)));
			}
			break;
		case RELOPT_TYPE_ENUM:
			{
				relopt_enum *optenum = (relopt_enum *) option->gen;
				relopt_enum_elt_def *elt;

				parsed = false;
				for (elt = optenum->members; elt->string_val; elt++)
				{
					if (pg_strcasecmp(value, elt->string_val) == 0)
					{
						option->values.enum_val = elt->symbol_val;
						parsed = true;
						break;
					}
				}
				if (validate && !parsed)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("invalid value for enum option \"%s\": %s",
									option->gen->name, value),
							 optenum->detailmsg ?
							 errdetail_internal("%s", _(optenum->detailmsg)) : 0));

			/*
			 * 如果值不在允许的字符串值集合中，但并没有要求我们进行
			 * 校验，则直接使用默认的数值。
			 */
			if (!parsed)
				option->values.enum_val = optenum->default_val;
			}
			break;
		case RELOPT_TYPE_STRING:
			{
				relopt_string *optstring = (relopt_string *) option->gen;

				option->values.string_val = value;
				nofree = true;
				if (validate && optstring->validate_cb)
					(optstring->validate_cb) (value);
				parsed = true;
			}
			break;
		default:
			elog(ERROR, "unsupported reloption type %d", option->gen->type);
			parsed = true;		/* 让编译器安静（避免告警） */
			break;
	}

	if (parsed)
		option->isset = true;
	if (!nofree)
		pfree(value);
}

/*
 * 给定 parseRelOptions 的结果，分配一个结构体，其大小为指定的基础大小
 * 加上字符串变量所需的额外空间。
 *
 * "base" 应为 reloptions 结构体（StdRdOptions 或等价物）的 sizeof(struct)。
 */
static void *
allocateReloptStruct(Size base, relopt_value *options, int numoptions)
{
	Size		size = base;
	int			i;

	for (i = 0; i < numoptions; i++)
	{
		relopt_value *optval = &options[i];

		if (optval->gen->type == RELOPT_TYPE_STRING)
		{
			relopt_string *optstr = (relopt_string *) optval->gen;

			if (optstr->fill_cb)
			{
				const char *val = optval->isset ? optval->values.string_val :
					optstr->default_isnull ? NULL : optstr->default_val;

				size += optstr->fill_cb(val, NULL);
			}
			else
				size += GET_STRING_RELOPTION_LEN(*optval) + 1;
		}
	}

	return palloc0(size);
}

/*
 * 给定 parseRelOptions 的结果以及一个解析表，用解析得到的值填充
 * 结构体（该结构体先前已由 allocateReloptStruct 分配）。
 *
 * rdopts 是指向待填充的已分配结构体的指针。
 * basesize 是传给 allocateReloptStruct 的 sizeof(struct)。
 * options 长度为 numoptions，是 parseRelOptions 的输出。
 * elems 长度为 numelems，是描述所允许选项的表。
 * 当 validate 为真时，期望所有选项都出现在 elems 中。
 */
static void
fillRelOptions(void *rdopts, Size basesize,
			   relopt_value *options, int numoptions,
			   bool validate,
			   const relopt_parse_elt *elems, int numelems)
{
	int			i;
	int			offset = basesize;

	for (i = 0; i < numoptions; i++)
	{
		int			j;
		bool		found = false;

		for (j = 0; j < numelems; j++)
		{
			if (strcmp(options[i].gen->name, elems[j].optname) == 0)
			{
				relopt_string *optstring;
				char	   *itempos = ((char *) rdopts) + elems[j].offset;
				char	   *string_val;

			/*
			 * 如果提供了 isset_offset，则在对应位置存储该 reloption
			 * 是否被设置。
			 */
				if (elems[j].isset_offset > 0)
				{
					char	   *setpos = ((char *) rdopts) + elems[j].isset_offset;

					*(bool *) setpos = options[i].isset;
				}

				switch (options[i].gen->type)
				{
					case RELOPT_TYPE_BOOL:
						*(bool *) itempos = options[i].isset ?
							options[i].values.bool_val :
							((relopt_bool *) options[i].gen)->default_val;
						break;
					case RELOPT_TYPE_INT:
						*(int *) itempos = options[i].isset ?
							options[i].values.int_val :
							((relopt_int *) options[i].gen)->default_val;
						break;
					case RELOPT_TYPE_REAL:
						*(double *) itempos = options[i].isset ?
							options[i].values.real_val :
							((relopt_real *) options[i].gen)->default_val;
						break;
					case RELOPT_TYPE_ENUM:
						*(int *) itempos = options[i].isset ?
							options[i].values.enum_val :
							((relopt_enum *) options[i].gen)->default_val;
						break;
					case RELOPT_TYPE_STRING:
						optstring = (relopt_string *) options[i].gen;
						if (options[i].isset)
							string_val = options[i].values.string_val;
						else if (!optstring->default_isnull)
							string_val = optstring->default_val;
						else
							string_val = NULL;

						if (optstring->fill_cb)
						{
							Size		size =
								optstring->fill_cb(string_val,
												   (char *) rdopts + offset);

							if (size)
							{
								*(int *) itempos = offset;
								offset += size;
							}
							else
								*(int *) itempos = 0;
						}
						else if (string_val == NULL)
							*(int *) itempos = 0;
						else
						{
							strcpy((char *) rdopts + offset, string_val);
							*(int *) itempos = offset;
							offset += strlen(string_val) + 1;
						}
						break;
					default:
						elog(ERROR, "unsupported reloption type %d",
							 options[i].gen->type);
						break;
				}
				found = true;
				break;
			}
		}
		if (validate && !found)
			elog(ERROR, "reloption \"%s\" not found in parse table",
				 options[i].gen->name);
	}
	SET_VARSIZE(rdopts, offset);
}


/*
 * 用于任何使用 StdRdOptions 的对象的选项解析器。
 */
bytea *
default_reloptions(Datum reloptions, bool validate, relopt_kind kind)
{
	static const relopt_parse_elt tab[] = {
		{"fillfactor", RELOPT_TYPE_INT, offsetof(StdRdOptions, fillfactor)},
		{"autovacuum_enabled", RELOPT_TYPE_BOOL,
		offsetof(StdRdOptions, autovacuum) + offsetof(AutoVacOpts, enabled)},
		{"autovacuum_vacuum_threshold", RELOPT_TYPE_INT,
		offsetof(StdRdOptions, autovacuum) + offsetof(AutoVacOpts, vacuum_threshold)},
		{"autovacuum_vacuum_max_threshold", RELOPT_TYPE_INT,
		offsetof(StdRdOptions, autovacuum) + offsetof(AutoVacOpts, vacuum_max_threshold)},
		{"autovacuum_vacuum_insert_threshold", RELOPT_TYPE_INT,
		offsetof(StdRdOptions, autovacuum) + offsetof(AutoVacOpts, vacuum_ins_threshold)},
		{"autovacuum_analyze_threshold", RELOPT_TYPE_INT,
		offsetof(StdRdOptions, autovacuum) + offsetof(AutoVacOpts, analyze_threshold)},
		{"autovacuum_vacuum_cost_limit", RELOPT_TYPE_INT,
		offsetof(StdRdOptions, autovacuum) + offsetof(AutoVacOpts, vacuum_cost_limit)},
		{"autovacuum_freeze_min_age", RELOPT_TYPE_INT,
		offsetof(StdRdOptions, autovacuum) + offsetof(AutoVacOpts, freeze_min_age)},
		{"autovacuum_freeze_max_age", RELOPT_TYPE_INT,
		offsetof(StdRdOptions, autovacuum) + offsetof(AutoVacOpts, freeze_max_age)},
		{"autovacuum_freeze_table_age", RELOPT_TYPE_INT,
		offsetof(StdRdOptions, autovacuum) + offsetof(AutoVacOpts, freeze_table_age)},
		{"autovacuum_multixact_freeze_min_age", RELOPT_TYPE_INT,
		offsetof(StdRdOptions, autovacuum) + offsetof(AutoVacOpts, multixact_freeze_min_age)},
		{"autovacuum_multixact_freeze_max_age", RELOPT_TYPE_INT,
		offsetof(StdRdOptions, autovacuum) + offsetof(AutoVacOpts, multixact_freeze_max_age)},
		{"autovacuum_multixact_freeze_table_age", RELOPT_TYPE_INT,
		offsetof(StdRdOptions, autovacuum) + offsetof(AutoVacOpts, multixact_freeze_table_age)},
		{"log_autovacuum_min_duration", RELOPT_TYPE_INT,
		offsetof(StdRdOptions, autovacuum) + offsetof(AutoVacOpts, log_min_duration)},
		{"toast_tuple_target", RELOPT_TYPE_INT,
		offsetof(StdRdOptions, toast_tuple_target)},
		{"autovacuum_vacuum_cost_delay", RELOPT_TYPE_REAL,
		offsetof(StdRdOptions, autovacuum) + offsetof(AutoVacOpts, vacuum_cost_delay)},
		{"autovacuum_vacuum_scale_factor", RELOPT_TYPE_REAL,
		offsetof(StdRdOptions, autovacuum) + offsetof(AutoVacOpts, vacuum_scale_factor)},
		{"autovacuum_vacuum_insert_scale_factor", RELOPT_TYPE_REAL,
		offsetof(StdRdOptions, autovacuum) + offsetof(AutoVacOpts, vacuum_ins_scale_factor)},
		{"autovacuum_analyze_scale_factor", RELOPT_TYPE_REAL,
		offsetof(StdRdOptions, autovacuum) + offsetof(AutoVacOpts, analyze_scale_factor)},
		{"user_catalog_table", RELOPT_TYPE_BOOL,
		offsetof(StdRdOptions, user_catalog_table)},
		{"parallel_workers", RELOPT_TYPE_INT,
		offsetof(StdRdOptions, parallel_workers)},
		{"vacuum_index_cleanup", RELOPT_TYPE_ENUM,
		offsetof(StdRdOptions, vacuum_index_cleanup)},
		{"vacuum_truncate", RELOPT_TYPE_BOOL,
		offsetof(StdRdOptions, vacuum_truncate), offsetof(StdRdOptions, vacuum_truncate_set)},
		{"vacuum_max_eager_freeze_failure_rate", RELOPT_TYPE_REAL,
		offsetof(StdRdOptions, vacuum_max_eager_freeze_failure_rate)}
	};

	return (bytea *) build_reloptions(reloptions, validate, kind,
									  sizeof(StdRdOptions),
									  tab, lengthof(tab));
}

/*
 * build_reloptions
 *
 * 解析由调用方提供的 "reloptions"，并将它们返回在一个包含了已解析选项
 * 的结构体中。解析工作借助一张解析表来完成，该表描述了所允许的选项，
 * 由长度为 "num_relopt_elems" 的 "relopt_elems" 定义。
 *
 * 如果 reloptions 值是由 transformRelOptions() 新近构建的（而非从系统
 * 目录中读出），则 "validate" 必须为真；在后一种情形下，其中包含的
 * 值必然已经合法。
 *
 * 如果传入的选项与解析表中的任何选项都不匹配，则返回 NULL；除非
 * validate 为真，此时会报错。
 */
void *
build_reloptions(Datum reloptions, bool validate,
				 relopt_kind kind,
				 Size relopt_struct_size,
				 const relopt_parse_elt *relopt_elems,
				 int num_relopt_elems)
{
	int			numoptions;
	relopt_value *options;
	void	   *rdopts;

	/* 解析针对给定关系选项类型特有的选项 */
	options = parseRelOptions(reloptions, validate, kind, &numoptions);
	Assert(numoptions <= num_relopt_elems);

	/* 如果没有设置任何选项，则结束 */
	if (numoptions == 0)
	{
		Assert(options == NULL);
		return NULL;
	}

	/* 分配并填充结构体 */
	rdopts = allocateReloptStruct(relopt_struct_size, options, numoptions);
	fillRelOptions(rdopts, relopt_struct_size, options, numoptions,
				   validate, relopt_elems, num_relopt_elems);

	pfree(options);

	return rdopts;
}

/*
 * 解析本地选项，分配一个大小为指定的 'base_size' 加上字符串变量所需
 * 额外空间的 bytea 结构体，填充位于给定偏移量处的选项字段，并将其返回。
 */
void *
build_local_reloptions(local_relopts *relopts, Datum options, bool validate)
{
	int			noptions = list_length(relopts->options);
	relopt_parse_elt *elems = palloc(sizeof(*elems) * noptions);
	relopt_value *vals;
	void	   *opts;
	int			i = 0;
	ListCell   *lc;

	foreach(lc, relopts->options)
	{
		local_relopt *opt = lfirst(lc);

		elems[i].optname = opt->option->name;
		elems[i].opttype = opt->option->type;
		elems[i].offset = opt->offset;
		elems[i].isset_offset = 0;	/* 本地 reloption 目前尚不支持此功能 */

		i++;
	}

	vals = parseLocalRelOptions(relopts, options, validate);
	opts = allocateReloptStruct(relopts->relopt_struct_size, vals, noptions);
	fillRelOptions(opts, relopts->relopt_struct_size, vals, noptions, validate,
				   elems, noptions);

	if (validate)
		foreach(lc, relopts->validators)
			((relopts_validator) lfirst(lc)) (opts, vals, noptions);

	if (elems)
		pfree(elems);

	return opts;
}

/*
 * 用于分区表的选项解析器
 */
bytea *
partitioned_table_reloptions(Datum reloptions, bool validate)
{
	if (validate && reloptions)
		ereport(ERROR,
				errcode(ERRCODE_WRONG_OBJECT_TYPE),
				errmsg("cannot specify storage parameters for a partitioned table"),
				errhint("Specify storage parameters for its leaf partitions instead."));
	return NULL;
}

/*
 * 用于视图的选项解析器
 */
bytea *
view_reloptions(Datum reloptions, bool validate)
{
	static const relopt_parse_elt tab[] = {
		{"security_barrier", RELOPT_TYPE_BOOL,
		offsetof(ViewOptions, security_barrier)},
		{"security_invoker", RELOPT_TYPE_BOOL,
		offsetof(ViewOptions, security_invoker)},
		{"check_option", RELOPT_TYPE_ENUM,
		offsetof(ViewOptions, check_option)}
	};

	return (bytea *) build_reloptions(reloptions, validate,
									  RELOPT_KIND_VIEW,
									  sizeof(ViewOptions),
									  tab, lengthof(tab));
}

/*
 * 为堆表、视图以及 TOAST 表解析选项。
 */
bytea *
heap_reloptions(char relkind, Datum reloptions, bool validate)
{
	StdRdOptions *rdopts;

	switch (relkind)
	{
		case RELKIND_TOASTVALUE:
			rdopts = (StdRdOptions *)
				default_reloptions(reloptions, validate, RELOPT_KIND_TOAST);
			if (rdopts != NULL)
			{
				/* 调整仅适用于默认值的参数（针对 TOAST 关系） */
				rdopts->fillfactor = 100;
				rdopts->autovacuum.analyze_threshold = -1;
				rdopts->autovacuum.analyze_scale_factor = -1;
			}
			return (bytea *) rdopts;
		case RELKIND_RELATION:
		case RELKIND_MATVIEW:
			return default_reloptions(reloptions, validate, RELOPT_KIND_HEAP);
		default:
			/* 其他关系类型不受支持 */
			return NULL;
	}
}


/*
 * 为索引解析选项。
 *
 *	amoptions	索引 AM 的 options 解析函数
 *	reloptions	以 text[] datum 形式给出的选项
 *	validate	是否报错的标志
 */
bytea *
index_reloptions(amoptions_function amoptions, Datum reloptions, bool validate)
{
	Assert(amoptions != NULL);

	/* 假定该函数是严格（strict）的 */
	if (!PointerIsValid(DatumGetPointer(reloptions)))
		return NULL;

	return amoptions(reloptions, validate);
}

/*
 * 用于属性（attribute）reloption 的选项解析器
 */
bytea *
attribute_reloptions(Datum reloptions, bool validate)
{
	static const relopt_parse_elt tab[] = {
		{"n_distinct", RELOPT_TYPE_REAL, offsetof(AttributeOpts, n_distinct)},
		{"n_distinct_inherited", RELOPT_TYPE_REAL, offsetof(AttributeOpts, n_distinct_inherited)}
	};

	return (bytea *) build_reloptions(reloptions, validate,
									  RELOPT_KIND_ATTRIBUTE,
									  sizeof(AttributeOpts),
									  tab, lengthof(tab));
}

/*
 * 用于表空间（tablespace）reloption 的选项解析器
 */
bytea *
tablespace_reloptions(Datum reloptions, bool validate)
{
	static const relopt_parse_elt tab[] = {
		{"random_page_cost", RELOPT_TYPE_REAL, offsetof(TableSpaceOpts, random_page_cost)},
		{"seq_page_cost", RELOPT_TYPE_REAL, offsetof(TableSpaceOpts, seq_page_cost)},
		{"effective_io_concurrency", RELOPT_TYPE_INT, offsetof(TableSpaceOpts, effective_io_concurrency)},
		{"maintenance_io_concurrency", RELOPT_TYPE_INT, offsetof(TableSpaceOpts, maintenance_io_concurrency)}
	};

	return (bytea *) build_reloptions(reloptions, validate,
									  RELOPT_KIND_TABLESPACE,
									  sizeof(TableSpaceOpts),
									  tab, lengthof(tab));
}

/*
 * 从一个选项列表中确定所需的 LOCKMODE。
 *
 * 由 AlterTableGetLockLevel() 调用，关于其工作方式的更详细解释
 * 请参见该函数。
 */
LOCKMODE
AlterTableGetRelOptionsLockLevel(List *defList)
{
	LOCKMODE	lockmode = NoLock;
	ListCell   *cell;

	if (defList == NIL)
		return AccessExclusiveLock;

	if (need_initialization)
		initialize_reloptions();

	foreach(cell, defList)
	{
		DefElem    *def = (DefElem *) lfirst(cell);
		int			i;

		for (i = 0; relOpts[i]; i++)
		{
			if (strncmp(relOpts[i]->name,
						def->defname,
						relOpts[i]->namelen + 1) == 0)
			{
				if (lockmode < relOpts[i]->lockmode)
					lockmode = relOpts[i]->lockmode;
			}
		}
	}

	return lockmode;
}
