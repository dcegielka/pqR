/*
 *  pqR : A pretty quick version of R
 *  Copyright (C) 2013 by Radford M. Neal
 *
 *  Based on R : A Computer Language for Statistical Data Analysis
 *  Copyright (C) 1995, 1996, 1997  Robert Gentleman and Ross Ihaka
 *  Copyright (C) 1998--2011	    The R Development Core Team.
 *  Copyright (C) 2003-4	    The R Foundation
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

#ifdef __OpenBSD__
/* for definition of "struct exception" in math.h */
# define __LIBM_PRIVATE
#endif
#define USE_FAST_PROTECT_MACROS
#include "Defn.h"		/*-> Arith.h -> math.h */
#ifdef __OpenBSD__
# undef __LIBM_PRIVATE
#endif

#define R_MSG_NA	_("NaNs produced")
#define R_MSG_NONNUM_MATH _("Non-numeric argument to mathematical function")

#include <Rmath.h>
extern double Rf_gamma_cody(double);

#include "arithmetic.h"

#include <errno.h>

#ifdef HAVE_MATHERR

/* Override the SVID matherr function:
   the main difference here is not to print warnings.
 */
#ifndef __cplusplus
int matherr(struct exception *exc)
{
    switch (exc->type) {
    case DOMAIN:
    case SING:
	errno = EDOM;
	break;
    case OVERFLOW:
	errno = ERANGE;
	break;
    case UNDERFLOW:
	exc->retval = 0.0;
	break;
	/*
	   There are cases TLOSS and PLOSS which are ignored here.
	   According to the Solaris man page, there are for
	   trigonometric algorithms and not needed for good ones.
	 */
    }
    return 1;
}
#endif
#endif

#ifndef _AIX
const double R_Zero_Hack = 0.0;	/* Silence the Sun compiler */
#else
double R_Zero_Hack = 0.0;
#endif
typedef union
{
    double value;
    unsigned int word[2];
} ieee_double;

/* gcc had problems with static const on AIX and Solaris
   Solaris was for gcc 3.1 and 3.2 under -O2 32-bit on 64-bit kernel */
#ifdef _AIX
#define CONST
#elif defined(sparc) && defined (__GNUC__) && __GNUC__ == 3
#define CONST
#else
#define CONST const
#endif

#ifdef WORDS_BIGENDIAN
static CONST int hw = 0;
static CONST int lw = 1;
#else  /* !WORDS_BIGENDIAN */
static CONST int hw = 1;
static CONST int lw = 0;
#endif /* WORDS_BIGENDIAN */


static double R_ValueOfNA(void)
{
    /* The gcc shipping with RedHat 9 gets this wrong without
     * the volatile declaration. Thanks to Marc Schwartz. */
    volatile ieee_double x;
    x.word[hw] = 0x7ff00000;
    x.word[lw] = 1954;
    return x.value;
}

int R_IsNA(double x)
{
    if (isnan(x)) {
	ieee_double y;
	y.value = x;
	return (y.word[lw] == 1954);
    }
    return 0;
}

int R_IsNaN(double x)
{
    if (isnan(x)) {
	ieee_double y;
	y.value = x;
	return (y.word[lw] != 1954);
    }
    return 0;
}

/* ISNAN uses isnan, which is undefined by C++ headers
   This workaround is called only when ISNAN() is used
   in a user code in a file with __cplusplus defined */

int R_isnancpp(double x)
{
   return (isnan(x)!=0);
}


/* Mainly for use in packages */
int R_finite(double x)
{
#ifdef HAVE_WORKING_ISFINITE
    return isfinite(x);
#else
    return (!isnan(x) & (x != R_PosInf) & (x != R_NegInf));
#endif
}


/* Arithmetic Initialization */

void attribute_hidden InitArithmetic()
{
    R_NaInt = INT_MIN; /* now mostly unused: NA_INTEGER defined as INT_MIN */
    R_NaN = 0.0/R_Zero_Hack;
    R_NaReal = R_ValueOfNA();
    R_PosInf = 1.0/R_Zero_Hack;
    R_NegInf = -1.0/R_Zero_Hack;
    R_NaN_cast_to_int = (int) R_NaN;

#ifdef ENABLE_ISNAN_TRICK
    if (R_NaN_cast_to_int != (int) R_NaReal
     || R_NaN_cast_to_int != (int) (-R_NaReal)
     || R_NaN_cast_to_int != (int) (-R_NaN))
        error("Integer casts of NaN, NA, -NaN, -NA differ, don't define ENABLE_ISNAN_TRICK");
#endif
}

/* Keep these two in step */
/* FIXME: consider using
    tmp = (long double)x1 - floor(q) * (long double)x2;
 */
static double myfmod(double x1, double x2)
{
    double q = x1 / x2, tmp;

    if (x2 == 0.0) return R_NaN;
    tmp = x1 - floor(q) * x2;
    if(R_FINITE(q) && (fabs(q) > 1/R_AccuracyInfo.eps))
	warning(_("probable complete loss of accuracy in modulus"));
    q = floor(tmp/x2);
    return tmp - q * x2;
}

static double myfloor(double x1, double x2)
{
    double q = x1 / x2, tmp;

    if (x2 == 0.0) return q;
    tmp = x1 - floor(q) * x2;
    return floor(q) + floor(tmp/x2);
}

/* some systems get this wrong, possibly depend on what libs are loaded */
static R_INLINE double R_log(double x) {
    return x > 0 ? log(x) : x < 0 ? R_NaN : R_NegInf;
}

/* Macro handling powers 1 and 2 quickly, and otherwise using R_pow.  
   First argument should be double, second may be double or int. */

#define R_POW(x,y) ((y) == 2 ? (x)*(x) : (y) == 1 ? (x) : R_pow((x),(y)))

double R_pow(double x, double y) /* = x ^ y */
{
    /* Don't optimize for powers of 1 or 2, since we assume most calls are
       via R_POW, which already does that, or from some other place making
       similar checks. */

    if(x == 1. || y == 0.)
	return(1.);
    if(x == 0.) {
	if(y > 0.) return(0.);
	else if(y < 0) return(R_PosInf);
	else return(y); /* NA or NaN, we assert */
    }

    if (R_FINITE(x) && R_FINITE(y))
        if (y == 0.5)
            return sqrt(x);
        else
            return pow(x, y);

    if (ISNAN(x) || ISNAN(y))
	return(x + y);
    if(!R_FINITE(x)) {
	if(x > 0)		/* Inf ^ y */
	    return (y < 0.)? 0. : R_PosInf;
	else {			/* (-Inf) ^ y */
	    if(R_FINITE(y) && y == floor(y)) /* (-Inf) ^ n */
		return (y < 0.) ? 0. : (myfmod(y, 2.) ? x  : -x);
	}
    }
    if(!R_FINITE(y)) {
	if(x >= 0) {
	    if(y > 0)		/* y == +Inf */
		return (x >= 1) ? R_PosInf : 0.;
	    else		/* y == -Inf */
		return (x < 1) ? R_PosInf : 0.;
	}
    }
    return(R_NaN);		/* all other cases: (-Inf)^{+-Inf,
				   non-int}; (neg)^{+-Inf} */
}

double R_pow_di(double x, int n)
{
    double xn = 1.0;

    if (ISNAN(x)) return x;
    if (n == NA_INTEGER) return NA_REAL;

    if (n != 0) {
	if (!R_FINITE(x)) return R_POW(x, n);

	Rboolean is_neg = (n < 0);
	if(is_neg) n = -n;
	for(;;) {
	    if(n & 01) xn *= x;
	    if(n >>= 1) x *= x; else break;
	}
        if(is_neg) xn = 1. / xn;
    }
    return xn;
}


/* General Base Logarithms */

/* Note that the behaviour of log(0) required is not necessarily that
   mandated by C99 (-HUGE_VAL), and the behaviour of log(x < 0) is
   optional in C99.  Some systems return -Inf for log(x < 0), e.g.
   libsunmath on Solaris.
*/
static double logbase(double x, double base)
{
#ifdef HAVE_LOG10
    if(base == 10) return x > 0 ? log10(x) : x < 0 ? R_NaN : R_NegInf;
#endif
#ifdef HAVE_LOG2
    if(base == 2) return x > 0 ? log2(x) : x < 0 ? R_NaN : R_NegInf;
#endif
    return R_log(x) / R_log(base);
}

SEXP R_unary(SEXP, SEXP, SEXP);
SEXP R_binary(SEXP, SEXP, SEXP, SEXP);
static SEXP integer_unary(ARITHOP_TYPE, SEXP, SEXP);
static SEXP real_unary(ARITHOP_TYPE, SEXP, SEXP);
static SEXP real_binary(ARITHOP_TYPE, SEXP, SEXP);
static SEXP integer_binary(ARITHOP_TYPE, SEXP, SEXP, SEXP);

#if 0
static int naflag;
static SEXP lcall;
#endif


/* Unary and Binary Operators */

