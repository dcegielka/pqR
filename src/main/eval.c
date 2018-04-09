/*
 *  pqR : A pretty quick version of R
 *  Copyright (C) 2013, 2014, 2015, 2016, 2017, 2018 by Radford M. Neal
 *
 *  Based on R : A Computer Language for Statistical Data Analysis
 *  Copyright (C) 1995, 1996	Robert Gentleman and Ross Ihaka
 *  Copyright (C) 1998--2011	The R Core Team.
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
# include <config.h>
#endif

#define USE_FAST_PROTECT_MACROS
#define R_USE_SIGNALS 1
#include <Defn.h>
#include <R_ext/Callbacks.h>
#include <Rinterface.h>
#include <Fileio.h>

#include "scalar-stack.h"
#include <Rmath.h>
#include "arithmetic.h"

#include <helpers/helpers-app.h>


#define SCALAR_STACK_DEBUG 0


/* Inline version of findFun, meant to be fast when a special symbol is found 
   in the base environmet. */

static inline SEXP FINDFUN (SEXP symbol, SEXP rho)
{
    rho = SKIP_USING_SYMBITS (rho, symbol);

    if (rho == R_GlobalEnv && BASE_CACHE(symbol)) {
        SEXP res = SYMVALUE(symbol);
        if (TYPEOF(res) == PROMSXP)
            res = PRVALUE_PENDING_OK(res);
        if (isFunction(res))
            return res;
    }

    return findFun_nospecsym(symbol,rho);
}


#define ARGUSED(x) LEVELS(x)

static SEXP Rf_builtin_op_no_cntxt (SEXP op, SEXP e, SEXP rho, int variant);

/*#define BC_PROFILING*/
#ifdef BC_PROFILING
static Rboolean bc_profiling = FALSE;
#endif

#define R_Profiling R_high_frequency_globals.Profiling

#ifdef R_PROFILING

/* BDR 2000-07-15
   Profiling is now controlled by the R function Rprof(), and should
   have negligible cost when not enabled.
*/

/* A simple mechanism for profiling R code.  When R_PROFILING is
   enabled, eval will write out the call stack every PROFSAMPLE
   microseconds using the SIGPROF handler triggered by timer signals
   from the ITIMER_PROF timer.  Since this is the same timer used by C
   profiling, the two cannot be used together.  Output is written to
   the file PROFOUTNAME.  This is a plain text file.  The first line
   of the file contains the value of PROFSAMPLE.  The remaining lines
   each give the call stack found at a sampling point with the inner
   most function first.

   To enable profiling, recompile eval.c with R_PROFILING defined.  It
   would be possible to selectively turn profiling on and off from R
   and to specify the file name from R as well, but for now I won't
   bother.

   The stack is traced by walking back along the context stack, just
   like the traceback creation in jump_to_toplevel.  One drawback of
   this approach is that it does not show BUILTIN's since they don't
   get a context.  With recent changes to pos.to.env it seems possible
   to insert a context around BUILTIN calls to that they show up in
   the trace.  Since there is a cost in establishing these contexts,
   they are only inserted when profiling is enabled. [BDR: we have since
   also added contexts for the BUILTIN calls to foreign code.]

   One possible advantage of not tracing BUILTIN's is that then
   profiling adds no cost when the timer is turned off.  This would be
   useful if we want to allow profiling to be turned on and off from
   within R.

   One thing that makes interpreting profiling output tricky is lazy
   evaluation.  When an expression f(g(x)) is profiled, lazy
   evaluation will cause g to be called inside the call to f, so it
   will appear as if g is called by f.

   L. T.  */

#ifdef Win32
# define WIN32_LEAN_AND_MEAN 1
# include <windows.h>		/* for CreateEvent, SetEvent */
# include <process.h>		/* for _beginthread, _endthread */
#else
# ifdef HAVE_SYS_TIME_H
#  include <sys/time.h>
# endif
# include <signal.h>
#endif /* not Win32 */

static FILE *R_ProfileOutfile = NULL;
static int R_Mem_Profiling=0;
extern void get_current_mem(unsigned long *,unsigned long *,unsigned long *); /* in memory.c */
extern unsigned long get_duplicate_counter(void);  /* in duplicate.c */
extern void reset_duplicate_counter(void);         /* in duplicate.c */

#ifdef Win32
HANDLE MainThread;
HANDLE ProfileEvent;

static void doprof(void)
{
    RCNTXT *cptr;
    char buf[1100];
    unsigned long bigv, smallv, nodes;
    int len;

    buf[0] = '\0';
    SuspendThread(MainThread);
    if (R_Mem_Profiling){
	    get_current_mem(&smallv, &bigv, &nodes);
	    if((len = strlen(buf)) < 1000) {
		sprintf(buf+len, ":%ld:%ld:%ld:%ld:", smallv, bigv,
		     nodes, get_duplicate_counter());
	    }
	    reset_duplicate_counter();
    }
    for (cptr = R_GlobalContext; cptr; cptr = cptr->nextcontext) {
	if ((cptr->callflag & (CTXT_FUNCTION | CTXT_BUILTIN))
	    && TYPEOF(cptr->call) == LANGSXP) {
	    SEXP fun = CAR(cptr->call);
	    if(strlen(buf) < 1000) {
		strcat(buf, TYPEOF(fun) == SYMSXP ? CHAR(PRINTNAME(fun)) :
		       "<Anonymous>");
		strcat(buf, " ");
	    }
	}
    }
    ResumeThread(MainThread);
    if(strlen(buf))
	fprintf(R_ProfileOutfile, "%s\n", buf);
}

/* Profiling thread main function */
static void __cdecl ProfileThread(void *pwait)
{
    int wait = *((int *)pwait);

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    while(WaitForSingleObject(ProfileEvent, wait) != WAIT_OBJECT_0) {
	doprof();
    }
}
#else /* not Win32 */
static void doprof(int sig)
{
    RCNTXT *cptr;
    int newline = 0;
    unsigned long bigv, smallv, nodes;
    if (R_Mem_Profiling){
	    get_current_mem(&smallv, &bigv, &nodes);
	    if (!newline) newline = 1;
	    fprintf(R_ProfileOutfile, ":%ld:%ld:%ld:%ld:", smallv, bigv,
		     nodes, get_duplicate_counter());
	    reset_duplicate_counter();
    }
    for (cptr = R_GlobalContext; cptr; cptr = cptr->nextcontext) {
	if ((cptr->callflag & (CTXT_FUNCTION | CTXT_BUILTIN))
	    && TYPEOF(cptr->call) == LANGSXP) {
	    SEXP fun = CAR(cptr->call);
	    if (!newline) newline = 1;
	    fprintf(R_ProfileOutfile, "\"%s\" ",
		    TYPEOF(fun) == SYMSXP ? CHAR(PRINTNAME(fun)) :
		    "<Anonymous>");
	}
    }
    if (newline) fprintf(R_ProfileOutfile, "\n");
    signal(SIGPROF, doprof);
}

static void doprof_null(int sig)
{
    signal(SIGPROF, doprof_null);
}
#endif /* not Win32 */


static void R_EndProfiling(void)
{
#ifdef Win32
    SetEvent(ProfileEvent);
    CloseHandle(MainThread);
#else /* not Win32 */
    struct itimerval itv;

    itv.it_interval.tv_sec = 0;
    itv.it_interval.tv_usec = 0;
    itv.it_value.tv_sec = 0;
    itv.it_value.tv_usec = 0;
    setitimer(ITIMER_PROF, &itv, NULL);
    signal(SIGPROF, doprof_null);
#endif /* not Win32 */
    if(R_ProfileOutfile) fclose(R_ProfileOutfile);
    R_ProfileOutfile = NULL;
    R_Profiling = 0;
}

static void R_InitProfiling(SEXP filename, int append, double dinterval, int mem_profiling)
{
#ifndef Win32
    struct itimerval itv;
#else
    int wait;
    HANDLE Proc = GetCurrentProcess();
#endif
    int interval;

    interval = 1e6 * dinterval + 0.5;
    if(R_ProfileOutfile != NULL) R_EndProfiling();
    R_ProfileOutfile = RC_fopen(filename, append ? "a" : "w", TRUE);
    if (R_ProfileOutfile == NULL)
	error(_("Rprof: cannot open profile file '%s'"),
	      translateChar(filename));
    if(mem_profiling)
	fprintf(R_ProfileOutfile, "memory profiling: sample.interval=%d\n", interval);
    else
	fprintf(R_ProfileOutfile, "sample.interval=%d\n", interval);

    R_Mem_Profiling=mem_profiling;
    if (mem_profiling)
	reset_duplicate_counter();

#ifdef Win32
    /* need to duplicate to make a real handle */
    DuplicateHandle(Proc, GetCurrentThread(), Proc, &MainThread,
		    0, FALSE, DUPLICATE_SAME_ACCESS);
    wait = interval/1000;
    if(!(ProfileEvent = CreateEvent(NULL, FALSE, FALSE, NULL)) ||
       (_beginthread(ProfileThread, 0, &wait) == -1))
	R_Suicide("unable to create profiling thread");
    Sleep(wait/2); /* suspend this thread to ensure that the other one starts */
#else /* not Win32 */
    signal(SIGPROF, doprof);

    itv.it_interval.tv_sec = 0;
    itv.it_interval.tv_usec = interval;
    itv.it_value.tv_sec = 0;
    itv.it_value.tv_usec = interval;
    if (setitimer(ITIMER_PROF, &itv, NULL) == -1)
	R_Suicide("setting profile timer failed");
#endif /* not Win32 */
    R_Profiling = 1;
}

static SEXP do_Rprof(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP filename;
    int append_mode, mem_profiling;
    double dinterval;

#ifdef BC_PROFILING
    if (bc_profiling) {
	warning(_("can't use R profiling while byte code profiling"));
	return R_NilValue;
    }
#endif
    checkArity(op, args);
    if (!isString(CAR(args)) || (LENGTH(CAR(args))) != 1)
	error(_("invalid '%s' argument"), "filename");
    append_mode = asLogical(CADR(args));
    dinterval = asReal(CADDR(args));
    mem_profiling = asLogical(CADDDR(args));
    filename = STRING_ELT(CAR(args), 0);
    if (LENGTH(filename))
	R_InitProfiling(filename, append_mode, dinterval, mem_profiling);
    else
	R_EndProfiling();
    return R_NilValue;
}
#else /* not R_PROFILING */
static SEXP do_Rprof(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    error(_("R profiling is not available on this system"));
}
#endif /* not R_PROFILING */

/* NEEDED: A fixup is needed in browser, because it can trap errors,
 *	and currently does not reset the limit to the right value. */

#define CHECK_STACK_BALANCE(o,s) do { \
  if (s != R_PPStackTop) check_stack_balance(o,s); \
} while (0)

void attribute_hidden check_stack_balance(SEXP op, int save)
{
    if(save == R_PPStackTop) return;
    REprintf("Warning: stack imbalance in '%s', %d then %d\n",
	     PRIMNAME(op), save, R_PPStackTop);
}


/* Wait until no value in an argument list is still being computed by a task.
   Macro version does preliminary check in-line for speed. */

#define WAIT_UNTIL_ARGUMENTS_COMPUTED(_args_) \
    do { \
        if (helpers_tasks > 0) { \
            SEXP _a_ = (_args_); \
            while (_a_ != R_NilValue) { \
                if (helpers_is_being_computed(CAR(_a_))) { \
                    wait_until_arguments_computed (_a_); \
                    break; \
                } \
                _a_ = CDR(_a_); \
            } \
        } \
    } while (0)

void attribute_hidden wait_until_arguments_computed (SEXP args)
{
    SEXP wait_for, a;

    if (helpers_tasks == 0) return;

    wait_for = R_NoObject;

    for (a = args; a != R_NilValue; a = CDR(a)) {
        SEXP this_arg = CAR(a);
        if (helpers_is_being_computed(this_arg)) {
            if (wait_for == R_NoObject)
                wait_for = this_arg;
            else {
                helpers_wait_until_not_being_computed2 (wait_for, this_arg);
                wait_for = R_NoObject;
            }
        }
    }

    if (wait_for != R_NoObject)
        helpers_wait_until_not_being_computed (wait_for);
}

/* e is protected here */
SEXP attribute_hidden forcePromiseUnbound (SEXP e, int variant)
{
    RPRSTACK prstack;
    SEXP val;

    PROTECT(e);

    if (PRSEEN(e)) PRSEEN_error_or_warning(e);

    /* Mark the promise as under evaluation and push it on a stack
       that can be used to unmark pending promises if a jump out
       of the evaluation occurs. */

    prstack.promise = e;
    prstack.next = R_PendingPromises;
    R_PendingPromises = &prstack;

    SET_PRSEEN(e, 1);

    val = EVALV (PRCODE(e), PRENV(e), 
                 (variant & VARIANT_PENDING_OK) | VARIANT_MISSING_OK);

    /* Pop the stack, unmark the promise and set its value field. */

    R_PendingPromises = prstack.next;
    SET_PRSEEN(e, 0);
    SET_PRVALUE(e, val);
    INC_NAMEDCNT(val);

    /* Attempt to mimic past behaviour... */
    if (val == R_MissingArg) {
        if ( ! (variant & VARIANT_MISSING_OK) && TYPEOF(PRCODE(e)) == SYMSXP 
                  && R_isMissing (PRCODE(e), PRENV(e)))
            arg_missing_error(PRCODE(e));
    }
    else {
        /* Set the environment to R_NilValue to allow GC to
           reclaim the promise environment (unless value is R_MissingArg);
           this is also useful for fancy games with delayedAssign() */
        SET_PRENV(e, R_NilValue);
    }

    UNPROTECT(1);

    return val;
}

SEXP forcePromise (SEXP e) /* e protected here if necessary */
{
    if (PRVALUE(e) == R_UnboundValue) {
        return forcePromiseUnbound(e,0);
    }
    else
        return PRVALUE(e);
}


/* The "evalv" function returns the value of "e" evaluated in "rho",
   with given variant.  The caller must ensure that both SEXP
   arguments are protected.  The "eval" function is just like "evalv"
   with 0 for the variant return argument.

   The "Rf_evalv2" function, if it exists, is the main part of
   "evalv", split off so that constants may be evaluated with less
   overhead within "eval" or "evalv".  It may also be used in the
   EVALV macro in Defn.h. 

   Some optional tweaks can be done here, controlled by R_EVAL_TWEAKS,
   set to decimal integer XYZ.  If XYZ is zero, no tweaks are done.
   Otherwise, the meanings are

       Z = 1      Enable and use Rf_evalv2 (also done if X or Y is non-zero)
       Y = 1      Have eval do its own prelude, rather than just calling evalv
       X = 0      Have EVALV in Defn.h just call evalv here
           1      Have EVALV do its own prelude, then call evalv2
           2      Have EVALV do its own prelude and easy symbol stuff, then
                  call evalv2
 */

SEXP Rf_evalv2(SEXP,SEXP,int);
SEXP Rf_builtin_op (SEXP op, SEXP e, SEXP rho, int variant);

#define evalcount R_high_frequency_globals.evalcount

#define EVAL_PRELUDE do { \
\
    R_variant_result = 0; \
\
    /* Evaluate constants quickly, without the overhead that's necessary when \
       the computation might be complex.  This code is repeated in evalv2 \
       for when evalcount < 0.  That way we avoid calling any procedure \
       other than evalv2 in this procedure, possibly reducing overhead \
       for constant evaluation. */ \
\
    if (SELF_EVAL(TYPEOF(e)) && --evalcount >= 0) { \
	/* Make sure constants in expressions have maximum NAMEDCNT when \
	   used as values, so they won't be modified. */ \
        SET_NAMEDCNT_MAX(e); \
        R_Visible = TRUE; \
        return e; \
    } \
} while (0)

SEXP eval(SEXP e, SEXP rho)
{
#   if (R_EVAL_TWEAKS/10)%10 == 0
        return Rf_evalv(e,rho,0);
#   else
        EVAL_PRELUDE;
        return Rf_evalv2(e,rho,0);
#   endif
}

SEXP evalv(SEXP e, SEXP rho, int variant)
{
    if (0) {
        /* THE "IF" CONDITION ABOVE IS NORMALLY 0; CAN SET TO 1 FOR DEBUGGING.
           Enabling this zeroing of variant will test that callers who normally
           get a variant result can actually handle an ordinary result. */
        variant = 0;
    }

    EVAL_PRELUDE;

#if R_EVAL_TWEAKS > 0

    return Rf_evalv2(e,rho,variant);
}

SEXP attribute_hidden Rf_evalv2(SEXP e, SEXP rho, int variant)
{

#endif

    /* Handle check for user interrupt.  When negative, repeats check for 
       SELF_EVAL which may have already been done, but not acted on since
       evalcount went negative. */

    if (--evalcount < 0) {
        R_CheckUserInterrupt();
        evalcount = 1000;
        /* Evaluate constants quickly. */
        if (SELF_EVAL(TYPEOF(e))) {
            /* Make sure constants in expressions have maximum NAMEDCNT when
               used as values, so they won't be modified. */
            SET_NAMEDCNT_MAX(e);
            R_Visible = TRUE;
            return e;
        }
    }

    SEXP op, res;

    R_EvalDepth += 1;

    if (R_EvalDepth > R_Expressions) {
        R_Expressions = R_Expressions_keep + 500;
        errorcall (R_NilValue /* avoids deparsing call in the error handler */,
         _("evaluation nested too deeply: infinite recursion / options(expressions=)?"));
    }

    R_CHECKSTACK();

#ifdef Win32
    /* This resets the precision, rounding and exception modes of a ix86 fpu. */
    __asm__ ( "fninit" );
#endif

    SEXPTYPE typeof_e;

    if (SYM_NO_DOTS(e)) {

        R_Visible = TRUE;  /* May be set FALSE by active binding / lazy eval */

        res = FIND_VAR_PENDING_OK (e, rho);

      symbol:  /* can also get here for ..1, ..2, etc., from below */

        if (TYPEOF(res) == PROMSXP) {
            if (PRVALUE_PENDING_OK(res) == R_UnboundValue)
                res = forcePromiseUnbound(res,variant);
            else
                res = PRVALUE_PENDING_OK(res);
        }
        else if (TYPEOF(res) == SYMSXP) {
            if (res == R_MissingArg) {
                if ( ! (variant & VARIANT_MISSING_OK))
                    if (!DDVAL(e))  /* revert bug fix for the moment */
                        arg_missing_error(e);
            }
            else if (res == R_UnboundValue)
                unbound_var_error(e);
        }

        /* A NAMEDCNT of 0 might arise from an inadverently missing increment
           somewhere, or from a save/load sequence (since loaded values in
           promises have NAMEDCNT of 0), so fix up here... */

        SET_NAMEDCNT_NOT_0(res);

        if ( ! (variant & VARIANT_PENDING_OK))
            WAIT_UNTIL_COMPUTED(res);
    }

    else if ((typeof_e = TYPEOF(e)) == LANGSXP) {

#       if SCALAR_STACK_DEBUG
            SEXP sv_stack = R_scalar_stack;
#       endif

        SEXP fn = CAR(e), args = CDR(e);

        if (TYPEOF(fn) == SYMSXP)
            op = FINDFUN(fn,rho);
        else
            op = eval(fn,rho);

        if (RTRACE(op)) R_trace_call(e,op);

        if (TYPEOF(op) == CLOSXP) {
            PROTECT(op);
            res = applyClosure_v (e, op, promiseArgs(args,rho), rho, 
                                  NULL, variant);
            UNPROTECT(1);
        }
        else {
            int save = R_PPStackTop;
            const void *vmax = VMAXGET();

            R_Visible = TRUE;

            if (TYPEOF(op) == SPECIALSXP)
                res = CALL_PRIMFUN (e, op, args, rho, variant);
            else if (TYPEOF(op) == BUILTINSXP)
                res = R_Profiling ? Rf_builtin_op(op, e, rho, variant)
                                  : Rf_builtin_op_no_cntxt(op, e, rho, variant);
            else
                apply_non_function_error();

            if (!R_Visible && PRIMPRINT(op) == 0)
                R_Visible = TRUE;

            CHECK_STACK_BALANCE(op, save);
            VMAXSET(vmax);
        }

#       if SCALAR_STACK_DEBUG
            if (variant & VARIANT_SCALAR_STACK_OK) {
                if (R_scalar_stack != sv_stack && (res != sv_stack 
                      || SCALAR_STACK_OFFSET(1) != sv_stack)) abort();
            }
            else {
                if (R_scalar_stack != sv_stack) abort();
            }
#       endif
    }

    else if (typeof_e == SYMSXP) {  /* Must be ... or ..1, ..2, etc. */

        if (e == R_DotsSymbol)
            dotdotdot_error();

        R_Visible = TRUE;  /* May be set FALSE by active binding / lazy eval */

        res = ddfindVar(e,rho);

        goto symbol;
    }

    else if (typeof_e == PROMSXP) {

        if (PRVALUE_PENDING_OK(e) == R_UnboundValue)
            res = forcePromiseUnbound(e,variant);
        else
            res = PRVALUE_PENDING_OK(e);

        if ( ! (variant & VARIANT_PENDING_OK))
            WAIT_UNTIL_COMPUTED(res);

        R_Visible = TRUE;
    }

    else if (typeof_e == BCODESXP) {

        res = bcEval(e, rho, TRUE);
    }

    else if (typeof_e == DOTSXP)
        dotdotdot_error();

    else
        UNIMPLEMENTED_TYPE("eval", e);

    R_EvalDepth -= 1;

#   if SCALAR_STACK_DEBUG /* Get debug output after typing SCALAR.STACK.DEBUG */
        if (installed_already("SCALAR.STACK.DEBUG") != R_NoObject) {
            if (ON_SCALAR_STACK(res)) {
                REprintf("SCALAR STACK VALUE RETURNED: %llx %llx %llx %s %f\n",
                 (long long) R_scalar_stack_start,
                 (long long) res, 
                 (long long) R_scalar_stack,
                 TYPEOF(res)==INTSXP ? "int" : "real",
                 TYPEOF(res)==INTSXP ? (double)*INTEGER(res) : *REAL(res));
            }
#           if 0
                REprintf("STACK:\n");
                for (int i = 0; i < 6; i++) {
                    if (SCALAR_STACK_ENTRY(i)==R_scalar_stack) REprintf("@@\n");
                    R_inspect(SCALAR_STACK_ENTRY(i));
                }
                REprintf("END\n");
#           endif
        }
#   endif

#   ifdef ENABLE_EVAL_DEBUG
    {
        sggc_cptr_t cptr = CPTR_FROM_SEXP(res);
        sggc_check_valid_cptr (cptr);
        if (SEXP_FROM_CPTR(cptr) != res) abort();
        if (res != R_NilValue && TYPEOF(res) == NILSXP) abort();
        if (TYPEOF(res) == FREESXP) abort();

#       ifdef ENABLE_SGGC_DEBUG
            if (sggc_trace_cptr_in_use) {
                sggc_check_valid_cptr (sggc_trace_cptr);
                SEXP trp = SEXP_FROM_CPTR (sggc_trace_cptr);
                if (trp != R_NilValue && TYPEOF(trp) == NILSXP) abort();
                if (TYPEOF(trp) == FREESXP) abort();
            }
#       endif
    }
#   endif

    return res;
}


/* Like Rf_builtin_op (defined in builtin.c) except that no context is
   created.  Making this separate from Rf_builtin_op saves on stack
   space for the local context variable.  Since the somewhat
   time-consuming context creation is not done, there is no advantage
   to evaluating a single argument with pending OK. */

static SEXP Rf_builtin_op_no_cntxt (SEXP op, SEXP e, SEXP rho, int variant)
{
    SEXP args = CDR(e);
    SEXP arg1;
    SEXP res;

    /* See if this may be a fast primitive.  All fast primitives
       should be BUILTIN.  We do a fast call only if there is exactly
       one argument, with no tag, not missing or a ... argument; also
       must not be an object if the fast primitive dispatches, unless
       the argument was evaluated with VARIANT_UNCLASS and we got this
       variant result.  The argument is stored in arg1. */

    if (args!=R_NilValue) {
        if (PRIMFUN_FAST(op) 
              && TAG(args)==R_NilValue && CDR(args)==R_NilValue
              && (arg1 = CAR(args))!=R_DotsSymbol 
              && arg1!=R_MissingArg && arg1!=R_MissingUnder) {

            PROTECT(arg1 = EVALV (arg1, rho, PRIMFUN_ARG1VAR(op)));

            if (isObject(arg1) && PRIMFUN_DSPTCH1(op)) {
                if ((PRIMFUN_ARG1VAR (op) & VARIANT_UNCLASS)
                       && (R_variant_result & VARIANT_UNCLASS_FLAG)) {
                    R_variant_result &= ~VARIANT_UNCLASS_FLAG;
                }
                else {
                    UNPROTECT(1);
                    PROTECT(args = CONS(arg1,R_NilValue));
                    goto not_fast;
                }
            }

            res = ((SEXP(*)(SEXP,SEXP,SEXP,SEXP,int)) PRIMFUN_FAST(op)) 
                     (e, op, arg1, rho, variant);

            UNPROTECT(1); /* arg1 */
            return res;
        }

        args = evalList (args, rho);
    }

    PROTECT(args);

    /* Handle a non-fast op.  We may get here after starting to handle a
       fast op, but if so, args has been set to the evaluated argument list. */

  not_fast: 

    res = CALL_PRIMFUN(e, op, args, rho, variant);

    UNPROTECT(1); /* args */
    return res;
}


attribute_hidden
void SrcrefPrompt(const char * prefix, SEXP srcref)
{
    /* If we have a valid srcref, use it */
    if (srcref && srcref != R_NilValue) {
        if (TYPEOF(srcref) == VECSXP) srcref = VECTOR_ELT(srcref, 0);
	SEXP srcfile = getAttrib00(srcref, R_SrcfileSymbol);
	if (TYPEOF(srcfile) == ENVSXP) {
	    SEXP filename = findVar(install("filename"), srcfile);
	    if (isString(filename) && length(filename)) {
	    	Rprintf(_("%s at %s#%d: "), prefix, CHAR(STRING_ELT(filename, 0)), 
	                                    asInteger(srcref));
	        return;
	    }
	}
    }
    /* default: */
    Rprintf("%s: ", prefix);
}


/* This function gets the srcref attribute from a statement block, 
   and confirms it's in the expected format */
   
static inline void getBlockSrcrefs(SEXP call, SEXP **refs, int *len)
{
    SEXP srcrefs = getAttrib00(call, R_SrcrefSymbol);
    if (TYPEOF(srcrefs) == VECSXP) {
        *refs = (SEXP *) DATAPTR(srcrefs);
        *len = LENGTH(srcrefs);
    }
    else
    {   *len = 0;
    }
}


