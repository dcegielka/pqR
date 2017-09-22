/*
 *  pqR : A pretty quick version of R
 *  Copyright (C) 2013, 2014, 2017 by Radford M. Neal
 *
 *  Based on R : A Computer Language for Statistical Data Analysis
 *  Copyright (C) 1995, 1996  Robert Gentleman and Ross Ihaka
 *            (C) 2004  The R Foundation
 *  Copyright (C) 1998-2009 The R Core Team.
 *
 *  The changes in pqR from R-2.15.0 distributed by the R Core Team are
 *  documented in the NEWS and MODS files in the top-level source directory.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) anylater version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, a copy is available at
 *  http://www.r-project.org/Licenses/
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define USE_FAST_PROTECT_MACROS
#include "Defn.h"

#include <R_ext/RS.h> /* S4 bit */

/*  duplicate  -  object duplication  */

/*  Because we try to maintain the illusion of call by
 *  value, we often need to duplicate entire data
 *  objects.  There are a couple of points to note.
 *  First, duplication of list-like objects is done
 *  iteratively to prevent growth of the pointer
 *  protection stack, and second, the duplication of
 *  promises requires that the promises be forced and
 *  the value duplicated.  */

/* This macro pulls out the common code in copying an atomic vector. */

#define DUPLICATE_ATOMIC_VECTOR(type, fun, to, from) do {\
  int __n__ = LENGTH(from);\
  PROTECT(from); \
  PROTECT(to = allocVector(TYPEOF(from), __n__)); \
  if (__n__ == 1) fun(to)[0] = fun(from)[0]; \
  else { \
    int __i__; \
    type *__fp__ = fun(from), *__tp__ = fun(to); \
    memcpy (__tp__, __fp__, __n__ * sizeof(type)); \
  } \
  DUPLICATE_ATTRIB(to, from);		\
  SET_TRUELENGTH(to, TRUELENGTH(from)); \
  UNPROTECT(2); \
} while (0)

/* The following macros avoid the cost of going through calls to the
   assignment functions (and duplicate in the case of ATTRIB) when the
   ATTRIB or TAG value to be stored is R_NilValue, the value the field
   will have been set to by the allocation function */
#define DUPLICATE_ATTRIB(to, from) do {\
  SEXP __a__ = ATTRIB(from); \
  if (__a__ != R_NilValue) { \
    SET_ATTRIB(to, duplicate1(__a__)); \
    SET_OBJECT(to, OBJECT(from)); \
    if (IS_S4_OBJECT(from)) SET_S4_OBJECT(to); else UNSET_S4_OBJECT(to);  \
  } \
} while (0)

#define COPY_TAG(to, from) do { \
  SEXP __tag__ = TAG(from); \
  if (__tag__ != R_NilValue) SET_TAG(to, __tag__); \
} while (0)


/* For memory profiling.  */
/* We want a count of calls to duplicate from outside
   which requires a wrapper function.

   The original duplicate() function is now duplicate1().

   I don't see how to make the wrapper go away when R_PROFILING
   is not defined, because we still need to be able to
   optionally rename duplicate() as Rf_duplicate().
*/
static SEXP duplicate1(SEXP);

#ifdef R_PROFILING
static unsigned long duplicate_counter = (unsigned long)-1;

unsigned long  attribute_hidden
get_duplicate_counter(void)
{
    return duplicate_counter;
}

void attribute_hidden reset_duplicate_counter(void)
{
    duplicate_counter = 0;
    return;
}
#endif


/* Duplicate object.  The argument need not be protected by the caller (will
   be protected here if necessary).  Duplicate will also wait for it to be
   computed if necessary. */

SEXP duplicate(SEXP s){
    SEXP t;

#ifdef R_PROFILING
    duplicate_counter++;
#endif
    t = duplicate1(s);
    return t;
}

