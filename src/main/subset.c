/*
 *  pqR : A pretty quick version of R
 *  Copyright (C) 2013, 2014, 2015, 2016, 2017, 2018 by Radford M. Neal
 *
 *  Based on R : A Computer Language for Statistical Data Analysis
 *  Copyright (C) 1995, 1996  Robert Gentleman and Ross Ihaka
 *  Copyright (C) 1997-2007   The R Core Team
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
 *  Vector and List Subsetting
 *
 *  There are three kinds of subscripting [, [[, and $.
 *  We have three different functions to compute these.
 *
 *
 *  Note on Matrix Subscripts
 *
 *  The special [ subscripting where dim(x) == ncol(subscript matrix)
 *  is handled inside VectorSubset. The subscript matrix is turned
 *  into a subscript vector of the appropriate size and then
 *  VectorSubset continues.  This provides coherence especially
 *  regarding attributes etc. (it would be quicker to handle this case
 *  separately, but then we would have more to keep in step.
 *
 *
 *  Subscripts that are ranges:  "[" now asks for a variant result from
 *  a sequence operator (for first index), hoping to get a range rather
 *  than a sequence, allowing for faster extraction, and avoidance of
 *  memory use for the sequence.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define USE_FAST_PROTECT_MACROS
#include "Defn.h"

#include "scalar-stack.h"

#include <helpers/helpers-app.h>

/* JMC convinced MM that this was not a good idea: */
#undef _S4_subsettable

/* Convert range to vector. The caller must ensure that its size won't 
   overflow. */

SEXP attribute_hidden Rf_VectorFromRange (int rng0, int rng1)
{ 
    SEXP vec;
    int i, n;

    n = rng1>=rng0 ? rng1-rng0+1 : 0;
  
    vec = allocVector(INTSXP, n);

    for (i = 0; i<n; i++) 
        INTEGER(vec)[i] = rng0 + i;
  
    return vec;
}

/* Take a range (in seq) and either extract a range (possibly empty)
   of positive subscripts into start and end and return R_NoObject, or
   convert the range to a vector of negative integer subscripts that
   is returned. */

SEXP attribute_hidden Rf_DecideVectorOrRange(int64_t seq, int *start, int *end,
                                             SEXP call)
{
    int from, len;

    from = seq >> 32;
    len = (seq >> 1) & 0x7fffffff;

    if (from==0) {                     /* get rid of 0 subscript at beginning */
        from = 1;
        len -= 1;
    }
    if (from < 0 && from+(len-1) == 0) /* get rid of 0 subscript at end */
        len -= 1;

    if (from < 0 && from+(len-1) > 0) 
        errorcall(call, _("only 0's may be mixed with negative subscripts"));

    if (from > 0) {
        *start = from;
        *end = from+(len-1);
        return R_NoObject;
    }
    else {
        return Rf_VectorFromRange (from, from+(len-1));
    }
}


/* ExtractRange does the transfer of elements from "x" to "result" 
   according to the range given by start and end.  The caller will
   have allocated "result" to be at least the required length, and
   for VECSXP and EXPRSXP, the entries will be R_NilValue (done by
   allocVector).

   Arguments x and result must be protected by the caller. */

static void ExtractRange(SEXP x, SEXP result, int start, int end, SEXP call)
{
    int nx = length(x);

    SEXP tmp, tmp2;
    int n, m, i;

    start -= 1;
    n = end-start;
    m = start>=nx ? 0 : end<=nx ? n : nx-start;

    tmp = result;

    switch (TYPEOF(x)) {
    case LGLSXP:
        memcpy (LOGICAL(result), LOGICAL(x)+start, m * sizeof *LOGICAL(x));
        for (i = m; i<n; i++) LOGICAL(result)[i] = NA_LOGICAL;
        break;
    case INTSXP:
        memcpy (INTEGER(result), INTEGER(x)+start, m * sizeof *INTEGER(x));
        for (i = m; i<n; i++) INTEGER(result)[i] = NA_INTEGER;
        break;
    case REALSXP:
        memcpy (REAL(result), REAL(x)+start, m * sizeof *REAL(x));
        for (i = m; i<n; i++) REAL(result)[i] = NA_REAL;
        break;
    case CPLXSXP:
        memcpy (COMPLEX(result), COMPLEX(x)+start, m * sizeof *COMPLEX(x));
        for (i = m; i<n; i++) {
            COMPLEX(result)[i].r = NA_REAL;
            COMPLEX(result)[i].i = NA_REAL;
        }
        break;
    case STRSXP:
        copy_string_elements (result, 0, x, start, m);
        for (i = m; i<n; i++) SET_STRING_ELT(result, i, NA_STRING);
        break;
    case VECSXP:
    case EXPRSXP:
        if (!DUPVE || NAMEDCNT_EQ_0(x)) {
            copy_vector_elements (result, 0, x, start, m);
            if (NAMEDCNT_GT_0(x))
                for (i = 0; i<m; i++)
                    INC_NAMEDCNT_0_AS_1(VECTOR_ELT(result,i));
        }
        else {
            for (i = 0; i<m; i++)
                SET_VECTOR_ELT (result, i, duplicate(VECTOR_ELT(x,start+i)));
        }
        /* remaining elements already set to R_NilValue */
        break;
    case LISTSXP:
            /* cannot happen: pairlists are coerced to lists */
    case LANGSXP:
        for (i = 0; i<m; i++) {
            tmp2 = nthcdr(x, start+i);
            SETCAR(tmp, CAR(tmp2));
            SET_TAG(tmp, TAG(tmp2));
            tmp = CDR(tmp);
        }
        for ( ; i<n; i++) {
            SETCAR_NIL(tmp);
            tmp = CDR(tmp);
        }
        break;
    case RAWSXP:
        memcpy (RAW(result), RAW(x)+start, m * sizeof *RAW(x));
        for (i = m; i<n; i++) RAW(result)[i] = (Rbyte) 0;
        break;
    default:
        nonsubsettable_error(call,x);
    }
}


/* ExtractSubset does the transfer of elements from "x" to "result" 
   according to the integer subscripts given in "indx". The caller will
   have allocated "result" to be at least the required length, and
   for VECSXP and EXPRSXP, the entries will be R_NilValue (done by
   allocVector).  

   Out of bound indexes (including 0 and NA) give NA.

   Arguments x and result must be protected by the caller. */

static void ExtractSubset(SEXP x, SEXP result, SEXP indx, SEXP call)
{
    int *ix = INTEGER(indx);
    int n = LENGTH(indx);
    int nx = LENGTH(x);
    int i, ii;

    switch (TYPEOF(x)) {
    case LGLSXP:
        for (i = 0; i<n; i++)
            if ((ii = ix[i]) <= 0 || ii > nx)
                LOGICAL(result)[i] = NA_LOGICAL;
            else
                LOGICAL(result)[i] = LOGICAL(x)[ii-1];
        break;
    case INTSXP:
        for (i = 0; i<n; i++)
            if ((ii = ix[i]) <= 0 || ii > nx)
                INTEGER(result)[i] = NA_INTEGER;
            else
                INTEGER(result)[i] = INTEGER(x)[ii-1];
        break;
    case REALSXP:
        for (i = 0; i<n; i++)
            if ((ii = ix[i]) <= 0 || ii > nx)
                REAL(result)[i] = NA_REAL;
            else
                REAL(result)[i] = REAL(x)[ii-1];
        break;
    case CPLXSXP:
        for (i = 0; i<n; i++)
            if ((ii = ix[i]) <= 0 || ii > nx) {
                COMPLEX(result)[i].r = NA_REAL;
                COMPLEX(result)[i].i = NA_REAL; 
            }
            else
                COMPLEX(result)[i] = COMPLEX(x)[ii-1];
        break;
    case STRSXP:
        for (i = 0; i<n; i++)
            if ((ii = ix[i]) <= 0 || ii > nx)
                SET_STRING_ELT(result, i, NA_STRING);
            else
                SET_STRING_ELT(result, i, STRING_ELT(x, ii-1));
        break;
    case VECSXP:
    case EXPRSXP:
        if (NAMEDCNT_EQ_0(x)) {
            for (i = 0; i<n; i++)
                if ((ii = ix[i]) <= 0 || ii > nx)
                    /* nothing, already R_NilValue */ ;
                else {
                    SEXP ve = VECTOR_ELT(x, ii-1);
                    SET_VECTOR_ELT(result, i, ve);
                    if (i > 0) INC_NAMEDCNT_0_AS_1(ve);
                }
        }
        else {
            for (i = 0; i<n; i++)
                if ((ii = ix[i]) <= 0 || ii > nx)
                    /* nothing, already R_NilValue */ ;
                else 
                    SET_VECTOR_ELEMENT_FROM_VECTOR(result, i, x, ii-1);
        }
        break;
    case LISTSXP:
	    /* cannot happen: pairlists are coerced to lists */
    case LANGSXP: ;
        SEXP tmp, tmp2;
        tmp = result;
        for (i = 0; i<n; i++) {
            if ((ii = ix[i]) <= 0 || ii > nx)
                SETCAR_NIL(tmp);
            else {
                tmp2 = nthcdr(x, ii-1);
                SETCAR(tmp, CAR(tmp2));
                SET_TAG(tmp, TAG(tmp2));
            }
            tmp = CDR(tmp);
        }
        break;
    case RAWSXP:
        for (i = 0; i<n; i++)
            if ((ii = ix[i]) <= 0 || ii > nx)
                RAW(result)[i] = (Rbyte) 0;
            else
                RAW(result)[i] = RAW(x)[ii-1];
        break;
    default:
        nonsubsettable_error(call,x);
    }
}


/* Check whether subscript is such that we suppress dropping dimensions 
   when the drop argument is NA (the default).  Assumes that it's not a
   missing argument (which needs to be checked separately). */

static inline int whether_suppress_drop (SEXP sb)
{
    SEXP d;
    return TYPEOF(sb) != LGLSXP 
             && HAS_ATTRIB(sb)
             && (d = getDimAttrib(sb)) != R_NilValue
             && LENGTH(d) == 1;
}


/* This is for all cases with a single index, including 1D arrays and
   matrix indexing of arrays */
