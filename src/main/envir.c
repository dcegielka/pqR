/*
 *  pqR : A pretty quick version of R
 *  Copyright (C) 2013, 2014, 2015, 2016, 2017 by Radford M. Neal
 *
 *  Based on R : A Computer Language for Statistical Data Analysis
 *  Copyright (C) 1995, 1996  Robert Gentleman and Ross Ihaka
 *  Copyright (C) 1999-2012  The R Core Team.
 *
 *  The changes in pqR from R-2.15.0 distributed by the R Core Team are
 *  documented in the NEWS and MODS files in the top-level source directory.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, a copy is available at
 *  http://www.r-project.org/Licenses/
 *
 *
 *
 *  Environments:
 *
 *  All the action of associating values with symbols happens
 *  in this code.  An environment is (essentially) a list of
 *  environment "frames" of the form
 *
 *	FRAME(envir) = environment frame
 *	ENCLOS(envir) = parent environment
 *	HASHTAB(envir) = (optional) hash table
 *
 *  Each frame is a (tagged) list with
 *
 *	TAG(item) = symbol
 *	CAR(item) = value bound to symbol in this frame
 *	CDR(item) = next value on the list
 *
 *  When the value of a symbol is required, the environment is
 *  traversed frame-by-frame until a value is found.
 *
 *  If a value is not found during the traversal, the symbol's
 *  "value" slot is inspected for a value.  This "top-level"
 *  environment is where system functions and variables reside.
 *
 *  Environments with the NO_SPEC_SYM flag are known to not contain any
 *  special symbols, as indicated by the SPEC_SYM macro.  Lookup for
 *  such a symbol can then bypass  this environment without searching it.
 */

/* R 1.8.0: namespaces are no longer experimental, so the following
 *  are no longer 'experimental options':
 *
 * EXPERIMENTAL_NAMESPACES: When this is defined the variable
 *     R_BaseNamespace holds an environment that has R_GlobalEnv as
 *     its parent.  This environment does not actually contain any
 *     bindings of its own.  Instead, it redirects all fetches and
 *     assignments to the SYMVALUE fields of the base (R_BaseEnv)
 *     environment.  If evaluation occurs in R_BaseNamespace, then
 *     base is searched before R_GlobalEnv.
 *
 * ENVIRONMENT_LOCKING: Locking an environment prevents new bindings
 *     from being created and existing bindings from being removed.
 *
 * FANCY_BINDINGS: This enables binding locking and "active bindings".
 *     When a binding is locked, its value cannot be changed.  It may
 *     still be removed from the environment if the environment is not
 *     locked.
 *
 *     Active bindings contain a function in their value cell.
 *     Getting the value of an active binding calls this function with
 *     no arguments and returns the result.  Assigning to an active
 *     binding calls this function with one argument, the new value.
 *     Active bindings may be useful for mapping external variables,
 *     such as C variables or data base entries, to R variables.  They
 *     may also be useful for making some globals thread-safe.
 *
 *     Bindings are marked as locked or active using bits 14 and 15 in
 *     their gp fields.  Since the save/load code writes out this
 *     field it means the value will be preserved across save/load.
 *     But older versions of R will interpret the entire gp field as
 *     the MISSING field, which may cause confusion.  If we keep this
 *     code, then we will need to make sure that there are no
 *     locked/active bindings in workspaces written for older versions
 *     of R to read.
 *
 * LT */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#define USE_FAST_PROTECT_MACROS
#define R_USE_SIGNALS 1
#define NEED_SGGC_FUNCTIONS
#include "Defn.h"
#include <R_ext/Callbacks.h>

#include <helpers/helpers-app.h>

#define DEBUG_OUTPUT 0          /* 0 to 2 for increasing debug output */
#define DEBUG_CHECK 0           /* 1 to enable debug check of HASHSLOTSUSED */

/* various definitions of macros/functions in Defn.h */

#define FRAME_LOCK_MASK (1<<14)
#define FRAME_IS_LOCKED(e) (ENVFLAGS(e) & FRAME_LOCK_MASK)
#define LOCK_FRAME(e) SET_ENVFLAGS(e, ENVFLAGS(e) | FRAME_LOCK_MASK)
/*#define UNLOCK_FRAME(e) SET_ENVFLAGS(e, ENVFLAGS(e) & (~ FRAME_LOCK_MASK))*/

/* use the same bits (15 and 14) in symbols and bindings */
#define BINDING_VALUE(b) ((IS_ACTIVE_BINDING(b) ? getActiveValue(CAR(b)) : CAR(b)))

#define SYMBOL_BINDING_VALUE(s) ((IS_ACTIVE_BINDING(s) ? getActiveValue(SYMVALUE(s)) : SYMVALUE(s)))
#define SYMBOL_HAS_BINDING(s) (IS_ACTIVE_BINDING(s) || (SYMVALUE(s) != R_UnboundValue))

#define SET_BINDING_VALUE(b,val) do { \
  SEXP __b__ = (b); \
  SEXP __val__ = (val); \
  if (BINDING_IS_LOCKED(__b__)) \
    error(_("cannot change value of locked binding for '%s'"), \
	  CHAR(PRINTNAME(TAG(__b__)))); \
  if (IS_ACTIVE_BINDING(__b__)) \
    setActiveValue(CAR(__b__), __val__); \
  else \
    SETCAR(__b__, __val__); \
} while (0)

#define SET_SYMBOL_BINDING_VALUE(sym, val) do { \
  SEXP __sym__ = (sym); \
  SEXP __val__ = (val); \
  if (BINDING_IS_LOCKED(__sym__)) \
    error(_("cannot change value of locked binding for '%s'"), \
	  CHAR(PRINTNAME(__sym__))); \
  if (IS_ACTIVE_BINDING(__sym__)) \
    setActiveValue(SYMVALUE(__sym__), __val__); \
  else \
    SET_SYMVALUE(__sym__, __val__); \
} while (0)

static void setActiveValue(SEXP fun, SEXP val)
{
    SEXP arg = LCONS(R_QuoteSymbol, CONS(val, R_NilValue));
    SEXP expr = LCONS(fun, CONS(arg, R_NilValue));
    WAIT_UNTIL_COMPUTED(val); \
    PROTECT(expr);
    eval(expr, R_GlobalEnv);
    UNPROTECT(1);
}

static SEXP getActiveValue(SEXP fun)
{
    SEXP expr = LCONS(fun, R_NilValue);
    PROTECT(expr);
    expr = eval(expr, R_GlobalEnv);
    UNPROTECT(1);
    return expr;
}

/* Macro to produce an unrolled loop to search for a symbol in a chain.
   This code takes advantage of the CAR, CDR and TAG of R_NilValue being
   R_NilValue to avoid a check for R_NilValue in unrolled part.  The
   arguments are the pointer to the start of the chain (which is modified
   to point to the binding cell found), the symbol to search for, and
   the statement to do if the symbol is found, which must have the effect
   of exitting the loop (ie, be a "break", "return", or "goto" statement).
   If the symbol is not found, execution continues after this macro, with
   the chain pointer being R_NilValue. 

   The optimal amount of unrolling may depend on whether compressed or
   uncompressed pointers are used, so these cases are distinguished. */

#if USE_COMPRESSED_POINTERS
   
#define SEARCH_LOOP(chain,symbol,statement) \
    do { \
        if (TAG(chain) == symbol) statement; \
        chain = CDR(chain); \
    } while (chain != R_NilValue)

#else
   
#define SEARCH_LOOP(chain,symbol,statement) \
    do { \
        if (TAG(chain) == symbol) statement; \
        chain = CDR(chain); \
        if (TAG(chain) == symbol) statement; \
        chain = CDR(chain); \
        if (TAG(chain) == symbol) statement; \
        chain = CDR(chain); \
    } while (chain != R_NilValue)

#endif

/* Function to correctly set NO_SPEC_SYM flag for an (unhashed) environment. */

void setNoSpecSymFlag (SEXP env)
{
    SEXP frame;
   
    if (HASHTAB(env)!=R_NilValue) {
        SET_NO_SPEC_SYM (env, 0); 
        return;
    }

    /* Unrolled loop, which relies on CAR, CDR, and TAG of R_NilValue 
       being R_NilValue.  Also relies on SPEC_SYM(R_NilValue) being 0. */

    frame = FRAME(env);
    do {
        if (SPEC_SYM(TAG(frame))) goto special;
        frame = CDR(frame);
        if (SPEC_SYM(TAG(frame))) goto special;
        frame = CDR(frame);
    } while (frame != R_NilValue);

    SET_NO_SPEC_SYM (env, 1);
    return;

  special:
    SET_NO_SPEC_SYM (env, 0);
    return;
}

/*----------------------------------------------------------------------

  Hash Tables

  We use a basic separate chaining algorithm.	A hash table consists
  of SEXP (vector) which contains a number of SEXPs (lists).

  The only non-static function is R_NewHashedEnv, which allows code to
  request a hashed environment.  All others are static to allow
  internal changes of implementation without affecting client code.
*/


/*----------------------------------------------------------------------

  R_HashSet

  Hashtable set function.  Sets 'symbol' in 'table' to be 'value'.
  'hashcode' must be provided by user.	Allocates some memory for list
  entries.

*/

static void R_HashSet(int hashcode, SEXP symbol, SEXP table, SEXP value,
                      Rboolean frame_locked)
{
    SEXP chain = VECTOR_ELT(table, hashcode);

    SEXP loc, new_chain;

    loc = chain;
    SEARCH_LOOP (loc, symbol, goto found);

    if (frame_locked)
        error(_("cannot add bindings to a locked environment"));

    /* Add the value into the chain */

    if (chain == R_NilValue) SET_HASHSLOTSUSED(table, HASHSLOTSUSED(table) + 1);
    new_chain = cons_with_tag (value, chain, symbol);
    SET_VECTOR_ELT (table, hashcode, new_chain);
    return;

  found:
    SET_BINDING_VALUE(loc, value);
    SET_MISSING(loc, 0);        /* Over-ride for new value */
    return;
}



/*----------------------------------------------------------------------

  R_HashGet

  Hashtable get function.  Returns 'value' from 'table' indexed by
  'symbol'.  'hashcode' must be provided by user.  Returns
  'R_UnboundValue' if value is not present.

*/

static SEXP R_HashGet(int hashcode, SEXP symbol, SEXP table)
{
    SEXP chain = VECTOR_ELT(table, hashcode);

    SEARCH_LOOP (chain, symbol, goto found);

    return R_UnboundValue;

found:
    return BINDING_VALUE(chain);
}

static Rboolean R_HashExists(int hashcode, SEXP symbol, SEXP table)
{
    SEXP chain = VECTOR_ELT(table, hashcode);

    SEARCH_LOOP (chain, symbol, goto found);

    return FALSE;

found:
    return TRUE;
}



/*----------------------------------------------------------------------

  R_HashGetLoc

  Hashtable get location function. Just like R_HashGet, but returns
  location of variable, rather than its value. Returns R_NilValue
  if not found.

*/

static SEXP R_HashGetLoc(int hashcode, SEXP symbol, SEXP table)
{
    SEXP chain = VECTOR_ELT(table, hashcode);

    SEARCH_LOOP (chain, symbol, return chain);

    return R_NilValue;
}



/*----------------------------------------------------------------------

  R_NewHashTable

  Hash table initialisation function.  Creates a table of size 'size'. 
*/

static SEXP R_NewHashTable(int size)
{
    SEXP table;

    if (size <= 0) size = HASHMINSIZE;

    /* Allocate hash table in the form of a vector */
    PROTECT(table = allocVector(VECSXP, size));
    SET_HASHSLOTSUSED(table, 0);
    UNPROTECT(1);
    return(table);
}

/*----------------------------------------------------------------------

  R_NewHashedEnv

  Returns a new environment with a hash table initialized with default
  size.  The only non-static hash table function.
*/

SEXP R_NewHashedEnv(SEXP enclos, SEXP size)
{
    SEXP s;

    PROTECT(enclos);
    PROTECT(size);
    PROTECT(s = NewEnvironment(R_NilValue, R_NilValue, enclos));
    SET_HASHTAB(s, R_NewHashTable(asInteger(size)));
    UNPROTECT(3);
    return s;
}


/*----------------------------------------------------------------------

  R_HashResize

  Hash table resizing function. Increase the size of the hash table by
  HASHTABLEGROWTHRATE. The vector is reallocated, but the
  lists within the hash table have their pointers shuffled around
  so that they are not reallocated.
*/

static SEXP R_HashResize(SEXP table)
{
    SEXP new_table, chain, new_chain, tmp_chain;
    int new_size, counter, new_hashcode;
#if DEBUG_OUTPUT
    int n_entries = 0;
#endif

    /* Do some checking */
    if (TYPEOF(table) != VECSXP)
	error("argument not of type VECSXP, from R_HashResize");

    /* Allocate the new hash table.  Return old table if not worth resizing
       because near maximum allowed. */
    new_size = (int) (HASHSIZE(table) * HASHTABLEGROWTHRATE);
    if (new_size > HASHMAXSIZE) {
        new_size = HASHMAXSIZE;
        if (new_size <= 1.05 * HASHSIZE(table))
            return table;
    }
    new_table = R_NewHashTable (new_size);

    /* Move entries into new table. */
    for (counter = 0; counter < LENGTH(table); counter++) {
	chain = VECTOR_ELT(table, counter);
	while (chain != R_NilValue) {
#if DEBUG_OUTPUT
            n_entries += 1;
#endif
            new_hashcode = SYM_HASH(TAG(chain)) % HASHSIZE(new_table);
	    new_chain = VECTOR_ELT(new_table, new_hashcode);
	    /* If using a previously-unused slot then increase HASHSLOTSUSED */
	    if (new_chain == R_NilValue)
		SET_HASHSLOTSUSED(new_table, HASHSLOTSUSED(new_table) + 1);
	    tmp_chain = chain;
	    chain = CDR(chain);
	    SETCDR(tmp_chain, new_chain);
	    SET_VECTOR_ELT(new_table, new_hashcode,  tmp_chain);
#if DEBUG_OUTPUT>1
	    Rprintf(
             "HASHSIZE=%d HASHSLOTSUSED=%d counter=%d HASHCODE=%d\n",
              HASHSIZE(table), HASHSLOTSUSED(table), counter, new_hashcode);
#endif
	}
    }

    /* Some debugging statements */
#if DEBUG_OUTPUT
    Rprintf("RESIZED TABLE WITH %d ENTRIES O.K.\n",n_entries);
    Rprintf("Old size: %d, New size: %d\n",HASHSIZE(table),HASHSIZE(new_table));
    Rprintf("Old slotsused: %d, New slotsused: %d\n",
	    HASHSLOTSUSED(table), HASHSLOTSUSED(new_table));
#endif

    return new_table;
} /* end R_HashResize */



/*----------------------------------------------------------------------

  R_HashSizeCheck

  Hash table size rechecking function.	Looks at the fraction of table
  entries that have one or more symbols, comparing to a threshold value
  (which should be in the interval (0,1)).  Returns true if the table 
  needs to be resized.  Does NOT check whether resizing shouldn't be 
  done because HASHMAXSIZE would then be exceeded.
*/