/* This function extracts one srcref, and confirms the format.  It is 
   passed an index and the array and length from getBlockSrcrefs. */

static inline SEXP getSrcref(SEXP *refs, int len, int ind)
{
    if (ind < len) {
        SEXP result = refs[ind];
        if (TYPEOF(result) == INTSXP && LENGTH(result) >= 6)
            return result;
    }

    return R_NilValue;
}

static void printcall (SEXP call, SEXP rho)
{
    int old_bl = R_BrowseLines;
    int blines = asInteger(GetOption1(install("deparse.max.lines")));
    if (blines != NA_INTEGER && blines > 0) R_BrowseLines = blines;
    PrintValueRec(call,rho);
    R_BrowseLines = old_bl;
}

static void start_browser (SEXP call, SEXP op, SEXP stmt, SEXP env)
{
    SrcrefPrompt("debug", R_Srcref);
    PrintValue(stmt);
    do_browser(call, op, R_NilValue, env);
}

/* 'supplied' is an array of SEXP values, first a set of pairs of tag and
   value, then a pairlist of tagged values (or R_NilValue).  If NULL, no
   extras supplied. */

SEXP attribute_hidden applyClosure_v(SEXP call, SEXP op, SEXP arglist, SEXP rho,
                                     SEXP *supplied, int variant)
{
    int vrnt = VARIANT_PENDING_OK | VARIANT_DIRECT_RETURN | VARIANT_WHOLE_BODY
                 | VARIANT_PASS_ON(variant);

    if (variant & VARIANT_NOT_WHOLE_BODY)
        vrnt &= ~VARIANT_WHOLE_BODY;

    SEXP formals, actuals, savedrho, savedsrcref;
    volatile SEXP body, newrho;
    SEXP f, a, res;
    RCNTXT cntxt;

    PROTECT2(op,arglist);

    formals = FORMALS(op);
    body = BODY(op);
    savedrho = CLOENV(op);

    if (R_jit_enabled > 0 && TYPEOF(body) != BCODESXP) {
	int old_enabled = R_jit_enabled;
	SEXP newop;
	R_jit_enabled = 0;
	newop = R_cmpfun(op);
	body = BODY(newop);
	SET_BODY(op, body);
	R_jit_enabled = old_enabled;
    }

    /*  Set up a context with the call in it for use if an error occurs below
        in matchArgs or from running out of memory (eg, in NewEnvironment). */

    begincontext(&cntxt, CTXT_RETURN, call, savedrho, rho, arglist, op);
    savedsrcref = R_Srcref;  /* saved in context for longjmp, and protection */

    /*  Build a list which matches the actual (unevaluated) arguments
	to the formal paramters.  Build a new environment which
	contains the matched pairs.  Note that actuals is protected via
        newrho. */

    actuals = matchArgs(formals, NULL, 0, arglist, call);
    PROTECT(newrho = NewEnvironment(R_NilValue, actuals, savedrho));
        /* no longer passes formals, since matchArg now puts tags in actuals */

    /* This piece of code is destructively modifying the actuals list,
       which is now also the list of bindings in the frame of newrho.
       This is one place where internal structure of environment
       bindings leaks out of envir.c.  It should be rewritten
       eventually so as not to break encapsulation of the internal
       environment layout.  We can live with it for now since it only
       happens immediately after the environment creation.  LT */

    f = formals;
    a = actuals;
    while (f != R_NilValue) {
	if (MISSING(a) && CAR(f) != R_MissingArg) {
	    SETCAR(a, mkPROMISE(CAR(f), newrho));
	    SET_MISSING(a, 2);
	}
	f = CDR(f);
	a = CDR(a);
    }

    set_symbits_in_env (newrho);

    /*  Fix up any extras that were supplied by usemethod. */

    if (supplied != NULL) {
        while (TYPEOF(*supplied) == SYMSXP) {
            set_var_in_frame (*supplied, *(supplied+1), newrho, TRUE, 3);
            supplied += 2;
        }
	for (SEXP t = *supplied; t != R_NilValue; t = CDR(t)) {
	    for (a = actuals; a != R_NilValue; a = CDR(a))
		if (TAG(a) == TAG(t))
		    break;
	    if (a == R_NilValue)
		set_var_in_frame (TAG(t), CAR(t), newrho, TRUE, 3);
	}
    }

    UNPROTECT(1); /* newrho, which will be protected below via revised context*/

    /*  Change the previously-set-up context to have the correct environment.

        If we have a generic function we need to use the sysparent of
	the generic as the sysparent of the method because the method
	is a straight substitution of the generic. */

    if (R_GlobalContext->nextcontext->callflag == CTXT_GENERIC)
	revisecontext (newrho, R_GlobalContext->nextcontext->sysparent);
    else
	revisecontext (newrho, rho);

    /* Get the srcref record from the closure object */
    
    R_Srcref = getAttrib00(op, R_SrcrefSymbol);

    /* Debugging */

    if (RDEBUG(op) | RSTEP(op)) {
        SET_RDEBUG(newrho, 1);
        if (RSTEP(op)) SET_RSTEP(op, 0);
	SEXP savesrcref; SEXP *srcrefs; int len;
	/* switch to interpreted version when debugging compiled code */
	if (TYPEOF(body) == BCODESXP)
	    body = bytecodeExpr(body);
	Rprintf("debugging in: ");
        printcall(call,rho);
	savesrcref = R_Srcref;
	getBlockSrcrefs(body,&srcrefs,&len);
	PROTECT(R_Srcref = getSrcref(srcrefs,len,0));
        start_browser (call, op, body, newrho);
	R_Srcref = savesrcref;
	UNPROTECT(1);
    }

    /*  Set a longjmp target which will catch any explicit returns
	from the function body.  */

    if ((SETJMP(cntxt.cjmpbuf))) {
	if (R_ReturnedValue == R_RestartToken) {
	    cntxt.callflag = CTXT_RETURN;  /* turn restart off */
	    R_ReturnedValue = R_NilValue;  /* remove restart token */
	    PROTECT(res = evalv (body, newrho, vrnt));
	}
	else {
	    PROTECT(res = R_ReturnedValue);
        }
    }
    else {
	PROTECT(res = evalv (body, newrho, vrnt));
    }

    R_variant_result &= ~VARIANT_RTN_FLAG;

    R_Srcref = savedsrcref;
    endcontext(&cntxt);

    if ( ! (variant & VARIANT_PENDING_OK))
        WAIT_UNTIL_COMPUTED(res);

    if (RDEBUG(op)) {
	Rprintf("exiting from: ");
        printcall(call,rho);
    }

    UNPROTECT(3); /* op, arglist, res */
    return res;
}

SEXP applyClosure (SEXP call, SEXP op, SEXP arglist, SEXP rho, 
                   SEXP *supplied)
{
    if (supplied != NULL) error("Last argument to applyClosure must be NULL");
    return applyClosure_v (call, op, arglist, rho, NULL, 0);
}

/* **** FIXME: This code is factored out of applyClosure.  If we keep
   **** it we should change applyClosure to run through this routine
   **** to avoid code drift. */
static SEXP R_execClosure(SEXP call, SEXP op, SEXP arglist, SEXP rho,
			  SEXP newrho)
{
    volatile SEXP body;
    SEXP res, savedsrcref;
    RCNTXT cntxt;

    PROTECT2(op,arglist);

    body = BODY(op);

    if (R_jit_enabled > 0 && TYPEOF(body) != BCODESXP) {
	int old_enabled = R_jit_enabled;
	SEXP newop;
	R_jit_enabled = 0;
	newop = R_cmpfun(op);
	body = BODY(newop);
	SET_BODY(op, body);
	R_jit_enabled = old_enabled;
    }

    begincontext(&cntxt, CTXT_RETURN, call, newrho, rho, arglist, op);
    savedsrcref = R_Srcref;  /* saved in context for longjmp, and protection */

    /* Get the srcref record from the closure object.  Disable for now
       at least, since it's not clear that it's needed. */
    
    R_Srcref = R_NilValue;  /* was: getAttrib(op, R_SrcrefSymbol); */

    /* Debugging */

    if (RDEBUG(op) | RSTEP(op)) {
        SET_RDEBUG(newrho, 1);
        if (RSTEP(op)) SET_RSTEP(op, 0);
        SEXP savesrcref; SEXP *srcrefs; int len;
	/* switch to interpreted version when debugging compiled code */
	if (TYPEOF(body) == BCODESXP)
	    body = bytecodeExpr(body);
	Rprintf("debugging in: ");
	printcall (call, rho);
	savesrcref = R_Srcref;
	getBlockSrcrefs(body,&srcrefs,&len);
	PROTECT(R_Srcref = getSrcref(srcrefs,len,0));
        start_browser (call, op, body, newrho);
	R_Srcref = savesrcref;
	UNPROTECT(1);
    }

    /*  Set a longjmp target which will catch any explicit returns
	from the function body.  */

    if ((SETJMP(cntxt.cjmpbuf))) {
	if (R_ReturnedValue == R_RestartToken) {
	    cntxt.callflag = CTXT_RETURN;  /* turn restart off */
	    R_ReturnedValue = R_NilValue;  /* remove restart token */
	    PROTECT(res = evalv(body, newrho, VARIANT_NOT_WHOLE_BODY));
	}
	else {
	    PROTECT(res = R_ReturnedValue);
            WAIT_UNTIL_COMPUTED(res);
        }
    }
    else {
	PROTECT(res = eval(body, newrho));
    }

    R_Srcref = savedsrcref;
    endcontext(&cntxt);

    if (RDEBUG(op)) {
	Rprintf("exiting from: ");
	printcall (call, rho);
    }

    UNPROTECT(3);  /* op, arglist, res */
    return res;
}

/* **** FIXME: Temporary code to execute S4 methods in a way that
   **** preserves lexical scope. */

/* called from methods_list_dispatch.c */
SEXP R_execMethod(SEXP op, SEXP rho)
{
    SEXP call, arglist, callerenv, newrho, next, val;
    RCNTXT *cptr;

    /* create a new environment frame enclosed by the lexical
       environment of the method */
    PROTECT(newrho = Rf_NewEnvironment(R_NilValue, R_NilValue, CLOENV(op)));

    /* copy the bindings for the formal environment from the top frame
       of the internal environment of the generic call to the new
       frame.  need to make sure missingness information is preserved
       and the environments for any default expression promises are
       set to the new environment.  should move this to envir.c where
       it can be done more efficiently. */
    for (next = FORMALS(op); next != R_NilValue; next = CDR(next)) {
	SEXP symbol =  TAG(next);
	R_varloc_t loc;
	int missing;
	loc = R_findVarLocInFrame(rho,symbol);
	if (loc == R_NoObject)
	    error(_("could not find symbol \"%s\" in environment of the generic function"),
		  CHAR(PRINTNAME(symbol)));
	missing = R_GetVarLocMISSING(loc);
	val = R_GetVarLocValue(loc);
	SET_FRAME(newrho, CONS(val, FRAME(newrho)));
	SET_TAG(FRAME(newrho), symbol);
	if (missing) {
	    SET_MISSING(FRAME(newrho), missing);
	    if (TYPEOF(val) == PROMSXP && PRENV(val) == rho) {
		SEXP deflt;
		SET_PRENV(val, newrho);
		/* find the symbol in the method, copy its expression
		 * to the promise */
		for(deflt = CAR(op); deflt != R_NilValue; deflt = CDR(deflt)) {
		    if(TAG(deflt) == symbol)
			break;
		}
		if(deflt == R_NilValue)
		    error(_("symbol \"%s\" not in environment of method"),
			  CHAR(PRINTNAME(symbol)));
		SET_PRCODE(val, CAR(deflt));
	    }
	}
    }

    /* copy the bindings of the spacial dispatch variables in the top
       frame of the generic call to the new frame */
    defineVar(R_dot_defined, findVarInFrame(rho, R_dot_defined), newrho);
    defineVar(R_dot_Method, findVarInFrame(rho, R_dot_Method), newrho);
    defineVar(R_dot_target, findVarInFrame(rho, R_dot_target), newrho);

    /* copy the bindings for .Generic and .Methods.  We know (I think)
       that they are in the second frame, so we could use that. */
    defineVar(R_dot_Generic, findVar(R_dot_Generic, rho), newrho);
    defineVar(R_dot_Methods, findVar(R_dot_Methods, rho), newrho);

    /* Find the calling context.  Should be R_GlobalContext unless
       profiling has inserted a CTXT_BUILTIN frame. */
    cptr = R_GlobalContext;
    if (cptr->callflag & CTXT_BUILTIN)
	cptr = cptr->nextcontext;

    /* The calling environment should either be the environment of the
       generic, rho, or the environment of the caller of the generic,
       the current sysparent. */
    callerenv = cptr->sysparent; /* or rho? */

    /* get the rest of the stuff we need from the current context,
       execute the method, and return the result */
    call = cptr->call;
    arglist = cptr->promargs;
    val = R_execClosure(call, op, arglist, callerenv, newrho);
    UNPROTECT(1);
    return val;
}


#define BodyHasBraces(body) \
    (isLanguage(body) && CAR(body) == R_BraceSymbol)


static SEXP do_if (SEXP call, SEXP op, SEXP args, SEXP rho, int variant)
{
    SEXP Cond, Stmt;
    int absent_else = 0;

    Cond = CAR(args); args = CDR(args);
    Stmt = CAR(args); args = CDR(args);

    SEXP condval = evalv (Cond, rho, VARIANT_SCALAR_STACK_OK);
    int condlogical = asLogicalNoNA (condval, call);
    POP_IF_TOP_OF_STACK(condval);

    if (!condlogical) {
        /* go to else part */
        if (args != R_NilValue)
            Stmt = CAR(args);
        else {
            absent_else = 1;
            Stmt = R_NilValue;
        }
    }

    if (RDEBUG(rho) && Stmt!=R_NilValue && !BodyHasBraces(Stmt))
        start_browser (call, op, Stmt, rho);

    if (absent_else) {
        R_Visible = FALSE; /* case of no 'else' so return invisible NULL */
        return R_NilValue;
    }

    return evalv (Stmt, rho, VARIANT_PASS_ON(variant));
}


/* For statement.  Unevaluated arguments for different formats are as follows:

       for (i in v) body          i, v, body
       for (i down v) body        i, down=v, body
       for (i across v) body      i, across=v, body
       for (i along v) body       i, along=v, body     (ok for vec or for array)
       for (i, j along M) body    i, j, along=M, body     (requires correct dim)
       etc.

   Extra variables after i are ignored for 'in', 'down', and 'across'.

   Evaluates body with VARIANT_NULL | VARIANT_PENDING_OK.
 */

#define DO_LOOP_RDEBUG(call, op, body, rho, bgn) do { \
        if (!bgn && RDEBUG(rho)) start_browser (call, op, body, rho); \
    } while (0)

static SEXP do_for (SEXP call, SEXP op, SEXP args, SEXP rho)
{
    /* Need to declare volatile variables whose values are relied on
       after for_next or for_break longjmps and that might change between
       the setjmp and longjmp calls.  Theoretically this does not include
       n, bgn, and some others, but gcc -O2 -Wclobbered warns about some, 
       so to be safe we declare them volatile as well. */

    volatile int i, n, bgn;
    volatile SEXP val, nval;
    volatile SEXP v, bcell;                /* for use with one 'for' variable */
    volatile SEXP indexes, ixvals, bcells; /* for use with >1 'for' variables */
    int dbg, val_type;
    SEXP a, syms, sym, body, dims;
    RCNTXT cntxt;
    PROTECT_INDEX vpi, bix;
    int is_seq, seq_start;
    int along = 0, across = 0, down = 0, in = 0;
    int nsyms;
    SEXP s;
    int j;

    R_Visible = FALSE;

    /* Count how many variables there are before the argument after the "in",
       "across", "down", or "along" keyword.  Set 'a' to the cell for the 
       argument after these variables. */

    syms = args;
    nsyms = 0;
    a = args;
    do {
        if (!isSymbol(CAR(a))) errorcall(call, _("non-symbol loop variable"));
        a = CDR(a);
        nsyms += 1;
    } while (CDDR(a) != R_NilValue);

    if (TAG(a) == R_AlongSymbol)
        along = 1;
    else if (TAG(a) == R_AcrossSymbol)
        across = 1;
    else if (TAG(a) == R_DownSymbol)
        down = 1;
    else
        in = 1;  /* we treat any other (or no) tag as "in" */

    if (!along) nsyms = 1;  /* ignore extras when not 'along' */

    val = CAR(a);
    body = CADR(a);
    sym = CAR(syms);

    PROTECT2(args,rho);

    PROTECT(val = evalv(val, rho, in    ? VARIANT_SEQ | VARIANT_ANY_ATTR :
                                  along ? VARIANT_UNCLASS | VARIANT_ANY_ATTR :
                                    VARIANT_UNCLASS | VARIANT_ANY_ATTR_EX_DIM));
    dims = R_NilValue;

    is_seq = 0;

    if (along) { /* "along" and therefore not seq variant */
        R_variant_result = 0;
        if (nsyms == 1) { /* go along vector/pairlist (may also be an array) */
            is_seq = 1;
            seq_start = 1;
            val_type = INTSXP;
        }
        else { /* "along" for array, with several dimensions */
            dims = getAttrib (val, R_DimSymbol);
            if (length(dims) != nsyms)
                errorcall (call, _("incorrect number of dimensions"));
            PROTECT(dims);
            INC_NAMEDCNT(dims);
            PROTECT(indexes = allocVector(INTSXP,nsyms));
            INTEGER(indexes)[0] = 0; /* so will be 1 after first increment */
            for (int j = 1; j < nsyms; j++) 
                INTEGER(indexes)[j] = 1;
            PROTECT(ixvals = allocVector(VECSXP,nsyms));
            PROTECT(bcells = allocVector(VECSXP,nsyms));
        }
        n = length(val);
    }
    else if (across || down) { /* "across" or "down", hence not seq variant*/
        R_variant_result = 0;
        is_seq = 1;
        seq_start = 1;
        val_type = INTSXP;
        dims = getAttrib (val, R_DimSymbol);
        if (TYPEOF(dims)!=INTSXP || LENGTH(dims)==0) /* no valid dim attribute*/
            n = length(val);
        else if (down)
            n = INTEGER(dims)[0];
        else /* across */
            n = LENGTH(dims) > 1 ? INTEGER(dims)[1] : INTEGER(dims)[0];
    }
    else if (R_variant_result) {  /* variant "in" value */
        R_variant_result = 0;
        is_seq = 1;
        seq_start = R_variant_seq_spec >> 32;
        n = (R_variant_seq_spec >> 1) & 0x7fffffff;
        val_type = INTSXP;
    }
    else { /* non-variant "in" value */

        INC_NAMEDCNT(val);  /* increment NAMEDCNT to avoid mods by loop code */
        nval = val;  /* for scanning pairlist */

        /* Deal with the case where we are iterating over a factor.
           We need to coerce to character, then iterate */

        if (inherits_CHAR (val, R_factor_CHARSXP)) {
            val = asCharacterFactor(val);
            UNPROTECT(1);
            PROTECT(val);
        }

        n = length(val);
        val_type = TYPEOF(val);
    }

    /* If no iterations, just set all variable(s) to R_NilValue or the 
       sizes of the dimensions and then return. */

    if (n == 0) {
        if (nsyms == 1)
            set_var_in_frame (sym, in ? R_NilValue : ScalarIntegerMaybeConst(0),
                              rho, TRUE, 3);
        else {
            int i;
            for (i = 0; i < nsyms; i++) {
                set_var_in_frame (CAR(syms), 
                                  ScalarIntegerMaybeConst(INTEGER(dims)[i]),
                                  rho, TRUE, 3);
                syms = CDR(syms);
            }
        }
        if (nsyms != 1)
            UNPROTECT(4);  /* dims, indexes, ixvals, bcells */
        if (in && !is_seq)
            DEC_NAMEDCNT(val);
        UNPROTECT(3);      /* args, rho, val */
        R_Visible = FALSE;
        return R_NilValue;
    }

    /* Initialize record of binding cells for variables. */

    if (nsyms == 1) { 
        PROTECT_WITH_INDEX (bcell = Rf_find_binding_in_frame (rho, sym, NULL),
                            &bix);
        PROTECT_WITH_INDEX (v = CAR(bcell), &vpi);
    }
    else { 
        for (j = 0, s = syms; j < nsyms; j++, s = CDR(s)) {
            bcell = Rf_find_binding_in_frame (rho, CAR(s), NULL);
            SET_VECTOR_ELT (bcells, j, bcell);
            SET_VECTOR_ELT (ixvals, j, CAR(bcell));
        }
    }

    dbg = RDEBUG(rho);
    bgn = BodyHasBraces(body);

    begincontext(&cntxt, CTXT_LOOP, R_NilValue, rho, R_BaseEnv, R_NilValue,
		 R_NilValue);

    switch (SETJMP(cntxt.cjmpbuf)) {
    case CTXT_BREAK: goto for_break;
    case CTXT_NEXT: goto for_next;
    }

    /* MAIN LOOP. */

    for (i = 0; i < n; i++) {

        /* Handle multi-dimensional "along". */

        if (nsyms > 1) {

            /* Increment to next combination of indexes. */

            for (j = 0; INTEGER(indexes)[j] == INTEGER(dims)[j]; j++) {
                if (j == nsyms-1) abort();
                INTEGER(indexes)[j] = 1;
            }
            INTEGER(indexes)[j] += 1;

            /* Make sure all 'for' variables are set to the right index,
               using records of the binding cells used for speed. */

            for (j = 0, s = syms; j < nsyms; j++, s = CDR(s)) {
                SEXP v = VECTOR_ELT(ixvals,j);
                if (TYPEOF(v) != INTSXP || LENGTH(v) != 1 || HAS_ATTRIB(v)
                                        || NAMEDCNT_GT_1(v)) {
                    v = allocVector(INTSXP,1);
                    SET_VECTOR_ELT(ixvals,j,v);
                }
                INTEGER(v)[0] = INTEGER(indexes)[j];
                SEXP bcell = VECTOR_ELT(bcells,j);
                if (bcell == R_NilValue || CAR(bcell) != v) {
                    set_var_in_frame (CAR(s), v, rho, TRUE, 3);
                    SET_VECTOR_ELT(bcells,j,R_binding_cell);
                }
            }
            
            goto do_iter;
        }

        /* Handle "in", "across", "down", and univariate "along". */

	switch (val_type) {

	case EXPRSXP:
	case VECSXP:
	    v = VECTOR_ELT(val, i);
	    SET_NAMEDCNT_MAX(v); /* maybe unnecessary? */
	    break;

	case LISTSXP:
	    v = CAR(nval);
	    nval = CDR(nval);
	    SET_NAMEDCNT_MAX(v);
	    break;

	default:

            /* Allocate new space for the loop variable value when the value has
               been assigned to another variable (NAMEDCNT(v) > 1), or when an
               attribute has been attached to it, etc. */

            if (TYPEOF(v) != val_type || LENGTH(v) != 1 || HAS_ATTRIB(v)
                                      || NAMEDCNT_GT_1(v))
                REPROTECT(v = allocVector(val_type, 1), vpi);

            switch (val_type) {
            case LGLSXP:
                LOGICAL(v)[0] = LOGICAL(val)[i];
                break;
            case INTSXP:
                INTEGER(v)[0] = is_seq ? seq_start + i : INTEGER(val)[i];
                break;
            case REALSXP:
                REAL(v)[0] = REAL(val)[i];
                break;
            case CPLXSXP:
                COMPLEX(v)[0] = COMPLEX(val)[i];
                break;
            case STRSXP:
                SET_STRING_ELT(v, 0, STRING_ELT(val, i));
                break;
            case RAWSXP:
                RAW(v)[0] = RAW(val)[i];
                break;
            default:
                errorcall(call, _("invalid for() loop sequence"));
            }

            break;
        }

        if (bcell == R_NilValue || CAR(bcell) != v) {
            set_var_in_frame (sym, v, rho, TRUE, 3);
            REPROTECT(bcell = R_binding_cell, bix);
        }

    do_iter: ;

        DO_LOOP_RDEBUG(call, op, body, rho, bgn);

        evalv (body, rho, VARIANT_NULL | VARIANT_PENDING_OK);

    for_next: ;  /* semi-colon needed for attaching label */
    }

 for_break:
    endcontext(&cntxt);
    if (in && !is_seq)
        DEC_NAMEDCNT(val);
    if (nsyms == 1)
        UNPROTECT(2);  /* v, bcell */
    else 
        UNPROTECT(4);  /* dims, indexes, ixvals, bcells */
    UNPROTECT(3);      /* val, rho, args */
    SET_RDEBUG(rho, dbg);

    R_Visible = FALSE;
    return R_NilValue;
}


/* While statement.  Evaluates body with VARIANT_NULL | VARIANT_PENDING_OK. */

static SEXP do_while(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    int dbg;
    volatile int bgn;
    volatile SEXP body;
    RCNTXT cntxt;

    R_Visible = FALSE;

    dbg = RDEBUG(rho);
    body = CADR(args);
    bgn = BodyHasBraces(body);

    begincontext(&cntxt, CTXT_LOOP, R_NilValue, rho, R_BaseEnv, R_NilValue,
		 R_NilValue);

    if (SETJMP(cntxt.cjmpbuf) != CTXT_BREAK) { /* <- back here for "next" */
        for (;;) {
            SEXP condval = evalv (CAR(args), rho, VARIANT_SCALAR_STACK_OK);
            int condlogical = asLogicalNoNA (condval, call);
            POP_IF_TOP_OF_STACK(condval);
            if (!condlogical) 
                break;
	    DO_LOOP_RDEBUG(call, op, body, rho, bgn);
	    evalv (body, rho, VARIANT_NULL | VARIANT_PENDING_OK);
	}
    }

    endcontext(&cntxt);
    SET_RDEBUG(rho, dbg);

    R_Visible = FALSE;
    return R_NilValue;
}


/* Repeat statement.  Evaluates body with VARIANT_NULL | VARIANT_PENDING_OK. */

static SEXP do_repeat(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    int dbg;
    volatile int bgn;
    volatile SEXP body;
    RCNTXT cntxt;

    R_Visible = FALSE;

    dbg = RDEBUG(rho);
    body = CAR(args);
    bgn = BodyHasBraces(body);

    begincontext(&cntxt, CTXT_LOOP, R_NilValue, rho, R_BaseEnv, R_NilValue,
		 R_NilValue);

    if (SETJMP(cntxt.cjmpbuf) != CTXT_BREAK) { /* <- back here for "next" */
	for (;;) {
	    DO_LOOP_RDEBUG(call, op, body, rho, bgn);
	    evalv (body, rho, VARIANT_NULL | VARIANT_PENDING_OK);
	}
    }

    endcontext(&cntxt);
    SET_RDEBUG(rho, dbg);

    R_Visible = FALSE;
    return R_NilValue;
}


