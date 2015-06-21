/*
 *  pqR : A pretty quick version of R
 *  Copyright (C) 2013, 2014, 2015 by Radford M. Neal
 *
 *  Based on R : A Computer Language for Statistical Data Analysis
 *  Copyright (C) 1995, 1996  Robert Gentleman and Ross Ihaka
 *  Copyright (C) 1997--2008  The R Development Core Team
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
#include "Defn.h"

static SEXP cumsum(SEXP x, SEXP s)
{
    int i;
    long double sum = 0.;
    double *rx = REAL(x), *rs = REAL(s);
    for (i = 0 ; i < LENGTH(x) ; i++) {
	if (ISNAN(rx[i])) break;
	sum += rx[i];
	rs[i] = sum;
    }
    return s;
}

/* We need to ensure that overflow gives NA here */
static SEXP icumsum(SEXP x, SEXP s)
{
    int i, *ix = INTEGER(x), *is = INTEGER(s);
    double sum = 0.0;
    for (i = 0 ; i < LENGTH(x) ; i++) {
	if (ix[i] == NA_INTEGER) break;
	sum += ix[i];
	if(sum > INT_MAX || sum < 1 + INT_MIN) { /* INT_MIN is NA_INTEGER */
	    warning(_("Integer overflow in 'cumsum'; use 'cumsum(as.numeric(.))'"));
	    break;
	}
	is[i] = sum;
    }
    return s;
}

static SEXP ccumsum(SEXP x, SEXP s)
{
    int i;
    Rcomplex sum;
    sum.r = 0;
    sum.i = 0;
    for (i = 0 ; i < LENGTH(x) ; i++) {
	sum.r += COMPLEX(x)[i].r;
	sum.i += COMPLEX(x)[i].i;
	COMPLEX(s)[i].r = sum.r;
	COMPLEX(s)[i].i = sum.i;
    }
    return s;
}

static SEXP cumprod(SEXP x, SEXP s)
{
    int i;
    long double prod;
    double *rx = REAL(x), *rs = REAL(s);
    prod = 1.0;
    for (i = 0 ; i < LENGTH(x) ; i++) {
	prod *= rx[i];
	rs[i] = prod;
    }
    return s;
}

static SEXP ccumprod(SEXP x, SEXP s)
{
    Rcomplex prod, tmp;
    int i;
    prod.r = 1;
    prod.i = 0;
    for (i = 0 ; i < LENGTH(x) ; i++) {
	tmp.r = prod.r;
	tmp.i = prod.i;
	prod.r = COMPLEX(x)[i].r * tmp.r - COMPLEX(x)[i].i * tmp.i;
	prod.i = COMPLEX(x)[i].r * tmp.i + COMPLEX(x)[i].i * tmp.r;
	COMPLEX(s)[i].r = prod.r;
	COMPLEX(s)[i].i = prod.i;
    }
    return s;
}

static SEXP cummax(SEXP x, SEXP s)
{
    int i;
    double max, *rx = REAL(x), *rs = REAL(s);
    max = R_NegInf;
    for (i = 0 ; i < LENGTH(x) ; i++) {
	if(ISNAN(rx[i]) || ISNAN(max))
	    max = max + rx[i];  /* propagate NA and NaN */
	else
	    max = (max > rx[i]) ? max : rx[i];
	rs[i] = max;
    }
    return s;
}

static SEXP cummin(SEXP x, SEXP s)
{
    int i;
    double min, *rx = REAL(x), *rs = REAL(s);
    min = R_PosInf; /* always positive, not NA */
    for (i = 0 ; i < LENGTH(x) ; i++ ) {
	if (ISNAN(rx[i]) || ISNAN(min))
	    min = min + rx[i];  /* propagate NA and NaN */
	else
	    min = (min < rx[i]) ? min : rx[i];
	rs[i] = min;
    }
    return s;
}

static SEXP icummax(SEXP x, SEXP s)
{
    int i, *ix = INTEGER(x), *is = INTEGER(s);
    int max = ix[0];
    is[0] = max;
    for (i = 1 ; i < LENGTH(x) ; i++) {
	if(ix[i] == NA_INTEGER) break;
	is[i] = max = (max > ix[i]) ? max : ix[i];
    }
    return s;
}