static R_INLINE int R_HashSizeCheck(SEXP table)
{
    /* Do some checking */
    if (TYPEOF(table) != VECSXP)
	error("argument not of type VECSXP, R_HashSizeCheck");

#if DEBUG_CHECK
    int slotsused = 0;
    int i;
    for (i = 0; i<LENGTH(table); i++) {
        if (VECTOR_ELT(table,i) != R_NilValue) {
            if (TYPEOF(VECTOR_ELT(table,i)) != LISTSXP) abort();
            slotsused += 1;
        }
    }
    if (HASHSLOTSUSED(table) != slotsused) {
        REprintf("WRONG SLOTSUSED IN HASH TABLE! %d %d\n",
                HASHSLOTSUSED(table), slotsused);
        abort();
    }
#endif

    return HASHSLOTSUSED(table) > 0.5 * HASHSIZE(table);
}



/*----------------------------------------------------------------------

  R_HashFrame

  Hashing for environment frames.  This function ensures that the
  frame in the given environment has been hashed.	 
*/

static void R_HashFrame(SEXP rho)
{
    int hashcode;
    SEXP frame, chain, tmp_chain, table;

    /* Do some checking */
    if (TYPEOF(rho) != ENVSXP)
	error("argument not of type ENVSXP, from R_Hashframe");
    table = HASHTAB(rho);
    frame = FRAME(rho);
    while (frame != R_NilValue) {
	hashcode = SYM_HASH(TAG(frame)) % HASHSIZE(table);
	chain = VECTOR_ELT(table, hashcode);
	/* If using a previously-unused slot then increase HASHSLOTSUSED */
	if (chain == R_NilValue) 
            SET_HASHSLOTSUSED(table, HASHSLOTSUSED(table) + 1);
	tmp_chain = frame;
	frame = CDR(frame);
	SETCDR(tmp_chain, chain);
	SET_VECTOR_ELT(table, hashcode, tmp_chain);
    }
    SET_FRAME(rho, R_NilValue);
    SET_NO_SPEC_SYM(rho, 0);
}


/* ---------------------------------------------------------------------

   R_HashProfile

   Profiling tool for analyzing hash table performance.  Returns a
   three element list with components:

   size: the total size of the hash table

   nchains: the number of non-null chains in the table (as reported by
	    HASHSLOTSUSED())

   counts: an integer vector the same length as size giving the length of
	   each chain (or zero if no chain is present).  This allows
	   for assessing collisions in the hash table.
 */

static SEXP R_HashProfile(SEXP table)
{
    SEXP chain, ans, chain_counts, nms;
    int i, count;

    /* Do some checking */
    if (TYPEOF(table) != VECSXP)
	error("argument not of type VECSXP, from R_HashProfile");

    int len_table = LENGTH(table);

    PROTECT(ans = allocVector(VECSXP, 3));
    PROTECT(nms = allocVector(STRSXP, 3));
    SET_STRING_ELT(nms, 0, mkChar("size"));    /* size of hashtable */
    SET_STRING_ELT(nms, 1, mkChar("nchains")); /* number of non-null chains */
    SET_STRING_ELT(nms, 2, mkChar("counts"));  /* length of each chain */
    setAttrib(ans, R_NamesSymbol, nms);
    UNPROTECT(1);

    SET_VECTOR_ELT(ans, 0, ScalarInteger(len_table));
    SET_VECTOR_ELT(ans, 1, ScalarInteger(HASHSLOTSUSED(table)));

    PROTECT(chain_counts = allocVector(INTSXP, len_table));
    for (i = 0; i < len_table; i++) {
	chain = VECTOR_ELT(table, i);
	count = 0;
	for (; chain != R_NilValue ; chain = CDR(chain)) {
	    count++;
	}
	INTEGER(chain_counts)[i] = count;
    }

    SET_VECTOR_ELT(ans, 2, chain_counts);

    UNPROTECT(2);
    return ans;
}



/*----------------------------------------------------------------------

  Environments

  The following code implements variable searching for environments. 

  ----------------------------------------------------------------------*/


/* Global variable caching.  A cache is maintained in the symvalue and
   attribute fields of symbols.  BASE_CACHE flags a symbol whose
   cached value is in symvalue.  Otherwise, the cached binding cell
   is in the attribute field, R_NilValue if the symbol is not in the
   global cache.

   To make sure the cache is valid, all binding creations and removals
   from global frames must go through the interface functions in this
   file.

   Initially only the R_GlobalEnv frame is a global frame.  Additional
   global frames can only be created by attach.  All other frames are
   considered local.  Whether a frame is local or not is recorded in
   the highest order bit of the ENVFLAGS field (the gp field of
   sxpinfo). */

#define GLOBAL_FRAME_MASK (1<<15)
#define IS_GLOBAL_FRAME(e) (ENVFLAGS(e) & GLOBAL_FRAME_MASK)
#define MARK_AS_GLOBAL_FRAME(e) \
  SET_ENVFLAGS(e, ENVFLAGS(e) | GLOBAL_FRAME_MASK)
#define MARK_AS_LOCAL_FRAME(e) \
  SET_ENVFLAGS(e, ENVFLAGS(e) & (~ GLOBAL_FRAME_MASK))

static SEXP R_BaseNamespaceName;

void attribute_hidden InitBaseEnv()
{
    R_BaseEnv = NewEnvironment(R_NilValue, R_NilValue, R_EmptyEnv);
    UPTR_FROM_SEXP(R_BaseEnv)->sxpinfo.trace_base = 1;
}

void attribute_hidden InitGlobalEnv()
{
    R_GlobalEnv = R_NewHashedEnv(R_BaseEnv, ScalarIntegerMaybeConst(0));

    MARK_AS_GLOBAL_FRAME(R_GlobalEnv);

    R_BaseNamespace = NewEnvironment(R_NilValue, R_NilValue, R_GlobalEnv);
    UPTR_FROM_SEXP(R_BaseNamespace)->sxpinfo.trace_base = 1;
    R_PreserveObject(R_BaseNamespace);
    SET_SYMVALUE(install(".BaseNamespaceEnv"), R_BaseNamespace);
    R_BaseNamespaceName = ScalarString(mkChar("base"));
    R_PreserveObject(R_BaseNamespaceName);
    R_NamespaceRegistry = R_NewHashedEnv(R_NilValue, ScalarIntegerMaybeConst(0));
    R_PreserveObject(R_NamespaceRegistry);
    defineVar(install("base"), R_BaseNamespace, R_NamespaceRegistry);

    /**** needed to properly initialize the base namespace */
}

static inline void R_FlushGlobalCache(SEXP sym)
{
    ATTRIB_W(sym) = R_NilValue;
    SET_BASE_CACHE(sym,0);
}

static void R_FlushGlobalCacheFromTable(SEXP table)
{
    int i, size;
    SEXP chain;
    size = HASHSIZE(table);
    for (i = 0; i < size; i++) {
	for (chain = VECTOR_ELT(table, i); chain != R_NilValue; chain = CDR(chain))
	    R_FlushGlobalCache(TAG(chain));
    }
}

/**
 Flush the cache based on the names provided by the user defined
 table, specifically returned from calling objects() for that
 table.
 */
static void R_FlushGlobalCacheFromUserTable(SEXP udb)
{
    int n, i;
    R_ObjectTable *tb;
    SEXP names;
    tb = (R_ObjectTable*) R_ExternalPtrAddr(udb);
    names = tb->objects(tb);
    n = length(names);
    for(i = 0; i < n ; i++)
	R_FlushGlobalCache(Rf_installChar(STRING_ELT(names,i)));
}

static inline void R_AddGlobalCacheBase(SEXP symbol)
{
    ATTRIB_W(symbol) = R_NilValue;
    SET_BASE_CACHE (symbol, !IS_ACTIVE_BINDING(symbol));
}

static inline void R_AddGlobalCacheNonBase(SEXP symbol, SEXP place)
{
    ATTRIB_W(symbol) = place;
    SET_BASE_CACHE (symbol, 0);
}

static inline void R_AddGlobalCacheNotFound(SEXP symbol)
{
    ATTRIB_W(symbol) = R_UnboundValue;
    SET_BASE_CACHE (symbol, 0);
}

static SEXP R_GetGlobalCache(SEXP symbol)
{
    SEXP val;

    if (BASE_CACHE(symbol)) {
        val = SYMVALUE(symbol);
        return val;
    }

    if (ATTRIB_W(symbol) == R_UnboundValue)
        return R_UnboundValue;

    if (ATTRIB_W(symbol) == R_NilValue)
        return R_NoObject;

    val = BINDING_VALUE(ATTRIB_W(symbol));

    if (val == R_UnboundValue) {
        ATTRIB_W(symbol) = R_NilValue;
        return R_NoObject;
    }

    return val;
}


/* Remove variable from a list, return the new list, and return its old value 
   (R_NoObject if not there) in 'value'. */

static SEXP RemoveFromList(SEXP thing, SEXP list, SEXP *value)
{
    SEXP last = R_NilValue;
    SEXP curr = list;

    while (curr != R_NilValue) {

        if (TAG(curr) == thing) {
            *value = CAR(curr);
            SETCAR(curr, R_UnboundValue); /* in case binding is cached */
            LOCK_BINDING(curr);           /* in case binding is cached */
            if (last==R_NilValue)
                list = CDR(curr);
            else
                SETCDR(last, CDR(curr));
            return list;
        }

        last = curr;
        curr = CDR(curr);
    }

    *value = R_NoObject;
    return list;
}


/*----------------------------------------------------------------------

  findVarLocInFrame

  Look up the location of the value of a symbol in a single
  environment frame.  Returns the binding cell rather than the value
  itself, or R_NilValue if not found.  Does not wait for the bound
  value to be computed.

  Callers set *canCache = TRUE or NULL
*/

static SEXP findVarLocInFrame(SEXP rho, SEXP symbol, Rboolean *canCache)
{
    SEXP loc;

    if (SEXP32_FROM_SEXP(rho) == LASTSYMENV(symbol)) {
         loc = LASTSYMBINDING(symbol);
         if (BINDING_VALUE(loc) == R_UnboundValue)
             LASTSYMENV(symbol) = R_NoObject32;
         else
             return loc;
    }

    if (IS_USER_DATABASE(rho)) {
	R_ObjectTable *table = (R_ObjectTable *)R_ExternalPtrAddr(HASHTAB(rho));
	SEXP val = table->get(CHAR(PRINTNAME(symbol)), canCache, table);
	/* Better to use exists() here if we don't actually need the value? */
	if (val == R_UnboundValue)
            loc = R_NilValue;
        else {
	    /* The result should probably be identified as being from
	       a user database, or maybe use an active binding
	       mechanism to allow setting a new value to get back to
	       the data base. */
            loc = cons_with_tag (val, R_NilValue, symbol);
	    /* If the database has a canCache method, then call that.
	       Otherwise, we believe the setting for canCache. */
	    if(canCache && table->canCache)
		*canCache = table->canCache(CHAR(PRINTNAME(symbol)), table);
	}
    }

    else if (IS_BASE(rho))
	error("'findVarLocInFrame' cannot be used on the base environment");

    else if (!isEnvironment(rho))  /* somebody does this... */
	return R_NilValue;

    else if (HASHTAB(rho) == R_NilValue) {

        loc = FRAME(rho);
        SEARCH_LOOP (loc, symbol, goto found);

        return R_NilValue;

      found:
        if ( ! IS_ACTIVE_BINDING(loc) && !DDVAL(symbol)) {
            /* Note:  R_MakeActiveBinding won't let an existing binding 
               become active, so we later assume this can't be active. */
            LASTSYMENV(symbol) = SEXP32_FROM_SEXP(rho);
            LASTSYMBINDING(symbol) = loc;
        }
    }
    else {
        int hashcode;
	hashcode = SYM_HASH(symbol) % HASHSIZE(HASHTAB(rho));
	/* Will return 'R_NilValue' if not found */
	loc = R_HashGetLoc(hashcode, symbol, HASHTAB(rho));
    }

    return loc;
}


/*
  External version and accessor functions. Returned value is cast as
  an opaque pointer to insure it is only used by routines in this
  group.  This allows the implementation to be changed without needing
  to change other files.
*/

R_varloc_t R_findVarLocInFrame(SEXP rho, SEXP symbol)
{
    SEXP binding = findVarLocInFrame(rho, symbol, NULL);
    return binding == R_NilValue ? R_NoObject : (R_varloc_t) binding;
}

SEXP R_GetVarLocValue(R_varloc_t vl)
{
    SEXP value = BINDING_VALUE((SEXP) vl);
    WAIT_UNTIL_COMPUTED(value);
    return value;
}

SEXP R_GetVarLocSymbol(R_varloc_t vl)
{
    return TAG((SEXP) vl);
}

Rboolean R_GetVarLocMISSING(R_varloc_t vl)
{
    return MISSING((SEXP) vl);
}

void R_SetVarLocValue(R_varloc_t vl, SEXP value)
{
    SET_BINDING_VALUE((SEXP) vl, value);
    /* Should this also do a SET_MISSING((SEXP)vl,0)? */
}


/*----------------------------------------------------------------------

  findVarInFrame3

  Look up the value of a symbol in a single environment frame.	This
  is the basic building block of all variable lookups.

  It is important that this be as efficient as possible.

  The final argument controls the exact behaviour, as follows:

    0 (or FALSE)  Lookup and return value, or R_UnboundValue if doesn't exist.
                  On a user database, does a "get" only if "exists" is true.
                  Waits for computation of the value to finish.

    1 (or TRUE)   Same as 0, except always does a "get" on a user database.

    2             Only checks existence, returning R_UnboundValue if a 
                  binding doesn't exist and R_NilValue if one does exist.
                  Doesn't wait for computation of the value to finish, since 
                  it isn't being returned.

    3             Same as 1, except doesn't wait for computation of the value 
                  to finish.

    7             Like 3, except that it returns R_UnboundValue if the 
                  binding found is active, or locked, or from a user database.

  Note that (option&1)!=0 if a get should aways be done on a user database,
  and (option&2)!=0 if we don't need to wait.

  The findVarInFramePendingOK version is an abbreviation for option 3.
  It may not only be more convenient, but also be faster.

  Sets R_binding_cell to the CONS cell for the binding, if the value returned
  is not R_UnboundValue, and there is a cell (not one for base environment),
  and the cell is suitable for updating by the caller (is not for an active 
  binding or locked).  Otherwise, R_binding_cell is set to R_NilValue.
*/

SEXP findVarInFramePendingOK(SEXP rho, SEXP symbol)
{
    SEXP value;

    if (TYPEOF(symbol) != SYMSXP) abort();

    if (SEXP32_FROM_SEXP(rho) == LASTSYMENV(symbol)) {
        SEXP binding = LASTSYMBINDING(symbol); /* won't be an active binding */
        if ( ! BINDING_IS_LOCKED(binding)) {
            value = CAR(binding);
            if (value == R_UnboundValue)
                LASTSYMENV(symbol) = R_NoObject32;
            else {
                R_binding_cell = binding;
                return value;
            }
        }
    }

    return findVarInFrame3_nolast (rho, symbol, 3);
}