static SEXP duplicate1(SEXP s)
{
    SEXP h, t,  sp;
    int i, n;

    WAIT_UNTIL_COMPUTED(s);

    switch (TYPEOF(s)) {
    case NILSXP:
    case SYMSXP:
    case ENVSXP:
    case SPECIALSXP:
    case BUILTINSXP:
    case EXTPTRSXP:
    case BCODESXP:
    case WEAKREFSXP:
	return s;
    case CLOSXP:
	PROTECT(s);
	if (R_jit_enabled > 1 && TYPEOF(BODY(s)) != BCODESXP) {
	    int old_enabled = R_jit_enabled;
	    SEXP new_s;
	    R_jit_enabled = 0;
	    new_s = R_cmpfun(s);
	    SET_BODY(s, BODY(new_s));
	    R_jit_enabled = old_enabled;
	}
	PROTECT(t = allocSExp(CLOSXP));
	SET_FORMALS(t, FORMALS(s));
	SET_BODY(t, BODY(s));
	SET_CLOENV(t, CLOENV(s));
	DUPLICATE_ATTRIB(t, s);
	UNPROTECT(2);
	break;
    case LISTSXP:
    case LANGSXP:
    case DOTSXP:
	PROTECT(sp = s);
	PROTECT(h = t = CONS(R_NilValue, R_NilValue));
	while(sp != R_NilValue) {
	    SETCDR(t, CONS(duplicate1(CAR(sp)), R_NilValue));
	    t = CDR(t);
	    COPY_TAG(t, sp);
	    DUPLICATE_ATTRIB(t, sp);
	    sp = CDR(sp);
	}
	t = CDR(h);
	SET_TYPEOF(t, TYPEOF(s));
	UNPROTECT(2);
	break;
    case CHARSXP:
	return s;
	break;
    case EXPRSXP:
    case VECSXP:
	n = LENGTH(s);
	PROTECT(s);
	PROTECT(t = allocVector(TYPEOF(s), n));
	for(i = 0 ; i < n ; i++)
	    SET_VECTOR_ELT(t, i, duplicate1(VECTOR_ELT(s, i)));
	DUPLICATE_ATTRIB(t, s);
	SET_TRUELENGTH(t, TRUELENGTH(s));
	UNPROTECT(2);
	break;
    case LGLSXP:  DUPLICATE_ATOMIC_VECTOR(int, LOGICAL, t, s); break;
    case INTSXP:  DUPLICATE_ATOMIC_VECTOR(int, INTEGER, t, s); break;
    case REALSXP: DUPLICATE_ATOMIC_VECTOR(double, REAL, t, s); break;
    case CPLXSXP: DUPLICATE_ATOMIC_VECTOR(Rcomplex, COMPLEX, t, s); break;
    case RAWSXP:  DUPLICATE_ATOMIC_VECTOR(Rbyte, RAW, t, s); break;
    case STRSXP:
	/* direct copying and bypassing the write barrier is OK since
	   t was just allocated and so it cannot be older than any of
	   the elements in s.  LT */
	DUPLICATE_ATOMIC_VECTOR(SEXP, STRING_PTR, t, s);
	break;
    case PROMSXP:
	return s;
	break;
    case S4SXP:
	PROTECT(s);
	PROTECT(t = allocS4Object());
	DUPLICATE_ATTRIB(t, s);
	UNPROTECT(2);
	break;
    default:
	UNIMPLEMENTED_TYPE("duplicate", s);
	t = s;/* for -Wall */
    }
    if (TYPEOF(t) == TYPEOF(s) ) { /* surely it only makes sense in this case*/
	SET_OBJECT(t, OBJECT(s));
	if (IS_S4_OBJECT(s)) SET_S4_OBJECT(t); else UNSET_S4_OBJECT(t);
    }
    return t;
}


/* Set n elements of vector x (starting at i) to the appropriate NA value, or
   to R_NilValue for a VECSXP or EXPRSXP, or to 0 for a RAWSXP. */

void set_elements_to_NA_or_NULL (SEXP x, int i, int n)
{
    int ln = i + n;

    if (n == 0) return;

    switch (TYPEOF(x)) {
    case RAWSXP:
        do RAW(x)[i++] = 0; while (i<ln);
        break;
    case LGLSXP:
        do LOGICAL(x)[i++] = NA_LOGICAL; while (i<ln);
        break;
    case INTSXP:
        do INTEGER(x)[i++] = NA_INTEGER; while (i<ln);
        break;
    case REALSXP:
        do REAL(x)[i++] = NA_REAL; while (i<ln);
        break;
    case CPLXSXP:
        do { COMPLEX(x)[i].r = COMPLEX(x)[i].i = NA_REAL; i++; } while (i<ln);
        break;
    case STRSXP:
        do SET_STRING_ELT (x, i++, NA_STRING); while (i<ln);
        break;
    case VECSXP: case EXPRSXP:
        do SET_VECTOR_ELT (x, i++, R_NilValue); while (i<ln);
        break;
    default:
	UNIMPLEMENTED_TYPE("set_elements_to_NA_or_NULL", x);
    }
}