static SEXP VectorSubset(SEXP x, SEXP subs, int64_t seq, int drop, SEXP call)
{
    SEXP sb = subs == R_NilValue ? R_MissingArg : CAR(subs);
    SEXP indx = R_NilValue;
    SEXP result, dims, attrib, nattrib;
    int start = 1, end = 0, n = 0;
    int suppress_drop = 0;
    int spi, ndim;

    /* R_NilValue is handled specially, always returning R_NilValue. */

    if (x == R_NilValue)
        return R_NilValue;

    /* SPECIAL KLUDGE:  To avoid breaking inexplicable code in some 
       packages, just return a duplicate of x if the subscripting has 
       the form x[,drop=FALSE]. */

    if (sb == R_MissingArg && drop == FALSE)
        return duplicate(x);

    PROTECT_WITH_INDEX (sb, &spi);
    dims = getDimAttrib(x);
    ndim = dims==R_NilValue ? 0 : LENGTH(dims);

    /* Check for variant result, which will be a range rather than a vector, 
       and if we have a range, see whether it can be used directly, or must
       be converted to a vector to be handled as other vectors. */

    if (seq) {
        suppress_drop = seq & 1;  /* get flag for seq having a dim attr */
        REPROTECT(sb = Rf_DecideVectorOrRange(seq,&start,&end,call), spi);
        if (sb == R_NoObject)
            n = end - start + 1;
    }

    /* If subscript is missing, convert to range over entire vector. 
       Also, suppress dropping if it was missing from '_'. */

    if (sb == R_MissingArg) {
        suppress_drop = subs != R_NilValue && MISSING(subs) == 2;
        start = 1;
        end = length(x);
        n = end;
        sb = R_NoObject;
    }

    /* Check to see if we have special matrix subscripting. */
    /* If we do, make a real subscript vector and protect it. */

    if (sb != R_NoObject && isMatrix(sb) && isArray(x) && ncols(sb) == ndim) {
        if (isString(sb)) {
            sb = strmat2intmat(sb, GetArrayDimnames(x), call);
            REPROTECT(sb,spi);
        }
        if (isInteger(sb) || isReal(sb)) {
            sb = mat2indsub(dims, sb, call);
            REPROTECT(sb,spi);
        }
    }

    /* Convert sb to a vector of integer subscripts (unless we have a range).
       We suppress dropping when drop is NA when the index is not logical
       and is a 1D array. */

    if (sb != R_NoObject) {
        int stretch = 1;  /* allow out of bounds, not for assignment */
        int hasna;
        if (drop == NA_LOGICAL) suppress_drop = whether_suppress_drop(sb);
        PROTECT(indx = makeSubscript(x, sb, &stretch, &hasna, call, 0));
        n = LENGTH(indx);
    }

    /* Allocate and extract the result. */

    PROTECT (result = allocVector(TYPEOF(x),n));
    if (sb==R_NoObject)
        ExtractRange(x, result, start, end, call);
    else 
        ExtractSubset(x, result, indx, call);

    if (((attrib = getAttrib(x, R_NamesSymbol)) != R_NilValue) ||
        ( /* here we might have an array.  Use row names if 1D */
            ndim == 1 &&
            (attrib = getAttrib(x, R_DimNamesSymbol)) != R_NilValue &&
            (attrib = GetRowNames(attrib)) != R_NilValue
            )
        ) {
        PROTECT(attrib);
        PROTECT(nattrib = allocVector(TYPEOF(attrib), n));
        if (sb==R_NoObject)
            ExtractRange(attrib, nattrib, start, end, call);
        else
            ExtractSubset(attrib, nattrib, indx, call);
        setAttrib(result, R_NamesSymbol, nattrib);
        UNPROTECT(2);
    }
    if ((attrib = getAttrib(x, R_SrcrefSymbol)) != R_NilValue &&
        TYPEOF(attrib) == VECSXP) {
        PROTECT(attrib);
        PROTECT(nattrib = allocVector(VECSXP, n));
        if (sb==R_NoObject)
            ExtractRange(attrib, nattrib, start, end, call);
        else
            ExtractSubset(attrib, nattrib, indx, call);
        setAttrib(result, R_SrcrefSymbol, nattrib);
           UNPROTECT(2);
    }

    /* FIXME: this is wrong, because the slots are gone, so result is
       an invalid object of the S4 class! JMC 3/3/09 */
#ifdef _S4_subsettable
    if(IS_S4_OBJECT(x)) { /* e.g. contains = "list" */
        setAttrib(result, R_ClassSymbol, getClassAttrib(x));
        SET_S4_OBJECT(result);
    }
#endif

    UNPROTECT(2 + (sb!=R_NoObject));

    /* One-dimensional arrays should have their dimensions dropped only 
       if the result has length one and drop TRUE or is NA_INTEGER without
       the drop being suppressed by the index being a 1D array. */

    if (ndim == 1) {
        int len = length(result);

        if (len > 1 || drop == FALSE || drop == NA_INTEGER && suppress_drop) {
            SEXP attr;
            PROTECT(result);
            PROTECT(attr = allocVector1INT());
            INTEGER(attr)[0] = len;
            if((attrib = getAttrib(x, R_DimNamesSymbol)) != R_NilValue) {
                /* reinstate dimnames, include names of dimnames, which
                   should be in the names attribute at this point. */
                PROTECT(attrib = dup_top_level(attrib));
                SET_VECTOR_ELT(attrib, 0, getAttrib(result, R_NamesSymbol));
                setAttrib(result, R_DimSymbol, attr);
                setAttrib(result, R_DimNamesSymbol, attrib);
                setAttrib(result, R_NamesSymbol, R_NilValue);
                UNPROTECT(1);
            }
            else 
                setAttrib(result, R_DimSymbol, attr);
            UNPROTECT(2);
        }
    }

    return result;
}


/* Used in MatrixSubset to set a whole row or column of a matrix to NAs. */

static void set_row_or_col_to_na (SEXP result, int start, int step, int end,
                                  SEXP call)
{
    int i;
    switch (TYPEOF(result)) {
    case LGLSXP:
        for (i = start; i<end; i += step)
            LOGICAL(result)[i] = NA_LOGICAL;
        break;
    case INTSXP:
        for (i = start; i<end; i += step)
            INTEGER(result)[i] = NA_INTEGER;
        break;
    case REALSXP:
        for (i = start; i<end; i += step)
            REAL(result)[i] = NA_REAL;
        break;
    case CPLXSXP:
        for (i = start; i<end; i += step) {
            COMPLEX(result)[i].r = NA_REAL;
            COMPLEX(result)[i].i = NA_REAL;
        }
        break;
    case STRSXP:
        for (i = start; i<end; i += step)
            SET_STRING_ELT(result, i, NA_STRING);
        break;
    case VECSXP:
        for (i = start; i<end; i += step)
            SET_VECTOR_ELT_NIL(result, i);
        break;
    case RAWSXP:
        for (i = start; i<end; i += step)
            RAW(result)[i] = (Rbyte) 0;
        break;
    default:
        errorcall(call, _("matrix subscripting not handled for this type"));
    }
}


/* Used in MatrixSubset when only one (valid) row is accessed. */

static void one_row_of_matrix (SEXP call, SEXP x, SEXP result, 
                               int ii, int nr, SEXP sc, int ncs, int nc)
{
    int typeofx = TYPEOF(x);
    int j, jj, st;

    st = (ii-1) - nr;

    for (j = 0; j < ncs; j++) {

        jj = INTEGER(sc)[j];

        if (jj == NA_INTEGER) {
            set_row_or_col_to_na (result, j, 1, j+1, call);
            continue;
        }

        if (jj < 1 || jj > nc)
            out_of_bounds_error(call);

        switch (typeofx) {
        case LGLSXP:
            LOGICAL(result)[j] = LOGICAL(x) [st+jj*nr];
            break;
        case INTSXP:
            INTEGER(result)[j] = INTEGER(x) [st+jj*nr];
            break;
        case REALSXP:
            REAL(result)[j] = REAL(x) [st+jj*nr];
            break;
        case CPLXSXP:
            COMPLEX(result)[j] = COMPLEX(x) [st+jj*nr];
            break;
        case STRSXP:
            SET_STRING_ELT(result, j, STRING_ELT(x, st+jj*nr));
            break;
        case VECSXP: ;
            SET_VECTOR_ELEMENT_FROM_VECTOR(result, j, x, st+jj*nr);
            break;
        case RAWSXP:
            RAW(result)[j] = RAW(x) [st+jj*nr];
            break;
        default:
            errorcall(call, _("matrix subscripting not handled for this type"));
        }
    }
}


/* Used in MatrixSubset for subsetting with range of rows. */

static void range_of_rows_of_matrix (SEXP call, SEXP x, SEXP result, 
    int start, int nrs, int nr, SEXP sc, int ncs, int nc)
{
    int i, j, jj, ij, jjnr;

    start -= 1;

    if (start < 0 || start+nrs > nr)
        out_of_bounds_error(call);

    /* Loop to handle extraction, with outer loop over columns. */

    ij = 0;
    for (j = 0; j < ncs; j++) {
        jj = INTEGER(sc)[j];

        /* If column index is NA, just set column of result to NAs. */

        if (jj == NA_INTEGER) {
            set_row_or_col_to_na (result, ij, 1, ij+nrs, call);
            ij += nrs;
            continue;
        }

        /* Check for bad column index. */

        if (jj < 1 || jj > nc)
            out_of_bounds_error(call);

        /* Loops over range of rows. */

        jjnr = (jj-1) * nr + start;
        switch (TYPEOF(x)) {
        case LGLSXP:
            memcpy (LOGICAL(result)+ij, LOGICAL(x)+jjnr, 
                    nrs * sizeof *LOGICAL(x));
            break;
        case INTSXP:
            memcpy (INTEGER(result)+ij, INTEGER(x)+jjnr, 
                    nrs * sizeof *INTEGER(x));
            break;
        case REALSXP:
            memcpy (REAL(result)+ij, REAL(x)+jjnr, 
                    nrs * sizeof *REAL(x));
            break;
        case CPLXSXP:
            memcpy (COMPLEX(result)+ij, COMPLEX(x)+jjnr, 
                    nrs * sizeof *COMPLEX(x));
            break;
        case STRSXP:
            copy_string_elements (result, ij, x, jjnr, nrs);
            break;
        case VECSXP:
            if (!DUPVE || NAMEDCNT_EQ_0(x)) {
                copy_vector_elements (result, ij, x, jjnr, nrs);
                if (NAMEDCNT_GT_0(x))
                    for (i = 0; i<nrs; i++)
                        INC_NAMEDCNT_0_AS_1(VECTOR_ELT(result,ij+i));
            }
            else {
                for (i = 0; i<nrs; i++)
                    SET_VECTOR_ELT (result, ij+i, 
                                    duplicate(VECTOR_ELT(x,jjnr+i)));
            }
            break;
        case RAWSXP:
            memcpy (RAW(result)+ij, RAW(x)+jjnr, 
                    nrs * sizeof *RAW(x));
            break;
        default:
            errorcall(call, _("matrix subscripting not handled for this type"));
        }

        ij += nrs;
    }
}