SEXP findVarInFrame3(SEXP rho, SEXP symbol, int option)
{
    SEXP value;

    if (TYPEOF(symbol) != SYMSXP) abort();

    if (SEXP32_FROM_SEXP(rho) == LASTSYMENV(symbol)) {
        SEXP binding = LASTSYMBINDING(symbol); /* won't be an active binding */
        if ( ! BINDING_IS_LOCKED(binding)) {
            value = CAR(binding);
            if (value == R_UnboundValue)
                LASTSYMENV(symbol) = R_NoObject32;
            else {
                switch (option) {
                case 0:
                case 1:
                    WAIT_UNTIL_COMPUTED(value);
                    break;
                case 2:
                    value = R_NilValue;
                    break;
                default:
                    break;
                }
                R_binding_cell = binding;
                return value;
            }
        }
    }

    return findVarInFrame3_nolast (rho, symbol, option);
}

/* Version that doesn't check LASTSYMBINDING.  Split from above so the
   simplest case will have low overhead.  Can also be called directly
   if it's known that checking LASTSYMBINDING won't help. 

   Will fail quickly when the symbol has LASTSYMENVNOTFOUND equal to 
   the environment.  LASTSYMENVNOTFOUND is also updated on failure if the 
   environment is unhashed. */

SEXP findVarInFrame3_nolast(SEXP rho, SEXP symbol, int option)
{
    SEXP loc, value, bcell;

    bcell = R_NilValue;
    value = R_UnboundValue;

    if (IS_BASE(rho)) {
        if (option==2) 
            value = SYMBOL_HAS_BINDING(symbol) ? R_NilValue : R_UnboundValue;
        else if (option==7 && IS_ACTIVE_BINDING(symbol))
            value = R_UnboundValue;
        else 
            value = SYMBOL_BINDING_VALUE(symbol);
    }

    else if (IS_USER_DATABASE(rho)) {
        if (option==7)
            value = R_UnboundValue;
        else {
            /* Use the objects function pointer for this symbol. */
            R_ObjectTable *table = 
              (R_ObjectTable *) R_ExternalPtrAddr(HASHTAB(rho));
            value = R_UnboundValue;
            if (table->active) {
                if (option&1)
                    value = table->get (CHAR(PRINTNAME(symbol)), NULL, table);
                else {
                    if (table->exists (CHAR(PRINTNAME(symbol)), NULL, table))
                        value = option==2 ? R_NilValue
                          : table->get (CHAR(PRINTNAME(symbol)), NULL, table);
                    else
                        value = R_UnboundValue;
                }
            }
        }
    }

    else if (!isEnvironment(rho))
        error(_("argument to '%s' is not an environment"), "findVarInFrame3");

    else if (HASHTAB(rho) == R_NilValue) {

        if (LASTSYMENVNOTFOUND(symbol) != SEXP32_FROM_SEXP(rho)) {
            loc = FRAME(rho);
            SEARCH_LOOP (loc, symbol, goto found);
            LASTSYMENVNOTFOUND(symbol) = SEXP32_FROM_SEXP(rho);
        }

        goto ret;

      found: 
        if (IS_ACTIVE_BINDING(loc)) {
            if (option==7) 
                goto ret;
        }
        else {
            if (!DDVAL(symbol)) {
                /* Note:  R_MakeActiveBinding won't let an existing binding 
                   become active, so we later assume this can't be active. */
                LASTSYMENV(symbol) = SEXP32_FROM_SEXP(rho);
                LASTSYMBINDING(symbol) = loc;
            }
            if (BINDING_IS_LOCKED(loc)) {
                if (option==7)
                    goto ret;
            }
            else
                bcell = loc;
        }
        value = option==2 ? R_NilValue : BINDING_VALUE(loc);
    }

    else {
        int hashcode;
        hashcode = SYM_HASH(symbol) % HASHSIZE(HASHTAB(rho));
        loc = R_HashGetLoc(hashcode, symbol, HASHTAB(rho));
        if (loc == R_NilValue)
            goto ret;
        if (IS_ACTIVE_BINDING(loc) || BINDING_IS_LOCKED(loc)) {
            if (option==7)
                goto ret;
        }
        else
            bcell = loc;
        value = option == 2 ? R_NilValue : BINDING_VALUE(loc);
    }

  ret:
    R_binding_cell = bcell;

    if ((option&2) == 0)
        WAIT_UNTIL_COMPUTED(value);

    return value;
}

SEXP findVarInFrame(SEXP rho, SEXP symbol)
{
    return findVarInFrame3(rho, symbol, TRUE);
}


/* findGlobalVar searches for a symbol value starting at R_GlobalEnv, so the
   cache can be used.  Doesn't wait for the value found to be computed.
   Always set R_binding_cell to R_NilValue - fast updates here aren't needed. */

static inline SEXP findGlobalVar(SEXP symbol)
{
    SEXP vl, rho;

    R_binding_cell = R_NilValue;

    vl = R_GetGlobalCache(symbol);
    if (vl != R_NoObject)
	return vl;

    Rboolean canCache = TRUE;

    for (rho = R_GlobalEnv; rho != R_EmptyEnv; rho = ENCLOS(rho)) {
	if (IS_BASE(rho)) {
	    vl = SYMBOL_BINDING_VALUE(symbol);
	    if (vl != R_UnboundValue) {
		R_AddGlobalCacheBase(symbol);
                return vl;
            }
        }
        else {
	    vl = findVarLocInFrame(rho, symbol, &canCache);
	    if (vl != R_NilValue) {
		if(canCache)
		    R_AddGlobalCacheNonBase(symbol, vl);
		return BINDING_VALUE(vl);
	    }
	}

    }

    R_AddGlobalCacheNotFound(symbol);
    return R_UnboundValue;
}


/*----------------------------------------------------------------------

  findVar

     Look up a symbol in an environment.  Waits for the value to be computed.

  findVarPendingOK 

     Like findVar, but doesn't wait for the value to be computed.

  These set R_binding_cell as for findVarInFrame3.
*/

SEXP findVar(SEXP symbol, SEXP rho)
{
    SEXP value;

    /* This first loop handles local frames, if there are any.  It
       will also handle all frames if rho is a global frame other than
       R_GlobalEnv */

    while (rho != R_GlobalEnv && rho != R_EmptyEnv) {
	value = findVarInFrame3 (rho, symbol, 1);
	if (value != R_UnboundValue) 
            return value;
	rho = ENCLOS(rho);
    }

    if (rho == R_GlobalEnv) {
	value = findGlobalVar(symbol);
        WAIT_UNTIL_COMPUTED(value);
        return value;
    }
    else {
        R_binding_cell = R_NilValue;
        return R_UnboundValue;
    }
}

SEXP findVarPendingOK(SEXP symbol, SEXP rho)
{
    SEXP value;

    /* This first loop handles local frames, if there are any.  It
       will also handle all frames if rho is a global frame other than
       R_GlobalEnv */

    while (rho != R_GlobalEnv && rho != R_EmptyEnv) {
	value = findVarInFramePendingOK (rho, symbol);
	if (value != R_UnboundValue) 
            return value;
	rho = ENCLOS(rho);
    }

    if (rho == R_GlobalEnv) {
	value = findGlobalVar(symbol);
        return value;
    }
    else {
        R_binding_cell = R_NilValue;
        return R_UnboundValue;
    }
}


/*----------------------------------------------------------------------

  findVar1

  Look up a symbol in an environment.  Ignore any values which are
  not of the specified type.

*/

SEXP attribute_hidden
findVar1(SEXP symbol, SEXP rho, SEXPTYPE mode, int inherits)
{
    SEXP vl;
    while (rho != R_EmptyEnv) {
	vl = findVarInFrame3(rho, symbol, TRUE);
	if (vl != R_UnboundValue) {
	    if (mode == ANYSXP) return vl;
	    if (TYPEOF(vl) == PROMSXP)
                vl = forcePromise(vl);
	    if (TYPEOF(vl) == mode) return vl;
	    if (mode == FUNSXP && isFunction(vl)) return (vl);
	}
	if (inherits)
	    rho = ENCLOS(rho);
	else
	    return (R_UnboundValue);
    }
    return (R_UnboundValue);
}

/*
 *  ditto, but check *mode* not *type*
 */

static SEXP
findVar1mode(SEXP symbol, SEXP rho, SEXPTYPE mode, int inherits,
	     Rboolean doGet)
{
    SEXP vl;
    int tl;
    if (mode == INTSXP) mode = REALSXP;
    if (mode == FUNSXP || mode ==  BUILTINSXP || mode == SPECIALSXP)
	mode = CLOSXP;
    while (rho != R_EmptyEnv) {
	if (! doGet && mode == ANYSXP)
	    vl = findVarInFrame3(rho, symbol, 2);
	else
	    vl = findVarInFrame3(rho, symbol, doGet);

	if (vl != R_UnboundValue) {
	    if (mode == ANYSXP) return vl;
	    if (TYPEOF(vl) == PROMSXP)
                vl = forcePromise(vl);
	    if (mode == CLOSXP && isFunction(vl)) return vl;
	    tl = TYPEOF(vl);
            if (tl == INTSXP) tl = REALSXP;
	    if (tl == mode) return vl;
	}
	if (inherits)
	    rho = ENCLOS(rho);
	else
	    return (R_UnboundValue);
    }
    return (R_UnboundValue);
}


/*
   ddVal:
   a function to take a name and determine if it is of the form
   ..x where x is an integer; if so x is returned otherwise 0 is returned
*/
static int ddVal(SEXP symbol)
{
    const char *buf;
    char *endp;
    int rval;

    buf = CHAR(PRINTNAME(symbol));
    if( !strncmp(buf,"..",2) && strlen(buf) > 2 ) {
	buf += 2;
	rval = strtol(buf, &endp, 10);
	if( *endp != '\0')
	    return 0;
	else
	    return rval;
    }
    return 0;
}

/*----------------------------------------------------------------------
  ddfindVar

  This function fetches the variables ..1, ..2, etc from the first
  frame of the environment passed as the second argument to ddfindVar.
  These variables are implicitly defined whenever a ... object is
  created.

  To determine values for the variables we first search for an
  explicit definition of the symbol, them we look for a ... object in
  the frame and then walk through it to find the appropriate values.

  If no value is obtained we return R_UnboundValue.

  It is an error to specify a .. index longer than the length of the
  ... object the value is sought in.

*/

SEXP ddfindVar(SEXP symbol, SEXP rho)
{
    int i;
    SEXP vl;

    /* first look for ... symbol  */
    vl = findVar(R_DotsSymbol, rho);
    i = ddVal(symbol);
    if (vl != R_UnboundValue) {
	if (vl != R_MissingArg && vl != R_MissingUnder && length(vl) >= i) {
	    vl = nthcdr(vl, i - 1);
	    return(CAR(vl));
	}
	else
	    error(_("The ... list does not contain %d elements"), i);
    }
    else error(_("..%d used in an incorrect context, no ... to look in"), i);

    return R_NilValue;
}



/*----------------------------------------------------------------------

  dynamicFindVar

  This function does a variable lookup, but uses dynamic scoping rules
  rather than the lexical scoping rules used in findVar.

  Return R_UnboundValue if the symbol isn't located and the calling
  function needs to handle the errors.

*/

SEXP dynamicfindVar(SEXP symbol, RCNTXT *cptr)
{
    SEXP vl;
    while (cptr != R_ToplevelContext) {
	if (cptr->callflag & CTXT_FUNCTION) {
	    vl = findVarInFrame3(cptr->cloenv, symbol, TRUE);
	    if (vl != R_UnboundValue) return vl;
	}
	cptr = cptr->nextcontext;
    }

    return R_UnboundValue;
}



/*----------------------------------------------------------------------

  findFun

  Search for a function in an environment. This is a specially modified
  version of findVar which ignores values its finds if they are not
  functions.  This is extremely time-critical code, since it is used
  to lookup all the base language elements, such as "+" and "if".  
  (Though it's often bypassed by the FINDFUN macro in eval.c.)

  In typical usage, rho will be an unhashed local environment with the
  NO_SPEC_SYM flag set, with ENCLOS giving further such environments, until 
  R_GlobalEnv is reached, where the function will be found in the global
  cache (if it wasn't in one of the local environemnts).  Note that
  R_GlobalEnv will not have NO_SPEC_SYM set, even if it has no special
  symbols - otherwise, it might be skipped, and hence the global cache
  would be skipped as well.

  An environment can also be skipped when the symbol has LASTSYMENVNOTFOUND
  equal to that environment.  LASTSYMENVNOTFOUND is updated to the last
  unhashed environment where the symbol wasn't found.

  There is no need to wait for computations of the values found to finish, 
  since functions never have their computation deferred.

  The findFun_nospecsym function skips the special symbol check, for use in 
  the FINDFUN macro when that check has already been done.
*/

SEXP findFun(SEXP symbol, SEXP rho)
{
    /* If it's a special symbol, skip to the first environment that might 
       contain such a symbol. */

    if (SPEC_SYM(symbol)) {
        while (NO_SPEC_SYM(rho)) /* note that NO_SPEC_SYM(R_EmptyEnv) is 0 */
            rho = ENCLOS(rho);
    }

    return findFun_nospecsym(symbol,rho);
}

SEXP attribute_hidden findFun_nospecsym(SEXP symbol, SEXP rho)
{
    SEXP32 last_sym_not_found = LASTSYMENVNOTFOUND(symbol);
    SEXP last_unhashed_env_nf = R_NoObject;
    SEXP vl;

    if (TYPEOF(symbol) != SYMSXP) abort();

    /* Search environments for a definition that is a function. */

    for ( ; rho != R_EmptyEnv; rho = ENCLOS(rho)) {

        /* Handle base environment/namespace quickly, as functions in base
           and other packages will see it rather than the global cache. */

        if (IS_BASE(rho)) {
            vl = SYMBOL_BINDING_VALUE(symbol);
            if (vl != R_UnboundValue) 
                goto got_value;
            continue;
        }

        if (rho == R_GlobalEnv) {
            vl = findGlobalVar(symbol);
            if (vl != R_UnboundValue)
                goto got_value;
            goto err;
        }

        /* See if it's known from LASTSYMENVNOTFOUND that this symbol isn't 
           in this environment. */

        if (SEXP32_FROM_SEXP(rho) == last_sym_not_found) {
            last_unhashed_env_nf = rho;
            continue;
        }

        vl = findVarInFramePendingOK (rho, symbol);
	if (vl != R_UnboundValue)
            goto got_value;

        if (HASHTAB(rho) == R_NilValue)
            last_unhashed_env_nf = rho;
        continue;

    got_value:
        if (TYPEOF(vl) == PROMSXP) {
            SEXP pv = PRVALUE_PENDING_OK(vl);
            vl = pv != R_UnboundValue ? pv : forcePromise(vl);
        }

        if (isFunction (vl)) {
            if (last_unhashed_env_nf != R_NoObject)
                LASTSYMENVNOTFOUND(symbol) = 
                  SEXP32_FROM_SEXP(last_unhashed_env_nf);
            return vl;
        }

        if (vl == R_MissingArg)
            arg_missing_error(symbol);
    }

  err:
    error(_("could not find function \"%s\""), CHAR(PRINTNAME(symbol)));
}

