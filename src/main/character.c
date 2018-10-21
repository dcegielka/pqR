/*
 *  pqR : A pretty quick version of R
 *  Copyright (C) 2013, 2014, 2015, 2016, 2017, 2018 by Radford M. Neal
 *
 *  Based on R : A Computer Language for Statistical Data Analysis
 *  Copyright (C) 1995, 1996  Robert Gentleman and Ross Ihaka
 *  Copyright (C) 1997--2010  The R Core Team
 *
 *  The changes in pqR from R-2.15.0 distributed by the R Core Team are
 *  documented in the NEWS and MODS files in the top-level source directory.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Pulic License as published by
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

/* The character functions in this file are

nzchar nchar substr substr<- abbreviate tolower toupper chartr strtrim

and the utility 

make.names

The regex functions

strsplit grep [g]sub [g]regexpr agrep

here prior to 2.10.0 are now in grep.c and agrep.c

make.unique, duplicated, unique, match, pmatch, charmatch are in unique.c
iconv is in sysutils.c


Support for UTF-8-encoded strings in non-UTF-8 locales
======================================================

Comparison is done directly unless you happen to be comparing the same
string in different encodings.

nzchar and nchar(, "bytes") are independent of the encoding
nchar(, "char") nchar(, "width") handle UTF-8 directly, translate Latin-1
substr substr<-  handle UTF-8 and Latin-1 directly
tolower toupper chartr  translate UTF-8 to wchar, rest to current charset
  which needs Unicode wide characters
abbreviate strtrim  translate

All the string matching functions handle UTF-8 directly, otherwise
translate (latin1 to UTF-8, otherwise to native).

Support for "bytes" marked encoding
===================================

nzchar and nchar(, "bytes") are independent of the encoding.

nchar(, "char") nchar(, "width") give NA (if allowed) or error.
substr substr<-  work in bytes

abbreviate chartr make.names strtrim tolower toupper give error.

*/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#define USE_FAST_PROTECT_MACROS
#include <Defn.h>
#include <errno.h>

#include <R_ext/RS.h>  /* for Calloc/Free */

#include <R_ext/rlocale.h>

/* We use a shared buffer here to avoid reallocing small buffers, and
   keep a standard-size (MAXELTSIZE = 8192) buffer allocated shared
   between the various functions.

   If we want to make this thread-safe, we would need to initialize an
   instance non-statically in each using function, but this would add
   to the overhead.
 */

#include "RBufferUtils.h"
static R_StringBuffer cbuff = {NULL, 0, MAXELTSIZE};

/* Functions to perform analogues of the standard C string library. */
/* Most are vectorized */

/* primitive */
static SEXP do_nzchar(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP x, ans;
    int i, len;

    checkArity(op, args);
    check1arg_x (args, call);

    if (isFactor(CAR(args)))
	error(_("'%s' requires a character vector"), "nzchar()");
    PROTECT(x = coerceVector(CAR(args), STRSXP));
    if (!isString(x))
	error(_("'%s' requires a character vector"), "nzchar()");
    len = LENGTH(x);
    PROTECT(ans = allocVector(LGLSXP, len));
    for (i = 0; i < len; i++)
	LOGICAL(ans)[i] = LENGTH(STRING_ELT(x, i)) > 0;
    UNPROTECT(2);
    return ans;
}


static SEXP do_nchar(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP names, dim, dimnames, ans, x, stype;
    int i, len, allowNA;
    R_len_t type_len;
    int nc;
    const char *type;
    const char *xi;
    wchar_t *wc;
    const void *vmax;

    checkArity(op, args);
    if (isFactor(CAR(args)))
        error(_("'%s' requires a character vector"), "nchar()");
    PROTECT(x = coerceVector(CAR(args), STRSXP));
    if (!isString(x))
        error(_("'%s' requires a character vector"), "nchar()");
    len = LENGTH(x);
    stype = CADR(args);
    if (!isString(stype) || LENGTH(stype)!=1 || LENGTH(STRING_ELT(stype,0))==0)
        error(_("invalid '%s' argument"), "type");
    type = CHAR(STRING_ELT(stype,0));
    type_len = LENGTH(STRING_ELT(stype,0));
    allowNA = asLogical(CADDR(args));
    if (allowNA == NA_LOGICAL) allowNA = 0;

    names = getAttrib (x, R_NamesSymbol);
    dim =getDimAttrib (x);
    dimnames = getAttrib (x, R_DimNamesSymbol);

    if (len == 1 && names == R_NilValue 
                 && dim == R_NilValue
                 && dimnames == R_NilValue) {
        ans = R_NilValue; /* simple scalars done with ScalarIntegerMaybeConst */
    }
    else {
        PROTECT(ans = allocVector(INTSXP, len));
        if (names != R_NilValue)    setAttrib(ans, R_NamesSymbol, names);
        if (dim != R_NilValue)      setAttrib(ans, R_DimSymbol, dim);
        if (dimnames != R_NilValue) setAttrib(ans, R_DimNamesSymbol, dimnames);
    }

    enum { type_chars, type_bytes, type_width } op_type;
    
    if (strncmp(type, "chars", type_len) == 0)
        op_type = type_chars;
    else if (strncmp(type, "bytes", type_len) == 0)
        op_type = type_bytes;
    else if (strncmp(type, "width", type_len) == 0)
        op_type = type_width;
    else
        error(_("invalid '%s' argument"), "type");

    vmax = VMAXGET();
    int nch;

    for (i = 0; i < len; i++) {
        SEXP sxi = STRING_ELT(x, i);
        if (sxi == NA_STRING) {
            nch = 2;
        }
        else if (IS_ASCII(sxi) || op_type == type_bytes) {
            nch = LENGTH(sxi);
        }
        else if (op_type == type_chars) {
            if (IS_UTF8(sxi)) { /* assume this is valid */
                const char *p = CHAR(sxi);
                nc = 0;
                for( ; *p; p += utf8clen(*p)) nc++;
                nch = nc;
            } else if (IS_BYTES(sxi)) {
                if (!allowNA) /* could do chars 0 */
                    error(_("number of characters is not computable for element %d in \"bytes\" encoding"), i+1);
                nch = NA_INTEGER;
            } else if (mbcslocale) {
                nc = mbstowcs(NULL, translateChar(sxi), 0);
                if (!allowNA && nc < 0)
                    error(_("invalid multibyte string %d"), i+1);
                nch = nc >= 0 ? nc : NA_INTEGER;
            } else
                nch = strlen(translateChar(sxi));
        }
        else {  /* op_type == type_width */
            if (IS_UTF8(sxi)) { /* assume this is valid */
                const char *p = CHAR(sxi);
                wchar_t wc1;
                nc = 0;
                for( ; *p; p += utf8clen(*p)) {
                    utf8toucs(&wc1, p);
                    nc += Ri18n_wcwidth(wc1);
                }
                nch = nc;
            } else if (IS_BYTES(sxi)) {
                if (!allowNA) /* could do width 0 */
                    error(_("width is not computable for element %d in \"bytes\" encoding"), i+1);
                nch = NA_INTEGER;
            } else if (mbcslocale) {
                xi = translateChar(sxi);
                nc = mbstowcs(NULL, xi, 0);
                if (nc >= 0) {
                    wc = (wchar_t *) R_AllocStringBuffer((nc+1)*sizeof(wchar_t), &cbuff);

                    mbstowcs(wc, xi, nc + 1);
                    nch = Ri18n_wcswidth(wc, 2147483647);
                    if (nch < 1) nch = nc;
                } else if (allowNA)
                    error(_("invalid multibyte string %d"), i+1);
                else
                    nch = NA_INTEGER;
            } else
                nch = strlen(translateChar(sxi));
        }
        VMAXSET(vmax);
        if (ans != R_NilValue)
            INTEGER(ans)[i] = nch;
    }

    R_FreeStringBufferL(&cbuff);

    UNPROTECT (1 + (ans!=R_NilValue));

    return ans == R_NilValue ? ScalarIntegerMaybeConst(nch) : ans;
}

