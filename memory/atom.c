/*
 * Atom table functions
 *
 * Copyright 1993, 1994, 1995 Alexandre Julliard
 */

/*
 * Warning: The code assumes that LocalAlloc() returns a block aligned
 * on a 4-bytes boundary (because of the shifting done in
 * HANDLETOATOM).  If this is not the case, the allocation code will
 * have to be changed.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "wine/winbase16.h"
#include "wine/winuser16.h"
#include "winuser.h"
#include "global.h"
#include "instance.h"
#include "ldt.h"
#include "stackframe.h"
#include "user.h"
#include "debugtools.h"

#ifdef CONFIG_IPC
#include "dde_atom.h"
#endif

DEFAULT_DEBUG_CHANNEL(atom)

#define DEFAULT_ATOMTABLE_SIZE    37
#define MIN_STR_ATOM              0xc000
#define MAX_ATOM_LEN              255

#define ATOMTOHANDLE(atom)        ((HANDLE16)(atom) << 2)
#define HANDLETOATOM(handle)      ((ATOM)(0xc000 | ((handle) >> 2)))

#define HAS_ATOM_TABLE(sel)  \
          ((INSTANCEDATA*)PTR_SEG_OFF_TO_LIN(sel,0))->atomtable != 0)

#define GET_ATOM_TABLE(sel)  ((ATOMTABLE*)PTR_SEG_OFF_TO_LIN(sel, \
          ((INSTANCEDATA*)PTR_SEG_OFF_TO_LIN(sel,0))->atomtable))

typedef struct
{
    HANDLE16    next;
    WORD        refCount;
    BYTE        length;
    BYTE        str[1];
} ATOMENTRY;

typedef struct
{
    WORD        size;
    HANDLE16    entries[1];
} ATOMTABLE;
		
static WORD ATOM_GlobalTable = 0;

/***********************************************************************
 *           ATOM_InitTable
 *
 * NOTES
 *	Should this validate the value of entries to be 0 < x < 0x3fff?
 *
 * RETURNS
 *	Handle: Success
 *	0: Failure
 */
static HANDLE16 ATOM_InitTable(
                WORD selector, /* [in] Segment */
                WORD entries   /* [in] Size of atom table */
) {
    int i;
    HANDLE16 handle;
    ATOMTABLE *table;

      /* We consider the first table to be initialized as the global table. 
       * This works, as USER (both built-in and native) is the first one to 
       * register ... 
       */

    if (!ATOM_GlobalTable) ATOM_GlobalTable = selector; 


      /* Allocate the table */

    handle = LOCAL_Alloc( selector, LMEM_FIXED,
                          sizeof(ATOMTABLE) + (entries-1) * sizeof(HANDLE16) );
    if (!handle) return 0;
    table = (ATOMTABLE *)PTR_SEG_OFF_TO_LIN( selector, handle );
    table->size = entries;
    for (i = 0; i < entries; i++) table->entries[i] = 0;

      /* Store a pointer to the table in the instance data */

    ((INSTANCEDATA *)PTR_SEG_OFF_TO_LIN( selector, 0 ))->atomtable = handle;
    return handle;
}


/***********************************************************************
 *           ATOM_Init
 *
 * Global table initialisation.
 */
BOOL ATOM_Init( WORD globalTableSel )
{
    return ATOM_InitTable( globalTableSel, DEFAULT_ATOMTABLE_SIZE ) != 0;
}


/***********************************************************************
 *           ATOM_GetTable
 *
 * Return a pointer to the atom table of a given segment, creating
 * it if necessary.
 *
 * RETURNS
 *	Pointer to table: Success
 *	NULL: Failure
 */
static ATOMTABLE *ATOM_GetTable(
                  WORD selector, /* [in] Segment */
                  BOOL create  /* [in] Create */ )
{
    INSTANCEDATA *ptr = (INSTANCEDATA *)PTR_SEG_OFF_TO_LIN( selector, 0 );
    if (ptr->atomtable)
    {
        ATOMTABLE *table = (ATOMTABLE *)((char *)ptr + ptr->atomtable);
        if (table->size) return table;
    }
    if (!create) return NULL;
    if (!ATOM_InitTable( selector, DEFAULT_ATOMTABLE_SIZE )) return NULL;
    /* Reload ptr in case it moved in linear memory */
    ptr = (INSTANCEDATA *)PTR_SEG_OFF_TO_LIN( selector, 0 );
    return (ATOMTABLE *)((char *)ptr + ptr->atomtable);
}