static R_NORETURN SEXP do_break(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    findcontext(PRIMVAL(op), rho, R_NilValue);
}

/* Parens are now a SPECIAL, to avoid overhead of creating an arg list. 
   Also avoids overhead of calling checkArity when there is no error.  
   Care is taken to allow (...) when ... is bound to exactly one argument, 
   though it is debatable whether this should be considered valid. 

   The eval variant requested is passed on to the inner expression. 

   EVALV is not used because expr in parens is unlikely to be const or symbol.*/

static SEXP do_paren (SEXP call, SEXP op, SEXP args, SEXP rho, int variant)
{
    if (args!=R_NilValue && CAR(args)==R_DotsSymbol && CDR(args)==R_NilValue) {
        args = findVar(R_DotsSymbol, rho);
        if (TYPEOF(args) != DOTSXP)
            args = R_NilValue;
    }

    if (args == R_NilValue || CDR(args) != R_NilValue)
        checkArity(op, args);

    return evalv (CAR(args), rho, VARIANT_PASS_ON(variant));
}

/* Curly brackets.  Passes on the eval variant to the last expression.  For
   earlier expressions, passes either VARIANT_NULL | VARIANT_PENDING_OK or
   the variant passed OR'd with those, if the variant passed includes
   VARIANT_DIRECT_RETURN. */

static SEXP do_begin (SEXP call, SEXP op, SEXP args, SEXP rho, int variant)
{
    if (args == R_NilValue)
        return R_NilValue;

    SEXP arg, s;
    SEXP savedsrcref = R_Srcref;
    int len;
    SEXP *srcrefs;

    getBlockSrcrefs(call,&srcrefs,&len);

    int vrnt = VARIANT_NULL | VARIANT_PENDING_OK;
    variant = VARIANT_PASS_ON(variant);
    if (variant & VARIANT_DIRECT_RETURN) 
        vrnt |= variant;

    for (int i = 1; ; i++) {
        arg = CAR(args);
        args = CDR(args);
        R_Srcref = getSrcref (srcrefs, len, i);
        if (RDEBUG(rho))
            start_browser (call, op, arg, rho);
        if (args == R_NilValue)
            break;
        s = evalv (arg, rho, vrnt);
        if (R_variant_result & VARIANT_RTN_FLAG) {
            R_Srcref = savedsrcref;
            return s;
        }
    }

    s = EVALV (arg, rho, variant);
    R_Srcref = savedsrcref;
    return s;
}


static SEXP do_return(SEXP call, SEXP op, SEXP args, SEXP rho, int variant)
{
    SEXP v;

    if (args == R_NilValue) /* zero arguments provided */
	v = R_NilValue;
    else if (CDR(args) == R_NilValue) /* one argument */
	v = EVALV (CAR(args), rho, ! (variant & VARIANT_DIRECT_RETURN) ? 0
                    : VARIANT_PASS_ON(variant) & ~ VARIANT_NULL);
    else
	errorcall(call, _("multi-argument returns are not permitted"));

    if (variant & VARIANT_DIRECT_RETURN) {
        R_variant_result |= VARIANT_RTN_FLAG;
        return v;
    }

    findcontext(CTXT_BROWSER | CTXT_FUNCTION, rho, v);
}

/* Declared with a variable number of args in names.c */
static SEXP do_function(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP rval, srcref;

    /* The following is as in 2.15.0, but it's not clear how it can happen. */
    if (TYPEOF(op) == PROMSXP) {
	op = forcePromise(op);
	SET_NAMEDCNT_MAX(op);
    }

    CheckFormals(CAR(args));
    rval = mkCLOSXP(CAR(args), CADR(args), rho);
    srcref = CADDR(args);
    if (srcref != R_NilValue) 
        setAttrib(rval, R_SrcrefSymbol, srcref);
    return rval;
}

#define ASSIGNBUFSIZ 32
static SEXP installAssignFcnName(SEXP fun)
{
    /* Handle "[", "[[", and "$" specially for speed. */

    if (fun == R_BracketSymbol)
       return R_SubAssignSymbol;

    if (fun == R_Bracket2Symbol)
        return R_SubSubAssignSymbol;

    if (fun == R_DollarSymbol)
        return R_DollarAssignSymbol;

    /* The general case for a symbol */

    if (TYPEOF(fun) == SYMSXP) {

        char buf[ASSIGNBUFSIZ];
        const char *fname = CHAR(PRINTNAME(fun));

        if (!copy_2_strings (buf, sizeof buf, fname, "<-"))
            error(_("overlong name in '%s'"), fname);

        return install(buf);
    }

    /* Handle foo::bar and foo:::bar. */

    if (TYPEOF(fun)==LANGSXP && length(fun)==3 && TYPEOF(CADDR(fun))==SYMSXP
      && (CAR(fun)==R_DoubleColonSymbol || CAR(fun)==R_TripleColonSymbol))
        return lang3 (CAR(fun), CADR(fun), installAssignFcnName(CADDR(fun)));

    error(_("invalid function in complex assignment"));
}

/* arguments of replaceCall must be protected by the caller. */

static SEXP replaceCall(SEXP fun, SEXP varval, SEXP args, SEXP rhs)
{
    SEXP value, first;

    first = value = cons_with_tag (rhs, R_NilValue, R_ValueSymbol);

    if (args != R_NilValue) {

        first = cons_with_tag (CAR(args), value, TAG(args));

        SEXP p = CDR(args);
        if (p != R_NilValue) {
            PROTECT(first);
            SEXP q = first;
            do { 
                SETCDR (q, cons_with_tag (CAR(p), value, TAG(p)));
                q = CDR(q);
                p = CDR(p);
            } while (p != R_NilValue);
            UNPROTECT(1);
        }
    }

    first = LCONS (fun, CONS(varval, first));

    return first;
}


/* Macro used in promiseArgs and promiseArgsTwo. */

#define MAKE_PROMISE(a,rho) do { \
    if (TYPEOF(a) == PROMSXP) { \
        INC_NAMEDCNT(a); \
        SEXP p = PRVALUE_PENDING_OK(a); \
        if (p != R_UnboundValue && NAMEDCNT_GT_0(p)) \
            INC_NAMEDCNT(p); \
    } \
    else if (a != R_MissingArg && a != R_MissingUnder) \
        a = mkPROMISE (a, rho); \
} while (0)


/* Create two lists of promises to evaluate each argument, with promises
   shared.  When an argument is evaluated in a call using one argument list,
   its value is then known without re-evaluation in a second call using 
   the second argument list.  The argument lists are terminated with the
   initial values of *a1 and *a2. */

static void promiseArgsTwo (SEXP el, SEXP rho, SEXP *a1, SEXP *a2)
{
    /* Handle 0 or 1 arguments (not ...) specially, for speed. */

    if (CDR(el) == R_NilValue) {  /* Note that CDR(R_NilValue) == R_NilValue */
        if (el == R_NilValue)
            return;
        SEXP a = CAR(el);
        SEXP t = TAG(el);
        if (a != R_DotsSymbol) {
            MAKE_PROMISE(a,rho);
            PROTECT (*a1 = cons_with_tag (a, *a1, t));
            *a2 = cons_with_tag (a, *a2, t);
            UNPROTECT(1);
            return;
        }
    }

    /* The general case (except el == R_NilValue handled above). */

    BEGIN_PROTECT6 (head1, tail1, head2, tail2, ev, h);

    head1 = head2 = R_NilValue;

    do {  /* el won't be R_NilValue, so we loop at least once */

        SEXP a = CAR(el);

	/* If we have a ... symbol, we look to see what it is bound to.
	   If its binding is R_NilValue we just ignore it.  If it is bound
           to a ... list of promises, we repromise all the promises and 
           then splice the list of resulting values into the return value.
	   Anything else bound to a ... symbol is an error. */

	if (a == R_DotsSymbol) {
	    h = findVar(a, rho);
            if (h == R_NilValue) {
                /* nothing */
            }
	    else if (TYPEOF(h) == DOTSXP) {
		while (h != R_NilValue) {
                    a = CAR(h);
                    MAKE_PROMISE(a,rho);
                    INC_NAMEDCNT(a);
                    ev = cons_with_tag (a, R_NilValue, TAG(h));
                    if (head1==R_NilValue)
                        head1 = ev;
                    else
                        SETCDR(tail1,ev);
                    tail1 = ev;
                    ev = cons_with_tag (a, R_NilValue, TAG(h));
                    if (head2==R_NilValue)
                        head2 = ev;
                    else
                        SETCDR(tail2,ev);
                    tail2 = ev;
		    h = CDR(h);
		}
	    }
	    else if (h != R_MissingArg)
		dotdotdot_error();
	}
        else {
            MAKE_PROMISE(a,rho);
            INC_NAMEDCNT(a);
            ev = cons_with_tag (a, R_NilValue, TAG(el));
            if (head1 == R_NilValue)
                head1 = ev;
            else
                SETCDR(tail1, ev);
            tail1 = ev;
            ev = cons_with_tag (a, R_NilValue, TAG(el));
            if (head2 == R_NilValue)
                head2 = ev;
            else
                SETCDR(tail2, ev);
            tail2 = ev;
        }

	el = CDR(el);

    } while (el != R_NilValue);

    if (head1 != R_NilValue) {
        if (*a1 != R_NilValue)
            SETCDR(tail1,*a1);
        *a1 = head1;
        if (*a2 != R_NilValue)
            SETCDR(tail2,*a2);
        *a2 = head2;
    }

    END_PROTECT;
}


/*  Assignment in its various forms. */

SEXP Rf_set_subassign (SEXP call, SEXP lhs, SEXP rhs, SEXP rho, 
                       int variant, int opval);

static SEXP do_set (SEXP call, SEXP op, SEXP args, SEXP rho, int variant)
{
    SEXP a;

    if ((a = CDR(args)) == R_NilValue /* includes case of args == R_NilValue */
          || CDR(a) != R_NilValue)
        checkArity(op,args);

    SEXP lhs = CAR(args), rhs = CAR(a);
    int opval = PRIMVAL(op);

    /* Swap operands for -> and ->>. */

    if (opval >= 10) {
        rhs = lhs;
        lhs = CAR(a);
        opval -= 10;
    }

    /* Convert lhs string to a symbol. */

    if (TYPEOF(lhs) == STRSXP) {
        lhs = install(translateChar(STRING_ELT(lhs, 0)));
    }

    if (TYPEOF(lhs) == SYMSXP) {

        /* -- ASSIGNMENT TO A SIMPLE VARIABLE -- */

        /* Handle <<- without trying the optimizations done below. */

        if (opval == 2) {
            rhs = evalv (rhs, rho, VARIANT_PENDING_OK);
            set_var_nonlocal (lhs, rhs, ENCLOS(rho), 3);
            goto done;
        }

        /* Handle assignment into base and user database environments without
           any special optimizations. */

        if (IS_BASE(rho) || IS_USER_DATABASE(rho)) {
            rhs = evalv (rhs, rho, VARIANT_PENDING_OK);
            set_var_in_frame (lhs, rhs, rho, TRUE, 3);
            goto done;
        }

        /* We decide whether we'll ask the right hand side evalutation to do
           the assignment, for statements like v<-exp(v), v<-v+1, or v<-2*v. */

        int local_assign = 0;

        if (TYPEOF(rhs) == LANGSXP) {
            if (CADR(rhs) == lhs) 
                local_assign = VARIANT_LOCAL_ASSIGN1;
            else if (CADDR(rhs) == lhs)
                local_assign = VARIANT_LOCAL_ASSIGN2;
        }

        /* Evaluate the right hand side, asking for it on the scalar stack. */

        rhs = EVALV (rhs, rho, 
               local_assign | VARIANT_PENDING_OK | VARIANT_SCALAR_STACK_OK);

        /* See if the assignment was done by the rhs operator. */

        if (R_variant_result) {
            R_variant_result = 0;
            goto done;
        }

        /* Try to copy the value, not assign the object, if the rhs is
           scalar and doesn't have zero NAMEDCNT (for which assignment
           would be free).  This will copy from the scalar stack,
           which must be replaced by a regular value if the copy can't
           be done.  If the copy can't be done, but a binding cell was
           found here, the assignment is done directly into the binding 
           cell, avoiding overhead of calling set_var_in_frame.

           Avoid accessing NAMEDCNT in a way that will cause unnecessary waits
           for task completion. */

        if (isVectorNonpointer(rhs) && LENGTH(rhs) == 1 && NAMEDCNT_GT_0(rhs)) {
            SEXPTYPE rhs_type = TYPEOF(rhs);
            SEXP v;
            if (SEXP32_FROM_SEXP(rho) != LASTSYMENV(lhs) 
                  || BINDING_IS_LOCKED((R_binding_cell = LASTSYMBINDING(lhs)))
                  || (v = CAR(R_binding_cell)) == R_UnboundValue)
                v = findVarInFrame3_nolast (rho, lhs, 7);
            if (v != R_UnboundValue && TYPEOF(v) == rhs_type && LENGTH(v) == 1
                 && ATTRIB(v) == ATTRIB(rhs) && TRUELENGTH(v) == TRUELENGTH(rhs)
                 && LEVELS(v) == LEVELS(rhs) && !NAMEDCNT_GT_1(v)) {
                SET_NAMEDCNT_NOT_0(v);
                POP_IF_TOP_OF_STACK(rhs);
                helpers_wait_until_not_in_use(v);
                WAIT_UNTIL_COMPUTED(v);
                switch (rhs_type) {
                case LGLSXP:  *LOGICAL(v) = *LOGICAL(rhs); break;
                case INTSXP:  *INTEGER(v) = *INTEGER(rhs); break;
                case REALSXP: *REAL(v)    = *REAL(rhs);    break;
                case CPLXSXP: *COMPLEX(v) = *COMPLEX(rhs); break;
                case RAWSXP:  *RAW(v)     = *RAW(rhs);     break;
                }
                rhs = v; /* for return value */
                goto done;
            }
            if (POP_IF_TOP_OF_STACK(rhs))
                rhs = DUP_STACK_VALUE(rhs);
            if (R_binding_cell != R_NilValue) {
                DEC_NAMEDCNT_AND_PRVALUE(v);
                SETCAR(R_binding_cell, rhs);
                SET_MISSING(R_binding_cell,0);
                INC_NAMEDCNT(rhs);
                if (rho == R_GlobalEnv) 
                    R_DirtyImage = 1;
                goto done;
            }
        }

        /* Assign rhs object to lhs symbol the usual way. */

        set_var_in_frame (lhs, rhs, rho, TRUE, 3);
    }

    else if (TYPEOF(lhs) == LANGSXP) {

        /* -- ASSIGNMENT TO A COMPLEX TARGET -- */

        rhs = Rf_set_subassign (call, lhs, rhs, rho, variant, opval);
    }

    else {
        errorcall (call, _("invalid assignment left-hand side"));
    }

  done:

    R_Visible = FALSE;

    if (variant & VARIANT_NULL)
        return R_NilValue;

    if ( ! (variant & VARIANT_PENDING_OK)) 
        WAIT_UNTIL_COMPUTED(rhs);
    
    return rhs;
}

/* Complex assignment.  Made a separate, non-static, function in order
   to avoid possible overhead of a large function (eg, stack frame size)
   for the simple case. */

SEXP attribute_hidden Rf_set_subassign (SEXP call, SEXP lhs, SEXP rhs, SEXP rho,
                                        int variant, int opval)
{
    SEXP var, varval, newval, rhsprom, lhsprom, e, fn;

    /* Find the variable ultimately assigned to, and its depth.
       The depth is 1 for a variable within one replacement function
       (eg, in names(a) <- ...). */

    int depth = 1;
    for (var = CADR(lhs); TYPEOF(var) != SYMSXP; var = CADR(var)) {
        if (TYPEOF(var) != LANGSXP) {
            if (TYPEOF(var) == STRSXP && LENGTH(var) == 1) {
                var = install (CHAR (STRING_ELT(var,0)));
                break;
            }
            errorcall (call, _("invalid assignment left-hand side"));
        }
        depth += 1;
    }

    /* Find the assignment function symbol for the depth 1 assignment, and
       see if we maybe (tentatively) will be using the fast interface. */

    SEXP assgnfcn = installAssignFcnName(CAR(lhs));

    int maybe_fast = assgnfcn == R_SubAssignSymbol ||
                     assgnfcn == R_DollarAssignSymbol ||
                     assgnfcn == R_SubSubAssignSymbol;

    /* We evaluate the right hand side now, asking for it on the
       scalar stack if we (tentatively) will be using the fast
       interface (unless value needed for return, and not allowed on
       scalar stack), and otherwise for pending computation. */

    SEXP rhs_uneval = rhs;  /* save unevaluated rhs */

    if (maybe_fast) {
        PROTECT(rhs = EVALV (rhs, rho, 
          (variant & (VARIANT_SCALAR_STACK_OK | VARIANT_NULL)) ? 
             VARIANT_SCALAR_STACK_OK : 0));
    }
    else
        PROTECT(rhs = EVALV (rhs, rho, VARIANT_PENDING_OK));

    /* Increment NAMEDCNT temporarily if rhs will be needed as the value,
       to protect it from being modified by the assignment, or otherwise. */

    if ( ! (variant & VARIANT_NULL))
        INC_NAMEDCNT(rhs);

    /* Get the value of the variable assigned to, and ensure it is local
       (unless this is the <<- operator).  Save and protect the binding 
       cell used. */

    if (opval == 2) /* <<- */
        varval = findVar (var, ENCLOS(rho));
    else {
        varval = findVarInFramePendingOK (rho, var);
        if (varval == R_UnboundValue && rho != R_EmptyEnv) {
            varval = findVar (var, ENCLOS(rho));
            if (varval != R_UnboundValue) {
                if (TYPEOF(varval) == PROMSXP)
                    varval = forcePromise(varval);
                set_var_in_frame (var, varval, rho, TRUE, 3);
            }
        }
    }

    SET_NAMEDCNT_NOT_0(varval); /* 0 may sometime happen - should mean 1 */

    SEXP bcell = R_binding_cell;
    PROTECT(bcell);

    if (TYPEOF(varval) == PROMSXP)
        varval = forcePromise(varval);
    if (varval == R_UnboundValue)
        unbound_var_error(var);
    if (varval == R_MissingArg)
        arg_missing_error(var);

    /* We might be able to avoid this duplication sometimes (eg, in
       a <- b <- integer(10); a[1] <- 0.5), except that some packages 
       (eg, Matrix 1.0-6) assume (in C code) that the object in a 
       replacement function is not shared. */

    if (NAMEDCNT_GT_1(varval))
        varval = dup_top_level(varval);

    PROTECT(varval);

    /* Special code for depth of 1.  This is purely an optimization - the
       general code below should also work when depth is 1. */

    if (depth == 1) {
        if (maybe_fast && !isObject(varval)
              && CADDR(lhs) != R_DotsSymbol
              && (fn = FINDFUN(assgnfcn,rho), 
                  TYPEOF(fn) == SPECIALSXP && PRIMFASTSUB(fn) && !RTRACE(fn))) {
            /* Use the fast interface.  No need to wait for rhs, since
               not evaluated with PENDING_OK */
            R_fast_sub_var = varval;
            R_fast_sub_replacement = rhs;
            R_variant_result = 0;
            newval = CALL_PRIMFUN (call, fn, CDDR(lhs), rho, 
                                   VARIANT_FAST_SUB);
            UNPROTECT(3);
        }
        else {
            if (POP_IF_TOP_OF_STACK(rhs)) 
                rhs = DUP_STACK_VALUE(rhs);
            PROTECT (rhsprom = mkValuePROMISE(rhs_uneval, rhs));
            PROTECT (lhsprom = mkValuePROMISE(CADR(lhs), varval));
            PROTECT(e = replaceCall (assgnfcn, lhsprom, CDDR(lhs), rhsprom));
            newval = eval(e,rho);
            UNPROTECT(6);
        }
    }

    else {  /* the general case, for any depth */

        SEXP v, b, op, prom, fetch_args;
        int d, fast;

        /* Structure recording information on expressions at all levels of 
           the lhs.  Level 'depth' is the ultimate variable; level 0 is the
           whole lhs expression. */

        struct { 
            SEXP expr;        /* Expression at this level */
            SEXP value;       /* Value of expr, may later change */
            SEXP store_args;  /* Arg list for store; depth 0 special, else  */
                              /*   LISTSXP or NILSXP - pairlist of promises */
                              /*   PROMSXP - promise for single argument    */
                              /*   R_NoObject - one arg from CADDR(expr)    */
            int in_top;       /* 1 or 2 if value is an unshared part of the */
                              /*   value at top level, else 0               */
        } s[depth+1];         

        /* For each level from 1 to depth, store the lhs expression at that
           level. */

        s[0].expr = lhs;
        for (v = CADR(lhs), d = 1; d < depth; v = CADR(v), d++) {
            s[d].expr = v;
        }
        s[depth].expr = var;

        /* Note: In code below, promises with the value already filled
                 in are used to 'quote' values passsed as arguments,
                 so they will not be changed when the arguments are
                 evaluated, and so deparsed error messages will have
                 the source expression.  These promises should not be
                 recycled, since they may be saved in warning messages
                 stored for later display.  */

        /* For each level except the outermost, evaluate and save the
           value of the expression as it is before the assignment.
           Also, ask if it is an unshared subset of the next larger
           expression (and all larger ones).  If it is not known to be
           part of the larger expressions, we do a top-level duplicate
           of it.

           Also, for each level except the final variable and
           outermost level, which only does a store, save argument
           lists for the fetch/store functions that are built with
           shared promises, so that they are evaluated only once.  The
           store argument list has a "value" cell at the end to fill
           in the stored value.

           For efficiency, $ and [[ are handled with VARIANT_FAST_SUB,
           and for $, no promise is created for its argument. */

        s[depth].value = varval;
        s[depth].in_top = 1;

        s[0].store_args = CDDR(lhs);  /* original args, no value cell */

        for (d = depth-1; d > 0; d--) {

            op = CAR(s[d].expr);

            fast = 0;
            if (op == R_DollarSymbol || op == R_Bracket2Symbol) {
                fn = FINDFUN (op, rho);
                fast = TYPEOF(fn)==SPECIALSXP && PRIMFASTSUB(fn) && !RTRACE(fn);
            }

            if (fast && op == R_DollarSymbol 
                     && CDDR(s[d].expr) != R_NilValue 
                     && CDR(CDDR(s[d].expr)) == R_NilValue) {
                fetch_args = CDDR(s[d].expr);
                s[d].store_args = R_NoObject;
            }
            else {
                fetch_args = promiseArgs (CDDR(s[d].expr), rho);
                if (CDR(fetch_args)==R_NilValue && TAG(fetch_args)==R_NilValue)
                    s[d].store_args = CAR(fetch_args);
                else
                    s[d].store_args = dup_arg_list (fetch_args);
            }

            PROTECT(s[d].store_args);
            PROTECT(fetch_args);

            /* We'll need this value for the subsequent replacement
               operation, so make sure it doesn't change.  Incrementing
               NAMEDCNT would be the obvious way, but if NAMEDCNT 
               was already non-zero, that leads to undesirable duplication
               later (even if the increment is later undone).  Making sure
               that NAMEDCNT isn't zero seems to be sufficient. */

            SET_NAMEDCNT_NOT_0(s[d+1].value);

            if (fast) {
                R_fast_sub_var = s[d+1].value;
                R_variant_result = 0;
                e = CALL_PRIMFUN (call, fn, fetch_args, rho, 
                      VARIANT_FAST_SUB /* implies QUERY_UNSHARED_SUBSET */);
                UNPROTECT(1);  /* fetch_args */
            }
            else {
                prom = mkValuePROMISE(s[d+1].expr,s[d+1].value);
                PROTECT (e = LCONS (op, CONS (prom, fetch_args)));
                e = evalv (e, rho, VARIANT_QUERY_UNSHARED_SUBSET);
                UNPROTECT(2);  /* e, fetch_args */
            }

            s[d].in_top = 
              s[d+1].in_top == 1 ? R_variant_result : 0;  /* 0, 1, or 2 */
            R_variant_result = 0;
            if (s[d].in_top == 0)
                e = dup_top_level(e);
            s[d].value = e;
            PROTECT(e);
        }

        /* Call the replacement function at level 1, perhaps using the
           fast interface. */

        if (maybe_fast && !isObject(s[1].value) 
              && CAR(s[0].store_args) != R_DotsSymbol
              && (fn = FINDFUN(assgnfcn,rho), 
                  TYPEOF(fn) == SPECIALSXP && PRIMFASTSUB(fn) && !RTRACE(fn))) {
            R_fast_sub_var = s[1].value;
            R_fast_sub_replacement = rhs;
            PROTECT3(R_fast_sub_replacement,R_fast_sub_var,fn);
            newval = CALL_PRIMFUN (call, fn, s[0].store_args, rho, 
                                   VARIANT_FAST_SUB);
            e = R_NilValue;
        }
        else {
            if (POP_IF_TOP_OF_STACK(rhs)) 
                rhs = DUP_STACK_VALUE(rhs);
            PROTECT(rhsprom = mkValuePROMISE(rhs_uneval, rhs));
            PROTECT (lhsprom = mkValuePROMISE(s[1].expr, s[1].value));
            /* original args, no value cell at end, assgnfcn set above*/
            PROTECT(e = replaceCall (assgnfcn, lhsprom, 
                                     s[0].store_args, rhsprom));
            newval = eval(e,rho);
        }

        /* Unprotect e, lhsprom, rhsprom, and s[1].value from the
           previous loop, which went from depth-1 to 1 in the 
           opposite order as this one (plus unprotect one more from
           before that).  Note: e used later, but no alloc before. */

        UNPROTECT(4);

        /* Call the replacement functions at levels 2 to depth, changing the
           values at each level, using the fetched value at that level 
           (was perhaps duplicated), and the new value after replacement at 
           the lower level.  Except we don't do that if it's not necessary
           because the new value is already part of the larger object. */
        
        for (d = 1; d < depth; d++) {

            /* If the replacement function returned a different object, 
               we have to replace, since that new object won't be part 
               of the object at the next level, even if the old one was. */

            if (s[d].in_top == 1 && s[d].value == newval) { 

                /* Don't need to do replacement. */

                newval = s[d+1].value;
                UNPROTECT(1);  /* s[d+1].value protected in previous loop */
            }
            else {

                /* Put value into the next-higher object. */

                PROTECT (rhsprom = mkValuePROMISE (e, newval));
                PROTECT (lhsprom = mkValuePROMISE (s[d+1].expr, s[d+1].value));
                assgnfcn = installAssignFcnName(CAR(s[d].expr));
                b = cons_with_tag (rhsprom, R_NilValue, R_ValueSymbol);
                if (s[d].store_args == R_NoObject)
                    s[d].store_args = CONS (CADDR(s[d].expr), b);
                else if (s[d].store_args == R_NilValue)
                    s[d].store_args = b;
                else if (TYPEOF(s[d].store_args) != LISTSXP) /* one arg */
                    s[d].store_args = CONS (s[d].store_args, b);
                else {
                    for (v = s[d].store_args; CDR(v)!=R_NilValue; v = CDR(v)) ;
                    SETCDR(v, b);
                }
                PROTECT(e = LCONS (assgnfcn, CONS(lhsprom, s[d].store_args)));

                newval = eval(e,rho);

                /* Unprotect e, lhsprom, rhsprom, and s[d+1].value from the
                   previous loop, which went from depth-1 to 1 in the 
                   opposite order as this one (plus unprotect one more from
                   before that).  Note: e used later, but no alloc before. */

                UNPROTECT(4);
            }
        }

        UNPROTECT(depth-1+2);  /* store_args + two more */
    }

    /* Assign the final result after the top level replacement.  We
       can sometimes avoid the cost of this by looking at the saved
       binding cell, if we have one. */

    if (bcell != R_NilValue && CAR(bcell) == newval) {
        SET_MISSING(bcell,0);
        /* The replacement function might have changed NAMEDCNT to 0. */
        SET_NAMEDCNT_NOT_0(varval);
    }
    else {
        if (opval == 2) /* <<- */
            set_var_nonlocal (var, newval, ENCLOS(rho), 3);
        else
            set_var_in_frame (var, newval, rho, TRUE, 3);
    }

    if (variant & VARIANT_NULL) {
        POP_IF_TOP_OF_STACK(rhs);
        return R_NilValue;
    }
    else {
        DEC_NAMEDCNT(rhs);
        return rhs;
    }
}


