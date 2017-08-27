/*
 *  pqR : A pretty quick version of R
 *  Copyright (C) 2013, 2014, 2015, 2016, 2017 by Radford M. Neal
 *
 *  Based on R : A Computer Language for Statistical Data Analysis
 *  Copyright (C) 1995, 1996  Robert Gentleman and Ross Ihaka
 *  Copyright (C) 1997-2011   The R Core Team
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
#define R_MSG_type	_("invalid 'type' (%s) of argument")
#define imax2(x, y) ((x < y) ? y : x)

#define R_INT_MIN	(1+INT_MIN)
	/* since INT_MIN is the NA_INTEGER value ! */
#define Int2Real(i)	(((i) == NA_INTEGER) ? NA_REAL : (double)(i))

#ifdef DEBUG_sum
#define DbgP1(s) REprintf(s)
#define DbgP2(s,a) REprintf(s,a)
#define DbgP3(s,a,b) REprintf(s,a,b)
#else
#define DbgP1(s)
#define DbgP2(s,a)
#define DbgP3(s,a,b)
#endif

#include <stdint.h>

static int isum(int *x, int n, Rboolean narm, SEXP call)
{
    int_fast64_t s = 0;
    int i;

    if (narm) {
        for (i = 0; i < n; i++) 
	    if (x[i] != NA_INTEGER) s += x[i];
    } else { 
        for (i = 0; i < n; i++) {
            if (x[i] == NA_INTEGER) 
                return NA_INTEGER;
            s += x[i];
        }
    }

    if (s > INT_MAX || s < R_INT_MIN) {
	warningcall(call, _("Integer overflow - use sum(as.numeric(.))"));
	return NA_INTEGER;
    }

    return (int) s;
}

static double rsum (double *x, int n, Rboolean narm)
{
    long double s = 0.0;
    int i;

    if (narm) {
        for (i = 0; i < n; i++) 
	    if (!ISNAN(x[i])) s += x[i];
    } else { 
        for (i = 0; i < n; i++)
            s += x[i];
    }

    return s;
}

static Rcomplex csum (Rcomplex *x, int n, Rboolean narm)
{
    long double sr, si;
    Rcomplex s;
    int i;

    sr = si = 0.0;

    if (narm) {
        for (i = 0; i < n; i++) {
	    if (!ISNAN(x[i].r) && !ISNAN(x[i].i)) {
                sr += x[i].r;
                si += x[i].i;
            }
        }
    } else { 
        for (i = 0; i < n; i++) {
            sr += x[i].r;
            si += x[i].i;
        }
    }

    s.r = sr; 
    s.i = si;

    return s;
}

static Rboolean imin(int *x, int n, int *value, Rboolean narm)
{
    int i, s = 0 /* -Wall */;
    Rboolean updated = FALSE;

    /* Used to set s = INT_MAX, but this ignored INT_MAX in the input */
    for (i = 0; i < n; i++) {
	if (x[i] != NA_INTEGER) {
	    if (!updated || s > x[i]) {
		s = x[i];
		if(!updated) updated = TRUE;
	    }
	}
	else if (!narm) {
	    *value = NA_INTEGER;
	    return(TRUE);
	}
    }
    *value = s;

    return(updated);
}

static Rboolean rmin(double *x, int n, double *value, Rboolean narm)
{
    double s = 0.0; /* -Wall */
    int i;
    Rboolean updated = FALSE;

    /* s = R_PosInf; */
    for (i = 0; i < n; i++) {
	if (ISNAN(x[i])) {/* Na(N) */
	    if (!narm) {
		if(!ISNA(s)) s = x[i]; /* so any NA trumps all NaNs */
		if(!updated) updated = TRUE;
	    }
	}
	else if (!updated || x[i] < s) {  /* Never true if s is NA/NaN */
	    s = x[i];
	    if(!updated) updated = TRUE;
	}
    }
    *value = s;

    return(updated);
}

static Rboolean smin(SEXP x, SEXP *value, Rboolean narm)
{
    int i;
    SEXP s = NA_STRING; /* -Wall */
    Rboolean updated = FALSE;
    int len = length(x);

    for (i = 0; i < len; i++) {
	if (STRING_ELT(x, i) != NA_STRING) {
	    if (!updated ||
		(s != STRING_ELT(x, i) && Scollate(s, STRING_ELT(x, i)) > 0)) {
		s = STRING_ELT(x, i);
		if(!updated) updated = TRUE;
	    }
	}
	else if (!narm) {
	    *value = NA_STRING;
	    return(TRUE);
	}
    }
    *value = s;

    return(updated);
}

static Rboolean imax(int *x, int n, int *value, Rboolean narm)
{
    int i, s = 0 /* -Wall */;
    Rboolean updated = FALSE;

    for (i = 0; i < n; i++) {
	if (x[i] != NA_INTEGER) {
	    if (!updated || s < x[i]) {
		s = x[i];
		if(!updated) updated = TRUE;
	    }
	} else if (!narm) {
	    *value = NA_INTEGER;
	    return(TRUE);
	}
    }
    *value = s;

    return(updated);
}