/* Find the beginning and end (indexes start at 0) of the substring in str
   from sa to so (indexes start at 1).  If so is beyond the end of str, 
   the end is just past the last character of str.  Returns the number of
   characters (not necessarily same as bytes) in the substring (which will
   be so - sa + 1 or fewer).

   Tries to fudge something if the last character extends beyond slen
   (only possible in a string with an invalid multibyte character). */

static int find_substr (const char *str, size_t slen, int ienc, 
                        int sa, int so, size_t *beginning, size_t *end)
{
    size_t j;
    int i;

    *beginning = *end = slen;

    if (ienc == CE_UTF8) {
        for (i = 1, j = 0; i < sa; i++, j += utf8clen(str[j])) {
            if (j >= slen) return 0;
        }
        *beginning = j;
        while (i <= so) {
            j += utf8clen(str[j]);
            if (j > slen) return i - sa;
            i += 1;
            if (j == slen) return i - sa;
        }
        *end = j;
    }
    else if (ienc!=CE_LATIN1 && ienc!=CE_BYTES 
              && mbcslocale && !strIsASCII(str)) {
        mbstate_t mb_st;
        mbs_init(&mb_st);
        for (i = 1, j = 0; i < sa; 
                           i++, j += Mbrtowc(NULL,str+j,MB_CUR_MAX,&mb_st)) {
            if (j >= slen) return 0;
        }
        *beginning = j;
        while (i <= so) {
            j += Mbrtowc(NULL,str+j,MB_CUR_MAX,&mb_st);
            if (j > slen) return i - sa;
            i += 1;
            if (j == slen) return i - sa;
        }
        *end = j;
    }
    else {
        if (sa > slen) return 0;
        *beginning = sa-1;
        if (so > slen) return slen - sa + 1;
        *end = so;
    }

    return so - sa + 1; 
}

static SEXP do_substr(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP ans, res, x, sa, so, el;
    int i, len, start, stop, k, l;
    size_t slen;
    cetype_t ienc;

    checkArity(op, args);
    x = CAR(args);
    sa = CADR(args);
    so = CADDR(args);
    k = LENGTH(sa);
    l = LENGTH(so);

    if (!isString(x))
        error(_("extracting substrings from a non-character object"));

    len = LENGTH(x);
    if (len == 1 && !HAS_ATTRIB(x)) {
        ans = R_NilValue; /* simple scalars done with ScalarStringMaybeConst */
    }
    else {
        PROTECT(ans = allocVector (STRSXP, len));
    }

    if (len > 0) {
        if (!isInteger(sa) || !isInteger(so) || k == 0 || l == 0)
            error(_("invalid substring argument(s)"));

        for (i = 0; i < len; i++) {

            el = STRING_ELT(x,i);

            start = INTEGER(sa)[i % k];
            if (start != NA_INTEGER && start <= 0) start = 1;
            stop = INTEGER(so)[i % l];

            if (el == NA_STRING || start == NA_INTEGER || stop == NA_INTEGER) {
                res = NA_STRING;
            }
            else if (start > stop) {
                res = R_BlankString;
            }
            else {
                ienc = getCharCE(el);
                slen = LENGTH(el);
                const char *ss = CHAR(el);
                size_t beginning, end;
                if (IS_ASCII(el)) {
                    if (start > LENGTH(el))
                        beginning = end = 0;
                    else {
                        beginning = start-1;
                        if (stop > LENGTH(el))
                            end = LENGTH(el);
                        else
                            end = stop;
                    }
                }
                else {
                    (void) find_substr (ss, slen, ienc, start, stop, 
                                        &beginning, &end);
                }
    
                res = mkCharLenCE (ss+beginning, (int)(end-beginning), ienc);
            }

            if (ans != R_NilValue)
                SET_STRING_ELT (ans, i, res);
        }
    }

    if (ans == R_NilValue) {
        return ScalarStringMaybeConst(res);
    }
    else {
        DUPLICATE_ATTRIB (ans, x);  /* This copied the class, if any */
        UNPROTECT(1);
        return ans;
    }
}

/* Adapted from R-3.3.0:

  .Internal( startsWith(x, prefix) )  and
  .Internal( endsWith  (x, suffix) )
*/

