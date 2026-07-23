/*-------------------------------------------------------------------------
 *
 * attnum.h
 *	  POSTGRES 属性号定义。
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/attnum.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ATTNUM_H
#define ATTNUM_H


/*
 * user defined attribute numbers start at 1.   -ay 2/95
 */
typedef int16 AttrNumber;

#define InvalidAttrNumber		0
#define MaxAttrNumber			32767

/* ----------------
 *		support macros
 * ----------------
 */
/*
 * AttributeNumberIsValid
 *		当且仅当属性号有效时为真。
 */
#define AttributeNumberIsValid(attributeNumber) \
	((bool) ((attributeNumber) != InvalidAttrNumber))

/*
 * AttrNumberIsForUserDefinedAttr
 *		当且仅当属性号对应用户定义的属性时为真。
 */
#define AttrNumberIsForUserDefinedAttr(attributeNumber) \
	((bool) ((attributeNumber) > 0))

/*
 * AttrNumberGetAttrOffset
 *		Returns the attribute offset for an attribute number.
 *
 * Note:
 *		假定属性号对应用户定义的属性。
 */
#define AttrNumberGetAttrOffset(attNum) \
( \
	AssertMacro(AttrNumberIsForUserDefinedAttr(attNum)), \
	((attNum) - 1) \
)

/*
 * AttrOffsetGetAttrNumber
 *		返回属性偏移量对应的属性号。
 */
#define AttrOffsetGetAttrNumber(attributeOffset) \
	 ((AttrNumber) (1 + (attributeOffset)))

#endif							/* ATTNUM_H */