static Rboolean rmax(double *x, int n, double *value, Rboolean narm)
{
    double s = 0.0 /* -Wall */;
    int i;
    Rboolean updated = FALSE;

    for (i = 0; i < n; i++) {
	if (ISNAN(x[i])) {/* Na(N) */
	    if (!narm) {
		if(!ISNA(s)) s = x[i]; /* so any NA trumps all NaNs */
		if(!updated) updated = TRUE;
	    }
	}
	else if (!updated || x[i] > s) {  /* Never true if s is NA/NaN */
	    s = x[i];
	    if(!updated) updated = TRUE;
	}
    }
    *value = s;

    return(updated);
}

static Rboolean smax(SEXP x, SEXP *value, Rboolean narm)
{
    int i;
    SEXP s = NA_STRING; /* -Wall */
    Rboolean updated = FALSE;
    int len = length(x);

    for (i = 0; i < len; i++) {
	if (STRING_ELT(x, i) != NA_STRING) {
	    if (!updated ||
		(s != STRING_ELT(x, i) && Scollate(s, STRING_ELT(x, i)) < 0)) {
		s = STRING_ELT(x, i);
		if(!updated) updated = TRUE;
	    }
	}
	else if (!narm) {
	    *value = NA_STRING;
	    return(TRUE);
	}
    }
    *value = s;

    return(updated);
}

static double iprod(int *x, int n, Rboolean narm)
{
    double s = 1.0;
    int i;

    if (narm) {
        for (i = 0; i < n; i++) 
	    if (x[i] != NA_INTEGER) s *= x[i];
    } else { 
        for (i = 0; i < n; i++) {
            if (x[i] == NA_INTEGER) 
                return NA_REAL;
            s *= x[i];
        }
    }

    return s;
}

static double rprod(double *x, int n, Rboolean narm)
{
    long double s = 1.0;
    int i;

    if (narm) {
        for (i = 0; i < n; i++) 
	    if (!ISNAN(x[i])) s *= x[i];
    } else { 
        for (i = 0; i < n; i++)
            s *= x[i];
    }

    return s;
}

static Rcomplex cprod(Rcomplex *x, int n, Rboolean narm)
{
    long double sr, si, tr, ti;
    Rcomplex s;
    int i;

    sr = 1.0;
    si = 0.0;

    for (i = 0; i < n; i++) {
	if (!narm || (!ISNAN(x[i].r) && !ISNAN(x[i].i))) {
	    tr = sr;
	    ti = si;
	    sr = tr * x[i].r - ti * x[i].i;
	    si = tr * x[i].i + ti * x[i].r;
	}
    }

    s.r = sr;
    s.i = si;

    return s;
}

static SEXP do_mean (SEXP call, SEXP op, SEXP args, SEXP env)
{
    long double s, si, t, ti;
    int_fast64_t smi;
    SEXP x, ans;
    int n, i;

    x = CAR(args);

    switch(TYPEOF(x)) {
    case LGLSXP:
    case INTSXP:
        n = LENGTH(x);
        PROTECT(ans = allocVector1REAL());
        smi = 0;
        for (i = 0; i < n; i++) {
            if(INTEGER(x)[i] == NA_INTEGER) {
                REAL(ans)[0] = R_NaReal;
                UNPROTECT(1);
                return ans;
            }
            smi += INTEGER(x)[i];
        }
        REAL(ans)[0] = (double)smi / n;
        break;
    case REALSXP:
        n = LENGTH(x);
        PROTECT(ans = allocVector1REAL());
        s = 0;
        for (i = 0; i < n; i++) 
            s += REAL(x)[i];
        s /= n;
        if(R_FINITE((double)s)) {
            t = 0;
            for (i = 0; i < n; i++) 
                t += REAL(x)[i]-s;
            s += t/n;
        }
        REAL(ans)[0] = s;
        break;
    case CPLXSXP:
        n = LENGTH(x);
        PROTECT(ans = allocVector(CPLXSXP, 1));
        s = si = 0;
        for (i = 0; i < n; i++) {
            s += COMPLEX(x)[i].r;
            si += COMPLEX(x)[i].i;
        }
        s /= n; si /= n;
        if( R_FINITE((double)s) && R_FINITE((double)si) ) {
            t = ti = 0;
            for (i = 0; i < n; i++) {
                t += COMPLEX(x)[i].r-s;
                ti += COMPLEX(x)[i].i-si;
            }
            s += t/n; si += ti/n;
        }
        COMPLEX(ans)[0].r = s;
        COMPLEX(ans)[0].i = si;
        break;
    default:
        error(R_MSG_type, type2char(TYPEOF(x)));
    }
    UNPROTECT(1);
    return ans;
}

/* do_summary provides a variety of data summaries
	op :  0 = sum,  2 = min,  3 = max,  4 = prod

   NOTE: mean used to be done here, but is now separate, since it has
   nothing in common with the others (has only one arg and no na.rm, and
   dispatch is from an R-level generic). 

   Note that for the fast forms, na.rm must be FALSE. */