/* Copy n elements from vector v (starting at j, stepping by t) to 
   vector x (starting at i, stepping by s).  The vectors x and v must 
   be of the same type, which may be numeric or non-numeric.  Elements 
   of a VECSXP or EXPRSXP are duplicated.  If necessary, x and v are 
   protected. */

void copy_elements (SEXP x, int i, int s, SEXP v, int j, int t, int n)
{
    if (n == 0) return;

    if (n > 8 && s == 1 && t == 1 && isVectorAtomic(x)) {
        switch (TYPEOF(x)) {
        case RAWSXP:
            memmove (RAW(x)+i, RAW(v)+j, n * sizeof(char));
            break;
        case LGLSXP:
            memmove (LOGICAL(x)+i, LOGICAL(v)+j, n * sizeof(int));
            break;
        case INTSXP:
            memmove (INTEGER(x)+i, INTEGER(v)+j, n * sizeof(int));
            break;
        case REALSXP:
            memmove (REAL(x)+i, REAL(v)+j, n * sizeof(double));
            break;
        case CPLXSXP:
            memmove (COMPLEX(x)+i, COMPLEX(v)+j, n * sizeof(Rcomplex));
            break;
        case STRSXP:
            copy_string_elements (x, i, v, j, n);
            break;
        }
    }
    else {
        switch (TYPEOF(x)) {
        case RAWSXP:
            do { RAW(x)[i] = RAW(v)[j]; i += s; j += t; } while (--n>0);
            break;
        case LGLSXP:
            do { LOGICAL(x)[i] = LOGICAL(v)[j]; i += s; j += t; } while (--n>0);
            break;
        case INTSXP:
            do { INTEGER(x)[i] = INTEGER(v)[j]; i += s; j += t; } while (--n>0);
            break;
        case REALSXP:
            do { REAL(x)[i] = REAL(v)[j]; i += s; j += t; } while (--n>0);
            break;
        case CPLXSXP:
            do { COMPLEX(x)[i] = COMPLEX(v)[j]; i += s; j += t; } while (--n>0);
            break;
        case STRSXP:
            do { 
                SET_STRING_ELT (x, i, STRING_ELT(v,j));
                i += s; j += t; 
            } while (--n>0);
            break;
        case VECSXP: case EXPRSXP:
            PROTECT(x); PROTECT(v);
            do { 
                SET_VECTOR_ELT (x, i, duplicate(VECTOR_ELT(v,j)));
                i += s; j += t; 
            } while (--n>0);
            UNPROTECT(2);
            break;
        default:
            UNIMPLEMENTED_TYPE("copy_elements", x);
        }
    }
}

/* Copy n elements from vector v (starting at 0) to vector x (starting at i).
   The vector v may be shorter than n, in which case its elements are recycled.
   The vectors v and x must be of the same type, which may be numeric or 
   non-numeric.  Elements of a VECSXP or EXPRSXP are duplicated.  If necessary,
   x and v are protected. */