/* Evaluate each expression in "el" in the environment "rho".  
   The evaluation is done by calling evalv with the given variant.

   The MISSING gp field in the CONS cell for a missing argument is 
   set to the result of R_isMissing, which will allow identification 
   of missing arguments resulting from '_'.

   Used in eval and applyMethod (object.c) for builtin primitives,
   do_internal (names.c) for builtin .Internals and in evalArgs. */

SEXP attribute_hidden evalList_v (SEXP el, SEXP rho, int variant)
{
    /* Handle 0 or 1 arguments (not ...) specially, for speed. */

    if (CDR(el) == R_NilValue) { /* Note that CDR(R_NilValue) == R_NilValue */
        if (el == R_NilValue)
            return R_NilValue;
        if (CAR(el) != R_DotsSymbol)
            return cons_with_tag (EVALV (CAR(el), rho, variant), 
                                  R_NilValue, TAG(el));
    }

    /* The general case (except for el == R_NilValue, handed above). */

    int varpend = variant | VARIANT_PENDING_OK;

    BEGIN_PROTECT4 (head, tail, ev, h);

    head = R_NilValue;

    do {  /* el won't be R_NilValue, so will loop at least once */

	if (CAR(el) == R_DotsSymbol) {
            /* If we have a ... symbol, we look to see what it is bound to.
               If its binding is Null (i.e. zero length) or missing we just
               ignore it and return the cdr with all its expressions evaluated.
               If it is bound to a ... list of promises, we force all the 
               promises and then splice the list of resulting values into
               the return value. Anything else bound to a ... symbol is an 
               error. */
	    h = findVar(CAR(el), rho);
	    if (TYPEOF(h) == DOTSXP) {
		while (h != R_NilValue) {
                    ev = cons_with_tag (evalv (CAR(h), rho, varpend),
                                        R_NilValue, TAG(h));
                    if (head==R_NilValue)
                        head = ev;
                    else
                        SETCDR(tail, ev);
                    tail = ev;
                    if (CAR(ev) == R_MissingArg && isSymbol(CAR(h)))
                        SET_MISSING (ev, R_isMissing(CAR(h),rho));
		    h = CDR(h);
		}
	    }
	    else if (h != R_NilValue && h != R_MissingArg)
		dotdotdot_error();

	} else {
            if (CDR(el) == R_NilValue) 
                varpend = variant;  /* don't defer pointlessly for last one */
            ev = cons_with_tag(EVALV(CAR(el),rho,varpend), R_NilValue, TAG(el));
            if (head==R_NilValue)
                head = ev;
            else
                SETCDR(tail, ev);
            tail = ev;
            if (CAR(ev) == R_MissingArg && isSymbol(CAR(el)))
                SET_MISSING (ev, R_isMissing(CAR(el),rho));
	}

	el = CDR(el);

    } while (el != R_NilValue);

    if (! (variant & VARIANT_PENDING_OK))
        WAIT_UNTIL_ARGUMENTS_COMPUTED(head);

    RETURN_SEXP_INSIDE_PROTECT (head);
    END_PROTECT;

} /* evalList_v */


/* evalListUnshared evaluates each expression in "el" in the
   environment "rho", ensuring that the values of variables evaluated
   are unshared, if they are atomic scalars without attributes, by
   assigning a duplicate to them if necessary.

   Used in .External (with .Call using eval_unshared directly) as a
   defensive measure against argument abuse.  evalListUnshared waits
   for arguments to be computed, and does not allow missing
   arguments. */

SEXP attribute_hidden eval_unshared (SEXP e, SEXP rho, int variant)
{
    SEXP res;

    if (!isSymbol(e) || e == R_DotsSymbol || DDVAL(e)) {
        res = evalv (e, rho, variant);
    }
    else {

        res = findVarPendingOK (e, rho);

        if (res == R_UnboundValue)
            unbound_var_error(e);
        else if (res == R_MissingArg) {
            if ( ! (variant & VARIANT_MISSING_OK))
                if (!DDVAL(e))  /* revert bug fix for the moment */
                    arg_missing_error(e);
        }
        else if (TYPEOF(res) == PROMSXP) {
            if (PRVALUE_PENDING_OK(res) == R_UnboundValue)
                res = forcePromiseUnbound(res,VARIANT_PENDING_OK);
            else
                res = PRVALUE_PENDING_OK(res);
        }
        else {
            if (NAMEDCNT_GT_1(res) && R_binding_cell != R_NilValue
              && isVectorAtomic(res) && LENGTH(res) == 1 && !HAS_ATTRIB(res)) {
                if (0) { /* Enable for debugging */
                    if (installed_already("UNSHARED.DEBUG") != R_NoObject)
                        Rprintf("Making %s unshared\n",CHAR(PRINTNAME(e)));
                }
                res = duplicate(res);
                SETCAR (R_binding_cell, res);
                /* DON'T clear MISSING, though may not get here if it matters */
            }
            SET_NAMEDCNT_NOT_0(res);
        }
    }

    return res;
}

SEXP attribute_hidden evalListUnshared(SEXP el, SEXP rho)
{
    BEGIN_PROTECT4 (head, tail, ev, h);

    int variant = VARIANT_PENDING_OK;

    head = R_NilValue;

    while (el != R_NilValue) {

        if (CDR(el) == R_NilValue)
            variant = 0;  /* would need to wait for last immediately anyway */

	if (CAR(el) == R_DotsSymbol) {
            /* If we have a ... symbol, we look to see what it is bound to.
               If its binding is Null (i.e. zero length) or missing we just
               ignore it and return the cdr with all its expressions evaluated.
               If it is bound to a ... list of promises, we force all the 
               promises and then splice the list of resulting values into
               the return value. Anything else bound to a ... symbol is an 
               error. */
	    h = findVar(CAR(el), rho);
	    if (TYPEOF(h) == DOTSXP) {
		while (h != R_NilValue) {
                    ev = cons_with_tag (eval_unshared (CAR(h), rho, variant),
                                        R_NilValue, TAG(h));
                    if (head==R_NilValue)
                        head = ev;
                    else
                        SETCDR(tail, ev);
                    tail = ev;
                    if (CAR(ev) == R_MissingArg && isSymbol(CAR(h)))
                        SET_MISSING (ev, R_isMissing(CAR(h),rho));
		    h = CDR(h);
		}
	    }
	    else if (h != R_NilValue && h != R_MissingArg)
		dotdotdot_error();

	} else {
            ev = cons_with_tag (eval_unshared (CAR(el), rho, variant), 
                                R_NilValue, TAG(el));
            if (head==R_NilValue)
                head = ev;
            else
                SETCDR(tail, ev);
            tail = ev;
            if (CAR(ev) == R_MissingArg && isSymbol(CAR(el)))
                SET_MISSING (ev, R_isMissing(CAR(el),rho));
	}

	el = CDR(el);
    }

    WAIT_UNTIL_ARGUMENTS_COMPUTED (head);

    RETURN_SEXP_INSIDE_PROTECT (head);
    END_PROTECT;

} /* evalListUnshared() */

/* Evaluate argument list, waiting for any pending computations of arguments. */

SEXP attribute_hidden evalList(SEXP el, SEXP rho)
{
    return evalList_v (el, rho, 0);
}

/* Evaluate argument list, waiting for pending computations, and with no 
   error for missing arguments. */

SEXP attribute_hidden evalListKeepMissing(SEXP el, SEXP rho)
{ 
    return evalList_v (el, rho, VARIANT_MISSING_OK);
}


/* Create a promise to evaluate each argument.	If the argument is itself
   a promise, it is used unchanged, except that it has its NAMEDCNT
   incremented, and the NAMEDCNT of its value (if not unbound) incremented
   unless it is zero.  See inside for handling of ... */

SEXP attribute_hidden promiseArgs(SEXP el, SEXP rho)
{
    /* Handle 0 or 1 arguments (not ...) specially, for speed. */

    if (CDR(el) == R_NilValue) {  /* Note that CDR(R_NilValue) == R_NilValue */
        if (el == R_NilValue)
            return el;
        SEXP a = CAR(el);
        if (a != R_DotsSymbol) {
            MAKE_PROMISE(a,rho);
            return cons_with_tag (a, R_NilValue, TAG(el));
        }
    }

    /* Handle the general case (except for el being R_NilValue, done above). */

    BEGIN_PROTECT4 (head, tail, ev, h);

    head = R_NilValue;

    do {  /* el == R_NilValue is handled above, so always loop at least once */

        SEXP a = CAR(el);

	/* If we have a ... symbol, we look to see what it is bound to.
	   If its binding is R_NilValue we just ignore it.  If it is bound
           to a list, promises in the list (typical case) are re-used with
           NAMEDCNT incremented, and non-promises have promises created for
           them; the promise is then spliced into the list that is returned.
           Anything else bound to a ... symbol is an error. */

	if (a == R_DotsSymbol) {
	    h = findVar(a, rho);
            if (h == R_NilValue) {
                /* nothing */
            }
	    else if (TYPEOF(h) == DOTSXP) {
		while (h != R_NilValue) {
                    a = CAR(h);
                    MAKE_PROMISE(a,rho);
                    ev = cons_with_tag (a, R_NilValue, TAG(h));
                    if (head==R_NilValue)
                        head = ev;
                    else
                        SETCDR(tail,ev);
                    tail = ev;
		    h = CDR(h);
		}
	    }
	    else if (h != R_MissingArg)
		dotdotdot_error();
	}
        else {
            MAKE_PROMISE(a,rho);
            ev = cons_with_tag (a, R_NilValue, TAG(el));
            if (head == R_NilValue)
                head = ev;
            else
                SETCDR(tail, ev);
            tail = ev;
        }
	el = CDR(el);

    } while (el != R_NilValue);

    RETURN_SEXP_INSIDE_PROTECT (head);
    END_PROTECT;
}
 
/* Create promises for arguments, with values for promises filled in.  
   Values for arguments that don't become promises are silently ignored.  
   This is used in method dispatch, hence the text of the error message 
   (which should never occur). */
 
SEXP attribute_hidden promiseArgsWithValues(SEXP el, SEXP rho, SEXP values)
{
    SEXP s, a, b;
    PROTECT(s = promiseArgs(el, rho));
    if (length(s) != length(values)) error(_("dispatch error"));
    for (a = values, b = s; a != R_NilValue; a = CDR(a), b = CDR(b))
        if (TYPEOF(CAR(b)) == PROMSXP) {
            SET_PRVALUE(CAR(b), CAR(a));
            INC_NAMEDCNT(CAR(a));
        }
    UNPROTECT(1);
    return s;
}

/* Like promiseArgsWithValues except it sets only the first value. */

SEXP attribute_hidden promiseArgsWith1Value(SEXP el, SEXP rho, SEXP value)
{
    SEXP s;
    PROTECT(s = promiseArgs(el, rho));
    if (s == R_NilValue) error(_("dispatch error"));
    if (TYPEOF(CAR(s)) == PROMSXP) {
        SET_PRVALUE(CAR(s), value);
        INC_NAMEDCNT(value);
    }
    UNPROTECT(1);
    return s;
}


/* Check that each formal is a symbol */

/* used in coerce.c */
void attribute_hidden CheckFormals(SEXP ls)
{
    if (isList(ls)) {
	for (; ls != R_NilValue; ls = CDR(ls))
	    if (TYPEOF(TAG(ls)) != SYMSXP)
		goto err;
	return;
    }
 err:
    error(_("invalid formal argument list for \"function\""));
}


static SEXP VectorToPairListNamed(SEXP x)
{
    SEXP xptr, xnew, xnames;
    int i, len, len_x = length(x);

    PROTECT(x);
    PROTECT(xnames = getAttrib(x, R_NamesSymbol)); 
                       /* isn't this protected via x?  Or could be concocted? */

    len = 0;
    if (xnames != R_NilValue) {
	for (i = 0; i < len_x; i++)
	    if (CHAR(STRING_ELT(xnames,i))[0] != 0) len += 1;
    }

    PROTECT(xnew = allocList(len));

    if (len > 0) {
	xptr = xnew;
	for (i = 0; i < len_x; i++) {
	    if (CHAR(STRING_ELT(xnames,i))[0] != 0) {
		SETCAR (xptr, VECTOR_ELT(x,i));
		SET_TAG (xptr, install (translateChar (STRING_ELT(xnames,i))));
		xptr = CDR(xptr);
	    }
	}
    } 

    UNPROTECT(3);
    return xnew;
}

#define simple_as_environment(arg) (IS_S4_OBJECT(arg) && (TYPEOF(arg) == S4SXP) ? R_getS4DataSlot(arg, ENVSXP) : R_NilValue)

/* "eval" and "eval.with.vis" : Evaluate the first argument */
/* in the environment specified by the second argument. */

static SEXP do_eval (SEXP call, SEXP op, SEXP args, SEXP rho, int variant)
{
    SEXP encl, x, xptr;
    volatile SEXP expr, env, tmp;

    int frame;
    RCNTXT cntxt;

    checkArity(op, args);

    expr = CAR(args);
    env = CADR(args);
    encl = CADDR(args);
    if (isNull(encl)) {
	/* This is supposed to be defunct, but has been kept here
	   (and documented as such) */
	encl = R_BaseEnv;
    } else if ( !isEnvironment(encl) &&
		!isEnvironment((encl = simple_as_environment(encl))) )
	error(_("invalid '%s' argument"), "enclos");
    if(IS_S4_OBJECT(env) && (TYPEOF(env) == S4SXP))
	env = R_getS4DataSlot(env, ANYSXP); /* usually an ENVSXP */
    switch(TYPEOF(env)) {
    case NILSXP:
	env = encl;     /* so eval(expr, NULL, encl) works */
        break;
    case ENVSXP:
	break;
    case LISTSXP:
	/* This usage requires all the pairlist to be named */
	env = NewEnvironment(R_NilValue, duplicate(CADR(args)), encl);
        set_symbits_in_env(env);
	break;
    case VECSXP:
	/* PR#14035 */
	x = VectorToPairListNamed(CADR(args));
	for (xptr = x ; xptr != R_NilValue ; xptr = CDR(xptr))
	    SET_NAMEDCNT_MAX(CAR(xptr));
	env = NewEnvironment(R_NilValue, x, encl);
        set_symbits_in_env(env);
	break;
    case INTSXP:
    case REALSXP:
	if (length(env) != 1)
	    error(_("numeric 'envir' arg not of length one"));
	frame = asInteger(env);
	if (frame == NA_INTEGER)
	    error(_("invalid '%s' argument"), "envir");
	env = R_sysframe(frame, R_GlobalContext);
	break;
    default:
	error(_("invalid '%s' argument"), "envir");
    }

    PROTECT(env); /* may no longer be what was passed in arg */

    /* isLanguage includes NILSXP, and that does not need to be evaluated,
       so don't use isLanguage(expr) || isSymbol(expr) || isByteCode(expr) */
    if (TYPEOF(expr) == LANGSXP || TYPEOF(expr) == SYMSXP || isByteCode(expr)) {
	begincontext(&cntxt, CTXT_RETURN, call, env, rho, args, op);
	if (!SETJMP(cntxt.cjmpbuf))
	    expr = evalv (expr, env, VARIANT_PASS_ON(variant));
	else {
	    expr = R_ReturnedValue;
	    if (expr == R_RestartToken) {
		cntxt.callflag = CTXT_RETURN;  /* turn restart off */
		error(_("restarts not supported in 'eval'"));
	    }
            if ( ! (variant & VARIANT_PENDING_OK))
                WAIT_UNTIL_COMPUTED(R_ReturnedValue);
	}
	UNPROTECT(1);
	PROTECT(expr);
	endcontext(&cntxt);
    }
    else if (TYPEOF(expr) == EXPRSXP) {
	int i, n;
        int len;
        SEXP *srcrefs;
        getBlockSrcrefs(expr,&srcrefs,&len);
	n = LENGTH(expr);
	tmp = R_NilValue;
	begincontext(&cntxt, CTXT_RETURN, call, env, rho, args, op);
        SEXP savedsrcref = R_Srcref;
	if (!SETJMP(cntxt.cjmpbuf)) {
	    for (i = 0 ; i < n ; i++) {
                R_Srcref = getSrcref (srcrefs, len, i); 
		tmp = evalv (VECTOR_ELT(expr, i), env, 
                        i==n-1 ? VARIANT_PASS_ON(variant) 
                               : VARIANT_NULL | VARIANT_PENDING_OK);
            }
        }
	else {
	    tmp = R_ReturnedValue;
	    if (tmp == R_RestartToken) {
		cntxt.callflag = CTXT_RETURN;  /* turn restart off */
		error(_("restarts not supported in 'eval'"));
	    }
            if ( ! (variant & VARIANT_PENDING_OK))
                WAIT_UNTIL_COMPUTED(R_ReturnedValue);
	}
	UNPROTECT(1);
	PROTECT(tmp);
        R_Srcref = savedsrcref;
	endcontext(&cntxt);
	expr = tmp;
    }
    else if( TYPEOF(expr) == PROMSXP ) {
	expr = forcePromise(expr);
    } 
    else 
        ; /* expr is returned unchanged */

    if (PRIMVAL(op)) { /* eval.with.vis(*) : */
	PROTECT(expr);
	PROTECT(env = allocVector(VECSXP, 2));
	PROTECT(encl = allocVector(STRSXP, 2));
	SET_STRING_ELT(encl, 0, mkChar("value"));
	SET_STRING_ELT(encl, 1, mkChar("visible"));
	SET_VECTOR_ELT(env, 0, expr);
	SET_VECTOR_ELT(env, 1, ScalarLogicalMaybeConst(R_Visible));
	setAttrib(env, R_NamesSymbol, encl);
	expr = env;
	UNPROTECT(3);
    }

    UNPROTECT(1);
    return expr;
}

/* This is a special .Internal */
static SEXP do_withVisible(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP x, nm, ret;

    checkArity(op, args);
    x = CAR(args);
    x = eval(x, rho);
    PROTECT(x);
    PROTECT(ret = allocVector(VECSXP, 2));
    PROTECT(nm = allocVector(STRSXP, 2));
    SET_STRING_ELT(nm, 0, mkChar("value"));
    SET_STRING_ELT(nm, 1, mkChar("visible"));
    SET_VECTOR_ELT(ret, 0, x);
    SET_VECTOR_ELT(ret, 1, ScalarLogicalMaybeConst(R_Visible));
    setAttrib(ret, R_NamesSymbol, nm);
    UNPROTECT(3);
    return ret;
}

/* This is a special .Internal */
static SEXP do_recall(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    RCNTXT *cptr;
    SEXP s, ans ;
    cptr = R_GlobalContext;
    /* get the args supplied */
    while (cptr != NULL) {
	if (cptr->callflag == CTXT_RETURN && cptr->cloenv == rho)
	    break;
	cptr = cptr->nextcontext;
    }
    if (cptr != NULL) {
	args = cptr->promargs;
    }
    /* get the env recall was called from */
    s = R_GlobalContext->sysparent;
    while (cptr != NULL) {
	if (cptr->callflag == CTXT_RETURN && cptr->cloenv == s)
	    break;
	cptr = cptr->nextcontext;
    }
    if (cptr == NULL)
	error(_("'Recall' called from outside a closure"));

    /* If the function has been recorded in the context, use it
       otherwise search for it by name or evaluate the expression
       originally used to get it.
    */
    if (cptr->callfun != R_NilValue)
	PROTECT(s = cptr->callfun);
    else if( TYPEOF(CAR(cptr->call)) == SYMSXP)
	PROTECT(s = findFun(CAR(cptr->call), cptr->sysparent));
    else
	PROTECT(s = eval(CAR(cptr->call), cptr->sysparent));
    if (TYPEOF(s) != CLOSXP) 
    	error(_("'Recall' called from outside a closure"));
    ans = applyClosure_v(cptr->call, s, args, cptr->sysparent, NULL, 0);
    UNPROTECT(1);
    return ans;
}


static SEXP evalArgs(SEXP el, SEXP rho, int dropmissing)
{
    return dropmissing ? evalList(el,rho) : evalListKeepMissing(el,rho);
}


/* A version of DispatchOrEval that checks for possible S4 methods for
 * any argument, not just the first.  Used in the code for `[` in
 * do_subset.  Differs in that all arguments are evaluated
 * immediately, rather than after the call to R_possible_dispatch.
 * NOT ACTUALLY USED AT PRESENT.
 */
attribute_hidden
int DispatchAnyOrEval(SEXP call, SEXP op, const char *generic, SEXP args,
		      SEXP rho, SEXP *ans, int dropmissing, int argsevald)
{
    if(R_has_methods(op)) {
        SEXP argValue, el,  value; 
	/* Rboolean hasS4 = FALSE; */ 
	int nprotect = 0, dispatch;
	if(!argsevald) {
            PROTECT(argValue = evalArgs(args, rho, dropmissing));
	    nprotect++;
	    argsevald = TRUE;
	}
	else argValue = args;
	for(el = argValue; el != R_NilValue; el = CDR(el)) {
	    if(IS_S4_OBJECT(CAR(el))) {
	        value = R_possible_dispatch(call, op, argValue, rho, TRUE);
	        if (value != R_NoObject) {
		    *ans = value;
		    UNPROTECT(nprotect);
		    return 1;
	        }
		else break;
	    }
	}
	 /* else, use the regular DispatchOrEval, but now with evaluated args */
	dispatch = DispatchOrEval(call, op, generic, argValue, rho, ans, dropmissing, argsevald);
	UNPROTECT(nprotect);
	return dispatch;
    }
    return DispatchOrEval(call, op, generic, args, rho, ans, dropmissing, argsevald);
}


/* DispatchOrEval is used in internal functions which dispatch to
 * object methods (e.g. "[" or "[[").  The code either builds promises
 * and dispatches to the appropriate method, or it evaluates the
 * arguments it comes in with (if argsevald is 0) and returns them so that
 * the generic built-in C code can continue.  Note that CDR(call) is
 * used to obtain the unevaluated arguments when creating promises, even
 * when argsevald is 1 (so args is the evaluated arguments).  If argsevald 
 * is -1, only the first argument will have been evaluated.
 *
 * The arg list is protected by this function, and needn't be by the caller.
 */