/* Variation on findFun that doesn't report errors itself, doesn't
   check for special symbols, and records a failed local search even
   if the whole search fails (unlike findFun).

   Used for looking up methods in objects.c. */

SEXP findFunMethod(SEXP symbol, SEXP rho)
{
    if (TYPEOF(symbol) != SYMSXP) abort();

    SEXP32 last_sym_not_found = LASTSYMENVNOTFOUND(symbol);
    SEXP last_unhashed_env_nf = R_NoObject;
    SEXP vl;

    for ( ; rho != R_EmptyEnv; rho = ENCLOS(rho)) {
        if (SEXP32_FROM_SEXP(rho) == last_sym_not_found) {
            last_unhashed_env_nf = rho;
            continue;
        }
        if (rho == R_GlobalEnv) {
            vl = findGlobalVar(symbol);
            if (vl == R_UnboundValue)
                break;
        }
        else {
            vl = findVarInFramePendingOK(rho, symbol);
            if (vl == R_UnboundValue) {
                if (HASHTAB(rho) == R_NilValue)
                    last_unhashed_env_nf = rho;
                continue;
            }
        }
        if (TYPEOF(vl) == PROMSXP)
            vl = forcePromise(vl);
        if (isFunction(vl)) {
            if (last_unhashed_env_nf != R_NoObject)
                LASTSYMENVNOTFOUND(symbol) = 
                  SEXP32_FROM_SEXP(last_unhashed_env_nf);
            return vl;
        }
    }

    if (last_unhashed_env_nf != R_NoObject && !IS_BASE(last_unhashed_env_nf))
        LASTSYMENVNOTFOUND(symbol) = SEXP32_FROM_SEXP(last_unhashed_env_nf);
    return R_UnboundValue;
}

/*----------------------------------------------------------------------

  set_var_in_frame

  Assign a value in a specific environment frame.  If 'create' is TRUE, it
  creates the variable if it doesn't already exist.  May increment or decrement
  NAMEDCNT for the assigned and previous value, depending on the setting of
  'incdec' - 0 = no increment or decrement, 1 = decrement old value only,
  2 = increment new value only, 3 = decrement old value and increment new value.
  Returns TRUE if an assignment was successfully made. 

  Waits for computation to finish for values stored into user databases and 
  into the base environment.

  The symbol, value, and rho arguments are protected by this function. 

  Sets R_binding_cell to the CONS cell for the binding, if there is one,
  and it is suitable for updating. */

int set_var_in_frame (SEXP symbol, SEXP value, SEXP rho, int create, int incdec)
{
    int hashcode;
    SEXP loc;

    if (TYPEOF(symbol) != SYMSXP) abort();

    PROTECT3(symbol,value,rho);

    R_binding_cell = R_NilValue;

    if (SEXP32_FROM_SEXP(rho) == LASTSYMENV(symbol)) {
        loc = LASTSYMBINDING(symbol);    /* won't be an active binding */
        if (CAR(loc) != R_UnboundValue)  /* could be unbound if var removed */
            goto found;
    }

    if (IS_USER_DATABASE(rho)) {
        /* FIXME: This does not behave as described - DESCRIBED WHERE? HOW? */
        WAIT_UNTIL_COMPUTED(value);
        R_ObjectTable *table;
        table = (R_ObjectTable *) R_ExternalPtrAddr(HASHTAB(rho));
        if (table->assign == NULL) /* maybe should just return FALSE? */
            error(_("cannot assign variables to this database"));
        table->assign (CHAR(PRINTNAME(symbol)), value, table);
        if (incdec&2) 
            INC_NAMEDCNT(value);
        /* don't try to do decrement on old value (getting it might be slow) */

        if (IS_GLOBAL_FRAME(rho)) R_FlushGlobalCache(symbol);

        if (rho == R_GlobalEnv) 
            R_DirtyImage = 1;
        UNPROTECT(3);
        return TRUE; /* unclear whether assign always succeeds, but assume so */
    }

    if (IS_BASE(rho)) {
        if (!create && SYMVALUE(symbol) == R_UnboundValue) {
            UNPROTECT(3);
            return FALSE;
        }
        gsetVar(symbol,value,rho);
        if (incdec&2) 
            INC_NAMEDCNT(value);  
        /* don't bother with decrement on the old value (not time-critical) */
        UNPROTECT(3);
        return TRUE;      /* should have either succeeded, or raised an error */
    }

    if (HASHTAB(rho) == R_NilValue) {
        if (LASTSYMENVNOTFOUND(symbol) != SEXP32_FROM_SEXP(rho)) {
            loc = FRAME(rho);
            SEARCH_LOOP (loc, symbol, goto found_update_last);
        }
    }
    else { /* hashed environment */
        hashcode = SYM_HASH(symbol) % HASHSIZE(HASHTAB(rho));
        loc = VECTOR_ELT(HASHTAB(rho), hashcode);
        SEARCH_LOOP (loc, symbol, goto found);
    }

    if (create) { /* try to create new variable */

        SEXP new;

        if (rho == R_EmptyEnv)
            error(_("cannot assign values in the empty environment"));

        if (FRAME_IS_LOCKED(rho))
            error(_("cannot add bindings to a locked environment"));

        if (IS_GLOBAL_FRAME(rho)) R_FlushGlobalCache(symbol);

        if (rho == R_GlobalEnv) 
            R_DirtyImage = 1;

        if (HASHTAB(rho) == R_NilValue) {
            new = cons_with_tag (value, FRAME(rho), symbol);
            SET_FRAME(rho, new);
            if (SPEC_SYM(symbol))
                SET_NO_SPEC_SYM(rho,0);
        }
        else {
            SEXP table = HASHTAB(rho);
            SEXP chain = VECTOR_ELT(table,hashcode);
            new = cons_with_tag (value, chain, symbol);
            SET_VECTOR_ELT (table, hashcode, new);
            if (chain == R_NilValue)
                SET_HASHSLOTSUSED (table, HASHSLOTSUSED(table) + 1);
            if (R_HashSizeCheck(table))
                SET_HASHTAB(rho, R_HashResize(table));
        }

        if (LASTSYMENVNOTFOUND(symbol) == SEXP32_FROM_SEXP(rho))
            LASTSYMENVNOTFOUND(symbol) = R_NoObject32;

        if (incdec&2)
            INC_NAMEDCNT(value);

        UNPROTECT(3);
        R_binding_cell = new;
        return TRUE;
    }

    UNPROTECT(3);
    return FALSE;

  found_update_last:
    if ( ! IS_ACTIVE_BINDING(loc) && !DDVAL(symbol)) {
        /* Note:  R_MakeActiveBinding won't let an existing binding 
           become active, so we later assume this can't be active. */
        LASTSYMENV(symbol) = SEXP32_FROM_SEXP(rho);
        LASTSYMBINDING(symbol) = loc;
    }
    
  found:
    if (!IS_ACTIVE_BINDING(loc)) {
        R_binding_cell = BINDING_IS_LOCKED(loc) ? R_NilValue : loc;
        if (incdec&1)
            DEC_NAMEDCNT_AND_PRVALUE(BINDING_VALUE(loc));
    }
    SET_BINDING_VALUE(loc,value);
    if (incdec&2) 
        INC_NAMEDCNT(value);
    SET_MISSING(loc,0);
    if (rho == R_GlobalEnv) 
        R_DirtyImage = 1;
    UNPROTECT(3);
    return TRUE;
}


/*----------------------------------------------------------------------

  set_var_nonlocal

  Assign a value in a specific environment frame or any enclosing frame,
  or create it in R_GlobalEnv if it doesn't exist in any such frame.  May 
  increment or decrement NAMEDCNT for the assigned and previous value, 
  depending on the setting of 'incdec' - 0 = no increment or decrement, 
  1 = decrement old value only, 2 = increment new value only, 3 = decrement 
  old value and increment new value. 

  Sets R_binding_cell as for set_var_in_frame. */

void set_var_nonlocal (SEXP symbol, SEXP value, SEXP rho, int incdec)
{
    while (rho != R_EmptyEnv) {
	if (set_var_in_frame (symbol, value, rho, FALSE, incdec)) 
            return;
	rho = ENCLOS(rho);
    }
    set_var_in_frame (symbol, value, R_GlobalEnv, TRUE, incdec);
}


/*----------------------------------------------------------------------

  OLD FUNCTION PROVIDED FOR BACKWARDS COMPATIBILITY (MENTIONED IN R API).

  defineVar

  Assign a value in a specific environment frame. */

void defineVar(SEXP symbol, SEXP value, SEXP rho)
{
    set_var_in_frame (symbol, value, rho, TRUE, 0);
}


/*----------------------------------------------------------------------

  OLD FUNCTION PROVIDED FOR BACKWARDS COMPATIBILITY (MENTIONED IN R API).

  setVar

  Assign a new value to bound symbol.	 Note this does the "inherits"
  case.  I.e. it searches frame-by-frame for a symbol and binds the
  given value to the first symbol encountered.  If no symbol is
  found then a binding is created in the global environment. */

void setVar(SEXP symbol, SEXP value, SEXP rho)
{
    while (rho != R_EmptyEnv) {
	if (set_var_in_frame (symbol, value, rho, FALSE, 0)) 
            return;
	rho = ENCLOS(rho);
    }
    set_var_in_frame (symbol, value, R_GlobalEnv, TRUE, 0);
}


/*----------------------------------------------------------------------

  gsetVar

  Assignment directly into the base environment.  

  Waits until the value has been computed. */

void gsetVar(SEXP symbol, SEXP value, SEXP rho)
{
    if (TYPEOF(symbol) != SYMSXP) abort();

    if (SYMVALUE(symbol) == R_UnboundValue) {
        if (FRAME_IS_LOCKED(rho))
            error(_("cannot add binding of '%s' to the base environment"),
                  CHAR(PRINTNAME(symbol)));
    }

    R_FlushGlobalCache(symbol);

    WAIT_UNTIL_COMPUTED(value);
    SET_SYMBOL_BINDING_VALUE(symbol, value);
}


/*----------------------------------------------------------------------

  get environment from a subclass if possible; else return NULL. */

#define simple_as_environment(arg) \
  (IS_S4_OBJECT(arg) && (TYPEOF(arg) == S4SXP) ? R_getS4DataSlot(arg, ENVSXP) \
                                               : R_NilValue)
	    

/*----------------------------------------------------------------------

  do_assign : .Internal(assign(x, value, envir, inherits))

*/
static SEXP do_assign(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP name=R_NilValue, val, aenv;
    int ginherits = 0;
    checkArity(op, args);

    if (!isString(CAR(args)) || length(CAR(args)) == 0)
	error(_("invalid first argument"));
    else {
	if (length(CAR(args)) > 1)
	    warning(_("only the first element is used as variable name"));
	name = install(translateChar(STRING_ELT(CAR(args), 0)));
    }
    PROTECT(val = CADR(args));
    aenv = CADDR(args);
    if (TYPEOF(aenv) == NILSXP)
	error(_("use of NULL environment is defunct"));
    if (TYPEOF(aenv) != ENVSXP &&
	TYPEOF((aenv = simple_as_environment(aenv))) != ENVSXP)
	error(_("invalid '%s' argument"), "envir");
    ginherits = asLogical(CADDDR(args));
    if (ginherits == NA_LOGICAL)
	error(_("invalid '%s' argument"), "inherits");
    if (ginherits)
	set_var_nonlocal (name, val, aenv, 3);
    else
	set_var_in_frame (name, val, aenv, TRUE, 3);
    UNPROTECT(1);
    return val;
}


/**
 * do_list2env : .Internal(list2env(x, envir))
  */
static SEXP do_list2env(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP x, xnms, envir;
    int n;
    checkArity(op, args);

    if (TYPEOF(CAR(args)) != VECSXP)
	error(_("first argument must be a named list"));
    x = CAR(args);
    n = LENGTH(x);
    xnms = getAttrib(x, R_NamesSymbol);
    if (TYPEOF(xnms) != STRSXP || LENGTH(xnms) != n)
	error(_("names(x) must be a character vector of the same length as x"));
    envir = CADR(args);
    if (TYPEOF(envir) != ENVSXP)
	error(_("'envir' argument must be an environment"));

    for(int i = 0; i < LENGTH(x) ; i++) {
	SEXP name = install(translateChar(STRING_ELT(xnms, i)));
	defineVar(name, VECTOR_ELT(x, i), envir);
    }

    return envir;
}

/* Remove variable and return its previous value, or R_NoObject if it
   didn't exist.  For a user database, R_NilValue is returned when the
   variable exists, rather than the value. */

SEXP attribute_hidden RemoveVariable(SEXP name, SEXP env)
{
    SEXP list, value;

    if (TYPEOF(name) != SYMSXP) abort();

    if (env == R_BaseNamespace)
	error(_("cannot remove variables from base namespace"));
    if (env == R_BaseEnv)
	error(_("cannot remove variables from the base environment"));
    if (env == R_EmptyEnv)
	error(_("cannot remove variables from the empty environment"));
    if (FRAME_IS_LOCKED(env))
	error(_("cannot remove bindings from a locked environment"));

    if(IS_USER_DATABASE(env)) {
	R_ObjectTable *table;
	table = (R_ObjectTable *) R_ExternalPtrAddr(HASHTAB(env));
	if(table->remove == NULL)
	    error(_("cannot remove variables from this database"));
	return table->remove(CHAR(PRINTNAME(name)), table) 
                 ? R_NilValue : R_NoObject;
    }

    if (IS_HASHED(env)) {
	SEXP hashtab = HASHTAB(env);
        int hashcode = SYM_HASH(name);
	int idx = hashcode % HASHSIZE(hashtab);
	list = RemoveFromList(name, VECTOR_ELT(hashtab, idx), &value);
	if (value != R_NoObject) {
	    SET_VECTOR_ELT(hashtab, idx, list);
            if (list == R_NilValue)
                SET_HASHSLOTSUSED(hashtab,HASHSLOTSUSED(hashtab)-1);
        }
    }
    else {
	list = RemoveFromList(name, FRAME(env), &value);
	if (value != R_NoObject)
	    SET_FRAME(env, list);
    }

    if (value != R_NoObject) {
        if(env == R_GlobalEnv) R_DirtyImage = 1;
	if (IS_GLOBAL_FRAME(env)) {
            PROTECT(value);
            R_FlushGlobalCache(name);
            UNPROTECT(1);
        }
    }

    return value;
}

/*----------------------------------------------------------------------

  do_remove

  There are three arguments to do_remove; a list of names to remove,
  an optional environment (if missing set it to R_GlobalEnv) and
  inherits, a logical indicating whether to look in the parent env if
  a symbol is not found in the supplied env.  This is ignored if
  environment is not specified.

*/