/***********************************************************************
 *           ATOM_MakePtr
 *
 * Make an ATOMENTRY pointer from a handle (obtained from GetAtomHandle()).
 */
static ATOMENTRY *ATOM_MakePtr(
                  WORD selector,  /* [in] Segment */
                  HANDLE16 handle /* [in] Handle */
) {
    return (ATOMENTRY *)PTR_SEG_OFF_TO_LIN( selector, handle );
}


/***********************************************************************
 *           ATOM_Hash
 * RETURNS
 *	The hash value for the input string
 */
static WORD ATOM_Hash(
            WORD entries, /* [in] Total number of entries */
            LPCSTR str,   /* [in] Pointer to string to hash */
            WORD len      /* [in] Length of string */
) {
    WORD i, hash = 0;

    TRACE("%x, %s, %x\n", entries, str, len);
    
    for (i = 0; i < len; i++) hash ^= toupper(str[i]) + i;
    return hash % entries;
}

static BOOL ATOM_IsIntAtom(LPCSTR atomstr,WORD *atomid) {
	LPSTR 	xend;

	if (!HIWORD(atomstr)) {
		*atomid = LOWORD(atomstr);
		return TRUE;
	}
	if (atomstr[0]!='#')
		return FALSE;
	*atomid=strtol(atomstr+1,&xend,10);
	if (*xend) {
		FIXME("found atom named '%s'\n",atomstr);
		return FALSE;
	}
	return TRUE;
}


/***********************************************************************
 *           ATOM_AddAtom
 *
 * Windows DWORD aligns the atom entry size.
 * The remaining unused string space created by the alignment
 * gets padded with '\0's in a certain way to ensure
 * that at least one trailing '\0' remains.
 *
 * RETURNS
 *	Atom: Success
 *	0: Failure
 */
static ATOM ATOM_AddAtom(
            WORD selector, /* [in] Segment */
            LPCSTR str     /* [in] Pointer to the string to add */
) {
    WORD hash;
    HANDLE16 entry;
    ATOMENTRY * entryPtr;
    ATOMTABLE * table;
    int len, ae_len;
    WORD	iatom;

    TRACE("0x%x, %s\n", selector, str);
    
    if (ATOM_IsIntAtom(str,&iatom))
    	return iatom;
    if ((len = strlen( str )) > MAX_ATOM_LEN) len = MAX_ATOM_LEN;
    if (!(table = ATOM_GetTable( selector, TRUE ))) return 0;
    hash = ATOM_Hash( table->size, str, len );
    entry = table->entries[hash];
    while (entry)
    {
	entryPtr = ATOM_MakePtr( selector, entry );
	if ((entryPtr->length == len) && 
	    (!lstrncmpiA( entryPtr->str, str, len )))
	{
	    entryPtr->refCount++;
        TRACE("-- existing 0x%x\n", entry);
	    return HANDLETOATOM( entry );
	}
	entry = entryPtr->next;
    }

    ae_len = (sizeof(ATOMENTRY)+len+3) & ~3;
    entry = LOCAL_Alloc( selector, LMEM_FIXED, ae_len);
    if (!entry) return 0;
    /* Reload the table ptr in case it moved in linear memory */
    table = ATOM_GetTable( selector, FALSE );
    entryPtr = ATOM_MakePtr( selector, entry );
    entryPtr->next = table->entries[hash];
    entryPtr->refCount = 1;
    entryPtr->length = len;
    strncpy( entryPtr->str, str, ae_len - sizeof(ATOMENTRY) + 1); /* always use strncpy ('\0's padding) */
    table->entries[hash] = entry;
    TRACE("-- new 0x%x\n", entry);
    return HANDLETOATOM( entry );
}


/***********************************************************************
 *           ATOM_DeleteAtom
 * RETURNS
 *	0: Success
 *	Atom: Failure
 */
