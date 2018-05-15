/*
 *  pqR : A Computer Language for Statistical Data Analysis
 *  Copyright (C) 2013, 2014, 2015 Radford M. Neal
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

#ifndef HELPERS_APP_H_
#define HELPERS_APP_H_

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <Defn.h>


/* Defn.h defines the macros helpers_mark_in_use, helpers_mark_being_computed,
   helpers_mark_not_in_use, helpers_mark_not_being_computed, helpers_is_in_use,
   and helpers_is_being_computed. */

#undef helpers_wait_until_not_being_computed  /* May've been def'd in Defn.h */
#undef helpers_wait_until_not_being_computed2 /* so helpers.h not always req */


/* HELPER THREAD INITIALIZATION.  Needed only for MS Windows.  Sets
   the rounding mode in each helper thread, to allow long double
   arithmetic.  It seems this is not inherited from the master thread
   (at least in Windows 7 32-bit, using the Rtools215 compiler).  Also
   sets things to ignore cntrl-C. */

#ifdef Win32
#define helpers_helper_init() do { \
    extern void no_ctrl_C(void); \
    no_ctrl_C(); \
    __asm__("fninit"); \
} while (0)
#endif


/* MAXIMUM NUMBER OF TASKS THAT CAN BE OUTSTANDING.  Must be a power of two
   minus one, and no more than 255 (to fit in an unsigned char).  A lower
   value may be desirable to prevent large numbers of outstanding tasks
   when some values are computed but never used. */

#define MAX_TASKS 15


/* TYPES DEFINED BY THE APPLICATION. */

#include <stdint.h>

typedef uint_least32_t helpers_size_t; /* "least" so more likely to be atomic */
typedef uint_fast64_t helpers_op_t;    /* "fast" since no reason shouldn't be */

typedef SEXP helpers_var_ptr;


/* MACROS FOR TASK MERGING.  They are defined here only if R_TASK_MERGING is 
   defined and HELPERS_DISABLED is not. */

#ifndef HELPERS_DISABLED
#ifdef R_TASK_MERGING

#define MAX_OPS_MERGED 3      /* Must be 3 for the current procedure */

#define HELPERS_TASK_DATA_AMT MAX_OPS_MERGED

#define helpers_can_merge(out,proc_a,op_a,in1_a,in2_a,proc_b,op_b,in1_b,in2_b) \
((proc_b) == task_merged_arith_abs \
   ? ((op_b)&(0x7f<<(8*MAX_OPS_MERGED)))==0 /* not already at maximum */ \
        && (helpers_not_multithreading_now \
             || (op_a)<=TIMESOP) /* not slow and might be done in parallel */ \
   : helpers_not_multithreading_now \
             || (op_b)<=TIMESOP || (op_a)<=TIMESOP /* not both slow */ \
)

#define helpers_merge(out,proc_a,op_a,in1_a,in2_a, \
                          proc_b_ptr,op_b_ptr,in1_b_ptr,in2_b_ptr, \
                          task_data) \
  helpers_merge_proc (/*out,*/proc_a,op_a,in1_a,in2_a, \
                      proc_b_ptr,op_b_ptr,in1_b_ptr,in2_b_ptr,task_data)

#endif
#endif


/* INCLUDE HELPERS.H AFTER ABOVE TYPE DEFINITIONS. */

#include "helpers.h"


/* PROCEDURES FOR TASK MERGING.  They are declared here only if R_TASK_MERGING
   is defined and HELPERS_DISABLED is not. */

#ifndef HELPERS_DISABLED
#ifdef R_TASK_MERGING

extern helpers_task_proc task_merged_arith_abs, task_abs;

extern void helpers_merge_proc ( /* helpers_var_ptr out, */
  helpers_task_proc *proc_A, helpers_op_t op_A, 
  helpers_var_ptr in1_A, helpers_var_ptr in2_A,
  helpers_task_proc **proc_B, helpers_op_t *op_B, 
  helpers_var_ptr *in1_B, helpers_var_ptr *in2_B,
  double *task_data);

#endif
#endif


/* TRACE AND STATISTICS OUTPUT */

#define helpers_printf REprintf

#define ENABLE_TRACE 1
#define ENABLE_STATS 0


/* TASK AND VARIABLE NAMES FOR TRACE OUTPUT.  Functions references are in
   helpers-app.c. */

extern char *Rf_task_name (helpers_task_proc *);
#define helpers_task_name(t) Rf_task_name(t)

extern char *Rf_var_name (helpers_var_ptr);
#define helpers_var_name(v) Rf_var_name(v)


/* MACROS TO COMBINE TWO LENGTHS INTO AN OPERAND, AND TO EXTRACT THEM. */

#define COMBINE_LENGTHS(_a_,_b_) (((helpers_op_t)(_a_)<<32) | (_b_))
#define EXTRACT_LENGTH1(_x_) ((helpers_op_t)(_x_)>>32)
#define EXTRACT_LENGTH2(_x_) ((_x_)&0xffffffff)


/* MACROS TO DO TASK NOW OR LATER.  Looks at whether a pending result is allowed
   given the variant (pass 0 if no variant), whether the inputs are being
   computed, and a condition given as the second argument.  Also, if there is
   no multithreading, tasks are done directly unless task merging is possible
   or the inputs are being computed.

   There are two macros, for when there are two inputs or only one. */

#define DO_NOW_OR_LATER2(_variant_,_c_,_flags_,_proc_,_op_,_out_,_in1_,_in2_) \
 HELPERS_NOW_OR_LATER( (((_flags_) & HELPERS_MERGE_IN_OUT) != 0 \
                               && !helpers_not_merging_now \
                          || !helpers_not_multithreading_now) \
                        && (_variant_ & VARIANT_PENDING_OK) && (_c_), \
                       helpers_is_being_computed(_in1_) \
                          || helpers_is_being_computed(_in2_), \
                       _flags_, _proc_, _op_, _out_, _in1_, _in2_)

#define DO_NOW_OR_LATER1(_variant_,_c_,_flags_,_proc_,_op_,_out_,_in_) \
 HELPERS_NOW_OR_LATER( (((_flags_) & HELPERS_MERGE_IN_OUT) != 0 \
                                && !helpers_not_merging_now \
                          || !helpers_not_multithreading_now) \
                        && (_variant_ & VARIANT_PENDING_OK) && (_c_), \
                       helpers_is_being_computed(_in_), \
                       _flags_, _proc_, _op_, _out_, _in_, (helpers_var_ptr)0)


/* ADJUSTMENT OF THRESHOLDS FOR SCHEDULING COMPUTATIONS AS TASKS.  The 
   factor below can be adjusted to account for the overhead of scheduling
   a task, with the adjustment applying to all the thresholds set this way. */

#define THRESHOLD_ADJUST(a) ((a)*10)


#endif