static SEXP do_remove(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    /* .Internal(remove(list, envir, inherits)) */

    SEXP name, envarg, tsym, tenv, value;
    int ginherits = 0;
    int i;

    checkArity(op, args);

    name = CAR(args);
    if (!isString(name))
	error(_("invalid first argument"));
    args = CDR(args);

    envarg = CAR(args);
    if (TYPEOF(envarg) == NILSXP)
	error(_("use of NULL environment is defunct"));
    if (TYPEOF(envarg) != ENVSXP &&
	TYPEOF((envarg = simple_as_environment(envarg))) != ENVSXP)
	error(_("invalid '%s' argument"), "envir");
    args = CDR(args);

    ginherits = asLogical(CAR(args));
    if (ginherits == NA_LOGICAL)
	error(_("invalid '%s' argument"), "inherits");

    for (i = 0; i < LENGTH(name); i++) {
	value = R_NoObject;
	tsym = install(translateChar(STRING_ELT(name, i)));
	tenv = envarg;
	while (tenv != R_EmptyEnv) {
	    value = RemoveVariable(tsym, tenv);
	    if (value != R_NoObject || !ginherits)
		break;
	    tenv = CDR(tenv);
	}
	if (value == R_NoObject)
	    warning (_("object '%s' not found"), CHAR(PRINTNAME(tsym)));
        else
            DEC_NAMEDCNT_AND_PRVALUE(value);
    }
    return R_NilValue;
}

/*----------------------------------------------------------------------

 do_get_rm - get value of variable and then remove the variable, decrementing
             NAMEDCNT when possible.  If return of pending value is allowed,
             will pass on pending value in the variable without waiting for it.
*/

static SEXP do_get_rm (SEXP call, SEXP op, SEXP args, SEXP rho, int variant)
{
    SEXP name, value;

    checkArity(op, args);
    check1arg(args, call, "x");

    name = CAR(args);
    if (TYPEOF(name) != SYMSXP)
        error(_("invalid argument"));

    value = RemoveVariable (name, rho);

    if (value == R_NoObject)
        unbound_var_error(name);

    if (TYPEOF(value) == PROMSXP) {
        SEXP prvalue = forcePromise(value);
        DEC_NAMEDCNT_AND_PRVALUE(value);
        value = prvalue;
    }
    else
        DEC_NAMEDCNT(value);

    if (variant & VARIANT_NULL)
        return R_NilValue;

    if ( ! (variant & VARIANT_PENDING_OK))
        WAIT_UNTIL_COMPUTED(value);

    return value;
}

/*----------------------------------------------------------------------

  do_get

  This function returns the SEXP associated with the character
  argument.  It needs the environment of the calling function as a
  default.

      get(x, envir, mode, inherits)
      exists(x, envir, mode, inherits)

*/


static SEXP do_get(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP rval, genv, t1 = R_NilValue;
    SEXPTYPE gmode;
    int ginherits = 0, where;
    checkArity(op, args);

    /* The first arg is the object name */
    /* It must be present and a non-empty string */

    if (!isValidStringF(CAR(args)))
	error(_("invalid first argument"));
    else
	t1 = install(translateChar(STRING_ELT(CAR(args), 0)));

    /* envir :	originally, the "where=" argument */

    if (TYPEOF(CADR(args)) == REALSXP || TYPEOF(CADR(args)) == INTSXP) {
	where = asInteger(CADR(args));
	genv = R_sysframe(where, R_GlobalContext);
    }
    else if (TYPEOF(CADR(args)) == NILSXP)
	error(_("use of NULL environment is defunct"));
    else if (TYPEOF(CADR(args)) == ENVSXP)
	genv = CADR(args);
    else if(TYPEOF((genv = simple_as_environment(CADR(args)))) != ENVSXP)
	error(_("invalid '%s' argument"), "envir");

    /* mode :  The mode of the object being sought */

    /* as from R 1.2.0, this is the *mode*, not the *typeof* aka
       storage.mode.
    */

    if (isString(CADDR(args))) {
	if (!strcmp(CHAR(STRING_ELT(CAR(CDDR(args)), 0)), "function")) /*ASCII*/
	    gmode = FUNSXP;
	else
	    gmode = str2type(CHAR(STRING_ELT(CAR(CDDR(args)), 0))); /* ASCII */
    } 
    else
	error(_("invalid '%s' argument"), "mode");

    ginherits = asLogical(CADDDR(args));
    if (ginherits == NA_LOGICAL)
	error(_("invalid '%s' argument"), "inherits");

    /* Search for the object */
    rval = findVar1mode(t1, genv, gmode, ginherits, PRIMVAL(op));

    if (PRIMVAL(op)) { /* have get(.) */
	if (rval == R_MissingArg)
            arg_missing_error(t1);
	if (rval == R_UnboundValue) {
	    if (gmode == ANYSXP)
                unbound_var_error(t1);
	    else
		error(_("object '%s' of mode '%s' was not found"),
		      CHAR(PRINTNAME(t1)),
		      CHAR(STRING_ELT(CAR(CDDR(args)), 0))); /* ASCII */
	}

	/* We need to evaluate if it is a promise */
	if (TYPEOF(rval) == PROMSXP)
	    rval = forcePromise(rval);

	if (NAMEDCNT_EQ_0(rval))
	    SET_NAMEDCNT_1(rval);
	return rval;
    }
    else /* exists(.) */
	return ScalarLogicalMaybeConst (rval != R_UnboundValue);
}

static SEXP gfind(const char *name, SEXP env, SEXPTYPE mode,
		  SEXP ifnotfound, int inherits, SEXP enclos)
{
    SEXP rval, t1, R_fcall, var;

    t1 = install(name);

    /* Search for the object - last arg is 1 to 'get' */
    rval = findVar1mode(t1, env, mode, inherits, 1);

    if (rval == R_UnboundValue) {
	if( isFunction(ifnotfound) ) {
	    PROTECT(var = mkString(name));
	    PROTECT(R_fcall = LCONS(ifnotfound, CONS(var, R_NilValue)));
	    rval = eval(R_fcall, enclos);
	    UNPROTECT(2);
	} else
	    rval = ifnotfound;
    }

    /* We need to evaluate if it is a promise */
    if (TYPEOF(rval) == PROMSXP) rval = forcePromise(rval);
    if (NAMEDCNT_EQ_0(rval)) 
        SET_NAMEDCNT_1(rval);
    return rval;
}


/** mget(): get multiple values from an environment
 *
 * .Internal(mget(x, envir, mode, ifnotfound, inherits))
 *
 * @return  a list of the same length as x, a character vector (of names).
 */
static SEXP do_mget(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP ans, env, x, mode, ifnotfound, ifnfnd;
    SEXPTYPE gmode; /* is unsigned int */
    int ginherits = 0, nvals, nmode, nifnfnd, i;

    checkArity(op, args);

    x = CAR(args);

    nvals = length(x);

    /* The first arg is the object name */
    /* It must be present and a string */
    if (!isString(x) )
	error(_("invalid first argument"));
    for(i = 0; i < nvals; i++)
	if( isNull(STRING_ELT(x, i)) || !CHAR(STRING_ELT(x, 0))[0] )
	    error(_("invalid name in position %d"), i+1);

    /* FIXME: should we install them all?) */

    env = CADR(args);
    if (env == R_NilValue) {
	error(_("use of NULL environment is defunct"));
    } else if( !isEnvironment(env) )
	error(_("second argument must be an environment"));

    mode = CADDR(args);
    nmode = length(mode);
    if( !isString(mode) )
	error(_("invalid '%s' argument"), "mode");

    if( nmode != nvals && nmode != 1 )
	error(_("wrong length for '%s' argument"), "mode");

    PROTECT(ifnotfound = coerceVector(CADDDR(args), VECSXP));
    nifnfnd = length(ifnotfound);
    if( !isVector(ifnotfound) )
	error(_("invalid '%s' argument"), "ifnotfound");

    if( nifnfnd != nvals && nifnfnd != 1 )
	error(_("wrong length for '%s' argument"), "ifnotfound");

    ginherits = asLogical(CAD4R(args));
    if (ginherits == NA_LOGICAL)
	error(_("invalid '%s' argument"), "inherits");

    PROTECT(ans = allocVector(VECSXP, nvals));

    /* now for each element of x, we look for it, using the inherits,
       etc */

    for(i = 0; i < nvals; i++) {
	if (isString(mode)) { /* ASCII */
	    if (!strcmp(CHAR(STRING_ELT(CAR(CDDR(args)), i % nmode )), "function"))
		gmode = FUNSXP;
	    else
		gmode = str2type(CHAR(STRING_ELT(CAR(CDDR(args)), i % nmode )));
	} 
        else
	    error(_("invalid '%s' argument"), "mode");

	/* is the mode provided one of the real modes? */
	if( gmode == (SEXPTYPE) (-1))
	    error(_("invalid '%s' argument"), "mode");


	if( TYPEOF(ifnotfound) != VECSXP )
	    error(_("invalid '%s' argument"), "ifnotfound");
	if( nifnfnd == 1 ) /* length has been checked to be 1 or nvals. */
	    ifnfnd = VECTOR_ELT(ifnotfound, 0);
	else
	    ifnfnd = VECTOR_ELT(ifnotfound, i);

        SET_VECTOR_ELEMENT_TO_VALUE (ans, i, 
          gfind (translateChar(STRING_ELT(x,i % nvals)), 
                 env, gmode, ifnfnd, ginherits, rho));
    }

    setAttrib(ans, R_NamesSymbol, duplicate(x));
    UNPROTECT(2);
    return(ans);
}

/* R_isMissing is called on the not-yet-evaluated (or sometimes evaluated)
   value of an argument, if this is a symbol, as it could be a missing 
   argument that has been passed down.  So 'symbol' is the promise value, 
   and 'rho' its evaluation argument.

   It is called in do_missing and in evalList_v.

   Return 0 if not missing, 1 if missing from empty arg, 2 if missing from "_".
   Note that R_isMissing pays no attention to the MISSING field, only to
   whether things are R_MissingArg or R_MissingUnder.

   Cycles in promises checked are detected by looking at each previous one.
   This takes quadratic time, but the number of promises looked at should
   normally be very small.
*/

struct detectcycle { struct detectcycle *next; SEXP prom; };

static int isMissing_recursive (SEXP, SEXP, struct detectcycle *);

int attribute_hidden R_isMissing(SEXP symbol, SEXP rho)
{
    return isMissing_recursive (symbol, rho, NULL);
}

static int isMissing_recursive(SEXP symbol, SEXP rho, struct detectcycle *dc)
{
    int ddv=0;
    SEXP vl, s;

    if (symbol == R_MissingArg)
	return 1;
    if (symbol == R_MissingUnder)
	return 2;

    if (DDVAL(symbol)) {
	s = R_DotsSymbol;
	ddv = ddVal(symbol);
    }
    else
	s = symbol;

    if (rho == R_BaseEnv || rho == R_BaseNamespace)
	return 0;

    vl = findVarLocInFrame(rho, s, NULL);
    if (vl != R_NilValue) {
        SEXP vlv = CAR(vl);
	if (DDVAL(symbol)) {
            if (vlv == R_MissingUnder)
                return 2;
	    if (vlv == R_UnboundValue || vlv == R_MissingArg || length(vlv)<ddv)
		return 1;
            vl = nthcdr(vlv, ddv-1);
            vlv = CAR(vl);
	}
	if (vlv==R_MissingArg)
	    return 1;
        if (vlv==R_MissingUnder)
            return 2;
	if (IS_ACTIVE_BINDING(vl))
	    return 0;
	if (TYPEOF(vlv)==PROMSXP && TYPEOF(PREXPR(vlv))==SYMSXP
             && (PRVALUE(vlv)==R_UnboundValue || PRVALUE(vlv)==R_MissingArg)) {
            for (struct detectcycle *p = dc; p != NULL; p = p->next) {
                if (p->prom == vlv) {
                    return 1;
                }
            }
            struct detectcycle dc2;
            dc2.next = dc;
            dc2.prom = vlv;
            int val;
            PROTECT(vl);
            R_CHECKSTACK();
            val = isMissing_recursive(PREXPR(vlv), PRENV(vlv), &dc2);
            UNPROTECT(1); /* vl */
            return val;
	}
	else
	    return 0;
    }
    return 0;
}


/*----------------------------------------------------------------------

  do_missing and do_missing_from_underline

  This function tests whether the symbol passed as its first argument
  is a missing argument to the current closure.  rho is the
  environment that missing was called from.

  Note that an argument with a default value is considered missing
  if the default was used, but this is NOT applied recursively to 
  arguments that are arguments in the calling function that were
  filled in from the default value.

  These are primitive and SPECIALSXP */

static SEXP do_missing(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP t, sym, s;
    int under = PRIMVAL(op);
    int ddv = 0;

    checkArity(op, args);
    check1arg_x (args, call);

    sym = CAR(args);
    if (isString(sym) && length(sym)==1)
	sym = install(translateChar(STRING_ELT(CAR(args), 0)));
    if (!isSymbol(sym))
	errorcall(call, _("invalid use of 'missing'"));

    if (DDVAL(sym)) {
	ddv = ddVal(sym);
	s = R_DotsSymbol;
    }
    else
        s = sym;

    t = findVarLocInFrame (rho, s, NULL);

    if (t == R_NilValue)  /* no error for local variables, despite msg below */
	errorcall(call, _("'missing' can only be used for arguments"));

    if (DDVAL(sym)) {
        if (CAR(t) == R_MissingUnder
             || !under && (CAR(t) == R_UnboundValue || CAR(t) == R_MissingArg
                                                    || length(CAR(t)) < ddv))
            goto true;
        t = nthcdr(CAR(t), ddv-1);
    }

    if (CAR(t) == R_MissingUnder
         || !under && (MISSING(t) || CAR(t) == R_MissingArg))
        goto true;

    t = CAR(t);
    if (TYPEOF(t)==PROMSXP && isSymbol(PREXPR(t))) { 
        PROTECT(t);
        int m = R_isMissing(PREXPR(t),PRENV(t));
        UNPROTECT(1);
        if (m == 2 || !under && m)
            goto true;
    }

    return ScalarLogicalMaybeConst(FALSE);

  true:
    return ScalarLogicalMaybeConst(TRUE);
}

/*----------------------------------------------------------------------

  do_globalenv

  Returns the current global environment.
*/


static SEXP do_globalenv(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    checkArity(op, args);
    return R_GlobalEnv;
}

/*----------------------------------------------------------------------

  do_baseenv

  Returns the current base environment.

*/


static SEXP do_baseenv(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    checkArity(op, args);
    return R_BaseEnv;
}

/*----------------------------------------------------------------------

  do_emptyenv

  Returns the current empty environment.

*/


static SEXP do_emptyenv(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    checkArity(op, args);
    return R_EmptyEnv;
}


/*----------------------------------------------------------------------

  do_attach

  To attach a list we make up an environment and insert components
  of the list in as the values of this env and install the tags from
  the list as the names.

*/