static ATOM ATOM_DeleteAtom(
            WORD selector, /* [in] Segment */
            ATOM atom      /* [in] Atom to delete */
) {
    ATOMENTRY * entryPtr;
    ATOMTABLE * table;
    HANDLE16 entry, *prevEntry;
    WORD hash;

    TRACE("0x%x, 0x%x\n", selector, atom);
    
    if (atom < MIN_STR_ATOM) return 0;  /* Integer atom */

    if (!(table = ATOM_GetTable( selector, FALSE ))) return 0;
    entry = ATOMTOHANDLE( atom );
    entryPtr = ATOM_MakePtr( selector, entry );

      /* Find previous atom */
    hash = ATOM_Hash( table->size, entryPtr->str, entryPtr->length );
    prevEntry = &table->entries[hash];
    while (*prevEntry && *prevEntry != entry)
    {
	ATOMENTRY * prevEntryPtr = ATOM_MakePtr( selector, *prevEntry );
	prevEntry = &prevEntryPtr->next;
    }    
    if (!*prevEntry) return atom;

      /* Delete atom */
    if (--entryPtr->refCount == 0)
    {
	*prevEntry = entryPtr->next;
        LOCAL_Free( selector, entry );
    }    
    return 0;
}


/***********************************************************************
 *           ATOM_FindAtom
 * RETURNS
 *	Atom: Success
 *	0: Failure
 */
static ATOM ATOM_FindAtom(
            WORD selector, /* [in] Segment */
            LPCSTR str     /* [in] Pointer to string to find */
) {
    ATOMTABLE * table;
    WORD hash,iatom;
    HANDLE16 entry;
    int len;

    TRACE("%x, %s\n", selector, str);
    if (ATOM_IsIntAtom(str,&iatom))
    	return iatom;
    if ((len = strlen( str )) > 255) len = 255;
    if (!(table = ATOM_GetTable( selector, FALSE ))) return 0;
    hash = ATOM_Hash( table->size, str, len );
    entry = table->entries[hash];
    while (entry)
    {
	ATOMENTRY * entryPtr = ATOM_MakePtr( selector, entry );
	if ((entryPtr->length == len) && 
	    (!lstrncmpiA( entryPtr->str, str, len )))
    {    TRACE("-- found %x\n", entry);
	    return HANDLETOATOM( entry );
    }
	entry = entryPtr->next;
    }
    TRACE("-- not found\n");
    return 0;
}


/***********************************************************************
 *           ATOM_GetAtomName
 * RETURNS
 *	Length of string copied to buffer: Success
 *	0: Failure
 */
static UINT ATOM_GetAtomName(
              WORD selector, /* [in]  Segment */
              ATOM atom,     /* [in]  Atom identifier */
              LPSTR buffer,  /* [out] Pointer to buffer for atom string */
              INT count    /* [in]  Size of buffer */
) {
    ATOMTABLE * table;
    ATOMENTRY * entryPtr;
    HANDLE16 entry;
    char * strPtr;
    UINT len;
    char text[8];
    
    TRACE("%x, %x\n", selector, atom);
    
    if (!count) return 0;
    if (atom < MIN_STR_ATOM)
    {
	sprintf( text, "#%d", atom );
	len = strlen(text);
	strPtr = text;
    }
    else
    {
       if (!(table = ATOM_GetTable( selector, FALSE ))) return 0;
	entry = ATOMTOHANDLE( atom );
	entryPtr = ATOM_MakePtr( selector, entry );
	len = entryPtr->length;
	strPtr = entryPtr->str;
    }
    if (len >= count) len = count-1;
    memcpy( buffer, strPtr, len );
    buffer[len] = '\0';
    return len;
}


/***********************************************************************
 *           InitAtomTable16   (KERNEL.68)
 */
WORD WINAPI InitAtomTable16( WORD entries )
{
    if (!entries) entries = DEFAULT_ATOMTABLE_SIZE;  /* sanity check */
    return ATOM_InitTable( CURRENT_DS, entries );
}


/***********************************************************************
 *           GetAtomHandle   (KERNEL.73)
 */
HANDLE16 WINAPI GetAtomHandle16( ATOM atom )
{
    if (atom < MIN_STR_ATOM) return 0;
    return ATOMTOHANDLE( atom );
}


/***********************************************************************
 *           AddAtom16   (KERNEL.70)
 */
ATOM WINAPI AddAtom16( SEGPTR str )
{
    ATOM atom;
    HANDLE16 ds = CURRENT_DS;

    if (!HIWORD(str)) return (ATOM)LOWORD(str);  /* Integer atom */
    if (SELECTOR_TO_ENTRY(LOWORD(str)) == SELECTOR_TO_ENTRY(ds))
    {
        /* If the string is in the same data segment as the atom table, make */
        /* a copy of the string to be sure it doesn't move in linear memory. */
        char buffer[MAX_ATOM_LEN+1];
        lstrcpynA( buffer, (char *)PTR_SEG_TO_LIN(str), sizeof(buffer) );
        atom = ATOM_AddAtom( ds, buffer );
    }
    else atom = ATOM_AddAtom( ds, (LPCSTR)PTR_SEG_TO_LIN(str) );
    return atom;
}


