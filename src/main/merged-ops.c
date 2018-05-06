/*
 *  pqR : A pretty quick version of R
 *  Copyright (C) 2013, 2018 by Radford M. Neal
 *
 *  Based on R : A Computer Language for Statistical Data Analysis
 *  Copyright (C) 1995, 1996, 1997  Robert Gentleman and Ross Ihaka
 *  Copyright (C) 1998--2011	    The R Core Team.
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

#include <complex.h>
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

#include <helpers/helpers-app.h>


/* Suppress all this if task merging isn't enabled. */

#ifdef helpers_can_merge


extern helpers_task_proc task_unary_minus, task_abs;


/* CODES FOR MERGED ARITHMETIC OPERATIONS AND ABS FUNCTION.  Note that
   PLUS and TIMES are not assumed to be commutative, since they aren't
   always when one or both operands are NaN or NA, and we want exactly
   the same result as is obtained without merging operations.

   Relies on PLUSOP ... POWOP being the integers from 1 to 5.  Code
   0x80 is abs.  Code 0 is for empty slots.  Additional operation
   codes are created internally.

   The opcode for the merged task procedure encodes two or more operations
   plus a flag saying which operand is scalar.  The flag is in the low-order
   byte.  The codes for the operations follow in higher-order bytes, with
   the last operation in lowest position.  The code for the null operation 
   is zero, so null operations occur naturally in higher-order bytes.  The
   64 bits in task operations codes could accommodate up to seven merged
   operations, but the limit for the fast procedure is three. 
*/

#define MERGED_OP_NULL      0   /* No operation, either created or empty slot */

#define MERGED_OP_C_PLUS_V  (2*PLUSOP - 1)
#define MERGED_OP_V_PLUS_C  (2*PLUSOP)
#define MERGED_OP_C_MINUS_V (2*MINUSOP - 1)
#define MERGED_OP_V_MINUS_C (2*MINUSOP)
#define MERGED_OP_C_TIMES_V (2*TIMESOP - 1)
#define MERGED_OP_V_TIMES_C (2*TIMESOP)
#define MERGED_OP_C_DIV_V   (2*DIVOP - 1)
#define MERGED_OP_V_DIV_C   (2*DIVOP)
#define MERGED_OP_C_POW_V   (2*POWOP - 1)
#define MERGED_OP_V_POW_C   (2*POWOP)

#define MERGED_OP_V_SQUARED 11  /* Created for V_POW_C when power is 2 */
#define MERGED_OP_CONSTANT 12   /* Created for V_POW_C when power is 0 */
#define MERGED_OP_ABS 13

#define N_MERGED_OPS 14         /* Number of operation codes above */


/* TASK FOR PERFORMING A SET OF MERGED ARITHMETIC OPERATIONS.

   Works only when MAX_OPS_MERGED is 3.

   Treats powers of -1, 0, 1, and 2 specially, replacing the MERGED_OP_V_POW_C
   code with another.  

   The code for the first operation (which will be MERGED_OP_NULL if
   there are less than three merged operations) will be used to select
   one of N_MERGED_OPS procedures to call that are defined below.
   These procedures contain the processing loop, and a switch on the
   remaining two operation codes.  This split into multiple procedures
   prevents the compiler (in particular gcc) from using lots of time
   and memory processing a single large switch statement. */

#if MAX_OPS_MERGED != 3
#error Merged operations are implemented only when MAX_OPS_MERGED is 3
#endif

#define SW_CASE(o,S) \
    case o: \
        do { \
            R_len_t u = HELPERS_UP_TO(i,a); \
            do { \
                v = vecp[i]; \
                S; \
                ansp[i] = v; \
            } while (++i <= u); \
            helpers_amount_out(i); \
        } while (i < a); \
        break;

#define SW_CASES(o,S) \
  SW_CASE(o*N_MERGED_OPS+MERGED_OP_NULL,      S) \
  SW_CASE(o*N_MERGED_OPS+MERGED_OP_C_PLUS_V,  S; v = c3 + v) \
  SW_CASE(o*N_MERGED_OPS+MERGED_OP_V_PLUS_C,  S; v = v + c3) \
  SW_CASE(o*N_MERGED_OPS+MERGED_OP_C_MINUS_V, S; v = c3 - v) \
  SW_CASE(o*N_MERGED_OPS+MERGED_OP_V_MINUS_C, S; v = v - c3) \
  SW_CASE(o*N_MERGED_OPS+MERGED_OP_C_TIMES_V, S; v = c3 * v) \
  SW_CASE(o*N_MERGED_OPS+MERGED_OP_V_TIMES_C, S; v = v * c3) \
  SW_CASE(o*N_MERGED_OPS+MERGED_OP_C_DIV_V,   S; v = c3 / v) \
  SW_CASE(o*N_MERGED_OPS+MERGED_OP_V_DIV_C,   S; v = v / c3) \
  SW_CASE(o*N_MERGED_OPS+MERGED_OP_C_POW_V,   S; v = R_pow(c3,v)) \
  SW_CASE(o*N_MERGED_OPS+MERGED_OP_V_POW_C,   S; v = R_pow(v,c3)) \
  SW_CASE(o*N_MERGED_OPS+MERGED_OP_V_SQUARED, S; v = v * v) \
  SW_CASE(o*N_MERGED_OPS+MERGED_OP_CONSTANT,  S; v = c3) \
  SW_CASE(o*N_MERGED_OPS+MERGED_OP_ABS,       S; v = fabs(v)) \