static SEXP do_fast_sum (SEXP call, SEXP op, SEXP arg, SEXP env, int variant)
{
    /* A variant return value for arg looks just like an arg of length one, 
       and can be treated the same.  An arg of length one can be returned
       as the result if it has the right type and no attributes. */

    switch (TYPEOF(arg)) {

    case NILSXP:  
        return ScalarIntegerMaybeConst (0);

    case LGLSXP:  /* assumes LOGICAL and INTEGER really the same */
        WAIT_UNTIL_COMPUTED(arg);
        return ScalarInteger (isum (INTEGER(arg), LENGTH(arg), 0, call));

    case INTSXP:  
        if (LENGTH(arg) == 1 && !HAS_ATTRIB(arg)) break;
        WAIT_UNTIL_COMPUTED(arg);
        return ScalarInteger (isum (INTEGER(arg), LENGTH(arg), 0, call));

    case REALSXP:
        if (LENGTH(arg) == 1 && !HAS_ATTRIB(arg)) break;
        WAIT_UNTIL_COMPUTED(arg);
        return ScalarReal (rsum (REAL(arg), LENGTH(arg), 0));

    case CPLXSXP:
        if (LENGTH(arg) == 1 && !HAS_ATTRIB(arg)) break;
        WAIT_UNTIL_COMPUTED(arg);
        return ScalarComplex (csum (COMPLEX(arg), LENGTH(arg), 0));

    default:
        errorcall(call, R_MSG_type, type2char(TYPEOF(arg)));
    }

    if (! (variant & VARIANT_PENDING_OK) )
        WAIT_UNTIL_COMPUTED(arg);
    return arg;
}

static SEXP do_fast_prod (SEXP call, SEXP op, SEXP arg, SEXP env, int variant)
{
    switch (TYPEOF(arg)) {

    case NILSXP:  
        return ScalarRealMaybeConst (1.0);

    case LGLSXP:  /* assumes LOGICAL and INTEGER really the same */
    case INTSXP:  
        return ScalarReal (iprod (INTEGER(arg), LENGTH(arg), 0));

    case REALSXP:
        return ScalarReal (rprod (REAL(arg), LENGTH(arg), 0));

    case CPLXSXP:
        return ScalarComplex (cprod (COMPLEX(arg), LENGTH(arg), 0));

    default:
        errorcall(call, R_MSG_type, type2char(TYPEOF(arg)));
    }
}