/***********************************************************************
 *           AddAtom32A   (KERNEL32.0)
 * Adds a string to the atom table and returns the atom identifying the
 * string.
 *
 * RETURNS
 *	Atom: Success
 *	0: Failure
 */
ATOM WINAPI AddAtomA(
            LPCSTR str /* [in] Pointer to string to add */
) {
    return GlobalAddAtomA( str );  /* FIXME */
}


/***********************************************************************
 *           AddAtom32W   (KERNEL32.1)
 * See AddAtom32A
 */
ATOM WINAPI AddAtomW( LPCWSTR str )
{
    return GlobalAddAtomW( str );  /* FIXME */
}


/***********************************************************************
 *           DeleteAtom16   (KERNEL.71)
 */
ATOM WINAPI DeleteAtom16( ATOM atom )
{
    return ATOM_DeleteAtom( CURRENT_DS, atom );
}


/***********************************************************************
 *           DeleteAtom32   (KERNEL32.69)
 * Decrements the reference count of a string atom.  If count becomes
 * zero, the string associated with the atom is removed from the table.
 *
 * RETURNS
 *	0: Success
 *	Atom: Failure
 */
ATOM WINAPI DeleteAtom(
            ATOM atom /* [in] Atom to delete */
) {
    return GlobalDeleteAtom( atom );  /* FIXME */
}


/***********************************************************************
 *           FindAtom16   (KERNEL.69)
 */
ATOM WINAPI FindAtom16( SEGPTR str )
{
    if (!HIWORD(str)) return (ATOM)LOWORD(str);  /* Integer atom */
    return ATOM_FindAtom( CURRENT_DS, (LPCSTR)PTR_SEG_TO_LIN(str) );
}


/***********************************************************************
 *           FindAtom32A   (KERNEL32.117)
 * Searches the local atom table for the string and returns the atom
 * associated with that string.
 *
 * RETURNS
 *	Atom: Success
 *	0: Failure
 */
ATOM WINAPI FindAtomA(
            LPCSTR str /* [in] Pointer to string to find */
) {
    return GlobalFindAtomA( str );  /* FIXME */
}


/***********************************************************************
 *           FindAtom32W   (KERNEL32.118)
 * See FindAtom32A
 */
ATOM WINAPI FindAtomW( LPCWSTR str )
{
    return GlobalFindAtomW( str );  /* FIXME */
}


/***********************************************************************
 *           GetAtomName16   (KERNEL.72)
 */
UINT16 WINAPI GetAtomName16( ATOM atom, LPSTR buffer, INT16 count )
{
    return (UINT16)ATOM_GetAtomName( CURRENT_DS, atom, buffer, count );
}


/***********************************************************************
 *           GetAtomName32A   (KERNEL32.149)
 * Retrieves a copy of the string associated with the atom.
 *
 * RETURNS
 *	Length of string: Success
 *	0: Failure
 */
UINT WINAPI GetAtomNameA(
              ATOM atom,    /* [in]  Atom */
              LPSTR buffer, /* [out] Pointer to string for atom string */
              INT count   /* [in]  Size of buffer */
) {
    return GlobalGetAtomNameA( atom, buffer, count );  /* FIXME */
}


/***********************************************************************
 *           GetAtomName32W   (KERNEL32.150)
 * See GetAtomName32A
 */
UINT WINAPI GetAtomNameW( ATOM atom, LPWSTR buffer, INT count )
{
    return GlobalGetAtomNameW( atom, buffer, count );  /* FIXME */
}


/***********************************************************************
 *           GlobalAddAtom16   (USER.268)
 */
ATOM WINAPI GlobalAddAtom16( SEGPTR str )
{
    if (!HIWORD(str)) return (ATOM)LOWORD(str);  /* Integer atom */
#ifdef CONFIG_IPC
    return DDE_GlobalAddAtom( str );
#else
    return ATOM_AddAtom( ATOM_GlobalTable, (LPCSTR)PTR_SEG_TO_LIN(str) );
#endif
}


/***********************************************************************
 *           GlobalAddAtom32A   (KERNEL32.313)
 * Adds a character string to the global atom table and returns a unique
 * value identifying the string.
 *
 * RETURNS
 *	Atom: Success
 *	0: Failure
 */