/* Used in MatrixSubset for the general case of subsetting. */

static void multiple_rows_of_matrix (SEXP call, SEXP x, SEXP result, 
    SEXP sr, int nrs, int nr, SEXP sc, int ncs, int nc)
{
    int i, j, ii, jj, ij, jjnr;

    /* Set rows of result to NAs where there are NA row indexes.  Also check 
       for bad row indexes (once here rather than many times in loop). */

    for (i = 0; i < nrs; i++) {
        ii = INTEGER(sr)[i];
        if (ii == NA_INTEGER) 
            set_row_or_col_to_na (result, i, nrs, i+nrs*ncs, call);
        else if (ii < 1 || ii > nr)
            out_of_bounds_error(call);
    }

    /* Loop to handle extraction except for NAs.  Outer loop is over columns so
       writes are sequential, which is faster for indexing, and probably better
       for memory speed. */

    for (j = 0, ij = 0; j < ncs; j++) {
        jj = INTEGER(sc)[j];

        /* If column index is NA, just set column of result to NAs. */

        if (jj == NA_INTEGER) {
            set_row_or_col_to_na (result, j*nrs, 1, (j+1)*nrs, call);
            ij += nrs;
            continue;
        }

        /* Check for bad column index. */

        if (jj < 1 || jj > nc)
            out_of_bounds_error(call);

        /* Loops over row indexes, except skips NA row indexes, done above. */

        int *sri = INTEGER(sr);
        jjnr = (jj-1) * nr;
        switch (TYPEOF(x)) {
        case LGLSXP:
            for (i = 0; i < nrs; i++, ij++) 
                if ((ii = sri[i]) != NA_INTEGER) 
                    LOGICAL(result)[ij] = LOGICAL(x)[(ii-1)+jjnr];
            break;
        case INTSXP:
            for (i = 0; i < nrs; i++, ij++) 
                if ((ii = sri[i]) != NA_INTEGER) 
                    INTEGER(result)[ij] = INTEGER(x)[(ii-1)+jjnr];
            break;
        case REALSXP:
            for (i = 0; i < nrs; i++, ij++) 
                if ((ii = sri[i]) != NA_INTEGER) 
                    REAL(result)[ij] = REAL(x)[(ii-1)+jjnr];
            break;
        case CPLXSXP:
            for (i = 0; i < nrs; i++, ij++) 
                if ((ii = sri[i]) != NA_INTEGER) 
                    COMPLEX(result)[ij] = COMPLEX(x)[(ii-1)+jjnr];
            break;
        case STRSXP:
            for (i = 0; i < nrs; i++, ij++) 
                if ((ii = sri[i]) != NA_INTEGER) 
                    SET_STRING_ELT(result, ij, STRING_ELT(x, (ii-1)+jjnr));
            break;
        case VECSXP:
            if (!DUPVE || NAMEDCNT_EQ_0(x)) {
                for (i = 0; i < nrs; i++, ij++) 
                    if ((ii = sri[i]) != NA_INTEGER) {
                        SEXP ve = VECTOR_ELT(x, (ii-1)+jjnr);
                        SET_VECTOR_ELT (result, ij, ve);
                        INC_NAMEDCNT_0_AS_1(ve);
                    }
            }
            else {
                for (i = 0; i < nrs; i++, ij++) 
                    if ((ii = sri[i]) != NA_INTEGER) 
                        SET_VECTOR_ELT (result, ij, 
                          duplicate(VECTOR_ELT(x,(ii-1)+jjnr)));
            }
            break;
        case RAWSXP:
            for (i = 0; i < nrs; i++, ij++) 
                if ((ii = sri[i]) != NA_INTEGER) 
                    RAW(result)[ij] = RAW(x)[(ii-1)+jjnr];
            break;
        default:
            errorcall(call, _("matrix subscripting not handled for this type"));
        }
    }
}


/* Subset for a vector with dim attribute specifying two dimensions. */

static SEXP MatrixSubset(SEXP x, SEXP subs, SEXP call, int drop, int64_t seq)
{
    SEXP s0 = CAR(subs), s1 = CADR(subs);
    SEXP dims, result, sr, sc;
    int rhasna = 0, chasna = 0;
    int start = 1, end = 0;
    int nr, nc, nrs = 0, ncs = 0;
    int suppress_drop_row = 0, suppress_drop_col = 0;
    int ii;

    PROTECT2(x,subs);
    int nprotect = 2;

    SEXP dim = getDimAttrib(x);

    nr = INTEGER(dim)[0];
    nc = INTEGER(dim)[1];

    /* s0 is set to R_NoObject when we have a range for the first subscript */

    if (s0 == R_MissingArg) {
        suppress_drop_row = MISSING(subs)==2;
        start = 1;
        end = nr;
        nrs = nr;
        s0 = R_NoObject;
    }
    else if (seq) {
        suppress_drop_row = seq & 1;
        PROTECT(s0 = Rf_DecideVectorOrRange(seq,&start,&end,call));
        nprotect++;
        if (s0 == R_NoObject)
            nrs = end - start + 1;
    }

    SEXP sv_scalar_stack = R_scalar_stack;

    if (s0 != R_NoObject) {
        if (drop == NA_LOGICAL) 
            suppress_drop_row = whether_suppress_drop(s0);
        PROTECT (sr = array_sub (s0, dim, 0, x, &rhasna));
        nprotect++;
        nrs = LENGTH(sr);
    }

    if (drop == NA_LOGICAL) 
        suppress_drop_col = s1 == R_MissingArg ? MISSING(CDR(subs)) == 2 
                                               : whether_suppress_drop(s1);

    if (drop == FALSE)
        suppress_drop_row = suppress_drop_col = 1;

    PROTECT (sc = array_sub (s1, dim, 1, x, &chasna));
    nprotect++;
    ncs = LENGTH(sc);

    if (nrs < 0 || ncs < 0)
        abort();  /* shouldn't happen, but code was conditional before... */

    /* Check this does not overflow */
    if ((double)nrs * (double)ncs > R_LEN_T_MAX)
        error(_("dimensions would exceed maximum size of array"));

    PROTECT (result = allocVector(TYPEOF(x), nrs*ncs));
    nprotect++;

    /* Extract elements from matrix x to result. */

    if (s0 == R_NoObject)
        range_of_rows_of_matrix(call, x, result, start, nrs, nr, sc, ncs, nc);
    else if (nrs == 1 && (ii = INTEGER(sr)[0]) != NA_INTEGER 
                      && ii >= 0 && ii <= nr)
        one_row_of_matrix (call, x, result, ii, nr, sc, ncs, nc);
    else
        multiple_rows_of_matrix (call, x, result, sr, nrs, nr, sc, ncs, nc);

    /* Set up dimnames of the returned value.  Not attached to result yet. */

    SEXP dimnames, dimnamesnames, newdimnames;
    PROTECT(dimnames = getAttrib(x, R_DimNamesSymbol));
    nprotect++;

    if (dimnames == R_NilValue)
        newdimnames = R_NilValue;
    else {
        PROTECT(dimnamesnames = getAttrib(dimnames, R_NamesSymbol));
        nprotect++;
        PROTECT(newdimnames = allocVector(VECSXP, 2));
        nprotect++;
        if (TYPEOF(dimnames) == VECSXP) {
            if (VECTOR_ELT(dimnames,0) != R_NilValue) {
                SET_VECTOR_ELT (newdimnames, 0, allocVector(STRSXP, nrs));
                if (s0 == R_NoObject)
                    ExtractRange (VECTOR_ELT(dimnames,0),
                      VECTOR_ELT(newdimnames,0), start, end, call);
                else
                    ExtractSubset(VECTOR_ELT(dimnames,0),
                      VECTOR_ELT(newdimnames,0), sr, call);
            }
            if (VECTOR_ELT(dimnames,1) != R_NilValue) {
                SET_VECTOR_ELT (newdimnames, 1, allocVector(STRSXP, ncs));
                ExtractSubset(VECTOR_ELT(dimnames,1),
                  VECTOR_ELT(newdimnames,1), sc, call);
            }
        }
        else {
            if (CAR(dimnames) != R_NilValue) {
                SET_VECTOR_ELT (newdimnames, 0, allocVector(STRSXP, nrs));
                if (s0 == R_NoObject)
                    ExtractRange (CAR(dimnames),
                      VECTOR_ELT(newdimnames,0), start, end, call);
                else
                    ExtractSubset(CAR(dimnames),
                      VECTOR_ELT(newdimnames,0), sr, call);
            }
            if (CADR(dimnames) != R_NilValue) {
                SET_VECTOR_ELT (newdimnames, 1, allocVector(STRSXP, ncs));
                ExtractSubset(CADR(dimnames),
                  VECTOR_ELT(newdimnames,1), sc, call);
            }
        }
        setAttrib(newdimnames, R_NamesSymbol, dimnamesnames);
    }

    /* Set up dimensions attribute and attach dimnames, unless dimensions
       will be dropped (in which case names attribute may be attached). */

    if (LENGTH(result) == 1 && suppress_drop_row + suppress_drop_col != 2) {
        if (newdimnames != R_NilValue) {
            /* attach names if unambiguous which are wanted */
            SEXP rn = VECTOR_ELT(newdimnames,0);
            SEXP cn = VECTOR_ELT(newdimnames,1);
            if (rn == R_NilValue || suppress_drop_col)
                setAttrib (result, R_NamesSymbol, cn);
            else if (cn == R_NilValue || suppress_drop_row)
                setAttrib (result, R_NamesSymbol, rn);
        }
    }
    else if (nrs == 1 && !suppress_drop_row) {
        if (newdimnames != R_NilValue)
            setAttrib (result, R_NamesSymbol, VECTOR_ELT(newdimnames,1));
    }
    else if (ncs == 1 && !suppress_drop_col) {
        if (newdimnames != R_NilValue)
            setAttrib (result, R_NamesSymbol, VECTOR_ELT(newdimnames,0));
    }
    else {
        PROTECT(dims = allocVector(INTSXP, 2));
        nprotect++;
        INTEGER(dims)[0] = nrs;
        INTEGER(dims)[1] = ncs;
        setAttrib(result, R_DimSymbol, dims);
        setAttrib(result, R_DimNamesSymbol, newdimnames);
    }

    UNPROTECT(nprotect);
    R_scalar_stack = sv_scalar_stack;
    return result;
}


