/*
 *  pqR : A pretty quick version of R
 *  Copyright (C) 2013, 2014, 2015, 2016, 2017, 2018, 2019 by Radford M. Neal
 *
 *  Based on R : A Computer Language for Statistical Data Analysis
 *  Copyright (C) 1995, 1996  Robert Gentleman and Ross Ihaka
 *  Copyright (C) 1997--2010  The R Core Team
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
 */


#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define USE_FAST_PROTECT_MACROS
#include <Defn.h>
#include <Rmath.h>

#include <helpers/helpers-app.h>

#include "scalar-stack.h"


/***  NOTE:  do_relop itself is in eval.c, calling R_relop here.  ***/


/* MACROS FOR BUILDING PROCEDURES THAT DO THE RELATIONAL OPERATIONS.  
   Separate macros are defined for non-variant operations, and for
   the and, or, and sum variants. 

   Note that T and F are the values to return for "true" and "false" 
   comparisons, which may be swapped from TRUE and FALSE, as part
   of the procedure for reducing all relational ops to == and <. */

#define RAW_FETCH(s,i)  RAW(s)[i]
#define INT_FETCH(s,i)  INTEGER(s)[i]
#define REAL_FETCH(s,i) REAL(s)[i]
#define CPLX_FETCH(s,i) COMPLEX(s)[i]

#define RELOP_MACRO(FETCH,NANCHK1,NANCHK2,COMPARE) do { \
 \
    if (n2 == 1) { \
        x2 = FETCH(s2,0); \
        if (NANCHK2) \
            Rf_set_elements_to_NA (ans, 0, 1, n); \
        else \
            for (R_len_t i = 0; i<n; i++) { \
                x1 = FETCH(s1,i); \
                lp[i] = NANCHK1 ? NA_LOGICAL : COMPARE ? T : F; \
            } \
    } \
    else if (n1 == 1) { \
        x1 = FETCH(s1,0); \
        if (NANCHK1) \
            Rf_set_elements_to_NA (ans, 0, 1, n); \
        else \
            for (R_len_t i = 0; i<n; i++) { \
                x2 = FETCH(s2,i); \
                lp[i] = NANCHK2 ? NA_LOGICAL : COMPARE ? T : F; \
            } \
    } \
    else if (n1 == n2) { \
        for (R_len_t i = 0; i<n; i++) { \
            x1 = FETCH(s1,i); \
            x2 = FETCH(s2,i); \
            lp[i] = \
              NANCHK1 || NANCHK2 ? NA_LOGICAL : COMPARE ? T : F; \
        } \
    } \
    else if (n1 < n2) { \
        mod_iterate_1 (n1, n2, i1, i2) { \
            x1 = FETCH(s1,i1); \
            x2 = FETCH(s2,i2); \
            lp[i] = \
              NANCHK1 || NANCHK2 ? NA_LOGICAL : COMPARE ? T : F; \
        } \
    } \
    else { /* n2 < n1 */ \
        mod_iterate_2 (n1, n2, i1, i2) { \
            x1 = FETCH(s1,i1); \
            x2 = FETCH(s2,i2); \
            lp[i] = \
              NANCHK1 || NANCHK2 ? NA_LOGICAL : COMPARE ? T : F; \
        } \
    } \
} while (0)

#define RELOP_AND_MACRO(FETCH,NANCHK1,NANCHK2,COMPARE) do { \
 \
    res = TRUE; \
 \
    if (n2 == 1) { \
        x2 = FETCH(s2,0); \
        if (NANCHK2) \
            res = NA_LOGICAL; \
        else \
            for (R_len_t i = 0; i<n; i++) { \
                x1 = FETCH(s1,i); \
                if (NANCHK1) \
                    res = NA_LOGICAL; \
                else if (COMPARE ? F : T) { \
                    res = FALSE; \
                    break; \
                } \
            } \
    } \
    else if (n1 == 1) { \
        x1 = FETCH(s1,0); \
        if (NANCHK1) \
            res = NA_LOGICAL; \
        else \
            for (R_len_t i = 0; i<n; i++) { \
                x2 = FETCH(s2,i); \
                if (NANCHK2) \
                    res = NA_LOGICAL; \
                else if (COMPARE ? F : T) { \
                    res = FALSE; \
                    break; \
                } \
            } \
    } \
    else if (n1 == n2) { \
        for (R_len_t i = 0; i<n; i++) { \
            x1 = FETCH(s1,i); \
            x2 = FETCH(s2,i); \
            if (NANCHK1 || NANCHK2) \
                res = NA_LOGICAL; \
            else if (COMPARE ? F : T) { \
                res = FALSE; \
                break; \
            } \
        } \
    } \
    else if (n1 < n2) { \
        mod_iterate_1 (n1, n2, i1, i2) { \
            x1 = FETCH(s1,i1); \
            x2 = FETCH(s2,i2); \
            if (NANCHK1 || NANCHK2) \
                res = NA_LOGICAL; \
            else if (COMPARE ? F : T) { \
                res = FALSE; \
                break; \
            } \
        } \
    } \
    else { /* n2 < n1 */ \
        mod_iterate_2 (n1, n2, i1, i2) { \
            x1 = FETCH(s1,i1); \
            x2 = FETCH(s2,i2); \
            if (NANCHK1 || NANCHK2) \
                res = NA_LOGICAL; \
            else if (COMPARE ? F : T) { \
                res = FALSE; \
                break; \
            } \
        } \
    } \
 \
} while (0)