ATOM WINAPI GlobalAddAtomA(
            LPCSTR str /* [in] Pointer to string to add */
) {
    if (!HIWORD(str)) return (ATOM)LOWORD(str);  /* Integer atom */
    return ATOM_AddAtom( ATOM_GlobalTable, str );
}


/***********************************************************************
 *           GlobalAddAtom32W   (KERNEL32.314)
 * See GlobalAddAtom32A
 */
ATOM WINAPI GlobalAddAtomW( LPCWSTR str )
{
    char buffer[MAX_ATOM_LEN+1];
    if (!HIWORD(str)) return (ATOM)LOWORD(str);  /* Integer atom */
    lstrcpynWtoA( buffer, str, sizeof(buffer) );
    return ATOM_AddAtom( ATOM_GlobalTable, buffer );
}


/***********************************************************************
 *           GlobalDeleteAtom   (USER.269) (KERNEL32.317)
 * Decrements the reference count of a string atom.  If the count is
 * zero, the string associated with the atom is removed from the table.
 *
 * RETURNS
 *	0: Success
 *	Atom: Failure
 */
ATOM WINAPI GlobalDeleteAtom(
            ATOM atom /* [in] Atom to delete */
) {
#ifdef CONFIG_IPC
    return DDE_GlobalDeleteAtom( atom );
#else
    return ATOM_DeleteAtom( ATOM_GlobalTable, atom );
#endif
}


/***********************************************************************
 *           GlobalFindAtom16   (USER.270)
 */
ATOM WINAPI GlobalFindAtom16( SEGPTR str )
{
    if (!HIWORD(str)) return (ATOM)LOWORD(str);  /* Integer atom */
#ifdef CONFIG_IPC
    return DDE_GlobalFindAtom( str );
#else
    return ATOM_FindAtom( ATOM_GlobalTable, (LPCSTR)PTR_SEG_TO_LIN(str) );
#endif
}


/***********************************************************************
 *           GlobalFindAtom32A   (KERNEL32.318)
 * Searches the atom table for the string and returns the atom
 * associated with it.
 *
 * RETURNS
 *	Atom: Success
 *	0: Failure
 */
ATOM WINAPI GlobalFindAtomA(
            LPCSTR str /* [in] Pointer to string to search for */
) {
    if (!HIWORD(str)) return (ATOM)LOWORD(str);  /* Integer atom */
    return ATOM_FindAtom( ATOM_GlobalTable, str );
}


/***********************************************************************
 *           GlobalFindAtom32W   (KERNEL32.319)
 * See GlobalFindAtom32A
 */
ATOM WINAPI GlobalFindAtomW( LPCWSTR str )
{
    char buffer[MAX_ATOM_LEN+1];
    if (!HIWORD(str)) return (ATOM)LOWORD(str);  /* Integer atom */
    lstrcpynWtoA( buffer, str, sizeof(buffer) );
    return ATOM_FindAtom( ATOM_GlobalTable, buffer );
}


/***********************************************************************
 *           GlobalGetAtomName16   (USER.271)
 */
UINT16 WINAPI GlobalGetAtomName16( ATOM atom, LPSTR buffer, INT16 count )
{
#ifdef CONFIG_IPC
    return DDE_GlobalGetAtomName( atom, buffer, count );
#else
    return (UINT16)ATOM_GetAtomName( ATOM_GlobalTable, atom, buffer, count );
#endif
}


/***********************************************************************
 *           GlobalGetAtomName32A   (KERNEL32.323)
 * Retrieves a copy of the string associated with an atom.
 *
 * RETURNS
 *	Length of string in characters: Success
 *	0: Failure
 */
UINT WINAPI GlobalGetAtomNameA(
              ATOM atom,    /* [in]  Atom identifier */
              LPSTR buffer, /* [out] Pointer to buffer for atom string */
              INT count   /* [in]  Size of buffer */
) {
    return ATOM_GetAtomName( ATOM_GlobalTable, atom, buffer, count );
}


/***********************************************************************
 *           GlobalGetAtomName32W   (KERNEL32.324)
 * See GlobalGetAtomName32A
 */
UINT WINAPI GlobalGetAtomNameW( ATOM atom, LPWSTR buffer, INT count )
{
    char tmp[MAX_ATOM_LEN+1];
    ATOM_GetAtomName( ATOM_GlobalTable, atom, tmp, sizeof(tmp) );
    lstrcpynAtoW( buffer, tmp, count );
    return lstrlenW( buffer );
}
