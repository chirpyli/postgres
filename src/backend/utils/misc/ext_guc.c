/*-------------------------------------------------------------------------
 *
 * ext_guc.c
 *
 * Support for grand unified configuration scheme(for new added), including SET
 * command, configuration file, and command line options
 *
 *
 * IDENTIFICATION
 *        src/backend/utils/misc/ext_guc.c
 *
 *-------------------------------------------------------------------------
 */

#ifdef EXT_GUC_VAR_DEFINE
int	database_mode = DB_PG;
int	compatible_db = PG_PARSER;

#endif

#ifdef EXT_GUC_VAR_STRUCT
/* The comments shown as blow define the
 * value range of guc parameters "database_mode"
 * and "compatible_db".
 */
static const struct config_enum_entry db_mode_options[] = {
	{"pg", DB_PG, false},
	{"oracle", DB_ORACLE, false},
	{"0", DB_PG, false},
	{"1", DB_ORACLE, false},
	{NULL, 0, false}
};

static const struct config_enum_entry db_parser_options[] = {
	{"pg", PG_PARSER, false},
	{"oracle", ORA_PARSER, false},
	{NULL, 0, false}

};
#endif



#ifdef EXT_GUC_FUNC_DECLARE
static bool check_compatible_mode(int *newval, void **extra, GucSource source);
static bool check_database_mode(int *newval, void **extra, GucSource source);
void assign_compatible_mode(int newval, void *extra);
#endif



#if 0
static struct config_enum Ext_ConfigureNamesEnum[] =
{
#endif
#ifdef EXT_GUC_ENUM_PARAMS

	{
		{"extend.database_mode", PGC_INTERNAL, PRESET_OPTIONS,
			gettext_noop("Set database mode"),
			NULL,
			GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
		},
		&database_mode,
		DB_PG, db_mode_options,
		check_database_mode, NULL, NULL
	},

	{
		{"extend.compatible_mode", PGC_USERSET, CLIENT_CONN_STATEMENT,
			gettext_noop("Set default sql parser compatibility mode"),
			NULL,
			GUC_NOT_IN_SAMPLE | GUC_DISALLOW_IN_FILE
		},
		&compatible_db,
		PG_PARSER, db_parser_options,
		check_compatible_mode, assign_compatible_mode, NULL
	},

#endif
#if 0
	/* End-of-list marker */
	{
		{NULL, 0, 0, NULL, NULL}, NULL, 0, NULL, NULL, NULL, NULL
	}
};
#endif

#ifdef EXT_GUC_FUNC_DEFINE

static bool
check_compatible_mode(int *newval, void **extra, GucSource source)
{
	int		newmode = *newval;

	// if (DB_PG == database_mode && newmode == DB_ORACLE)
	// 		ereport(ERROR,
	// 			(errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),
	// 			errmsg("parameter extsql.compatible_mode cannot be changed in native PG mode.")));

	// if(DB_ORACLE == database_mode
	// 	&&  (IsNormalProcessingMode() || (IsUnderPostmaster && MyProcPort)))
	// {
	// 	if (newmode == DB_ORACLE)
	// 	{
	// 		if (ora_raw_parser == NULL)
	// 			ereport(ERROR,
	// 				(errcode(ERRCODE_SYSTEM_ERROR),
	// 				errmsg("liboracle_parser not found!"),
	// 				errhint("You must load liboracle_parser to use oracle parser.")));
	// 		if (!ISLOADIVORYSQL_ORA)
	// 			ereport(ERROR,
	// 				(errcode(ERRCODE_SYSTEM_ERROR),
	// 				errmsg("IVORYSQL_ORA library not found!"),
	// 				errhint("You must load IVORYSQL_ORA to use oracle parser..")));
	// 	}
	// }
	return true;
}

static bool
check_database_mode(int *newval, void **extra, GucSource source)
{
	int		newmode = *newval;

	if (DB_PG == database_mode && DB_ORACLE == newmode)
			ereport(NOTICE,
				(errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),
				errmsg("parameter extend.database_mode cannot be changed")));

	return true;
}

void
assign_compatible_mode(int newval, void *extra)
{
	if(DB_ORACLE == database_mode
		&&  (IsNormalProcessingMode() || (IsUnderPostmaster && MyProcPort)))
	{
		if (newval == DB_ORACLE)
		{
			// sql_raw_parser = ora_raw_parser;


			// pg_transform_merge_stmt_hook = ora_transform_merge_stmt_hook;
			// pg_exec_merge_matched_hook = ora_exec_merge_matched_hook;

			assign_search_path(NULL, NULL);
		}
		else if (newval == DB_PG)
		{
			// sql_raw_parser = standard_raw_parser;


			// pg_transform_merge_stmt_hook = transformMergeStmt;
			// pg_exec_merge_matched_hook = ExecMergeMatched;

			assign_search_path(NULL, NULL);
		}
	}
}
#endif