#define RELOP_OR_MACRO(FETCH,NANCHK1,NANCHK2,COMPARE) do { \
 \
    res = FALSE; \
 \
    if (n2 == 1) { \
        x2 = FETCH(s2,0); \
        if (NANCHK2) \
            res = NA_LOGICAL; \
        else \
            for (R_len_t i = 0; i<n; i++) { \
                x1 = FETCH(s1,i); \
                if (NANCHK1) \
                    res = NA_LOGICAL; \
                else if (COMPARE ? T : F) { \
                    res = TRUE; \
                    break; \
                } \
            } \
    } \
    else if (n1 == 1) { \
        x1 = FETCH(s1,0); \
        if (NANCHK1) \
            res = NA_LOGICAL; \
        else \
            for (R_len_t i = 0; i<n; i++) { \
                x2 = FETCH(s2,i); \
                if (NANCHK2) \
                    res = NA_LOGICAL; \
                else if (COMPARE ? T : F) { \
                    res = TRUE; \
                    break; \
                } \
            } \
    } \
    else if (n1 == n2) { \
        for (R_len_t i = 0; i<n; i++) { \
            x1 = FETCH(s1,i); \
            x2 = FETCH(s2,i); \
            if (NANCHK1 || NANCHK2) \
                res = NA_LOGICAL; \
            else if (COMPARE ? T : F) { \
                res = TRUE; \
                break; \
            } \
        } \
    } \
    else if (n1 < n2) { \
        mod_iterate_1 (n1, n2, i1, i2) { \
            x1 = FETCH(s1,i1); \
            x2 = FETCH(s2,i2); \
            if (NANCHK1 || NANCHK2) \
                res = NA_LOGICAL; \
            else if (COMPARE ? T : F) { \
                res = TRUE; \
                break; \
            } \
        } \
    } \
    else { /* n2 < n1 */ \
        mod_iterate_2 (n1, n2, i1, i2) { \
            x1 = FETCH(s1,i1); \
            x2 = FETCH(s2,i2); \
            if (NANCHK1 || NANCHK2) \
                res = NA_LOGICAL; \
            else if (COMPARE ? T : F) { \
                res = TRUE; \
                break; \
            } \
        } \
    } \
 \
} while (0)

#define RELOP_SUM_MACRO(FETCH,NANCHK1,NANCHK2,COMPARE) do { \
 \
    res = 0; \
 \
    if (n2 == 1) { \
        x2 = FETCH(s2,0); \
        if (NANCHK2) \
            res = NA_INTEGER; \
        else \
            for (R_len_t i = 0; i<n; i++) { \
                x1 = FETCH(s1,i); \
                if (NANCHK1) { \
                    res = NA_INTEGER; \
                    break; \
                } \
                else if (COMPARE ? T : F) \
                    res += 1; \
            } \
    } \
    else if (n1 == 1) { \
        x1 = FETCH(s1,0); \
        if (NANCHK1) \
            res = NA_INTEGER; \
        else \
            for (R_len_t i = 0; i<n; i++) { \
                x2 = FETCH(s2,i); \
                if (NANCHK2) { \
                    res = NA_INTEGER; \
                    break; \
                } \
                else if (COMPARE ? T : F) \
                    res += 1; \
            } \
    } \
    else if (n1 == n2) { \
        for (R_len_t i = 0; i<n; i++) { \
            x1 = FETCH(s1,i); \
            x2 = FETCH(s2,i); \
            if (NANCHK1 || NANCHK2) { \
                res = NA_INTEGER; \
                break; \
            } \
            else if (COMPARE ? T : F) \
                res += 1; \
        } \
    } \
    else if (n1 < n2) { \
        mod_iterate_1 (n1, n2, i1, i2) { \
            x1 = FETCH(s1,i1); \
            x2 = FETCH(s2,i2); \
            if (NANCHK1 || NANCHK2) { \
                res = NA_INTEGER; \
                break; \
            } \
            else if (COMPARE ? T : F) \
                res += 1; \
        } \
    } \
    else { /* n2 < n1 */ \
        mod_iterate_2 (n1, n2, i1, i2) { \
            x1 = FETCH(s1,i1); \
            x2 = FETCH(s2,i2); \
            if (NANCHK1 || NANCHK2) { \
                res = NA_INTEGER; \
                break; \
            } \
            else if (COMPARE ? T : F) \
                res += 1; \
        } \
    } \
 \
} while (0)


/* TASK PROCEDURES FOR RELATIONAL OPERATIONS NOT ON STRINGS.  Note
   that the string operations may require translation, which involves
   memory allocation, and hence cannot be done in a procedure executed
   in a helper thread.  There are task procedures for non-variant
   operations and for AND, OR, and SUM variants. */

void task_relop (helpers_op_t code, SEXP ans, SEXP s1, SEXP s2)
{
    int * restrict lp = LOGICAL(ans);
    int F = code & 1;
    int T = !F;

    code >>= 1;

    int n1 = LENGTH(s1);
    int n2 = LENGTH(s2);
    int n = n1==0 || n2==0 ? 0 : n1>n2 ? n1 : n2;

    if (n == 0) return;

    switch (TYPEOF(s1)) {
    case RAWSXP: {
        Rbyte x1, x2;
        switch (code) {
        case EQOP:
            RELOP_MACRO (RAW_FETCH, 0, 0, x1 == x2);
            return;
        case LTOP:
            RELOP_MACRO (RAW_FETCH, 0, 0, x1 < x2);
            return;
        }
    }
    case LGLSXP: case INTSXP: {
        int x1, x2;
        switch (code) {
        case EQOP:
            RELOP_MACRO (INT_FETCH, x1==NA_INTEGER, x2==NA_INTEGER, x1==x2);
            return;
        case LTOP:
            RELOP_MACRO (INT_FETCH, x1==NA_INTEGER, x2==NA_INTEGER, x1<x2);
            return;
        }
    }
    case REALSXP: {
        double x1, x2;
        switch (code) {
        case EQOP:
            RELOP_MACRO (REAL_FETCH, ISNAN(x1), ISNAN(x2), x1 == x2);
            return;
        case LTOP:
            RELOP_MACRO (REAL_FETCH, ISNAN(x1), ISNAN(x2), x1 < x2);
            return;
        }
    }
    case CPLXSXP: {
        Rcomplex x1, x2;
        switch (code) {
        case EQOP:
            RELOP_MACRO (CPLX_FETCH, (ISNAN(x1.r) || ISNAN(x1.i)), 
                                     (ISNAN(x2.r) || ISNAN(x2.i)), 
                                     (x1.r == x2.r && x1.i == x2.i));
            return;
        }
    }}
}