SEXP attribute_hidden do_startsWith(SEXP call, SEXP op, SEXP args, SEXP env)
{
    checkArity(op, args);

    SEXP x = CAR(args), Xfix = CADR(args); // 'prefix' or 'suffix'

    if (isNull(x) || isNull(Xfix))
        return allocVector (LGLSXP, 0);

    if (!isString(x) || !isString(Xfix))
	error(_("non-character object(s)"));
    if (!isString(x) || !isString(Xfix))
	error(_("non-character object(s)"));

    R_xlen_t
	n1 = XLENGTH(x),
	n2 = XLENGTH(Xfix),
	n = (n1 > 0 && n2 > 0) ? ((n1 >= n2) ? n1 : n2) : 0;

    if (n == 0) 
        return allocVector (LGLSXP, 0);

    SEXP ans = PROTECT(allocVector(LGLSXP, n));

    typedef const char * cp;
    if (n2 == 1) { // optimize the most common case
	SEXP el = STRING_ELT(Xfix, 0);
	if (el == NA_STRING) {
	    for (R_xlen_t i = 0; i < n1; i++)
		LOGICAL(ans)[i] = NA_LOGICAL;
	} else {
	    // ASCII matching will do for ASCII Xfix except in non-UTF-8 MBCS
	    Rboolean need_translate = TRUE;
	    if (strIsASCII(CHAR(el)) && (utf8locale || !mbcslocale)) 
		need_translate = FALSE;
	    cp y0 = need_translate ? translateCharUTF8(el) : CHAR(el);
	    int ylen = (int) strlen(y0);
	    for (R_xlen_t i = 0; i < n1; i++) {
		SEXP el = STRING_ELT(x, i);
		if (el == NA_STRING) {
		    LOGICAL(ans)[i] = NA_LOGICAL;
		} else {
		    cp x0 = need_translate ? translateCharUTF8(el) : CHAR(el);
		    if(PRIMVAL(op) == 0) { // startsWith
			LOGICAL(ans)[i] = strncmp(x0, y0, ylen) == 0;
		    } else { // endsWith
			int off = (int)strlen(x0) - ylen;
			if (off < 0)
			    LOGICAL(ans)[i] = 0;
			else {
			    LOGICAL(ans)[i] = memcmp(x0 + off, y0, ylen) == 0;
			}
		    }
		}
	    }
	}
    } else { // n2 > 1
	// convert both inputs to UTF-8
	cp *x0 = (cp *) R_alloc(n1, sizeof(char *));
	cp *y0 = (cp *) R_alloc(n2, sizeof(char *));
	// and record lengths, -1 for NA
	int *x1 = (int *) R_alloc(n1, sizeof(int *));
	int *y1 = (int *) R_alloc(n2, sizeof(int *));
	for (R_xlen_t i = 0; i < n1; i++) {
	    SEXP el = STRING_ELT(x, i);
	    if (el == NA_STRING)
		x1[i] = -1;
	    else {
		x0[i] = translateCharUTF8(el);
		x1[i] = (int) strlen(x0[i]);
	    }
	}
	for (R_xlen_t i = 0; i < n2; i++) {
	    SEXP el = STRING_ELT(Xfix, i);
	    if (el == NA_STRING)
		y1[i] = -1;
	    else {
		y0[i] = translateCharUTF8(el);
		y1[i] = (int) strlen(y0[i]);
	    }
	}
	if(PRIMVAL(op) == 0) { // 0 = startsWith, 1 = endsWith
	    mod_iterate (n, n1, n2, i1, i2) {
		if (x1[i1] < 0 || y1[i2] < 0)
		    LOGICAL(ans)[i] = NA_LOGICAL;
		else if (x1[i1] < y1[i2])
		    LOGICAL(ans)[i] = 0;
		else // memcmp should be faster than strncmp
		    LOGICAL(ans)[i] = memcmp(x0[i1], y0[i2], y1[i2]) == 0;
            }
	} else { // endsWith
	    mod_iterate (n, n1, n2, i1, i2) {
		if (x1[i1] < 0 || y1[i2] < 0)
		    LOGICAL(ans)[i] = NA_LOGICAL;
		else {
		    int off = x1[i1] - y1[i2];
		    if (off < 0)
			LOGICAL(ans)[i] = 0;
		    else
			LOGICAL(ans)[i] = memcmp(x0[i1]+off,y0[i2],y1[i2]) == 0;
		}
            }
	}
    }
    UNPROTECT(1);
    return ans;
}

static SEXP do_substrgets(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP s, x, sa, so, value;
    int i, len, k, l, v;
    const void *vmax;

    const char *strings[4];  /* for interface to mkCharMulti */
    int lengths[3];

    checkArity(op, args);
    x = CAR(args);
    sa = CADR(args);
    so = CADDR(args);
    value = CADDDR(args);
    k = LENGTH(sa);
    l = LENGTH(so);

    if (!isString(x))
        error(_("replacing substrings in a non-character object"));
    len = LENGTH(x);
    PROTECT(s = allocVector(STRSXP, len));
    if (len > 0) {
        if (!isInteger(sa) || !isInteger(so) || k == 0 || l == 0)
            error(_("invalid substring argument(s)"));

        v = LENGTH(value);
        if (!isString(value) || v == 0) error(_("invalid value"));
        
        vmax = VMAXGET();
        for (i = 0; i < len; i++) {
            SEXP el = STRING_ELT(x, i);
            SEXP v_el = STRING_ELT(value, i % v);
            int start = INTEGER(sa)[i % k];
            int stop = INTEGER(so)[i % l];
            if (el == NA_STRING || v_el == NA_STRING ||
                start == NA_INTEGER || stop == NA_INTEGER) {
                SET_STRING_ELT_NA(s, i);
                continue;
            }
            if (start < 1) start = 1;
            if (start > stop) {
                SET_STRING_ELT(s, i, STRING_ELT(x, i));
                continue;
            }
            cetype_t ienc = getCharCE(el);
            int ienc2 = ienc;
            const char *ss = CHAR(el);
            size_t slen = LENGTH(el);
            const char *v_ss = CHAR(v_el);
            size_t v_ss_l = LENGTH(v_el);
            /* is the value in the same encoding?
               FIXME: could prefer UTF-8 here
             */
            if (!IS_ASCII(v_el) && getCharCE(el) != ienc) {
                ss = translateChar(el);
                slen = strlen(ss);
                v_ss = translateChar(v_el);
                ienc2 = CE_NATIVE;
                v_ss_l = strlen(v_ss);
            }
            size_t ss_b, ss_e, v_ss_b, v_ss_e;
            int n, v_n;
            n = find_substr (ss, slen, ienc2, start, stop, &ss_b, &ss_e);
            v_n = find_substr (v_ss, v_ss_l, ienc2, 1, n, &v_ss_b, &v_ss_e);
                  /* note: v_ss_b will be set to zero */
            if (v_n != n)
                n = find_substr (ss, slen, ienc2, start, start+v_n-1, 
                                 &ss_b, &ss_e);
            size_t new_len = slen - (ss_e-ss_b) + v_ss_e;
            if (new_len > INT_MAX) 
                error(_("new string is too long"));

            strings[0] = ss;    lengths[0] = (int) ss_b;
            strings[1] = v_ss;  lengths[1] = (int) v_ss_e;
            if (ss_e < slen) { 
                strings[2] = ss+ss_e;  lengths[2] = (int) (slen-ss_e);
                strings[3] = NULL;
            }
            else 
                strings[2] = NULL;

            SET_STRING_ELT(s, i, Rf_mkCharMulti(strings, lengths, 0, ienc2));

            VMAXSET(vmax);
        }
    }
    UNPROTECT(1);
    return s;
}

/* Abbreviate
   long names in the S-designated fashion:
   1) spaces
   2) lower case vowels
   3) lower case consonants
   4) upper case letters
   5) special characters.

   Letters are dropped from the end of words
   and at least one letter is retained from each word.

   If unique abbreviations are not produced letters are added until the
   results are unique (duplicated names are removed prior to entry).
   names, minlength, use.classes, dot
*/


#define FIRSTCHAR(i) (isspace((int)buff1[i-1]))
#define LASTCHAR(i) (!isspace((int)buff1[i-1]) && (!buff1[i+1] || isspace((int)buff1[i+1])))
#define LOWVOW(i) (buff1[i] == 'a' || buff1[i] == 'e' || buff1[i] == 'i' || \
        	   buff1[i] == 'o' || buff1[i] == 'u')


/* memmove does allow overlapping src and dest */
static void mystrcpy(char *dest, const char *src)
{
    memmove(dest, src, strlen(src)+1);
}


