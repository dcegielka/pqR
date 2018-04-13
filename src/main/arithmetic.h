/*
 *  pqR : A pretty quick version of R
 *  Copyright (C) 2013 by Radford M. Neal
 *
 *  Based on R : A Computer Language for Statistical Data Analysis
 *  Copyright (C) 1998--2007	    The R Core Team.
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

#define R_INT_MAX INT_MAX
#define R_INT_MIN (-INT_MAX)  /* Excludes INT_MIN, which is NA_INTEGER */

SEXP do_fast_cmathfuns(SEXP, SEXP, SEXP, SEXP, int);

SEXP complex_math1(SEXP, SEXP, SEXP, SEXP);
SEXP complex_math2(SEXP, SEXP, SEXP, SEXP);
SEXP complex_unary(ARITHOP_TYPE, SEXP, SEXP);
SEXP complex_binary(ARITHOP_TYPE, SEXP, SEXP);

SEXP do_math1(SEXP, SEXP, SEXP, SEXP, int);