static SEXP ArraySubset(SEXP x, SEXP s, SEXP call, int drop, SEXP xdims, int k)
{
    int i, j, ii, jj, n;
    SEXP dimnames, dimnamesnames, r, result;
    int mode = TYPEOF(x);

    int *subs[k], indx[k], nsubs[k], offset[k], suppress_drop[k];
    SEXP subv[k];

    SEXP sv_scalar_stack = R_scalar_stack;

    PROTECT3(x,s,xdims);

    n = 1; r = s;
    for (i = 0; i < k; i++) {
        int hasna;
        if (drop == TRUE)
            suppress_drop[i] = 0;
        else if (drop == FALSE)
            suppress_drop[i] = 1;
        else /* drop == NA_LOGICAL */ 
            suppress_drop[i] = CAR(r) == R_MissingArg ? MISSING(r) == 2
                                : whether_suppress_drop(CAR(r));
        PROTECT (subv[i] = array_sub (CAR(r), xdims, i, x, &hasna));
        subs[i] = INTEGER(subv[i]);
	nsubs[i] = LENGTH(subv[i]);
        n *= nsubs[i];
        indx[i] = 0;
	r = CDR(r);
    }

    offset[1] = INTEGER(xdims)[0];  /* offset[0] is not used */
    for (i = 2; i < k; i++)
        offset[i] = offset[i-1] * INTEGER(xdims)[i-1];

    /* Check for out-of-bounds indexes.  Disabled, since it seems unnecessary,
       given that arraySubscript checks bounds. */

#if 0
    for (j = 0; j < k; j++) {
        for (i = 0; i < nsubs[j]; i++) {
            jj = subs[j][i];
            if (jj != NA_INTEGER && (jj < 1 || jj > INTEGER(xdims)[j])) {
                out_of_bounds_error(call);
            }
        }
    }
#endif

    /* Vector to contain the returned values. */

    PROTECT(result = allocVector(mode, n));

    if (n == 0) goto done;

    /* Transfer the subset elements from "x" to "a". */

    for (i = 0; ; i++) {

        jj = subs[0][indx[0]];
        if (jj != NA_INTEGER) {
            ii = jj-1;
            for (j = 1; j < k; j++) {
                jj = subs[j][indx[j]];
                if (jj == NA_INTEGER)
                    break;
                ii += (jj-1) * offset[j];
            }
        }

        if (jj != NA_INTEGER) {
            switch (mode) {
            case LGLSXP:
                LOGICAL(result)[i] = LOGICAL(x)[ii];
                break;
            case INTSXP:
                INTEGER(result)[i] = INTEGER(x)[ii];
                break;
            case REALSXP:
                REAL(result)[i] = REAL(x)[ii];
                break;
            case CPLXSXP:
                COMPLEX(result)[i] = COMPLEX(x)[ii];
                break;
            case STRSXP:
                SET_STRING_ELT(result, i, STRING_ELT(x, ii));
                break;
            case VECSXP:
                if (!DUPVE || NAMEDCNT_EQ_0(x)) {
                    SET_VECTOR_ELT (result, i, VECTOR_ELT(x, ii));
                    INC_NAMEDCNT_0_AS_1(VECTOR_ELT(result,i));
                }
                else
                    SET_VECTOR_ELT (result, i, duplicate(VECTOR_ELT(x,ii)));
                break;
            case RAWSXP:
                RAW(result)[i] = RAW(x)[ii];
                break;
            default:
                errorcall (call, 
                           _("array subscripting not handled for this type"));
                break;
            }
        }
        else { /* jj == NA_INTEGER */
            switch (mode) {
            case LGLSXP:
                LOGICAL(result)[i] = NA_LOGICAL;
                break;
            case INTSXP:
                INTEGER(result)[i] = NA_INTEGER;
                break;
            case REALSXP:
                REAL(result)[i] = NA_REAL;
                break;
            case CPLXSXP:
                COMPLEX(result)[i].r = NA_REAL;
                COMPLEX(result)[i].i = NA_REAL;
                break;
            case STRSXP:
                SET_STRING_ELT(result, i, NA_STRING);
                break;
            case VECSXP:
                SET_VECTOR_ELT_NIL(result, i);
                break;
            case RAWSXP:
                RAW(result)[i] = (Rbyte) 0;
                break;
            default:
                errorcall (call, 
                           _("array subscripting not handled for this type"));
                break;
            }
        }

        j = 0;
        while (++indx[j] >= nsubs[j]) {
            indx[j] = 0;
            if (++j >= k) goto done;
        }
    }

  done: ;

    /* Set up dimnames for result, but don't attach to result yet. */

    SEXP newdimnames;
    PROTECT(dimnames = getAttrib(x, R_DimNamesSymbol));
    PROTECT(dimnamesnames = getAttrib(dimnames, R_NamesSymbol));
    if (TYPEOF(dimnames) == VECSXP) { /* broken code for others in R-2.15.0 */
        PROTECT(newdimnames = allocVector(VECSXP, k));
        for (i = 0; i < k ; i++) {
            if (nsubs[i] > 0 && VECTOR_ELT(dimnames,i) != R_NilValue) {
                SET_VECTOR_ELT(newdimnames, i, allocVector(STRSXP, nsubs[i]));
                ExtractSubset(VECTOR_ELT(dimnames,i), VECTOR_ELT(newdimnames,i),
                              subv[i], call);
            } 
            /* else leave as NULL for 0-length dims */
        }
        setAttrib(newdimnames, R_NamesSymbol, dimnamesnames);
    }
    else
        PROTECT(newdimnames = R_NilValue);

    /* See if dropping down to a vector. */

    int rdims = 0;
    for (i = 0; i < k; i++) {
        if (nsubs[i] != 1 || suppress_drop[i])
            rdims += 1;
    }

    if (rdims <= 1) { /* result is vector without dims, but maybe with names */
        if (newdimnames != R_NilValue) {
            int w = -1;   /* which dimension to take names from, -1 if none */
            for (i = 0; i < k; i++) {
                if (VECTOR_ELT(newdimnames,i) != R_NilValue) {
                    if (w < 0 || nsubs[i] != 1 || suppress_drop[i])
                        w = i;
                    else if (!suppress_drop[w]) {
                        w = -1;
                        break;
                    }
                }
            }
            if (w >= 0)
                setAttrib (result, R_NamesSymbol, VECTOR_ELT(newdimnames,w));
        }
    }

    else { /* not dropping down to a vector */
        SEXP newdims;
        PROTECT (newdims = allocVector(INTSXP, k));
        for (i = 0 ; i < k ; i++)
            INTEGER(newdims)[i] = nsubs[i];
        setAttrib (result, R_DimSymbol, newdims);
        setAttrib (result, R_DimNamesSymbol, newdimnames);
        if (drop == TRUE)
            DropDims(result);
        else if (drop == NA_LOGICAL)
            DropDimsNotSuppressed(result,suppress_drop);
        UNPROTECT(1); /* newdims */
    }

    UNPROTECT(k+7); /* ... + result, dimnames, dimnamesnames, newdimnames,
                             x, s, xdims */

    R_scalar_stack = sv_scalar_stack;
    return result;
}


/* Returns and removes a named argument from the argument list 
   pointed to by args_ptr, and updates args_ptr to account for
   the removal (when it was at the head).

   The search ends as soon as a matching argument is found.  If
   the argument is not found, the argument list is not modified
   and R_NoObject is is returned.

   The caller does not need to protect *args_ptr before.
 */
static SEXP ExtractArg(SEXP *args_ptr, SEXP arg_sym)
{
    SEXP prev_arg = R_NoObject;
    SEXP arg;

    for (arg = *args_ptr; arg != R_NilValue; arg = CDR(arg)) {
	if(TAG(arg) == arg_sym) {
	    if (prev_arg == R_NoObject) /* found at head of args */
		*args_ptr = CDR(arg);
	    else
		SETCDR(prev_arg, CDR(arg));
	    return CAR(arg);
	}
	prev_arg = arg;
    }
    return R_NoObject;
}

/* Extracts the drop argument, if present, from the argument list.
   The argument list will be modified, and the pointer passed changed
   if the first argument is deleted.  The caller does not need to
   protect *args_ptr before.  The value is FALSE, TRUE, or NA_LOGICAL,
   with NA_LOGICAL being the default when no drop argument is present.
   When used as a C boolean, NA_LOGICAL will have the same effect as
   TRUE, but NA_LOGICAL will sometimes allow dropping to be suppressed
   when a vector index has a 1D dim attribute. */

static int ExtractDropArg(SEXP *args_ptr)
{
    SEXP drop_arg = ExtractArg(args_ptr, R_DropSymbol);
    return drop_arg != R_NoObject ? asLogical(drop_arg) : NA_LOGICAL;
}


/* Extracts and, if present, removes the 'exact' argument from the
   argument list.  An integer code giving the desired exact matching
   behavior is returned:
       0  not exact
       1  exact
      -1  not exact, but warn when partial matching is used

   The argument list pointed to by args_ptr may be modified.  The
   caller does not need to protect *args_ptr before.
 */
static int ExtractExactArg(SEXP *args_ptr)
{
    SEXP argval = ExtractArg(args_ptr, R_ExactSymbol);
    if (argval == R_NoObject) return 1; /* Default is true as from R 2.7.0 */
    int exact = asLogical(argval);
    if (exact == NA_LOGICAL) exact = -1;
    return exact;
}


/* Returns simple (positive or negative) index, with no dim attribute, or 
   zero if not so simple. */

static R_INLINE R_len_t simple_index (SEXP s)
{
    if (HAS_ATTRIB(s) && getDimAttrib(s) != R_NilValue)
        return 0;

    switch (TYPEOF(s)) {
    case REALSXP:
        if (LENGTH(s) != 1 || ISNAN(REAL(s)[0])
         || REAL(s)[0] > R_LEN_T_MAX || REAL(s)[0] < -R_LEN_T_MAX)
            return 0;
         return (R_len_t) REAL(s)[0];
    case INTSXP:
        if (LENGTH(s) != 1 || INTEGER(s)[0] == NA_INTEGER)
            return 0;
        return INTEGER(s)[0];
    default:
        return 0;
    }
}


/* Look for the simple case of subscripting an atomic vector with one 
   valid integer or real subscript that is positive or negative (not zero, 
   NA, or out of bounds), with no dim attribute.  Returns the result, or 
   R_NilValue if it's not so simple.  Return result may be on scalar stack,
   if variant allows.

   The arguments x and s do not need to be protected before this
   function is called.  It's OK for x to still be being computed. The
   variant for the return result is the last argument. */