/* abbreviate(inchar, minlen) */
static SEXP stripchars(const char * const inchar, int minlen)
{
/* This routine used to use strcpy with overlapping dest and src.
   That is not allowed by ISO C.
 */
    int i, j, nspace = 0, upper;
    char *buff1 = cbuff.data;

    mystrcpy(buff1, inchar);
    upper = strlen(buff1)-1;

    /* remove leading blanks */
    j = 0;
    for (i = 0 ; i < upper ; i++)
	if (isspace((int)buff1[i]))
	    j++;
	else
	    break;

    mystrcpy(buff1, &buff1[j]);
    upper = strlen(buff1) - 1;

    if (strlen(buff1) < minlen)
	goto donesc;

    for (i = upper, j = 1; i > 0; i--) {
	if (isspace((int)buff1[i])) {
	    if (j)
		buff1[i] = '\0' ;
	    else
		nspace++;
	}
	else
	    j = 0;
	/*strcpy(buff1[i],buff1[i+1]);*/
	if (strlen(buff1) - nspace <= minlen)
	    goto donesc;
    }

    upper = strlen(buff1) -1;
    for (i = upper; i > 0; i--) {
	if (LOWVOW(i) && LASTCHAR(i))
	    mystrcpy(&buff1[i], &buff1[i + 1]);
	if (strlen(buff1) - nspace <= minlen)
	    goto donesc;
    }

    upper = strlen(buff1) -1;
    for (i = upper; i > 0; i--) {
	if (LOWVOW(i) && !FIRSTCHAR(i))
	    mystrcpy(&buff1[i], &buff1[i + 1]);
	if (strlen(buff1) - nspace <= minlen)
	    goto donesc;
    }

    upper = strlen(buff1) - 1;
    for (i = upper; i > 0; i--) {
	if (islower((int)buff1[i]) && LASTCHAR(i))
	    mystrcpy(&buff1[i], &buff1[i + 1]);
	if (strlen(buff1) - nspace <= minlen)
	    goto donesc;
    }

    upper = strlen(buff1) -1;
    for (i = upper; i > 0; i--) {
	if (islower((int)buff1[i]) && !FIRSTCHAR(i))
	    mystrcpy(&buff1[i], &buff1[i + 1]);
	if (strlen(buff1) - nspace <= minlen)
	    goto donesc;
    }

    /* all else has failed so we use brute force */

    upper = strlen(buff1) - 1;
    for (i = upper; i > 0; i--) {
	if (!FIRSTCHAR(i) && !isspace((int)buff1[i]))
	    mystrcpy(&buff1[i], &buff1[i + 1]);
	if (strlen(buff1) - nspace <= minlen)
	    goto donesc;
    }

donesc:

    upper = strlen(buff1);
    if (upper > minlen)
	for (i = upper - 1; i > 0; i--)
	    if (isspace((int)buff1[i]))
		mystrcpy(&buff1[i], &buff1[i + 1]);

    return(mkChar(buff1));
}


static SEXP do_abbrev(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP x, ans;
    int i, len, minlen;
    Rboolean warn = FALSE;
    const char *s;
    const void *vmax;

    checkArity(op,args);
    x = CAR(args);

    if (!isString(x))
	error(_("the first argument must be a character vector"));
    len = length(x);

    PROTECT(ans = allocVector(STRSXP, len));
    minlen = asInteger(CADR(args));
    vmax = VMAXGET();
    for (i = 0 ; i < len ; i++) {
	if (STRING_ELT(x, i) == NA_STRING)
	    SET_STRING_ELT_NA(ans, i);
	else {
	    s = translateChar(STRING_ELT(x, i));
	    warn = warn | !strIsASCII(s);
	    R_AllocStringBuffer(strlen(s), &cbuff);
	    SET_STRING_ELT(ans, i, stripchars(s, minlen));
	}
	VMAXSET(vmax);
    }
    if (warn) warning(_("abbreviate used with non-ASCII chars"));
    DUPLICATE_ATTRIB(ans, x);
    /* This copied the class, if any */
    R_FreeStringBufferL(&cbuff);
    UNPROTECT(1);
    return(ans);
}

static SEXP do_makenames(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP arg, ans;
    int i, l, n, allow_, nodotdot;
    char *tmp = NULL, *cbuf;
    const char *This;
    Rboolean need_prefix;
    const void *vmax;

    checkArity(op ,args);
    arg = CAR(args);
    if (!isString(arg))
	error(_("non-character names"));
    n = length(arg);
    allow_ = asLogical(CADR(args));
    if (allow_ == NA_LOGICAL)
	error(_("invalid '%s' value"), "allow_");
    nodotdot = asInteger(CADDR(args));
    if (nodotdot == NA_INTEGER)
	error(_("invalid '%s' value"), "no..");
    PROTECT(ans = allocVector(STRSXP, n));
    vmax = VMAXGET();
    for (i = 0 ; i < n ; i++) {
	This = translateChar(STRING_ELT(arg, i));
	l = strlen(This);
	/* need to prefix names not beginning with alpha or ., as
	   well as . followed by a number */
	need_prefix = FALSE;
	if (mbcslocale && This[0]) {
	    int nc = l, used;
	    wchar_t wc;
	    mbstate_t mb_st;
	    const char *pp = This;
	    mbs_init(&mb_st);
	    used = Mbrtowc(&wc, pp, MB_CUR_MAX, &mb_st);
	    pp += used; nc -= used;
	    if (wc == L'.') {
		if (nc > 0) {
		    Mbrtowc(&wc, pp, MB_CUR_MAX, &mb_st);
		    if (iswdigit(wc))  need_prefix = TRUE;
		}
	    } else if (!iswalpha(wc)) need_prefix = TRUE;
	} else {
	    if (This[0] == '.') {
		if (l >= 1 && isdigit(0xff & (int) This[1])) need_prefix = TRUE;
	    } else if (!isalpha(0xff & (int) This[0])) need_prefix = TRUE;
	}
	if (need_prefix) {
	    tmp = Calloc(l+2, char);
	    strcpy(tmp, "X");
	    strcat(tmp, translateChar(STRING_ELT(arg, i)));
	} else {
	    tmp = Calloc(l+1, char);
	    strcpy(tmp, translateChar(STRING_ELT(arg, i)));
	}
        int startd = -1;
        if (mbcslocale) {
            /* This cannot lengthen the string, so safe to overwrite it. */
            int nc = mbstowcs(NULL, tmp, 0);
            if (nc < 0) error(_("invalid multibyte string %d"), i+1);
            wchar_t *wstr = Calloc(nc+1, wchar_t);
            mbstowcs(wstr, tmp, nc+1);
            wchar_t *p, *q, *r;
            for (p = wstr; *p != L'\0'; p++) {
                if (*p != L'.' && startd == -1) startd = p-wstr;
                if (*p == L'.' || (allow_ && *p == L'_'))
                    /* leave alone */;
                else if (!iswalnum((int)*p))
                    *p = L'.';
            }
            if (nodotdot != 0 && startd != -1) { /* convert multiple ... to . */
                q = p;
                if (nodotdot == 1) while (*(q-1) == '.') q -= 1;
                p = r = wstr + startd + 1;
                while (p < q) {
                    if (*p != L'.' || *(r-1) != L'.') *r++ = *p;
                    p += 1;
                }
                while (*p != L'\0') *r++ = *p++;
                *r = L'\0';
            }
            wcstombs(tmp, wstr, strlen(tmp)+1);
            Free(wstr);
        } 
        else {
            char *p, *q, *r;
            for (p = tmp; *p; p++) {
                if (*p != '.' && startd == -1) startd = p-tmp;
                if (*p == '.' || (allow_ && *p == '_')) 
                    /* leave alone */;
                else if (!isalnum(0xff & (int)*p))
                    *p = '.';
            }
            if (nodotdot != 0 && startd != -1) { /* convert multiple ... to . */
                q = p;
                if (nodotdot == 1) while (*(q-1) == '.') q -= 1;
                p = r = tmp + startd + 1;
                while (p < q) {
                    if (*p != '.' || *(r-1) != '.') *r++ = *p;
                    p += 1;
                }
                while (*p != 0) *r++ = *p++;
                *r = 0;
            }
        }
	SET_STRING_ELT(ans, i, mkChar(tmp));
	/* do we have a reserved word?  If so the name is invalid */
	if (!isValidName(tmp)) {
	    /* FIXME: could use R_Realloc instead */
	    cbuf = CallocCharBuf(strlen(tmp) + 1);
	    strcpy(cbuf, tmp);
	    strcat(cbuf, ".");
	    SET_STRING_ELT(ans, i, mkChar(cbuf));
	    Free(cbuf);
	}
	Free(tmp);
	VMAXSET(vmax);
    }
    UNPROTECT(1);
    return ans;
}


