/*
 *  pqR : A Computer Language for Statistical Data Analysis
 *  Copyright (C) 2013 Radford M. Neal
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
# include <config.h>
#endif

#include <Defn.h>

#undef helpers_wait_until_not_being_computed  /* May've been def'd in Defn.h */
#undef helpers_wait_until_not_being_computed2 /* so helpers.h not always req */


/* TYPES DEFINED BY THE APPLICATION. */

#include <stdint.h>

typedef uint_least32_t helpers_size_t; /* "least" so more likely to be atomic */
typedef uint_fast64_t helpers_op_t;    /* "fast" since no reason shouldn't be */

typedef SEXP helpers_var_ptr;


/* INCLUDE HELPERS.H AFTER ABOVE TYPE DEFINITIONS. */

#include "helpers.h"


/* TRACE OUTPUT */

#define helpers_printf Rprintf

#define ENABLE_TRACE 1
#define ENABLE_STATS 1


/* MAXIMUM NUMBER OF TASKS THAT CAN BE OUTSTANDING.  Must be a power of two
   minus one, and no more than 255 (to fit in an unsigned char).  A lower
   value may be desirable to prevent large numbers of outstanding tasks
   when some values are computed but never used. */

#define MAX_TASKS 15


/* MARKING MACROS. */

#define helpers_mark_in_use(v)             ((v)->sxpinfo.in_use = 1)
#define helpers_mark_not_in_use(v)         ((v)->sxpinfo.in_use = 0)

#define helpers_mark_being_computed(v)     ((v)->sxpinfo.being_computed = 1)
#define helpers_mark_not_being_computed(v) ((v)->sxpinfo.being_computed = 0)


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


/* MACRO TO DO TASK NOW OR LATER.  Looks at whether a pending result is allowed
   given the variant (pass 0 if no variant), whether the inputs are being
   computed, and a condition given as the second argument.  There are two forms,
   for when there are two inputs or only one. */

#define DO_NOW_OR_LATER2(_variant_,_c_,_flags_,_proc_,_op_,_out_,_in1_,_in2_) \
  HELPERS_NOW_OR_LATER( (_variant_ & VARIANT_PENDING_OK) && (_c_), \
                        IS_BEING_COMPUTED_BY_TASK(_in1_) || \
                        IS_BEING_COMPUTED_BY_TASK(_in2_), \
                        _flags_, _proc_, _op_, _out_, _in1_, _in2_)

#define DO_NOW_OR_LATER1(_variant_,_c_,_flags_,_proc_,_op_,_out_,_in_) \
  HELPERS_NOW_OR_LATER( (_variant_ & VARIANT_PENDING_OK) && (_c_), \
                        IS_BEING_COMPUTED_BY_TASK(_in_), \
                        _flags_, _proc_, _op_, _out_, _in_, NULL)


/* ADJUSTMENT OF THRESHOLDS FOR SCHEDULING COMPUTATIONS AS TASKS.  The 
   factor below can be adjusted to account for the overhead of scheduling
   a task, with the adjustment applying to all the thresholds set this way. */

#define THRESHOLD_ADJUST(a) (a*10)