static SEXP do_fast_arith (SEXP call, SEXP op, SEXP arg1, SEXP arg2, SEXP env,
                           int variant)
{
    /* Quickly do real arithmetic and integer plus/minus on scalars with 
       no attributes. */

    int type = TYPEOF(arg1);

    if ((type==REALSXP || type==INTSXP) && LENGTH(arg1)==1 
                                        && ATTRIB(arg1)==R_NilValue) {
        int opcode = PRIMVAL(op);

        if (arg2==NULL) {
            switch (opcode) {
            case PLUSOP:
                return arg1;
            case MINUSOP: ;
                SEXP ans = NAMEDCNT_EQ_0(arg1) ? arg1 : allocVector(type,1);
                if (type==REALSXP) 
                    *REAL(ans) = - *REAL(arg1);
                else /* INTSXP */
                    *INTEGER(ans) = *INTEGER(arg1)==NA_INTEGER ? NA_INTEGER
                                      : - *INTEGER(arg1);
                return ans;
            }
        }
        else if (TYPEOF(arg2)==type && LENGTH(arg2)==1 
                                    && ATTRIB(arg2)==R_NilValue) {
            if (type==REALSXP) {

                SEXP ans = NAMEDCNT_EQ_0(arg1) ? arg1 
                         : NAMEDCNT_EQ_0(arg2) ? arg2
                         : allocVector(type,1);
    
                double a1 = *REAL(arg1), a2 = *REAL(arg2);
        
                switch (opcode) {
                case PLUSOP:
                    *REAL(ans) = a1 + a2;
                    return ans;
                case MINUSOP:
                    *REAL(ans) = a1 - a2;
                    return ans;
                case TIMESOP:
                    *REAL(ans) = a1 * a2;
                    return ans;
                case DIVOP:
                    *REAL(ans) = a1 / a2;
                    return ans;
                case POWOP:
                    if (a2 == 2.0)       *REAL(ans) = a1 * a1;
                    else if (a2 == 1.0)  *REAL(ans) = a1;
                    else if (a2 == 0.0)  *REAL(ans) = 1.0;
                    else if (a2 == -1.0) *REAL(ans) = 1.0 / a1;
                    else                 *REAL(ans) = R_pow(a1,a2);
                    return ans;
                case MODOP:
                    *REAL(ans) = myfmod(a1,a2);
                    return ans;
                case IDIVOP:
                    *REAL(ans) = myfloor(a1,a2);
                    return ans;
                }
            }
            else if (opcode==PLUSOP || opcode==MINUSOP) { /* type==INTSXP */

                SEXP ans = NAMEDCNT_EQ_0(arg1) ? arg1 
                         : NAMEDCNT_EQ_0(arg2) ? arg2
                         : allocVector(type,1);

                int a1 = *INTEGER(arg1), a2 = *INTEGER(arg2);

                if (a1==NA_INTEGER || a2==NA_INTEGER) {
                    *INTEGER(ans) = NA_INTEGER;
                    return ans;
                }

                if (opcode==MINUSOP) a2 = -a2;

                *INTEGER(ans) = a1 + a2;

                if (a1>0 ? (a2>0 && *INTEGER(ans)<0) 
                         : (a2<0 && *INTEGER(ans)>0)) {
                    warningcall(call, _("NAs produced by integer overflow"));
                    *INTEGER(ans) = NA_INTEGER;
                }

                return ans;
            }
        }
    }

    /* Otherwise, handle the general case. */

    return arg2==NULL ? R_unary (call, op, arg1) 
                      : R_binary (call, op, arg1, arg2);
}

SEXP attribute_hidden do_arith(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP ans;

    if (DispatchGroup("Ops", call, op, args, env, &ans))
	return ans;

    if (PRIMFUN_FAST(op)==0)
        SET_PRIMFUN_FAST_BINARY (op, do_fast_arith, 1, 1, 0, 0, 1);

    switch (length(args)) {
    case 1:
	return R_unary(call, op, CAR(args));
    case 2:
	return R_binary(call, op, CAR(args), CADR(args));
    default:
	errorcall(call,_("operator needs one or two arguments"));
    }
    return ans;			/* never used; to keep -Wall happy */
}

#define COERCE_IF_NEEDED(v, tp, vpi) do { \
    if (TYPEOF(v) != (tp)) { \
	int __vo__ = OBJECT(v); \
	REPROTECT(v = coerceVector(v, (tp)), vpi); \
	if (__vo__) SET_OBJECT(v, 1); \
    } \
} while (0)

#define FIXUP_NULL_AND_CHECK_TYPES(v, vpi) do { \
    switch (TYPEOF(v)) { \
    case NILSXP: REPROTECT(v = allocVector(REALSXP,0), vpi); break; \
    case CPLXSXP: case REALSXP: case INTSXP: case LGLSXP: break; \
    default: errorcall(lcall, _("non-numeric argument to binary operator")); \
    } \
} while (0)

SEXP attribute_hidden R_binary(SEXP call, SEXP op, SEXP x, SEXP y)
{
    SEXP klass, dims, tsp, xnames, ynames, val;
    int mismatch = 0, nx, ny, xarray, yarray, xts, yts, xS4 = 0, yS4 = 0;
    int xattr, yattr;
    SEXP lcall = call;
    PROTECT_INDEX xpi, ypi;
    ARITHOP_TYPE oper = (ARITHOP_TYPE) PRIMVAL(op);
    int nprotect = 2; /* x and y */


    PROTECT_WITH_INDEX(x, &xpi);
    PROTECT_WITH_INDEX(y, &ypi);

    FIXUP_NULL_AND_CHECK_TYPES(x, xpi);
    FIXUP_NULL_AND_CHECK_TYPES(y, ypi);

    nx = LENGTH(x);
    if (ATTRIB(x) != R_NilValue) {
	xattr = TRUE;
	xarray = isArray(x);
	xts = isTs(x);
	xS4 = isS4(x);
    }
    else xarray = xts = xattr = FALSE;
    ny = LENGTH(y);
    if (ATTRIB(y) != R_NilValue) {
	yattr = TRUE;
	yarray = isArray(y);
	yts = isTs(y);
	yS4 = isS4(y);
    }
    else yarray = yts = yattr = FALSE;

    /* If either x or y is a matrix with length 1 and the other is a
       vector, we want to coerce the matrix to be a vector. */

    if (xarray != yarray) {
	if (xarray && nx==1 && ny!=1) {
	    REPROTECT(x = duplicate(x), xpi);
	    setAttrib(x, R_DimSymbol, R_NilValue);
	}
	if (yarray && ny==1 && nx!=1) {
	    REPROTECT(y = duplicate(y), ypi);
	    setAttrib(y, R_DimSymbol, R_NilValue);
	}
    }

    if (xarray || yarray) {
	if (xarray && yarray && !conformable(x,y))
		errorcall(lcall, _("non-conformable arrays"));
        PROTECT(dims = getAttrib (xarray ? x : y, R_DimSymbol));
	nprotect++;
	if (xattr) {
	    PROTECT(xnames = getAttrib(x, R_DimNamesSymbol));
	    nprotect++;
	}
	else xnames = R_NilValue;
	if (yattr) {
	    PROTECT(ynames = getAttrib(y, R_DimNamesSymbol));
	    nprotect++;
	}
	else ynames = R_NilValue;
    }
    else {
	dims = R_NilValue;
	if (xattr) {
	    PROTECT(xnames = getAttrib(x, R_NamesSymbol));
	    nprotect++;
	}
	else xnames = R_NilValue;
	if (yattr) {
	    PROTECT(ynames = getAttrib(y, R_NamesSymbol));
	    nprotect++;
	}
	else ynames = R_NilValue;
    }
    if (nx == ny || nx == 1 || ny == 1) mismatch = 0;
    else if (nx > 0 && ny > 0) {
	if (nx > ny) mismatch = nx % ny;
	else mismatch = ny % nx;
    }

    if (xts || yts) {
	if (xts && yts) {
	    if (!tsConform(x, y))
		errorcall(lcall, _("non-conformable time-series"));
	    PROTECT(tsp = getAttrib(x, R_TspSymbol));
	    PROTECT(klass = getAttrib(x, R_ClassSymbol));
	}
	else if (xts) {
	    if (nx < ny)
		ErrorMessage(lcall, ERROR_TSVEC_MISMATCH);
	    PROTECT(tsp = getAttrib(x, R_TspSymbol));
	    PROTECT(klass = getAttrib(x, R_ClassSymbol));
	}
	else {			/* (yts) */
	    if (ny < nx)
		ErrorMessage(lcall, ERROR_TSVEC_MISMATCH);
	    PROTECT(tsp = getAttrib(y, R_TspSymbol));
	    PROTECT(klass = getAttrib(y, R_ClassSymbol));
	}
	nprotect += 2;
    }
    else klass = tsp = NULL; /* -Wall */

    if (mismatch)
	warningcall(lcall,
		    _("longer object length is not a multiple of shorter object length"));

    /* need to preserve object here, as *_binary copies class attributes */
    if (TYPEOF(x) == CPLXSXP || TYPEOF(y) == CPLXSXP) {
	COERCE_IF_NEEDED(x, CPLXSXP, xpi);
	COERCE_IF_NEEDED(y, CPLXSXP, ypi);
	val = complex_binary(oper, x, y);
    }
    else if (TYPEOF(x) == REALSXP || TYPEOF(y) == REALSXP) {
         /* real_binary can handle REAL and INT, but not LOGICAL, operands */
        if (TYPEOF(x) != INTSXP) COERCE_IF_NEEDED(x, REALSXP, xpi);
        if (TYPEOF(y) != INTSXP) COERCE_IF_NEEDED(y, REALSXP, ypi);
	val = real_binary(oper, x, y);
    }
    else {
        /* integer_binary is assumed to work for LOGICAL too, which won't
           be true if the aren't really the same */
        val = integer_binary(oper, x, y, lcall);
    }

    /* quick return if there are no attributes */
    if (! xattr && ! yattr) {
	UNPROTECT(nprotect);
	return val;
    }

    PROTECT(val);
    nprotect++;

    /* Don't set the dims if one argument is an array of size 0 and the
       other isn't of size zero, cos they're wrong */
    /* Not if the other argument is a scalar (PR#1979) */
    if (dims != R_NilValue) {
	if (!((xarray && (nx == 0) && (ny > 1)) ||
	      (yarray && (ny == 0) && (nx > 1)))){
	    setAttrib(val, R_DimSymbol, dims);
	    if (xnames != R_NilValue)
		setAttrib(val, R_DimNamesSymbol, xnames);
	    else if (ynames != R_NilValue)
		setAttrib(val, R_DimNamesSymbol, ynames);
	}
    }
    else {
	if (LENGTH(val) == length(xnames))
	    setAttrib(val, R_NamesSymbol, xnames);
	else if (LENGTH(val) == length(ynames))
	    setAttrib(val, R_NamesSymbol, ynames);
    }

    if (xts || yts) {		/* must set *after* dims! */
	setAttrib(val, R_TspSymbol, tsp);
	setAttrib(val, R_ClassSymbol, klass);
    }

    if(xS4 || yS4) {   /* Only set the bit:  no method defined! */
        val = asS4(val, TRUE, TRUE);
    }
    UNPROTECT(nprotect);
    return val;
}

SEXP attribute_hidden R_unary(SEXP call, SEXP op, SEXP s1)
{
    ARITHOP_TYPE operation = (ARITHOP_TYPE) PRIMVAL(op);
    switch (TYPEOF(s1)) {
    case LGLSXP:
    case INTSXP:
	return integer_unary(operation, s1, call);
    case REALSXP:
	return real_unary(operation, s1, call);
    case CPLXSXP:
	return complex_unary(operation, s1, call);
    default:
	errorcall(call, _("invalid argument to unary operator"));
    }
    return s1;			/* never used; to keep -Wall happy */
}