static SEXP do_tolower(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP x, y;
    int i, n, ul;
    char *p;
    SEXP el;
    cetype_t ienc;
    Rboolean use_UTF8 = FALSE;
    const void *vmax;

    checkArity(op, args);
    ul = PRIMVAL(op); /* 0 = tolower, 1 = toupper */

    x = CAR(args);
    /* coercion is done in wrapper */
    if (!isString(x)) error(_("non-character argument"));
    n = LENGTH(x);
    PROTECT(y = allocVector(STRSXP, n));
#if defined(Win32) || defined(__STDC_ISO_10646__) || defined(__APPLE__) || defined(__FreeBSD__)
    /* utf8towcs is really to UCS-4/2 */
    for (i = 0; i < n; i++)
	if (getCharCE(STRING_ELT(x, i)) == CE_UTF8) use_UTF8 = TRUE;
    if (mbcslocale || use_UTF8 == TRUE)
#else
    if (mbcslocale)
#endif
    {
	int nb, nc, j;
	wctrans_t tr = wctrans(ul ? "toupper" : "tolower");
	wchar_t * wc;
	char * cbuf;

	vmax = VMAXGET();
	/* the translated string need not be the same length in bytes */
	for (i = 0; i < n; i++) {
	    el = STRING_ELT(x, i);
	    if (el == NA_STRING) SET_STRING_ELT_NA(y, i);
	    else {
		const char *xi;
		ienc = getCharCE(el);
		if (use_UTF8 && ienc == CE_UTF8) {
		    xi = CHAR(el);
		    nc = utf8towcs(NULL, xi, 0);
		} else {
		    xi = translateChar(el);
		    nc = mbstowcs(NULL, xi, 0);
		    ienc = CE_NATIVE;
		}
		if (nc >= 0) {
		    /* FIXME use this buffer for new string as well */
		    wc = (wchar_t *)
			R_AllocStringBuffer((nc+1)*sizeof(wchar_t), &cbuff);
		    if (ienc == CE_UTF8) {
			utf8towcs(wc, xi, nc + 1);
			for (j = 0; j < nc; j++) wc[j] = towctrans(wc[j], tr);
			nb = wcstoutf8(NULL, wc, 0);
			cbuf = CallocCharBuf(nb);
			wcstoutf8(cbuf, wc, nb + 1);
			SET_STRING_ELT(y, i, mkCharCE(cbuf, CE_UTF8));
		    } else {
			mbstowcs(wc, xi, nc + 1);
			for (j = 0; j < nc; j++) wc[j] = towctrans(wc[j], tr);
			nb = wcstombs(NULL, wc, 0);
			cbuf = CallocCharBuf(nb);
			wcstombs(cbuf, wc, nb + 1);
			SET_STRING_ELT(y, i, markKnown(cbuf, el));
		    }
		    Free(cbuf);
		} else {
		    error(_("invalid multibyte string %d"), i+1);
		}
	    }
	    VMAXSET(vmax);
	}
	R_FreeStringBufferL(&cbuff);
    } else {
	char *xi;
	vmax = VMAXGET();
	for (i = 0; i < n; i++) {
	    if (STRING_ELT(x, i) == NA_STRING)
		SET_STRING_ELT_NA(y, i);
	    else {
		xi = CallocCharBuf(strlen(CHAR(STRING_ELT(x, i))));
		strcpy(xi, translateChar(STRING_ELT(x, i)));
		for (p = xi; *p != '\0'; p++)
		    *p = ul ? toupper(*p) : tolower(*p);
		SET_STRING_ELT(y, i, markKnown(xi, STRING_ELT(x, i)));
		Free(xi);
	    }
	    VMAXSET(vmax);
	}
    }
    DUPLICATE_ATTRIB(y, x);
    /* This copied the class, if any */
    UNPROTECT(1);
    return(y);
}


typedef enum { WTR_INIT, WTR_CHAR, WTR_RANGE } wtr_type;
struct wtr_spec {
    wtr_type type;
    struct wtr_spec *next;
    union {
	wchar_t c;
	struct {
	    wchar_t first;
	    wchar_t last;
	} r;
    } u;
};

static void
wtr_build_spec(const wchar_t *s, struct wtr_spec *trs) {
    int i, len = wcslen(s);
    struct wtr_spec *This, *_new;

    This = trs;
    for (i = 0; i < len - 2; ) {
	_new = Calloc(1, struct wtr_spec);
	_new->next = NULL;
	if (s[i + 1] == L'-') {
	    _new->type = WTR_RANGE;
	    if (s[i] > s[i + 2])
		error(_("decreasing range specification ('%lc-%lc')"),
		      s[i], s[i + 2]);
	    _new->u.r.first = s[i];
	    _new->u.r.last = s[i + 2];
	    i = i + 3;
	} else {
	    _new->type = WTR_CHAR;
	    _new->u.c = s[i];
	    i++;
	}
	This = This->next = _new;
    }
    for ( ; i < len; i++) {
	_new = Calloc(1, struct wtr_spec);
	_new->next = NULL;
	_new->type = WTR_CHAR;
	_new->u.c = s[i];
	This = This->next = _new;
    }
}

static void
wtr_free_spec(struct wtr_spec *trs) {
    struct wtr_spec *This, *next;
    This = trs;
    while(This) {
	next = This->next;
	Free(This);
	This = next;
    }
}

static wchar_t
wtr_get_next_char_from_spec(struct wtr_spec **p) {
    wchar_t c;
    struct wtr_spec *This;

    This = *p;
    if (!This)
	return('\0');
    switch(This->type) {
	/* Note: this code does not deal with the WTR_INIT case. */
    case WTR_CHAR:
	c = This->u.c;
	*p = This->next;
	break;
    case WTR_RANGE:
	c = This->u.r.first;
	if (c == This->u.r.last) {
	    *p = This->next;
	} else {
	    (This->u.r.first)++;
	}
	break;
    default:
	c = L'\0';
	break;
    }
    return(c);
}

typedef enum { TR_INIT, TR_CHAR, TR_RANGE } tr_spec_type;
struct tr_spec {
    tr_spec_type type;
    struct tr_spec *next;
    union {
	unsigned char c;
	struct {
	    unsigned char first;
	    unsigned char last;
	} r;
    } u;
};