void task_relop_and (helpers_op_t code, SEXP ans, SEXP s1, SEXP s2)
{
    int F = code & 1;
    int T = !F;

    code >>= 1;

    int n1 = LENGTH(s1);
    int n2 = LENGTH(s2);
    int n = n1==0 || n2==0 ? 0 : n1>n2 ? n1 : n2;

    int res = TRUE;

    if (n == 0) goto done;

    switch (TYPEOF(s1)) {
    case RAWSXP: {
        Rbyte x1, x2;
        switch (code) {
        case EQOP:
            RELOP_AND_MACRO (RAW_FETCH, 0, 0, x1 == x2);
            goto done;
        case LTOP:
            RELOP_AND_MACRO (RAW_FETCH, 0, 0, x1 < x2);
            goto done;
        }
    }
    case LGLSXP: case INTSXP: {
        int x1, x2;
        switch (code) {
        case EQOP:
            RELOP_AND_MACRO (INT_FETCH, x1==NA_INTEGER, x2==NA_INTEGER, x1==x2);
            goto done;
        case LTOP:
            RELOP_AND_MACRO (INT_FETCH, x1==NA_INTEGER, x2==NA_INTEGER, x1<x2);
            goto done;
        }
    }
    case REALSXP: {
        double x1, x2;
        switch (code) {
        case EQOP:
            RELOP_AND_MACRO (REAL_FETCH, ISNAN(x1), ISNAN(x2), x1 == x2);
            goto done;
        case LTOP:
            RELOP_AND_MACRO (REAL_FETCH, ISNAN(x1), ISNAN(x2), x1 < x2);
            goto done;
        }
    }
    case CPLXSXP: {
        Rcomplex x1, x2;
        switch (code) {
        case EQOP:
            RELOP_AND_MACRO (CPLX_FETCH, (ISNAN(x1.r) || ISNAN(x1.i)), 
                                         (ISNAN(x2.r) || ISNAN(x2.i)), 
                                         (x1.r == x2.r && x1.i == x2.i));
            goto done;
        }
    }}

  done:
    LOGICAL(ans)[0] = res;
}

void task_relop_or (helpers_op_t code, SEXP ans, SEXP s1, SEXP s2)
{
    int F = code & 1;
    int T = !F;

    code >>= 1;

    int n1 = LENGTH(s1);
    int n2 = LENGTH(s2);
    int n = n1==0 || n2==0 ? 0 : n1>n2 ? n1 : n2;

    int res = FALSE;

    if (n == 0) goto done;

    switch (TYPEOF(s1)) {
    case RAWSXP: {
        Rbyte x1, x2;
        switch (code) {
        case EQOP:
            RELOP_OR_MACRO (RAW_FETCH, 0, 0, x1 == x2);
            goto done;
        case LTOP:
            RELOP_OR_MACRO (RAW_FETCH, 0, 0, x1 < x2);
            goto done;
        }
    }
    case LGLSXP: case INTSXP: {
        int x1, x2;
        switch (code) {
        case EQOP:
            RELOP_OR_MACRO (INT_FETCH, x1==NA_INTEGER, x2==NA_INTEGER, x1==x2);
            goto done;
        case LTOP:
            RELOP_OR_MACRO (INT_FETCH, x1==NA_INTEGER, x2==NA_INTEGER, x1<x2);
            goto done;
        }
    }
    case REALSXP: {
        double x1, x2;
        switch (code) {
        case EQOP:
            RELOP_OR_MACRO (REAL_FETCH, ISNAN(x1), ISNAN(x2), x1 == x2);
            goto done;
        case LTOP:
            RELOP_OR_MACRO (REAL_FETCH, ISNAN(x1), ISNAN(x2), x1 < x2);
            goto done;
        }
    }
    case CPLXSXP: {
        Rcomplex x1, x2;
        switch (code) {
        case EQOP:
            RELOP_OR_MACRO (CPLX_FETCH, (ISNAN(x1.r) || ISNAN(x1.i)), 
                                        (ISNAN(x2.r) || ISNAN(x2.i)), 
                                        (x1.r == x2.r && x1.i == x2.i));
            goto done;
        }
    }}

  done:
    LOGICAL(ans)[0] = res;
}

void task_relop_sum (helpers_op_t code, SEXP ans, SEXP s1, SEXP s2)
{
    int F = code & 1;
    int T = !F;

    code >>= 1;

    int n1 = LENGTH(s1);
    int n2 = LENGTH(s2);
    int n = n1==0 || n2==0 ? 0 : n1>n2 ? n1 : n2;

    int res = 0;

    if (n == 0) goto done;

    switch (TYPEOF(s1)) {
    case RAWSXP: {
        Rbyte x1, x2;
        switch (code) {
        case EQOP:
            RELOP_SUM_MACRO (RAW_FETCH, 0, 0, x1 == x2);
            goto done;
        case LTOP:
            RELOP_SUM_MACRO (RAW_FETCH, 0, 0, x1 < x2);
            goto done;
        }
    }
    case LGLSXP: case INTSXP: {
        int x1, x2;
        switch (code) {
        case EQOP:
            RELOP_SUM_MACRO (INT_FETCH, x1==NA_INTEGER, x2==NA_INTEGER, x1==x2);
            goto done;
        case LTOP:
            RELOP_SUM_MACRO (INT_FETCH, x1==NA_INTEGER, x2==NA_INTEGER, x1<x2);
            goto done;
        }
    }
    case REALSXP: {
        double x1, x2;
        switch (code) {
        case EQOP:
            RELOP_SUM_MACRO (REAL_FETCH, ISNAN(x1), ISNAN(x2), x1 == x2);
            goto done;
        case LTOP:
            RELOP_SUM_MACRO (REAL_FETCH, ISNAN(x1), ISNAN(x2), x1 < x2);
            goto done;
        }
    }
    case CPLXSXP: {
        Rcomplex x1, x2;
        switch (code) {
        case EQOP:
            RELOP_SUM_MACRO (CPLX_FETCH, (ISNAN(x1.r) || ISNAN(x1.i)), 
                                         (ISNAN(x2.r) || ISNAN(x2.i)), 
                                         (x1.r == x2.r && x1.i == x2.i));
            goto done;
        }
    }}

  done:
    INTEGER(ans)[0] = res;
}