static inline SEXP one_vector_subscript (SEXP x, SEXP s, int variant)
{
    R_len_t ix, n;
    int typeofx;
    SEXP r;

    typeofx = TYPEOF(x);

    if (!isVectorAtomic(x))
        return R_NilValue;

    n = LENGTH(x);
    ix = simple_index (s);

    if (ix == 0 || ix > n || ix < -n)
        return R_NilValue;

    if (ix>0) {
        R_len_t avail;
        ix -= 1;
        if (helpers_is_being_computed(x)) {
            helpers_start_computing_var(x);
            HELPERS_WAIT_IN_VAR (x, avail, ix, n);
        }
        switch (typeofx) {
        case LGLSXP:  
            return ScalarLogicalMaybeConst (LOGICAL(x)[ix]);
        case INTSXP:  
            if (CAN_USE_SCALAR_STACK(variant))
                return PUSH_SCALAR_INTEGER(INTEGER(x)[ix]);
            else
                return ScalarIntegerMaybeConst(INTEGER(x)[ix]);
        case REALSXP: 
            if (CAN_USE_SCALAR_STACK(variant))
                return PUSH_SCALAR_REAL(REAL(x)[ix]);
            else
                return ScalarRealMaybeConst(REAL(x)[ix]);
        case RAWSXP:  
            return ScalarRawMaybeConst (RAW(x)[ix]);
        case STRSXP:  
            return ScalarStringMaybeConst (STRING_ELT(x,ix));
        case CPLXSXP: 
            return ScalarComplexMaybeConst (COMPLEX(x)[ix]);
        default: abort();
        }
    }
    else { /* ix < 0 */

        R_len_t ex;

        WAIT_UNTIL_COMPUTED(x);
        PROTECT(x);
        r = allocVector (typeofx, n-1);

        ix = -ix-1;
        ex = n-ix-1;

        switch (typeofx) {
        case LGLSXP: 
            if (ix!=0) memcpy(LOGICAL(r), LOGICAL(x), ix * sizeof *LOGICAL(r));
            if (ex!=0) memcpy(LOGICAL(r)+ix, LOGICAL(x)+ix+1, 
                                                      ex * sizeof *LOGICAL(r));
            break;
        case INTSXP: 
            if (ix!=0) memcpy(INTEGER(r), INTEGER(x), ix * sizeof *INTEGER(r));
            if (ex!=0) memcpy(INTEGER(r)+ix, INTEGER(x)+ix+1, 
                                                      ex * sizeof *INTEGER(r));
            break;
        case REALSXP: 
            if (ix!=0) memcpy(REAL(r), REAL(x), ix * sizeof *REAL(r));
            if (ex!=0) memcpy(REAL(r)+ix, REAL(x)+ix+1, ex * sizeof *REAL(r));
            break;
        case RAWSXP: 
            if (ix!=0) memcpy(RAW(r), RAW(x), ix * sizeof *RAW(r));
            if (ex!=0) memcpy(RAW(r)+ix, RAW(x)+ix+1, ex * sizeof *RAW(r));
            break;
        case STRSXP: 
            if (ix!=0) copy_string_elements (r, 0, x, 0, ix);
            if (ex!=0) copy_string_elements (r, ix, x, ix+1, ex); 
            break;
        case CPLXSXP: 
            if (ix!=0) memcpy(COMPLEX(r), COMPLEX(x), ix * sizeof *COMPLEX(r));
            if (ex!=0) memcpy(COMPLEX(r)+ix, COMPLEX(x)+ix+1, 
                                                      ex * sizeof *COMPLEX(r));
            break;
        default: abort();
        }

        UNPROTECT(1);
        return r;
    }
}


/* Look for the simple case of subscripting an atomic matrix with two
   valid integer or real subscript that are positive (not negative, zero, 
   NA, or out of bounds), with no dim attribute.  Returns the result, or 
   R_NilValue if it's not so simple.  The arguments x, dim, s1, and s2 do 
   not need to be protected before this function is called. It's OK for x to 
   still be being computed. The variant for the return result is the last 
   argument. */

static inline SEXP two_matrix_subscripts (SEXP x, SEXP dim, SEXP s1, SEXP s2, 
                                          int variant)
{
    R_len_t ix1, ix2, nrow, ncol, avail, e;

    if (!isVectorAtomic(x))
        return R_NilValue;

    nrow = INTEGER(dim)[0];
    ix1 = simple_index (s1);
    if (ix1 <= 0 || ix1 > nrow)
        return R_NilValue;

    ncol = INTEGER(dim)[1];
    ix2 = simple_index (s2);
    if (ix2 <= 0 || ix2 > ncol)
        return R_NilValue;

    e = (ix1 - 1) + nrow * (ix2 - 1);

    if (helpers_is_being_computed(x)) {
        helpers_start_computing_var(x);
        HELPERS_WAIT_IN_VAR (x, avail, e, LENGTH(x));
    }

    switch (TYPEOF(x)) {
    case LGLSXP:  
        return ScalarLogicalMaybeConst (LOGICAL(x)[e]);
    case INTSXP:  
        if (CAN_USE_SCALAR_STACK(variant))
            return PUSH_SCALAR_INTEGER(INTEGER(x)[e]);
        else
            return ScalarIntegerMaybeConst(INTEGER(x)[e]);
    case REALSXP: 
        if (CAN_USE_SCALAR_STACK(variant))
            return PUSH_SCALAR_REAL(REAL(x)[e]);
        else
            return ScalarRealMaybeConst(REAL(x)[e]);
    case RAWSXP:  
        return ScalarRawMaybeConst (RAW(x)[e]);
    case STRSXP:  
        return ScalarStringMaybeConst (STRING_ELT(x,e));
    case CPLXSXP: 
        return ScalarComplexMaybeConst (COMPLEX(x)[e]);
    default: abort();
    }
}


/* The "[" subset operator.
 * This provides the most general form of subsetting. */

static SEXP do_subset_dflt_seq (SEXP call, SEXP op, SEXP x, SEXP sb1, SEXP sb2,
                                SEXP subs, SEXP rho, int variant, int64_t seq);

static SEXP do_subset(SEXP call, SEXP op, SEXP args, SEXP rho, int variant)
{
    SEXP ans;
    int argsevald = 0;

    /* If we can easily determine that this will be handled by
       subset_dflt and has one or two index arguments in total, try to
       evaluate the first index with VARIANT_SEQ, so it may come as a
       range rather than a vector.  Also, evaluate the array with
       VARIANT_UNCLASS and VARIANT_PENDING_OK, and perhaps evaluate
       indexes with VARIANT_SCALAR_STACK (should be safe, since there
       will be no later call of eval). */

    if (args != R_NilValue && CAR(args) != R_DotsSymbol) {
        SEXP ixlist = CDR(args);
        SEXP array;
        PROTECT(array = evalv (CAR(args), rho, VARIANT_UNCLASS | 
                                               VARIANT_PENDING_OK));
        int obj = isObject(array);
        if (R_variant_result) {
            obj = 0;
            R_variant_result = 0;
        }
        if (obj) {
            args = CONS(array,ixlist);
            argsevald = -1;
            UNPROTECT(1);  /* array */
        }
        else if (ixlist == R_NilValue || TAG(ixlist) != R_NilValue 
                                      || CAR(ixlist) == R_DotsSymbol) {
            args = evalListKeepMissing(ixlist,rho);
            UNPROTECT(1);  /* array */
            return do_subset_dflt_seq (call, op, array, R_NoObject, R_NoObject,
                                       args, rho, variant, 0);
        }
        else {
            SEXP r;
            BEGIN_PROTECT3 (sb1, sb2, remargs);
            SEXP sv_scalar_stack = R_scalar_stack;
            SEXP ixlist2 = CDR(ixlist);
            int64_t seq = 0;
            sb1 = evalv (CAR(ixlist), rho, 
                         CDR(ixlist2)==R_NilValue /* no more than 2 arguments */
                          ? VARIANT_SEQ | VARIANT_SCALAR_STACK_OK |
                            VARIANT_MISSING_OK | VARIANT_PENDING_OK
                          : VARIANT_SCALAR_STACK_OK | 
                            VARIANT_MISSING_OK | VARIANT_PENDING_OK);
            if (R_variant_result) {
                seq = R_variant_seq_spec;
                R_variant_result = 0;
            }
            remargs = ixlist2;
            sb2 = R_NoObject;
            if (ixlist2 != R_NilValue && TAG(ixlist2) == R_NilValue 
                                      && CAR(ixlist2) != R_DotsSymbol) {
                sb2 = evalv (CAR(ixlist2), rho,
                             VARIANT_SCALAR_STACK_OK | VARIANT_MISSING_OK);
                remargs = CDR(ixlist2);
            }
            if (remargs != R_NilValue)
                remargs = evalList_v (remargs, rho, VARIANT_SCALAR_STACK_OK |
                                      VARIANT_PENDING_OK | VARIANT_MISSING_OK);
            if (sb2 == R_MissingArg && isSymbol(CAR(ixlist2))) {
                remargs = CONS(sb2,remargs);
                SET_MISSING (remargs, R_isMissing(CAR(ixlist2),rho));
                sb2 = R_NoObject;
            }
            if (sb1 == R_MissingArg && isSymbol(CAR(ixlist))) {
                if (sb2 != R_NoObject) {
                    remargs = CONS(sb2,remargs);
                    sb2 = R_NoObject;
                }
                remargs = CONS(sb1,remargs);
                SET_MISSING (remargs, R_isMissing(CAR(ixlist),rho));
                sb1 = R_NoObject;
            }
            else if (sb1 != R_NoObject)
                WAIT_UNTIL_COMPUTED(sb1);
            wait_until_arguments_computed(remargs);
            r = do_subset_dflt_seq (call, op, array, sb1, sb2,
                                    remargs, rho, variant, seq);
            R_scalar_stack = sv_scalar_stack;
            END_PROTECT;
            UNPROTECT(1); /* array */
            return ON_SCALAR_STACK(r) ? PUSH_SCALAR(r) : r;
        }
    }

    /* If the first argument is an object and there is an */
    /* appropriate method, we dispatch to that method, */
    /* otherwise we evaluate the arguments and fall through */
    /* to the generic code below.  Note that evaluation */
    /* retains any missing argument indicators. */

    if(DispatchOrEval(call, op, "[", args, rho, &ans, 0, argsevald)) {
/*     if(DispatchAnyOrEval(call, op, "[", args, rho, &ans, 0, 0)) */
	if (NAMEDCNT_GT_0(ans))
	    SET_NAMEDCNT_MAX(ans);    /* IS THIS NECESSARY? */
	return(ans);
    }

    /* Method dispatch has failed, we now */
    /* run the generic internal code. */
    SEXP x = CAR(ans);
    args = CDR(ans);

    if (args == R_NilValue || TAG(args) != R_NilValue)
        return do_subset_dflt_seq (call, op, x, R_NoObject, R_NoObject, 
                                   args, rho, variant, 0);
    else if (CDR(args) == R_NilValue || TAG(CDR(args)) != R_NilValue)
        return do_subset_dflt_seq (call, op, x, CAR(args), R_NoObject,
                                   CDR(args), rho, variant, 0);
    else
        return do_subset_dflt_seq (call, op, x, CAR(args), CADR(args),
                                   CDDR(args), rho, variant, 0);
}