#define PROC(o,S) \
static void proc_##o (SEXP ans, double *vecp, int sw, int which, \
                      double c1, double c2, double c3) \
{ \
    double *ansp = REAL(ans); \
    R_len_t n = LENGTH(ans); \
    R_len_t i = 0; \
    R_len_t a; \
    double v; \
    HELPERS_SETUP_OUT(6); \
    while (i < n) { \
        if (which) \
            HELPERS_WAIT_IN2 (a, i, n); \
        else \
            HELPERS_WAIT_IN1 (a, i, n); \
        switch (sw) { \
        SW_CASES(MERGED_OP_NULL,      S) \
        SW_CASES(MERGED_OP_C_PLUS_V,  S; v = c2 + v) \
        SW_CASES(MERGED_OP_V_PLUS_C,  S; v = v + c2) \
        SW_CASES(MERGED_OP_C_MINUS_V, S; v = c2 - v) \
        SW_CASES(MERGED_OP_V_MINUS_C, S; v = v - c2) \
        SW_CASES(MERGED_OP_C_TIMES_V, S; v = c2 * v) \
        SW_CASES(MERGED_OP_V_TIMES_C, S; v = v * c2) \
        SW_CASES(MERGED_OP_C_DIV_V,   S; v = c2 / v) \
        SW_CASES(MERGED_OP_V_DIV_C,   S; v = v / c2) \
        SW_CASES(MERGED_OP_C_POW_V,   S; v = R_pow(c2,v)) \
        SW_CASES(MERGED_OP_V_POW_C,   S; v = R_pow(v,c2)) \
        SW_CASES(MERGED_OP_V_SQUARED, S; v = v * v) \
        SW_CASES(MERGED_OP_CONSTANT,  S; v  = c2) \
        SW_CASES(MERGED_OP_ABS,       S; v = fabs(v)) \
        default: abort(); \
        } \
    } \
}

PROC(MERGED_OP_NULL, ;)
PROC(MERGED_OP_C_PLUS_V,  v = c1 + v)
PROC(MERGED_OP_V_PLUS_C,  v = v + c1)
PROC(MERGED_OP_C_MINUS_V, v = c1 - v)
PROC(MERGED_OP_V_MINUS_C, v = v - c1)
PROC(MERGED_OP_C_TIMES_V, v = c1 * v)
PROC(MERGED_OP_V_TIMES_C, v = v * c1)
PROC(MERGED_OP_C_DIV_V,   v = c1 / v)
PROC(MERGED_OP_V_DIV_C,   v = v / c1)
PROC(MERGED_OP_C_POW_V,   v = R_pow(c1,v))
PROC(MERGED_OP_V_POW_C,   v = R_pow(v,c1))
PROC(MERGED_OP_V_SQUARED, v = v * v)
PROC(MERGED_OP_CONSTANT,  v = c1)
PROC(MERGED_OP_ABS,       v = fabs(v))

static void (*proc_table[N_MERGED_OPS])() = {
    proc_MERGED_OP_NULL,
    proc_MERGED_OP_C_PLUS_V,
    proc_MERGED_OP_V_PLUS_C,
    proc_MERGED_OP_C_MINUS_V,
    proc_MERGED_OP_V_MINUS_C,
    proc_MERGED_OP_C_TIMES_V,
    proc_MERGED_OP_V_TIMES_C,
    proc_MERGED_OP_C_DIV_V,
    proc_MERGED_OP_V_DIV_C,
    proc_MERGED_OP_C_POW_V,
    proc_MERGED_OP_V_POW_C,
    proc_MERGED_OP_V_SQUARED,
    proc_MERGED_OP_CONSTANT,
    proc_MERGED_OP_ABS
};