/* PROCEDURES FOR RELATIONAL OPERATIONS ON STRINGS.  Separate versions
   for non-variant operations, and for AND, OR, and SUM variants. */

static SEXP string_relop(RELOP_TYPE code, int F, SEXP s1, SEXP s2)
{
    SEXP ans, x1, x2;
    const SEXP *e1 = STRING_PTR(s1);
    const SEXP *e2 = STRING_PTR(s2);
    int T = !F;

    int n1 = LENGTH(s1);
    int n2 = LENGTH(s2);
    int n = n1==0 || n2==0 ? 0 : n1>n2 ? n1 : n2;

    PROTECT(ans = allocVector(LGLSXP, n));
    int * restrict lp = LOGICAL(ans);

    if (n == 0) {
        /* nothing to do */
    }
    else if (code == EQOP) {
        if (n2 == 1) {
            x2 = e2[0];
            for (R_len_t i = 0; i<n; i++) {
                x1 = e1[i];
                lp[i] = x1==NA_STRING || x2==NA_STRING ? NA_LOGICAL
                      : SEQL(x1, x2) ? T : F;
            }
        }
        else if (n1 == 1) {
            x1 = e1[0];
            for (R_len_t i = 0; i<n; i++) {
                x2 = e2[i];
                lp[i] = x1==NA_STRING || x2==NA_STRING ? NA_LOGICAL
                      : SEQL(x1, x2) ? T : F;
            }
        }
        else if (n1 == n2) {
            for (R_len_t i = 0; i<n; i++) {
	        x1 = e1[i];
                x2 = e2[i];
                lp[i] = x1==NA_STRING || x2==NA_STRING ? NA_LOGICAL
                      : SEQL(x1, x2) ? T : F;
            }
        }
        else {
	    mod_iterate (n, n1, n2, i1, i2) {
	        x1 = e1[i1];
                x2 = e2[i2];
                lp[i] = x1==NA_STRING || x2==NA_STRING ? NA_LOGICAL
                      : SEQL(x1, x2) ? T : F;
            }
	}
    }
    else { /* LTOP */
        if (n2 == 1) {
            x2 = e2[0];
            for (R_len_t i = 0; i<n; i++) {
                x1 = e1[i];
                if (x1 == NA_STRING || x2 == NA_STRING)
                    lp[i] = NA_LOGICAL;
                else if (x1 == x2)
                    lp[i] = F;
                else
                    lp[i] = Scollate(x1, x2) < 0 ? T : F;
            }
        }
        else if (n1 == 1) {
            x1 = e1[0];
            for (R_len_t i = 0; i<n; i++) {
                x2 = e2[i];
                if (x1 == NA_STRING || x2 == NA_STRING)
                    lp[i] = NA_LOGICAL;
                else if (x1 == x2)
                    lp[i] = F;
                else
                    lp[i] = Scollate(x1, x2) < 0 ? T : F;
            }
        }
        else if (n1 == n2) {
            for (R_len_t i = 0; i<n; i++) {
	        x1 = e1[i];
                x2 = e2[i];
                if (x1 == NA_STRING || x2 == NA_STRING)
                    lp[i] = NA_LOGICAL;
                else if (x1 == x2)
                    lp[i] = F;
                else
                    lp[i] = Scollate(x1, x2) < 0 ? T : F;
            }
        }
        else {
            mod_iterate (n, n1, n2, i1, i2) {
                x1 = e1[i1];
                x2 = e2[i2];
                if (x1 == NA_STRING || x2 == NA_STRING)
                    lp[i] = NA_LOGICAL;
                else if (x1 == x2)
                    lp[i] = F;
                else
                    lp[i] = Scollate(x1, x2) < 0 ? T : F;
            }
        }
    }

    UNPROTECT(1);
    return ans;
}