static SEXP do_summary(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP ans, a, stmp = NA_STRING /* -Wall */, scum = NA_STRING, call2;
    double tmp = 0.0, s;
    Rcomplex z, ztmp, zcum={0.0, 0.0} /* -Wall */;
    int itmp = 0, icum=0, int_a, real_a, empty, warn = 0 /* dummy */;
    int iop;
    SEXPTYPE ans_type;/* only INTEGER, REAL, COMPLEX or STRSXP here */

    Rboolean narm;
    int updated;
	/* updated := 1 , as soon as (i)tmp (do_summary),
	   or *value ([ir]min / max) is assigned */

    /* match to foo(..., na.rm=FALSE) */
    PROTECT(args = fixup_NaRm(args));
    PROTECT(call2 = LCONS(CAR(call),args));

    if (DispatchGroup("Summary", call2, op, args, env, &ans)) {
	UNPROTECT(2);
	return(ans);
    }
    UNPROTECT(1);

#ifdef DEBUG_Summary
    REprintf("C do_summary(op%s, *): did NOT dispatch\n", PRIMNAME(op));
#endif

    ans = matchArgExact(R_NaRmSymbol, &args);
    narm = asLogical(ans);

    iop = PRIMVAL(op);
    switch(iop) {
    case 0:/* sum */
        /* we need to find out if _all_ the arguments are integer or logical
           in advance, as we might overflow before we find out.  NULL is
           documented to be the same as integer(0).
        */
	ans_type = INTSXP;
	for (SEXP a = args; !isNull(a); a = CDR(a)) {
	    if(!isInteger(CAR(a)) && !isLogical(CAR(a)) && !isNull(CAR(a))) {
		ans_type = REALSXP;  /* may change to CPLXSXP later */
		break;
	    }
	}
        icum = 0;
	zcum.r = zcum.i = 0.;
	break;

    case 2:/* min */
	DbgP2("do_summary: min(.. na.rm=%d) ", narm);
	ans_type = INTSXP;
	zcum.r = R_PosInf;
	icum = INT_MAX;
	break;

    case 3:/* max */
	DbgP2("do_summary: max(.. na.rm=%d) ", narm);
	ans_type = INTSXP;
	zcum.r = R_NegInf;;
	icum = R_INT_MIN;
	break;

    case 4:/* prod */
	ans_type = REALSXP;
	zcum.r = 1.;
	zcum.i = 0.;
	break;

    default:
	errorcall(call,
		  _("internal error ('op = %d' in do_summary).\t Call a Guru"),
		  iop);
    }

    /*-- now loop over all arguments.  Do the 'op' switch INSIDE : */

    updated = 0;
    empty = 1; /* for min/max, 1 if only 0-length arguments, or NA with na.rm=T */
    while (args != R_NilValue) {
	a = CAR(args);

	if(length(a) > 0) {

	    switch(iop) {
	    case 2:/* min */
	    case 3:/* max */

	        updated = 0;
	        int_a = 0;/* int_a = 1	<-->	a is INTEGER */
	        real_a = 0;

		switch(TYPEOF(a)) {
		case LGLSXP:
		case INTSXP:
		    int_a = 1;
                    updated = 
                      iop==2 ? imin(INTEGER(a), LENGTH(a), &itmp, narm)
		             : imax(INTEGER(a), LENGTH(a), &itmp, narm);
		    break;
		case REALSXP:
		    real_a = 1;
		    if(ans_type == INTSXP) {/* change to REAL */
			ans_type = REALSXP;
			if(!empty) zcum.r = Int2Real(icum);
		    }
                    updated = 
                      iop==2 ? rmin(REAL(a), LENGTH(a), &tmp, narm)
		             : rmax(REAL(a), LENGTH(a), &tmp, narm);
		    break;
		case STRSXP:
		    if(!empty && ans_type == INTSXP)
			scum = StringFromInteger(icum, &warn);
		    else if(!empty && ans_type == REALSXP)
			scum = StringFromReal(zcum.r, &warn);
		    ans_type = STRSXP;
		    if (iop == 2) updated = smin(a, &stmp, narm);
		    else updated = smax(a, &stmp, narm);
		    break;
		default:
		    goto invalid_type;
		}

		if(updated) {/* 'a' had non-NA elements; --> "add" tmp or itmp*/
		    DbgP1(" updated:");
		    if(ans_type == INTSXP) {
			DbgP3(" INT: (old)icum= %ld, itmp=%ld\n", icum,itmp);
			if (itmp == NA_INTEGER) goto na_answer;
			if ((iop == 2 && itmp < icum) || /* min */
			    (iop == 3 && itmp > icum))   /* max */
			    icum = itmp;
		    } else if(ans_type == REALSXP) {
			if (int_a) tmp = Int2Real(itmp);
			DbgP3(" REAL: (old)cum= %g, tmp=%g\n", zcum.r,tmp);
			if (ISNA(zcum.r)); /* NA trumps anything */
			else if (ISNAN(tmp)) {
			    if (ISNA(tmp)) zcum.r = tmp;
			    else zcum.r += tmp;/* NA or NaN */
			} else if(
			    (iop == 2 && tmp < zcum.r) ||
			    (iop == 3 && tmp > zcum.r))	zcum.r = tmp;
		    } else if(ans_type == STRSXP) {
			if(empty) scum = stmp;
			else {
			    if(int_a)
				stmp = StringFromInteger(itmp, &warn);
			    if(real_a)
				stmp = StringFromReal(tmp, &warn);
			    if(((iop == 2 && stmp != scum && Scollate(stmp, scum) < 0)) ||
			       (iop == 3 && stmp != scum && Scollate(stmp, scum) > 0) )
				scum = stmp;
			}
		    }
		}/*updated*/ else {
		    /*-- in what cases does this happen here at all?
		      -- if there are no non-missing elements.
		     */
		    DbgP2(" NOT updated [!! RARE !!]: int_a=%d\n", int_a);
		}

		break;/*--- end of  min() / max() ---*/

	    case 0:/* sum */

                WAIT_UNTIL_COMPUTED(a);

		switch(TYPEOF(a)) {
		case LGLSXP:
		case INTSXP:
		    itmp = isum (TYPEOF(a)==LGLSXP ? LOGICAL(a) : INTEGER(a),
                                 LENGTH(a), narm, call);
		    if (itmp == NA_INTEGER) goto na_answer;
		    if (ans_type == INTSXP) {
		        s = (double) icum + (double) itmp;
		        if (s > INT_MAX || s < R_INT_MIN) {
		            warningcall (call,
                              _("Integer overflow - use sum(as.numeric(.))"));
			    goto na_answer;
			}
                        icum += itmp;
		    } 
                    else
		        zcum.r += Int2Real(itmp);
		    break;
		case REALSXP:
		    if(ans_type == INTSXP) { /* shouldn't happen */
			ans_type = REALSXP;
			if(!empty) zcum.r = Int2Real(icum);
		    }
		    zcum.r += rsum(REAL(a), LENGTH(a), narm);
		    break;
		case CPLXSXP:
		    if(ans_type == INTSXP) { /* shouldn't happen */
			ans_type = CPLXSXP;
			if(!empty) zcum.r = Int2Real(icum);
		    } else if (ans_type == REALSXP)
			ans_type = CPLXSXP;
		    ztmp = csum(COMPLEX(a), LENGTH(a), narm);
		    zcum.r += ztmp.r;
		    zcum.i += ztmp.i;
		    break;
		default:
		    goto invalid_type;
		}

		break;/* sum() part */

	    case 4:/* prod */

		switch(TYPEOF(a)) {
		case LGLSXP:
		case INTSXP:
		case REALSXP:
		    if(TYPEOF(a) == REALSXP)
			tmp = rprod(REAL(a), LENGTH(a), narm);
		    else
			tmp = iprod(INTEGER(a), LENGTH(a), narm);
		    zcum.r *= tmp;
		    zcum.i *= tmp;
		    break;
		case CPLXSXP:
		    ans_type = CPLXSXP;
		    ztmp = cprod(COMPLEX(a), LENGTH(a), narm);
		    z.r = zcum.r;
		    z.i = zcum.i;
		    zcum.r = z.r * ztmp.r - z.i * ztmp.i;
		    zcum.i = z.r * ztmp.i + z.i * ztmp.r;
		    break;
		default:
		    goto invalid_type;
		}

		break;/* prod() part */

	    }/* switch(iop) */

	} else { /* len(a)=0 */
	    /* Even though this has length zero it can still be invalid,
	       e.g. list() or raw() */
	    switch(TYPEOF(a)) {
	    case LGLSXP:
	    case INTSXP:
	    case REALSXP:
	    case NILSXP:  /* OK historically, e.g. PR#1283 */
		break;
	    case CPLXSXP:
		if (iop == 2 || iop == 3) goto invalid_type;
		break;
	    case STRSXP:
		if (iop == 2 || iop == 3) {
		    if(!empty && ans_type == INTSXP) {
			scum = StringFromInteger(icum, &warn);
			UNPROTECT(1); /* scum */
			PROTECT(scum);
		    } else if(!empty && ans_type == REALSXP) {
			scum = StringFromReal(zcum.r, &warn);
			UNPROTECT(1); /* scum */
			PROTECT(scum);
		    }
		    ans_type = STRSXP;
		    break;
		}
	    default:
		goto invalid_type;
	    }
	    if(ans_type < TYPEOF(a) && ans_type != CPLXSXP) {
		if(!empty && ans_type == INTSXP)
		    zcum.r = Int2Real(icum);
		ans_type = TYPEOF(a);
	    }
	}
	DbgP3(" .. upd.=%d, empty: old=%d", updated, empty);
	if(empty && updated) empty=0;
	DbgP2(", new=%d\n", empty);
	args = CDR(args);
    } /*-- while(..) loop over args */

    /*-------------------------------------------------------*/
    if(empty && (iop == 2 || iop == 3)) {
	if(ans_type == STRSXP) {
	    warningcall(call, _("no non-missing arguments, returning NA"));
	} else {
	    if(iop == 2)
		warningcall(call, _("no non-missing arguments to min; returning Inf"));
	    else
		warningcall(call, _("no non-missing arguments to max; returning -Inf"));
	    ans_type = REALSXP;
	}
    }

    ans = allocVector(ans_type, 1);
    switch(ans_type) {
    case INTSXP:   INTEGER(ans)[0] = icum; break;
    case REALSXP:  REAL(ans)[0] = zcum.r; break;
    case CPLXSXP:  COMPLEX(ans)[0].r = zcum.r; 
                   COMPLEX(ans)[0].i = zcum.i; 
                   break;
    case STRSXP:   SET_STRING_ELT(ans, 0, scum); break;
    }
    UNPROTECT(1);  /* args */
    return ans;

na_answer: /* only INTSXP case currently used */
    ans = allocVector(ans_type, 1);
    switch(ans_type) {
    case INTSXP:   INTEGER(ans)[0] = NA_INTEGER; break;
    case REALSXP:  REAL(ans)[0] = NA_REAL; break;
    case CPLXSXP:  COMPLEX(ans)[0].r = COMPLEX(ans)[0].i = NA_REAL; break;
    case STRSXP:   SET_STRING_ELT(ans, 0, NA_STRING); break;
    }
    UNPROTECT(1);  /* args */
    return ans;

invalid_type:
    errorcall(call, R_MSG_type, type2char(TYPEOF(a)));
    return R_NilValue;
}/* do_summary */