void task_merged_arith_abs (helpers_op_t code, SEXP ans, SEXP s1, SEXP s2)
{
    /* Record which is the vector operand. */

    int which = code & 1;

    /* Get vector to operate on and the pointer to scalar operands (if any). */

    double *vecp = which ? REAL(s2) : REAL(s1);
    double *scp = helpers_task_data();

    /* Set up switch values encoding the first (possibly null) operation and
       the 2nd and 3rd operations, and scalar constants used by these ops. */

    int ops = code >> 8;
    double c1, c2, c3;

    int switch1;
    int switch23;
    int op;

#   define POW_SPECIAL(op,c) do { \
        if (op == MERGED_OP_V_POW_C) { \
            if (c == -1.0)     { op = MERGED_OP_C_DIV_V; c = 1.0; } \
            else if (c == 0.0) { op = MERGED_OP_CONSTANT; c = 1.0; } \
            else if (c == 1.0) { op = MERGED_OP_NULL; } \
            else if (c == 2.0) { op = MERGED_OP_V_SQUARED; } \
        } \
    } while (0)

    op = ops>>16;
    if (op==0) {
        switch1 = MERGED_OP_NULL;
    }
    else {
        ops &= 0xffff;
        if (op & 0x80) {
            switch1 = MERGED_OP_ABS;
        }
        else {
            c1 = *scp++;
            POW_SPECIAL(op,c1);
            switch1 = op;
        }
    }

    op = ops >> 8;
    if (op & 0x80) {
        switch23 = MERGED_OP_ABS * N_MERGED_OPS;
    }
    else {
        c2 = *scp++;
        POW_SPECIAL(op,c2);
        switch23 = op * N_MERGED_OPS;
    }

    op = ops & 0xff;
    if (op & 0x80) {
        switch23 += MERGED_OP_ABS;
    }
    else {
        c3 = *scp;
        POW_SPECIAL(op,c3);
        switch23 += op;
    }

    /* Do the operations by calling a procedure indexed by the first
       operation, which will switch on the second and third operations. */

    (*proc_table[switch1]) (ans, vecp, switch23, which, c1, c2, c3);
}


/* PROCEDURE FOR MERGING ARITHMETIC AND ABS OPERATIONS.  The scalar
   operands for all merged operations are placed in the task_data block.
   The vector operand for the merged operations may be either the first 
   or second operand of the merged task procedure, with this being indicated 
   by a flag in the operation code. */

#define MERGED_ARITH_OP(proc,op,in1,in2) \
 ((proc)==task_unary_minus ? MERGED_OP_C_MINUS_V \
          : LENGTH(in1)==1 ? 2*(op) - 1 : 2*(op))

void helpers_merge_proc ( /* helpers_var_ptr out, */
  helpers_task_proc *proc_A, helpers_op_t op_A, 
  helpers_var_ptr in1_A, helpers_var_ptr in2_A,
  helpers_task_proc **proc_B, helpers_op_t *op_B, 
  helpers_var_ptr *in1_B, helpers_var_ptr *in2_B,
  double *task_data)
{
    helpers_op_t ops;
    int which;
   
    /* Set flags, which, and ops according to operations other than op_A. */
  
    if (*proc_B == task_merged_arith_abs) {
        which = *op_B & 1;
        ops = *op_B >> 8;
    }
    else { /* create "merge" of just operation B */
        if (*proc_B == task_abs) {
            which = 0;
            ops = *op_B + 0x80;  /* just 0x80 would be OK, since only abs now */
        }
        else { /* binary or unary arithmetic operation */
            ops = MERGED_ARITH_OP (*proc_B, *op_B, *in1_B, *in2_B);
            if (*in2_B == 0) { /* unary minus */
                task_data[0] = 0.0;
                which = 0;
            }
            else if (LENGTH(*in2_B) == 1) {
                task_data[0] = *REAL(*in2_B);
                which = 0;
            }
            else {
                task_data[0] = *REAL(*in1_B);
                which = 1;
            }
        }
        *proc_B = task_merged_arith_abs;
    }
  
    /* Merge operation A into other operations. */

    helpers_op_t newop;

    if (proc_A == task_abs) {
        newop = op_A | 0x80;  /* just 0x80 would be OK, since only abs now */
    }
    else { /* binary or unary arithmetic operation */
        double *p = task_data;
#       if MAX_OPS_MERGED==3
            if ((ops&0x80) == 0) p += 1;
            if ((ops>>8) != 0 && (ops&0x8000) == 0) p += 1;
#       else
            for (helpers_op_t o = ops; o != 0; o >>= 8) {
                if (! (o&0x80)) p += 1;
            }
#       endif
        *p = in2_A==0 ? 0.0 : LENGTH(in2_A)==1 ? *REAL(in2_A) : *REAL(in1_A);
        newop = MERGED_ARITH_OP (proc_A, op_A, in1_A, in2_A);
    }

    ops = (ops << 8) | newop;
  
    /* Store the new operation specification in *op_B; *proc_B and task_data
       may have been updated above. */
  
    *op_B = (ops << 8) | which;
}

#endif