static SEXP string_relop_and(RELOP_TYPE code, int F, SEXP s1, SEXP s2)
{
    SEXP x1, x2;
    const SEXP *e1 = STRING_PTR(s1);
    const SEXP *e2 = STRING_PTR(s2);
    int T = !F;
    int ans;

    int n1 = LENGTH(s1);
    int n2 = LENGTH(s2);
    int n = n1==0 || n2==0 ? 0 : n1>n2 ? n1 : n2;

    ans = TRUE;

    if (n == 0) {
        /* nothing to do */
    }
    else if (code == EQOP) {
        if (n2 == 1) {
            x2 = e2[0];
            if (x2 == NA_STRING)
                ans = NA_LOGICAL;
            else
                for (R_len_t i = 0; i<n; i++) {
                    x1 = e1[i];
                    if (x1==NA_STRING)
                        ans = NA_LOGICAL;
                    else if (SEQL(x1, x2) ? F : T)
                        goto false;
            }
        }
        else if (n1 == 1) {
            x1 = e1[0];
            if (x1 == NA_STRING)
                ans = NA_LOGICAL;
            else
                for (R_len_t i = 0; i<n; i++) {
                    x2 = e2[i];
                    if (x2==NA_STRING)
                        ans = NA_LOGICAL;
                    else if (SEQL(x1, x2) ? F : T)
                        goto false;
            }
        }
        else if (n1 == n2) {
            for (R_len_t i = 0; i<n; i++) {
	        x1 = e1[i];
                x2 = e2[i];
                if (x1==NA_STRING || x2==NA_STRING)
                    ans = NA_LOGICAL;
                else if (SEQL(x1, x2) ? F : T)
                    goto false;
            }
        }
        else {
	    mod_iterate (n, n1, n2, i1, i2) {
	        x1 = e1[i1];
                x2 = e2[i2];
                if (x1==NA_STRING || x2==NA_STRING)
                    ans = NA_LOGICAL;
                else if (SEQL(x1, x2) ? F : T)
                    goto false;
            }
	}
    }
    else { /* LTOP */
	for (R_len_t i = 0; i < n; i++) {
	    x1 = e1[i % n1];
	    x2 = e2[i % n2];
	    if (x1 == NA_STRING || x2 == NA_STRING)
		ans = NA_LOGICAL;
	    else if (x1 != x2 /* quick check */ && Scollate(x1,x2) < 0 ? F : T)
                goto false;
	}
    }

    return ScalarLogicalMaybeConst(ans);

false:
    return ScalarLogicalMaybeConst(FALSE);
}

static SEXP string_relop_or(RELOP_TYPE code, int F, SEXP s1, SEXP s2)
{
    SEXP x1, x2;
    const SEXP *e1 = STRING_PTR(s1);
    const SEXP *e2 = STRING_PTR(s2);
    int T = !F;
    int ans;

    int n1 = LENGTH(s1);
    int n2 = LENGTH(s2);
    int n = n1==0 || n2==0 ? 0 : n1>n2 ? n1 : n2;

    ans = FALSE;

    if (n == 0) {
        /* nothing to do */
    }
    else if (code == EQOP) {
        if (n2 == 1) {
            x2 = e2[0];
            if (x2 == NA_STRING)
                ans = NA_LOGICAL;
            else
                for (R_len_t i = 0; i<n; i++) {
                    x1 = e1[i];
                    if (x1==NA_STRING)
                        ans = NA_LOGICAL;
                    else if (SEQL(x1, x2) ? T : F)
                        goto true;
            }
        }
        else if (n1 == 1) {
            x1 = e1[0];
            if (x1 == NA_STRING)
                ans = NA_LOGICAL;
            else
                for (R_len_t i = 0; i<n; i++) {
                    x2 = e2[i];
                    if (x2==NA_STRING)
                        ans = NA_LOGICAL;
                    else if (SEQL(x1, x2) ? T : F)
                        goto true;
            }
        }
        else if (n1 == n2) {
            for (R_len_t i = 0; i<n; i++) {
	        x1 = e1[i];
                x2 = e2[i];
                if (x1==NA_STRING || x2==NA_STRING)
                    ans = NA_LOGICAL;
                else if (SEQL(x1, x2) ? T : F)
                    goto true;
            }
        }
        else {
	    mod_iterate (n, n1, n2, i1, i2) {
	        x1 = e1[i1];
                x2 = e2[i2];
                if (x1==NA_STRING || x2==NA_STRING)
                    ans = NA_LOGICAL;
                else if (SEQL(x1, x2) ? T : F)
                    goto true;
            }
	}
    }
    else { /* LTOP */
	for (R_len_t i = 0; i < n; i++) {
	    x1 = e1[i % n1];
	    x2 = e2[i % n2];
	    if (x1 == NA_STRING || x2 == NA_STRING)
		ans = NA_LOGICAL;
	    else if (x1 != x2 /* quick check */ && Scollate(x1,x2) < 0 ? T : F)
                goto true;
	}
    }

    return ScalarLogicalMaybeConst(ans);

true:
    return ScalarLogicalMaybeConst(TRUE);
}

static SEXP string_relop_sum(RELOP_TYPE code, int F, SEXP s1, SEXP s2)
{
    SEXP x1, x2;
    const SEXP *e1 = STRING_PTR(s1);
    const SEXP *e2 = STRING_PTR(s2);
    int T = !F;
    int ans;

    int n1 = LENGTH(s1);
    int n2 = LENGTH(s2);
    int n = n1==0 || n2==0 ? 0 : n1>n2 ? n1 : n2;

    ans = 0;

    if (n == 0) {
        /* nothing to do */
    }
    else if (code == EQOP) {
        if (n2 == 1) {
            x2 = e2[0];
            if (x2 == NA_STRING)
                ans = NA_INTEGER;
            else
                for (R_len_t i = 0; i<n; i++) {
                    x1 = e1[i];
                    if (x1==NA_STRING) {
                        ans = NA_INTEGER;
                        break;
                    }
                    else if (SEQL(x1, x2) ? T : F)
                        ans += 1;
            }
        }
        else if (n1 == 1) {
            x1 = e1[0];
            if (x1 == NA_STRING)
                ans = NA_INTEGER;
            else
                for (R_len_t i = 0; i<n; i++) {
                    x2 = e2[i];
                    if (x2==NA_STRING) {
                        ans = NA_INTEGER;
                        break;
                    }
                    else if (SEQL(x1, x2) ? T : F)
                        ans += 1;
            }
        }
        else if (n1 == n2) {
            for (R_len_t i = 0; i<n; i++) {
	        x1 = e1[i];
                x2 = e2[i];
                if (x1==NA_STRING || x2==NA_STRING) {
                    ans = NA_INTEGER;
                    break;
                }
                else if (SEQL(x1, x2) ? T : F)
                    ans += 1;
            }
        }
        else {
	    mod_iterate (n, n1, n2, i1, i2) {
	        x1 = e1[i1];
                x2 = e2[i2];
                if (x1==NA_STRING || x2==NA_STRING) {
                    ans = NA_INTEGER;
                    break;
                }
                else if (SEQL(x1, x2) ? T : F)
                    ans += 1;
            }
	}
    }
    else { /* LTOP */
	for (R_len_t i = 0; i < n; i++) {
	    x1 = e1[i % n1];
	    x2 = e2[i % n2];
	    if (x1 == NA_STRING || x2 == NA_STRING) {
		ans = NA_INTEGER;
                break;
            }
	    else if (x1 != x2 /* quick check */ && Scollate(x1,x2) < 0 ? T : F)
                ans += 1;
	}
    }

    return ScalarIntegerMaybeConst(ans);
}