static SEXP integer_unary(ARITHOP_TYPE code, SEXP s1, SEXP call)
{
    int i, n, x;
    SEXP ans;

    switch (code) {
    case PLUSOP:
	return s1;
    case MINUSOP:
	n = LENGTH(s1);
	ans = NAMED(s1)==0 ? s1 : duplicate(s1);
	SET_TYPEOF(ans, INTSXP);  /* Assumes LGLSXP is really the same... */
	for (i = 0; i < n; i++) {
	    x = INTEGER(s1)[i];
	    INTEGER(ans)[i] = x==NA_INTEGER ? NA_INTEGER : -x;
	}
	return ans;
    default:
	errorcall(call, _("invalid unary operator"));
    }
    return s1;			/* never used; to keep -Wall happy */
}

static SEXP real_unary(ARITHOP_TYPE code, SEXP s1, SEXP lcall)
{
    int i, n;
    SEXP ans;

    switch (code) {
    case PLUSOP: return s1;
    case MINUSOP:
	n = LENGTH(s1);
        ans = NAMED(s1)==0 ? s1 : duplicate(s1);
	for (i = 0; i < n; i++)
	    REAL(ans)[i] = -REAL(s1)[i];
	return ans;
    default:
	errorcall(lcall, _("invalid unary operator"));
    }
    return s1;			/* never used; to keep -Wall happy */
}

/* i1 = i % n1; i2 = i % n2;
 * this macro is quite a bit faster than having real modulo calls
 * in the loop (tested on Intel and Sparc)
 */
#define mod_iterate(n1,n2,i1,i2) for (i=i1=i2=0; i<n; \
	i1 = (++i1 == n1) ? 0 : i1,\
	i2 = (++i2 == n2) ? 0 : i2,\
	++i)



/* The tests using integer comparisons are a bit faster than the tests
   using doubles, but they depend on a two's complement representation
   (but that is almost universal).  The tests that compare results to
   double's depend on being able to accurately represent all int's as
   double's.  Since int's are almost universally 32 bit that should be
   OK. */

#ifndef INT_32_BITS
/* configure checks whether int is 32 bits.  If not this code will
   need to be rewritten.  Since 32 bit ints are pretty much universal,
   we can worry about writing alternate code when the need arises.
   To be safe, we signal a compiler error if int is not 32 bits. */
# error code requires that int have 32 bits
#else
/* Just to be on the safe side, configure ought to check that the
   mashine uses two's complement. A define like
#define USES_TWOS_COMPLEMENT (~0 == (unsigned) -1)
   might work, but at least one compiler (CodeWarrior 6) chokes on it.
   So for now just assume it is true.
*/
#define USES_TWOS_COMPLEMENT 1

#if USES_TWOS_COMPLEMENT
# define OPPOSITE_SIGNS(x, y) ((x < 0) ^ (y < 0))
# define GOODISUM(x, y, z) (((x) > 0) ? ((y) < (z)) : ! ((y) < (z)))
# define GOODIDIFF(x, y, z) (!(OPPOSITE_SIGNS(x, y) && OPPOSITE_SIGNS(x, z)))
#else
# define GOODISUM(x, y, z) ((double) (x) + (double) (y) == (z))
# define GOODIDIFF(x, y, z) ((double) (x) - (double) (y) == (z))
#endif
#define GOODIPROD(x, y, z) ((double) (x) * (double) (y) == (z))
#define INTEGER_OVERFLOW_WARNING _("NAs produced by integer overflow")
#endif

static SEXP integer_binary(ARITHOP_TYPE code, SEXP s1, SEXP s2, SEXP lcall)
{
    int i, i1, i2, n, n1, n2;
    int x1, x2;
    SEXP ans;
    Rboolean naflag = FALSE;

    n1 = LENGTH(s1);
    n2 = LENGTH(s2);
    /* S4-compatibility change: if n1 or n2 is 0, result is of length 0 */
    n = n1==0 || n2==0 ? 0 : n1>n2 ? n1 : n2;

    if (code == DIVOP || code == POWOP)
	ans = allocVector(REALSXP, n);
    else {
        ans = can_save_alloc (s1, s2, INTSXP);
	if (ans==R_NilValue) 
            ans = allocVector(INTSXP, n);
    }

    if (n==0) return(ans);

    PROTECT(ans);

#ifdef R_MEMORY_PROFILING
    if (RTRACE(s1) || RTRACE(s2)) {
       if (RTRACE(s1) && RTRACE(s2)) {
	  if (n1 > n2)
	      memtrace_report(s1, ans);
	  else
	      memtrace_report(s2, ans);
       } else if (RTRACE(s1))
	   memtrace_report(s1, ans);
       else /* only s2 */
	   memtrace_report(s2, ans);
       SET_RTRACE(ans, 1);
    }
#endif

    switch (code) {
    case PLUSOP:
	mod_iterate(n1, n2, i1, i2) {
	    x1 = INTEGER(s1)[i1];
	    x2 = INTEGER(s2)[i2];
	    if (x1 == NA_INTEGER || x2 == NA_INTEGER)
		INTEGER(ans)[i] = NA_INTEGER;
	    else {
		int val = x1 + x2;
		if (val != NA_INTEGER && GOODISUM(x1, x2, val))
		    INTEGER(ans)[i] = val;
		else {
		    INTEGER(ans)[i] = NA_INTEGER;
		    naflag = TRUE;
		}
	    }
	}
	if (naflag)
	    warningcall(lcall, INTEGER_OVERFLOW_WARNING);
	break;
    case MINUSOP:
	mod_iterate(n1, n2, i1, i2) {
	    x1 = INTEGER(s1)[i1];
	    x2 = INTEGER(s2)[i2];
	    if (x1 == NA_INTEGER || x2 == NA_INTEGER)
		INTEGER(ans)[i] = NA_INTEGER;
	    else {
		int val = x1 - x2;
		if (val != NA_INTEGER && GOODIDIFF(x1, x2, val))
		    INTEGER(ans)[i] = val;
		else {
		    naflag = TRUE;
		    INTEGER(ans)[i] = NA_INTEGER;
		}
	    }
	}
	if (naflag)
	    warningcall(lcall, INTEGER_OVERFLOW_WARNING);
	break;
    case TIMESOP:
	mod_iterate(n1, n2, i1, i2) {
	    x1 = INTEGER(s1)[i1];
	    x2 = INTEGER(s2)[i2];
	    if (x1 == NA_INTEGER || x2 == NA_INTEGER)
		INTEGER(ans)[i] = NA_INTEGER;
	    else {
		int val = x1 * x2;
		if (val != NA_INTEGER && GOODIPROD(x1, x2, val))
		    INTEGER(ans)[i] = val;
		else {
		    naflag = TRUE;
		    INTEGER(ans)[i] = NA_INTEGER;
		}
	    }
	}
	if (naflag)
	    warningcall(lcall, INTEGER_OVERFLOW_WARNING);
	break;
    case DIVOP:
	mod_iterate(n1, n2, i1, i2) {
	    x1 = INTEGER(s1)[i1];
	    x2 = INTEGER(s2)[i2];
	    if (x1 == NA_INTEGER || x2 == NA_INTEGER)
		    REAL(ans)[i] = NA_REAL;
		else
		    REAL(ans)[i] = (double) x1 / (double) x2;
	}
	break;
    case POWOP:
	mod_iterate(n1, n2, i1, i2) {
	    x1 = INTEGER(s1)[i1];
	    x2 = INTEGER(s2)[i2];
	    if (x1 == NA_INTEGER || x2 == NA_INTEGER)
		REAL(ans)[i] = NA_REAL;
	    else {
		REAL(ans)[i] = R_POW((double) x1, x2);
	    }
	}
	break;
    case MODOP:
	mod_iterate(n1, n2, i1, i2) {
	    x1 = INTEGER(s1)[i1];
	    x2 = INTEGER(s2)[i2];
	    if (x1 == NA_INTEGER || x2 == NA_INTEGER || x2 == 0)
		INTEGER(ans)[i] = NA_INTEGER;
	    else {
		INTEGER(ans)[i] = /* till 0.63.2:	x1 % x2 */
		    (x1 >= 0 && x2 > 0) ? x1 % x2 :
		    (int)myfmod((double)x1,(double)x2);
	    }
	}
	break;
    case IDIVOP:
	mod_iterate(n1, n2, i1, i2) {
	    x1 = INTEGER(s1)[i1];
	    x2 = INTEGER(s2)[i2];
	    /* This had x %/% 0 == 0 prior to 2.14.1, but
	       it seems conventionally to be undefined */
	    if (x1 == NA_INTEGER || x2 == NA_INTEGER || x2 == 0)
		INTEGER(ans)[i] = NA_INTEGER;
	    else
		INTEGER(ans)[i] = floor((double)x1 / (double)x2);
	}
	break;
    }

    /* Copy attributes from longer argument. */

    if (ATTRIB(s2)!=R_NilValue && n2==n && ans!=s2)
        copyMostAttrib(s2, ans);
    if (ATTRIB(s1)!=R_NilValue && n1==n && ans!=s1)
        copyMostAttrib(s1, ans); /* Done 2nd so s1's attrs overwrite s2's */

    UNPROTECT(1);
    return ans;
}

#define R_INTEGER(robj, i) (double) (INTEGER(robj)[i] == NA_INTEGER ? NA_REAL : INTEGER(robj)[i])