void copy_elements_recycled (SEXP x, int i, int s, SEXP v, int n)
{
    if (n == 0)
        return;

    int vl = LENGTH(v);
    if (vl == 0) abort();

    if (vl >= n)
        copy_elements (x, i, s, v, 0, 1, n);

    else if (vl == 1) {
        switch (TYPEOF(x)) {
        case RAWSXP: {
            Rbyte e = RAW(v)[0];
            do { RAW(x)[i] = e; i += s; } while (--n>0);
            break;
        }
        case LGLSXP: {
            int e = LOGICAL(v)[0];
            do { LOGICAL(x)[i] = e; i += s; } while (--n>0);
            break;
        }
        case INTSXP: {
            int e = INTEGER(v)[0];
            do { INTEGER(x)[i] = e; i += s; } while (--n>0);
            break;
        }
        case REALSXP: {
            double e = REAL(v)[0];
            do { REAL(x)[i] = e; i += s; } while (--n>0);
            break;
        }
        case CPLXSXP: {
            Rcomplex e = COMPLEX(v)[0];
            do { COMPLEX(x)[i] = e; i += s; } while (--n>0);
            break;
        }
        case STRSXP: {
            rep_string_elements (x, i, s, v, n);
            break;
        }
        case VECSXP: case EXPRSXP: {
            PROTECT(x); PROTECT(v);
            SEXP e = VECTOR_ELT(v,0);
            do { SET_VECTOR_ELT (x, i, duplicate(e)); i += s; } while (--n>0);
            UNPROTECT(2);
            break;
        }
        default:
            UNIMPLEMENTED_TYPE("copy_elements_recycled", x);
        }
    }

    else if (s == 1) {
        copy_elements (x, i, 1, v, 0, 1, vl);
        i += vl;
        n -= vl;
        switch (TYPEOF(x)) {
        case RAWSXP: {
            do {
                RAW(x)[i] = RAW(x)[i-vl];
                i += 1;
            } while (--n>0);
            break;
        }
        case LGLSXP: {
            do {
                LOGICAL(x)[i] = LOGICAL(x)[i-vl];
                i += 1;
            } while (--n>0);
            break;
        }
        case INTSXP: {
            do {
                INTEGER(x)[i] = INTEGER(x)[i-vl];
                i += 1;
            } while (--n>0);
            break;
        }
        case REALSXP: {
            do {
                REAL(x)[i] = REAL(x)[i-vl];
                i += 1;
            } while (--n>0);
            break;
        }
        case CPLXSXP: {
            do {
                COMPLEX(x)[i] = COMPLEX(x)[i-vl];
                i += 1;
            } while (--n>0);
            break;
        }
        case STRSXP: {
            copy_string_elements (x, i, x, i-vl, n); /* done sequentially */
            break;
        }
        case VECSXP: case EXPRSXP: {
            PROTECT(x); PROTECT(v);
            do { 
                SET_VECTOR_ELT (x, i, duplicate(VECTOR_ELT(x,i-vl)));
                i += 1;
            } while (--n>0);
            UNPROTECT(2);
            break;
        }
        default:
            UNIMPLEMENTED_TYPE("copy_elements", x);
        }
    }

    else {  /* s > 1 */
        int j = 0;
        switch (TYPEOF(x)) {
        case RAWSXP: {
            do {
                RAW(x)[i] = RAW(v)[j];
                i += s; if (++j == vl) j = 0; 
            } while (--n>0);
            break;
        }
        case LGLSXP: {
            do {
                LOGICAL(x)[i] = LOGICAL(v)[j];
                i += s; if (++j == vl) j = 0; 
            } while (--n>0);
            break;
        }
        case INTSXP: {
            do {
                INTEGER(x)[i] = INTEGER(v)[j];
                i += s; if (++j == vl) j = 0; 
            } while (--n>0);
            break;
        }
        case REALSXP: {
            do {
                REAL(x)[i] = REAL(v)[j];
                i += s; if (++j == vl) j = 0; 
            } while (--n>0);
            break;
        }
        case CPLXSXP: {
            do {
                COMPLEX(x)[i] = COMPLEX(v)[j];
                i += s; if (++j == vl) j = 0; 
            } while (--n>0);
            break;
        }
        case STRSXP: {
            do { 
                SET_STRING_ELT (x, i, STRING_ELT(v,j));
                i += s; if (++j == vl) j = 0; 
            } while (--n>0);
            break;
        }
        case VECSXP: case EXPRSXP: {
            PROTECT(x); PROTECT(v);
            do { 
                SET_VECTOR_ELT (x, i, duplicate(VECTOR_ELT(v,j)));
                i += s; if (++j == vl) j = 0; 
            } while (--n>0);
            UNPROTECT(2);
            break;
        }
        default:
            UNIMPLEMENTED_TYPE("copy_elements", x);
        }
    }
}