attribute_hidden
int DispatchOrEval(SEXP call, SEXP op, const char *generic, SEXP args,
		   SEXP rho, SEXP *ans, int dropmissing, int argsevald)
{
/* DispatchOrEval is called very frequently, most often in cases where
   no dispatching is needed and the isObject or the string-based
   pre-test fail.  To avoid degrading performance it is therefore
   necessary to avoid creating promises in these cases.  The pre-test
   does require that we look at the first argument, so that needs to
   be evaluated.  The complicating factor is that the first argument
   might come in with a "..." and that there might be other arguments
   in the "..." as well.  LT */

    BEGIN_PROTECT1 (x);
    ALSO_PROTECT1 (args);

    int dots = FALSE;

    if (argsevald != 0)
	x = CAR(args);
    else {
	/* Find the object to dispatch on, dropping any leading
	   ... arguments with missing or empty values.  If there are no
	   arguments, R_NilValue is used. */
        x = R_NilValue;
	for (; args != R_NilValue; args = CDR(args)) {
	    if (CAR(args) == R_DotsSymbol) {
		SEXP h = findVar(R_DotsSymbol, rho);
		if (TYPEOF(h) == DOTSXP) {
#ifdef DODO
		    /**** any self-evaluating value should be OK; this
			  is used in byte compiled code. LT */
		    /* just a consistency check */
		    if (TYPEOF(CAR(h)) != PROMSXP)
			error(_("value in '...' is not a promise"));
#endif
		    dots = TRUE;
		    x = eval(CAR(h), rho);
                    break;
		}
		else if (h != R_NilValue && h != R_MissingArg)
		    dotdotdot_error();
	    }
	    else {
                dots = FALSE;
                x = eval(CAR(args), rho);
                break;
	    }
	}
    }

    if (isObject(x)) { /* try to dispatch on the object */
	char *pt;
	/* Try for formal method. */
	if(IS_S4_OBJECT(x) && R_has_methods(op)) {

	    BEGIN_INNER_PROTECT2 (value, argValue);

	    /* create a promise to pass down to applyClosure  */
	    if (argsevald < 0)
                argValue = promiseArgsWith1Value(CDR(call), rho, x);
            else if (argsevald == 0)
		argValue = promiseArgsWith1Value(args, rho, x);
	    else 
                argValue = args;
	    /* This means S4 dispatch */
	    value = R_possible_dispatch (call, op, argValue, rho, argsevald<=0);
	    if (value != R_NoObject) {
		*ans = value;
		RETURN_OUTSIDE_PROTECT (1);
	    }
	    else {
		/* go on, with the evaluated args.  Not guaranteed to have
		   the same semantics as if the arguments were not
		   evaluated, in special cases (e.g., arg values that are
		   LANGSXP).
		   The use of the promiseArgs is supposed to prevent
		   multiple evaluation after the call to possible_dispatch.
		*/
		if (dots)
		    argValue = evalArgs(argValue, rho, dropmissing);
		else {
		    argValue = CONS(x, evalArgs(CDR(argValue),rho,dropmissing));
		    SET_TAG(argValue, CreateTag(TAG(args)));
		}
		args = argValue; 
		argsevald = 1;
	    }

            END_INNER_PROTECT;
	}
	if (TYPEOF(CAR(call)) == SYMSXP)
	    pt = Rf_strrchr(CHAR(PRINTNAME(CAR(call))), '.');
	else
	    pt = NULL;

	if (pt == NULL || strcmp(pt,".default")) {

	    BEGIN_INNER_PROTECT2 (pargs, rho1);
	    RCNTXT cntxt;

            if (argsevald > 0) {  /* handle as in R_possible_dispatch */
                pargs = promiseArgsWithValues(CDR(call), rho, args);
            }
            else
                pargs = promiseArgsWith1Value(args, rho, x); 

	    /* The context set up here is needed because of the way
	       usemethod() is written.  DispatchGroup() repeats some
	       internal usemethod() code and avoids the need for a
	       context; perhaps the usemethod() code should be
	       refactored so the contexts around the usemethod() calls
	       in this file can be removed.

	       Using rho for current and calling environment can be
	       confusing for things like sys.parent() calls captured
	       in promises (Gabor G had an example of this).  Also,
	       since the context is established without a SETJMP using
	       an R-accessible environment allows a segfault to be
	       triggered (by something very obscure, but still).
	       Hence here and in the other usemethod() uses below a
	       new environment rho1 is created and used.  LT */
	    rho1 = NewEnvironment(R_NilValue, R_NilValue, rho);
	    begincontext(&cntxt, CTXT_RETURN, call, rho1, rho, pargs, op);
	    if(usemethod(generic, x, call, pargs, rho1, rho, R_BaseEnv, 0, ans))
	    {   endcontext(&cntxt);
		RETURN_OUTSIDE_PROTECT (1);
	    }
	    endcontext(&cntxt);

            END_INNER_PROTECT;
	}
    }

    if (argsevald <= 0) {
	if (dots)
	    /* The first call argument was ... and may contain more than the
	       object, so it needs to be evaluated here.  The object should be
	       in a promise, so evaluating it again should be no problem. */
	    args = evalArgs(args, rho, dropmissing);
	else {
	    args = cons_with_tag (x, evalArgs(CDR(args), rho, dropmissing),
                                  TAG(args));
	}
    }

    *ans = args;
    END_PROTECT;
    return 0;
}


/* gr needs to be protected on return from this function. */
static void findmethod(SEXP Class, const char *group, const char *generic,
		       SEXP *sxp,  SEXP *gr, SEXP *meth, int *which,
		       SEXP rho)
{
    int len, whichclass;
    char buf[512];

    len = length(Class);

    /* Need to interleave looking for group and generic methods
       e.g. if class(x) is c("foo", "bar)" then x > 3 should invoke
       "Ops.foo" rather than ">.bar"
    */
    for (whichclass = 0 ; whichclass < len ; whichclass++) {
	const char *ss = translateChar(STRING_ELT(Class, whichclass));
	if (!copy_3_strings (buf, sizeof buf, generic, ".", ss))
	    error(_("class name too long in '%s'"), generic);
	*meth = install(buf);
	*sxp = R_LookupMethod(*meth, rho, rho, R_BaseEnv);
	if (isFunction(*sxp)) {
	    *gr = R_BlankScalarString;
	    break;
	}
        if (!copy_3_strings (buf, sizeof buf, group, ".", ss))
	    error(_("class name too long in '%s'"), group);
	*meth = install(buf);
	*sxp = R_LookupMethod(*meth, rho, rho, R_BaseEnv);
	if (isFunction(*sxp)) {
	    *gr = mkString(group);
	    break;
	}
    }
    *which = whichclass;
}

attribute_hidden
int DispatchGroup(const char* group, SEXP call, SEXP op, SEXP args, SEXP rho,
		  SEXP *ans)
{
    int nargs, lwhich, rwhich, set;
    SEXP lclass, s, t, m, lmeth, lsxp, lgr;
    SEXP rclass, rmeth, rgr, rsxp, value;
    char *generic;
    Rboolean useS4 = TRUE, isOps = FALSE;

    /* pre-test to avoid string computations when there is nothing to
       dispatch on because either there is only one argument and it
       isn't an object or there are two or more arguments but neither
       of the first two is an object -- both of these cases would be
       rejected by the code following the string examination code
       below */
    if (args != R_NilValue && ! isObject(CAR(args)) &&
	(CDR(args) == R_NilValue || ! isObject(CADR(args))))
	return 0;

    isOps = strcmp(group, "Ops") == 0;

    /* try for formal method */
    if(length(args) == 1 && !IS_S4_OBJECT(CAR(args))) useS4 = FALSE;
    if(length(args) == 2 &&
       !IS_S4_OBJECT(CAR(args)) && !IS_S4_OBJECT(CADR(args))) useS4 = FALSE;
    if(useS4) {
	/* Remove argument names to ensure positional matching */
	if(isOps)
	    for(s = args; s != R_NilValue; s = CDR(s)) SET_TAG(s, R_NilValue);
	if(R_has_methods(op)) {
	    value = R_possible_dispatch(call, op, args, rho, FALSE);
            if (value != R_NoObject) {
	        *ans = value;
	        return 1;
            }
	}
	/* else go on to look for S3 methods */
    }

    /* check whether we are processing the default method */
    if ( isSymbol(CAR(call)) ) {
        const char *pt;
        pt = CHAR(PRINTNAME(CAR(call)));
        while (*pt == '.') pt += 1;   /* duplicate previous behaviour exactly */
        while (*pt != 0 && *pt != '.') pt += 1;
        if (*pt != 0) {
            while (*pt == '.') pt += 1;
            if (strcmp(pt,"default") == 0)
                return 0;
        }
    }

    if(isOps)
	nargs = length(args);
    else
	nargs = 1;

    if( nargs == 1 && !isObject(CAR(args)) )
	return 0;

    if(!isObject(CAR(args)) && !isObject(CADR(args)))
	return 0;

    generic = PRIMNAME(op);

    lclass = IS_S4_OBJECT(CAR(args)) ? R_data_class2(CAR(args))
              : getClassAttrib(CAR(args));
    PROTECT(lclass);

    if( nargs == 2 )
	rclass = IS_S4_OBJECT(CADR(args)) ? R_data_class2(CADR(args))
                  : getClassAttrib(CADR(args));
    else
	rclass = R_NilValue;
    PROTECT(rclass);

    lsxp = R_NilValue; lgr = R_NilValue; lmeth = R_NilValue;
    rsxp = R_NilValue; rgr = R_NilValue; rmeth = R_NilValue;

    findmethod(lclass, group, generic, &lsxp, &lgr, &lmeth, &lwhich, rho);
    PROTECT(lgr);
    if(isFunction(lsxp) && IS_S4_OBJECT(CAR(args)) && lwhich > 0
       && isBasicClass(translateChar(STRING_ELT(lclass, lwhich)))) {
	/* This and the similar test below implement the strategy
	 for S3 methods selected for S4 objects.  See ?Methods */
        value = CAR(args);
	if (NAMEDCNT_GT_0(value)) SET_NAMEDCNT_MAX(value);
	value = R_getS4DataSlot(value, S4SXP); /* the .S3Class obj. or NULL*/
	if(value != R_NilValue) /* use the S3Part as the inherited object */
	    SETCAR(args, value);
    }

    if( nargs == 2 )
	findmethod(rclass, group, generic, &rsxp, &rgr, &rmeth, &rwhich, rho);
    else
	rwhich = 0;

    if(isFunction(rsxp) && IS_S4_OBJECT(CADR(args)) && rwhich > 0
       && isBasicClass(translateChar(STRING_ELT(rclass, rwhich)))) {
        value = CADR(args);
	if (NAMEDCNT_GT_0(value)) SET_NAMEDCNT_MAX(value);
	value = R_getS4DataSlot(value, S4SXP);
	if(value != R_NilValue) SETCADR(args, value);
    }

    PROTECT(rgr);

    if( !isFunction(lsxp) && !isFunction(rsxp) ) {
	UNPROTECT(4);
	return 0; /* no generic or group method so use default*/
    }

    if( lsxp != rsxp ) {
	if ( isFunction(lsxp) && isFunction(rsxp) ) {
	    /* special-case some methods involving difftime */
	    const char *lname = CHAR(PRINTNAME(lmeth)),
		*rname = CHAR(PRINTNAME(rmeth));
	    if( streql(rname, "Ops.difftime") && 
		(streql(lname, "+.POSIXt") || streql(lname, "-.POSIXt") ||
		 streql(lname, "+.Date") || streql(lname, "-.Date")) )
		rsxp = R_NilValue;
	    else if (streql(lname, "Ops.difftime") && 
		     (streql(rname, "+.POSIXt") || streql(rname, "+.Date")) )
		lsxp = R_NilValue;
	    else {
		warning(_("Incompatible methods (\"%s\", \"%s\") for \"%s\""),
			lname, rname, generic);
		UNPROTECT(4);
		return 0;
	    }
	}
	/* if the right hand side is the one */
	if( !isFunction(lsxp) ) { /* copy over the righthand stuff */
	    lsxp = rsxp;
	    lmeth = rmeth;
	    lgr = rgr;
	    lclass = rclass;
	    lwhich = rwhich;
	}
    }

    /* we either have a group method or a class method */

    int i, j;

    PROTECT(m = allocVector(STRSXP,nargs));
    s = args;
    for (i = 0 ; i < nargs ; i++) {
	t = IS_S4_OBJECT(CAR(s)) ? R_data_class2(CAR(s))
	  : getClassAttrib(CAR(s));
	set = 0;
	if (isString(t)) {
	    for (j = 0 ; j < LENGTH(t) ; j++) {
		if (!strcmp(translateChar(STRING_ELT(t, j)),
			    translateChar(STRING_ELT(lclass, lwhich)))) {
		    SET_STRING_ELT(m, i, PRINTNAME(lmeth));
		    set = 1;
		    break;
		}
	    }
	}
	if( !set )
	    SET_STRING_ELT(m, i, R_BlankString);
	s = CDR(s);
    }

    SEXP genstr = PROTECT(mkString(generic));

    set = length(lclass) - lwhich;
    PROTECT(t = allocVector(STRSXP, set));
    copy_string_elements (t, 0, lclass, lwhich, set);

    SEXP supplied[13];
    supplied[0] = R_NilValue;

    i = 0;

    supplied[i++] = R_dot_Class;          supplied[i++] = t;
    supplied[i++] = R_dot_Generic;        supplied[i++] = genstr;
    supplied[i++] = R_dot_Method;         supplied[i++] = m;
    supplied[i++] = R_dot_GenericCallEnv; supplied[i++] = rho;
    supplied[i++] = R_dot_GenericDefEnv;  supplied[i++] = R_BaseEnv;
    supplied[i++] = R_dot_Group;          supplied[i++] = lgr;


    supplied[i] = R_NilValue;

    PROTECT(t = LCONS(lmeth, CDR(call)));

    /* the arguments have been evaluated; since we are passing them */
    /* out to a closure we need to wrap them in promises so that */
    /* they get duplicated and things like missing/substitute work. */

    PROTECT(s = promiseArgsWithValues(CDR(call), rho, args));
    if (isOps) {
        /* ensure positional matching for operators */
        for (m = s; m != R_NilValue; m = CDR(m))
            SET_TAG(m, R_NilValue);
    }

    *ans = applyClosure_v (t, lsxp, s, rho, supplied, 0);

    UNPROTECT(9);
    return 1;
}

static SEXP do_is_builtin_internal(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP symbol, i;

    checkArity(op, args);
    symbol = CAR(args);

    if (!isSymbol(symbol))
	errorcall(call, _("invalid symbol"));

    if ((i = INTERNAL(symbol)) != R_NilValue && TYPEOF(i) == BUILTINSXP)
	return R_ScalarLogicalTRUE;
    else
	return R_ScalarLogicalFALSE;
}

static SEXP do_loadfile(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP file, s;
    FILE *fp;

    checkArity(op, args);

    PROTECT(file = coerceVector(CAR(args), STRSXP));

    if (! isValidStringF(file))
	errorcall(call, _("bad file name"));

    fp = RC_fopen(STRING_ELT(file, 0), "rb", TRUE);
    if (!fp)
	errorcall(call, _("unable to open 'file'"));
    s = R_LoadFromFile(fp, 0);
    fclose(fp);

    UNPROTECT(1);
    return s;
}

static SEXP do_savefile(SEXP call, SEXP op, SEXP args, SEXP env)
{
    FILE *fp;

    checkArity(op, args);

    if (!isValidStringF(CADR(args)))
	errorcall(call, _("'file' must be non-empty string"));
    if (TYPEOF(CADDR(args)) != LGLSXP)
	errorcall(call, _("'ascii' must be logical"));

    fp = RC_fopen(STRING_ELT(CADR(args), 0), "wb", TRUE);
    if (!fp)
	errorcall(call, _("unable to open 'file'"));

    R_SaveToFileV(CAR(args), fp, INTEGER(CADDR(args))[0], 0);

    fclose(fp);
    return R_NilValue;
}

/* -------------------------------------------------------------------------- */
/*                         LOGICAL OPERATORS                                  */

static SEXP binaryLogic2(int code, SEXP s1, SEXP s2);

/* i1 = i % n1; i2 = i % n2;
 * this macro is quite a bit faster than having real modulo calls
 * in the loop (tested on Intel and Sparc)
 */
#define mod_iterate(n1,n2,i1,i2) for (i=i1=i2=0; i<n; \
	i1 = (++i1 == n1) ? 0 : i1,\
	i2 = (++i2 == n2) ? 0 : i2,\
	++i)

void task_and_or (helpers_op_t code, SEXP ans, SEXP s1, SEXP s2)
{
    int * restrict lans = LOGICAL(ans);

    int i, i1, i2, n, n1, n2;

    n1 = LENGTH(s1);
    n2 = LENGTH(s2);
    n = LENGTH(ans);

    switch (code) {
    case 1:  /* & : AND */
        if (n1 == n2) {
            for (i = 0; i<n; i++) {
                uint32_t u1 = LOGICAL(s1)[i];
                uint32_t u2 = LOGICAL(s2)[i];
                lans[i] = (u1 & u2) | (u1 & (u2<<31)) | (u2 & (u1<<31));
            }
        }
        else {
            mod_iterate(n1,n2,i1,i2) {
                uint32_t u1 = LOGICAL(s1)[i1];
                uint32_t u2 = LOGICAL(s2)[i2];
                lans[i] = (u1 & u2) | (u1 & (u2<<31)) | (u2 & (u1<<31));
            }
        }
        break;
    case 2:  /* | : OR */
        if (n1 == n2) {
            for (i = 0; i<n; i++) {
                uint32_t u = LOGICAL(s1)[i] | LOGICAL(s2)[i];
                lans[i] = u & ~ (u << 31);
            }
        }
        else {
            mod_iterate(n1,n2,i1,i2) {
                uint32_t u = LOGICAL(s1)[i1] | LOGICAL(s2)[i2];
                lans[i] = u & ~ (u << 31);
            }
        }
        break;
    }
}


/* & | */

#define T_and_or THRESHOLD_ADJUST(25)

SEXP attribute_hidden do_andor(SEXP call, SEXP op, SEXP args, SEXP env, 
                               int variant)
{
    SEXP ans, x, y;
    int args_evald;

    /* Evaluate arguments, setting x to first argument and y to
       second argument.  The whole argument list is in args, already 
       evaluated if args_evald is 1. */

    x = CAR(args); 
    y = CADR(args);

    if (x==R_DotsSymbol || y==R_DotsSymbol || CDDR(args)!=R_NilValue) {
        args = evalList (args, env);
        PROTECT(x = CAR(args)); 
        PROTECT(y = CADR(args));
        args_evald = 1;
    }
    else {
        PROTECT(x = evalv (x, env, VARIANT_PENDING_OK));
        PROTECT(y = evalv (y, env, VARIANT_PENDING_OK));
        args_evald = 0;
    }

    /* Check for dispatch on S3 or S4 objects.  Takes care to match length
       of "args" to length of original (number of args in "call"). */

    if (isObject(x) || isObject(y)) {
        if (!args_evald) {
            args = CDR(args)!=R_NilValue ? CONS(x,CONS(y,R_NilValue)) 
                                         : CONS(x,R_NilValue);
            WAIT_UNTIL_COMPUTED_2(x,y);
        }
        PROTECT(args);
        if (DispatchGroup("Ops", call, op, args, env, &ans)) {
            UNPROTECT(3);
            return ans;
        }
        UNPROTECT(1);
    }

    /* Check argument count now (after dispatch, since other methods may allow
       other argument count). */

    checkArity(op,args);

    /* Arguments are now in x and y, and are protected.  The value 
       in args may not be protected, and is not used below. */

    SEXP dims, tsp, klass, xnames, ynames;
    int xarray, yarray, xts, yts;

    if (! (isRaw(x) && isRaw(y)) && ! (isNumber(x) && isNumber(y)))
        errorcall (call,
       _("operations are possible only for numeric, logical or complex types"));

    tsp = R_NilValue;		/* -Wall */
    klass = R_NilValue;		/* -Wall */
    xarray = isArray(x);
    yarray = isArray(y);
    xts = isTs(x);
    yts = isTs(y);
    if (xarray || yarray) {
	if (xarray && yarray) {
	    if (!conformable(x, y))
		error(_("binary operation on non-conformable arrays"));
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
	    PROTECT(klass = getClassAttrib(x));
	}
	else if (xts) {
	    if (length(x) < length(y))
		ErrorMessage(call, ERROR_TSVEC_MISMATCH);
	    PROTECT(tsp = getAttrib(x, R_TspSymbol));
	    PROTECT(klass = getClassAttrib(x));
	}
	else /*(yts)*/ {
	    if (length(y) < length(x))
		ErrorMessage(call, ERROR_TSVEC_MISMATCH);
	    PROTECT(tsp = getAttrib(y, R_TspSymbol));
	    PROTECT(klass = getClassAttrib(y));
	}
    }

    R_len_t nx = LENGTH(x);
    R_len_t ny = LENGTH(y);

    if (nx > 0 && ny > 0 && (nx > ny ? nx % ny : ny % nx))
        warningcall(call,
         _("longer object length is not a multiple of shorter object length"));

    if (isRaw(x) && isRaw(y)) {
        WAIT_UNTIL_COMPUTED_2(x,y);
        PROTECT(ans = binaryLogic2(PRIMVAL(op), x, y));
    }
    else {

        if (nx == 0 || ny == 0) {
            ans = allocVector (LGLSXP, 0);
            UNPROTECT(5);
            return ans;
        }

        R_len_t n = (nx > ny) ? nx : ny;
        PROTECT(ans = allocVector (LGLSXP, n));

        if (!isLogical(x) || !isLogical(y)) {
            WAIT_UNTIL_COMPUTED_2(x,y);
            PROTECT(x = coerceVector(x, LGLSXP));
            y = coerceVector(y, LGLSXP);
            UNPROTECT(1);
        }

        DO_NOW_OR_LATER2 (variant, n >= T_and_or, 
                          0, task_and_or, PRIMVAL(op), ans, x, y);
    }

    if (dims != R_NilValue) {
	setAttrib (ans, R_DimSymbol, dims);
	if (xnames != R_NilValue)
	    setAttrib(ans, R_DimNamesSymbol, xnames);
	else if (ynames != R_NilValue)
	    setAttrib(ans, R_DimNamesSymbol, ynames);
    }
    else {
	if (LENGTH(ans) == length(xnames))
	    setAttrib (ans, R_NamesSymbol, xnames);
	else if (LENGTH(ans) == length(ynames))
	    setAttrib (ans, R_NamesSymbol, ynames);
    }

    if (xts || yts) {
	setAttrib(ans, R_TspSymbol, tsp);
	setAttrib(ans, R_ClassSymbol, klass);
	UNPROTECT(2);
    }

    UNPROTECT(6);
    return ans;
}

void task_not (helpers_op_t code, SEXP x, SEXP arg, SEXP unused)
{
    int len = LENGTH(arg);
    int i;

    switch(TYPEOF(arg)) {
    case LGLSXP:
        for (i = 0; i < len; i++) {
            uint32_t u = LOGICAL(arg)[i];
            LOGICAL(x)[i] = u ^ 1 ^ (u >> 31);
        }
        break;
    case INTSXP:
	for (i = 0; i < len; i++)
	    LOGICAL(x)[i] = (INTEGER(arg)[i] == NA_INTEGER) ? NA_LOGICAL 
                          : INTEGER(arg)[i] == 0;
	break;
    case REALSXP:
	for (i = 0; i < len; i++)
	    LOGICAL(x)[i] = ISNAN(REAL(arg)[i]) ? NA_LOGICAL 
                          : REAL(arg)[i] == 0;
	break;
    case CPLXSXP:
	for (i = 0; i < len; i++)
	    LOGICAL(x)[i] = ISNAN(COMPLEX(arg)[i].r) || ISNAN(COMPLEX(arg)[i].i)
              ? NA_LOGICAL : (COMPLEX(arg)[i].r == 0 && COMPLEX(arg)[i].i == 0);
	break;
    case RAWSXP:
	for (i = 0; i < len; i++)
	    RAW(x)[i] = ~ RAW(arg)[i];
	break;
    }
}

/* Handles the ! operator. */

#define T_not THRESHOLD_ADJUST(40)

static SEXP do_fast_not(SEXP call, SEXP op, SEXP arg, SEXP env, int variant)
{
    SEXP x, dim, dimnames, names;
    int len;

    if (!isLogical(arg) && !isNumber(arg) && !isRaw(arg)) {
	/* For back-compatibility */
	if (length(arg)==0) 
            return allocVector(LGLSXP, 0);
	else
            errorcall(call, _("invalid argument type"));
    }
    len = LENGTH(arg);

    /* Quickly do scalar operation on logical with no attributes. */

    if (len==1 && isLogical(arg) && !HAS_ATTRIB(arg)) {
        int v = LOGICAL(arg)[0];
        return ScalarLogicalMaybeConst (v==NA_LOGICAL ? v : !v);
    }

    /* The general case... */

    if (TYPEOF(arg) != LGLSXP && TYPEOF(arg) != RAWSXP)
        x = allocVector(LGLSXP,len);
    else if (isObject(arg) || NAMEDCNT_GT_0(arg))
        x = duplicate(arg);
    else
        x = arg;

    if (!isVectorAtomic(arg) || TYPEOF(arg) == STRSXP)
	UNIMPLEMENTED_TYPE("do_fast_not", arg);

    DO_NOW_OR_LATER1 (variant, len >= T_not, 0, task_not, 0, x, arg);

    if (TYPEOF(arg) != LGLSXP && TYPEOF(arg) != RAWSXP) {
        if (!NO_ATTRIBUTES_OK(variant,arg)) {
            PROTECT(x);
            PROTECT (names    = getAttrib (arg, R_NamesSymbol));
            PROTECT (dim      = getDimAttrib(arg));
            PROTECT (dimnames = getAttrib (arg, R_DimNamesSymbol));
            if (names    != R_NilValue) setAttrib(x,R_NamesSymbol,    names);
            if (dim      != R_NilValue) setAttrib(x,R_DimSymbol,      dim);
            if (dimnames != R_NilValue) setAttrib(x,R_DimNamesSymbol, dimnames);
            UNPROTECT(4);
        }
    }

    return x;
}

/* ! */

SEXP attribute_hidden do_not(SEXP call, SEXP op, SEXP args, SEXP env, 
                             int variant)
{
    SEXP ans;

    if (DispatchGroup("Ops", call, op, args, env, &ans))
	return ans;

    checkArity (op, args);

    return do_fast_not (call, op, CAR(args), env, variant);
}

/* Does && (op 1) and || (op 2). */

SEXP attribute_hidden do_andor2(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP s1, s2;
    int x1, x2;

    if (length(args) != 2)
	error(_("'%s' operator requires 2 arguments"),
	      PRIMVAL(op) == 1 ? "&&" : "||");

    s1 = eval(CAR(args), env);
    if (!isNumber(s1))
	errorcall(call, _("invalid 'x' type in 'x %s y'"),
		  PRIMVAL(op) == 1 ? "&&" : "||");
    x1 = asLogical(s1);

    if (PRIMVAL(op)==1 && x1==FALSE)  /* FALSE && ... */
        return ScalarLogicalMaybeConst(FALSE);

    if (PRIMVAL(op)==2 && x1==TRUE)   /* TRUE || ... */
        return ScalarLogicalMaybeConst(TRUE);
    
    s2  = eval(CADR(args), env);
    if (!isNumber(s2))	
        errorcall(call, _("invalid 'y' type in 'x %s y'"),
	          PRIMVAL(op) == 1 ? "&&" : "||");		
    x2 = asLogical(s2);

    if (PRIMVAL(op)==1) /* ... && ... */
        return ScalarLogicalMaybeConst (x2==FALSE ? FALSE
                                  : x1==TRUE && x2==TRUE ? TRUE
                                  : NA_LOGICAL);
    else /* ... || ... */
        return ScalarLogicalMaybeConst (x2==TRUE ? TRUE
                                  : x1==FALSE && x2==FALSE ? FALSE
                                  : NA_LOGICAL);
}

static SEXP binaryLogic2(int code, SEXP s1, SEXP s2)
{
    int i, i1, i2, n, n1, n2;
    SEXP ans;

    n1 = LENGTH(s1);
    n2 = LENGTH(s2);
    n = (n1 > n2) ? n1 : n2;
    if (n1 == 0 || n2 == 0) {
	ans = allocVector(RAWSXP, 0);
	return ans;
    }
    ans = allocVector(RAWSXP, n);

    switch (code) {
    case 1:  /* & : AND */
        if (n1 == n2) {
            for (i = 0; i<n; i++)
                RAW(ans)[i] = RAW(s1)[i] & RAW(s2)[i];
        }
        else {
            mod_iterate(n1,n2,i1,i2)
                RAW(ans)[i] = RAW(s1)[i1] & RAW(s2)[i2];
        }
	break;
    case 2:  /* | : OR */
        if (n1 == n2) {
            for (i = 0; i<n; i++)
                RAW(ans)[i] = RAW(s1)[i] | RAW(s2)[i];
        }
        else {
            mod_iterate(n1,n2,i1,i2)
                RAW(ans)[i] = RAW(s1)[i1] | RAW(s2)[i2];
        }
	break;
    }
    return ans;
}