static SEXP real_binary(ARITHOP_TYPE code, SEXP s1, SEXP s2)
{
    int i, i1, i2, n, n1, n2;
    SEXP ans;

    /* Note: "s1" and "s2" are protected above. */
    n1 = LENGTH(s1);
    n2 = LENGTH(s2);

    /* S4-compatibility change: if n1 or n2 is 0, result is of length 0 */
    if (n1 == 0 || n2 == 0) return(allocVector(REALSXP, 0));
    n = (n1 > n2) ? n1 : n2;

    ans = can_save_alloc (s1, s2, REALSXP);
    if (ans==R_NilValue)
        ans = allocVector(REALSXP, n);

    PROTECT(ans);

#ifdef R_MEMORY_PROFILING
    if (RTRACE(s1) || RTRACE(s2)) {
       if (RTRACE(s1) && RTRACE(s2)) {
	  if (n1 > n2)
	      memtrace_report(s1, ans);
	  else
	      memtrace_report(s2, ans);
       } else if (RTRACE(s1))
	   memtrace_report(s1,ans);
       else /* only s2 */
	   memtrace_report(s2, ans);
       SET_RTRACE(ans, 1);
    }
#endif

/*    if (n1 < 1 || n2 < 1) {
      for (i = 0; i < n; i++)
      REAL(ans)[i] = NA_REAL;
      return ans;
      } */

    switch (code) {
    case PLUSOP:
	if(TYPEOF(s1) == REALSXP && TYPEOF(s2) == REALSXP) {
            if (n2 == 1) {
                double tmp = REAL(s2)[0];
                for (i = 0; i < n; i++)
		    REAL(ans)[i] = REAL(s1)[i] + tmp;
            }
            else if (n1 == 1) {
                double tmp = REAL(s1)[0];
                for (i = 0; i < n; i++)
		    REAL(ans)[i] = tmp + REAL(s2)[i];
            }
            else if (n1 == n2)
                for (i = 0; i < n; i++)
		    REAL(ans)[i] = REAL(s1)[i] + REAL(s2)[i];
            else
                mod_iterate(n1, n2, i1, i2)
		    REAL(ans)[i] = REAL(s1)[i1] + REAL(s2)[i2];
	} else	if(TYPEOF(s1) == INTSXP ) {
	   mod_iterate(n1, n2, i1, i2) {
	       REAL(ans)[i] = R_INTEGER(s1, i1) + REAL(s2)[i2];
	     }
	} else	if(TYPEOF(s2) == INTSXP ) {
	   mod_iterate(n1, n2, i1, i2) {
	       REAL(ans)[i] = REAL(s1)[i1] + R_INTEGER(s2, i2);
	     }
	}
	break;
    case MINUSOP:
	if(TYPEOF(s1) == REALSXP && TYPEOF(s2) == REALSXP) {
            if (n2 == 1) {
                double tmp = REAL(s2)[0];
                for (i = 0; i < n; i++)
		    REAL(ans)[i] = REAL(s1)[i] - tmp;
            }
            else if (n1 == 1) {
                double tmp = REAL(s1)[0];
                for (i = 0; i < n; i++)
		    REAL(ans)[i] = tmp - REAL(s2)[i];
            }
            else if (n1 == n2)
                for (i = 0; i < n; i++)
		    REAL(ans)[i] = REAL(s1)[i] - REAL(s2)[i];
            else
                mod_iterate(n1, n2, i1, i2)
		    REAL(ans)[i] = REAL(s1)[i1] - REAL(s2)[i2];
	} else	if(TYPEOF(s1) == INTSXP ) {
	   mod_iterate(n1, n2, i1, i2) {
	       REAL(ans)[i] = R_INTEGER(s1, i1) - REAL(s2)[i2];
	   }
	} else	if(TYPEOF(s2) == INTSXP ) {
	   mod_iterate(n1, n2, i1, i2) {
	       REAL(ans)[i] = REAL(s1)[i1] - R_INTEGER(s2, i2);
	   }
	}
	break;
    case TIMESOP:
	if(TYPEOF(s1) == REALSXP && TYPEOF(s2) == REALSXP) {
            if (n2 == 1) {
                double tmp = REAL(s2)[0];
                for (i = 0; i < n; i++)
		    REAL(ans)[i] = REAL(s1)[i] * tmp;
            }
            else if (n1 == 1) {
                double tmp = REAL(s1)[0];
                for (i = 0; i < n; i++)
		    REAL(ans)[i] = tmp * REAL(s2)[i];
            }
            else if (n1 == n2)
                for (i = 0; i < n; i++)
		    REAL(ans)[i] = REAL(s1)[i] * REAL(s2)[i];
            else
                mod_iterate(n1, n2, i1, i2)
		    REAL(ans)[i] = REAL(s1)[i1] * REAL(s2)[i2];
	} else if(TYPEOF(s1) == INTSXP ) {
	   mod_iterate(n1, n2, i1, i2) {
	       REAL(ans)[i] = R_INTEGER(s1, i1) * REAL(s2)[i2];
	   }
	} else	if(TYPEOF(s2) == INTSXP ) {
	   mod_iterate(n1, n2, i1, i2) {
	       REAL(ans)[i] = REAL(s1)[i1] * R_INTEGER(s2, i2);
	   }
	}
	break;
    case DIVOP:
	if(TYPEOF(s1) == REALSXP && TYPEOF(s2) == REALSXP) {
            if (n2 == 1) {
                double tmp = REAL(s2)[0];
                for (i = 0; i < n; i++)
		    REAL(ans)[i] = REAL(s1)[i] / tmp;
            }
            else if (n1 == 1) {
                double tmp = REAL(s1)[0];
                for (i = 0; i < n; i++)
		    REAL(ans)[i] = tmp / REAL(s2)[i];
            }
            else if (n1 == n2)
                for (i = 0; i < n; i++)
		    REAL(ans)[i] = REAL(s1)[i] / REAL(s2)[i];
            else
                mod_iterate(n1, n2, i1, i2)
		    REAL(ans)[i] = REAL(s1)[i1] / REAL(s2)[i2];
	} else if(TYPEOF(s1) == INTSXP ) {
	   mod_iterate(n1, n2, i1, i2) {
	       REAL(ans)[i] = R_INTEGER(s1, i1) / REAL(s2)[i2];
	   }
	} else	if(TYPEOF(s2) == INTSXP ) {
	   mod_iterate(n1, n2, i1, i2) {
	       REAL(ans)[i] = REAL(s1)[i1] / R_INTEGER(s2, i2);
	   }
	}
	break;
    case POWOP:
	if(TYPEOF(s1) == REALSXP && TYPEOF(s2) == REALSXP) {
            if (n2 == 1) {
                double tmp = REAL(s2)[0];
                if (tmp == 2.0)
                    for (i = 0; i < n; i++) {
                        double tmp2 = REAL(s1)[i];
                        REAL(ans)[i] = tmp2 * tmp2;
                    }
                else if (tmp == 1.0)
                    for (i = 0; i < n; i++)
                        REAL(ans)[i] = REAL(s1)[i];
                else if (tmp == 0.0)
                    for (i = 0; i < n; i++)
                        REAL(ans)[i] = 1.0;
                else if (tmp == -1.0)
                    for (i = 0; i < n; i++)
                        REAL(ans)[i] = 1.0 / REAL(s1)[i];
                else
                    for (i = 0; i < n; i++)
                        REAL(ans)[i] = R_pow(REAL(s1)[i], tmp); 
            }
            else if (n1 == 1) {
                double tmp = REAL(s1)[0];
                for (i = 0; i < n; i++)
		    REAL(ans)[i] = R_POW(tmp, REAL(s2)[i]);
            }
            else if (n1 == n2)
                for (i = 0; i < n; i++)
		    REAL(ans)[i] = R_POW(REAL(s1)[i], REAL(s2)[i]);
            else
	        mod_iterate(n1, n2, i1, i2) {
	            REAL(ans)[i] = R_POW(REAL(s1)[i1], REAL(s2)[i2]);
	        }
	} else if(TYPEOF(s1) == INTSXP ) {
	   mod_iterate(n1, n2, i1, i2) {
	       REAL(ans)[i] = R_POW( R_INTEGER(s1, i1), REAL(s2)[i2]);
	   }
	} else	if(TYPEOF(s2) == INTSXP ) {
	   mod_iterate(n1, n2, i1, i2) {
	       REAL(ans)[i] = R_POW(REAL(s1)[i1], R_INTEGER(s2, i2));
	   }
	}
	break;
    case MODOP:
	if(TYPEOF(s1) == REALSXP && TYPEOF(s2) == REALSXP) {
	   mod_iterate(n1, n2, i1, i2) {
	       REAL(ans)[i] = myfmod(REAL(s1)[i1], REAL(s2)[i2]);
	   }
	} else if(TYPEOF(s1) == INTSXP ) {
	   mod_iterate(n1, n2, i1, i2) {
	       REAL(ans)[i] = myfmod( R_INTEGER(s1, i1), REAL(s2)[i2]);
	   }
	} else	if(TYPEOF(s2) == INTSXP ) {
	   mod_iterate(n1, n2, i1, i2) {
	       REAL(ans)[i] = myfmod(REAL(s1)[i1], R_INTEGER(s2, i2));
	   }
	}
	break;
    case IDIVOP:
	if(TYPEOF(s1) == REALSXP && TYPEOF(s2) == REALSXP) {
	   mod_iterate(n1, n2, i1, i2) {
	       REAL(ans)[i] = myfloor(REAL(s1)[i1], REAL(s2)[i2]);
	   }
	} else if(TYPEOF(s1) == INTSXP ) {
	   mod_iterate(n1, n2, i1, i2) {
	       REAL(ans)[i] = myfloor(R_INTEGER(s1, i1), REAL(s2)[i2]);
	   }
	} else	if(TYPEOF(s2) == INTSXP ) {
	   mod_iterate(n1, n2, i1, i2) {
	       REAL(ans)[i] = myfloor(REAL(s1)[i1], R_INTEGER(s2,i2));
	   }
	}
	break;
    }

    /* Copy attributes from arguments as needed. */

    if (ATTRIB(s2)!=R_NilValue && n2==n && ans!=s2)
        copyMostAttrib(s2, ans);
    if (ATTRIB(s1)!=R_NilValue && n1==n && ans!=s1)
        copyMostAttrib(s1, ans); /* Done 2nd so s1's attrs overwrite s2's */

    UNPROTECT(1);
    return ans;
}


/* Mathematical Functions of One Argument  Implements a variant return
   of the sum of the vector result, rather than the vector itself. */

static SEXP math1(SEXP sa, double(*f)(double), SEXP lcall, int variant)
{
    SEXP sy;
    double *a;
    int i, n;
    int naflag;

    if (!isNumeric(sa))
	errorcall(lcall, R_MSG_NONNUM_MATH);

    n = LENGTH(sa);
    /* coercion can lose the object bit */
    PROTECT(sa = coerceVector(sa, REALSXP));

    a = REAL(sa);
    naflag = 0;

    if (variant == VARIANT_SUM) {
        long double s = 0.0;
        double t;
        for (i = 0; i < n; i++) {
	    if (ISNAN(a[i]))
	        s  += a[i];
	    else {
	        t = f(a[i]);
	        if (ISNAN(t)) naflag = 1;
                s += t;
	    }
        }
        sy = allocVector(REALSXP, 1);
        REAL(sy)[0] = (double) s;
        SET_ATTRIB (sy, R_VariantResult);
    }
    else { /* non-variant return */
        double *y;
        sy = NAMED(sa)==0 ? sa : allocVector(REALSXP, n);
        PROTECT(sy);
        y = REAL(sy);

#ifdef R_MEMORY_PROFILING
        if (RTRACE(sa)){
           memtrace_report(sa, sy);
           SET_RTRACE(sy, 1);
        }
#endif
        for (i = 0; i < n; i++) {
	    if (ISNAN(a[i]))
	        y[i] = a[i];
	    else {
	        y[i] = f(a[i]);
	        if (ISNAN(y[i])) naflag = 1;
	    }
        }

        if (sa!=sy) 
            DUPLICATE_ATTRIB(sy, sa);
        UNPROTECT(1);
    }

    if (naflag)
	warningcall(lcall, R_MSG_NA);

    UNPROTECT(1);

    return sy;
}


