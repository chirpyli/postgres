/*--------------------------------------------------------------------
 *
 * sql_compatible.h
 *
 * Definition enumeration structure is fro supporting different compatibility modes.
 *
 *
 * src/include/utils/sql_compatible.h
 *
 * add the file for requirement "SQL PARSER"
 *
 *----------------------------------------------------------------------
 */

#ifndef SQL_COMPATIBLE_H
#define SQL_COMPATIBLE_H

#define DB_MODE_PARMATER "extend.database_mode"

typedef enum DBMode
{
	DB_PG = 0,
	DB_ORACLE
}DBMode;

typedef enum DBParser
{
	PG_PARSER = 0,
	ORA_PARSER
}DBParser;

#endif							/* SQL_COMPATIBLE_H */