static SEXP icummin(SEXP x, SEXP s)
{
    int i, *ix = INTEGER(x), *is = INTEGER(s);
    int min = ix[0];
    is[0] = min;
    for (i = 1 ; i < LENGTH(x) ; i++ ) {
	if(ix[i] == NA_INTEGER) break;
	is[i] = min = (min < ix[i]) ? min : ix[i];
    }
    return s;
}

static SEXP do_cum(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP s, t, ans;
    int i;
    checkArity(op, args);
    if (DispatchGroup("Math", call, op, args, env, &ans))
	return ans;
    if (isComplex(CAR(args))) {
	t = CAR(args);
	PROTECT(s = allocVector(CPLXSXP, LENGTH(t)));
	setAttrib(s, R_NamesSymbol, getAttrib(t, R_NamesSymbol));
	UNPROTECT(1);
	if(LENGTH(t) == 0) return s;
	for (i = 0 ; i < LENGTH(t) ; i++) {
	    COMPLEX(s)[i].r = NA_REAL;
	    COMPLEX(s)[i].i = NA_REAL;
	}
	switch (PRIMVAL(op) ) {
	case 1:	/* cumsum */
	    return ccumsum(t, s);
	    break;
	case 2: /* cumprod */
	    return ccumprod(t, s);
	    break;
	case 3: /* cummax */
	case 4: /* cummin */
	    errorcall(call, _("min/max not defined for complex numbers"));
	    break;
	default:
	    errorcall(call, _("unknown cumxxx function"));
	}
    } else if( ( isInteger(CAR(args)) || isLogical(CAR(args)) ) &&
	       PRIMVAL(op) != 2) {
	PROTECT(t = coerceVector(CAR(args), INTSXP));
        int n = LENGTH(t);
	PROTECT(s = allocVector(INTSXP, n));
	setAttrib(s, R_NamesSymbol, getAttrib(t, R_NamesSymbol));
	if (n == 0) {
            UNPROTECT(2); /* t, s */
            return s;
        }
	for (i = 0 ; i < n ; i++) INTEGER(s)[i] = NA_INTEGER;
        SEXP ans;
	switch (PRIMVAL(op) ) {
	case 1:	/* cumsum */
	    ans = icumsum(t,s);  /* may produce a warning, which allocates */
	    break;
	case 3: /* cummax */
	    ans = icummax(t,s);
	    break;
	case 4: /* cummin */
	    ans = icummin(t,s);
	    break;
	default:
	    errorcall(call, _("unknown cumxxx function"));
	}
        UNPROTECT(2); /* t, s */
        return ans;
    } else {
	PROTECT(t = coerceVector(CAR(args), REALSXP));
	PROTECT(s = allocVector(REALSXP, LENGTH(t)));
	setAttrib(s, R_NamesSymbol, getAttrib(t, R_NamesSymbol));
	UNPROTECT(2);
	if(LENGTH(t) == 0) return s;
	for(i = 0 ; i < LENGTH(t) ; i++) REAL(s)[i] = NA_REAL;
	switch (PRIMVAL(op) ) {
	case 1:	/* cumsum */
	    return cumsum(t,s);
	    break;
	case 2: /* cumprod */
	    return cumprod(t,s);
	    break;
	case 3: /* cummax */
	    return cummax(t,s);
	    break;
	case 4: /* cummin */
	    return cummin(t,s);
	    break;
	default:
	    errorcall(call, _("unknown cumxxx function"));
	}
    }
}

/* FUNTAB entries defined in this source file. See names.c for documentation. */

attribute_hidden FUNTAB R_FunTab_cum[] =
{
/* printname	c-entry		offset	eval	arity	pp-kind	     precedence	rightassoc */

{"cumsum",	do_cum,		1,	1,	1,	{PP_FUNCALL, PREC_FN,	0}},
{"cumprod",	do_cum,		2,	1,	1,	{PP_FUNCALL, PREC_FN,	0}},
{"cummax",	do_cum,		3,	1,	1,	{PP_FUNCALL, PREC_FN,	0}},
{"cummin",	do_cum,		4,	1,	1,	{PP_FUNCALL, PREC_FN,	0}},

{NULL,		NULL,		0,	0,	0,	{PP_INVALID, PREC_FN,	0}}
};