static SEXP do_fast_math1(SEXP call, SEXP op, SEXP arg, SEXP env, int variant)
{
    if (isComplex(arg)) {
        /* for the moment, keep the interface to complex_math1 the same */
        SEXP tmp;
        PROTECT(tmp = CONS(arg,R_NilValue));
	tmp = complex_math1(call, op, tmp, env);
        UNPROTECT(1);
        return tmp;
    }

#define MATH1(x) math1(arg, x, call, variant);
    switch (PRIMVAL(op)) {
    case 1: return MATH1(floor);
    case 2: return MATH1(ceil);
    case 3: return MATH1(sqrt);
    case 4: return MATH1(sign);
	/* case 5: return MATH1(trunc); separate from 2.6.0 */

    case 10: return MATH1(exp);
    case 11: return MATH1(expm1);
    case 12: return MATH1(log1p);
    case 20: return MATH1(cos);
    case 21: return MATH1(sin);
    case 22: return MATH1(tan);
    case 23: return MATH1(acos);
    case 24: return MATH1(asin);
    case 25: return MATH1(atan);

    case 30: return MATH1(cosh);
    case 31: return MATH1(sinh);
    case 32: return MATH1(tanh);
    case 33: return MATH1(acosh);
    case 34: return MATH1(asinh);
    case 35: return MATH1(atanh);

    case 40: return MATH1(lgammafn);
    case 41: return MATH1(gammafn);

    case 42: return MATH1(digamma);
    case 43: return MATH1(trigamma);
	/* case 44: return MATH1(tetragamma);
	   case 45: return MATH1(pentagamma);
	   removed in 2.0.0
	*/

	/* case 46: return MATH1(Rf_gamma_cody); removed in 2.8.0 */

    default:
        /* log put here in case the compiler handles a sparse switch poorly */
        if (PRIMVAL(op) == 10003) 
            return MATH1(R_log);

	errorcall(call, _("unimplemented real function of 1 argument"));
    }
    return R_NilValue; /* never used; to keep -Wall happy */
}


SEXP attribute_hidden do_math1(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP s;

    checkArity(op, args);
    check1arg_x (args, call);

    if (DispatchGroup("Math", call, op, args, env, &s))
	return s;

    if (PRIMFUN_FAST(op)==0)
        SET_PRIMFUN_FAST_UNARY (op, do_fast_math1, 1, 0);

    return do_fast_math1 (call, op, CAR(args), env, 0);
}

/* Methods for trunc are allowed to have more than one arg */

static SEXP do_fast_trunc (SEXP call, SEXP op, SEXP arg, SEXP env, int variant)
{
    return math1(arg, trunc, call, variant);
}

SEXP attribute_hidden do_trunc(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP s;
    if (DispatchGroup("Math", call, op, args, env, &s))
	return s;

    check1arg_x (args, call);
    if (isComplex(CAR(args)))
	errorcall(call, _("unimplemented complex function"));

    if (PRIMFUN_FAST(op)==0)
        SET_PRIMFUN_FAST_UNARY (op, do_fast_trunc, 1, 0);

    return math1(CAR(args), trunc, call, 0);
}

/* Note that abs is slightly different from the do_math1 set, both
   for integer/logical inputs and what it dispatches to for complex ones. */

static SEXP do_fast_abs (SEXP call, SEXP op, SEXP x, SEXP env, int variant)
{   
    SEXP s;

    if (isInteger(x) || isLogical(x)) {
	/* integer or logical ==> return integer,
	   factor was covered by Math.factor. */
        int n = LENGTH(x);
	s = NAMED(x)==0 && TYPEOF(x)==INTSXP ? x : allocVector(INTSXP, n);
	/* Note: relying on INTEGER(.) === LOGICAL(.) : */
	for (int i = 0 ; i < n ; i++) {
            int v = INTEGER(x)[i];
	    INTEGER(s)[i] = v==NA_INTEGER ? NA_INTEGER : v<0 ? -v : v;
        }
    } else if (TYPEOF(x) == REALSXP) {
	int n = LENGTH(x);
        if (variant == VARIANT_SUM) {
            long double r = 0.0;
	    for (int i = 0 ; i < n ; i++)
                r += fabs(REAL(x)[i]);
            s = allocVector (REALSXP, 1);
            REAL(s)[0] = (double) r;
            SET_ATTRIB (s, R_VariantResult);
            return s;
        }
        s = NAMED(x)==0 ? x : allocVector(REALSXP, n);
	for (int i = 0 ; i < n ; i++)
	    REAL(s)[i] = fabs(REAL(x)[i]);
    } else if (isComplex(x)) {
        SEXP args;
        PROTECT (args = CONS(x,R_NilValue));
	s = do_cmathfuns(call, op, args, env);
        UNPROTECT(1);
    } else
	errorcall(call, R_MSG_NONNUM_MATH);

    if (x!=s) {
        PROTECT(s);
        DUPLICATE_ATTRIB(s, x);
        UNPROTECT(1);
    }

    return s;
}

SEXP attribute_hidden do_abs(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP s;

    checkArity(op, args);
    check1arg_x (args, call);

    if (DispatchGroup("Math", call, op, args, env, &s))
	return s;

    if (PRIMFUN_FAST(op)==0)
        SET_PRIMFUN_FAST_UNARY (op, do_fast_abs, 1, 0);

    return do_fast_abs (call, op, CAR(args), env, 0);
}

/* Mathematical Functions of Two Numeric Arguments (plus 1 int) */

#define if_NA_Math2_set(y,a,b)				\
	if      (ISNA (a) || ISNA (b)) y = NA_REAL;	\
	else if (ISNAN(a) || ISNAN(b)) y = R_NaN;

static SEXP math2(SEXP sa, SEXP sb, double (*f)(double, double),
		  SEXP lcall)
{
    SEXP sy;
    int i, ia, ib, n, na, nb;
    double ai, bi, *a, *b, *y;
    int naflag;

    if (!isNumeric(sa) || !isNumeric(sb))
	errorcall(lcall, R_MSG_NONNUM_MATH);

    /* for 0-length a we want the attributes of a, not those of b
       as no recycling will occur */
#define SETUP_Math2				\
    na = LENGTH(sa);				\
    nb = LENGTH(sb);				\
    if ((na == 0) || (nb == 0))	{		\
	PROTECT(sy = allocVector(REALSXP, 0));	\
	if (na == 0) DUPLICATE_ATTRIB(sy, sa);	\
	UNPROTECT(1);				\
	return(sy);				\
    }						\
    n = (na < nb) ? nb : na;			\
    PROTECT(sa = coerceVector(sa, REALSXP));	\
    PROTECT(sb = coerceVector(sb, REALSXP));	\
    PROTECT(sy = allocVector(REALSXP, n));	\
    a = REAL(sa);				\
    b = REAL(sb);				\
    y = REAL(sy);				\
    naflag = 0

    SETUP_Math2;

#ifdef R_MEMORY_PROFILING
    if (RTRACE(sa) || RTRACE(sb)) {
       if (RTRACE(sa) && RTRACE(sb)){
	  if (na > nb)
	      memtrace_report(sa, sy);
	  else
	      memtrace_report(sb, sy);
       } else if (RTRACE(sa))
	   memtrace_report(sa, sy);
       else /* only s2 */
	   memtrace_report(sb, sy);
       SET_RTRACE(sy, 1);
    }
#endif

    mod_iterate(na, nb, ia, ib) {
	ai = a[ia];
	bi = b[ib];
	if_NA_Math2_set(y[i], ai, bi)
	else {
	    y[i] = f(ai, bi);
	    if (ISNAN(y[i])) naflag = 1;
	}
    }

#define FINISH_Math2				\
    if(naflag)					\
	warningcall(lcall, R_MSG_NA);		\
    if (n == na)  DUPLICATE_ATTRIB(sy, sa);	\
    else if (n == nb) DUPLICATE_ATTRIB(sy, sb);	\
    UNPROTECT(3)

    FINISH_Math2;

    return sy;
} /* math2() */

static SEXP math2_1(SEXP sa, SEXP sb, SEXP sI,
		    double (*f)(double, double, int), SEXP lcall)
{
    SEXP sy;
    int i, ia, ib, n, na, nb;
    double ai, bi, *a, *b, *y;
    int m_opt;
    int naflag;

    if (!isNumeric(sa) || !isNumeric(sb))
	errorcall(lcall, R_MSG_NONNUM_MATH);

    SETUP_Math2;
    m_opt = asInteger(sI);

#ifdef R_MEMORY_PROFILING
    if (RTRACE(sa) || RTRACE(sb)) {
       if (RTRACE(sa) && RTRACE(sb)) {
	  if (na > nb)
	      memtrace_report(sa, sy);
	  else
	      memtrace_report(sb, sy);
       } else if (RTRACE(sa))
	   memtrace_report(sa, sy);
       else /* only s2 */
	   memtrace_report(sb, sy);
       SET_RTRACE(sy, 1);
    }
#endif

    mod_iterate(na, nb, ia, ib) {
	ai = a[ia];
	bi = b[ib];
	if_NA_Math2_set(y[i], ai, bi)
	else {
	    y[i] = f(ai, bi, m_opt);
	    if (ISNAN(y[i])) naflag = 1;
	}
    }
    FINISH_Math2;
    return sy;
} /* math2_1() */