/* MAIN PART OF IMPLEMENTATION OF RELATIONAL OPERATORS.  Called from
   do_relop in eval.c, and from elsewhere. */

#define T_relop THRESHOLD_ADJUST(60) 

SEXP attribute_hidden R_relop (SEXP call, int opcode, SEXP x, SEXP y, 
                               int objx, int objy, SEXP env, int variant)
{
    SEXP klass = R_NilValue;
    SEXP tsp = R_NilValue;
    SEXP xnames, ynames, tmp, ans, dims;
    int xarray, yarray, xts, yts, itmp;
    PROTECT_INDEX xpi, ypi;

    /* Reduce operation codes to EQOP and LTOP by swapping and negating. */

    int negate = 0;

    switch (opcode) {
    case NEOP: 
        opcode = EQOP; 
        negate = 1; 
        break; 
    case GTOP: 
        opcode = LTOP; 
        tmp = x; x = y; y = tmp; 
        itmp = objx; objx = objy; objy = itmp; 
        break;
    case LEOP:
        opcode = LTOP;
        negate = 1;
        tmp = x; x = y; y = tmp;
        itmp = objx; objx = objy; objy = itmp; 
        break;
    case GEOP:
        opcode = LTOP;
        negate = 1;
        break;
    default:
        break;
    }

    /* Get types and lengths of operands.  Pretend that logical is
       integer, as they have the same representation. */

    int nx = isVector(x) ? LENGTH(x) : length(x);
    int ny = isVector(y) ? LENGTH(y) : length(y);
    int n = nx==0 || ny==0 ? 0 : nx>ny ? nx : ny;

    SEXPTYPE typeof_x = TYPEOF(x);
    SEXPTYPE typeof_y = TYPEOF(y);

    if (typeof_x == LGLSXP) typeof_x = INTSXP;
    if (typeof_y == LGLSXP) typeof_y = INTSXP;

    /* Handle integer/real/string vectors that have no attributes (or whose
       attributes we will ignore) quickly. */ 

    if ((typeof_x == REALSXP || typeof_x == INTSXP || typeof_x == STRSXP)
          && (typeof_y == typeof_x || typeof_x != STRSXP 
                                 && (typeof_y == REALSXP || typeof_y == INTSXP))
          && n > 0 && ((variant & VARIANT_ANY_ATTR) != 0
                         || !HAS_ATTRIB(x) && !HAS_ATTRIB(y))) {

        /* Handle scalars even quicker using ScalarLogicalMaybeConst. */
    
        if (nx==1 && ny==1) {

            int result;

            if (typeof_x == STRSXP) {

                SEXP x1 = STRING_ELT(x,0), y1 = STRING_ELT(y,0);
                result = x1==NA_STRING || y1==NA_STRING ? NA_LOGICAL
                          : opcode == EQOP ? SEQL(x1,y1) 
                          : /* LTOP */ x1 == y1 ? FALSE : Scollate(x1,y1) < 0;
            }
            else {  /* INTSXP or REALSXP */

                WAIT_UNTIL_COMPUTED_2(x,y);

                /* Assumes ints can be represented to full precision as reals */

                double x1 = typeof_x == REALSXP ? REAL(x)[0]
                   : INTEGER(x)[0]!=NA_INTEGER ? INTEGER(x)[0] : NA_REAL;

                double y1 = typeof_y == REALSXP ? REAL(y)[0]
                   : INTEGER(y)[0]!=NA_INTEGER ? INTEGER(y)[0] : NA_REAL;

                result = ISNAN(x1) || ISNAN(y1) ? NA_LOGICAL
                           : opcode == EQOP ? x1 == y1 : /* LTOP */ x1 < y1;
            }

            return ScalarLogicalMaybeConst (negate && result != NA_LOGICAL 
                                             ? !result : result);
        } 
        else {
            PROTECT2(x,y);
            if (((nx > ny) ? nx % ny : ny % nx) != 0) {
 	            warningcall (call,
          _("longer object length is not a multiple of shorter object length"));
            }

            if (typeof_x != REALSXP && typeof_y == REALSXP)
                x = coerceVector(x,REALSXP);
            else if (typeof_y != REALSXP && typeof_x == REALSXP)
                y = coerceVector(y,REALSXP);
            UNPROTECT(2);
            PROTECT2(x,y);
            PROTECT3(dims=R_NilValue,xnames=R_NilValue,ynames=R_NilValue);
            xts = yts = 0;
        }
    }

    else { /* the general case */

        PROTECT_WITH_INDEX(x, &xpi);
        PROTECT_WITH_INDEX(y, &ypi);

        /* That symbols and calls were allowed was undocumented prior to
           R 2.5.0.  We deparse them as deparse() would, minus attributes */

        Rboolean iS;

        if ((iS = isSymbol(x)) || typeof_x == LANGSXP) {
            SEXP tmp = allocVector(STRSXP, 1);
            PROTECT(tmp);
            SET_STRING_ELT(tmp, 0, (iS) ? PRINTNAME(x) :
                           STRING_ELT(deparse1(x, 0, DEFAULTDEPARSE), 0));
            REPROTECT(x = tmp, xpi);
            typeof_x = STRSXP;
            UNPROTECT(1);
        }
        if ((iS = isSymbol(y)) || typeof_y == LANGSXP) {
            SEXP tmp = allocVector(STRSXP, 1);
            PROTECT(tmp);
            SET_STRING_ELT(tmp, 0, (iS) ? PRINTNAME(y) :
                           STRING_ELT(deparse1(y, 0, DEFAULTDEPARSE), 0));
            REPROTECT(y = tmp, ypi);
            typeof_y = STRSXP;
            UNPROTECT(1);
        }

        /* Comparisons to strange types (eg, external pointer) are allowed
           only against NULL, and return logical(0).  This also handles
           the case where both operands are NULL. */

        if (!isVector(x) && isNull(y) || !isVector(y) && isNull(x)) {
            UNPROTECT(2);  /* x and y */
            return allocVector(LGLSXP,0);
        }

        /* Treat NULL same as list() from here on. */

        if (isNull(x)) {
            REPROTECT(x = allocVector(VECSXP,0), xpi);
            typeof_x = VECSXP;
        }
        if (isNull(y)) {
            REPROTECT(y = allocVector(VECSXP,0), ypi);
            typeof_y = VECSXP;
        }

        if (!isVector(x) || !isVector(y))
            goto cmp_err;

        /* At this point, x and y are both atomic or vector list */

        xarray = isArray(x);
        yarray = isArray(y);
        xts = isTs(x);
        yts = isTs(y);

        /* If either x or y is a matrix with length 1 and the other is a
           vector, we want to coerce the matrix to be a vector. */

        if (xarray != yarray) {
            if (xarray && nx==1 && ny!=1) {
                REPROTECT(x = duplicate(x), xpi);
                setAttrib(x, R_DimSymbol, R_NilValue);
                xarray = FALSE;
            }
            if (yarray && ny==1 && nx!=1) {
                REPROTECT(y = duplicate(y), ypi);
                setAttrib(y, R_DimSymbol, R_NilValue);
                yarray = FALSE;
            }
        }

        if (xarray || yarray) {
            if (xarray && yarray) {
                if (!conformable(x, y))
                    errorcall(call, _("non-conformable arrays"));
                PROTECT(dims = getDimAttrib(x));
            }
            else if (xarray) {
                PROTECT(dims = getDimAttrib(x));
            }
            else /*(yarray)*/ {
                PROTECT(dims = getDimAttrib(y));
            }
            PROTECT(xnames = getAttrib(x, R_DimNamesSymbol));
            PROTECT(ynames = getAttrib(y, R_DimNamesSymbol));
        }
        else {
            PROTECT(dims = R_NilValue);
            PROTECT(xnames = getAttrib(x, R_NamesSymbol));
            PROTECT(ynames = getAttrib(y, R_NamesSymbol));
        }

        if (xts || yts) {
            if (xts && yts) {
                if (!tsConform(x, y))
                    errorcall(call, _("non-conformable time series"));
                PROTECT(tsp = getAttrib(x, R_TspSymbol));
                PROTECT(klass = !objx ? R_NilValue :getClassAttrib(x));
            }
            else if (xts) {
                if (length(x) < length(y))
                    ErrorMessage(call, ERROR_TSVEC_MISMATCH);
                PROTECT(tsp = getAttrib(x, R_TspSymbol));
                PROTECT(klass = !objx ? R_NilValue :getClassAttrib(x));
            }
            else /*(yts)*/ {
                if (length(y) < length(x))
            	ErrorMessage(call, ERROR_TSVEC_MISMATCH);
                PROTECT(tsp = getAttrib(y, R_TspSymbol));
                PROTECT(klass = !objy ? R_NilValue :getClassAttrib(y));
            }
        }

        if (n > 0 && ((nx > ny) ? nx % ny : ny % nx) != 0)
            warningcall (call, 
          _("longer object length is not a multiple of shorter object length"));

        if (typeof_x == STRSXP || typeof_y == STRSXP) {
            if (typeof_x != STRSXP) REPROTECT(x = coerceVector(x, STRSXP), xpi);
            if (typeof_y != STRSXP) REPROTECT(y = coerceVector(y, STRSXP), ypi);
        }
        else if (typeof_x == CPLXSXP || typeof_y == CPLXSXP) {
            if (typeof_x!=CPLXSXP) REPROTECT(x = coerceVector(x, CPLXSXP), xpi);
            if (typeof_y!=CPLXSXP) REPROTECT(y = coerceVector(y, CPLXSXP), ypi);
            if (opcode != EQOP)
                errorcall (call, _("invalid comparison with complex values"));
        }
        else if (typeof_x == REALSXP || typeof_y == REALSXP) {
            if (typeof_x!=REALSXP) REPROTECT(x = coerceVector(x, REALSXP), xpi);
            if (typeof_y!=REALSXP) REPROTECT(y = coerceVector(y, REALSXP), ypi);
        }
        else if (typeof_x == INTSXP || typeof_y == INTSXP) {
            /* NOT isInteger, since it needs to be true for factors with
               VARIANT_UNCLASS_FLAG.  Assumes LOGICAL same as INTEGER. */
            if (typeof_x != INTSXP) REPROTECT(x = coerceVector(x, INTSXP), xpi);
            if (typeof_y != INTSXP) REPROTECT(y = coerceVector(y, INTSXP), ypi);
        }
        else if (typeof_x == RAWSXP || typeof_y == RAWSXP) {
            if (typeof_x != RAWSXP) REPROTECT(x = coerceVector(x, RAWSXP), xpi);
            if (typeof_y != RAWSXP) REPROTECT(y = coerceVector(y, RAWSXP), ypi);
        }
        else if (typeof_x != VECSXP || typeof_y != VECSXP || n != 0) {
            goto cmp_err;
        }
    }

    if (TYPEOF(x) == STRSXP) {  /* y will be STRSXP too */ 
        WAIT_UNTIL_COMPUTED_2(x,y);
        switch (VARIANT_KIND(variant)) {
        case VARIANT_AND: 
            ans = string_relop_and (opcode, negate, x, y); 
            if (xts || yts) UNPROTECT(2);
            UNPROTECT(5);
            return ans;
        case VARIANT_OR:
            ans = string_relop_or (opcode, negate, x, y);
            if (xts || yts) UNPROTECT(2);
            UNPROTECT(5);
            return ans;
        case VARIANT_SUM:
            ans = string_relop_sum (opcode, negate, x, y);
            if (xts || yts) UNPROTECT(2);
            UNPROTECT(5);
            return ans;
        default:
            PROTECT(ans = string_relop (opcode, negate, x, y));
            break;
        }
    }
    else if (TYPEOF(x) == VECSXP) {  /* y will be VECSXP too */
        if (n != 0)  /* only zero-length comparisons done; also checked above */
            goto cmp_err;
        PROTECT(ans = allocVector(LGLSXP,0));
    }
    else /* not strings or lists */ {
        helpers_op_t codeop = (opcode<<1) | negate;
        switch (VARIANT_KIND(variant)) {
        case VARIANT_AND: 
            WAIT_UNTIL_COMPUTED_2(x,y);
            ans = allocVector1LGL();
            task_relop_and (codeop, ans, x, y); 
            if (xts || yts) UNPROTECT(2);
            UNPROTECT(5);
            return ans;
        case VARIANT_OR:
            WAIT_UNTIL_COMPUTED_2(x,y);
            ans = allocVector1LGL();
            task_relop_or (codeop, ans, x, y); 
            if (xts || yts) UNPROTECT(2);
            UNPROTECT(5);
            return ans;
        case VARIANT_SUM:
            PROTECT(ans = allocVector1INT());
            if (ON_SCALAR_STACK(x) && ON_SCALAR_STACK(y)) {
                PROTECT(x = DUP_STACK_VALUE(x));
                y = DUP_STACK_VALUE(y);
                UNPROTECT(1);
            }
            else if (ON_SCALAR_STACK(x)) x = DUP_STACK_VALUE(x);
            else if (ON_SCALAR_STACK(y)) y = DUP_STACK_VALUE(y);
            DO_NOW_OR_LATER2 (variant, n >= T_relop, 0, task_relop_sum, codeop, 
                              ans, x, y);
            if (xts || yts) UNPROTECT(2);
            UNPROTECT(6);
            return ans;
        default:
            PROTECT(ans = allocVector(LGLSXP,n));
            if (ON_SCALAR_STACK(x) && ON_SCALAR_STACK(y)) {
                PROTECT(x = DUP_STACK_VALUE(x));
                y = DUP_STACK_VALUE(y);
                UNPROTECT(1);
            }
            else if (ON_SCALAR_STACK(x)) x = DUP_STACK_VALUE(x);
            else if (ON_SCALAR_STACK(y)) y = DUP_STACK_VALUE(y);
            DO_NOW_OR_LATER2 (variant, n >= T_relop, 0, task_relop, codeop, 
                              ans, x, y);
            break;
        }
    }

    /* Tack on dims, names, ts stuff, if necessary. */

    if (! (variant & VARIANT_ANY_ATTR)) {
        if (dims != R_NilValue) {
            setAttrib(ans, R_DimSymbol, dims);
            if (xnames != R_NilValue)
                setAttrib(ans, R_DimNamesSymbol, xnames);
            else if (ynames != R_NilValue)
                setAttrib(ans, R_DimNamesSymbol, ynames);
        }
        else if (xnames != R_NilValue && LENGTH(ans) == LENGTH(xnames))
            setAttrib(ans, R_NamesSymbol, xnames);
        else if (ynames != R_NilValue && LENGTH(ans) == LENGTH(ynames))
            setAttrib(ans, R_NamesSymbol, ynames);
    
        if (xts || yts) {
            setAttrib(ans, R_TspSymbol, tsp);
            setAttrib(ans, R_ClassSymbol, klass);
            UNPROTECT(2);
        }
    }

    UNPROTECT(6);
    return ans;

  cmp_err:
    errorcall (call, _("comparison of these types is not implemented"));

}


