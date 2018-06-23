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


/* CODES FOR MERGED ARITHMETIC OPERATIONS AND ABS FUNCTION. .

   Relies on PLUSOP ... DIVOP being the integers from 1 to 4. 

   The opcode for the merged task procedure encodes two or more
   operations plus a flag saying which is the vector operand.  The
   flag is in the low-order byte.  The codes for the operations follow
   in higher-order bytes, with the last operation in lowest position.
   The code for the null operation is zero, so a null operation may
   naturally occur in the higher-order byte, but this is manipulated
   in task_merged_arith_abs so that it instead occurs only as the
   middle operation.  

   For the PLUS and TIMES operations, which are commutative (except
   for NaN handling on Intel processors), only the C op V forms are
   implemented (operands are swapped when scheduling a task as
   required), and the codes for V op C are used for other
   operations. */

#define MERGED_OP_NULL      0   /* No operation */

#define MERGED_OP_C_PLUS_V  (2*PLUSOP - 1)
#define MERGED_OP_ABS_V     (2*PLUSOP)      /* no V_PLUS_C */
#define MERGED_OP_C_MINUS_V (2*MINUSOP - 1)
#define MERGED_OP_V_MINUS_C (2*MINUSOP)
#define MERGED_OP_C_TIMES_V (2*TIMESOP - 1)
#define MERGED_OP_V_SQUARED (2*TIMESOP)     /* no V_TIMES_C */

#define MERGED_OP_C_DIV_V   (2*DIVOP - 1)   /* these allowed only for last op */
#define MERGED_OP_V_DIV_C   (2*DIVOP)

#define N_MERGED_OPS 9          /* Number of operation codes above */

#define MERGED_OP_W_PLUS_V  7   /* Codes for first operation; these codes */
#define MERGED_OP_W_MINUS_V 8   /*    overlap codes for the last operaton */
#define MERGED_OP_W_TIMES_V 9

#define N_MERGED_OPS_FIRST 10   /* Number of op codes for the first op,
                                   including NULL, which is not actually used */


/* TASK FOR PERFORMING A SET OF MERGED ARITHMETIC OPERATIONS.

   Works only when MAX_OPS_MERGED is 3.

   The code for the first operation (which is never MERGED_OP_NULL or
   a DIV operation) is used to select one of N_MERGED_OPS_FIRST-1
   procedures to call that are defined below.  These procedures
   contain the processing loop, and a switch on the remaining two
   operation codes.  This split into multiple procedures prevents the
   compiler (in particular gcc) from using lots of time and memory
   processing a single large switch statement. */

#if MAX_OPS_MERGED != 3
#error Merged operations are implemented only when MAX_OPS_MERGED is 3
#endif

#define OP_NULL(k)       (void)0
#define OP_C_PLUS_V(k)   v = c ## k + v
#define OP_ABS_V(k)      v = fabs(v)
#define OP_C_MINUS_V(k)  v = c ## k - v
#define OP_V_MINUS_C(k)  v = v - c ## k
#define OP_C_TIMES_V(k)  v = c ## k * v
#define OP_V_SQUARED(k)  v = v * v
#define OP_C_DIV_V(k)    v = c ## k / v
#define OP_V_DIV_C(k)    v = v / c ## k
#define OP_W_PLUS_V      v = w[i] + v
#define OP_W_MINUS_V     v = w[i] - v
#define OP_W_TIMES_V     v = w[i] * v

#if __AVX__ && !defined(DISABLE_AVX_CODE)

#include <immintrin.h>

#define MM_OP_NULL(k)       (void)0
#define MM_OP_C_PLUS_V(k)   V = _mm256_add_pd (C ## k, V)
#define MM_OP_ABS_V(k)      V = _mm256_and_pd (V, _mm256_set1_pd (signb))
#define MM_OP_C_MINUS_V(k)  V = _mm256_sub_pd (C ## k, V)
#define MM_OP_V_MINUS_C(k)  V = _mm256_sub_pd (V, C ## k)
#define MM_OP_C_TIMES_V(k)  V = _mm256_mul_pd (C ## k, V)
#define MM_OP_V_SQUARED(k)  V = _mm256_mul_pd (V, V)
#define MM_OP_C_DIV_V(k)    V = _mm256_div_pd (C ## k, V)
#define MM_OP_V_DIV_C(k)    V = _mm256_div_pd (V, C ## k)
#define MM_OP_W_PLUS_V      V = _mm256_add_pd (_mm256_load_pd(w+i), V)
#define MM_OP_W_MINUS_V     V = _mm256_sub_pd (_mm256_load_pd(w+i), V)
#define MM_OP_W_TIMES_V     V = _mm256_mul_pd (_mm256_load_pd(w+i), V)

#define LOAD_MM \
    __m256d C1 = _mm256_set1_pd(c1); \
    __m256d C2 = _mm256_set1_pd(c2); \
    __m256d C3 = _mm256_set1_pd(c3);