static SEXP math2_2(SEXP sa, SEXP sb, SEXP sI1, SEXP sI2,
		    double (*f)(double, double, int, int), SEXP lcall)
{
    SEXP sy;
    int i, ia, ib, n, na, nb;
    double ai, bi, *a, *b, *y;
    int i_1, i_2;
    int naflag;
    if (!isNumeric(sa) || !isNumeric(sb))
	errorcall(lcall, R_MSG_NONNUM_MATH);

    SETUP_Math2;
    i_1 = asInteger(sI1);
    i_2 = asInteger(sI2);

#ifdef R_MEMORY_PROFILING
    if (RTRACE(sa) || RTRACE(sb)) {
       if (RTRACE(sa) && RTRACE(sb)) {
	  if (na > nb)
	      memtrace_report(sa, sy);
	  else
	      memtrace_report(sb, sy);
       } else if (RTRACE(sa))
	   memtrace_report(sa, sy);
       else /* only s2 */
	   memtrace_report(sb, sy);
       SET_RTRACE(sy, 1);
    }
#endif

    mod_iterate(na, nb, ia, ib) {
	ai = a[ia];
	bi = b[ib];
	if_NA_Math2_set(y[i], ai, bi)
	else {
	    y[i] = f(ai, bi, i_1, i_2);
	    if (ISNAN(y[i])) naflag = 1;
	}
    }
    FINISH_Math2;
    return sy;
} /* math2_2() */

static SEXP math2B(SEXP sa, SEXP sb, double (*f)(double, double, double *),
		   SEXP lcall)
{
    SEXP sy;
    int i, ia, ib, n, na, nb;
    double ai, bi, *a, *b, *y;
    int naflag;
    double amax, *work;
    long nw;

    if (!isNumeric(sa) || !isNumeric(sb))
	errorcall(lcall, R_MSG_NONNUM_MATH);

    /* for 0-length a we want the attributes of a, not those of b
       as no recycling will occur */
    SETUP_Math2;

#ifdef R_MEMORY_PROFILING
    if (RTRACE(sa) || RTRACE(sb)) {
       if (RTRACE(sa) && RTRACE(sb)) {
	  if (na > nb)
	      memtrace_report(sa, sy);
	  else
	      memtrace_report(sb, sy);
       } else if (RTRACE(sa))
	   memtrace_report(sa, sy);
       else /* only s2 */
	   memtrace_report(sb, sy);
       SET_RTRACE(sy, 1);
    }
#endif

    /* allocate work array for BesselJ, BesselY large enough for all
       arguments */
    amax = 0.0;
    for (i = 0; i < nb; i++) {
	double av = b[i] < 0 ? -b[i] : b[i];
	if (av > amax) amax = av;
    }
    nw = 1 + (long)floor(amax);
    work = (double *) R_alloc((size_t) nw, sizeof(double));

    mod_iterate(na, nb, ia, ib) {
	ai = a[ia];
	bi = b[ib];
	if_NA_Math2_set(y[i], ai, bi)
	else {
	    y[i] = f(ai, bi, work);
	    if (ISNAN(y[i])) naflag = 1;
	}
    }


    FINISH_Math2;

    return sy;
} /* math2B() */

#define Math2(A, FUN)	  math2(CAR(A), CADR(A), FUN, call);
#define Math2_1(A, FUN)	math2_1(CAR(A), CADR(A), CADDR(A), FUN, call);
#define Math2_2(A, FUN) math2_2(CAR(A), CADR(A), CADDR(A), CADDDR(A), FUN, call)
#define Math2B(A, FUN)	  math2B(CAR(A), CADR(A), FUN, call);

SEXP attribute_hidden do_math2(SEXP call, SEXP op, SEXP args, SEXP env)
{
    checkArity(op, args);

    if (isComplex(CAR(args)) ||
	(PRIMVAL(op) == 0 && isComplex(CADR(args))))
	return complex_math2(call, op, args, env);


    switch (PRIMVAL(op)) {

    case  0: return Math2(args, atan2);
    case 10001: return Math2(args, fround);/* round(), src/nmath/fround.c */
    case 10004: return Math2(args, fprec); /* signif(), src/nmath/fprec.c */

    case  2: return Math2(args, lbeta);
    case  3: return Math2(args, beta);
    case  4: return Math2(args, lchoose);
    case  5: return Math2(args, choose);

    case  6: return Math2_1(args, dchisq);
    case  7: return Math2_2(args, pchisq);
    case  8: return Math2_2(args, qchisq);

    case  9: return Math2_1(args, dexp);
    case 10: return Math2_2(args, pexp);
    case 11: return Math2_2(args, qexp);

    case 12: return Math2_1(args, dgeom);
    case 13: return Math2_2(args, pgeom);
    case 14: return Math2_2(args, qgeom);

    case 15: return Math2_1(args, dpois);
    case 16: return Math2_2(args, ppois);
    case 17: return Math2_2(args, qpois);

    case 18: return Math2_1(args, dt);
    case 19: return Math2_2(args, pt);
    case 20: return Math2_2(args, qt);

    case 21: return Math2_1(args, dsignrank);
    case 22: return Math2_2(args, psignrank);
    case 23: return Math2_2(args, qsignrank);

    case 24: return Math2B(args, bessel_j_ex);
    case 25: return Math2B(args, bessel_y_ex);
    case 26: return Math2(args, psigamma);

    default:
	errorcall(call,
		  _("unimplemented real function of %d numeric arguments"), 2);
    }
    return op;			/* never used; to keep -Wall happy */
}


/* The S4 Math2 group, round and signif */
/* This is a primitive SPECIALSXP with internal argument matching */
SEXP attribute_hidden do_Math2(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP res, call2;
    int n, nprotect = 2;

    if (length(args) >= 2 &&
	isSymbol(CADR(args)) && R_isMissing(CADR(args), env)) {
	double digits = 0;
	if(PRIMVAL(op) == 10004) digits = 6.0;
	PROTECT(args = list2(CAR(args), ScalarReal(digits))); nprotect++;
    }

    PROTECT(args = evalListKeepMissing(args, env));
    PROTECT(call2 = lang2(CAR(call), R_NilValue));
    SETCDR(call2, args);

    n = length(args);

    if (n != 1 && n != 2)
	error(_("%d arguments passed to '%s' which requires 1 or 2"),
	      n, PRIMNAME(op));

    if (! DispatchGroup("Math", call2, op, args, env, &res)) {
	if(n == 1) {
	    double digits = 0.0;
	    if(PRIMVAL(op) == 10004) digits = 6.0;
	    SETCDR(args, CONS(ScalarReal(digits), R_NilValue));
	} else {
	    /* If named, do argument matching by name */
	    if (TAG(args) != R_NilValue || TAG(CDR(args)) != R_NilValue) {
                static char *ap[2] = { "x", "digits" };
		PROTECT(args = matchArgs(R_NilValue, ap, 2, args, call));
		nprotect += 1;
	    }
	    if (length(CADR(args)) == 0)
		errorcall(call, _("invalid second argument of length 0"));
	}
	res = do_math2(call, op, args, env);
    }
    UNPROTECT(nprotect);
    return res;
}

/* log{2,10} are builtins */
SEXP attribute_hidden do_log1arg(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP res, call2, args2, tmp = R_NilValue /* -Wall */;

    checkArity(op, args);
    check1arg_x (args, call);

    if (DispatchGroup("Math", call, op, args, env, &res)) return res;

    if(PRIMVAL(op) == 10) tmp = ScalarReal(10.0);
    if(PRIMVAL(op) == 2)  tmp = ScalarReal(2.0);

    PROTECT(call2 = lang3(install("log"), CAR(args), tmp));
    PROTECT(args2 = lang2(CAR(args), tmp));
    if (! DispatchGroup("Math", call2, op, args2, env, &res)) {
	if (isComplex(CAR(args)))
	    res = complex_math2(call2, op, args2, env);
	else
	    res = math2(CAR(args), tmp, logbase, call);
    }
    UNPROTECT(2);
    return res;
}


/* This is a primitive SPECIALSXP with internal argument matching */
SEXP attribute_hidden do_log (SEXP call, SEXP op, SEXP args, SEXP env,
                              int variant)
{
    int nprotect = 2;
    int n;

    /* Do the common case of one un-tagged, non-object, argument quickly. */
    if (!isNull(args) && isNull(CDR(args)) && isNull(TAG(args)) 
          && CAR(args) != R_DotsSymbol && CAR(args) != R_MissingArg) {
        SEXP arg, ans;
        PROTECT(arg = eval (CAR(args), env));
        if (isObject(arg)) {
            UNPROTECT(1);
            PROTECT(args = CONS(arg, R_NilValue));
            n = 1;
        }
        else {
            ans = do_fast_math1 (call, op, arg, env, variant);
            UNPROTECT(1);
            return ans;
        }
    }
    else {
        n = length(args);

        /* This seems like some sort of horrible kludge that can't possibly
           be right in general (it ignores the argument names, and silently
           discards arguments after the first two). */
        if (n >= 2 && isSymbol(CADR(args)) && R_isMissing(CADR(args), env)) {
#ifdef M_E
	    double e = M_E;
#else
	    double e = exp(1.);
#endif
	    PROTECT(args = list2(CAR(args), ScalarReal(e))); nprotect++;
        }
        PROTECT(args = evalListKeepMissing(args, env));
    }

    SEXP res, call2;
    PROTECT(call2 = lang2(CAR(call), R_NilValue));
    SETCDR(call2, args);
    n = length(args);

    if (! DispatchGroup("Math", call2, op, args, env, &res)) {
	switch (n) {
	case 1:
            check1arg_x (args, call);
	    if (isComplex(CAR(args)))
		res = complex_math1(call, op, args, env);
	    else
		res = math1(CAR(args), R_log, call, variant);
	    break;
	case 2:
	{
	    /* match argument names if supplied */
            static char *ap[2] = { "x", "base" };
	    PROTECT(args = matchArgs(R_NilValue, ap, 2, args, call));
	    nprotect += 1;
	    if (length(CADR(args)) == 0)
		errorcall(call, _("invalid argument 'base' of length 0"));
	    if (isComplex(CAR(args)) || isComplex(CADR(args)))
		res = complex_math2(call, op, args, env);
	    else
		res = math2(CAR(args), CADR(args), logbase, call);
	    break;
	}
	default:
	    error(_("%d arguments passed to 'log' which requires 1 or 2"), n);
	}
    }
    UNPROTECT(nprotect);
    return res;
}


/* Mathematical Functions of Three (Real) Arguments */

#define if_NA_Math3_set(y,a,b,c)			        \
	if      (ISNA (a) || ISNA (b)|| ISNA (c)) y = NA_REAL;	\
	else if (ISNAN(a) || ISNAN(b)|| ISNAN(c)) y = R_NaN;