static SEXP do_range(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP ans, prargs, call2;

    PROTECT(args = fixup_NaRm(args));
    PROTECT(call2 = LCONS(CAR(call),args));

    if (DispatchGroup("Summary", call2, op, args, env, &ans)) {
	UNPROTECT(2);
	return(ans);
    }
    UNPROTECT(1);

    PROTECT(op = findFun(install("range.default"), env));
    /* Below should really use CDR(call) for the unevaluated expressions, 
       but it can't because args has been fiddled with by fixup_NaRm. */
    PROTECT(prargs = promiseArgsWithValues(args, R_EmptyEnv, args));
    ans = applyClosure(call, op, prargs, env, NULL);
    UNPROTECT(3);
    return(ans);
}

/* which.min(x) : The index (starting at 1), of the first min(x) in x */
static SEXP do_first_min(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP sx, ans;
    double s, *r;
    int i, n, indx;

    checkArity(op, args);
    PROTECT(sx = coerceVector(CAR(args), REALSXP));
    if (!isNumeric(sx))
	error(_("non-numeric argument"));
    r = REAL(sx);
    n = LENGTH(sx);
    indx = NA_INTEGER;

    if(PRIMVAL(op) == 0) { /* which.min */
	s = R_PosInf;
	for (i = 0; i < n; i++)
	    if ( !ISNAN(r[i]) && (r[i] < s || indx == NA_INTEGER) ) {
		s = r[i]; indx = i;
	    }
    } else { /* which.max */
	s = R_NegInf;
	for (i = 0; i < n; i++)
	    if ( !ISNAN(r[i]) && (r[i] > s || indx == NA_INTEGER) ) {
		s = r[i]; indx = i;
	    }
    }

    i = (indx != NA_INTEGER);
    PROTECT(ans = allocVector(INTSXP, i ? 1 : 0));
    if (i) {
	INTEGER(ans)[0] = indx + 1;
	if (getAttrib(sx, R_NamesSymbol) != R_NilValue) { /* preserve names */
	    SEXP ansnam;
	    PROTECT(ansnam =
		    ScalarString(STRING_ELT(getAttrib(sx, R_NamesSymbol), indx)));
	    setAttrib(ans, R_NamesSymbol, ansnam);
	    UNPROTECT(1);
	}
    }
    UNPROTECT(2);
    return ans;
}