/* N.B.  do_subset_dflt is sometimes called directly from outside this module. 
   It doesn't have the "seq" argument of do_subset_dflt_seq, and takes all
   arguments as an arg list. */

SEXP attribute_hidden do_subset_dflt (SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP x = CAR(args);
    args = CDR(args);
    
    if (args == R_NilValue || TAG(args) != R_NilValue)
        return do_subset_dflt_seq (call, op, x, R_NoObject, R_NoObject, 
                                   args, rho, 0, 0);
    else if (CDR(args) == R_NilValue || TAG(CDR(args)) != R_NilValue)
        return do_subset_dflt_seq (call, op, x, CAR(args), R_NoObject,
                                   CDR(args), rho, 0, 0);
    else
        return do_subset_dflt_seq (call, op, x, CAR(args), CADR(args),
                                   CDDR(args), rho, 0, 0);
}

/* The "seq" argument below is non-zero if the first subscript is a sequence
   specification (a variant result), in which case it encodes the start, 
   length, and whether .. properties of the sequence.

   The first argument (the array, x) is passed separately rather than
   as part of an argument list, for efficiency.  If sb1 is not R_NoObject, 
   it is the first subscript, which has no tag.  Similarly for sb2.
   Remaining subscripts and other arguments are in the pairlist subs.

   May return its result on the scalar stack, depending on variant.

   Note:  x, sb1, and subs need not be protected on entry. */

static SEXP do_subset_dflt_seq (SEXP call, SEXP op, SEXP x, SEXP sb1, SEXP sb2,
                                SEXP subs, SEXP rho, int variant, int64_t seq)
{
    int drop, i, nsubs, type;
    SEXP ans, ax, px;

    if (seq == 0 && x != R_NilValue && sb1 != R_NoObject) {

        /* Check for one subscript, handling simple cases like this */
        if (sb2 == R_NoObject && subs == R_NilValue) { 
            SEXP attr = ATTRIB(x);
            if (attr != R_NilValue) {
                if (TAG(attr) == R_DimSymbol && CDR(attr) == R_NilValue) {
                    SEXP dim = CAR(attr);
                    if (TYPEOF(dim) == INTSXP && LENGTH(dim) == 1)
                        attr = R_NilValue;  /* only a dim attribute, 1D */
                }
            }
            if (attr == R_NilValue) {
                SEXP r = one_vector_subscript (x, sb1, variant);
                if (r != R_NilValue)
                    return r;
            }
        }

        /* Check for two subscripts, handling simple cases like this */
        else if (sb2 != R_NoObject && subs == R_NilValue) {
            SEXP attr = ATTRIB(x);
            if (TAG(attr) == R_DimSymbol && CDR(attr) == R_NilValue) {
                SEXP dim = CAR(attr);
                if (TYPEOF(dim) == INTSXP && LENGTH(dim) == 2) {
                    /* x is a matrix */
                    SEXP r = two_matrix_subscripts (x, dim, sb1, sb2, variant);
                    if (r != R_NilValue)
                        return r;
                }
            }
        }
    }

    /* This was intended for compatibility with S, */
    /* but in fact S does not do this. */

    if (x == R_NilValue)
	return x;

    PROTECT3(x,sb1,sb2);

    drop = ExtractDropArg(&subs);
    if (sb2 != R_NoObject)
        subs = CONS (sb2, subs);
    if (sb1 != R_NoObject) 
        subs = CONS (sb1, subs);
    PROTECT(subs);

    WAIT_UNTIL_COMPUTED(x);

    nsubs = length(subs);
    type = TYPEOF(x);

    /* Here coerce pair-based objects into generic vectors. */
    /* All subsetting takes place on the generic vector form. */

    ax = x;
    if (isVector(x))
	PROTECT(ax);
    else if (isPairList(x)) {
	SEXP dim = getDimAttrib(x);
	int ndim = length(dim);
	if (ndim > 1) {
	    PROTECT(ax = allocArray(VECSXP, dim));
	    setAttrib(ax, R_DimNamesSymbol, getAttrib(x, R_DimNamesSymbol));
	    setAttrib(ax, R_NamesSymbol, getAttrib(x, R_DimNamesSymbol));
	}
	else {
	    PROTECT(ax = allocVector(VECSXP, length(x)));
	    setAttrib(ax, R_NamesSymbol, getAttrib(x, R_NamesSymbol));
	}
        SET_NAMEDCNT(ax,NAMEDCNT(x));
	for(px = x, i = 0 ; px != R_NilValue ; px = CDR(px))
	    SET_VECTOR_ELT(ax, i++, CAR(px));
    }
    else
        nonsubsettable_error(call,x);

    /* This is the actual subsetting code. */
    /* The separation of arrays and matrices is purely an optimization. */

    if(nsubs < 2)
	PROTECT(ans = VectorSubset(ax, subs, seq, drop, call));
    else {
        SEXP xdims = getDimAttrib(x);
	if (nsubs != length(xdims))
	    errorcall(call, _("incorrect number of dimensions"));
	if (nsubs == 2)
	    ans = MatrixSubset(ax, subs, call, drop, seq);
	else
	    ans = ArraySubset(ax, subs, call, drop, xdims, nsubs);
	PROTECT(ans);
    }

    /* Note: we do not coerce back to pair-based lists. */

    if (type == LANGSXP) {
	ax = ans;
	PROTECT(ans = allocList(LENGTH(ax)));
	if (ans != R_NilValue) {
	    SET_TYPEOF(ans, LANGSXP);
            for (px = ans, i = 0 ; px != R_NilValue ; px = CDR(px))
                SETCAR(px, VECTOR_ELT(ax, i++));
            setAttrib(ans, R_DimSymbol, getDimAttrib(ax));
            setAttrib(ans, R_DimNamesSymbol, getAttrib(ax, R_DimNamesSymbol));
            setAttrib(ans, R_NamesSymbol, getAttrib(ax, R_NamesSymbol));
            SET_NAMEDCNT_MAX(ans);
        }
        UNPROTECT(2);
        PROTECT(ans);
    }

    if (HAS_ATTRIB(ans)) { /* remove probably erroneous attr's */
	setAttrib(ans, R_TspSymbol, R_NilValue);
#ifdef _S4_subsettable
	if(!IS_S4_OBJECT(x))
#endif
	    setAttrib(ans, R_ClassSymbol, R_NilValue);
    }
    UNPROTECT(6);

    return ans;
}


/* The [[ subset operator.  It needs to be fast. */

static SEXP do_subset2_dflt_x (SEXP call, SEXP op, SEXP x, SEXP sb1, SEXP sb2,
                               SEXP subs, SEXP rho, int variant);

static SEXP do_subset2(SEXP call, SEXP op, SEXP args, SEXP rho, int variant)
{
    int fast_sub = VARIANT_KIND(variant) == VARIANT_FAST_SUB;
    SEXP ans;
        
    /* If we can easily determine that this will be handled by
       subset2_dflt, evaluate the array with VARIANT_UNCLASS and
       VARIANT_PENDING_OK, and perhaps evaluate indexes with
       VARIANT_SCALAR_STACK_OK (should be safe, since there will be
       no later call of eval). */

    if (fast_sub || args != R_NilValue && CAR(args) != R_DotsSymbol) {

        SEXP array, ixlist;
        int obj;

        if (fast_sub) {
            ixlist = args;
            PROTECT(array = R_fast_sub_var);
            obj = isObject(array);
        }
        else {
            ixlist = CDR(args);
            array = CAR(args);
            PROTECT(array = evalv (array, rho, VARIANT_UNCLASS | 
                                               VARIANT_PENDING_OK));
            obj = isObject(array);
            if (R_variant_result) {
                obj = 0;
                R_variant_result = 0;
            }
        }

        if (obj) {
            args = CONS(array,ixlist);
            UNPROTECT(1);  /* array */
        }
        else if (ixlist == R_NilValue || TAG(ixlist) != R_NilValue 
                                      || CAR(ixlist) == R_DotsSymbol) {
            args = evalListKeepMissing(ixlist,rho);
            UNPROTECT(1);  /* array */
            return do_subset2_dflt_x (call, op, array, R_NoObject, R_NoObject,
                                      args, rho, variant);
        }
        else {
            SEXP r;
            BEGIN_PROTECT3 (sb1, sb2, remargs);
            SEXP sv_scalar_stack = R_scalar_stack;
            SEXP ixlist2 = CDR(ixlist);
            sb1 = evalv (CAR(ixlist), rho, VARIANT_SCALAR_STACK_OK | 
                         VARIANT_MISSING_OK | VARIANT_PENDING_OK);
            remargs = ixlist2;
            sb2 = R_NoObject;
            if (sb1 == R_MissingArg && isSymbol(CAR(ixlist))) {
                remargs = CONS(sb1,remargs);
                SET_MISSING (remargs, R_isMissing(CAR(ixlist),rho));
                sb1 = R_NoObject;
            }
            else if (ixlist2 != R_NilValue && TAG(ixlist2) == R_NilValue 
                                      && CAR(ixlist2) != R_DotsSymbol) {
                sb2 = evalv (CAR(ixlist2), rho,
                             VARIANT_SCALAR_STACK_OK | VARIANT_MISSING_OK);
                remargs = CDR(ixlist2);
            }
            if (remargs != R_NilValue)
                remargs = evalList_v (remargs, rho, VARIANT_SCALAR_STACK_OK |
                                      VARIANT_PENDING_OK | VARIANT_MISSING_OK);
            if (sb2 == R_MissingArg && isSymbol(CAR(ixlist2))) {
                remargs = CONS(sb2,remargs);
                SET_MISSING (remargs, R_isMissing(CAR(ixlist2),rho));
                sb2 = R_NoObject;
            }
            if (sb1 != R_NoObject)
                WAIT_UNTIL_COMPUTED(sb1);
            wait_until_arguments_computed(remargs);
            r = do_subset2_dflt_x (call, op, array, sb1, sb2,
                                   remargs, rho, variant);
            R_scalar_stack = sv_scalar_stack;
            END_PROTECT;
            UNPROTECT(1);  /* array */
            return ON_SCALAR_STACK(r) ? PUSH_SCALAR(r) : r;
        }
    }
    else {
        if (fast_sub) {
            args = CONS (R_fast_sub_var, args);
        }
    }

    PROTECT(args);

    /* If the first argument is an object and there is an */
    /* appropriate method, we dispatch to that method, */
    /* otherwise we evaluate the arguments and fall through */
    /* to the generic code below.  Note that evaluation */
    /* retains any missing argument indicators. */

    if(DispatchOrEval(call, op, "[[", args, rho, &ans, 0, 0)) {
/*     if(DispatchAnyOrEval(call, op, "[[", args, rho, &ans, 0, 0)) */
	if (NAMEDCNT_GT_0(ans))
	    SET_NAMEDCNT_MAX(ans);    /* IS THIS NECESSARY? */
    }
    else {

        /* Method dispatch has failed, we now */
        /* run the generic internal code. */

        UNPROTECT(1);
        PROTECT(ans);

        SEXP x = CAR(ans);
        args = CDR(ans);
    
        if (args == R_NilValue || TAG(args) != R_NilValue)
            ans = do_subset2_dflt_x (call, op, x, R_NoObject, R_NoObject, 
                                     args, rho, variant);
        else if (CDR(args) == R_NilValue || TAG(CDR(args)) != R_NilValue)
            ans = do_subset2_dflt_x (call, op, x, CAR(args), R_NoObject,
                                     CDR(args), rho, variant);
        else
            ans = do_subset2_dflt_x (call, op, x, CAR(args), CADR(args),
                                     CDDR(args), rho, variant);
    }

    UNPROTECT(1);
    return ans;
}