#define OP_ALL 1
#define OP_ANY 2

static int any_all_check (int op, int na_rm, int *x, int n)
{
    if (na_rm) {

        if (op == OP_ANY) {
            unsigned res = 0;
            for (int i = 0; i<n; i++) {
                res |= x[i];
                if (res & 1)
                    return TRUE;
            }
            return FALSE;
        }
        else { /* OP_ALL */
            unsigned res = 1;
            for (int i = 0; i<n; i++) {
                res &= x[i] | (x[i]>>31);
                if (! (res & 1))
                    return FALSE;
            }
            return TRUE;
        }

    }
    else { /* !na_rm */

        if (op == OP_ANY) {
            unsigned res = 0;
            for (int i = 0; i<n; i++) {
                res |= x[i];
                if (res & 1)
                    return TRUE;
            }
            return res>>31 ? NA_LOGICAL : FALSE;
        }
        else { /* OP_ALL */
            unsigned res = 1;
            unsigned na = 0;
            for (int i = 0; i<n; i++) {
                res &= x[i] | (x[i]>>31);
                if (! (res & 1))
                    return FALSE;
                na |= x[i];
            }
            return na>>31 ? NA_LOGICAL : TRUE;
        }

    }
}


/* fast version handles only one unnamed argument, so narm is FALSE. */

static SEXP do_fast_allany (SEXP call, SEXP op, SEXP arg, SEXP env, 
                            int variant)
{
    int val;

    if (length(arg) == 0)
        /* Avoid memory waste from coercing empty inputs, and also
           avoid warnings with empty lists coming from sapply */
        val = PRIMVAL(op) == OP_ALL ? TRUE : FALSE;

    else {
	if (TYPEOF(arg) != LGLSXP) {
	    /* Coercion of integers seems reasonably safe, but for
	       other types it is more often than not an error.
	       One exception is perhaps the result of lapply, but
	       then sapply was often what was intended. */
	    if (TYPEOF(arg) != INTSXP)
		warningcall(call,
			    _("coercing argument of type '%s' to logical"),
			    type2char(TYPEOF(arg)));
	    arg = coerceVector(arg, LGLSXP);
	}
        if (LENGTH(arg) == 1) /* includes variant return of AND or OR of vec */
            val = LOGICAL(arg)[0];
        else
            val = any_all_check (PRIMVAL(op), FALSE, LOGICAL(arg), LENGTH(arg));
    }

    return ScalarLogicalMaybeConst(val);
}

static SEXP do_allany(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP ans, s, t, call2;
    int narm, has_na = 0;
    /* initialize for behavior on empty vector
       all(logical(0)) -> TRUE
       any(logical(0)) -> FALSE
     */
    int val = PRIMVAL(op) == OP_ALL ? TRUE : FALSE;

    PROTECT(args = fixup_NaRm(args));
    PROTECT(call2 = LCONS(CAR(call),args));

    if (DispatchGroup("Summary", call2, op, args, env, &ans)) {
	UNPROTECT(2);
	return(ans);
    }

    ans = matchArgExact(R_NaRmSymbol, &args);
    narm = asLogical(ans);

    for (s = args; s != R_NilValue; s = CDR(s)) {
	t = CAR(s);
	/* Avoid memory waste from coercing empty inputs, and also
	   avoid warnings with empty lists coming from sapply */
	if(length(t) == 0) continue;
	/* coerceVector protects its argument so this actually works
	   just fine */
	if (TYPEOF(t) != LGLSXP) {
	    /* Coercion of integers seems reasonably safe, but for
	       other types it is more often than not an error.
	       One exception is perhaps the result of lapply, but
	       then sapply was often what was intended. */
	    if(TYPEOF(t) != INTSXP)
		warningcall(call,
			    _("coercing argument of type '%s' to logical"),
			    type2char(TYPEOF(t)));
	    t = coerceVector(t, LGLSXP);
	}
	val = any_all_check (PRIMVAL(op), narm, LOGICAL(t), LENGTH(t));
        if (val == NA_LOGICAL)
            has_na = 1;
        else {
            if (PRIMVAL(op) == OP_ANY && val || PRIMVAL(op) == OP_ALL && !val) {
                has_na = 0;
                break;
            }
        } 
    }
    UNPROTECT(2);
    return ScalarLogicalMaybeConst (has_na ? NA_LOGICAL : val);
}

/* -------------------------------------------------------------------------- */
/*                        ARITHMETIC OPERATORS.                               */
/*                                                                            */
/* All but simple cases are handled in R_unary and R_binary in arithmetic.c.  */

static SEXP do_arith (SEXP call, SEXP op, SEXP args, SEXP env, int variant)
{
    int opcode = PRIMVAL(op), obj1, obj2;
    SEXP argsevald, ans, arg1, arg2;

    /* Evaluate arguments, maybe putting them on the scalar stack. */

    SEXP sv_scalar_stack = R_scalar_stack;

    PROTECT (argsevald = 
               scalar_stack_eval2(args, &arg1, &arg2, &obj1, &obj2, env));
    PROTECT2(arg1,arg2);

    /* Check for dispatch on S3 or S4 objects. */

    if (obj1 || obj2) {
        if (DispatchGroup("Ops", call, op, argsevald, env, &ans)) {
            UNPROTECT(3);
            return ans;
        }
    }

    /* Check for argument count error (not before dispatch, since other
       methods may have different requirements). */

    if (argsevald==R_NilValue || CDDR(argsevald)!=R_NilValue)
	errorcall(call,_("operator needs one or two arguments"));

    if (CDR(argsevald)==R_NilValue && opcode!=MINUSOP && opcode!=PLUSOP)
        errorcall(call, _("%d argument passed to '%s' which requires %d"),
                        1, PRIMNAME(op), 2);

    /* Arguments are now in arg1 and arg2, and are protected. They may
       be on the scalar stack, but if so, are removed now, though they
       may still be referenced.  Note that result might be on top of
       one of them - OK since after storing into it, the args won't be
       accessed again.

       Below same as POP_IF_TOP_OF_STACK(arg2); POP_IF_TOP_OF_STACK(arg1);
       but faster. */

    R_scalar_stack = sv_scalar_stack;

    /* We quickly do real arithmetic and integer plus/minus/times on scalars 
       with no attributes (as will be the case for scalar stack values).  We
       don't bother trying local assignment, since returning the result on the
       scalar stack should be about as fast. */

    int type1 = TYPEOF(arg1);

    if ((type1==REALSXP || type1==INTSXP) && LENGTH(arg1) == 1
                                          && NO_ATTRIBUTES_OK (variant, arg1)) {

        if (CDR(argsevald)==R_NilValue) { /* Unary operation */
            WAIT_UNTIL_COMPUTED(arg1);
            if (type1==REALSXP) {
                double val = opcode == PLUSOP ? *REAL(arg1) : -*REAL(arg1);
                ans = NAMEDCNT_EQ_0(arg1) ? (*REAL(arg1) = val, arg1)
                    : CAN_USE_SCALAR_STACK(variant) ? PUSH_SCALAR_REAL(val)
                    :   ScalarReal(val);
            }
            else { /* INTSXP */
                int val  = *INTEGER(arg1)==NA_INTEGER ? NA_INTEGER
                         : opcode == PLUSOP ? *INTEGER(arg1) : -*INTEGER(arg1);
                ans = NAMEDCNT_EQ_0(arg1) ? (*INTEGER(arg1) = val, arg1)
                    : CAN_USE_SCALAR_STACK(variant) ? PUSH_SCALAR_INTEGER(val)
                    :   ScalarInteger(val);
            }
            goto ret;
        }

        int type2 = TYPEOF(arg2);

        if ((type2 == REALSXP || type2 == INTSXP) && LENGTH(arg2) == 1 
                                       && NO_ATTRIBUTES_OK (variant, arg2)) {

            if (type1 == INTSXP && type2 == INTSXP) {

                if (opcode==PLUSOP || opcode==MINUSOP || opcode==TIMESOP) {

                    WAIT_UNTIL_COMPUTED_2(arg1,arg2);
    
                    int a1 = *INTEGER(arg1), a2 = *INTEGER(arg2);
                    int_fast64_t val;
    
                    if (a1==NA_INTEGER || a2==NA_INTEGER)
                        val = NA_INTEGER;
                    else {
                        val = 
                         opcode==PLUSOP  ? (int_fast64_t)a1 + (int_fast64_t)a2 :
                         opcode==MINUSOP ? (int_fast64_t)a1 - (int_fast64_t)a2 :
                                           (int_fast64_t)a1 * (int_fast64_t)a2;
    
                          if (val < R_INT_MIN || val > R_INT_MAX) {
                              val = NA_INTEGER;
                              warningcall (call, 
                                         _("NAs produced by integer overflow"));
                          }
                    }
    
                    int ival = (int) val;

                    ans = NAMEDCNT_EQ_0(arg2) ?
                            (*INTEGER(arg2) = ival, arg2)
                        : NAMEDCNT_EQ_0(arg1) ?
                            (*INTEGER(arg1) = ival, arg1)
                        : CAN_USE_SCALAR_STACK(variant) ? 
                            PUSH_SCALAR_INTEGER(ival)
                        :   ScalarInteger(ival);
  
                    goto ret;
                }
                else {
                    /* fall through to general code below */
                }
            }

            else { /* not both INTSXP, so at least one is REALSXP */

                double a1, a2, val;
    
                WAIT_UNTIL_COMPUTED_2(arg1,arg2);

                if (type1 == INTSXP) {
                    a1 = (double) *INTEGER(arg1);
                    a2 = *REAL(arg2);
                }
                else if (type2 == INTSXP) {
                    a1 = *REAL(arg1);
                    a2 = (double) *INTEGER(arg2);
                }
                else {
                    a1 = *REAL(arg1);
                    a2 = *REAL(arg2);
                }
            
                switch (opcode) {
                case PLUSOP:
                    val = a1 + a2;
                    break;
                case MINUSOP:
                    val = a1 - a2;
                    break;
                case TIMESOP:
                    val = a1 * a2;
                    break;
                case DIVOP:
                    val = a1 / a2;
                    break;
                case POWOP:
                    if (a2 == 2.0)       val = a1 * a1;
                    else if (a2 == 1.0)  val = a1;
                    else if (a2 == 0.0)  val = 1.0;
                    else if (a2 == -1.0) val = 1.0 / a1;
                    else                 val = R_pow(a1,a2);
                    break;
                case MODOP:
                    val = myfmod(a1,a2);
                    break;
                case IDIVOP:
                    val = myfloor(a1,a2);
                    break;
                default: abort();
                }

                ans = NAMEDCNT_EQ_0(arg2) && type2 == REALSXP ?
                        (*REAL(arg2) = val, arg2)
                    : NAMEDCNT_EQ_0(arg1) && type1 == REALSXP ?
                        (*REAL(arg1) = val, arg1)
                    : CAN_USE_SCALAR_STACK(variant) ? 
                        PUSH_SCALAR_REAL(val)
                    :   ScalarReal(val);

                goto ret;
            }
        }
    }

    /* Otherwise, handle the general case. */

    ans = CDR(argsevald)==R_NilValue 
           ? R_unary (call, op, arg1, obj1, env, variant) 
           : R_binary (call, op, arg1, arg2, obj1, obj2, env, variant);

  ret:
    UNPROTECT(3);
    return ans;
}

/* -------------------------------------------------------------------------- */
/*                       RELATIONAL OPERATORS.                                */
/*                                                                            */
/* Main work is done in R_relop, in relop.c.                                  */

static SEXP do_relop(SEXP call, SEXP op, SEXP args, SEXP env, int variant)
{
    SEXP argsevald, ans, x, y;
    int objx, objy;

    /* Evaluate arguments, maybe putting them on the scalar stack. */

    SEXP sv_scalar_stack = R_scalar_stack;

    PROTECT(argsevald = 
              scalar_stack_eval2 (args, &x, &y, &objx, &objy, env));
    PROTECT2(x,y);

    /* Check for dispatch on S3 or S4 objects. */

    if (objx || objy) {
        if (DispatchGroup("Ops", call, op, argsevald, env, &ans)) {
            UNPROTECT(3);
            return ans;
        }
    }

    /* Check argument count now (after dispatch, since other methods may allow
       other argument count). */

    checkArity(op,argsevald);

    /* Arguments are now in x and y, and are protected.  They may be on
       the scalar stack, but if so are popped off here (but retain their
       values if eval is not called). */

    /* Below does same as POP_IF_TOP_OF_STACK(y); POP_IF_TOP_OF_STACK(x);
       but faster. */

    R_scalar_stack = sv_scalar_stack;

    ans = R_relop (call, op, x, y, objx, objy, env, variant);

    UNPROTECT(3);
    return ans;
}


/* -------------------------------------------------------------------------- */
/*                       ENVIRONMENT FUNCTIONS                                */

#define DEBUG_OUTPUT 0          /* 0 to 2 for increasing debug output */
#define DEBUG_CHECK 0           /* 1 to enable debug check of HASHSLOTSUSED */

#define BINDING_VALUE(b) ((IS_ACTIVE_BINDING(b) ? getActiveValue(CAR(b)) : CAR(b)))

#define SYMBOL_BINDING_VALUE(s) ((IS_ACTIVE_BINDING(s) ? getActiveValue(SYMVALUE(s)) : SYMVALUE(s)))
#define SYMBOL_HAS_BINDING(s) (IS_ACTIVE_BINDING(s) || (SYMVALUE(s) != R_UnboundValue))

#define SET_BINDING_VALUE(b,val) do { \
  SEXP __b__ = (b); \
  SEXP __val__ = (val); \
  if (BINDING_IS_LOCKED(__b__)) \
    error(_("cannot change value of locked binding for '%s'"), \
	  CHAR(PRINTNAME(TAG(__b__)))); \
  if (IS_ACTIVE_BINDING(__b__)) \
    setActiveValue(CAR(__b__), __val__); \
  else \
    SETCAR(__b__, __val__); \
} while (0)

#define SET_SYMBOL_BINDING_VALUE(sym, val) do { \
  SEXP __sym__ = (sym); \
  SEXP __val__ = (val); \
  if (BINDING_IS_LOCKED(__sym__)) \
    error(_("cannot change value of locked binding for '%s'"), \
	  CHAR(PRINTNAME(__sym__))); \
  if (IS_ACTIVE_BINDING(__sym__)) \
    setActiveValue(SYMVALUE(__sym__), __val__); \
  else \
    SET_SYMVALUE(__sym__, __val__); \
} while (0)

static void setActiveValue(SEXP fun, SEXP val)
{
    SEXP arg = LCONS(R_QuoteSymbol, CONS(val, R_NilValue));
    SEXP expr = LCONS(fun, CONS(arg, R_NilValue));
    WAIT_UNTIL_COMPUTED(val); \
    PROTECT(expr);
    eval(expr, R_GlobalEnv);
    UNPROTECT(1);
}

static SEXP getActiveValue(SEXP fun)
{
    SEXP expr = LCONS(fun, R_NilValue);
    PROTECT(expr);
    expr = eval(expr, R_GlobalEnv);
    UNPROTECT(1);
    return expr;
}

/* Macro to produce an unrolled loop to search for a symbol in a chain.
   This code takes advantage of the CAR, CDR and TAG of R_NilValue being
   R_NilValue to avoid a check for R_NilValue in unrolled part.  The
   arguments are the pointer to the start of the chain (which is modified
   to point to the binding cell found), the symbol to search for, and
   the statement to do if the symbol is found, which must have the effect
   of exitting the loop (ie, be a "break", "return", or "goto" statement).
   If the symbol is not found, execution continues after this macro, with
   the chain pointer being R_NilValue. 

   The optimal amount of unrolling may depend on whether compressed or
   uncompressed pointers are used, so these cases are distinguished. */

#if USE_SYM_TUNECNTS
#define INC_SYM_TUNECNT(sym) (((SYMSEXP)UPTR_FROM_SEXP(sym))->sym_tunecnt += 1)
#else 
#define INC_SYM_TUNECNT(sym) 0
#endif

#if USE_ENV_TUNECNTS
#define INC_ENV_TUNECNT(env) (((ENVSEXP)UPTR_FROM_SEXP(env))->env_tunecnt += 1)
#else 
#define INC_ENV_TUNECNT(env) 0
#endif

#if USE_COMPRESSED_POINTERS

#define SEARCH_LOOP(env,chain,symbol,statement) do { \
    INC_SYM_TUNECNT(symbol); \
    INC_ENV_TUNECNT(env); \
    do { \
        if (TAG(chain) == symbol) statement; \
        chain = CDR(chain); \
    } while (chain != R_NilValue); \
} while (0)

#else
   
#define SEARCH_LOOP(env,chain,symbol,statement) do { \
    INC_SYM_TUNECNT(symbol); \
    INC_ENV_TUNECNT(env); \
    do { \
        if (TAG(chain) == symbol) statement; \
        chain = CDR(chain); \
        if (TAG(chain) == symbol) statement; \
        chain = CDR(chain); \
        if (TAG(chain) == symbol) statement; \
        chain = CDR(chain); \
    } while (chain != R_NilValue); \
} while (0)

#endif

/* Function to correctly set ENVSYMBITS for an environment. */

static void chainbits (SEXP chain, R_symbits_t *pbits)
{
    R_symbits_t bits;
   
    bits = 0;

    while (chain != R_NilValue) {
        bits |= SYMBITS (TAG (chain));
        chain = CDR(chain);
    }

    *pbits |= bits;
}

void attribute_hidden set_symbits_in_env (SEXP env)
{
    R_symbits_t bits = 0;
   
    if (HASHTAB(env) != R_NilValue) {
        SEXP table = HASHTAB(env);
        R_len_t len = HASHLEN(env);
        R_len_t i;
        for (i = 0; i < len; i++) {
            chainbits (VECTOR_ELT(table,i), &bits);
        }
    }
    else {
        chainbits (FRAME(env), &bits);
    }

    SET_ENVSYMBITS (env, bits);
}

/*--------------------------------------------------------------------------- */
/*                               HASH TABLES                                  */

/* We use a basic separate chaining algorithm.	A hash table consists
   of SEXP (vector) which contains a number of SEXPs (lists).

   The main non-static function is R_NewHashedEnv, which allows code to
   request a hashed environment.  All others are static to allow
   internal changes of implementation without affecting client code. */


/*----------------------------------------------------------------------
  R_HashAddEntry 

  Adds an entry to the chain at index i in a hash table, keeping the
  chain sorted by increasing hash value, and secondarily printname (out 
  of a desire for save/load to not change the order).  HASHSLOTUSED is 
  updated. */

static void R_HashAddEntry (SEXP table, int i, SEXP entry)
{
    SEXP next = VECTOR_ELT(table,i);

    if (next == R_NilValue) {
        SET_HASHSLOTSUSED (table, HASHSLOTSUSED(table) + 1);
        SET_VECTOR_ELT (table, i, entry);
        SETCDR (entry, R_NilValue);
    }
    else {
        int entry_hash = SYM_HASH(TAG(entry));
        SEXP last = R_NilValue;

        for (;;) {
            int next_hash = SYM_HASH(TAG(next));
            if (entry_hash < next_hash || entry_hash == next_hash &&
                  strcmp (CHAR(PRINTNAME(TAG(entry))),
                          CHAR(PRINTNAME(TAG(next)))) < 0)
                break;
            last = next;
            next = CDR(next);
            if (next == R_NilValue)
                break;
        }

        SETCDR (entry, next);
        if (last == R_NilValue)
            SET_VECTOR_ELT (table, i, entry);
        else
            SETCDR (last, entry);
    }
}


/*----------------------------------------------------------------------
  R_HashGet

  Hashtable get function.  Returns 'value' from 'table' indexed by
  'symbol'.  'hashcode' must be provided by user.  Returns
  'R_UnboundValue' if value is not present. */

static SEXP R_HashGet(SEXP env, int hashcode, SEXP symbol, SEXP table)
{
    SEXP chain = VECTOR_ELT(table, hashcode);

    SEARCH_LOOP (env, chain, symbol, goto found);

    return R_UnboundValue;

found:
    return BINDING_VALUE(chain);
}

static Rboolean R_HashExists(SEXP env, int hashcode, SEXP symbol, SEXP table)
{
    SEXP chain = VECTOR_ELT(table, hashcode);

    SEARCH_LOOP (env, chain, symbol, goto found);

    return FALSE;

found:
    return TRUE;
}


/*----------------------------------------------------------------------
  R_HashGetLoc

  Hashtable get location function. Just like R_HashGet, but returns
  location of variable, rather than its value. Returns R_NilValue
  if not found. */

static inline SEXP R_HashGetLoc(SEXP env, int hashcode, SEXP symbol, SEXP table)
{
    SEXP chain = VECTOR_ELT(table, hashcode);

    SEARCH_LOOP (env, chain, symbol, return chain);

    return R_NilValue;
}

/*----------------------------------------------------------------------
  R_NewHashTable

  Environment hash table initialisation.  Creates a table of size 'size'. */

SEXP attribute_hidden R_NewHashTable(int size)
{
    SEXP table;

    if (size < HASHMINSIZE) size = HASHMINSIZE;

    table = allocVector(VECSXP, size);
    SET_HASHSLOTSUSED(table, 0);

    return table;
}


/*----------------------------------------------------------------------
  R_NewHashedEnv

  Returns a new environment with a hash table initialized with specified
  size.  The only non-static hash table function. */

SEXP R_NewHashedEnv(SEXP enclos, SEXP size)
{
    SEXP s, t;

    PROTECT(size);
    PROTECT(s = NewEnvironment(R_NilValue, R_NilValue, enclos));
    t = R_NewHashTable(asInteger(size));
    SET_HASHTAB (s, t);
    UNPROTECT(2);
    return s;
}


/*----------------------------------------------------------------------
  R_HashRehash

  Redo the hashing in the table, since it may have been done with a
  different hash function.  The lists within the hash table have their
  pointers shuffled around so that they are not reallocated.  */

void attribute_hidden R_HashRehash (SEXP table)
{
    /* Do some checking */
    if (TYPEOF(table) != VECSXP)
	error("argument not of type VECSXP, from R_HashRehash");

    int size = LENGTH(table);
    int slots = HASHSLOTSUSED(table);
    int i;

    for (i = 0; i < size; i++) {
        SEXP e;
        e = VECTOR_ELT (table, i);
        SET_VECTOR_ELT (table, i, R_NilValue);
        while (e != R_NilValue) {
            int j = SYM_HASH(TAG(e)) % size;
            SEXP f = CDR(e);
            R_HashAddEntry (table, j, e);
            e = f;
        }
    }

    SET_HASHSLOTSUSED (table, slots);
}


/* ---------------------------------------------------------------------
   OLD HASH FUNCTION.  Only for writing out data, for compatibility with
   R versions that use the old hash function and assume data was written
   using it.

   Note that there is some confusion here about whether the result
   is signed or unsigned, but since in fact the top four bits (out
   of 32) are always zero, it makes no real difference.

   PROBLEM HERE???  If characters can have the top bit set, the result
   can depend on whether the "char" type is signed, which is
   platform-dependent. */

static int R_Newhashpjw(const char *s)
{
    signed char *p;
    unsigned h = 0, g;
    for (p = (char *) s; *p; p++) {
	h = (h << 4) + (*p);
	if ((g = h & 0xf0000000) != 0) {
	    h = h ^ (g >> 24);
	    h = h ^ g;
	}
    }
    return h;
}

/*----------------------------------------------------------------------
  R_HashRehashOld

  Return a new version of the table, rehashed with the old hash function.
  Allocates new nodes for the chains. */

SEXP attribute_hidden R_HashRehashOld (SEXP table)
{
    /* Do some checking */
    if (TYPEOF(table) != VECSXP)
	error("argument not of type VECSXP, from R_HashRehashOld");

    int size = LENGTH(table);
    SEXP newtable;
    int i;

    PROTECT (newtable = allocVector (VECSXP, size));
    SET_HASHSLOTSUSED (newtable, 0);

    SETLEVELS (newtable, LEVELS(table));              /* just in case... */
    SET_ATTRIB (newtable, ATTRIB(table));
    SET_OBJECT (newtable, OBJECT(table));
    if (IS_S4_OBJECT(table)) SET_S4_OBJECT(newtable);

    for (i = 0; i < size; i++) {
        SEXP e;
        e = VECTOR_ELT (table, i);
        while (e != R_NilValue) {
            int j = R_Newhashpjw(CHAR(PRINTNAME(TAG(e)))) % size;
            SEXP f = cons_with_tag (CAR(e), R_NilValue, TAG(e));
            SETLEVELS (f, LEVELS(e));
            SET_ATTRIB (f, ATTRIB(e));
            R_HashAddEntry (newtable, j, f);
            e = CDR(e);
        }
    }

    UNPROTECT(1); /* newtable */

    return newtable;
}


/*----------------------------------------------------------------------
  R_HashResize

  Hash table resizing function. Increases the size of the hash table
  by a factor of two (accounting for header). The vector is
  reallocated, but the lists within the hash table have their pointers
  shuffled around so that they are not reallocated.  */