static SEXP do_attach(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP name, s, t, x;
    int pos, hsize;
    Rboolean isSpecial;

    checkArity(op, args);

    pos = asInteger(CADR(args));
    if (pos == NA_INTEGER)
	error(_("'pos' must be an integer"));

    name = CADDR(args);
    if (!isValidStringF(name))
	error(_("invalid '%s' argument"), "name");

    isSpecial = IS_USER_DATABASE(CAR(args));

    if(!isSpecial) {
	if (isNewList(CAR(args))) {
	    SETCAR(args, VectorToPairList(CAR(args)));

	    for (x = CAR(args); x != R_NilValue; x = CDR(x))
		if (TAG(x) == R_NilValue)
		    error(_("all elements of a list must be named"));
	    PROTECT(s = allocSExp(ENVSXP));
	    SET_FRAME(s, duplicate(CAR(args)));
	} else if (isEnvironment(CAR(args))) {
	    SEXP p, loadenv = CAR(args);

	    PROTECT(s = allocSExp(ENVSXP));
	    if (HASHTAB(loadenv) != R_NilValue) {
		int i, n;
		n = length(HASHTAB(loadenv));
		for (i = 0; i < n; i++) {
		    p = VECTOR_ELT(HASHTAB(loadenv), i);
		    while (p != R_NilValue) {
			defineVar(TAG(p), duplicate(CAR(p)), s);
			p = CDR(p);
		    }
		}
		/* FIXME: duplicate the hash table and assign here */
	    } else {
		for(p = FRAME(loadenv); p != R_NilValue; p = CDR(p))
		    defineVar(TAG(p), duplicate(CAR(p)), s);
	    }
	} 
        else
	    error(_("'attach' only works for lists, data frames and environments"));

	/* Connect FRAME(s) into HASHTAB(s) */
        hsize = (int) (length(s)/0.6);   /* about 45% of entries will be used */
	if (hsize < HASHMINSIZE)
	    hsize = HASHMINSIZE;

	SET_HASHTAB(s, R_NewHashTable(hsize));
	R_HashFrame(s);

	while (R_HashSizeCheck(HASHTAB(s)))
	    SET_HASHTAB(s, R_HashResize(HASHTAB(s)));

    } else { /* is a user object */
	/* Having this here (rather than below) means that the onAttach routine
	   is called before the table is attached. This may not be necessary or
	   desirable. */
	R_ObjectTable *tb = (R_ObjectTable*) R_ExternalPtrAddr(CAR(args));
	if(tb->onAttach)
	    tb->onAttach(tb);
	PROTECT(s = allocSExp(ENVSXP));
	SET_HASHTAB(s, CAR(args));
	setAttrib(s, R_ClassSymbol, getAttrib(HASHTAB(s), R_ClassSymbol));
    }

    setAttrib(s, R_NameSymbol, name);
    for (t = R_GlobalEnv; ENCLOS(t) != R_BaseEnv && pos > 2; t = ENCLOS(t))
	pos--;

    if (ENCLOS(t) == R_BaseEnv) {
	SET_ENCLOS(t, s);
	SET_ENCLOS(s, R_BaseEnv);
    }
    else {
	x = ENCLOS(t);
	SET_ENCLOS(t, s);
	SET_ENCLOS(s, x);
    }

    if(!isSpecial) { /* Temporary: need to remove the elements identified by objects(CAR(args)) */
	R_FlushGlobalCacheFromTable(HASHTAB(s));
	MARK_AS_GLOBAL_FRAME(s);
    } else {
	R_FlushGlobalCacheFromUserTable(HASHTAB(s));
	MARK_AS_GLOBAL_FRAME(s);
    }

    UNPROTECT(1); /* s */
    return s;
}



/*----------------------------------------------------------------------

  do_detach

  detach the specified environment.  Detachment only takes place by
  position.

*/

static SEXP do_detach(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP s, t, x;
    int pos, n;
    Rboolean isSpecial = FALSE;

    checkArity(op, args);
    pos = asInteger(CAR(args));

    for (n = 2, t = ENCLOS(R_GlobalEnv); t != R_BaseEnv; t = ENCLOS(t))
	n++;

    if (pos == n) /* n is the length of the search list */
	error(_("detaching \"package:base\" is not allowed"));

    for (t = R_GlobalEnv ; ENCLOS(t) != R_BaseEnv && pos > 2 ; t = ENCLOS(t))
	pos--;
    if (pos != 2)
	error(_("invalid '%s' argument"), "pos");
    else {
	PROTECT(s = ENCLOS(t));
	x = ENCLOS(s);
	SET_ENCLOS(t, x);
	isSpecial = IS_USER_DATABASE(s);
	if(isSpecial) {
	    R_ObjectTable *tb = (R_ObjectTable*) R_ExternalPtrAddr(HASHTAB(s));
	    if(tb->onDetach) tb->onDetach(tb);
	}

	SET_ENCLOS(s, R_BaseEnv);
    }

    if(!isSpecial) {
	R_FlushGlobalCacheFromTable(HASHTAB(s));
	MARK_AS_LOCAL_FRAME(s);
    } else {
	R_FlushGlobalCacheFromUserTable(HASHTAB(s));
	MARK_AS_LOCAL_FRAME(s); /* was _GLOBAL_ prior to 2.4.0 */
    }

    UNPROTECT(1);
    return s;
}



/*----------------------------------------------------------------------

  do_search

  Print out the current search path.

*/

static SEXP do_search(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP ans, name, t;
    int i, n;

    checkArity(op, args);
    n = 2;
    for (t = ENCLOS(R_GlobalEnv); t != R_BaseEnv ; t = ENCLOS(t))
	n++;
    PROTECT(ans = allocVector(STRSXP, n));
    /* TODO - what should the name of this be? */
    SET_STRING_ELT(ans, 0, mkChar(".GlobalEnv"));
    SET_STRING_ELT(ans, n-1, mkChar("package:base"));
    i = 1;
    for (t = ENCLOS(R_GlobalEnv); t != R_BaseEnv ; t = ENCLOS(t)) {
	name = getAttrib(t, R_NameSymbol);
	if (!isString(name) || length(name) < 1)
	    SET_STRING_ELT(ans, i, mkChar("(unknown)"));
	else
	    SET_STRING_ELT(ans, i, STRING_ELT(name, 0));
	i++;
    }
    UNPROTECT(1);
    return ans;
}


/*----------------------------------------------------------------------

  do_ls

  This code implements the functionality of the "ls" and "objects"
  functions.  [ ls(envir, all.names) ]

*/

static int FrameSize(SEXP frame, int all)
{
    int count = 0;

    while (frame != R_NilValue) {
	if ((all || CHAR(PRINTNAME(TAG(frame)))[0] != '.') &&
				      CAR(frame) != R_UnboundValue)
	    count += 1;
	frame = CDR(frame);
    }
    return count;
}

static void FrameNames(SEXP frame, int all, SEXP names, int *indx)
{
    while (frame != R_NilValue) {
	if ((all || CHAR(PRINTNAME(TAG(frame)))[0] != '.') &&
				      CAR(frame) != R_UnboundValue) {
	    SET_STRING_ELT(names, *indx, PRINTNAME(TAG(frame)));
	    (*indx)++;
	}
	frame = CDR(frame);
    }
}

static void FrameValues(SEXP frame, int all, SEXP values, int *indx)
{
    while (frame != R_NilValue) {
	if ((all || CHAR(PRINTNAME(TAG(frame)))[0] != '.') &&
				      CAR(frame) != R_UnboundValue) {
	    SEXP value = CAR(frame);
	    if (TYPEOF(value) == PROMSXP)
		value = forcePromise(value);
	    SET_VECTOR_ELT(values, *indx, duplicate(value));
	    (*indx)++;
	}
	frame = CDR(frame);
    }
}

static int HashTableSize(SEXP table, int all)
{
    int count = 0;
    int n = length(table);
    int i;
    for (i = 0; i < n; i++)
	count += FrameSize(VECTOR_ELT(table, i), all);
    return count;
}

static void HashTableNames(SEXP table, int all, SEXP names, int *indx)
{
    int n = length(table);
    int i;
    for (i = 0; i < n; i++)
	FrameNames(VECTOR_ELT(table, i), all, names, indx);
}

static void HashTableValues(SEXP table, int all, SEXP values, int *indx)
{
    int n = length(table);
    int i;
    for (i = 0; i < n; i++)
	FrameValues(VECTOR_ELT(table, i), all, values, indx);
}

#define NOT_IN_SYMBOL_TABLE(s) \
    (s == R_MissingArg || s == R_MissingUnder || s == R_RestartToken)

static int BuiltinSize(int all, int intern)
{
    sggc_cptr_t nxt;
    int count = 0;

    for (nxt = sggc_first_uncollected_of_kind(SGGC_SYM_KIND);
         nxt != SGGC_NO_OBJECT;
         nxt = sggc_next_uncollected_of_kind(nxt)) {
        SEXP s = SEXP_FROM_CPTR(nxt);
        if (NOT_IN_SYMBOL_TABLE(s)) continue;
        if (intern) {
            if (INTERNAL(s) != R_NilValue)
                count++;
        }
        else {
            if ((all || CHAR(PRINTNAME(s))[0] != '.')
                && SYMVALUE(s) != R_UnboundValue)
                count++;
        }
    }
    return count;
}

static void
BuiltinNames(int all, int intern, SEXP names, int *indx)
{
    sggc_cptr_t nxt;

    for (nxt = sggc_first_uncollected_of_kind(SGGC_SYM_KIND);
         nxt != SGGC_NO_OBJECT;
         nxt = sggc_next_uncollected_of_kind(nxt)) {
        SEXP s = SEXP_FROM_CPTR(nxt);
        if (NOT_IN_SYMBOL_TABLE(s)) continue;
        if (intern) {
            if (INTERNAL(s) != R_NilValue)
                SET_STRING_ELT(names, (*indx)++, PRINTNAME(s));
        }
        else {
            if ((all || CHAR(PRINTNAME(s))[0] != '.')
                && SYMVALUE(s) != R_UnboundValue)
                SET_STRING_ELT(names, (*indx)++, PRINTNAME(s));
        }
    }
}

static void
BuiltinValues(int all, int intern, SEXP values, int *indx)
{
    sggc_cptr_t nxt;

    for (nxt = sggc_first_uncollected_of_kind(SGGC_SYM_KIND);
         nxt != SGGC_NO_OBJECT;
         nxt = sggc_next_uncollected_of_kind(nxt)) {
        SEXP s = SEXP_FROM_CPTR(nxt);
        if (NOT_IN_SYMBOL_TABLE(s)) continue;
        SEXP vl;
        if (intern) {
            if (INTERNAL(s) != R_NilValue) {
                vl = SYMVALUE(s);
                if (TYPEOF(vl) == PROMSXP)
                    vl = forcePromise(vl);
                SET_VECTOR_ELT(values, (*indx)++, duplicate(vl));
            }
        }
        else {
            if ((all || CHAR(PRINTNAME(s))[0] != '.')
                && SYMVALUE(s) != R_UnboundValue) {
                vl = SYMVALUE(s);
                if (TYPEOF(vl) == PROMSXP)
                    vl = forcePromise(vl);
                SET_VECTOR_ELT(values, (*indx)++, duplicate(vl));
            }
        }
    }
}

static SEXP do_ls(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP env;
    int all;
    checkArity(op, args);

    if(IS_USER_DATABASE(CAR(args))) {
	R_ObjectTable *tb = (R_ObjectTable*)
	    R_ExternalPtrAddr(HASHTAB(CAR(args)));
	return(tb->objects(tb));
    }

    env = CAR(args);

    /* if (env == R_BaseNamespace) env = R_BaseEnv; */

    all = asLogical(CADR(args));
    if (all == NA_LOGICAL) all = 0;

    return R_lsInternal(env, all);
}

/* takes an environment and a boolean indicating whether to get all names */
SEXP R_lsInternal(SEXP env, Rboolean all)
{
    int  k;
    SEXP ans;


    /* Step 1 : Compute the Vector Size */
    k = 0;
    if (env == R_BaseEnv || env == R_BaseNamespace)
	k += BuiltinSize(all, 0);
    else if (isEnvironment(env) ||
	isEnvironment(env = simple_as_environment(env))) {
	if (HASHTAB(env) != R_NilValue)
	    k += HashTableSize(HASHTAB(env), all);
	else
	    k += FrameSize(FRAME(env), all);
    }
    else
	error(_("invalid '%s' argument"), "envir");

    /* Step 2 : Allocate and Fill the Result */
    PROTECT(ans = allocVector(STRSXP, k));
    k = 0;

    if (IS_BASE(env))
        BuiltinNames(all, 0, ans, &k);
    else if (HASHTAB(env) != R_NilValue)
        HashTableNames(HASHTAB(env), all, ans, &k);
    else
        FrameNames(FRAME(env), all, ans, &k);

    sortVector(ans, FALSE);
    UNPROTECT(1);
    return ans;
}

/* transform an environment into a named list */

static SEXP do_env2list(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP env, ans, names;
    int k, all;

    checkArity(op, args);

    env = CAR(args);
    if (env == R_NilValue)
	error(_("use of NULL environment is defunct"));
    if( !isEnvironment(env) ) {
        SEXP xdata;
	if( IS_S4_OBJECT(env) && TYPEOF(env) == S4SXP &&
	    (xdata = R_getS4DataSlot(env, ENVSXP)) != R_NilValue)
	    env = xdata;
	else
	    error(_("argument must be an environment"));
    }

    all = asLogical(CADR(args)); /* all.names = TRUE/FALSE */
    if (all == NA_LOGICAL) all = 0;

    if (IS_BASE(env))
	k = BuiltinSize(all, 0);
    else if (HASHTAB(env) != R_NilValue)
	k = HashTableSize(HASHTAB(env), all);
    else
	k = FrameSize(FRAME(env), all);

    PROTECT(names = allocVector(STRSXP, k));
    PROTECT(ans = allocVector(VECSXP, k));

    k = 0;
    if (IS_BASE(env))
	BuiltinValues(all, 0, ans, &k);
    else if (HASHTAB(env) != R_NilValue)
	HashTableValues(HASHTAB(env), all, ans, &k);
    else
	FrameValues(FRAME(env), all, ans, &k);

    k = 0;
    if (IS_BASE(env))
	BuiltinNames(all, 0, names, &k);
    else if (HASHTAB(env) != R_NilValue)
	HashTableNames(HASHTAB(env), all, names, &k);
    else
	FrameNames(FRAME(env), all, names, &k);

    setAttrib(ans, R_NamesSymbol, names);
    UNPROTECT(2);
    return(ans);
}

/*
 * apply a function to all objects in an environment and return the
 * results in a list.
 * Equivalent to lapply(as.list(env, all.names=all.names), FUN, ...)
 */
