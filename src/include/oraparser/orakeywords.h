/*-------------------------------------------------------------------------
 *
 * orakeywords.h
 *	  list of SQL keywords
 *
 *
 *
 * src/include/oraparser/orakeywords.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ORA_KEYWORDS_H
#define ORA_KEYWORDS_H

#include "common/kwlookup.h"

/* Keyword categories --- should match lists in gram.y */
#define UNRESERVED_KEYWORD		0
#define COL_NAME_KEYWORD		1
#define TYPE_FUNC_NAME_KEYWORD	2
#define RESERVED_KEYWORD		3

extern const ScanKeywordList OraScanKeywords;
extern const uint8 OraScanKeywordCategories[];
extern const bool OraScanKeywordBareLabel[];


#endif							/* ORA_KEYWORDS_H */