SEXP attribute_hidden R_HashResize(SEXP table)
{
    SEXP new_table, chain, tmp_chain;
    int new_size, counter, new_hashcode;
#if DEBUG_OUTPUT
    Rprintf("\nABOUT TO RESIZE HASH TABLE %llu/%u\n",
            (long long unsigned) UPTR_FROM_SEXP(table),
            (unsigned) CPTR_FROM_SEXP(table));
    int n_entries = 0;
#endif

    /* Do some checking */
    if (TYPEOF(table) != VECSXP)
	error("argument not of type VECSXP, from R_HashResize");

    /* Allocate the new hash table.  Return old table if would exceed max. */

    new_size = 2 * (LENGTH(table) + SGGC_ENV_HASH_HEAD) - SGGC_ENV_HASH_HEAD;

    if (new_size > HASHMAXSIZE)
        return table;

    new_table = R_NewHashTable (new_size);

    /* Move entries into new table. */
    for (counter = 0; counter < LENGTH(table); counter++) {
	chain = VECTOR_ELT(table, counter);
	while (chain != R_NilValue) {
#if DEBUG_OUTPUT
            n_entries += 1;
#endif
            if (TYPEOF(chain) != LISTSXP) abort();
            new_hashcode = SYM_HASH(TAG(chain)) % LENGTH(new_table);
	    tmp_chain = chain;
	    chain = CDR(chain);
            R_HashAddEntry (new_table, new_hashcode, tmp_chain);
#if DEBUG_OUTPUT>1
	    Rprintf(
             "LENGTH=%d HASHSLOTSUSED=%d counter=%d HASHCODE=%d\n",
              LENGTH(table), HASHSLOTSUSED(table), counter, new_hashcode);
#endif
	}
    }

    /* Some debugging statements */
#if DEBUG_OUTPUT
    Rprintf("RESIZED TABLE WITH %d ENTRIES, NEW TABLE IS %llu/%u\n", n_entries,
            (long long unsigned) UPTR_FROM_SEXP(new_table),
            (unsigned) CPTR_FROM_SEXP(new_table));
    Rprintf("Old size: %d, New size: %d\n",LENGTH(table),LENGTH(new_table));
    Rprintf("Old slotsused: %d, New slotsused: %d\n",
	    HASHSLOTSUSED(table), HASHSLOTSUSED(new_table));
#endif

    return new_table;
} /* end R_HashResize */


/*----------------------------------------------------------------------
  R_HashFrame

  Hashing for environment frames.  This function ensures that the
  frame in the given environment has been hashed. */

void attribute_hidden R_HashFrame(SEXP rho)
{
    int hashcode;
    SEXP frame, tmp_chain, table;

    /* Do some checking */
    if (TYPEOF(rho) != ENVSXP)
	error("argument not of type ENVSXP, from R_Hashframe");

    table = HASHTAB(rho);

#if DEBUG_OUTPUT
    Rprintf("\nMAKING ENVIRONMENT %llu HASHED, WITH SIZE %d\n",
            (long long unsigned)table, LENGTH(table));
#endif

    frame = FRAME(rho);
    while (frame != R_NilValue) {
	hashcode = SYM_HASH(TAG(frame)) % LENGTH(table);
	tmp_chain = frame;
	frame = CDR(frame);
        R_HashAddEntry (table, hashcode, tmp_chain);
    }
    SET_FRAME(rho, R_NilValue);
    SET_ENVSYMBITS(rho, ~(R_symbits_t)0);
}


/* ---------------------------------------------------------------------
   R_HashProfile

   Profiling tool for analyzing hash table performance.  Returns a
   three element list with components:

   size: the total size of the hash table

   nchains: the number of non-null chains in the table (as reported by
	    HASHSLOTSUSED())

   counts: an integer vector the same length as size giving the length of
	   each chain (or zero if no chain is present).  This allows
	   for assessing collisions in the hash table. */

static SEXP R_HashProfile(SEXP table)
{
    SEXP chain, ans, chain_counts, nms;
    int i, count;

    /* Do some checking */
    if (TYPEOF(table) != VECSXP)
	error("argument not of type VECSXP, from R_HashProfile");

    int len_table = LENGTH(table);

    PROTECT(ans = allocVector(VECSXP, 3));
    PROTECT(nms = allocVector(STRSXP, 3));
    SET_STRING_ELT(nms, 0, mkChar("size"));    /* size of hashtable */
    SET_STRING_ELT(nms, 1, mkChar("nchains")); /* number of non-null chains */
    SET_STRING_ELT(nms, 2, mkChar("counts"));  /* length of each chain */
    setAttrib(ans, R_NamesSymbol, nms);
    UNPROTECT(1);

    SET_VECTOR_ELT(ans, 0, ScalarInteger(len_table));
    SET_VECTOR_ELT(ans, 1, ScalarInteger(HASHSLOTSUSED(table)));

    PROTECT(chain_counts = allocVector(INTSXP, len_table));
    for (i = 0; i < len_table; i++) {
	chain = VECTOR_ELT(table, i);
	count = 0;
	for (; chain != R_NilValue ; chain = CDR(chain)) {
	    count++;
	}
	INTEGER(chain_counts)[i] = count;
    }

    SET_VECTOR_ELT(ans, 2, chain_counts);

    UNPROTECT(2);
    return ans;
}

static SEXP do_envprofile(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    /* Return a list containing profiling information given a hashed
       environment.  For non-hashed environments, this function
       returns R_NilValue.  This seems appropriate since there is no
       way to test whether an environment is hashed at the R level.
    */
    SEXP env = CAR(args);
    if (isEnvironment(env))
	return IS_HASHED(env) ? R_HashProfile(HASHTAB(env)) : R_NilValue;
    else
	error("argument must be a hashed environment");
}


/* --------------------------------------------------------------------------
   GLOBAL VARIABLE CACHING.  A cache is maintained in the symvalue and
   attribute fields of symbols.  BASE_CACHE flags a symbol whose
   cached value is in symvalue.  Otherwise, the cached binding cell
   is in the attribute field, R_NilValue if the symbol is not in the
   global cache.

   To make sure the cache is valid, all binding creations and removals
   from global frames must go through the interface functions in this file.

   Initially only the R_GlobalEnv frame is a global frame.  Additional
   global frames can only be created by attach.  All other frames are
   considered local.  Whether a frame is local or not is recorded in
   the highest order bit of the ENVFLAGS field (the gp field of sxpinfo). */

#define GLOBAL_FRAME_MASK (1<<15)
#define IS_GLOBAL_FRAME(e) (ENVFLAGS(e) & GLOBAL_FRAME_MASK)
#define MARK_AS_GLOBAL_FRAME(e) \
  SET_ENVFLAGS(e, ENVFLAGS(e) | GLOBAL_FRAME_MASK)
#define MARK_AS_LOCAL_FRAME(e) \
  SET_ENVFLAGS(e, ENVFLAGS(e) & (~ GLOBAL_FRAME_MASK))

void attribute_hidden InitBaseEnv()
{
    R_BaseEnv = NewEnvironment(R_NilValue, R_NilValue, R_EmptyEnv);
    UPTR_FROM_SEXP(R_BaseEnv)->sxpinfo.trace_base = 1;
}

void attribute_hidden InitGlobalEnv()
{
    R_GlobalEnv = R_NewHashedEnv(R_BaseEnv, ScalarIntegerMaybeConst(0));

    MARK_AS_GLOBAL_FRAME(R_GlobalEnv);

    R_BaseNamespace = NewEnvironment(R_NilValue, R_NilValue, R_GlobalEnv);
    UPTR_FROM_SEXP(R_BaseNamespace)->sxpinfo.trace_base = 1;
    R_PreserveObject(R_BaseNamespace);
    SET_SYMVALUE(install(".BaseNamespaceEnv"), R_BaseNamespace);
    R_NamespaceRegistry = R_NewHashedEnv(R_NilValue,ScalarIntegerMaybeConst(0));
    R_PreserveObject(R_NamespaceRegistry);
    defineVar(install("base"), R_BaseNamespace, R_NamespaceRegistry);

    /**** needed to properly initialize the base namespace */
}

static inline void R_FlushGlobalCache(SEXP sym)
{
    ATTRIB_W(sym) = R_NilValue;
    SET_BASE_CACHE(sym,0);
}

static void R_FlushGlobalCacheFromTable(SEXP table)
{
    int i, size;
    SEXP chain;
    size = LENGTH(table);
    for (i = 0; i < size; i++) {
	for (chain = VECTOR_ELT(table, i); chain != R_NilValue; chain = CDR(chain))
	    R_FlushGlobalCache(TAG(chain));
    }
}

/* Flush the cache based on the names provided by the user defined
   table, specifically returned from calling objects() for that table. */
static void R_FlushGlobalCacheFromUserTable(SEXP udb)
{
    int n, i;
    R_ObjectTable *tb;
    SEXP names;
    tb = (R_ObjectTable*) R_ExternalPtrAddr(udb);
    names = tb->objects(tb);
    n = length(names);
    for(i = 0; i < n ; i++)
	R_FlushGlobalCache(Rf_installChar(STRING_ELT(names,i)));
}

static inline void R_AddGlobalCacheBase(SEXP symbol)
{
    ATTRIB_W(symbol) = R_NilValue;
    SET_BASE_CACHE (symbol, !IS_ACTIVE_BINDING(symbol));
}

static inline void R_AddGlobalCacheNonBase(SEXP symbol, SEXP place)
{
    ATTRIB_W(symbol) = place;
    SET_BASE_CACHE (symbol, 0);
}

static inline void R_AddGlobalCacheNotFound(SEXP symbol)
{
    ATTRIB_W(symbol) = R_UnboundValue;
    SET_BASE_CACHE (symbol, 0);
}

static SEXP R_GetGlobalCache(SEXP symbol)
{
    SEXP val;

    if (BASE_CACHE(symbol)) {
        val = SYMVALUE(symbol);
        return val;
    }

    if (ATTRIB_W(symbol) == R_UnboundValue)
        return R_UnboundValue;

    if (ATTRIB_W(symbol) == R_NilValue)
        return R_NoObject;

    val = BINDING_VALUE(ATTRIB_W(symbol));

    if (val == R_UnboundValue) {
        ATTRIB_W(symbol) = R_NilValue;
        return R_NoObject;
    }

    return val;
}


/* Remove variable from a list, return the new list, and return its old value 
   (R_NoObject if not there) in 'value'. */

static SEXP RemoveFromList(SEXP thing, SEXP list, SEXP *value)
{
    SEXP last = R_NilValue;
    SEXP curr = list;

    while (curr != R_NilValue) {

        if (TAG(curr) == thing) {
            *value = CAR(curr);
            SETCAR(curr, R_UnboundValue); /* in case binding is cached */
            LOCK_BINDING(curr);           /* in case binding is cached */
            if (last==R_NilValue)
                list = CDR(curr);
            else
                SETCDR(last, CDR(curr));
            return list;
        }

        last = curr;
        curr = CDR(curr);
    }

    *value = R_NoObject;
    return list;
}


/*----------------------------------------------------------------------
  find_binding_in_frame

  Look up the location of the value of a symbol in a single
  environment frame.  Returns the binding cell rather than the value
  itself, or R_NilValue if not found.  Does not wait for the bound
  value to be computed.

  Callers set *canCache = TRUE or NULL */

SEXP attribute_hidden Rf_find_binding_in_frame (SEXP rho, SEXP symbol, 
                                                Rboolean *canCache)
{
    SEXP loc;

    if (SEXP32_FROM_SEXP(rho) == LASTSYMENV(symbol)) {
         loc = LASTSYMBINDING(symbol);
         if (BINDING_VALUE(loc) == R_UnboundValue)
             LASTSYMENV(symbol) = R_NoObject32;
         else
             return loc;
    }

    if (IS_USER_DATABASE(rho)) {
	R_ObjectTable *table = (R_ObjectTable *)R_ExternalPtrAddr(HASHTAB(rho));
	SEXP val = table->get(CHAR(PRINTNAME(symbol)), canCache, table);
	/* Better to use exists() here if we don't actually need the value? */
	if (val == R_UnboundValue)
            loc = R_NilValue;
        else {
	    /* The result should probably be identified as being from
	       a user database, or maybe use an active binding
	       mechanism to allow setting a new value to get back to
	       the data base. */
            loc = cons_with_tag (val, R_NilValue, symbol);
	    /* If the database has a canCache method, then call that.
	       Otherwise, we believe the setting for canCache. */
	    if(canCache && table->canCache)
		*canCache = table->canCache(CHAR(PRINTNAME(symbol)), table);
	}
    }

    else if (IS_BASE(rho))
	error("'find_binding_in_frame' cannot be used on the base environment");

    else if (!isEnvironment(rho))  /* somebody does this... */
	return R_NilValue;

    else if (HASHTAB(rho) == R_NilValue) {

        loc = FRAME(rho);
        SEARCH_LOOP (rho, loc, symbol, goto found);

        return R_NilValue;

      found:
        if ( ! IS_ACTIVE_BINDING(loc) && !DDVAL(symbol)) {
            /* Note:  R_MakeActiveBinding won't let an existing binding 
               become active, so we later assume this can't be active. */
            LASTSYMENV(symbol) = SEXP32_FROM_SEXP(rho);
            LASTSYMBINDING(symbol) = loc;
        }
    }
    else {
        int hashcode;
	hashcode = SYM_HASH(symbol) % HASHLEN(rho);
	/* Will return 'R_NilValue' if not found */
	loc = R_HashGetLoc(rho, hashcode, symbol, HASHTAB(rho));
    }

    return loc;
}


/* External version and accessor functions. Returned value is cast as
   an opaque pointer to insure it is only used by routines in this
   group.  This allows the implementation to be changed without needing
   to change other files. */

R_varloc_t R_findVarLocInFrame(SEXP rho, SEXP symbol)
{
    SEXP binding = Rf_find_binding_in_frame(rho, symbol, NULL);
    return binding == R_NilValue ? R_NoObject : (R_varloc_t) binding;
}

SEXP R_GetVarLocValue(R_varloc_t vl)
{
    SEXP value = BINDING_VALUE((SEXP) vl);
    WAIT_UNTIL_COMPUTED(value);
    return value;
}

SEXP R_GetVarLocSymbol(R_varloc_t vl)
{
    return TAG((SEXP) vl);
}

Rboolean R_GetVarLocMISSING(R_varloc_t vl)
{
    return MISSING((SEXP) vl);
}

void R_SetVarLocValue(R_varloc_t vl, SEXP value)
{
    SET_BINDING_VALUE((SEXP) vl, value);
    /* Should this also do a SET_MISSING((SEXP)vl,0)? */
}


/*----------------------------------------------------------------------
  findVarInFrame3

  Look up the value of a symbol in a single environment frame.	This
  is the basic building block of all variable lookups.

  It is important that this be as efficient as possible.

  The final argument controls the exact behaviour, as follows:

    0 (or FALSE)  Lookup and return value, or R_UnboundValue if doesn't exist.
                  On a user database, does a "get" only if "exists" is true.
                  Waits for computation of the value to finish.

    1 (or TRUE)   Same as 0, except always does a "get" on a user database.

    2             Only checks existence, returning R_UnboundValue if a 
                  binding doesn't exist and R_NilValue if one does exist.
                  Doesn't wait for computation of the value to finish, since 
                  it isn't being returned.

    3             Same as 1, except doesn't wait for computation of the value 
                  to finish.

    7             Like 3, except that it returns R_UnboundValue if the 
                  binding found is active, or locked, or from a user database.

  Note that (option&1)!=0 if a get should aways be done on a user database,
  and (option&2)!=0 if we don't need to wait.

  The findVarInFramePendingOK version is an abbreviation for option 3.
  It may not only be more convenient, but also be faster.

  Sets R_binding_cell to the CONS cell for the binding, if the value returned
  is not R_UnboundValue, and there is a cell (not one for base environment),
  and the cell is suitable for updating by the caller (is not for an active 
  binding or locked).  Otherwise, R_binding_cell is set to R_NilValue. */

SEXP findVarInFramePendingOK(SEXP rho, SEXP symbol)
{
    SEXP value;

    if (SEXP32_FROM_SEXP(rho) == LASTSYMENV(symbol)) {
        SEXP binding = LASTSYMBINDING(symbol); /* won't be an active binding */
        if ( ! BINDING_IS_LOCKED(binding)) {
            value = CAR(binding);
            if (value == R_UnboundValue)
                LASTSYMENV(symbol) = R_NoObject32;
            else {
                R_binding_cell = binding;
                return value;
            }
        }
    }

    return findVarInFrame3_nolast (rho, symbol, 3);
}

SEXP findVarInFrame3(SEXP rho, SEXP symbol, int option)
{
    SEXP value;

    if (SEXP32_FROM_SEXP(rho) == LASTSYMENV(symbol)) {
        SEXP binding = LASTSYMBINDING(symbol); /* won't be an active binding */
        if ( ! BINDING_IS_LOCKED(binding)) {
            value = CAR(binding);
            if (value == R_UnboundValue)
                LASTSYMENV(symbol) = R_NoObject32;
            else {
                switch (option) {
                case 0:
                case 1:
                    WAIT_UNTIL_COMPUTED(value);
                    break;
                case 2:
                    value = R_NilValue;
                    break;
                default:
                    break;
                }
                R_binding_cell = binding;
                return value;
            }
        }
    }

    return findVarInFrame3_nolast (rho, symbol, option);
}

/* Version that doesn't check LASTSYMBINDING.  Split from above so the
   simplest case will have low overhead.  Can also be called directly
   if it's known that checking LASTSYMBINDING won't help. 

   Will fail quickly when the symbol has LASTENVNOTFOUND equal to 
   the environment.  LASTENVNOTFOUND is also updated on failure if the 
   environment is unhashed. */

SEXP findVarInFrame3_nolast(SEXP rho, SEXP symbol, int option)
{
    SEXP loc, value, bcell;

    bcell = R_NilValue;
    value = R_UnboundValue;

    if (IS_BASE(rho)) {
        if (option==2) 
            value = SYMBOL_HAS_BINDING(symbol) ? R_NilValue : R_UnboundValue;
        else if (option==7 && IS_ACTIVE_BINDING(symbol))
            value = R_UnboundValue;
        else 
            value = SYMBOL_BINDING_VALUE(symbol);
    }

    else if (IS_USER_DATABASE(rho)) {
        if (option==7)
            value = R_UnboundValue;
        else {
            /* Use the objects function pointer for this symbol. */
            R_ObjectTable *table = 
              (R_ObjectTable *) R_ExternalPtrAddr(HASHTAB(rho));
            value = R_UnboundValue;
            if (table->active) {
                if (option&1)
                    value = table->get (CHAR(PRINTNAME(symbol)), NULL, table);
                else {
                    if (table->exists (CHAR(PRINTNAME(symbol)), NULL, table))
                        value = option==2 ? R_NilValue
                          : table->get (CHAR(PRINTNAME(symbol)), NULL, table);
                    else
                        value = R_UnboundValue;
                }
            }
        }
    }

    else if (!isEnvironment(rho))
        error(_("argument to '%s' is not an environment"), "findVarInFrame3");

    else if (HASHTAB(rho) == R_NilValue) {

        if (LASTENVNOTFOUND(symbol) != SEXP32_FROM_SEXP(rho)) {
            loc = FRAME(rho);
            SEARCH_LOOP (rho, loc, symbol, goto found);
            LASTENVNOTFOUND(symbol) = SEXP32_FROM_SEXP(rho);
        }
        goto ret;

      found: 
        if (IS_ACTIVE_BINDING(loc)) {
            if (option==7) 
                goto ret;
        }
        else {
            if (!DDVAL(symbol)) {
                /* Note:  R_MakeActiveBinding won't let an existing binding 
                   become active, so we later assume this can't be active. */
                LASTSYMENV(symbol) = SEXP32_FROM_SEXP(rho);
                LASTSYMBINDING(symbol) = loc;
            }
            if (BINDING_IS_LOCKED(loc)) {
                if (option==7)
                    goto ret;
            }
            else
                bcell = loc;
        }
        value = option==2 ? R_NilValue : BINDING_VALUE(loc);
    }

    else {
        int hashcode;
        hashcode = SYM_HASH(symbol) % HASHLEN(rho);
        loc = R_HashGetLoc(rho, hashcode, symbol, HASHTAB(rho));
        if (loc == R_NilValue)
            goto ret;
        if (IS_ACTIVE_BINDING(loc) || BINDING_IS_LOCKED(loc)) {
            if (option==7)
                goto ret;
        }
        else
            bcell = loc;
        value = option == 2 ? R_NilValue : BINDING_VALUE(loc);
    }

  ret:
    R_binding_cell = bcell;

    if ((option&2) == 0)
        WAIT_UNTIL_COMPUTED(value);

    return value;
}

SEXP findVarInFrame(SEXP rho, SEXP symbol)
{
    return findVarInFrame3(rho, symbol, TRUE);
}


/* findGlobalVar searches for a symbol value starting at R_GlobalEnv, so the
   cache can be used.  Doesn't wait for the value found to be computed.
   Always set R_binding_cell to R_NilValue - fast updates here aren't needed. */

static inline SEXP findGlobalVar(SEXP symbol)
{
    SEXP vl, rho;

    R_binding_cell = R_NilValue;

    vl = R_GetGlobalCache(symbol);
    if (vl != R_NoObject)
	return vl;

    Rboolean canCache = TRUE;

    for (rho = R_GlobalEnv; rho != R_EmptyEnv; rho = ENCLOS(rho)) {
	if (IS_BASE(rho)) {
	    vl = SYMBOL_BINDING_VALUE(symbol);
	    if (vl != R_UnboundValue) {
		R_AddGlobalCacheBase(symbol);
                return vl;
            }
        }
        else {
	    vl = Rf_find_binding_in_frame(rho, symbol, &canCache);
	    if (vl != R_NilValue) {
		if(canCache)
		    R_AddGlobalCacheNonBase(symbol, vl);
		return BINDING_VALUE(vl);
	    }
	}

    }

    R_AddGlobalCacheNotFound(symbol);
    return R_UnboundValue;
}


/*----------------------------------------------------------------------
  findVar

     Look up a symbol in an environment.  Waits for the value to be computed.

  findVarPendingOK 

     Like findVar, but doesn't wait for the value to be computed.

  These set R_binding_cell as for findVarInFrame3. */

SEXP findVar(SEXP symbol, SEXP rho)
{
    SEXP value;

    rho = SKIP_USING_SYMBITS (rho, symbol);

    /* This first loop handles local frames, if there are any.  It
       will also handle all frames if rho is a global frame other than
       R_GlobalEnv */

    while (rho != R_GlobalEnv && rho != R_EmptyEnv) {
	value = findVarInFrame3 (rho, symbol, 1);
	if (value != R_UnboundValue) 
            return value;
	rho = ENCLOS(rho);
    }

    if (rho == R_GlobalEnv) {
	value = findGlobalVar(symbol);
        WAIT_UNTIL_COMPUTED(value);
        return value;
    }
    else {
        R_binding_cell = R_NilValue;
        return R_UnboundValue;
    }
}

SEXP findVarPendingOK(SEXP symbol, SEXP rho)
{
    SEXP value;

    rho = SKIP_USING_SYMBITS (rho, symbol);

    /* This first loop handles local frames, if there are any.  It
       will also handle all frames if rho is a global frame other than
       R_GlobalEnv */

    while (rho != R_GlobalEnv && rho != R_EmptyEnv) {
	value = findVarInFramePendingOK (rho, symbol);
	if (value != R_UnboundValue) 
            return value;
	rho = ENCLOS(rho);
    }

    if (rho == R_GlobalEnv) {
	value = findGlobalVar(symbol);
        return value;
    }
    else {
        R_binding_cell = R_NilValue;
        return R_UnboundValue;
    }
}


/*----------------------------------------------------------------------
  findVar1

  Look up a symbol in an environment.  Ignore any values which are
  not of the specified type. */

SEXP attribute_hidden findVar1 (SEXP symbol, SEXP rho, SEXPTYPE mode,
                                int inherits)
{
    if (inherits) rho = SKIP_USING_SYMBITS (rho, symbol);

    while (rho != R_EmptyEnv) {
	SEXP vl = findVarInFrame3(rho, symbol, TRUE);
	if (vl != R_UnboundValue) {
	    if (mode == ANYSXP) return vl;
	    if (TYPEOF(vl) == PROMSXP)
                vl = forcePromise(vl);
	    if (TYPEOF(vl) == mode) return vl;
	    if (mode == FUNSXP && isFunction(vl)) return (vl);
	}
	if (inherits)
	    rho = ENCLOS(rho);
	else
	    return (R_UnboundValue);
    }
    return (R_UnboundValue);
}

/* ditto, but check *mode* not *type*. */

SEXP attribute_hidden findVar1mode (SEXP symbol, SEXP rho, SEXPTYPE mode,
                                     int inherits, Rboolean doGet)
{
    if (inherits) rho = SKIP_USING_SYMBITS (rho, symbol);

    if (mode == INTSXP) mode = REALSXP;
    if (mode == FUNSXP || mode ==  BUILTINSXP || mode == SPECIALSXP)
	mode = CLOSXP;

    while (rho != R_EmptyEnv) {
        SEXP vl;
	if (! doGet && mode == ANYSXP)
	    vl = findVarInFrame3(rho, symbol, 2);
	else
	    vl = findVarInFrame3(rho, symbol, doGet);
	if (vl != R_UnboundValue) {
	    if (mode == ANYSXP) return vl;
	    if (TYPEOF(vl) == PROMSXP)
                vl = forcePromise(vl);
	    if (mode == CLOSXP && isFunction(vl)) return vl;
	    int tl = TYPEOF(vl);
            if (tl == INTSXP) tl = REALSXP;
	    if (tl == mode) return vl;
	}
	if (inherits)
	    rho = ENCLOS(rho);
	else
	    return (R_UnboundValue);
    }
    return (R_UnboundValue);
}


/*----------------------------------------------------------------------
  ddfindVar

  This function fetches the variables ..1, ..2, etc from the first
  frame of the environment passed as the second argument to ddfindVar.
  These variables are implicitly defined whenever a ... object is
  created.

  To determine values for the variables we first search for an
  explicit definition of the symbol, them we look for a ... object in
  the frame and then walk through it to find the appropriate values.

  If no value is obtained we return R_UnboundValue.

  It is an error to specify a .. index longer than the length of the
  ... object the value is sought in. */

SEXP ddfindVar(SEXP symbol, SEXP rho)
{
    int i;
    SEXP vl;

    /* first look for ... symbol  */
    vl = findVar(R_DotsSymbol, rho);
    i = ddVal(symbol);
    if (vl != R_UnboundValue) {
	if (vl != R_MissingArg && vl != R_MissingUnder && length(vl) >= i) {
	    vl = nthcdr(vl, i - 1);
	    return(CAR(vl));
	}
	else
	    error(_("The ... list does not contain %d elements"), i);
    }
    else error(_("..%d used in an incorrect context, no ... to look in"), i);

    return R_NilValue;
}



/*----------------------------------------------------------------------
  dynamicFindVar

  This function does a variable lookup, but uses dynamic scoping rules
  rather than the lexical scoping rules used in findVar.

  Return R_UnboundValue if the symbol isn't located and the calling
  function needs to handle the errors. */

SEXP dynamicfindVar(SEXP symbol, RCNTXT *cptr)
{
    SEXP vl;
    while (cptr != R_ToplevelContext) {
	if (cptr->callflag & CTXT_FUNCTION) {
	    vl = findVarInFrame3(cptr->cloenv, symbol, TRUE);
	    if (vl != R_UnboundValue) return vl;
	}
	cptr = cptr->nextcontext;
    }

    return R_UnboundValue;
}