/* This is a special .Internal */
static SEXP do_eapply(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP env, ans, R_fcall, FUN, tmp2, End;
    int i, k, k2;
    int all, useNms, no_dots;

    checkArity(op, args);

    PROTECT(env = eval(CAR(args), rho));
    if (env == R_NilValue)
	error(_("use of NULL environment is defunct"));
    if( !isEnvironment(env) )
	error(_("argument must be an environment"));

    FUN = CADR(args);
    if (!isSymbol(FUN))
	error(_("arguments must be symbolic"));

    SEXP dotsv = findVarInFrame3 (rho, R_DotsSymbol, 3);
    no_dots = dotsv==R_MissingArg || dotsv==R_NilValue || dotsv==R_UnboundValue;

    /* 'all.names' : */
    all = asLogical(eval(CADDR(args), rho));
    if (all == NA_LOGICAL) all = 0;

    /* 'USE.NAMES' : */
    useNms = asLogical(eval(CADDDR(args), rho));
    if (useNms == NA_LOGICAL) useNms = 0;

    if (IS_BASE(env))
	k = BuiltinSize(all, 0);
    else if (HASHTAB(env) != R_NilValue)
	k = HashTableSize(HASHTAB(env), all);
    else
	k = FrameSize(FRAME(env), all);

    PROTECT(ans  = allocVector(VECSXP, k));
    PROTECT(tmp2 = allocVector(VECSXP, k));

    k2 = 0;
    if (IS_BASE(env))
	BuiltinValues(all, 0, tmp2, &k2);
    else if (HASHTAB(env) != R_NilValue)
	HashTableValues(HASHTAB(env), all, tmp2, &k2);
    else
	FrameValues(FRAME(env), all, tmp2, &k2);

    /* fcall :=  <FUN>( `[`(<elist>, i), tmp, ... ), with ... maybe omitted 

       Don't try to reuse the cell holding the index - causes problems. */

    PROTECT(End = no_dots ? R_NilValue : CONS(R_DotsSymbol,R_NilValue));

    for(i = 0; i < k2; i++) {
        PROTECT(R_fcall = LCONS(FUN, 
                            CONS(LCONS(R_Bracket2Symbol,
                                     CONS(tmp2, 
                                        CONS(ScalarInteger(i+1), R_NilValue))),
                                 End)));
        SET_VECTOR_ELEMENT_TO_VALUE (ans, i, eval(R_fcall, rho));
        UNPROTECT(1);
    }

    if (useNms) {
	SEXP names;
	PROTECT(names = allocVector(STRSXP, k));
	k = 0;
	if (IS_BASE(env))
	    BuiltinNames(all, 0, names, &k);
	else if(HASHTAB(env) != R_NilValue)
	    HashTableNames(HASHTAB(env), all, names, &k);
	else
	    FrameNames(FRAME(env), all, names, &k);

	setAttrib(ans, R_NamesSymbol, names);
	UNPROTECT(1);
    }
    UNPROTECT(4);
    return(ans);
}

int envlength(SEXP rho)
{
    if( HASHTAB(rho) != R_NilValue)
	return HashTableSize(HASHTAB(rho), 1);
    else
	return FrameSize(FRAME(rho), 1);
}

/*----------------------------------------------------------------------

  do_builtins

  Return the names of all the built in functions.  These are fetched
  directly from the symbol table.

*/

static SEXP do_builtins(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP ans;
    int intern, nelts;
    checkArity(op, args);
    intern = asLogical(CAR(args));
    if (intern == NA_INTEGER) intern = 0;
    nelts = BuiltinSize(1, intern);
    ans = allocVector(STRSXP, nelts);
    nelts = 0;
    BuiltinNames(1, intern, ans, &nelts);
    sortVector(ans, TRUE);
    return ans;
}


/*----------------------------------------------------------------------

  do_pos2env

  This function returns the environment at a specified position in the
  search path or the environment of the caller of
  pos.to.env (? but pos.to.env is usually used in arg lists and hence
  is evaluated in the calling environment so this is one higher).

  When pos = -1 the environment of the closure that pos2env is
  evaluated in is obtained. Note: this relies on pos.to.env being
  a primitive.

 */
static SEXP pos2env(int pos, SEXP call)
{
    SEXP env;
    RCNTXT *cptr;

    if (pos == NA_INTEGER || pos < -1 || pos == 0)
	errorcall(call, _("invalid '%s' argument"), "pos");
    else if (pos == -1) {
	/* make sure the context is a funcall */
	cptr = R_GlobalContext;
	while( !(cptr->callflag & CTXT_FUNCTION) && cptr->nextcontext
	       != NULL )
	    cptr = cptr->nextcontext;
	if( !(cptr->callflag & CTXT_FUNCTION) )
	    errorcall(call, _("no enclosing environment"));

	env = cptr->sysparent;
	if (R_GlobalEnv != R_NilValue && env == R_NilValue)
	    errorcall(call, _("invalid '%s' argument"), "pos");
    }
    else {
	for (env = R_GlobalEnv; env != R_EmptyEnv && pos > 1;
	     env = ENCLOS(env))
	    pos--;
	if (pos != 1)
	    errorcall(call, _("invalid '%s' argument"), "pos");
    }
    return env;
}

/* this is primitive */
static SEXP do_pos2env(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP env, pos;
    int i, npos;
    checkArity(op, args);
    check1arg_x (args, call);

    PROTECT(pos = coerceVector(CAR(args), INTSXP));
    npos = length(pos);
    if (npos <= 0)
	errorcall(call, _("invalid '%s' argument"), "pos");
    PROTECT(env = allocVector(VECSXP, npos));
    for (i = 0; i < npos; i++) {
	SET_VECTOR_ELT(env, i, pos2env(INTEGER(pos)[i], call));
    }
    if (npos == 1) env = VECTOR_ELT(env, 0);
    UNPROTECT(2);
    return env;
}

static SEXP matchEnvir(SEXP call, const char *what)
{
    SEXP t, name;
    if(!strcmp(".GlobalEnv", what))
	return R_GlobalEnv;
    if(!strcmp("package:base", what))
	return R_BaseEnv;
    for (t = ENCLOS(R_GlobalEnv); t != R_EmptyEnv ; t = ENCLOS(t)) {
	name = getAttrib(t, R_NameSymbol);
	if(isString(name) && length(name) > 0 &&
	   !strcmp(translateChar(STRING_ELT(name, 0)), what))
	    return t;
    }
    errorcall(call, _("no item called \"%s\" on the search list"), what);
    return R_NilValue;
}

/* This is primitive */
static SEXP do_as_environment(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP arg = CAR(args), ans;
    checkArity(op, args);
    check1arg(args, call, "object");
    if(isEnvironment(arg))
	return arg;
    if(isObject(arg) &&
       DispatchOrEval(call, op, "as.environment", args, rho, &ans, 0, 1))
	return ans;
    switch(TYPEOF(arg)) {
    case STRSXP:
	return matchEnvir(call, translateChar(asChar(arg)));
    case REALSXP:
    case INTSXP:
	return do_pos2env(call, op, args, rho);
    case NILSXP:
	errorcall(call,_("using 'as.environment(NULL)' is defunct"));
    case S4SXP: {
	/* dispatch was tried above already */
	SEXP dot_xData = R_getS4DataSlot(arg, ENVSXP);
	if(!isEnvironment(dot_xData))
	    errorcall(call, _("S4 object does not extend class \"environment\""));
	else
	    return(dot_xData);
    }
    case VECSXP: {
	/* implement as.environment.list() {isObject(.) is false for a list} */
	SEXP call, val;
	PROTECT(call = lang4(install("list2env"), arg,
			     /* envir = */R_NilValue,
			     /* parent = */R_EmptyEnv));
	val = eval(call, rho);
	UNPROTECT(1);
	return val;
    }
    default:
	errorcall(call, _("invalid object for 'as.environment'"));
    }
}

void R_LockEnvironment(SEXP env, Rboolean bindings)
{
    if(IS_S4_OBJECT(env) && (TYPEOF(env) == S4SXP))
	env = R_getS4DataSlot(env, ANYSXP); /* better be an ENVSXP */

    if (TYPEOF(env) != ENVSXP)
	error(_("not an environment"));

    if (IS_BASE(env)) {
	if (bindings) {
            sggc_cptr_t nxt;
            for (nxt = sggc_first_uncollected_of_kind(SGGC_SYM_KIND);
                 nxt != SGGC_NO_OBJECT;
                 nxt = sggc_next_uncollected_of_kind(nxt)) {
                SEXP s = SEXP_FROM_CPTR(nxt);
                if (NOT_IN_SYMBOL_TABLE(s)) continue;
                if (SYMVALUE(s) != R_UnboundValue)
                    LOCK_BINDING(s);
            }
	}
#ifdef NOT_YET
	/* causes problems with Matrix */
	LOCK_FRAME(env);
#endif
	return;
    }

    if (bindings) {
	if (IS_HASHED(env)) {
	    SEXP table, chain;
	    int i, size;
	    table = HASHTAB(env);
	    size = HASHSIZE(table);
	    for (i = 0; i < size; i++)
		for (chain = VECTOR_ELT(table, i);
		     chain != R_NilValue;
		     chain = CDR(chain))
		    LOCK_BINDING(chain);
	}
	else {
	    SEXP frame;
	    for (frame = FRAME(env); frame != R_NilValue; frame = CDR(frame))
		LOCK_BINDING(frame);
	}
    }
    LOCK_FRAME(env);
}

Rboolean R_EnvironmentIsLocked(SEXP env)
{
    if (TYPEOF(env) == NILSXP)
	error(_("use of NULL environment is defunct"));
    if (TYPEOF(env) != ENVSXP &&
	TYPEOF((env = simple_as_environment(env))) != ENVSXP)
	error(_("not an environment"));
    return FRAME_IS_LOCKED(env) != 0;
}

static SEXP do_lockEnv(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP frame;
    Rboolean bindings;
    checkArity(op, args);
    frame = CAR(args);
    bindings = asLogical(CADR(args));
    R_LockEnvironment(frame, bindings);
    return R_NilValue;
}

static SEXP do_envIsLocked(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    checkArity(op, args);
    return ScalarLogicalMaybeConst(R_EnvironmentIsLocked(CAR(args)));
}

void R_LockBinding(SEXP sym, SEXP env)
{
    if (TYPEOF(sym) != SYMSXP)
	error(_("not a symbol"));
    if (TYPEOF(env) == NILSXP)
	error(_("use of NULL environment is defunct"));
    if (TYPEOF(env) != ENVSXP &&
	TYPEOF((env = simple_as_environment(env))) != ENVSXP)
	error(_("not an environment"));
    if (IS_BASE(env))
	/* It is a symbol, so must have a binding even if it is
	   R_UnboundSymbol */
	LOCK_BINDING(sym);
    else {
	SEXP binding = findVarLocInFrame(env, sym, NULL);
	if (binding == R_NilValue)
	    error(_("no binding for \"%s\""), CHAR(PRINTNAME(sym)));
	LOCK_BINDING(binding);
    }
}

void R_unLockBinding(SEXP sym, SEXP env)
{
    if (TYPEOF(sym) != SYMSXP)
	error(_("not a symbol"));
    if (TYPEOF(env) == NILSXP)
	error(_("use of NULL environment is defunct"));
    if (TYPEOF(env) != ENVSXP &&
	TYPEOF((env = simple_as_environment(env))) != ENVSXP)
	error(_("not an environment"));
    if (IS_BASE(env))
	/* It is a symbol, so must have a binding even if it is
	   R_UnboundSymbol */
	UNLOCK_BINDING(sym);
    else {
	SEXP binding = findVarLocInFrame(env, sym, NULL);
	if (binding == R_NilValue)
	    error(_("no binding for \"%s\""), CHAR(PRINTNAME(sym)));
	UNLOCK_BINDING(binding);
    }
}

void R_MakeActiveBinding(SEXP sym, SEXP fun, SEXP env)
{
    if (TYPEOF(sym) != SYMSXP)
	error(_("not a symbol"));
    if (! isFunction(fun))
	error(_("not a function"));
    if (TYPEOF(env) == NILSXP)
	error(_("use of NULL environment is defunct"));
    if (TYPEOF(env) != ENVSXP &&
	TYPEOF((env = simple_as_environment(env))) != ENVSXP)
	error(_("not an environment"));
    if (IS_BASE(env)) {
	if (SYMVALUE(sym) != R_UnboundValue && ! IS_ACTIVE_BINDING(sym))
	    error(_("symbol already has a regular binding"));
	else if (BINDING_IS_LOCKED(sym))
	    error(_("cannot change active binding if binding is locked"));
	SET_SYMVALUE(sym, fun);
	SET_ACTIVE_BINDING_BIT(sym);
	/* we don't need to worry about the global cache here as
	   a regular binding cannot be changed */
    }
    else {
	SEXP binding = findVarLocInFrame(env, sym, NULL);
	if (binding == R_NilValue) {
	    defineVar(sym, fun, env); /* fails if env is locked */
	    binding = findVarLocInFrame(env, sym, NULL);
	    SET_ACTIVE_BINDING_BIT(binding);
	}
	else if (! IS_ACTIVE_BINDING(binding))
	    error(_("symbol already has a regular binding"));
	else if (BINDING_IS_LOCKED(binding))
	    error(_("cannot change active binding if binding is locked"));
	else
	    SETCAR(binding, fun);
    }
}

Rboolean R_BindingIsLocked(SEXP sym, SEXP env)
{
    if (TYPEOF(sym) != SYMSXP)
	error(_("not a symbol"));
    if (TYPEOF(env) == NILSXP)
	error(_("use of NULL environment is defunct"));
    if (TYPEOF(env) != ENVSXP &&
	TYPEOF((env = simple_as_environment(env))) != ENVSXP)
	error(_("not an environment"));
    if (IS_BASE(env))
	/* It is a symbol, so must have a binding even if it is
	   R_UnboundSymbol */
	return BINDING_IS_LOCKED(sym) != 0;
    else {
	SEXP binding = findVarLocInFrame(env, sym, NULL);
	if (binding == R_NilValue)
	    error(_("no binding for \"%s\""), CHAR(PRINTNAME(sym)));
	return BINDING_IS_LOCKED(binding) != 0;
    }
}

Rboolean R_BindingIsActive(SEXP sym, SEXP env)
{
    if (TYPEOF(sym) != SYMSXP)
	error(_("not a symbol"));
    if (TYPEOF(env) == NILSXP)
	error(_("use of NULL environment is defunct"));
    if (TYPEOF(env) != ENVSXP &&
	TYPEOF((env = simple_as_environment(env))) != ENVSXP)
	error(_("not an environment"));
    if (IS_BASE(env))
	/* It is a symbol, so must have a binding even if it is
	   R_UnboundSymbol */
	return IS_ACTIVE_BINDING(sym) != 0;
    else {
	SEXP binding = findVarLocInFrame(env, sym, NULL);
	if (binding == R_NilValue)
	    error(_("no binding for \"%s\""), CHAR(PRINTNAME(sym)));
	return IS_ACTIVE_BINDING(binding) != 0;
    }
}

Rboolean R_HasFancyBindings(SEXP rho)
{
    if (IS_HASHED(rho)) {
	SEXP table, chain;
	int i, size;

	table = HASHTAB(rho);
	size = HASHSIZE(table);
	for (i = 0; i < size; i++)
	    for (chain = VECTOR_ELT(table, i);
		 chain != R_NilValue;
		 chain = CDR(chain))
		if (IS_ACTIVE_BINDING(chain) || BINDING_IS_LOCKED(chain))
		    return TRUE;
	return FALSE;
    }
    else {
	SEXP frame;

	for (frame = FRAME(rho); frame != R_NilValue; frame = CDR(frame))
	    if (IS_ACTIVE_BINDING(frame) || BINDING_IS_LOCKED(frame))
		return TRUE;
	return FALSE;
    }
}