/* BITWISE INTEGER OPERATORS.  Used for "octmode" versions of the !,
   &, |, and xor operator, defined in 'base' (not for the operations
   on raw bytes). */

SEXP bitwiseNot(SEXP a)
{
    R_len_t m = LENGTH(a);
    SEXP ans = allocVector(INTSXP, m);
    for (R_len_t i = 0; i < m; i++)
        INTEGER(ans)[i] =  ~INTEGER(a)[i];
    return ans;
}

SEXP bitwiseAnd(SEXP a, SEXP b)
{
    R_len_t m = LENGTH(a), n = LENGTH(b), mn = (m && n) ? fmax2(m, n) : 0;
    SEXP ans = allocVector(INTSXP, mn);
    for (R_len_t i = 0; i < mn; i++)
	INTEGER(ans)[i] = INTEGER(a)[i%m] & INTEGER(b)[i%n];
    return ans;
}

SEXP bitwiseOr(SEXP a, SEXP b)
{
    R_len_t m = LENGTH(a), n = LENGTH(b), mn = (m && n) ? fmax2(m, n) : 0;
    SEXP ans = allocVector(INTSXP, mn);
    for (R_len_t i = 0; i < mn; i++)
	INTEGER(ans)[i] = INTEGER(a)[i%m] | INTEGER(b)[i%n];
    return ans;
}

SEXP bitwiseXor(SEXP a, SEXP b)
{
    R_len_t m = LENGTH(a), n = LENGTH(b), mn = (m && n) ? fmax2(m, n) : 0;
    SEXP ans = allocVector(INTSXP, mn);
    for (R_len_t i = 0; i < mn; i++)
	INTEGER(ans)[i] = INTEGER(a)[i%m] ^ INTEGER(b)[i%n];
    return ans;
}