#define mod_iterate3(n1,n2,n3,i1,i2,i3) for (i=i1=i2=i3=0; i<n; \
	i1 = (++i1==n1) ? 0 : i1,				\
	i2 = (++i2==n2) ? 0 : i2,				\
	i3 = (++i3==n3) ? 0 : i3,				\
	++i)

#define SETUP_Math3						\
    if (!isNumeric(sa) || !isNumeric(sb) || !isNumeric(sc))	\
	errorcall(lcall, R_MSG_NONNUM_MATH);			\
								\
    na = LENGTH(sa);						\
    nb = LENGTH(sb);						\
    nc = LENGTH(sc);						\
    if ((na == 0) || (nb == 0) || (nc == 0))			\
	return(allocVector(REALSXP, 0));			\
    n = na;							\
    if (n < nb) n = nb;						\
    if (n < nc) n = nc;						\
    PROTECT(sa = coerceVector(sa, REALSXP));			\
    PROTECT(sb = coerceVector(sb, REALSXP));			\
    PROTECT(sc = coerceVector(sc, REALSXP));			\
    PROTECT(sy = allocVector(REALSXP, n));			\
    a = REAL(sa);						\
    b = REAL(sb);						\
    c = REAL(sc);						\
    y = REAL(sy);						\
    naflag = 0

#define FINISH_Math3				\
    if(naflag)					\
	warningcall(lcall, R_MSG_NA);		\
						\
    if (n == na) DUPLICATE_ATTRIB(sy, sa);	\
    else if (n == nb) DUPLICATE_ATTRIB(sy, sb);	\
    else if (n == nc) DUPLICATE_ATTRIB(sy, sc);	\
    UNPROTECT(4)

static SEXP math3_1(SEXP sa, SEXP sb, SEXP sc, SEXP sI,
		    double (*f)(double, double, double, int), SEXP lcall)
{
    SEXP sy;
    int i, ia, ib, ic, n, na, nb, nc;
    double ai, bi, ci, *a, *b, *c, *y;
    int i_1;
    int naflag;

    SETUP_Math3;
    i_1 = asInteger(sI);

#ifdef R_MEMORY_PROFILING
    if (RTRACE(sa) || RTRACE(sb) || RTRACE(sc)) {
       if (RTRACE(sa))
	  memtrace_report(sa, sy);
       else if (RTRACE(sb))
	  memtrace_report(sb, sy);
       else if (RTRACE(sc))
	  memtrace_report(sc, sy);
       SET_RTRACE(sy, 1);
    }
#endif

    mod_iterate3 (na, nb, nc, ia, ib, ic) {
	ai = a[ia];
	bi = b[ib];
	ci = c[ic];
	if_NA_Math3_set(y[i], ai,bi,ci)
	else {
	    y[i] = f(ai, bi, ci, i_1);
	    if (ISNAN(y[i])) naflag = 1;
	}
    }

    FINISH_Math3;
    return sy;
} /* math3_1 */

static SEXP math3_2(SEXP sa, SEXP sb, SEXP sc, SEXP sI, SEXP sJ,
		    double (*f)(double, double, double, int, int), SEXP lcall)
{
    SEXP sy;
    int i, ia, ib, ic, n, na, nb, nc;
    double ai, bi, ci, *a, *b, *c, *y;
    int i_1,i_2;
    int naflag;

    SETUP_Math3;
    i_1 = asInteger(sI);
    i_2 = asInteger(sJ);

#ifdef R_MEMORY_PROFILING
    if (RTRACE(sa) || RTRACE(sb) || RTRACE(sc)) {
       if (RTRACE(sa))
	  memtrace_report(sa, sy);
       else if (RTRACE(sb))
	  memtrace_report(sb, sy);
       else if (RTRACE(sc))
	  memtrace_report(sc, sy);
       SET_RTRACE(sy, 1);
    }
#endif


    mod_iterate3 (na, nb, nc, ia, ib, ic) {
	ai = a[ia];
	bi = b[ib];
	ci = c[ic];
	if_NA_Math3_set(y[i], ai,bi,ci)
	else {
	    y[i] = f(ai, bi, ci, i_1, i_2);
	    if (ISNAN(y[i])) naflag = 1;
	}
    }

    FINISH_Math3;
    return sy;
} /* math3_2 */

static SEXP math3B(SEXP sa, SEXP sb, SEXP sc,
		   double (*f)(double, double, double, double *), SEXP lcall)
{
    SEXP sy;
    int i, ia, ib, ic, n, na, nb, nc;
    double ai, bi, ci, *a, *b, *c, *y;
    int naflag;
    double amax, *work;
    long nw;

    SETUP_Math3;

#ifdef R_MEMORY_PROFILING
    if (RTRACE(sa) || RTRACE(sb) || RTRACE(sc)) {
       if (RTRACE(sa))
	  memtrace_report(sa, sy);
       else if (RTRACE(sb))
	  memtrace_report(sb, sy);
       else if (RTRACE(sc))
	  memtrace_report(sc, sy);
       SET_RTRACE(sy, 1);
    }
#endif

    /* allocate work array for BesselI, BesselK large enough for all
       arguments */
    amax = 0.0;
    for (i = 0; i < nb; i++) {
	double av = b[i] < 0 ? -b[i] : b[i];
	if (av > amax) amax = av;
    }
    nw = 1 + (long)floor(amax);
    work = (double *) R_alloc((size_t) nw, sizeof(double));

    mod_iterate3 (na, nb, nc, ia, ib, ic) {
	ai = a[ia];
	bi = b[ib];
	ci = c[ic];
	if_NA_Math3_set(y[i], ai,bi,ci)
	else {
	    y[i] = f(ai, bi, ci, work);
	    if (ISNAN(y[i])) naflag = 1;
	}
    }

    FINISH_Math3;

    return sy;
} /* math3B */

#define Math3_1(A, FUN)	math3_1(CAR(A), CADR(A), CADDR(A), CADDDR(A), FUN, call);
#define Math3_2(A, FUN) math3_2(CAR(A), CADR(A), CADDR(A), CADDDR(A), CAD4R(A), FUN, call)
#define Math3B(A, FUN)  math3B (CAR(A), CADR(A), CADDR(A), FUN, call);

SEXP attribute_hidden do_math3(SEXP call, SEXP op, SEXP args, SEXP env)
{
    checkArity(op, args);

    switch (PRIMVAL(op)) {

    case  1:  return Math3_1(args, dbeta);
    case  2:  return Math3_2(args, pbeta);
    case  3:  return Math3_2(args, qbeta);

    case  4:  return Math3_1(args, dbinom);
    case  5:  return Math3_2(args, pbinom);
    case  6:  return Math3_2(args, qbinom);

    case  7:  return Math3_1(args, dcauchy);
    case  8:  return Math3_2(args, pcauchy);
    case  9:  return Math3_2(args, qcauchy);

    case 10:  return Math3_1(args, df);
    case 11:  return Math3_2(args, pf);
    case 12:  return Math3_2(args, qf);

    case 13:  return Math3_1(args, dgamma);
    case 14:  return Math3_2(args, pgamma);
    case 15:  return Math3_2(args, qgamma);

    case 16:  return Math3_1(args, dlnorm);
    case 17:  return Math3_2(args, plnorm);
    case 18:  return Math3_2(args, qlnorm);

    case 19:  return Math3_1(args, dlogis);
    case 20:  return Math3_2(args, plogis);
    case 21:  return Math3_2(args, qlogis);

    case 22:  return Math3_1(args, dnbinom);
    case 23:  return Math3_2(args, pnbinom);
    case 24:  return Math3_2(args, qnbinom);

    case 25:  return Math3_1(args, dnorm);
    case 26:  return Math3_2(args, pnorm);
    case 27:  return Math3_2(args, qnorm);

    case 28:  return Math3_1(args, dunif);
    case 29:  return Math3_2(args, punif);
    case 30:  return Math3_2(args, qunif);

    case 31:  return Math3_1(args, dweibull);
    case 32:  return Math3_2(args, pweibull);
    case 33:  return Math3_2(args, qweibull);

    case 34:  return Math3_1(args, dnchisq);
    case 35:  return Math3_2(args, pnchisq);
    case 36:  return Math3_2(args, qnchisq);

    case 37:  return Math3_1(args, dnt);
    case 38:  return Math3_2(args, pnt);
    case 39:  return Math3_2(args, qnt);

    case 40:  return Math3_1(args, dwilcox);
    case 41:  return Math3_2(args, pwilcox);
    case 42:  return Math3_2(args, qwilcox);

    case 43:  return Math3B(args, bessel_i_ex);
    case 44:  return Math3B(args, bessel_k_ex);

    case 45:  return Math3_1(args, dnbinom_mu);
    case 46:  return Math3_2(args, pnbinom_mu);
    case 47:  return Math3_2(args, qnbinom_mu);

    default:
	errorcall(call,
		  _("unimplemented real function of %d numeric arguments"), 3);
    }
    return op;			/* never used; to keep -Wall happy */
} /* do_math3() */

/* Mathematical Functions of Four (Real) Arguments */

#define if_NA_Math4_set(y,a,b,c,d)				\
	if      (ISNA (a)|| ISNA (b)|| ISNA (c)|| ISNA (d)) y = NA_REAL;\
	else if (ISNAN(a)|| ISNAN(b)|| ISNAN(c)|| ISNAN(d)) y = R_NaN;

#define mod_iterate4(n1,n2,n3,n4,i1,i2,i3,i4) for (i=i1=i2=i3=i4=0; i<n; \
	i1 = (++i1==n1) ? 0 : i1,					\
	i2 = (++i2==n2) ? 0 : i2,					\
	i3 = (++i3==n3) ? 0 : i3,					\
	i4 = (++i4==n4) ? 0 : i4,					\
	++i)