/*----------------------------------------------------------------------
  findFun

  Search for a function in an environment. This is a specially modified
  version of findVar which ignores values its finds if they are not
  functions.  This is extremely time-critical code, since it is used
  to lookup all the base language elements, such as "+" and "if".  
  (Though it's often bypassed by the FINDFUN macro in eval.c.)

  In typical usage, rho will be an unhashed local environment with the
  with ENCLOS giving further such environments, until R_GlobalEnv is
  reached, where the function will be found in the global cache (if it
  wasn't in one of the local environemnts). 

  An environment can be skipped when the symbol has LASTENVNOTFOUND
  equal to that environment.  LASTENVNOTFOUND is updated to the last
  unhashed environment where the symbol wasn't found.

  There is no need to wait for computations of the values found to finish, 
  since functions never have their computation deferred. */

SEXP findFun(SEXP symbol, SEXP rho)
{
    return findFun_nospecsym (symbol, SKIP_USING_SYMBITS(rho,symbol));
}

SEXP attribute_hidden findFun_nospecsym(SEXP symbol, SEXP rho)
{
    SEXP32 last_sym_not_found = LASTENVNOTFOUND(symbol);
    SEXP last_unhashed_env_nf = R_NoObject;
    SEXP vl;

    /* Search environments for a definition that is a function. */

    for ( ; rho != R_EmptyEnv; rho = ENCLOS(rho)) {

        /* Handle base environment/namespace quickly, as functions in base
           and other packages will see it rather than the global cache. */

        if (IS_BASE(rho)) {
            vl = SYMBOL_BINDING_VALUE(symbol);
            if (vl != R_UnboundValue) 
                goto got_value;
            continue;
        }

        if (rho == R_GlobalEnv) {
            vl = findGlobalVar(symbol);
            if (vl != R_UnboundValue)
                goto got_value;
            goto err;
        }

        /* See if it's known from LASTENVNOTFOUND that this symbol isn't 
           in this environment. */

        if (SEXP32_FROM_SEXP(rho) == last_sym_not_found) {
            last_unhashed_env_nf = rho;
            continue;
        }

        vl = findVarInFramePendingOK (rho, symbol);
	if (vl != R_UnboundValue)
            goto got_value;

        if (HASHTAB(rho) == R_NilValue)
            last_unhashed_env_nf = rho;
        continue;

    got_value:
        if (TYPEOF(vl) == PROMSXP) {
            SEXP pv = PRVALUE_PENDING_OK(vl);
            vl = pv != R_UnboundValue ? pv : forcePromise(vl);
        }

        if (isFunction (vl)) {
            if (last_unhashed_env_nf != R_NoObject)
                LASTENVNOTFOUND(symbol) = 
                  SEXP32_FROM_SEXP(last_unhashed_env_nf);
            return vl;
        }

        if (vl == R_MissingArg)
            arg_missing_error(symbol);
    }

  err:
    error(_("could not find function \"%s\""), CHAR(PRINTNAME(symbol)));
}


/* Variation on findFun that doesn't report errors itself.
   Used for looking up methods in objects.c. */

SEXP findFunMethod(SEXP symbol, SEXP rho)
{
    SEXP last_unhashed_env_nf = R_NoObject;

    for (rho = SKIP_USING_SYMBITS(rho,symbol); 
         rho != R_EmptyEnv; 
         rho = ENCLOS(rho)) {

        SEXP vl;

        if (rho == R_GlobalEnv) {
            vl = findGlobalVar(symbol);
            if (vl == R_UnboundValue)
                break;
        }
        else {
            vl = findVarInFramePendingOK (rho, symbol);
            if (vl == R_UnboundValue) {
                if (HASHTAB(rho) == R_NilValue)
                    last_unhashed_env_nf = rho;
                continue;
            }
        }

        if (TYPEOF(vl) == PROMSXP)
            vl = forcePromise(vl);
        if (isFunction(vl)) {
            if (last_unhashed_env_nf != R_NoObject)
                LASTENVNOTFOUND(symbol) = 
                  SEXP32_FROM_SEXP(last_unhashed_env_nf);
            return vl;
        }
    }

    if (last_unhashed_env_nf != R_NoObject && !IS_BASE(last_unhashed_env_nf))
        LASTENVNOTFOUND(symbol) = SEXP32_FROM_SEXP(last_unhashed_env_nf);
    return R_UnboundValue;
}

/*----------------------------------------------------------------------
  set_var_in_frame

  Assign a value in a specific environment frame.  If 'create' is TRUE, it
  creates the variable if it doesn't already exist.  May increment or decrement
  NAMEDCNT for the assigned and previous value, depending on the setting of
  'incdec' - 0 = no increment or decrement, 1 = decrement old value only,
  2 = increment new value only, 3 = decrement old value and increment new value.
  Returns TRUE if an assignment was successfully made. 

  Waits for computation to finish for values stored into user databases and 
  into the base environment.

  The symbol, value, and rho arguments are protected by this function. 

  Sets R_binding_cell to the CONS cell for the binding, if there is one,
  and it is suitable for updating. */

int set_var_in_frame (SEXP symbol, SEXP value, SEXP rho, int create, int incdec)
{
    int hashcode;
    SEXP loc;

    PROTECT3(symbol,value,rho);

    R_binding_cell = R_NilValue;

    if (SEXP32_FROM_SEXP(rho) == LASTSYMENV(symbol)) {
        loc = LASTSYMBINDING(symbol);    /* won't be an active binding */
        if (CAR(loc) != R_UnboundValue)  /* could be unbound if var removed */
            goto found;
    }

    if (IS_USER_DATABASE(rho)) {
        /* FIXME: This does not behave as described - DESCRIBED WHERE? HOW? */
        WAIT_UNTIL_COMPUTED(value);
        R_ObjectTable *table;
        table = (R_ObjectTable *) R_ExternalPtrAddr(HASHTAB(rho));
        if (table->assign == NULL) /* maybe should just return FALSE? */
            error(_("cannot assign variables to this database"));
        table->assign (CHAR(PRINTNAME(symbol)), value, table);
        if (incdec&2) 
            INC_NAMEDCNT(value);
        /* don't try to do decrement on old value (getting it might be slow) */

        if (IS_GLOBAL_FRAME(rho)) R_FlushGlobalCache(symbol);

        if (rho == R_GlobalEnv) 
            R_DirtyImage = 1;
        UNPROTECT(3);
        return TRUE; /* unclear whether assign always succeeds, but assume so */
    }

    if (IS_BASE(rho)) {
        if (!create && SYMVALUE(symbol) == R_UnboundValue) {
            UNPROTECT(3);
            return FALSE;
        }
        gsetVar(symbol,value,rho);
        if (incdec&2) 
            INC_NAMEDCNT(value);  
        /* don't bother with decrement on the old value (not time-critical) */
        UNPROTECT(3);
        return TRUE;      /* should have either succeeded, or raised an error */
    }

    if (HASHTAB(rho) == R_NilValue) {
        if (LASTENVNOTFOUND(symbol) != SEXP32_FROM_SEXP(rho)) {
            loc = FRAME(rho);
            SEARCH_LOOP (rho, loc, symbol, goto found_update_last);
        }
    }
    else { /* hashed environment */
        hashcode = SYM_HASH(symbol) % HASHLEN(rho);
        loc = VECTOR_ELT(HASHTAB(rho), hashcode);
        SEARCH_LOOP (rho, loc, symbol, goto found);
    }

    if (create) { /* try to create new variable */

        SEXP new;

        if (rho == R_EmptyEnv)
            error(_("cannot assign values in the empty environment"));

        if (FRAME_IS_LOCKED(rho))
            error(_("cannot add bindings to a locked environment"));

        if (IS_GLOBAL_FRAME(rho)) R_FlushGlobalCache(symbol);

        if (rho == R_GlobalEnv) 
            R_DirtyImage = 1;

        if (HASHTAB(rho) == R_NilValue) {
            new = cons_with_tag (value, FRAME(rho), symbol);
            SET_FRAME(rho, new);
        }
        else {
            SEXP table = HASHTAB(rho);
            new = cons_with_tag (value, R_NilValue, symbol);
            R_HashAddEntry (table, hashcode, new);
            if (R_HashSizeCheck(table))
                SET_HASHTAB(rho, R_HashResize(table));
        }

        SET_ENVSYMBITS (rho, ENVSYMBITS(rho) | SYMBITS(symbol));

        if (LASTENVNOTFOUND(symbol) == SEXP32_FROM_SEXP(rho))
            LASTENVNOTFOUND(symbol) = R_NoObject32;

        if (incdec&2)
            INC_NAMEDCNT(value);

        UNPROTECT(3);
        R_binding_cell = new;
        return TRUE;
    }

    UNPROTECT(3);
    return FALSE;

  found_update_last:
    if ( ! IS_ACTIVE_BINDING(loc) && !DDVAL(symbol)) {
        /* Note:  R_MakeActiveBinding won't let an existing binding 
           become active, so we later assume this can't be active. */
        LASTSYMENV(symbol) = SEXP32_FROM_SEXP(rho);
        LASTSYMBINDING(symbol) = loc;
    }
    
  found:
    if (!IS_ACTIVE_BINDING(loc)) {
        R_binding_cell = BINDING_IS_LOCKED(loc) ? R_NilValue : loc;
        if (incdec&1)
            DEC_NAMEDCNT_AND_PRVALUE(BINDING_VALUE(loc));
    }
    SET_BINDING_VALUE(loc,value);
    if (incdec&2) 
        INC_NAMEDCNT(value);
    SET_MISSING(loc,0);
    if (rho == R_GlobalEnv) 
        R_DirtyImage = 1;
    UNPROTECT(3);
    return TRUE;
}


/*----------------------------------------------------------------------
  set_var_nonlocal

  Assign a value in a specific environment frame or any enclosing frame,
  or create it in R_GlobalEnv if it doesn't exist in any such frame.  May 
  increment or decrement NAMEDCNT for the assigned and previous value, 
  depending on the setting of 'incdec' - 0 = no increment or decrement, 
  1 = decrement old value only, 2 = increment new value only, 3 = decrement 
  old value and increment new value. 

  Sets R_binding_cell as for set_var_in_frame. */

void set_var_nonlocal (SEXP symbol, SEXP value, SEXP rho, int incdec)
{
    while (rho != R_EmptyEnv) {
	if (set_var_in_frame (symbol, value, rho, FALSE, incdec)) 
            return;
	rho = ENCLOS(rho);
    }
    set_var_in_frame (symbol, value, R_GlobalEnv, TRUE, incdec);
}


/*----------------------------------------------------------------------

  OLD FUNCTION PROVIDED FOR BACKWARDS COMPATIBILITY (MENTIONED IN R API).

  defineVar

  Assign a value in a specific environment frame. */

void defineVar(SEXP symbol, SEXP value, SEXP rho)
{
    set_var_in_frame (symbol, value, rho, TRUE, 0);
}


/*----------------------------------------------------------------------

  OLD FUNCTION PROVIDED FOR BACKWARDS COMPATIBILITY (MENTIONED IN R API).

  setVar

  Assign a new value to bound symbol.	 Note this does the "inherits"
  case.  I.e. it searches frame-by-frame for a symbol and binds the
  given value to the first symbol encountered.  If no symbol is
  found then a binding is created in the global environment. */

void setVar(SEXP symbol, SEXP value, SEXP rho)
{
    while (rho != R_EmptyEnv) {
	if (set_var_in_frame (symbol, value, rho, FALSE, 0)) 
            return;
	rho = ENCLOS(rho);
    }
    set_var_in_frame (symbol, value, R_GlobalEnv, TRUE, 0);
}


/*----------------------------------------------------------------------
  gsetVar

  Assignment directly into the base environment.  

  Waits until the value has been computed. */

void gsetVar(SEXP symbol, SEXP value, SEXP rho)
{
    if (TYPEOF(symbol) != SYMSXP) abort();

    if (SYMVALUE(symbol) == R_UnboundValue) {
        if (FRAME_IS_LOCKED(rho))
            error(_("cannot add binding of '%s' to the base environment"),
                  CHAR(PRINTNAME(symbol)));
    }

    R_FlushGlobalCache(symbol);

    WAIT_UNTIL_COMPUTED(value);
    SET_SYMBOL_BINDING_VALUE(symbol, value);
}


/*---------------------------------------------------------------------
   Remove variable and return its previous value, or R_NoObject if it
   didn't exist.  For a user database, R_NilValue is returned when the
   variable exists, rather than the value. */

SEXP attribute_hidden RemoveVariable(SEXP name, SEXP env)
{
    SEXP list, value;

    if (TYPEOF(name) != SYMSXP) abort();

    if (env == R_BaseNamespace)
	error(_("cannot remove variables from base namespace"));
    if (env == R_BaseEnv)
	error(_("cannot remove variables from the base environment"));
    if (env == R_EmptyEnv)
	error(_("cannot remove variables from the empty environment"));
    if (FRAME_IS_LOCKED(env))
	error(_("cannot remove bindings from a locked environment"));

    if(IS_USER_DATABASE(env)) {
	R_ObjectTable *table;
	table = (R_ObjectTable *) R_ExternalPtrAddr(HASHTAB(env));
	if(table->remove == NULL)
	    error(_("cannot remove variables from this database"));
	return table->remove(CHAR(PRINTNAME(name)), table) 
                 ? R_NilValue : R_NoObject;
    }

    if (IS_HASHED(env)) {
	SEXP hashtab = HASHTAB(env);
        int hashcode = SYM_HASH(name);
	int idx = hashcode % HASHLEN(env);
	list = RemoveFromList(name, VECTOR_ELT(hashtab, idx), &value);
	if (value != R_NoObject) {
	    SET_VECTOR_ELT(hashtab, idx, list);
            if (list == R_NilValue)
                SET_HASHSLOTSUSED(hashtab,HASHSLOTSUSED(hashtab)-1);
        }
    }
    else {
	list = RemoveFromList(name, FRAME(env), &value);
	if (value != R_NoObject)
	    SET_FRAME(env, list);
    }

    if (value != R_NoObject) {
        if(env == R_GlobalEnv) R_DirtyImage = 1;
	if (IS_GLOBAL_FRAME(env)) {
            PROTECT(value);
            R_FlushGlobalCache(name);
            UNPROTECT(1);
        }
    }

    return value;
}

/*----------------------------------------------------------------------
  do_attach

  To attach a list we make up an environment and insert components
  of the list in as the values of this env and install the tags from
  the list as the names. */

static SEXP do_attach(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP name, s, t, x;
    int pos, hsize;
    Rboolean isSpecial;

    checkArity(op, args);

    pos = asInteger(CADR(args));
    if (pos == NA_INTEGER)
	error(_("'pos' must be an integer"));

    name = CADDR(args);
    if (!isValidStringF(name))
	error(_("invalid '%s' argument"), "name");

    isSpecial = IS_USER_DATABASE(CAR(args));

    if(!isSpecial) {
	if (isNewList(CAR(args))) {
	    SETCAR(args, VectorToPairList(CAR(args)));

	    for (x = CAR(args); x != R_NilValue; x = CDR(x))
		if (TAG(x) == R_NilValue)
		    error(_("all elements of a list must be named"));
	    PROTECT(s = allocSExp(ENVSXP));
	    SET_FRAME(s, duplicate(CAR(args)));
            set_symbits_in_env(s);
	} else if (isEnvironment(CAR(args))) {
	    SEXP p, loadenv = CAR(args);

	    PROTECT(s = allocSExp(ENVSXP));
            set_symbits_in_env(s);  /* will get set to 0s, since nothing yet */
	    if (HASHTAB(loadenv) != R_NilValue) {
		int i, n;
		n = length(HASHTAB(loadenv));
		for (i = 0; i < n; i++) {
		    p = VECTOR_ELT(HASHTAB(loadenv), i);
		    while (p != R_NilValue) {
			defineVar(TAG(p), duplicate(CAR(p)), s);
			p = CDR(p);
		    }
		}
		/* FIXME: duplicate the hash table and assign here */
	    } else {
		for(p = FRAME(loadenv); p != R_NilValue; p = CDR(p))
		    defineVar(TAG(p), duplicate(CAR(p)), s);
	    }
	} 
        else
	    error(_("'attach' only works for lists, data frames and environments"));

	/* Connect FRAME(s) into HASHTAB(s) */
        hsize = (int) (length(s)/0.6);   /* about 45% of entries will be used */
	if (hsize < HASHMINSIZE)
	    hsize = HASHMINSIZE;

	SET_HASHTAB(s, R_NewHashTable(hsize));
	R_HashFrame(s);

	while (R_HashSizeCheck(HASHTAB(s)))
	    SET_HASHTAB(s, R_HashResize(HASHTAB(s)));

    } else { /* is a user object */
	/* Having this here (rather than below) means that the onAttach routine
	   is called before the table is attached. This may not be necessary or
	   desirable. */
	R_ObjectTable *tb = (R_ObjectTable*) R_ExternalPtrAddr(CAR(args));
	if(tb->onAttach)
	    tb->onAttach(tb);
	PROTECT(s = allocSExp(ENVSXP));
	SET_HASHTAB(s, CAR(args));
	setAttrib(s, R_ClassSymbol, getClassAttrib(HASHTAB(s)));
    }

    setAttrib(s, R_NameSymbol, name);
    for (t = R_GlobalEnv; ENCLOS(t) != R_BaseEnv && pos > 2; t = ENCLOS(t))
	pos--;

    if (ENCLOS(t) == R_BaseEnv) {
	SET_ENCLOS(t, s);
	SET_ENCLOS(s, R_BaseEnv);
    }
    else {
	x = ENCLOS(t);
	SET_ENCLOS(t, s);
	SET_ENCLOS(s, x);
    }

    if(!isSpecial) { /* Temporary: need to remove the elements identified by objects(CAR(args)) */
	R_FlushGlobalCacheFromTable(HASHTAB(s));
	MARK_AS_GLOBAL_FRAME(s);
    } else {
	R_FlushGlobalCacheFromUserTable(HASHTAB(s));
	MARK_AS_GLOBAL_FRAME(s);
    }

    UNPROTECT(1); /* s */
    return s;
}

/*----------------------------------------------------------------------
  do_detach

  detach the specified environment.  Detachment only takes place by
  position. */

static SEXP do_detach(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP s, t, x;
    int pos, n;
    Rboolean isSpecial = FALSE;

    checkArity(op, args);
    pos = asInteger(CAR(args));

    for (n = 2, t = ENCLOS(R_GlobalEnv); t != R_BaseEnv; t = ENCLOS(t))
	n++;

    if (pos == n) /* n is the length of the search list */
	error(_("detaching \"package:base\" is not allowed"));

    for (t = R_GlobalEnv ; ENCLOS(t) != R_BaseEnv && pos > 2 ; t = ENCLOS(t))
	pos--;
    if (pos != 2)
	error(_("invalid '%s' argument"), "pos");
    else {
	PROTECT(s = ENCLOS(t));
	x = ENCLOS(s);
	SET_ENCLOS(t, x);
	isSpecial = IS_USER_DATABASE(s);
	if(isSpecial) {
	    R_ObjectTable *tb = (R_ObjectTable*) R_ExternalPtrAddr(HASHTAB(s));
	    if(tb->onDetach) tb->onDetach(tb);
	}

	SET_ENCLOS(s, R_BaseEnv);
    }

    if(!isSpecial) {
	R_FlushGlobalCacheFromTable(HASHTAB(s));
	MARK_AS_LOCAL_FRAME(s);
    } else {
	R_FlushGlobalCacheFromUserTable(HASHTAB(s));
	MARK_AS_LOCAL_FRAME(s); /* was _GLOBAL_ prior to 2.4.0 */
    }

    UNPROTECT(1);
    return s;
}

/*----------------------------------------------------------------------
  This is a .Internal with no wrapper, currently unused in base R */

static SEXP do_mkUnbound(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP sym;
    checkArity(op, args);
    sym = CAR(args);

    if (TYPEOF(sym) != SYMSXP) error(_("not a symbol"));
    /* This is not quite the same as SET_SYMBOL_BINDING_VALUE as it
       does not allow active bindings to be unbound */
    if (R_BindingIsLocked(sym, R_BaseEnv))
	error(_("cannot unbind a locked binding"));
    if (R_BindingIsActive(sym, R_BaseEnv))
	error(_("cannot unbind an active binding"));
    SET_SYMVALUE(sym, R_UnboundValue);

    R_FlushGlobalCache(sym);

    return R_NilValue;
}


/*--------------------------------------------------------------------------- */
/* FUNTAB entries defined in this source file. See names.c for documentation. */

attribute_hidden FUNTAB R_FunTab_eval[] =
{
/* printname	c-entry		offset	eval	arity	pp-kind	     precedence	rightassoc */

{"if",		do_if,		0,	1200,	-1,	{PP_IF,	     PREC_FN,	  1}},
{"for",		do_for,		0,	100,	-1,	{PP_FOR,     PREC_FN,	  0}},
{"while",	do_while,	0,	100,	-1,	{PP_WHILE,   PREC_FN,	  0}},
{"repeat",	do_repeat,	0,	100,	-1,	{PP_REPEAT,  PREC_FN,	  0}},
{"break",	do_break, CTXT_BREAK,	0,	-1,	{PP_BREAK,   PREC_FN,	  0}},
{"next",	do_break, CTXT_NEXT,	0,	-1,	{PP_NEXT,    PREC_FN,	  0}},
{"(",		do_paren,	0,	1000,	1,	{PP_PAREN,   PREC_FN,	  0}},
{"{",		do_begin,	0,	1200,	-1,	{PP_CURLY,   PREC_FN,	  0}},
{"return",	do_return,	0,	1200,	-1,	{PP_RETURN,  PREC_FN,	  0}},
{"function",	do_function,	0,	0,	-1,	{PP_FUNCTION,PREC_FN,	  0}},
{"<-",		do_set,		1,	1100,	2,	{PP_ASSIGN,  PREC_LEFT,	  1}},
{"=",		do_set,		3,	1100,	2,	{PP_ASSIGN,  PREC_EQ,	  1}},
{"<<-",		do_set,		2,	1100,	2,	{PP_ASSIGN2, PREC_LEFT,	  1}},
{"->",		do_set,		11,	1100,	2,	{PP_ASSIGN,  PREC_RIGHT,	  1}},
{"->>",		do_set,		12,	1100,	2,	{PP_ASSIGN2, PREC_RIGHT,	  1}},
{"eval",	do_eval,	0,	1211,	3,	{PP_FUNCALL, PREC_FN,	0}},
{"eval.with.vis",do_eval,	1,	1211,	3,	{PP_FUNCALL, PREC_FN,	0}},
{"Recall",	do_recall,	0,	210,	-1,	{PP_FUNCALL, PREC_FN,	  0}},

{"Rprof",	do_Rprof,	0,	11,	4,	{PP_FUNCALL, PREC_FN,	0}},
{"withVisible", do_withVisible,	1,	10,	1,	{PP_FUNCALL, PREC_FN,	0}},

/* Logical Operators, all primitives */
/* these are group generic and so need to eval args (as builtin or themselves)*/

{"&",		do_andor,	1,	1000,	2,	{PP_BINARY,  PREC_AND,	  0}},
{"|",		do_andor,	2,	1000,	2,	{PP_BINARY,  PREC_OR,	  0}},
{"!",		do_not,		1,	1001,	1,	{PP_UNARY,   PREC_NOT,	  0}},

/* specials as conditionally evaluate second arg */
{"&&",		do_andor2,	1,	0,	2,	{PP_BINARY,  PREC_AND,	  0}},
{"||",		do_andor2,	2,	0,	2,	{PP_BINARY,  PREC_OR,	  0}},

/* these are group generic and so need to eval args */
{"all",		do_allany,	1,	1,	-1,	{PP_FUNCALL, PREC_FN,	  0}},
{"any",		do_allany,	2,	1,	-1,	{PP_FUNCALL, PREC_FN,	  0}},

/* Arithmetic Operators, all primitives, now special, though always eval args */

{"+",		do_arith,	PLUSOP,	1000,	2,	{PP_BINARY,  PREC_SUM,	  0}},
{"-",		do_arith,	MINUSOP,1000,	2,	{PP_BINARY,  PREC_SUM,	  0}},
{"*",		do_arith,	TIMESOP,1000,	2,	{PP_BINARY,  PREC_PROD,	  0}},
{"/",		do_arith,	DIVOP,	1000,	2,	{PP_BINARY2, PREC_PROD,	  0}},
{"^",		do_arith,	POWOP,	1000,	2,	{PP_BINARY2, PREC_POWER,  1}},
{"%%",		do_arith,	MODOP,	1000,	2,	{PP_BINARY2, PREC_PERCENT,0}},
{"%/%",		do_arith,	IDIVOP,	1000,	2,	{PP_BINARY2, PREC_PERCENT,0}},

/* Relational Operators, all primitives */
/* these are group generic and so need to eval args (inside, as special) */

{"==",		do_relop,	EQOP,	1000,	2,	{PP_BINARY,  PREC_COMPARE,0}},
{"!=",		do_relop,	NEOP,	1000,	2,	{PP_BINARY,  PREC_COMPARE,0}},
{"<",		do_relop,	LTOP,	1000,	2,	{PP_BINARY,  PREC_COMPARE,0}},
{"<=",		do_relop,	LEOP,	1000,	2,	{PP_BINARY,  PREC_COMPARE,0}},
{">=",		do_relop,	GEOP,	1000,	2,	{PP_BINARY,  PREC_COMPARE,0}},
{">",		do_relop,	GTOP,	1000,	2,	{PP_BINARY,  PREC_COMPARE,0}},

/* Environment related. */

{"attach",	do_attach,	0,	111,	3,	{PP_FUNCALL, PREC_FN,	0}},
{"detach",	do_detach,	0,	111,	1,	{PP_FUNCALL, PREC_FN,	0}},
{"mkUnbound",	do_mkUnbound,		0, 111,	1,      {PP_FUNCALL, PREC_FN,	0}},
{"env.profile",  do_envprofile,    0,	211,	1,	{PP_FUNCALL, PREC_FN,	0}},

{NULL,		NULL,		0,	0,	0,	{PP_INVALID, PREC_FN,	0}},
};

/* Fast built-in functions in this file. See names.c for documentation */

attribute_hidden FASTFUNTAB R_FastFunTab_eval[] = {
/*slow func	fast func,     code or -1   dsptch  variant */
{ do_not,	do_fast_not,	-1,		1,  VARIANT_PENDING_OK },
{ do_allany,	do_fast_allany,	OP_ALL,		1,  VARIANT_AND },
{ do_allany,	do_fast_allany,	OP_ANY,		1,  VARIANT_OR },
{ 0,		0,		0,		0,  0 }
};