static void
tr_build_spec(const char *s, struct tr_spec *trs) {
    int i, len = strlen(s);
    struct tr_spec *This, *_new;

    This = trs;
    for (i = 0; i < len - 2; ) {
	_new = Calloc(1, struct tr_spec);
	_new->next = NULL;
	if (s[i + 1] == '-') {
	    _new->type = TR_RANGE;
	    if (s[i] > s[i + 2])
		error(_("decreasing range specification ('%c-%c')"),
		      s[i], s[i + 2]);
	    _new->u.r.first = s[i];
	    _new->u.r.last = s[i + 2];
	    i = i + 3;
	} else {
	    _new->type = TR_CHAR;
	    _new->u.c = s[i];
	    i++;
	}
	This = This->next = _new;
    }
    for ( ; i < len; i++) {
	_new = Calloc(1, struct tr_spec);
	_new->next = NULL;
	_new->type = TR_CHAR;
	_new->u.c = s[i];
	This = This->next = _new;
    }
}

static void
tr_free_spec(struct tr_spec *trs) {
    struct tr_spec *This, *next;
    This = trs;
    while(This) {
	next = This->next;
	Free(This);
	This = next;
    }
}

static unsigned char
tr_get_next_char_from_spec(struct tr_spec **p) {
    unsigned char c;
    struct tr_spec *This;

    This = *p;
    if (!This)
	return('\0');
    switch(This->type) {
	/* Note: this code does not deal with the TR_INIT case. */
    case TR_CHAR:
	c = This->u.c;
	*p = This->next;
	break;
    case TR_RANGE:
	c = This->u.r.first;
	if (c == This->u.r.last) {
	    *p = This->next;
	} else {
	    (This->u.r.first)++;
	}
	break;
    default:
	c = '\0';
	break;
    }
    return(c);
}

typedef struct { wchar_t c_old, c_new; } xtable_t;

static R_INLINE int xtable_comp(const void *a, const void *b)
{
    return ((xtable_t *)a)->c_old - ((xtable_t *)b)->c_old;
}

static R_INLINE int xtable_key_comp(const void *a, const void *b)
{
    return *((wchar_t *)a) - ((xtable_t *)b)->c_old;
}

#define SWAP(_a, _b, _TYPE)                                    \
{                                                              \
    _TYPE _t;                                                  \
    _t    = *(_a);                                             \
    *(_a) = *(_b);                                             \
    *(_b) = _t;                                                \
}

#define ISORT(_base,_num,_TYPE,_comp)                          \
{                                                              \
/* insert sort */                                              \
/* require stable data */                                      \
    int _i, _j ;                                               \
    for ( _i = 1 ; _i < _num ; _i++ )                          \
	for ( _j = _i; _j > 0 &&                               \
		      (*_comp)(_base+_j-1, _base+_j)>0; _j--)  \
	   SWAP(_base+_j-1, _base+_j, _TYPE);                  \
}

#define COMPRESS(_base,_num,_TYPE,_comp)                       \
{                                                              \
/* supress even c_old. last use */                             \
    int _i,_j ;                                                \
    for ( _i = 0 ; _i < (*(_num)) - 1 ; _i++ ){                \
	int rc = (*_comp)(_base+_i, _base+_i+1);               \
	if (rc == 0){                                          \
	   for ( _j = _i, _i-- ; _j < (*(_num)) - 1; _j++ )     \
		*((_base)+_j) = *((_base)+_j+1);               \
	    (*(_num))--;                                       \
	}                                                      \
    }                                                          \
}

#define BSEARCH(_rc,_key,_base,_nmemb,_TYPE,_comp)             \
{                                                              \
    size_t l, u, idx;                                          \
    _TYPE *p;                                                  \
    int comp;                                                  \
    l = 0;                                                     \
    u = _nmemb;                                                \
    _rc = NULL;                                                \
    while (l < u)                                              \
    {                                                          \
	idx = (l + u) / 2;                                     \
	p =  (_base) + idx;                                    \
	comp = (*_comp)(_key, p);                              \
	if (comp < 0)                                          \
	    u = idx;                                           \
	else if (comp > 0)                                     \
	    l = idx + 1;                                       \
	else{                                                  \
	  _rc = p;                                             \
	  break;                                               \
	}                                                      \
    }                                                          \
}