SEXP attribute_hidden do_subset2_dflt(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP x = CAR(args);
    args = CDR(args);
    
    if (args == R_NilValue || TAG(args) != R_NilValue)
        return do_subset2_dflt_x (call, op, x, R_NoObject, R_NoObject, 
                                  args, rho, 0);
    else if (CDR(args) == R_NilValue || TAG(CDR(args)) != R_NilValue)
        return do_subset2_dflt_x (call, op, x, CAR(args), R_NoObject,
                                  CDR(args), rho, 0);
    else
        return do_subset2_dflt_x (call, op, x, CAR(args), CADR(args),
                                  CDDR(args), rho, 0);
}

static SEXP do_subset2_dflt_x (SEXP call, SEXP op, SEXP x, SEXP sb1, SEXP sb2,
                               SEXP subs, SEXP rho, int variant)
{
    int offset;
    SEXP ans;

    if (isVector(x) && sb1 != R_NoObject) {

        /* Check for one subscript, handling simple cases.  Doesn't handle
           simple cases with two subscripts here yet. */

        if (sb2 == R_NoObject && subs == R_NilValue 
              && isVectorAtomic(sb1) && LENGTH(sb1) == 1) { 
            int str_sym_sub = isString(sb1) || isSymbol(sb1);
            offset = get1index(sb1, 
                          str_sym_sub ? getAttrib(x,R_NamesSymbol) : R_NilValue,
                          LENGTH(x), 0/*exact*/, 0, call);
            if (offset >= 0 && offset < LENGTH(x)) {  /* not out of bounds */
                if (isVectorList(x)) {
                    ans = VECTOR_ELT(x, offset);
                    if (NAMEDCNT_GT_0(x))
                        SET_NAMEDCNT_NOT_0(ans);
                    if ((VARIANT_KIND(variant) == VARIANT_QUERY_UNSHARED_SUBSET 
                          || VARIANT_KIND(variant) == VARIANT_FAST_SUB)
                         && !NAMEDCNT_GT_1(x) && !NAMEDCNT_GT_1(ans))
                        R_variant_result = 1;
                }
                else if (TYPEOF(x) == INTSXP && CAN_USE_SCALAR_STACK(variant))
                    ans = PUSH_SCALAR_INTEGER (INTEGER(x)[offset]);
                else if (TYPEOF(x) == REALSXP && CAN_USE_SCALAR_STACK(variant))
                    ans = PUSH_SCALAR_REAL (REAL(x)[offset]);
                else {
                    ans = allocVector(TYPEOF(x), 1);
                    copy_elements (ans, 0, 0, x, offset, 0, 1);
                }
                return ans;
            }
        }
    }

    SEXP dims, dimnames;
    int i, drop, ndims, nsubs;
    int pok, exact = -1;

    /* This was intended for compatibility with S, */
    /* but in fact S does not do this. */

    if (x == R_NilValue)
        return x;

    PROTECT(x);

    drop = ExtractDropArg(&subs);  /* a "drop" arg is tolerated, but ignored */
    exact = ExtractExactArg(&subs);
    PROTECT(subs);
    if (sb1 == R_NoObject && subs != R_NilValue) {
        sb1 = CAR(subs);
        subs = CDR(subs);
    }
    if (sb2 == R_NoObject && subs != R_NilValue) {
        sb2 = CAR(subs);
        subs = CDR(subs);
    }

    WAIT_UNTIL_COMPUTED(x);

    /* Is partial matching ok?  When the exact arg is NA, a warning is
       issued if partial matching occurs.
     */
    if (exact == -1)
	pok = exact;
    else
	pok = !exact;

    /* Get the subscripting and dimensioning information */
    /* and check that any array subscripting is compatible. */

    nsubs = length(subs) + (sb1 != R_NoObject) + (sb2 != R_NoObject);
    if (nsubs == 0)
	errorcall(call, _("no index specified"));
    dims = getDimAttrib(x);
    ndims = length(dims);
    if(nsubs > 1 && nsubs != ndims)
	errorcall(call, _("incorrect number of subscripts"));

    /* code to allow classes to extend environment */
    if(TYPEOF(x) == S4SXP) {
        x = R_getS4DataSlot(x, ANYSXP);
	if(x == R_NilValue)
	  errorcall(call, _("this S4 class is not subsettable"));
        UNPROTECT(1);
        PROTECT(x);
    }

    /* split out ENVSXP for now */
    if( TYPEOF(x) == ENVSXP ) {
        if (nsubs != 1 || !isString(sb1) || length(sb1) != 1)
            errorcall(call, _("wrong arguments for subsetting an environment"));
        SEXP sym = installed_already (translateChar (STRING_ELT(sb1,0)));
        if (sym == R_NoObject)
            ans = R_NilValue;
        else {
            ans = findVarInFrame (x, sym);
            if (ans == R_UnboundValue)
                ans = R_NilValue;
            else {
                if (TYPEOF(ans) == PROMSXP)
                    ans = forcePromise(ans);
                SET_NAMEDCNT_NOT_0(ans);
            }
        }
        UNPROTECT(2);
        return(ans);
    }

    /* back to the regular program */
    if (!(isVector(x) || isList(x) || isLanguage(x)))
	nonsubsettable_error(call,x);

    int max_named = NAMEDCNT(x);

    if (nsubs == 1) { /* simple or vector indexing */

        int str_sym_sub = isString(sb1) || isSymbol(sb1);
	int len = length(sb1);
        int i, lenx;

        for (i = 1; i < len; i++) {
            if (!isVectorList(x) && !isPairList(x))
                errorcall(call,_("recursive indexing failed at level %d\n"),i);
            lenx = length(x);
            offset = get1index(sb1, 
                       str_sym_sub ? getAttrib(x, R_NamesSymbol) : R_NilValue,
                       lenx, pok, i-1, call);
            if (offset < 0 || offset >= lenx)
                errorcall(call, _("no such index at level %d\n"), i);
            if (isPairList(x)) {
                x = CAR(nthcdr(x, offset));
                max_named = MAX_NAMEDCNT;
            } 
            else {
                x = VECTOR_ELT(x, offset);
                int nm = NAMEDCNT(x);
                if (nm > max_named) 
                    max_named = nm;
            }
        }

        lenx = length(x);
	offset = get1index(sb1, 
                   str_sym_sub ? getAttrib(x, R_NamesSymbol) : R_NilValue,
                   lenx, pok, len > 1 ? len-1 : -1, call);
	if (offset < 0 || offset >= lenx) {
	    /* a bold attempt to get the same behaviour for $ and [[ */
	    if (offset >= lenx && PRIMVAL(op) == 0 /* .el.methods */
             || offset < 0 && (isNewList(x) ||
			       isExpression(x) ||
			       isList(x) ||
			       isLanguage(x))) {
		UNPROTECT(2);
		return R_NilValue;
	    }
	    else
                out_of_bounds_error(call);
	}
    } else { /* matrix or array indexing */

	dimnames = getAttrib(x, R_DimNamesSymbol);

	int ndn = length(dimnames); /* Number of dimnames. Unlikely anything
                                       but or 0 or nsubs, but just in case... */
        R_len_t indx[nsubs];

	for (i = 0; i < nsubs; i++) {
            SEXP ix;
            if (i == 0)
                ix = sb1;
            else if (i == 1)
                ix = sb2;
            else {
                ix = CAR(subs);
                subs = CDR(subs);
            }
	    indx[i] = get1index(ix, i<ndn ? VECTOR_ELT(dimnames,i) : R_NilValue,
			        INTEGER(dims)[i], pok, -1, call);
	    if (indx[i] < 0 || indx[i] >= INTEGER(dims)[i])
		out_of_bounds_error(call);
	}
	offset = 0;
	for (i = nsubs-1; i > 0; i--)
	    offset = (offset + indx[i]) * INTEGER(dims)[i-1];
	offset += indx[0];
    }

    if (isPairList(x)) {
	ans = CAR(nthcdr(x, offset));
        SET_NAMEDCNT_MAX(ans);
    }
    else if (isVectorList(x)) {
	ans = VECTOR_ELT(x, offset);
	if (max_named > 0)
            SET_NAMEDCNT_NOT_0(ans);
        if ((VARIANT_KIND(variant) == VARIANT_QUERY_UNSHARED_SUBSET 
              || VARIANT_KIND(variant) == VARIANT_FAST_SUB)
             && max_named <= 1 && !NAMEDCNT_GT_1(ans))
            R_variant_result = 1;
    }
    else if (TYPEOF(x) == INTSXP && CAN_USE_SCALAR_STACK(variant))
        ans = PUSH_SCALAR_INTEGER (INTEGER(x)[offset]);
    else if (TYPEOF(x) == REALSXP && CAN_USE_SCALAR_STACK(variant))
        ans = PUSH_SCALAR_REAL (REAL(x)[offset]);
    else {
	ans = allocVector(TYPEOF(x), 1);
        copy_elements (ans, 0, 0, x, offset, 0, 1);
    }
    UNPROTECT(2);
    return ans;
}