void copyVector(SEXP s, SEXP t)
{
    int i, ns, nt;

    nt = LENGTH(t);
    ns = LENGTH(s);

    if (nt >= ns && TYPEOF(s) != VECSXP && TYPEOF(s) != EXPRSXP) {
        copy_elements (s, 0, 1, t, 0, 1, ns);
        return;
    }

    switch (TYPEOF(s)) {
    case RAWSXP:
	for (i = 0; i < ns; i++)
	    RAW(s)[i] = RAW(t)[i % nt];
	break;
    case LGLSXP:
	for (i = 0; i < ns; i++)
	    LOGICAL(s)[i] = LOGICAL(t)[i % nt];
	break;
    case INTSXP:
	for (i = 0; i < ns; i++)
	    INTEGER(s)[i] = INTEGER(t)[i % nt];
	break;
    case REALSXP:
	for (i = 0; i < ns; i++)
	    REAL(s)[i] = REAL(t)[i % nt];
	break;
    case CPLXSXP:
	for (i = 0; i < ns; i++)
	    COMPLEX(s)[i] = COMPLEX(t)[i % nt];
	break;
    case STRSXP:
	for (i = 0; i < ns; i++)
	    SET_STRING_ELT(s, i, STRING_ELT(t, i % nt));
	break;
    case VECSXP: case EXPRSXP:
	for (i = 0; i < ns; i++)
	    SET_VECTOR_ELT(s, i, VECTOR_ELT(t, i % nt));
	break;
    }
}

void attribute_hidden copyListMatrix(SEXP s, SEXP t, Rboolean byrow)
{
    SEXP pt, tmp;
    int i, j, nr, nc, ns;

    nr = nrows(s);
    nc = ncols(s);
    ns = nr*nc;
    pt = t;
    if(byrow) {
	PROTECT(tmp = allocVector(STRSXP, nr*nc));
	for (i = 0; i < nr; i++)
	    for (j = 0; j < nc; j++) {
		SET_STRING_ELT(tmp, i + j * nr, duplicate(CAR(pt)));
		pt = CDR(pt);
		if(pt == R_NilValue) pt = t;
	    }
	for (i = 0; i < ns; i++) {
	    SETCAR(s, STRING_ELT(tmp, i++));
	    s = CDR(s);
	}
	UNPROTECT(1);
    }
    else {
	for (i = 0; i < ns; i++) {
	    SETCAR(s, duplicate(CAR(pt)));
	    s = CDR(s);
	    pt = CDR(pt);
	    if(pt == R_NilValue) pt = t;
	}
    }
}