static SEXP do_chartr(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP old, _new, x, y;
    int i, n;
    char *cbuf;
    SEXP el;
    cetype_t ienc;
    Rboolean use_UTF8 = FALSE;
    const void *vmax;

    checkArity(op, args);
    old = CAR(args); args = CDR(args);
    _new = CAR(args); args = CDR(args);
    x = CAR(args);
    n = LENGTH(x);
    if (!isString(old) || length(old) < 1 || STRING_ELT(old, 0) == NA_STRING)
	error(_("invalid '%s' argument"), "old");
    if (length(old) > 1)
	warning(_("argument '%s' has length > 1 and only the first element will be used"), "old");
    if (!isString(_new) || length(_new) < 1 || STRING_ELT(_new, 0) == NA_STRING)
	error(_("invalid '%s' argument"), "new");
    if (length(_new) > 1)
	warning(_("argument '%s' has length > 1 and only the first element will be used"), "new");
    if (!isString(x)) error("invalid '%s' argument", "x");

    /* utf8towcs is really to UCS-4/2 */
#if defined(Win32) || defined(__STDC_ISO_10646__) || defined(__APPLE__) || defined(__FreeBSD__)
    for (i = 0; i < n; i++)
	if (getCharCE(STRING_ELT(x, i)) == CE_UTF8) use_UTF8 = TRUE;

    if (getCharCE(STRING_ELT(old, 0)) == CE_UTF8) use_UTF8 = TRUE;
    if (getCharCE(STRING_ELT(_new, 0)) == CE_UTF8) use_UTF8 = TRUE;

    if (mbcslocale || use_UTF8 == TRUE)
#else
    if (mbcslocale)
#endif
    {
	int j, nb, nc;
	xtable_t *xtable, *tbl;
	int xtable_cnt;
	struct wtr_spec *trs_cnt, **trs_cnt_ptr;
	wchar_t c_old, c_new, *wc;
	const char *xi, *s;
	struct wtr_spec *trs_old, **trs_old_ptr;
	struct wtr_spec *trs_new, **trs_new_ptr;

	/* Initialize the old and new wtr_spec lists. */
	trs_old = Calloc(1, struct wtr_spec);
	trs_old->type = WTR_INIT;
	trs_old->next = NULL;
	trs_new = Calloc(1, struct wtr_spec);
	trs_new->type = WTR_INIT;
	trs_new->next = NULL;
	/* Build the old and new wtr_spec lists. */
	if (use_UTF8 && getCharCE(STRING_ELT(old, 0)) == CE_UTF8) {
	    s = CHAR(STRING_ELT(old, 0));
	    nc = utf8towcs(NULL, s, 0);
	    if (nc < 0) error(_("invalid UTF-8 string 'old'"));
	    wc = (wchar_t *) R_AllocStringBuffer((nc+1)*sizeof(wchar_t), &cbuff);
	    utf8towcs(wc, s, nc + 1);
	} else {
	    s = translateChar(STRING_ELT(old, 0));
	    nc = mbstowcs(NULL, s, 0);
	    if (nc < 0) error(_("invalid multibyte string 'old'"));
	    wc = (wchar_t *) R_AllocStringBuffer((nc+1)*sizeof(wchar_t), &cbuff);
	    mbstowcs(wc, s, nc + 1);
	}
	wtr_build_spec(wc, trs_old);
	trs_cnt = Calloc(1, struct wtr_spec);
	trs_cnt->type = WTR_INIT;
	trs_cnt->next = NULL;
	wtr_build_spec(wc, trs_cnt); /* use count only */

	if (use_UTF8 && getCharCE(STRING_ELT(_new, 0)) == CE_UTF8) {
	    s = CHAR(STRING_ELT(_new, 0));
	    nc = utf8towcs(NULL, s, 0);
	    if (nc < 0) error(_("invalid UTF-8 string 'new'"));
	    wc = (wchar_t *) R_AllocStringBuffer((nc+1)*sizeof(wchar_t), &cbuff);
	    utf8towcs(wc, s, nc + 1);
	} else {
	    s = translateChar(STRING_ELT(_new, 0));
	    nc = mbstowcs(NULL, s, 0);
	    if (nc < 0) error(_("invalid multibyte string 'new'"));
	    wc = (wchar_t *) R_AllocStringBuffer((nc+1)*sizeof(wchar_t), &cbuff);
	    mbstowcs(wc, s, nc + 1);
	}
	wtr_build_spec(wc, trs_new);

	/* Initialize the pointers for walking through the old and new
	   wtr_spec lists and retrieving the next chars from the lists.
	*/

	trs_cnt_ptr = Calloc(1, struct wtr_spec *);
	*trs_cnt_ptr = trs_cnt->next;
	for (xtable_cnt = 0 ; wtr_get_next_char_from_spec(trs_cnt_ptr); 
	      xtable_cnt++) ;
	wtr_free_spec(trs_cnt);
	Free(trs_cnt_ptr);
	xtable = (xtable_t *) R_alloc(xtable_cnt+1, sizeof(xtable_t));

	trs_old_ptr = Calloc(1, struct wtr_spec *);
	*trs_old_ptr = trs_old->next;
	trs_new_ptr = Calloc(1, struct wtr_spec *);
	*trs_new_ptr = trs_new->next;
	for (i = 0; ; i++) {
	    c_old = wtr_get_next_char_from_spec(trs_old_ptr);
	    c_new = wtr_get_next_char_from_spec(trs_new_ptr);
	    if (c_old == '\0')
		break;
	    else if (c_new == '\0')
		error(_("'old' is longer than 'new'"));
	    else {
		xtable[i].c_old = c_old;
		xtable[i].c_new = c_new;
	    }
	}

	/* Free the memory occupied by the wtr_spec lists. */
	wtr_free_spec(trs_old);
	wtr_free_spec(trs_new);
	Free(trs_old_ptr); Free(trs_new_ptr);

	ISORT(xtable, xtable_cnt, xtable_t , xtable_comp);
	COMPRESS(xtable, &xtable_cnt, xtable_t, xtable_comp);

	PROTECT(y = allocVector(STRSXP, n));
	vmax = VMAXGET();
	for (i = 0; i < n; i++) {
	    el = STRING_ELT(x,i);
	    if (el == NA_STRING)
		SET_STRING_ELT_NA(y, i);
	    else {
		ienc = getCharCE(el);
		if (use_UTF8 && ienc == CE_UTF8) {
		    xi = CHAR(el);
		    nc = utf8towcs(NULL, xi, 0);
		} else {
		    xi = translateChar(el);
		    nc = mbstowcs(NULL, xi, 0);
		    ienc = CE_NATIVE;
		}
		if (nc < 0)
		    error(_("invalid input multibyte string %d"), i+1);
		wc = (wchar_t *) R_AllocStringBuffer((nc+1)*sizeof(wchar_t),
						     &cbuff);
		if (ienc == CE_UTF8) utf8towcs(wc, xi, nc + 1);
		else mbstowcs(wc, xi, nc + 1);
		for (j = 0; j < nc; j++){
		    BSEARCH(tbl,&wc[j], xtable, xtable_cnt,
			    xtable_t, xtable_key_comp);
		    if (tbl) wc[j] = tbl->c_new;
		}
		if (ienc == CE_UTF8) {
		    nb = wcstoutf8(NULL, wc, 0);
		    cbuf = CallocCharBuf(nb);
		    wcstoutf8(cbuf, wc, nb + 1);
		    SET_STRING_ELT(y, i, mkCharCE(cbuf, CE_UTF8));
		} else {
		    nb = wcstombs(NULL, wc, 0);
		    cbuf = CallocCharBuf(nb);
		    wcstombs(cbuf, wc, nb + 1);
		    SET_STRING_ELT(y, i, markKnown(cbuf, el));
		}
		Free(cbuf);
	    }
	    VMAXSET(vmax);
	}
	R_FreeStringBufferL(&cbuff);
    } else {
	unsigned char xtable[UCHAR_MAX + 1], *p, c_old, c_new;
	struct tr_spec *trs_old, **trs_old_ptr;
	struct tr_spec *trs_new, **trs_new_ptr;

	for (i = 0; i <= UCHAR_MAX; i++) xtable[i] = i;

	/* Initialize the old and new tr_spec lists. */
	trs_old = Calloc(1, struct tr_spec);
	trs_old->type = TR_INIT;
	trs_old->next = NULL;
	trs_new = Calloc(1, struct tr_spec);
	trs_new->type = TR_INIT;
	trs_new->next = NULL;
	/* Build the old and new tr_spec lists. */
	tr_build_spec(translateChar(STRING_ELT(old, 0)), trs_old);
	tr_build_spec(translateChar(STRING_ELT(_new, 0)), trs_new);
	/* Initialize the pointers for walking through the old and new
	   tr_spec lists and retrieving the next chars from the lists.
	*/
	trs_old_ptr = Calloc(1, struct tr_spec *);
	*trs_old_ptr = trs_old->next;
	trs_new_ptr = Calloc(1, struct tr_spec *);
	*trs_new_ptr = trs_new->next;
	for (;;) {
	    c_old = tr_get_next_char_from_spec(trs_old_ptr);
	    c_new = tr_get_next_char_from_spec(trs_new_ptr);
	    if (c_old == '\0')
		break;
	    else if (c_new == '\0')
		error(_("'old' is longer than 'new'"));
	    else
		xtable[c_old] = c_new;
	}
	/* Free the memory occupied by the tr_spec lists. */
	tr_free_spec(trs_old);
	tr_free_spec(trs_new);
	Free(trs_old_ptr); Free(trs_new_ptr);

	n = LENGTH(x);
	PROTECT(y = allocVector(STRSXP, n));
	vmax = VMAXGET();
	for (i = 0; i < n; i++) {
	    if (STRING_ELT(x,i) == NA_STRING)
		SET_STRING_ELT_NA(y, i);
	    else {
		const char *xi = translateChar(STRING_ELT(x, i));
		cbuf = CallocCharBuf(strlen(xi));
		strcpy(cbuf, xi);
		for (p = (unsigned char *) cbuf; *p != '\0'; p++)
		    *p = xtable[*p];
		SET_STRING_ELT(y, i, markKnown(cbuf, STRING_ELT(x, i)));
		Free(cbuf);
	    }
	}
	VMAXSET(vmax);
    }

    DUPLICATE_ATTRIB(y, x);
    /* This copied the class, if any */
    UNPROTECT(1);
    return(y);
}