/* which(x) : indices of TRUE values in x (ignores NA) */
static SEXP do_which(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP v, v_nms, ans, ans_nms = R_NilValue;
    int i, j, len;
    int *vi, *ai;

    checkArity(op, args);
    v = CAR(args);
    if (!isLogical(v))
        error(_("argument to 'which' is not logical"));

    len = LENGTH(v);
    ans = allocVector (INTSXP, len);
    vi = LOGICAL(v);
    ai = INTEGER(ans);
    j = 0;

    if (len > 0) {
        i = 0;
        if (len & 1) {
            if (vi[i++] > 0) ai[j++] = i;
        }
        if (len & 2) {
            if (vi[i++] > 0) ai[j++] = i;
            if (vi[i++] > 0) ai[j++] = i;
        }
        while (i < len) {
            if (vi[i++] > 0) ai[j++] = i;
            if (vi[i++] > 0) ai[j++] = i;
            if (vi[i++] > 0) ai[j++] = i;
            if (vi[i++] > 0) ai[j++] = i;
        }
    }

    len = j;
    PROTECT (ans = reallocVector(ans,len));

    if ((v_nms = getNamesAttrib(v)) != R_NilValue) {
        PROTECT(ans_nms = allocVector(STRSXP, len));
        for (i = 0; i < len; i++) {
            SET_STRING_ELT (ans_nms, i, STRING_ELT(v_nms,ai[i]-1));
        }
        setAttrib(ans, R_NamesSymbol, ans_nms);
        UNPROTECT(1);
    }

    UNPROTECT(1);
    return ans;
}

/* complete.cases(.) */
static SEXP do_compcases(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP s, t, u, rval;
    int i, len;

    /* checkArity(op, args); */
    len = -1;

    for (s = args; s != R_NilValue; s = CDR(s)) {
	if (isList(CAR(s))) {
	    for (t = CAR(s); t != R_NilValue; t = CDR(t))
		if (isMatrix(CAR(t))) {
		    u = getDimAttrib(CAR(t));
		    if (len < 0)
			len = INTEGER(u)[0];
		    else if (len != INTEGER(u)[0])
			goto bad;
		}
		else if (isVector(CAR(t))) {
		    if (len < 0)
			len = LENGTH(CAR(t));
		    else if (len != LENGTH(CAR(t)))
			goto bad;
		}
		else
		    error(R_MSG_type, type2char(TYPEOF(CAR(t))));
	}
	/* FIXME : Need to be careful with the use of isVector() */
	/* since this includes lists and expressions. */
	else if (isNewList(CAR(s))) {
	    int it, nt;
	    t = CAR(s);
	    nt = length(t);
	    /* 0-column data frames are a special case */
	    if(nt) {
		for (it = 0 ; it < nt ; it++) {
		    if (isMatrix(VECTOR_ELT(t, it))) {
			u = getDimAttrib(VECTOR_ELT(t, it));
			if (len < 0)
			    len = INTEGER(u)[0];
			else if (len != INTEGER(u)[0])
			    goto bad;
		    }
		    else if (isVector(VECTOR_ELT(t, it))) {
			if (len < 0)
			    len = LENGTH(VECTOR_ELT(t, it));
			else if (len != LENGTH(VECTOR_ELT(t, it)))
			    goto bad;
		    }
		    else
			error(R_MSG_type, "unknown");
		}
	    } else {
		u = getAttrib(t, R_RowNamesSymbol);
		if (!isNull(u)) {
		    if (len < 0)
			len = LENGTH(u);
		    else if (len != INTEGER(u)[0])
			goto bad;
		}
	    }
	}
	else if (isMatrix(CAR(s))) {
	    u = getDimAttrib(CAR(s));
	    if (len < 0)
		len = INTEGER(u)[0];
	    else if (len != INTEGER(u)[0])
		goto bad;
	}
	else if (isVector(CAR(s))) {
	    if (len < 0)
		len = LENGTH(CAR(s));
	    else if (len != LENGTH(CAR(s)))
		goto bad;
	}
	else
	    error(R_MSG_type, type2char(TYPEOF(CAR(s))));
    }

    if (len < 0)
	error(_("no input has determined the number of cases"));
    PROTECT(rval = allocVector(LGLSXP, len));
    for (i = 0; i < len; i++) INTEGER(rval)[i] = 1;
    /* FIXME : there is a lot of shared code here for vectors. */
    /* It should be abstracted out and optimized. */
    for (s = args; s != R_NilValue; s = CDR(s)) {
	if (isList(CAR(s))) {
	    /* Now we only need to worry about vectors */
	    /* since we use mod to handle arrays. */
	    /* FIXME : using mod like this causes */
	    /* a potential performance hit. */
	    for (t = CAR(s); t != R_NilValue; t = CDR(t)) {
		u = CAR(t);
		for (i = 0; i < LENGTH(u); i++) {
		    switch (TYPEOF(u)) {
		    case INTSXP:
		    case LGLSXP:
			if (INTEGER(u)[i] == NA_INTEGER)
			    INTEGER(rval)[i % len] = 0;
			break;
		    case REALSXP:
			if (ISNAN(REAL(u)[i]))
			    INTEGER(rval)[i % len] = 0;
			break;
		    case CPLXSXP:
			if (ISNAN(COMPLEX(u)[i].r) || ISNAN(COMPLEX(u)[i].i))
			    INTEGER(rval)[i % len] = 0;
			break;
		    case STRSXP:
			if (STRING_ELT(u, i) == NA_STRING)
			    INTEGER(rval)[i % len] = 0;
			break;
		    default:
			UNPROTECT(1);
			error(R_MSG_type, type2char(TYPEOF(u)));
		    }
		}
	    }
	}
	if (isNewList(CAR(s))) {
	    int it, nt;
	    t = CAR(s);
	    nt = length(t);
	    for (it = 0 ; it < nt ; it++) {
		u = VECTOR_ELT(t, it);
		for (i = 0; i < LENGTH(u); i++) {
		    switch (TYPEOF(u)) {
		    case INTSXP:
		    case LGLSXP:
			if (INTEGER(u)[i] == NA_INTEGER)
			    INTEGER(rval)[i % len] = 0;
			break;
		    case REALSXP:
			if (ISNAN(REAL(u)[i]))
			    INTEGER(rval)[i % len] = 0;
			break;
		    case CPLXSXP:
			if (ISNAN(COMPLEX(u)[i].r) || ISNAN(COMPLEX(u)[i].i))
			    INTEGER(rval)[i % len] = 0;
			break;
		    case STRSXP:
			if (STRING_ELT(u, i) == NA_STRING)
			    INTEGER(rval)[i % len] = 0;
			break;
		    default:
			UNPROTECT(1);
			error(R_MSG_type, type2char(TYPEOF(u)));
		    }
		}
	    }
	}
	else {
	    for (i = 0; i < LENGTH(CAR(s)); i++) {
		u = CAR(s);
		switch (TYPEOF(u)) {
		case INTSXP:
		case LGLSXP:
		    if (INTEGER(u)[i] == NA_INTEGER)
			INTEGER(rval)[i % len] = 0;
		    break;
		case REALSXP:
		    if (ISNAN(REAL(u)[i]))
			INTEGER(rval)[i % len] = 0;
		    break;
		case CPLXSXP:
		    if (ISNAN(COMPLEX(u)[i].r) || ISNAN(COMPLEX(u)[i].i))
			INTEGER(rval)[i % len] = 0;
		    break;
		case STRSXP:
		    if (STRING_ELT(u, i) == NA_STRING)
			INTEGER(rval)[i % len] = 0;
		    break;
		default:
		    UNPROTECT(1);
		    error(R_MSG_type, type2char(TYPEOF(u)));
		}
	    }
	}
    }
    UNPROTECT(1);
    return rval;

 bad:
    error(_("not all arguments have the same length"));
}