static SEXP math4(SEXP sa, SEXP sb, SEXP sc, SEXP sd,
		  double (*f)(double, double, double, double), SEXP lcall)
{
    SEXP sy;
    int i, ia, ib, ic, id, n, na, nb, nc, nd;
    double ai, bi, ci, di, *a, *b, *c, *d, *y;
    int naflag;

#define SETUP_Math4							\
    if(!isNumeric(sa)|| !isNumeric(sb)|| !isNumeric(sc)|| !isNumeric(sd))\
	errorcall(lcall, R_MSG_NONNUM_MATH);				\
									\
    na = LENGTH(sa);							\
    nb = LENGTH(sb);							\
    nc = LENGTH(sc);							\
    nd = LENGTH(sd);							\
    if ((na == 0) || (nb == 0) || (nc == 0) || (nd == 0))		\
	return(allocVector(REALSXP, 0));				\
    n = na;								\
    if (n < nb) n = nb;							\
    if (n < nc) n = nc;							\
    if (n < nd) n = nd;							\
    PROTECT(sa = coerceVector(sa, REALSXP));				\
    PROTECT(sb = coerceVector(sb, REALSXP));				\
    PROTECT(sc = coerceVector(sc, REALSXP));				\
    PROTECT(sd = coerceVector(sd, REALSXP));				\
    PROTECT(sy = allocVector(REALSXP, n));				\
    a = REAL(sa);							\
    b = REAL(sb);							\
    c = REAL(sc);							\
    d = REAL(sd);							\
    y = REAL(sy);							\
    naflag = 0

    SETUP_Math4;

    mod_iterate4 (na, nb, nc, nd, ia, ib, ic, id) {
	ai = a[ia];
	bi = b[ib];
	ci = c[ic];
	di = d[id];
	if_NA_Math4_set(y[i], ai,bi,ci,di)
	else {
	    y[i] = f(ai, bi, ci, di);
	    if (ISNAN(y[i])) naflag = 1;
	}
    }

#define FINISH_Math4				\
    if(naflag)					\
	warningcall(lcall, R_MSG_NA);		\
						\
    if (n == na) DUPLICATE_ATTRIB(sy, sa);	\
    else if (n == nb) DUPLICATE_ATTRIB(sy, sb);	\
    else if (n == nc) DUPLICATE_ATTRIB(sy, sc);	\
    else if (n == nd) DUPLICATE_ATTRIB(sy, sd);	\
    UNPROTECT(5)

    FINISH_Math4;

    return sy;
} /* math4() */

static SEXP math4_1(SEXP sa, SEXP sb, SEXP sc, SEXP sd, SEXP sI, double (*f)(double, double, double, double, int), SEXP lcall)
{
    SEXP sy;
    int i, ia, ib, ic, id, n, na, nb, nc, nd;
    double ai, bi, ci, di, *a, *b, *c, *d, *y;
    int i_1;
    int naflag;

    SETUP_Math4;
    i_1 = asInteger(sI);

    mod_iterate4 (na, nb, nc, nd, ia, ib, ic, id) {
	ai = a[ia];
	bi = b[ib];
	ci = c[ic];
	di = d[id];
	if_NA_Math4_set(y[i], ai,bi,ci,di)
	else {
	    y[i] = f(ai, bi, ci, di, i_1);
	    if (ISNAN(y[i])) naflag = 1;
	}
    }
    FINISH_Math4;
    return sy;
} /* math4_1() */

static SEXP math4_2(SEXP sa, SEXP sb, SEXP sc, SEXP sd, SEXP sI, SEXP sJ,
		    double (*f)(double, double, double, double, int, int), SEXP lcall)
{
    SEXP sy;
    int i, ia, ib, ic, id, n, na, nb, nc, nd;
    double ai, bi, ci, di, *a, *b, *c, *d, *y;
    int i_1, i_2;
    int naflag;

    SETUP_Math4;
    i_1 = asInteger(sI);
    i_2 = asInteger(sJ);

    mod_iterate4 (na, nb, nc, nd, ia, ib, ic, id) {
	ai = a[ia];
	bi = b[ib];
	ci = c[ic];
	di = d[id];
	if_NA_Math4_set(y[i], ai,bi,ci,di)
	else {
	    y[i] = f(ai, bi, ci, di, i_1, i_2);
	    if (ISNAN(y[i])) naflag = 1;
	}
    }
    FINISH_Math4;
    return sy;
} /* math4_2() */


#define CAD3R	CADDDR
/* This is not (yet) in Rinternals.h : */
#define CAD5R(e)	CAR(CDR(CDR(CDR(CDR(CDR(e))))))

#define Math4(A, FUN)   math4  (CAR(A), CADR(A), CADDR(A), CAD3R(A), FUN, call)
#define Math4_1(A, FUN) math4_1(CAR(A), CADR(A), CADDR(A), CAD3R(A), CAD4R(A), \
				FUN, call)
#define Math4_2(A, FUN) math4_2(CAR(A), CADR(A), CADDR(A), CAD3R(A), CAD4R(A), \
				CAD5R(A), FUN, call)


SEXP attribute_hidden do_math4(SEXP call, SEXP op, SEXP args, SEXP env)
{
    checkArity(op, args);


    switch (PRIMVAL(op)) {

	/* Completely dummy for -Wall -- math4() at all! : */
    case -99: return Math4(args, (double (*)(double, double, double, double))NULL);

    case  1: return Math4_1(args, dhyper);
    case  2: return Math4_2(args, phyper);
    case  3: return Math4_2(args, qhyper);

    case  4: return Math4_1(args, dnbeta);
    case  5: return Math4_2(args, pnbeta);
    case  6: return Math4_2(args, qnbeta);
    case  7: return Math4_1(args, dnf);
    case  8: return Math4_2(args, pnf);
    case  9: return Math4_2(args, qnf);
#ifdef UNIMP
    case 10: return Math4_1(args, dtukey);
#endif
    case 11: return Math4_2(args, ptukey);
    case 12: return Math4_2(args, qtukey);
    default:
	errorcall(call,
		  _("unimplemented real function of %d numeric arguments"), 4);
    }
    return op;			/* never used; to keep -Wall happy */
}


#ifdef WHEN_MATH5_IS_THERE/* as in ./arithmetic.h */

/* Mathematical Functions of Five (Real) Arguments */

#define if_NA_Math5_set(y,a,b,c,d,e)					\
	if     (ISNA (a)|| ISNA (b)|| ISNA (c)|| ISNA (d)|| ISNA (e))	\
		y = NA_REAL;						\
	else if(ISNAN(a)|| ISNAN(b)|| ISNAN(c)|| ISNAN(d)|| ISNAN(e))	\
		y = R_NaN;

#define mod_iterate5(n1,n2,n3,n4,n5, i1,i2,i3,i4,i5)	\
 for (i=i1=i2=i3=i4=i5=0; i<n;				\
	i1 = (++i1==n1) ? 0 : i1,			\
	i2 = (++i2==n2) ? 0 : i2,			\
	i3 = (++i3==n3) ? 0 : i3,			\
	i4 = (++i4==n4) ? 0 : i4,			\
	i5 = (++i5==n5) ? 0 : i5,			\
	++i)

static SEXP math5(SEXP sa, SEXP sb, SEXP sc, SEXP sd, SEXP se, double (*f)())
{
    SEXP sy;
    int i, ia, ib, ic, id, ie, n, na, nb, nc, nd, ne;
    double ai, bi, ci, di, ei, *a, *b, *c, *d, *e, *y;

#define SETUP_Math5							\
    if (!isNumeric(sa) || !isNumeric(sb) || !isNumeric(sc) ||		\
	!isNumeric(sd) || !isNumeric(se))				\
	errorcall(lcall, R_MSG_NONNUM_MATH);				\
									\
    na = LENGTH(sa);							\
    nb = LENGTH(sb);							\
    nc = LENGTH(sc);							\
    nd = LENGTH(sd);							\
    ne = LENGTH(se);							\
    if ((na == 0) || (nb == 0) || (nc == 0) || (nd == 0) || (ne == 0))	\
	return(allocVector(REALSXP, 0));				\
    n = na;								\
    if (n < nb) n = nb;							\
    if (n < nc) n = nc;							\
    if (n < nd) n = nd;							\
    if (n < ne) n = ne;		/* n = max(na,nb,nc,nd,ne) */		\
    PROTECT(sa = coerceVector(sa, REALSXP));				\
    PROTECT(sb = coerceVector(sb, REALSXP));				\
    PROTECT(sc = coerceVector(sc, REALSXP));				\
    PROTECT(sd = coerceVector(sd, REALSXP));				\
    PROTECT(se = coerceVector(se, REALSXP));				\
    PROTECT(sy = allocVector(REALSXP, n));				\
    a = REAL(sa);							\
    b = REAL(sb);							\
    c = REAL(sc);							\
    d = REAL(sd);							\
    e = REAL(se);							\
    y = REAL(sy);							\
    naflag = 0

    SETUP_Math5;

    mod_iterate5 (na, nb, nc, nd, ne,
		  ia, ib, ic, id, ie) {
	ai = a[ia];
	bi = b[ib];
	ci = c[ic];
	di = d[id];
	ei = e[ie];
	if_NA_Math5_set(y[i], ai,bi,ci,di,ei)
	else {
	    y[i] = f(ai, bi, ci, di, ei);
	    if (ISNAN(y[i])) naflag = 1;
	}
    }

#define FINISH_Math5				\
    if(naflag)					\
	warningcall(lcall, R_MSG_NA);		\
						\
    if (n == na) DUPLICATE_ATTRIB(sy, sa);	\
    else if (n == nb) DUPLICATE_ATTRIB(sy, sb);	\
    else if (n == nc) DUPLICATE_ATTRIB(sy, sc);	\
    else if (n == nd) DUPLICATE_ATTRIB(sy, sd);	\
    else if (n == ne) DUPLICATE_ATTRIB(sy, se);	\
    UNPROTECT(6)

    FINISH_Math5;

    return sy;
} /* math5() */

#define Math5(A, FUN) \
	math5(CAR(A), CADR(A), CADDR(A), CAD3R(A), CAD4R(A), FUN);

SEXP attribute_hidden do_math5(SEXP call, SEXP op, SEXP args, SEXP env)
{
    checkArity(op, args);
    lcall = call;

    switch (PRIMVAL(op)) {

	/* Completely dummy for -Wall -- use math5() at all! : */
    case -99: return Math5(args, dhyper);
#ifdef UNIMP
    case  2: return Math5(args, p...);
    case  3: return Math5(args, q...);
#endif
    default:
	errorcall(call,
		  _("unimplemented real function of %d numeric arguments"), 5);
    }
    return op;			/* never used; to keep -Wall happy */
} /* do_math5() */

#endif /* Math5 is there */

/* This is used for experimenting with parallelized nmath functions -- LT */
CCODE R_get_arith_function(int which)
{
    switch (which) {
    case 1: return do_math1;
    case 2: return do_math2;
    case 3: return do_math3;
    case 4: return do_math4;
    case 11: return complex_math1;
    case 12: return complex_math2;
    default: error("bad arith function index"); return NULL;
    }
}