/* Below is used to implement 'lengths'. */
SEXP attribute_hidden dispatch_subset2(SEXP x, R_xlen_t i, SEXP call, SEXP rho)
{
    static SEXP bracket_op = NULL;
    SEXP args, x_elt;
    if (isObject(x)) {
	if (bracket_op == NULL)
            bracket_op = R_Primitive("[[");
        PROTECT(args = list2(x, ScalarReal(i + 1)));
        x_elt = do_subset2(call, bracket_op, args, rho, 0);
        UNPROTECT(1);
    } else {
      // FIXME: throw error if not a list
	x_elt = VECTOR_ELT(x, i);
    }
    return(x_elt);
}


/* The $ subset operator.
   We need to be sure to only evaluate the first argument.
   The second will be a symbol that needs to be matched, not evaluated.
*/
static SEXP do_subset3(SEXP call, SEXP op, SEXP args, SEXP env, int variant)
{
    SEXP from, what, ans, input, ncall;

    SEXP string = R_NilValue;
    SEXP name = R_NilValue;
    int argsevald = 0;

    if (VARIANT_KIND(variant) == VARIANT_FAST_SUB) {

        /* Fast interface: object to be subsetted comes already evaluated. */

        what = CAR(args);
        from = R_fast_sub_var;
        argsevald = 1;
    }

    else {  /* regular SPECIAL interface */

        checkArity(op, args);
        what = CADR(args);
        from = CAR(args);

        if (from != R_DotsSymbol) {
            from = evalv (from, env, VARIANT_ONE_NAMED | VARIANT_UNCLASS);
            argsevald = 1;
        }
    }

    if (TYPEOF(what) == PROMSXP)
        what = PRCODE(what);
    if (isSymbol(what))
        name = what;
    else if (isString(what) && LENGTH(what) > 0) 
        string = STRING_ELT(what,0);
    else
	errorcall(call, _("invalid subscript type '%s'"), 
                        type2char(TYPEOF(what)));

    if (argsevald) {
        if (isObject(from) && ! (R_variant_result & VARIANT_UNCLASS_FLAG))
            PROTECT(from);
        else {
            R_variant_result = 0;
            return R_subset3_dflt (from, string, name, call, variant);
        }
    }

    /* first translate CADR of args into a string so that we can
       pass it down to DispatchorEval and have it behave correctly */

    input = allocVector(STRSXP,1);

    if (name!=R_NilValue)
	SET_STRING_ELT(input, 0, PRINTNAME(name));
    else
	SET_STRING_ELT(input, 0, string);

    /* replace the second argument with a string */

    /* Previously this was SETCADR(args, input); */
    /* which could cause problems when "from" was */
    /* ..., as in PR#8718 */
    PROTECT(args = CONS(from, CONS(input, R_NilValue)));
    /* Change call used too, for compatibility with
       R-2.15.0:  It's accessible using "substitute", 
       and was a string in R-2.15.0. */
    PROTECT(ncall = LCONS(CAR(call), CONS(CADR(call), CONS(input,R_NilValue))));

    /* If the first argument is an object and there is */
    /* an approriate method, we dispatch to that method, */
    /* otherwise we evaluate the arguments and fall */
    /* through to the generic code below.  Note that */
    /* evaluation retains any missing argument indicators. */

    if(DispatchOrEval(ncall, op, "$", args, env, &ans, 0, argsevald)) {
        UNPROTECT(2+argsevald);
	if (NAMEDCNT_GT_0(ans))         /* IS THIS NECESSARY? */
	    SET_NAMEDCNT_MAX(ans);
	return ans;
    }

    ans = R_subset3_dflt(CAR(ans), string, name, call, variant);
    UNPROTECT(2+argsevald);
    return ans;
}

/* Used above and in eval.c.  The field to extract is specified by either 
   the "input" argument or the "name" argument, or both.  Protects x. */

SEXP attribute_hidden R_subset3_dflt(SEXP x, SEXP input, SEXP name, SEXP call,
                                     int variant)
{
    const char *cinp, *ctarg;
    int mtch;
    SEXP y;

     /* The mechanism to allow  a class extending "environment" */
    if( IS_S4_OBJECT(x) && TYPEOF(x) == S4SXP ){
        PROTECT(x);
        x = R_getS4DataSlot(x, ANYSXP);
        UNPROTECT(1);
	if(x == R_NilValue)
	    errorcall(call, "$ operator not defined for this S4 class");
    }

    PROTECT(x);

    if (isPairList(x)) {

	SEXP xmatch = R_NilValue;
	int havematch;
        if (name!=R_NilValue) {
            /* Quick check for exact match by name */
            for (y = x; y != R_NilValue; y = CDR(y))
                if (TAG(y)==name) {
                    y = CAR(y);
                    goto found_pairlist;
                }
        }
        cinp = input==R_NilValue ? CHAR(PRINTNAME(name)) : translateChar(input);
	havematch = 0;
	for (y = x ; y != R_NilValue ; y = CDR(y)) {
            if (TAG(y) == R_NilValue)
                continue;
            ctarg = CHAR(PRINTNAME(TAG(y)));
	    mtch = ep_match_strings(ctarg, cinp);
	    if (mtch>0) /* exact */ {
		y = CAR(y);
                goto found_pairlist;
            }
            else if (mtch<0) /* partial */ {
		havematch++;
		xmatch = y;
            }
	}
	if (havematch == 1) { /* unique partial match */
	    if(R_warn_partial_match_dollar) {
                ctarg = CHAR(PRINTNAME(TAG(xmatch)));
		warningcall(call, _("partial match of '%s' to '%s'"),
			    cinp, ctarg);
            }
	    y = CAR(xmatch);
            goto found_pairlist;
	}

        UNPROTECT(1);
	return R_NilValue;

      found_pairlist:
        SET_NAMEDCNT_MAX(y);
        UNPROTECT(1);
        return y;
    }

    else if (isVectorList(x)) {

	int i, n, havematch, imatch=-1;
        SEXP str_elt;
        SEXP nlist = getAttrib(x, R_NamesSymbol);
        cinp = input==R_NilValue ? CHAR(PRINTNAME(name)) : translateChar(input);
	n = length(nlist);
	havematch = 0;
	for (i = 0 ; i < n ; i = i + 1) {
            str_elt = STRING_ELT (nlist, i);
            ctarg = translateChar(str_elt);
	    mtch = ep_match_strings(ctarg, cinp);
            if (mtch>0) /* exact */ {
		y = VECTOR_ELT(x, i);
                goto found_veclist;
            }
	    else if (mtch<0) /* partial */ {
		havematch++;
		imatch = i;
	    }
	}
	if (havematch == 1) { /* unique partial match */
	    if (R_warn_partial_match_dollar) {
                str_elt = STRING_ELT (nlist, imatch);
                ctarg = TYPEOF(str_elt)==CHARSXP ? translateChar(str_elt)
                                                 : CHAR(PRINTNAME(str_elt));
		warningcall(call, _("partial match of '%s' to '%s'"),
			    cinp, ctarg);
	    }
	    y = VECTOR_ELT(x, imatch);

            /* OLD COMMENT: Partial matches can cause aliasing in
               eval.c:evalseq This is overkill, but alternative ways
               to prevent the aliasing appear to be even worse.
               SHOULD REVISIT. */
            SET_NAMEDCNT_MAX(y);

	    goto found_veclist;
	}

        UNPROTECT(1);
	return R_NilValue;

      found_veclist:
        if (NAMEDCNT_GT_0(x) && NAMEDCNT_EQ_0(y))
            SET_NAMEDCNT(y,1);
        if ((VARIANT_KIND(variant) == VARIANT_QUERY_UNSHARED_SUBSET 
               || VARIANT_KIND(variant) == VARIANT_FAST_SUB) 
             && !NAMEDCNT_GT_1(x) && !NAMEDCNT_GT_1(y))
            R_variant_result = 1;

        UNPROTECT(1);
        return y;
    }
    else if (isEnvironment(x)) {
        if (name==R_NilValue) {
            name = installed_already (translateChar(input));
            if (name == R_NoObject) {
                UNPROTECT(1);
                return R_NilValue;
            }
        }

        y = findVarInFrame (x, name);
        if (y == R_UnboundValue)
            y = R_NilValue;
        else {
             if (TYPEOF(y) == PROMSXP)
                 y = forcePromise(y);
             else {
                 SET_NAMEDCNT_NOT_0(y);
                 if ((VARIANT_KIND(variant) == VARIANT_QUERY_UNSHARED_SUBSET 
                        || VARIANT_KIND(variant) == VARIANT_FAST_SUB) 
                      && !NAMEDCNT_GT_1(y))
                     R_variant_result = R_binding_cell == R_NilValue ? 2 : 1;
             }
        }
        UNPROTECT(1);
        return y;
    }
    else if( isVectorAtomic(x) ){
	errorcall(call, "$ operator is invalid for atomic vectors");
    }
    else /* e.g. a function */
	nonsubsettable_error(call,x);

    return R_NilValue;
}

/* FUNTAB entries defined in this source file. See names.c for documentation. */

attribute_hidden FUNTAB R_FunTab_subset[] =
{
/* printname	c-entry		offset	eval	arity	pp-kind	     precedence	rightassoc */

{"[",		do_subset,	1,	1000,	-1,	{PP_SUBSET,  PREC_SUBSET, 0}},
{"[[",		do_subset2,	2,	101000,	-1,	{PP_SUBSET,  PREC_SUBSET, 0}},
{".el.methods",	do_subset2,	0,	101000,	-1,	{PP_SUBSET,  PREC_SUBSET, 0}},
{"$",		do_subset3,	3,	101000,	2,	{PP_DOLLAR,  PREC_DOLLAR, 0}},

{".subset",	do_subset_dflt,	1,	1,	-1,	{PP_FUNCALL, PREC_FN,	  0}},
{".subset2",	do_subset2_dflt,2,	1,	-1,	{PP_FUNCALL, PREC_FN,	  0}},

{NULL,		NULL,		0,	0,	0,	{PP_INVALID, PREC_FN,	0}}
};