/* op = 0 is pmin, op = 1 is pmax
   NULL and logicals are handled as if they had been coerced to integer.
 */
static SEXP do_pmin(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP a, x, ans;
    int i, n, len, narm;
    SEXPTYPE type, anstype;

    narm = asLogical(CAR(args));
    if(narm == NA_LOGICAL)
	error(_("invalid '%s' value"), "na.rm");
    args = CDR(args);
    x = CAR(args);
    if(args == R_NilValue) error(_("no arguments"));

    anstype = TYPEOF(x);
    switch(anstype) {
    case NILSXP:
    case LGLSXP:
    case INTSXP:
    case REALSXP:
    case STRSXP:
	break;
    default:
	error(_("invalid input type"));
    }
    a = CDR(args);
    if(a == R_NilValue) return x; /* one input */

    len = length(x); /* not LENGTH, as NULL is allowed */
    for(; a != R_NilValue; a = CDR(a)) {
	x = CAR(a);
	type = TYPEOF(x);
	switch(type) {
	case NILSXP:
	case LGLSXP:
	case INTSXP:
	case REALSXP:
	case STRSXP:
	    break;
	default:
	    error(_("invalid input type"));
	}
	if(type > anstype) anstype = type;  /* RELIES ON SEXPTYPE ORDERING! */
	n = length(x);
	if ((len > 0) ^ (n > 0)) {
	    // till 2.15.0:  error(_("cannot mix 0-length vectors with others"));
	    len = 0;
	    break;
	}
	len = imax2(len, n);
    }
    if(anstype < INTSXP) anstype = INTSXP;

    if(len == 0) return allocVector(anstype, 0);

    /* Check for fractional recycling (added in 2.14.0) */
    for(a = args; a != R_NilValue; a = CDR(a)) {
	n = LENGTH(CAR(a));
	if (len % n != 0) {
	    warning(_("an argument will be fractionally recycled"));
	    break;
	}
    }

    PROTECT(ans = allocVector(anstype, len));
    switch(anstype) {
    case INTSXP:
    {
	int *r,  *ra = INTEGER(ans), tmp;
	PROTECT(x = coerceVector(CAR(args), anstype));
	r = INTEGER(x);
	n = LENGTH(x);
        if (n == len)
	    for (i = 0; i < len; i++) ra[i] = r[i];
        else if (n == 1)
	    for (i = 0; i < len; i++) ra[i] = r[0];
        else
	    for (i = 0; i < len; i++) ra[i] = r[i % n];
	UNPROTECT(1);
	for(a = CDR(args); a != R_NilValue; a = CDR(a)) {
	    PROTECT(x = coerceVector(CAR(a), anstype));
	    n = LENGTH(x);
	    r = INTEGER(x);
	    for (i = 0; i < len; i++) {
		tmp = r[i % n];
                if (tmp == NA_INTEGER) {
                    if (narm && ra[i] != NA_INTEGER) continue;
                }
                else if (ra[i] == NA_INTEGER) {
                    if (!narm) continue;
                }
                else if (PRIMVAL(op)==1 ? tmp <= ra[i] : tmp >= ra[i])
                    continue;
                ra[i] = tmp;
	    }
	    UNPROTECT(1);
	}
    }
	break;
    case REALSXP:
    {
	double *r, *ra = REAL(ans), tmp;
	PROTECT(x = coerceVector(CAR(args), anstype));
	r = REAL(x);
	n = LENGTH(x);
        if (n == len)
	    for (i = 0; i < len; i++) ra[i] = r[i];
        else if (n == 1)
	    for (i = 0; i < len; i++) ra[i] = r[0];
        else
	    for (i = 0; i < len; i++) ra[i] = r[i % n];
	UNPROTECT(1);
	for(a = CDR(args); a != R_NilValue; a = CDR(a)) {
	    PROTECT(x = coerceVector(CAR(a), anstype));
	    n = LENGTH(x);
	    r = REAL(x);
	    for (i = 0; i < len; i++) {
		tmp = r[i % n];
                if (ISNAN(tmp)) {
                    if (narm && !ISNAN(ra[i])) continue;
                }
                else if (ISNAN(ra[i])) {
                    if (!narm) continue;
                }
                else if (PRIMVAL(op)==1 ? tmp <= ra[i] : tmp >= ra[i])
                    continue;
                ra[i] = tmp;
            }
	    UNPROTECT(1);
	}
    }
	break;
    case STRSXP:
    {
	PROTECT(x = coerceVector(CAR(args), anstype));
	n = LENGTH(x);
        if (n == len)
	    for (i = 0; i < len; i++)
                SET_STRING_ELT(ans, i, STRING_ELT(x, i));
        else if (n == 1)
	    for (i = 0; i < len; i++)
                SET_STRING_ELT(ans, i, STRING_ELT(x, 0));
        else
	    for (i = 0; i < len; i++)
                SET_STRING_ELT(ans, i, STRING_ELT(x, i % n));
	UNPROTECT(1);
	for(a = CDR(args); a != R_NilValue; a = CDR(a)) {
	    SEXP tmp, t2;
	    PROTECT(x = coerceVector(CAR(a), anstype));
	    n = LENGTH(x);
	    for(i = 0; i < len; i++) {
		tmp = STRING_ELT(x, i % n);
		t2 = STRING_ELT(ans, i);
                if (tmp == NA_STRING) {
                    if (narm && t2 != NA_STRING) continue;
                }
                else if (t2 == NA_STRING) {
                    if (!narm) continue;
                }
                else if (PRIMVAL(op)==1 ? Scollate (tmp, t2) <= 0 
                                        : Scollate (tmp, t2) >= 0)
                    continue;
                SET_STRING_ELT(ans, i, tmp);
	    }
	    UNPROTECT(1);
	}
    }
	break;
    default:
	break;
    }
    UNPROTECT(1);
    return ans;
}