#define SW_CASE(o,S,S_AVX) \
    case o: \
        do { \
            R_len_t u = HELPERS_UP_TO(i,a); \
            while (((uintptr_t)(ansp+i) & 0x1f) != 0 && i <= u) { \
                double v; \
                v = vecp[i]; S; ansp[i] = v; \
                i += 1; \
            } \
            while (i <= u-3) { \
                __m256d V = _mm256_load_pd (vecp+i); \
                S_AVX; \
                _mm256_store_pd (ansp+i, V); \
                i += 4; \
            } \
            while (i <= u) { \
                double v; \
                v = vecp[i]; S; ansp[i] = v; \
                i += 1; \
            } \
            helpers_amount_out(i); \
        } while (i < a); \
        break;

#else

#define LOAD_MM

#define SW_CASE(o,S,S_AVX) \
    case o: \
        do { \
            R_len_t u = HELPERS_UP_TO(i,a); \
            do { \
                double v; \
                v = vecp[i]; S; ansp[i] = v; \
                i += 1; \
            } while (i <= u); \
            helpers_amount_out(i); \
        } while (i < a); \
        break;

#endif

/* Cases for earlier and last op; last op never NULL, but possibly a DIV. */

#define SW_CASES(o,S,S_AVX) \
  SW_CASE(o*N_MERGED_OPS+MERGED_OP_C_PLUS_V, \
             S; OP_C_PLUS_V(3), \
             S_AVX; MM_OP_C_PLUS_V(3)) \
  SW_CASE(o*N_MERGED_OPS+MERGED_OP_ABS_V,    \
             S; OP_ABS_V(3), \
             S_AVX; MM_OP_ABS_V(3)) \
  SW_CASE(o*N_MERGED_OPS+MERGED_OP_C_MINUS_V,\
             S; OP_C_MINUS_V(3), \
             S_AVX; MM_OP_C_MINUS_V(3)) \
  SW_CASE(o*N_MERGED_OPS+MERGED_OP_V_MINUS_C,\
             S; OP_V_MINUS_C(3), \
             S_AVX; MM_OP_V_MINUS_C(3)) \
  SW_CASE(o*N_MERGED_OPS+MERGED_OP_C_TIMES_V,\
             S; OP_C_TIMES_V(3), \
             S_AVX; MM_OP_C_TIMES_V(3)) \
  SW_CASE(o*N_MERGED_OPS+MERGED_OP_V_SQUARED,\
             S; OP_V_SQUARED(3), \
             S_AVX; MM_OP_V_SQUARED(3)) \
  SW_CASE(o*N_MERGED_OPS+MERGED_OP_C_DIV_V,  \
             S; OP_C_DIV_V(3), \
             S_AVX; MM_OP_C_DIV_V(3)) \
  SW_CASE(o*N_MERGED_OPS+MERGED_OP_V_DIV_C,  \
             S; OP_V_DIV_C(3), \
             S_AVX; MM_OP_V_DIV_C(3))

#define PROC(o,S,S_AVX) \
static void proc_##o (SEXP ans, double *vecp, double *w, int sw, int which, \
                      double c1, double c2, double c3) \
{ \
    LOAD_MM /* load AVX versions of c1, c2, c3, if doing AVX */ \
    double signb; uint64_t usignb = ~ ((uint64_t)1 << 63); \
    double *ansp = REAL(ans); \
    memcpy(&signb,&usignb,sizeof(double)); \
    R_len_t n = LENGTH(ans); \
    R_len_t i = 0; \
    R_len_t a; \
    HELPERS_SETUP_OUT(6); \
    while (i < n) { \
        if (which) \
            HELPERS_WAIT_IN2 (a, i, n); \
        else \
            HELPERS_WAIT_IN1 (a, i, n); \
        switch (sw) { \
        SW_CASES(MERGED_OP_NULL,     \
             S; OP_NULL(2), \
             S_AVX; MM_OP_NULL(2)) \
        SW_CASES(MERGED_OP_C_PLUS_V, \
             S; OP_C_PLUS_V(2), \
             S_AVX; MM_OP_C_PLUS_V(2)) \
        SW_CASES(MERGED_OP_ABS_V,    \
             S; OP_ABS_V(2), \
             S_AVX; MM_OP_ABS_V(2)) \
        SW_CASES(MERGED_OP_C_MINUS_V,\
             S; OP_C_MINUS_V(2), \
             S_AVX; MM_OP_C_MINUS_V(2)) \
        SW_CASES(MERGED_OP_V_MINUS_C,\
             S; OP_V_MINUS_C(2), \
             S_AVX; MM_OP_V_MINUS_C(2)) \
        SW_CASES(MERGED_OP_C_TIMES_V,\
             S; OP_C_TIMES_V(2), \
             S_AVX; MM_OP_C_TIMES_V(2)) \
        SW_CASES(MERGED_OP_V_SQUARED,\
             S; OP_V_SQUARED(2), \
             S_AVX; MM_OP_V_SQUARED(2)) \
        default: abort(); \
        } \
    } \
}

PROC(MERGED_OP_C_PLUS_V,  
    OP_C_PLUS_V(1),
    MM_OP_C_PLUS_V(1))
PROC(MERGED_OP_ABS_V,     
    OP_ABS_V(1),
    MM_OP_ABS_V(1))