static SEXP do_lockBnd(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP sym, env;
    checkArity(op, args);
    sym = CAR(args);
    env = CADR(args);
    switch(PRIMVAL(op)) {
    case 0:
	R_LockBinding(sym, env);
	break;
    case 1:
	R_unLockBinding(sym, env);
	break;
    default:
	error(_("unknown op"));
    }
    return R_NilValue;
}

static SEXP do_bndIsLocked(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP sym, env;
    checkArity(op, args);
    sym = CAR(args);
    env = CADR(args);
    return ScalarLogicalMaybeConst(R_BindingIsLocked(sym, env));
}

static SEXP do_mkActiveBnd(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP sym, fun, env;
    checkArity(op, args);
    sym = CAR(args);
    fun = CADR(args);
    env = CADDR(args);
    R_MakeActiveBinding(sym, fun, env);
    return R_NilValue;
}

static SEXP do_bndIsActive(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP sym, env;
    checkArity(op, args);
    sym = CAR(args);
    env = CADR(args);
    return ScalarLogicalMaybeConst(R_BindingIsActive(sym, env));
}

/* This is a .Internal with no wrapper, currently unused in base R */
static SEXP do_mkUnbound(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP sym;
    checkArity(op, args);
    sym = CAR(args);

    if (TYPEOF(sym) != SYMSXP) error(_("not a symbol"));
    /* This is not quite the same as SET_SYMBOL_BINDING_VALUE as it
       does not allow active bindings to be unbound */
    if (R_BindingIsLocked(sym, R_BaseEnv))
	error(_("cannot unbind a locked binding"));
    if (R_BindingIsActive(sym, R_BaseEnv))
	error(_("cannot unbind an active binding"));
    SET_SYMVALUE(sym, R_UnboundValue);

    R_FlushGlobalCache(sym);

    return R_NilValue;
}

void R_RestoreHashCount(SEXP rho)
{
    if (IS_HASHED(rho)) {
	SEXP table;
	int i, count, size;

	table = HASHTAB(rho);
	size = HASHSIZE(table);
	for (i = 0, count = 0; i < size; i++)
	    if (VECTOR_ELT(table, i) != R_NilValue)
		count++;
	SET_HASHSLOTSUSED(table, count);
    }
}

Rboolean R_IsPackageEnv(SEXP rho)
{
    if (TYPEOF(rho) == ENVSXP) {
	SEXP name = getAttrib(rho, R_NameSymbol);
	char *packprefix = "package:";
	int pplen = strlen(packprefix);
	if(isString(name) && length(name) > 0 &&
	   ! strncmp(packprefix, CHAR(STRING_ELT(name, 0)), pplen)) /* ASCII */
	    return TRUE;
	else
	    return FALSE;
    }
    else
	return FALSE;
}

SEXP R_PackageEnvName(SEXP rho)
{
    if (TYPEOF(rho) == ENVSXP) {
	SEXP name = getAttrib(rho, R_NameSymbol);
	char *packprefix = "package:";
	int pplen = strlen(packprefix);
	if(isString(name) && length(name) > 0 &&
	   ! strncmp(packprefix, CHAR(STRING_ELT(name, 0)), pplen)) /* ASCII */
	    return name;
	else
	    return R_NilValue;
    }
    else
	return R_NilValue;
}

SEXP R_FindPackageEnv(SEXP info)
{
    SEXP expr, val;
    SEXP findPackageEnv_install = install("findPackageEnv");
    PROTECT(info);
    PROTECT(expr = LCONS(findPackageEnv_install, CONS(info, R_NilValue)));
    val = eval(expr, R_GlobalEnv);
    UNPROTECT(2);
    return val;
}

Rboolean R_IsNamespaceEnv(SEXP rho)
{
    if (rho == R_BaseNamespace)
	return TRUE;
    else if (TYPEOF(rho) == ENVSXP) {
	SEXP info = findVarInFrame3(rho, install(".__NAMESPACE__."), TRUE);
	if (info != R_UnboundValue && TYPEOF(info) == ENVSXP) {
            PROTECT(info);
	    SEXP spec = findVarInFrame3(info, install("spec"), TRUE);
            UNPROTECT(1);
	    if (spec != R_UnboundValue &&
		TYPEOF(spec) == STRSXP && LENGTH(spec) > 0)
		return TRUE;
	    else
		return FALSE;
	}
	else return FALSE;
    }
    else return FALSE;
}

static SEXP do_isNSEnv(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    checkArity(op, args);
    return R_IsNamespaceEnv(CAR(args)) ? mkTrue() : mkFalse();
}

SEXP R_NamespaceEnvSpec(SEXP rho)
{
    /* The namespace spec is a character vector that specifies the
       namespace.  The first element is the namespace name.  The
       second element, if present, is the namespace version.  Further
       elements may be added later. */
    if (rho == R_BaseNamespace)
	return R_BaseNamespaceName;
    else if (TYPEOF(rho) == ENVSXP) {
	SEXP info = findVarInFrame3(rho, install(".__NAMESPACE__."), TRUE);
	if (info != R_UnboundValue && TYPEOF(info) == ENVSXP) {
            PROTECT(info);
	    SEXP spec = findVarInFrame3(info, install("spec"), TRUE);
            UNPROTECT(1);
	    if (spec != R_UnboundValue &&
		TYPEOF(spec) == STRSXP && LENGTH(spec) > 0)
		return spec;
	    else
		return R_NilValue;
	}
	else return R_NilValue;
    }
    else return R_NilValue;
}

SEXP R_FindNamespace(SEXP info)
{
    SEXP expr, val;
    SEXP getNamespace_install = install("getNamespace");
    PROTECT(info);
    PROTECT(expr = LCONS(getNamespace_install, CONS(info, R_NilValue)));
    val = eval(expr, R_GlobalEnv);
    UNPROTECT(2);
    return val;
}

static SEXP checkNSname(SEXP call, SEXP name)
{
    switch (TYPEOF(name)) {
    case SYMSXP:
	break;
    case STRSXP:
	if (LENGTH(name) >= 1) {
	    name = install(translateChar(STRING_ELT(name, 0)));
	    break;
	}
	/* else fall through */
    default:
	errorcall(call, _("bad namespace name"));
    }
    return name;
}

static SEXP do_regNS(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP name, val;
    checkArity(op, args);
    name = checkNSname(call, CAR(args));
    val = CADR(args);
    if (findVarInFrame(R_NamespaceRegistry, name) != R_UnboundValue)
	errorcall(call, _("namespace already registered"));
    defineVar(name, val, R_NamespaceRegistry);
    return R_NilValue;
}

static SEXP do_unregNS(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP name;
    checkArity(op, args);
    name = checkNSname(call, CAR(args));
    if (findVarInFrame(R_NamespaceRegistry, name) == R_UnboundValue)
	errorcall(call, _("namespace not registered"));
    RemoveVariable(name, R_NamespaceRegistry);
    return R_NilValue;
}

static SEXP do_getRegNS(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP name, val;
    checkArity(op, args);
    name = checkNSname(call, CAR(args));
    val = findVarInFrame(R_NamespaceRegistry, name);
    if (val == R_UnboundValue)
	return R_NilValue;
    else
	return val;
}

static SEXP do_getNSRegistry(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    checkArity(op, args);
    return R_NamespaceRegistry;
}

static SEXP do_importIntoEnv(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    /* This function copies values of variables from one environment
       to another environment, possibly with different names.
       Promises are not forced and active bindings are preserved. */
    SEXP impenv, impnames, expenv, expnames;
    SEXP impsym, expsym, val;
    int i, n;

    checkArity(op, args);

    impenv = CAR(args); args = CDR(args);
    impnames = CAR(args); args = CDR(args);
    expenv = CAR(args); args = CDR(args);
    expnames = CAR(args); args = CDR(args);

    if (TYPEOF(impenv) == NILSXP)
	error(_("use of NULL environment is defunct"));
    if (TYPEOF(impenv) != ENVSXP && 
	TYPEOF((impenv = simple_as_environment(impenv))) != ENVSXP)
	error(_("bad import environment argument"));
    if (TYPEOF(expenv) == NILSXP)
	error(_("use of NULL environment is defunct"));
    if (TYPEOF(expenv) != ENVSXP &&
	TYPEOF((expenv = simple_as_environment(expenv))) != ENVSXP)
	error(_("bad export environment argument"));
    if (TYPEOF(impnames) != STRSXP || TYPEOF(expnames) != STRSXP)
	error(_("invalid '%s' argument"), "names");
    if (LENGTH(impnames) != LENGTH(expnames))
	error(_("length of import and export names must match"));

    n = LENGTH(impnames);
    for (i = 0; i < n; i++) {
	impsym = install(translateChar(STRING_ELT(impnames, i)));
	expsym = install(translateChar(STRING_ELT(expnames, i)));

	/* find the binding--may be a CONS cell or a symbol */
	SEXP binding = R_NilValue;
	for (SEXP env = expenv;
	     env != R_EmptyEnv && binding == R_NilValue;
	     env = ENCLOS(env))
	    if (env == R_BaseNamespace) {
		if (SYMVALUE(expsym) != R_UnboundValue)
		    binding = expsym;
	    } else
		binding = findVarLocInFrame(env, expsym, NULL);
	if (binding == R_NilValue)
	    binding = expsym;

	/* get value of the binding; do not force promises */
	if (TYPEOF(binding) == SYMSXP) {
	    if (SYMVALUE(expsym) == R_UnboundValue)
		error(_("exported symbol '%s' has no value"),
		      CHAR(PRINTNAME(expsym)));
	    val = SYMVALUE(expsym);
	}
	else val = CAR(binding);

	/* import the binding */
	if (IS_ACTIVE_BINDING(binding))
	    R_MakeActiveBinding(impsym, val, impenv);
	/* This is just a tiny optimization */
	else if (IS_BASE(impenv))
	    gsetVar(impsym, val, impenv);
	else
	    defineVar(impsym, val, impenv);
    }
    return R_NilValue;
}


static SEXP do_envprofile(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    /* Return a list containing profiling information given a hashed
       environment.  For non-hashed environments, this function
       returns R_NilValue.  This seems appropriate since there is no
       way to test whether an environment is hashed at the R level.
    */
    SEXP env = CAR(args);
    if (isEnvironment(env))
	return IS_HASHED(env) ? R_HashProfile(HASHTAB(env)) : R_NilValue;
    else
	error("argument must be a hashed environment");
}

/* FUNTAB entries defined in this source file. See names.c for documentation. */

attribute_hidden FUNTAB R_FunTab_envir[] =
{
/* printname	c-entry		offset	eval	arity	pp-kind	     precedence	rightassoc */

{"assign",	do_assign,	0,	111,	4,	{PP_FUNCALL, PREC_FN,	0}},
{"list2env",	do_list2env,	0,	11,	2,	{PP_FUNCALL, PREC_FN,	0}},
{"remove",	do_remove,	0,	111,	3,	{PP_FUNCALL, PREC_FN,	0}},
{"get_rm",	do_get_rm,	0,	1000,	1,	{PP_FUNCALL, PREC_FN,	0}},
{"get",		do_get,		1,	11,	4,	{PP_FUNCALL, PREC_FN,	0}},
{"exists",	do_get,		0,	11,	4,	{PP_FUNCALL, PREC_FN,	0}},
{"mget",	do_mget,	1,	11,	5,	{PP_FUNCALL, PREC_FN,	0}},
{"missing",	do_missing,	0,	0,	1,	{PP_FUNCALL, PREC_FN,	0}},
{"missing_from_underline",do_missing,1,	0,	1,	{PP_FUNCALL, PREC_FN,	0}},
{"globalenv",	do_globalenv,	0,	1,	0,	{PP_FUNCALL, PREC_FN,	0}},
{"baseenv",	do_baseenv,	0,	1,	0,	{PP_FUNCALL, PREC_FN,	0}},
{"emptyenv",	do_emptyenv,	0,	1,	0,	{PP_FUNCALL, PREC_FN,	0}},
{"attach",	do_attach,	0,	111,	3,	{PP_FUNCALL, PREC_FN,	0}},
{"detach",	do_detach,	0,	111,	1,	{PP_FUNCALL, PREC_FN,	0}},
{"search",	do_search,	0,	11,	0,	{PP_FUNCALL, PREC_FN,	0}},
{"ls",		do_ls,		1,	11,	2,	{PP_FUNCALL, PREC_FN,	0}},
{"env2list",	do_env2list,	0,	11,	2,	{PP_FUNCALL, PREC_FN,	0}},
{"eapply",	do_eapply,	0,	10,	4,	{PP_FUNCALL, PREC_FN,	0}},
{"builtins",	do_builtins,	0,	11,	1,	{PP_FUNCALL, PREC_FN,	0}},
{"pos.to.env",	do_pos2env,	0,	1,	1,	{PP_FUNCALL, PREC_FN,	0}},
{"as.environment",do_as_environment,0,	1,	1,	{PP_FUNCALL, PREC_FN,	0}},
{"lockEnvironment", do_lockEnv,		0, 111,  2,      {PP_FUNCALL, PREC_FN,	0}},
{"environmentIsLocked",	do_envIsLocked,	0, 11,  1,      {PP_FUNCALL, PREC_FN,	0}},
{"lockBinding", do_lockBnd,		0, 111,	2,      {PP_FUNCALL, PREC_FN,	0}},
{"unlockBinding", do_lockBnd,		1, 111,	2,      {PP_FUNCALL, PREC_FN,	0}},
{"bindingIsLocked", do_bndIsLocked,	0, 11,	2,      {PP_FUNCALL, PREC_FN,	0}},
{"makeActiveBinding", do_mkActiveBnd,	0, 111,	3,      {PP_FUNCALL, PREC_FN,	0}},
{"bindingIsActive", do_bndIsActive,	0, 11,	2,      {PP_FUNCALL, PREC_FN,	0}},
{"mkUnbound",	do_mkUnbound,		0, 111,	1,      {PP_FUNCALL, PREC_FN,	0}},
{"isNamespaceEnv",do_isNSEnv,		0, 11,	1,      {PP_FUNCALL, PREC_FN,	0}},
{"registerNamespace",do_regNS,		0, 11,	2,      {PP_FUNCALL, PREC_FN,	0}},
{"unregisterNamespace",do_unregNS,	0, 11,  1,      {PP_FUNCALL, PREC_FN,	0}},
{"getRegisteredNamespace",do_getRegNS,	0, 11,  1,      {PP_FUNCALL, PREC_FN,	0}},
{"getNamespaceRegistry",do_getNSRegistry, 0, 11, 0,     {PP_FUNCALL, PREC_FN,	0}},
{"importIntoEnv",do_importIntoEnv, 0,	11,	4,	{PP_FUNCALL, PREC_FN,	0}},
{"env.profile",  do_envprofile,    0,	211,	1,	{PP_FUNCALL, PREC_FN,	0}},

{NULL,		NULL,		0,	0,	0,	{PP_INVALID, PREC_FN,	0}}
};