/* FUNTAB entries defined in this source file. See names.c for documentation. */

attribute_hidden FUNTAB R_FunTab_summary[] =
{
/* printname	c-entry		offset	eval	arity	pp-kind	     precedence	rightassoc */

{"mean",	do_mean,	0,	11,	1,	{PP_FUNCALL, PREC_FN,	0}},
{"range",	do_range,	0,	1,	-1,	{PP_FUNCALL, PREC_FN,	0}},
{"which.min",	do_first_min,	0,	11,	1,	{PP_FUNCALL, PREC_FN,	0}},
{"which.max",	do_first_min,	1,	11,	1,	{PP_FUNCALL, PREC_FN,	0}},
{"which",	do_which,	0,	11,	1,	{PP_FUNCALL, PREC_FN,	0}},
{"complete.cases",do_compcases,	0,	11,	1,	{PP_FUNCALL, PREC_FN,	0}},
{"pmin",	do_pmin,	0,	11,	-1,	{PP_FUNCALL, PREC_FN,	0}},
{"pmax",	do_pmin,	1,	11,	-1,	{PP_FUNCALL, PREC_FN,	0}},

/* these four are group generic and so need to eval args */
{"sum",		do_summary,	0,	10001,	-1,	{PP_FUNCALL, PREC_FN,	0}},
{"min",		do_summary,	2,	1,	-1,	{PP_FUNCALL, PREC_FN,	0}},
{"max",		do_summary,	3,	1,	-1,	{PP_FUNCALL, PREC_FN,	0}},
{"prod",	do_summary,	4,	1,	-1,	{PP_FUNCALL, PREC_FN,	0}},

{NULL,		NULL,		0,	0,	0,	{PP_INVALID, PREC_FN,	0}}
};

/* Fast built-in functions in this file. See names.c for documentation */

attribute_hidden FASTFUNTAB R_FastFunTab_summary[] = {
/*slow func	fast func,   code or -1  dsptch variant */
{ do_summary,	do_fast_sum,	0,	    1,  VARIANT_ANY_ATTR|VARIANT_SUM },
{ do_summary,	do_fast_prod,	4,	    1,  VARIANT_ANY_ATTR },
{ 0,		0,		0,	    0,  0 }
};