void copyMatrix(SEXP s, SEXP t, Rboolean byrow)
{
    SEXP dims = getDimAttrib(s);
    int len = LENGTH(s);
    int nt = LENGTH(t);
    int nr, nc;

    if (dims == R_NilValue || LENGTH(dims) < 2) {
        nr = len;
        nc = 1;
    }
    else {
        nr = INTEGER(dims)[0];
        nc = INTEGER(dims)[1];
    }

    if (!byrow || nr == 1 || nt == 1) {  /* byrow=TRUE or byrow irrelevant */
        copy_elements_recycled (s, 0, 1, t, len);
    }
    else if (nt == len) {  /* same as a transpose operation */
        copy_transposed (s, t, nc, nr);  /* NOT nr, nc ! */
    }
    else if (nt <= nc) {  /* each column has repetitions of a single element */
        int i, j, k;
        i = j = k = 0;
        switch (TYPEOF(s)) {
        case RAWSXP:
            while (j < len) {
                Rbyte e = RAW(t)[k];
                i = j; j += nr;
                while (i < j) RAW(s)[i++] = e;
                k += 1; if (k >= nt) k = 0;
            }
            break;
        case LGLSXP:
            while (j < len) {
                int e = LOGICAL(t)[k];
                i = j; j += nr;
                while (i < j) LOGICAL(s)[i++] = e;
                k += 1; if (k >= nt) k = 0;
            }
            break;
        case INTSXP:
            while (j < len) {
                int e = INTEGER(t)[k];
                i = j; j += nr;
                while (i < j) INTEGER(s)[i++] = e;
                k += 1; if (k >= nt) k = 0;
            }
            break;
        case REALSXP:
            while (j < len) {
                double e = REAL(t)[k];
                i = j; j += nr;
                while (i < j) REAL(s)[i++] = e;
                k += 1; if (k >= nt) k = 0;
            }
            break;
        case CPLXSXP:
            while (j < len) {
                Rcomplex e = COMPLEX(t)[k];
                i = j; j += nr;
                while (i < j) COMPLEX(s)[i++] = e;
                k += 1; if (k >= nt) k = 0;
            }
            break;
        case STRSXP:
            while (j < len) {
                SEXP e = STRING_ELT(t,k);
                i = j; j += nr;
                while (i < j) SET_STRING_ELT(s,i++,e);
                k += 1; if (k >= nt) k = 0;
            }
            break;
        case VECSXP: case EXPRSXP:
            while (j < len) {
                SEXP e = VECTOR_ELT(t,k);
                i = j; j += nr;
                while (i < j) SET_VECTOR_ELT(s,i++,e);
                k += 1; if (k >= nt) k = 0;
            }
            break;
        default:
            UNIMPLEMENTED_TYPE("copyMatrix", s);
        }
    }

    else {  /* general case for byrow=TRUE */
        int len_1 = len - 1;
        int nomod = nt > len_1;
        int i, j;
        switch (TYPEOF(s)) {
        case RAWSXP:
            for (i = 0, j = 0; i <= len_1; i++, j += nc) {
                if (j > len_1) j -= len_1;
                RAW(s)[i] = RAW(t) [nomod ? j : j % nt];
            }
            break;
        case LGLSXP:
            for (i = 0, j = 0; i <= len_1; i++, j += nc) {
                if (j > len_1) j -= len_1;
                LOGICAL(s)[i] = LOGICAL(t) [nomod ? j : j % nt];
            }
            break;
        case INTSXP:
            for (i = 0, j = 0; i <= len_1; i++, j += nc) {
                if (j > len_1) j -= len_1;
                INTEGER(s)[i] = INTEGER(t) [nomod ? j : j % nt];
            }
            break;
        case REALSXP:
            for (i = 0, j = 0; i <= len_1; i++, j += nc) {
                if (j > len_1) j -= len_1;
                REAL(s)[i] = REAL(t) [nomod ? j : j % nt];
            }
            break;
        case CPLXSXP:
            for (i = 0, j = 0; i <= len_1; i++, j += nc) {
                if (j > len_1) j -= len_1;
                COMPLEX(s)[i] = COMPLEX(t) [nomod ? j : j % nt];
            }
            break;
        case STRSXP:
            for (i = 0, j = 0; i <= len_1; i++, j += nc) {
                if (j > len_1) j -= len_1;
                SET_STRING_ELT (s, i, STRING_ELT (t, nomod ? j : j % nt));
            }
            break;
        case VECSXP: case EXPRSXP:
            for (i = 0, j = 0; i <= len_1; i++, j += nc) {
                if (j > len_1) j -= len_1;
                SET_VECTOR_ELT (s, i, VECTOR_ELT (t, nomod ? j : j % nt));
            }
            break;
        default:
            UNIMPLEMENTED_TYPE("copyMatrix", s);
        }
    }
}


/* Duplicates object, including attributes, except that a VECSXP or EXPRSXP 
   is duplicated at the top level only, and the NAMEDCNT field of each element 
   is incremented to account for the extra reference.  The argument needn't be 
   protected by the caller. */

SEXP attribute_hidden dup_top_level (SEXP x)
{
    R_len_t n;
    SEXP r;
    int i;

    if (!isVectorList(x))
        return duplicate(x);

    PROTECT(x);
    n = LENGTH(x);
    PROTECT(r = allocVector(TYPEOF(x),n));
    copy_vector_elements (r, 0, x, 0, n);
    for (i = 0; i < n; i++) INC_NAMEDCNT_0_AS_1 (VECTOR_ELT(r,i));
    DUPLICATE_ATTRIB(r,x);
    UNPROTECT(2);

    return r;
}


/* Duplicate an argument list.  Does not adjust NAMEDCNT, and does not
   duplicate any attributes of binding cells. */

SEXP attribute_hidden dup_arg_list (SEXP x)
{
    SEXP r;

    if (x == R_NilValue)
        return x;

    PROTECT(x);

    if (CDR(x) == R_NilValue) 

        r = cons_with_tag (CAR(x), R_NilValue, TAG(x));

    else if (CDDR(x) == R_NilValue)

        r = cons_with_tag (CAR(x), 
                           cons_with_tag (CADR(x), R_NilValue, TAG(CDR(x))),
                           TAG(x));
    else {

        SEXP s, t, u;
        PROTECT (r = cons_with_tag (CAR(x), R_NilValue, TAG(x)));
        t = r;
        s = CDR(x);
        do { 
            u = cons_with_tag (CAR(s), R_NilValue, TAG(s));
            SETCDR(t,u);
            t = u;
            s = CDR(s);
        } while (s != R_NilValue);
        UNPROTECT(1);
    }

    UNPROTECT(1);
    return r;
}