PROC(MERGED_OP_C_MINUS_V, 
    OP_C_MINUS_V(1),
    MM_OP_C_MINUS_V(1))
PROC(MERGED_OP_V_MINUS_C, 
    OP_V_MINUS_C(1),
    MM_OP_V_MINUS_C(1))
PROC(MERGED_OP_C_TIMES_V, 
    OP_C_TIMES_V(1),
    MM_OP_C_TIMES_V(1))
PROC(MERGED_OP_V_SQUARED, 
    OP_V_SQUARED(1),
    MM_OP_V_SQUARED(1))
PROC(MERGED_OP_W_PLUS_V,
    OP_W_PLUS_V,
    MM_OP_W_PLUS_V)
PROC(MERGED_OP_W_MINUS_V,
    OP_W_MINUS_V,
    MM_OP_W_MINUS_V)
PROC(MERGED_OP_W_TIMES_V,
    OP_W_TIMES_V,
    MM_OP_W_TIMES_V)

static void (*proc_table[N_MERGED_OPS_FIRST])() = {
    0,
    proc_MERGED_OP_C_PLUS_V,
    proc_MERGED_OP_ABS_V,
    proc_MERGED_OP_C_MINUS_V,
    proc_MERGED_OP_V_MINUS_C,
    proc_MERGED_OP_C_TIMES_V,
    proc_MERGED_OP_V_SQUARED,
    proc_MERGED_OP_W_PLUS_V,
    proc_MERGED_OP_W_MINUS_V,
    proc_MERGED_OP_W_TIMES_V,
};

void task_merged_arith_abs (helpers_op_t code, SEXP ans, SEXP s1, SEXP s2)
{
    double *data = helpers_task_data();
    double c1 = data[2], c2 = data[1], c3 = data[0]; \
    int which = code & 1;  /* which is the main vector operand? */

    /* Set up switch values encoding the first (possibly null) operation and
       the 2nd and 3rd operations. */

    int ops = code >> 8;

    int switch1;
    int switch23;
    int op;

    switch1 = ops >> 16;
    switch23 = ops & 0xff;
    ops = (ops >> 8) & 0xff;

    if (switch1 == 0) {
        switch1 = ops;
        c1 = c2;
    }
    else {
        switch23 += ops * N_MERGED_OPS;
    }

    /* Do the operations by calling a procedure indexed by the first
       operation, which will switch on the second and third operations. */

    if (switch1 >= N_MERGED_OPS_FIRST) abort();

    if (which)
        (*proc_table[switch1]) (ans, REAL(s2), REAL(s1), switch23, 
                                which, c1, c2, c3);
    else
        (*proc_table[switch1]) (ans, REAL(s1), REAL(s2), switch23, 
                                which, c1, c2, c3);
}


/* PROCEDURE FOR MERGING ARITHMETIC AND ABS OPERATIONS.  The scalar
   operands for all merged operations are placed in the task_data block.
   The vector operand for the merged operations may be either the first 
   or second operand of the merged task procedure, with this being indicated 
   by a flag in the operation code. */

#define MERGED_BINARY_OP(proc,op,in1,in2) \
  ( op == POWOP    ? MERGED_OP_V_SQUARED : \
    LENGTH(in1)==1 ? 2*(op) - 1 : 2*(op) )

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
        task_data[2] = task_data[1];
        task_data[1] = task_data[0];
    }
    else { 
        task_data[2] = task_data[1] = 0.0;
        if (*proc_B == task_abs) {
            ops = MERGED_OP_ABS_V;
            which = 0;
        }
        else if (*proc_B == task_unary_minus) {
            ops = MERGED_OP_C_MINUS_V;
            task_data[1] = 0.0;
            which = 0;
        }
        else if (LENGTH(*in1_B) == LENGTH(*in2_B)) {  /* two vector operands */
            ops = *op_B + MERGED_OP_W_PLUS_V - 1;
            which = 1;
        }
        else { /* binary or unary arithmetic operation */
            ops = MERGED_BINARY_OP (*proc_B, *op_B, *in1_B, *in2_B);
            if (LENGTH(*in2_B) == 1) {
                task_data[1] = *REAL(*in2_B);
                which = 0;
            }
            else {
                task_data[1] = *REAL(*in1_B);
                which = 1;
            }
        }
        *proc_B = task_merged_arith_abs;
    }
  
    /* Merge operation A into other operations. */

    helpers_op_t newop;

    if (proc_A == task_abs) {
        newop = MERGED_OP_ABS_V;  
    }
    else if (proc_A == task_unary_minus) {
        newop = MERGED_OP_C_MINUS_V;
        task_data[0] = 0.0;
    }
    else { /* binary arithmetic operation on vector and scalar */
        newop = MERGED_BINARY_OP (proc_A, op_A, in1_A, in2_A);
        if (LENGTH(in2_A) == 1)
            task_data[0] = *REAL(in2_A);
        else
            task_data[0] = *REAL(in1_A);
    }

    ops = (ops << 8) | newop;
  
    /* Store the new operation specification in *op_B; *proc_B and task_data
       may have been updated above. */
  
    *op_B = (ops << 8) | which;
}

#endif