static SEXP do_strtrim(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP s, x, width;
    int i, len, nw, w, nc;
    const char *This;
    char *buf;
    const char *p; char *q;
    int w0, wsum, k, nb;
    wchar_t wc;
    mbstate_t mb_st;
    const void *vmax;

    checkArity(op, args);
    /* as.character happens at R level now */
    if (!isString(x = CAR(args)))
	error(_("strtrim() requires a character vector"));
    len = LENGTH(x);
    PROTECT(width = coerceVector(CADR(args), INTSXP));
    nw = LENGTH(width);
    if (!nw || (nw < len && len % nw))
	error(_("invalid '%s' argument"), "width");
    for (i = 0; i < nw; i++)
	if (INTEGER(width)[i] == NA_INTEGER ||
	   INTEGER(width)[i] < 0)
	    error(_("invalid '%s' argument"), "width");
    PROTECT(s = allocVector(STRSXP, len));
    vmax = VMAXGET();
    for (i = 0; i < len; i++) {
	if (STRING_ELT(x, i) == NA_STRING) {
	    SET_STRING_ELT(s, i, STRING_ELT(x, i));
	    continue;
	}
	w = INTEGER(width)[i % nw];
	This = translateChar(STRING_ELT(x, i));
	nc = strlen(This);
	buf = ALLOC_STRING_BUFF(nc,&cbuff);
	wsum = 0;
	mbs_init(&mb_st);
	for (p = This, w0 = 0, q = buf; *p ;) {
	    nb =  Mbrtowc(&wc, p, MB_CUR_MAX, &mb_st);
	    w0 = Ri18n_wcwidth(wc);
	    if (w0 < 0) { p += nb; continue; } /* skip non-printable chars */
	    wsum += w0;
	    if (wsum <= w) {
		for (k = 0; k < nb; k++) *q++ = *p++;
	    } else break;
	}
	*q = '\0';
	SET_STRING_ELT(s, i, markKnown(buf, STRING_ELT(x, i)));
	VMAXSET(vmax);
    }
    if (len > 0) R_FreeStringBufferL(&cbuff);
    DUPLICATE_ATTRIB(s, x);
    /* This copied the class, if any */
    UNPROTECT(2);
    return s;
}

static int strtoi(SEXP s, int base)
{
    long res;
    char *endp;

    /* strtol might return extreme values on error */
    errno = 0;

    if(s == NA_STRING) return(NA_INTEGER);
    res = strtol(CHAR(s), &endp, base); /* ASCII */
    if(errno || *endp != '\0') res = NA_INTEGER;
    if(res > INT_MAX || res < INT_MIN) res = NA_INTEGER;
    return(res);
}

static SEXP do_strtoi(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP ans, x, b;
    int i, n, base;

    checkArity(op, args);

    x = CAR(args); args = CDR(args);
    b = CAR(args);
    
    if(!isInteger(b) || (length(b) < 1))
	error(_("invalid '%s' argument"), "base");
    base = INTEGER(b)[0];
    if((base != 0) && ((base < 2) || (base > 36)))
	error(_("invalid '%s' argument"), "base");

    PROTECT(ans = allocVector(INTSXP, n = LENGTH(x)));
    for(i = 0; i < n; i++)
	INTEGER(ans)[i] = strtoi(STRING_ELT(x, i), base);
    UNPROTECT(1);
    
    return ans;
}

/* From R-3.3.0 / R-3.4.2, with substantial modifications for pqR. */

SEXP attribute_hidden do_strrep(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP d, s, x, n, el;
    R_xlen_t is, ix, in, ns, nx, nn;
    const char *xi;
    int ni, nc;

    checkArity(op, args);

    x = CAR(args); args = CDR(args);
    n = CAR(args);

    nx = XLENGTH(x);
    nn = XLENGTH(n);
    if((nx == 0) || (nn == 0))
        return allocVector(STRSXP, 0);

    ns = (nx > nn) ? nx : nn;

    PROTECT(s = allocVector(STRSXP, ns));

    ix = in = 0;
    for (is = 0; is < ns; is++) {
        el = STRING_ELT(x, ix);
        ni = INTEGER(n)[in];
        if (el == NA_STRING || ni == NA_INTEGER) {
            SET_STRING_ELT_NA(s, is);
        }
        else {
            if (ni < 0)
                error(_("invalid '%s' value"), "times");
            xi = CHAR(el);
            nc = (int) strlen(xi);
            SET_STRING_ELT (s, is, Rf_mkCharRep (xi, nc, ni, getCharCE(el)));
        }
        ix = ++ix == nx ? 0 : ix;
        in = ++in == nn ? 0 : in;
    }

    /* Copy names if not recycled. */

    if (ns == nx && (d = getAttrib(x,R_NamesSymbol)) != R_NilValue)
        setAttrib(s, R_NamesSymbol, d);

    UNPROTECT(1);
    return s;
}

/* FUNTAB entries defined in this source file. See names.c for documentation. */

attribute_hidden FUNTAB R_FunTab_character[] =
{
/* printname	c-entry		offset	eval	arity	pp-kind	     precedence	rightassoc */

{"nzchar",	do_nzchar,	1,	1,	1,	{PP_FUNCALL, PREC_FN,	0}},
{"nchar",	do_nchar,	1,   1000011,	3,	{PP_FUNCALL, PREC_FN,	0}},
{"substr",	do_substr,	1,   1000011,	3,	{PP_FUNCALL, PREC_FN,	0}},
{"substr<-",	do_substrgets,	1,   1000011,	4,	{PP_FUNCALL, PREC_FN,	0}},
{"abbreviate",	do_abbrev,	1,   1000011,	3,	{PP_FUNCALL, PREC_FN,	0}},
{"make.names",	do_makenames,	0,   1000011,	3,	{PP_FUNCALL, PREC_FN,	0}},
{"tolower",	do_tolower,	0,   1000011,	1,	{PP_FUNCALL, PREC_FN,	0}},
{"toupper",	do_tolower,	1,   1000011,	1,	{PP_FUNCALL, PREC_FN,	0}},
{"chartr",	do_chartr,	1,   1000011,	3,	{PP_FUNCALL, PREC_FN,	0}},
{"strtrim",	do_strtrim,	0,   1000011,	2,	{PP_FUNCALL, PREC_FN,	0}},
{"strtoi",	do_strtoi,	0,   1000011,	2,	{PP_FUNCALL, PREC_FN,	0}},
{"strrep",	do_strrep,	0,   1000011,	2,	{PP_FUNCALL, PREC_FN,	0}},
{"startsWith",	do_startsWith,  0,        11,   2,      {PP_FUNCALL, PREC_FN,    0}},
{"endsWith",	do_startsWith,  1,        11,   2,      {PP_FUNCALL, PREC_FN,    0}},

{NULL,		NULL,		0,	0,	0,	{PP_INVALID, PREC_FN,	0}}
};
