/*
 *  pqR : A pretty quick version of R
 *  Copyright (C) 2013, 2014, 2015, 2016, 2017, 2018 by Radford M. Neal
 *
 *  Based on R : A Computer Language for Statistical Data Analysis
 *  Copyright (C) 1995--2012  The R Core Team
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

/* <UTF8> byte-level access is only to compare with chars <= 0x7F */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define NEED_CONNECTION_PSTREAMS
#define USE_FAST_PROTECT_MACROS
#define R_USE_SIGNALS 1
#include <Defn.h>
#include <Rmath.h>
#include <Fileio.h>
#include <Rversion.h>
#include <R_ext/RS.h>           /* for CallocCharBuf, Free */
#include <errno.h>
#include <ctype.h>		/* for isspace */

/* From time to time changes in R, such as the addition of a new SXP,
 * may require changes in the save file format.  Here are some
 * guidelines on handling format changes:
 *
 *    Starting with 1.4 there is a version number associated with save
 *    file formats.  This version number should be incremented when
 *    the format is changed so older versions of R can recognize and
 *    reject the new format with a meaningful error message.
 *
 *    R should remain able to write older workspace formats.  An error
 *    should be signaled if the contents to be saved is not compatible
 *    with the requested format.
 *
 *    To allow older versions of R to give useful error messages, the
 *    header now contains the version of R that wrote the workspace
 *    and the oldest version that can read the workspace.  These
 *    versions are stored as an integer packed by the R_Version macro
 *    from Rversion.h.  Some workspace formats may only exist
 *    temporarily in the development stage.  If readers are not
 *    provided in a release version, then these should specify the
 *    oldest reader R version as -1.
 */


/* ----- V e r s i o n -- T w o -- S a v e / R e s t o r e ----- */

/* Adapted from Chris Young and Ross Ihaka's Version One by Luke
   Tierney.  Copyright Assigned to the R Project.

   The approach used here uses a single pass over the node tree to be
   serialized.  Sharing of reference objects is preserved, but sharing
   among other objects is ignored.  The first time a reference object
   is encountered it is entered in a hash table; the value stored with
   the object is the index in the sequence of reference objects (1 for
   first reference object, 2 for second, etc.).  When an object is
   seen again, i.e. it is already in the hash table, a reference
   marker along with the index is written out.  The unserialize code
   does not know in advance how many reference objects it will see, so
   it starts with an initial array of some reasonable size and doubles
   it each time space runs out.  Reference objects are entered as they
   are encountered.

   This means the serialize and unserialize code needs to agree on
   what is a reference object.  Making a non-reference object into
   a reference object requires a version change in the format.  An
   alternate design would be to precede each reference object with a
   marker that says the next thing is a possibly shared object and
   needs to be entered into the reference table.

   Adding new SXP types is easy, whether they are reference objects or
   not.  The unserialize code will signal an error if it sees a type
   value it does not know.  It is of course better to increment the
   serialization format number when a new SXP is added, but if that
   SXP is unlikely to be saved by users then it may be simpler to keep
   the version number and let the error handling code deal with it.

   The output format for dotted pairs writes the ATTRIB value first
   rather than last.  This allows CDR's to be processed by iterative
   tail calls to avoid recursion stack overflows when processing long
   lists.  Both the writing code and the reading code take advantage 
   of this.

   CHARSXPs are now handled in a way that preserves both embedded null
   characters and NA_STRING values.

   The "XDR" save format no longer uses the XDR routines, which are
   slow and cumbersome.  Conversion to big-endian representation is
   accomplished with routines in this module.

   The output format packs the type flag and other flags into a single
   integer.  This produces more compact output for code; it has little
   effect on data.

   Environments recognized as package or namespace environments are
   not saved directly. Instead, a STRSXP is saved that is then used to
   attempt to find the package/namespace when unserialized.  The
   exact mechanism for choosing the name and finding the package/name
   space from the name still has to be developed, but the
   serialization format should be able to accommodate any reasonable
   mechanism.

   The mechanism assumes that user code supplies one routine for
   handling single characters and one for handling an array of bytes.
   Higher level interfaces that serialize to/from a FILE * pointer or
   an Rconnection pointer are provided in this file; others can be
   built easily.

   A mechanism is provided to allow special handling of non-system
   reference objects (all weak references and external pointers, and
   all environments other than package environments, namespace
   environments, and the global environment).  The hook function
   consists of a function pointer and a data value.  The serialization
   function pointer is called with the reference object and the data
   value as arguments.  It should return R_NilValue for standard
   handling and an STRSXP for special handling.  In an STRSXP is
   returned, then a special handing mark is written followed by the
   strings in the STRSXP (attributes are ignored).  On unserializing,
   any specially marked entry causes a call to the hook function with
   the reconstructed STRSXP and data value as arguments.  This should
   return the value to use for the reference object.  A reasonable
   convention on how to use this mechanism is neded, but again the
   format should be compatible with any reasonable convention.

   Eventually it may be useful to use these hooks to allow objects
   with a class to have a class-specific serialization mechanism.  The
   serialization format should support this.  It is trickier than in
   Java and other reference based languages where creation and
   initialization can be separated--we don't really have that option
   at the R level.  */


/* FORWARD DECLARATIONS. */

struct outpar { 
    R_outpstream_t stream;
    SEXP ref_table;
    int nosharing;
    char *buf;
};

struct inpar {
    R_inpstream_t stream;
    SEXP ref_table;
    char *buf;
};

static void OutStringVec (struct outpar *par, SEXP s);
static void WriteItem  (struct outpar *par, SEXP s);
static SEXP InStringVec (struct inpar *par);
static SEXP ReadItem (struct inpar *par);
static void WriteBC (struct outpar *par, SEXP s);
static SEXP ReadBC (struct inpar *par);


/* CONSTANTS. */

/* The default version used when a stream Init function is called with
   version = 0 */

static const int R_DefaultSerializeVersion = 2;


/* UTILITY FUNCTIONS. */

/* An assert function which doesn't crash the program.
 * Something like this might be useful in an R header file
 */

#ifdef NDEBUG
#define R_assert(e) ((void) 0)
#else
/* The line below requires an ANSI C preprocessor (stringify operator) */
#define R_assert(e) ((e) ? (void) 0 : error("assertion '%s' failed: file '%s', line %d\n", #e, __FILE__, __LINE__))
#endif /* NDEBUG */

/* Rsnprintf: like snprintf, but guaranteed to null-terminate. */
static int Rsnprintf(char *buf, int size, const char *format, ...)
{
    int val;
    va_list ap;
    va_start(ap, format);
    /* On Windows this uses the non-C99 MSVCRT.dll version, which is OK */
    val = vsnprintf(buf, size, format, ap);
    buf[size-1] = '\0';
    va_end(ap);
    return val;
}


/* HANDLE XDR ENCODE/DECODE FOR BOTH BIG AND LITTLE ENDIAN MACHINES.

   The "XDR" format is big-endian, but most processors are now little-endian. 
   Handles both below, but may not work for more bizarre mixed-endian machines.
*/

static inline void encode_integer(int i, void *buf)
{
#   if __GNUC__ && defined(__BYTE_ORDER__)
#       if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
            i = __builtin_bswap32(i);
#       endif
        memcpy(buf,&i,sizeof(int));
#   else
        ((signed char *)buf)[0] = i >> 24;
        ((unsigned char *)buf)[1] = (i >> 16) & 0xff;
        ((unsigned char *)buf)[2] = (i >> 8) & 0xff;
        ((unsigned char *)buf)[3] = i & 0xff;
#   endif
}

static inline void encode_double(double d, void *buf)
{
    uint64_t u;
    memcpy(&u,&d,8);

#   if __GNUC__ && defined(__FLOAT_WORD_ORDER__)
#       if __FLOAT_WORD_ORDER__ == __ORDER_LITTLE_ENDIAN__
            u = __builtin_bswap64(u);
#       endif
        memcpy(buf,&u,sizeof(double));
#   else
        ((unsigned char *)buf)[0] = (u >> 56) & 0xff;
        ((unsigned char *)buf)[1] = (u >> 48) & 0xff;
        ((unsigned char *)buf)[2] = (u >> 40) & 0xff;
        ((unsigned char *)buf)[3] = (u >> 32) & 0xff;
        ((unsigned char *)buf)[4] = (u >> 24) & 0xff;
        ((unsigned char *)buf)[5] = (u >> 16) & 0xff;
        ((unsigned char *)buf)[6] = (u >> 8) & 0xff;
        ((unsigned char *)buf)[7] = u & 0xff;
#   endif
}

static inline int decode_integer(void *buf)
{
    int i;

#   if __GNUC__ && defined(__BYTE_ORDER__)
        memcpy(&i,buf,sizeof(int));
#       if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
            i = __builtin_bswap32(i);
#       endif
#   else
        i = ((int)(((signed char *)buf)[0]) << 24) |
            ((int)(((unsigned char *)buf)[1]) << 16) |
            ((int)(((unsigned char *)buf)[2]) << 8) |
            (int)(((unsigned char *)buf)[3]);
#   endif

    return i;
}

static inline double decode_double(void *buf)
{
    uint64_t u;

#   if __GNUC__ && defined(__FLOAT_WORD_ORDER__)
        memcpy(&u,buf,sizeof(double));
#       if __FLOAT_WORD_ORDER__ == __ORDER_LITTLE_ENDIAN__
            u = __builtin_bswap64(u);
#       endif
#   else
        u = ((uint64_t)(((unsigned char *)buf)[0]) << 56) |
            ((uint64_t)(((unsigned char *)buf)[1]) << 48) |
            ((uint64_t)(((unsigned char *)buf)[2]) << 40) |
            ((uint64_t)(((unsigned char *)buf)[3]) << 32) |
            ((uint64_t)(((unsigned char *)buf)[4]) << 24) |
            ((uint64_t)(((unsigned char *)buf)[5]) << 16) |
            ((uint64_t)(((unsigned char *)buf)[6]) << 8) |
            (uint64_t)(((unsigned char *)buf)[7]);
#   endif

    double d;
    memcpy(&d,&u,8);
    return d;
}

static attribute_noinline void encode_doubles (double *d, int n, void *buf)
{
    double *p = (double *) buf;
    int i;
    for (i = 0; i < n; i++) {
        encode_double (*d, (void *)p);
        p += 1;
        d += 1;
    }
}

static attribute_noinline void decode_doubles (double *d, int n, void *buf)
{
    double *p = (double *) buf;
    int i;
    for (i = 0; i < n; i++) {
        *d = decode_double ((void *)p);
        p += 1;
        d += 1;
    }
}


/* SIZE OF BUFFERS USED BY INPUT/OUTPUT ROUTINES. */

#define CHUNK_SIZE 1024
#define CBUF_SIZE (CHUNK_SIZE * sizeof (Rcomplex))  /* Rcomplex is biggest */


/* BASIC OUTPUT ROUTINES. */

static attribute_noinline void OutInteger (struct outpar *par, int i)
{
    R_outpstream_t stream = par->stream;
    char *buf = par->buf;

    switch (stream->type) {
    case R_pstream_ascii_format:
	if (i == NA_INTEGER)
	    Rsnprintf(buf, CBUF_SIZE, "NA\n");
	else
	    Rsnprintf(buf, CBUF_SIZE, "%d\n", i);
	stream->OutBytes(stream, buf, strlen(buf));
	break;
    case R_pstream_binary_format:
	stream->OutBytes(stream, &i, sizeof (int));
	break;
    case R_pstream_xdr_format:
	encode_integer(i, buf);
	stream->OutBytes(stream, buf, sizeof (int));
	break;
    default:
	error(_("unknown or inappropriate output format"));
    }
}

static attribute_noinline void OutReal (struct outpar *par, double d)
{
    R_outpstream_t stream = par->stream;
    char *buf = par->buf;

    switch (stream->type) {
    case R_pstream_ascii_format:
	if (! R_FINITE(d)) {
	    if (ISNAN(d))
		Rsnprintf(buf, CBUF_SIZE, "NA\n");
	    else if (d < 0)
		Rsnprintf(buf, CBUF_SIZE, "-Inf\n");
	    else
		Rsnprintf(buf, CBUF_SIZE, "Inf\n");
	}
	else
	    /* 16: full precision; 17 gives 999, 000 &c */
	    Rsnprintf(buf, CBUF_SIZE, "%.16g\n", d);
	stream->OutBytes(stream, buf, strlen(buf));
	break;
    case R_pstream_binary_format:
	stream->OutBytes(stream, &d, sizeof (double));
	break;
    case R_pstream_xdr_format:
	encode_double (d, buf);
	stream->OutBytes(stream, buf, sizeof (double));
	break;
    default:
	error(_("unknown or inappropriate output format"));
    }
}

static void OutComplex (struct outpar *par, Rcomplex c)
{
    OutReal(par, c.r);
    OutReal(par, c.i);
}

static attribute_noinline void OutByte (struct outpar *par, Rbyte i)
{
    R_outpstream_t stream = par->stream;
    char *buf = par->buf;

    switch (stream->type) {
    case R_pstream_ascii_format:
	Rsnprintf(buf, CBUF_SIZE, "%02x\n", i);
	stream->OutBytes(stream, buf, strlen(buf));
	break;
    case R_pstream_binary_format:
    case R_pstream_xdr_format:
	stream->OutBytes(stream, &i, 1);
	break;
    default:
	error(_("unknown or inappropriate output format"));
    }
}

static attribute_noinline void OutString (struct outpar *par, const char *s,
                                          int length)
{
    R_outpstream_t stream = par->stream;
    char *buf = par->buf;

    OutInteger (par, length);

    if (stream->type == R_pstream_ascii_format) {
	int i;
	for (i = 0; i < length; i++) {
	    switch(s[i]) {
	    case '\n': sprintf(buf, "\\n");  break;
	    case '\t': sprintf(buf, "\\t");  break;
	    case '\v': sprintf(buf, "\\v");  break;
	    case '\b': sprintf(buf, "\\b");  break;
	    case '\r': sprintf(buf, "\\r");  break;
	    case '\f': sprintf(buf, "\\f");  break;
	    case '\a': sprintf(buf, "\\a");  break;
	    case '\\': sprintf(buf, "\\\\"); break;
	    case '\?': sprintf(buf, "\\?");  break;
	    case '\'': sprintf(buf, "\\'");  break;
	    case '\"': sprintf(buf, "\\\""); break;
	    default  :
		/* cannot print char in octal mode -> cast to unsigned
		   char first */
		/* actually, since s is signed char and '\?' == 127
		   is handled above, s[i] > 126 can't happen, but
		   I'm superstitious...  -pd */
		if (s[i] <= 32 || s[i] > 126)
		    sprintf(buf, "\\%03o", (unsigned char) s[i]);
		else
		    sprintf(buf, "%c", s[i]);
	    }
	    stream->OutBytes(stream, buf, strlen(buf));
	}
	stream->OutChar(stream, '\n');
    }
    else
	stream->OutBytes(stream, (void *)s, length); /* FIXME: is this case right? */
}


/* BASIC INPUT ROUTINES. */

static void InWord (R_inpstream_t stream, char *word, int size)
{
    int c, i;
    i = 0;
    do {
	c = stream->InChar(stream);
	if (c == EOF)
	    error(_("read error"));
    } while (isspace(c));
    while (! isspace(c) && i < size) {
	word[i++] = c;
	c = stream->InChar(stream);
    }
    if (i == size)
	error(_("read error"));
    word[i] = 0;
}

static char word[100];  /* used in InInteger and InReal */

static attribute_noinline int InInteger (struct inpar *par)
{
    R_inpstream_t stream = par->stream;
    char *buf = par->buf;

    int i;

    switch (stream->type) {
    case R_pstream_ascii_format: ;
	InWord(stream, buf, CBUF_SIZE);
	sscanf(buf, "%s", word);
	if (strcmp(word, "NA") == 0)
	    return NA_INTEGER;
	else
	    sscanf(word, "%d", &i);
	return i;
    case R_pstream_binary_format:
	stream->InBytes(stream, &i, sizeof (int));
	return i;
    case R_pstream_xdr_format:
	stream->InBytes(stream, buf, sizeof (int));
	return decode_integer(buf);
    default:
	return NA_INTEGER;
    }
}

static attribute_noinline double InReal (struct inpar *par)
{
    R_inpstream_t stream = par->stream;
    char *buf = par->buf;

    double d;

    switch (stream->type) {
    case R_pstream_ascii_format: ;
	InWord(stream, buf, CBUF_SIZE);
	sscanf(buf, "%s", word);
	if (strcmp(word, "NA") == 0)
	    return NA_REAL;
	else if (strcmp(word, "Inf") == 0)
	    return R_PosInf;
	else if (strcmp(word, "-Inf") == 0)
	    return R_NegInf;
	else
	    sscanf(word, "%lg", &d);
	return d;
    case R_pstream_binary_format:
	stream->InBytes(stream, &d, sizeof (double));
	return d;
    case R_pstream_xdr_format:
	stream->InBytes(stream, buf, sizeof (double));
	return decode_double (buf);
    default:
	return NA_REAL;
    }
}

static Rcomplex InComplex (struct inpar *par)
{
    Rcomplex c;
    c.r = InReal(par);
    c.i = InReal(par);
    return c;
}

/* These utilities for reading characters with an unget option are
   defined so the code in InString can match the code in
   saveload.c:InStringAscii--that way it is easier to match changes in
   one to the other. */
typedef struct R_instring_stream_st {
    int last;
    R_inpstream_t stream;
} *R_instring_stream_t;

static void InitInStringStream(R_instring_stream_t s, R_inpstream_t stream)
{
    s->last = EOF;
    s->stream = stream;
}

static R_INLINE int GetChar(R_instring_stream_t s)
{
    int c;
    if (s->last != EOF) {
	c = s->last;
	s->last = EOF;
    }
    else c = s->stream->InChar(s->stream);
    return c;
}

static R_INLINE void UngetChar(R_instring_stream_t s, int c)
{
    s->last = c;
}


static attribute_noinline void InString (R_inpstream_t stream, char *buf, int length)
{
    if (stream->type == R_pstream_ascii_format) {
	if (length > 0) {
	    int c, d, i, j;
	    struct R_instring_stream_st iss;

	    InitInStringStream(&iss, stream);
	    while(isspace(c = GetChar(&iss)))
		;
	    UngetChar(&iss, c);
	    for (i = 0; i < length; i++) {
		if ((c =  GetChar(&iss)) == '\\') {
		    switch(c = GetChar(&iss)) {
		    case 'n' : buf[i] = '\n'; break;
		    case 't' : buf[i] = '\t'; break;
		    case 'v' : buf[i] = '\v'; break;
		    case 'b' : buf[i] = '\b'; break;
		    case 'r' : buf[i] = '\r'; break;
		    case 'f' : buf[i] = '\f'; break;
		    case 'a' : buf[i] = '\a'; break;
		    case '\\': buf[i] = '\\'; break;
		    case '?' : buf[i] = '\?'; break;
		    case '\'': buf[i] = '\''; break;
		    case '\"': buf[i] = '\"'; break; /* closing " for emacs */
		    case '0': case '1': case '2': case '3':
		    case '4': case '5': case '6': case '7':
			d = 0; j = 0;
			while('0' <= c && c < '8' && j < 3) {
			    d = d * 8 + (c - '0');
			    c = GetChar(&iss);
			    j++;
			}
			buf[i] = d;
			UngetChar(&iss, c);
			break;
		    default  : buf[i] = c;
		    }
		}
		else buf[i] = c;
	    }
	}
    }
    else  /* this limits the string length: used for CHARSXPs */
	stream->InBytes(stream, buf, length);
}


/*
 * Format Header Reading and Writing
 *
 * The header starts with one of three characters, A for ascii, B for
 * binary, or X for xdr.
 */

static void OutFormat(R_outpstream_t stream)
{
/*    if (stream->type == R_pstream_binary_format) {
	warning(_("binary format is deprecated; using xdr instead"));
	stream->type = R_pstream_xdr_format;
	} */
    switch (stream->type) {
    case R_pstream_ascii_format:  stream->OutBytes(stream, "A\n", 2); break;
    case R_pstream_binary_format: stream->OutBytes(stream, "B\n", 2); break;
    case R_pstream_xdr_format:    stream->OutBytes(stream, "X\n", 2); break;
    case R_pstream_any_format:
	error(_("must specify ascii, binary, or xdr format"));
    default: error(_("unknown output format"));
    }
}

static void InFormat(R_inpstream_t stream)
{
    char buf[2];
    R_pstream_format_t type;
    stream->InBytes(stream, buf, 2);
    switch (buf[0]) {
    case 'A': type = R_pstream_ascii_format; break;
    case 'B': type = R_pstream_binary_format; break;
    case 'X': type = R_pstream_xdr_format; break;
    case '\n':
	/* GROSS HACK: ASCII unserialize may leave a trailing newline
	   in the stream.  If the stream contains a second
	   serialization, then a second unserialize will fail if such
	   a newline is present.  The right fix is to make sure
	   unserialize consumes exactly what serialize produces.  But
	   this seems hard because of the current use of whitespace
	   skipping in unserialize.  So a temporary hack to cure the
	   symptom is to deal with a possible leading newline.  I
	   don't think more than one is possible, but I'm not sure.
	   LT */
	if (buf[1] == 'A') {
	    type = R_pstream_ascii_format;
	    stream->InBytes(stream, buf, 1);
	    break;
	}
    default:
	error(_("unknown input format"));
    }
    if (stream->type == R_pstream_any_format)
	stream->type = type;
    else if (type != stream->type)
	error(_("input format does not match specified format"));
}


/* HASH TABLE FUNCTIONS.

   Hashing functions for hashing reference objects during writing.
   Objects are entered, and the order in which they are encountered is
   recorded.  HashGet returns this number, a positive integer, if the
   object was seen before, and zero if not.  A fixed hash table size
   is used, which seems adequate for now. 

   The hash table is a VECSXP, with count in TRUELENGTH, and buckets
   that are chains of VECSXP nodes of length 2, with value in TRUELENGTH
   and key and link as the two elements.
*/

#define HASHSIZE_HERE 1103

#define PTRHASH(obj) ((unsigned)((R_size_t)obj ^ ((R_size_t)obj>>16)) >> 2)

#define HASH_TABLE_COUNT(ht) TRUELENGTH(ht)
#define SET_HASH_TABLE_COUNT(ht,val) SET_TRUELENGTH(ht,val)

#define HASH_BUCKET(ht,pos) VECTOR_ELT(ht, pos)
#define SET_HASH_BUCKET(ht,pos,val) SET_VECTOR_ELT(ht,pos,val)

static SEXP MakeHashTable(void)
{
    SEXP ht = allocVector (VECSXP, HASHSIZE_HERE);
    SET_HASH_TABLE_COUNT (ht, 0);
    return ht;
}

static attribute_noinline void HashAdd (SEXP obj, SEXP ht)
{
    int pos = PTRHASH(obj) % HASHSIZE_HERE;
    int count = HASH_TABLE_COUNT(ht) + 1;

    SEXP cell = allocVector(VECSXP,2);
    SET_TRUELENGTH(cell,count);
    SET_VECTOR_ELT(cell,0,obj);
    SET_VECTOR_ELT(cell,1,HASH_BUCKET(ht,pos));

    SET_HASH_BUCKET (ht, pos, cell);
    SET_HASH_TABLE_COUNT (ht, count);
}

static attribute_noinline int HashGet (SEXP obj, SEXP ht)
{
    int pos = PTRHASH(obj) % HASHSIZE_HERE;
    SEXP cell;
    for (cell = HASH_BUCKET(ht,pos); 
         cell != R_NilValue; 
         cell = VECTOR_ELT(cell,1)) {
	if (obj == VECTOR_ELT(cell,0))
	    return TRUELENGTH(cell);
    }
    return 0;
}

/*
 * Administrative SXP values
 *
 * These macros defind SXP "type" for specifying special object, such
 * as R_NilValue, or control information, like REFSXP or NAMESPACESXP.
 * The range of SXP types is limited to 5 bit by the current sxpinfo
 * layout, but just in case these values are placed at the top of the
 * 8 bit range.
 */

#define REFSXP            255
#define NILVALUE_SXP      254
#define GLOBALENV_SXP     253
#define UNBOUNDVALUE_SXP  252
#define MISSINGARG_SXP    251
#define BASENAMESPACE_SXP 250
#define NAMESPACESXP      249
#define PACKAGESXP        248
#define PERSISTSXP        247

/* the following are speculative--we may or may not need them soon */
#define CLASSREFSXP       246
#define GENERICREFSXP     245
#define BCREPDEF          244
#define BCREPREF          243
#define EMPTYENV_SXP	  242
#define BASEENV_SXP	  241

/* The following are needed to preserve attribute information on
   expressions in the constant pool of byte code objects. This is
   mainly for preserving source references attributes.  The original
   implementation of the sharing-preserving writing and reading of yte
   code objects did not account for the need to preserve attributes,
   so there is now a work-around using these SXP types to flag when
   the ATTRIB field has been written out. Object bits and S4 bits are
   still not preserved.  It the long run in might be better to change
   to a scheme in which all sharing is preserved and byte code objects
   don't need to be handled as a special case.  LT */
#define ATTRLANGSXP       240
#define ATTRLISTSXP       239

/* the following added for pqR */
#define MISSINGUNDER_SXP 229

/*
 * Type/Flag Packing and Unpacking
 *
 * To reduce space consumption for serializing code (lots of list
 * structure) the type (at most 8 bits), several single bit flags,
 * and the sxpinfo gp field (LEVELS, 16 bits) are packed into a single
 * integer.  The integer is signed, so this shouldn't be pushed too
 * far.  It assumes at least 28 bits, but that should be no problem.
 */

#define IS_OBJECT_BIT_MASK (1 << 8)
#define HAS_ATTR_BIT_MASK (1 << 9)
#define HAS_TAG_BIT_MASK (1 << 10)
#define IS_CONSTANT_MASK (1 << 11)
#define ENCODE_LEVELS(v) ((v) << 12)
#define DECODE_LEVELS(v) ((v) >> 12)
#define DECODE_TYPE(v) ((v) & 255)

static R_INLINE int PackFlags(int type, int levs, int isobj, int hasattr, 
                              int hastag, int isconstant)
{
    /* We don't write out bit 5 as from R 2.8.0.
       It is used to indicate if an object is in CHARSXP cache
       - not that it matters to this version of R, but it saves
       checking all previous versions.

       Also make sure the former HASHASH bit (1) is not written out.
    */
    int val;
    if (type == CHARSXP) levs &= (~(CACHED_MASK | 1 /* was HASHASH */));
    val = type | ENCODE_LEVELS(levs);
    if (isobj) val |= IS_OBJECT_BIT_MASK;
    if (hasattr) val |= HAS_ATTR_BIT_MASK;
    if (hastag) val |= HAS_TAG_BIT_MASK;
    if (isconstant) val |= IS_CONSTANT_MASK;
    return val;
}

static R_INLINE void UnpackFlags(int flags, SEXPTYPE *ptype, int *plevs,
                                 int *pisobj, int *phasattr, int *phastag,
                                 int *pisconstant)
{
    if ((*ptype = DECODE_TYPE(flags)) != REFSXP) {
        *plevs = DECODE_LEVELS(flags);
        *pisobj = (flags & IS_OBJECT_BIT_MASK) != 0;
        *phasattr = (flags & HAS_ATTR_BIT_MASK) != 0;
        *phastag = (flags & HAS_TAG_BIT_MASK) != 0;
        *pisconstant = (flags & IS_CONSTANT_MASK) != 0;
    }
}


/* REFERENCE/INDEX PACKING AND UNPACKING.

   Code will contain many references to symbols. As long as there are
   not too many references, the index and the REFSXP flag indicating a
   reference can be packed in a single integeger.  Since the index is
   1-based, a 0 is used to indicate an index that doesn't fit and
   therefore follows. */

#define PACK_REF_INDEX(i) (((i) << 8) | REFSXP)
#define UNPACK_REF_INDEX(i) ((i) >> 8)
#define MAX_PACKED_INDEX (INT_MAX >> 8)

static R_INLINE void OutRefIndex (struct outpar *par, int i)
{
    if (i > MAX_PACKED_INDEX) {
	OutInteger(par, REFSXP);
	OutInteger(par, i);
    }
    else OutInteger(par, PACK_REF_INDEX(i));
}

static R_INLINE int InRefIndex (struct inpar *par, int flags)
{
    int i = UNPACK_REF_INDEX(flags);
    if (i == 0)
	return InInteger(par);
    else
	return i;
}


/* PERSISTENT NAME HOOKS.

   These routines call the appropriate hook functions for allowing
   customized handling of reference objects. */

static inline SEXP GetPersistentName(R_outpstream_t stream, SEXP s)
{
    if (stream->OutPersistHookFunc != NULL) {
	switch (TYPEOF(s)) {
	case WEAKREFSXP:
	case EXTPTRSXP: break;
	case ENVSXP:
	    if (s == R_GlobalEnv ||
		s == R_BaseEnv ||
		s == R_EmptyEnv ||
		R_IsNamespaceEnv(s) ||
		R_IsPackageEnv(s))
		return R_NilValue;
	    else
		break;
	default: return R_NilValue;
	}
	return stream->OutPersistHookFunc(s, stream->OutPersistHookData);
    }
    else
	return R_NilValue;
}

static inline SEXP PersistentRestore(R_inpstream_t stream, SEXP s)
{
    if (stream->InPersistHookFunc == NULL)
	error(_("no restore method available"));
    return stream->InPersistHookFunc(s, stream->InPersistHookData);
}


/* SERIALIZATION CODE. */

static inline int SaveSpecialHook(SEXP item)
{
    if (item == R_NilValue)      return NILVALUE_SXP;
    if (item == R_EmptyEnv)	 return EMPTYENV_SXP;
    if (item == R_BaseEnv)	 return BASEENV_SXP;
    if (item == R_GlobalEnv)     return GLOBALENV_SXP;
    if (item == R_UnboundValue)  return UNBOUNDVALUE_SXP;
    if (item == R_MissingArg)    return MISSINGARG_SXP;
    if (item == R_MissingUnder)  return MISSINGUNDER_SXP;
    if (item == R_BaseNamespace) return BASENAMESPACE_SXP;
    return 0;
}

static attribute_noinline void OutStringVec (struct outpar *par, SEXP s)
{
    int i, len;

    R_assert(TYPEOF(s) == STRSXP);

#ifdef WARN_ABOUT_NAMES_IN_PERSISTENT_STRINGS
    SEXP names = getAttrib(s, R_NamesSymbol);
    if (names != R_NilValue)
	warning(_("names in persistent strings are currently ignored"));
#endif

    len = LENGTH(s);
    OutInteger(par, 0); /* place holder to allow names if we want to */
    OutInteger(par, len);
    for (i = 0; i < len; i++)
	WriteItem (par, STRING_ELT(s, i));
}

#define min2(a, b) ((a) < (b)) ? (a) : (b)

/* length will need to be another type to allow longer vectors */
static attribute_noinline void OutIntegerVec (struct outpar *par, SEXP s)
{
    R_outpstream_t stream = par->stream;
    char *buf = par->buf;

    R_len_t length = LENGTH(s);
    OutInteger(par, length);

    switch (stream->type) {
    case R_pstream_xdr_format:
    {
	int done, this;
	for (done = 0; done < length; done += this) {
	    this = min2(CHUNK_SIZE, length - done);
	    for (int cnt = 0; cnt < this; cnt++)
                encode_integer (INTEGER(s)[done+cnt], buf + sizeof(int) * cnt);
	    stream->OutBytes(stream, buf, sizeof(int) * this);
	}
	break;
    }
    case R_pstream_binary_format:
    {
	/* write in chunks to avoid overflowing ints */
	int done, this;
	for (done = 0; done < length; done += this) {
	    this = min2(CHUNK_SIZE, length - done);
	    stream->OutBytes(stream, INTEGER(s) + done, sizeof(int) * this);
	}
	break;
    }
    default:
	for (int cnt = 0; cnt < length; cnt++)
	    OutInteger(par, INTEGER(s)[cnt]);
    }
}

static attribute_noinline void OutRealVec (struct outpar *par, SEXP s) 
{
    R_outpstream_t stream = par->stream;
    char *buf = par->buf;

    R_len_t length = LENGTH(s);
    OutInteger(par, length);

    switch (stream->type) {
    case R_pstream_xdr_format:
    {
	int done, this;
        for (done = 0; done < length; done += this) {
	    this = min2(CHUNK_SIZE, length - done);
            encode_doubles (REAL(s)+done, this, buf);
	    stream->OutBytes(stream, buf, sizeof(double) * this);
	}
	break;
    }
    case R_pstream_binary_format:
    {
	int done, this;
        for (done = 0; done < length; done += this) {
	    this = min2(CHUNK_SIZE, length - done);
	    stream->OutBytes(stream, REAL(s) + done, sizeof(double) * this);
	}
	break;
    }
    default:
	for (int cnt = 0; cnt < length; cnt++)
	    OutReal(par, REAL(s)[cnt]);
    }
}

static attribute_noinline void OutComplexVec (struct outpar *par, SEXP s)
{
    R_outpstream_t stream = par->stream;
    char *buf = par->buf;

    R_len_t length = LENGTH(s);
    OutInteger(par, length);

    switch (stream->type) {
    case R_pstream_xdr_format:
    {
	int done, this;
        for (done = 0; done < length; done += this) {
	    this = min2(CHUNK_SIZE, length - done);
            encode_doubles ((double*)(COMPLEX(s)+done), 2*this, buf);
	    stream->OutBytes(stream, buf, sizeof(Rcomplex) * this);
	}
	break;
    }
    case R_pstream_binary_format:
    {
	int done, this;
        for (done = 0; done < length; done += this) {
	    this = min2(CHUNK_SIZE, length - done);
	    stream->OutBytes(stream, COMPLEX(s) + done, 
			     sizeof(Rcomplex) * this);
	}
	break;
    }
    default:
	for (int cnt = 0; cnt < length; cnt++)
	    OutComplex(par, COMPLEX(s)[cnt]);
    }
}

static void WriteItem (struct outpar *par, SEXP s)
{
    R_outpstream_t stream = par->stream;
    SEXP ref_table = par->ref_table;
    int nosharing = par->nosharing;

    int i;
    int ix; /* this could be a different type for longer vectors */
    SEXP t;

    if (R_compile_pkgs && TYPEOF(s) == CLOSXP && TYPEOF(BODY(s)) != BCODESXP) {
	SEXP new_s;
	R_compile_pkgs = FALSE;
	PROTECT(new_s = R_cmpfun(s));
	WriteItem (par, new_s);
	UNPROTECT(1);
	R_compile_pkgs = TRUE;
	return;
    }

 tailcall: ;

    int cannot_be_special = ((VECTOR_TYPES | CONS_TYPES) >> TYPEOF(s)) & 1;

    if (!cannot_be_special && (i = SaveSpecialHook(s)) != 0) {
	OutInteger(par, i);
        return;
    }

    if (!cannot_be_special && (t = GetPersistentName(stream,s)) != R_NilValue) {
	R_assert(TYPEOF(t) == STRSXP && LENGTH(t) > 0);
	PROTECT(t);
	HashAdd(s, ref_table);
	OutInteger(par, PERSISTSXP);
	OutStringVec (par, t);
	UNPROTECT(1);
        return;
    }

    if ((i = HashGet(s, ref_table)) != 0) {
	OutRefIndex(par, i);
        return;
    }

    R_CHECKSTACK();

    if (TYPEOF(s) == SYMSXP) {
	/* Note : NILSXP can't occur here */
	HashAdd(s, ref_table);
	OutInteger(par, SYMSXP);
	WriteItem (par, PRINTNAME(s));
        return;
    }

    if (TYPEOF(s) == ENVSXP) {
	HashAdd(s, ref_table);
	if (R_IsPackageEnv(s)) {
	    SEXP name = R_PackageEnvName(s);
	    warning(_("'%s' may not be available when loading"),
		    CHAR(STRING_ELT(name, 0)));
	    OutInteger(par, PACKAGESXP);
	    OutStringVec (par, name);
	}
	else if (R_IsNamespaceEnv(s)) {
#ifdef WARN_ABOUT_NAME_SPACES_MAYBE_NOT_AVAILABLE
	    warning(_("namespaces may not be available when loading"));
#endif
	    OutInteger(par, NAMESPACESXP);
	    OutStringVec (par, PROTECT(R_NamespaceEnvSpec(s)));
	    UNPROTECT(1);
	}
	else {
	    OutInteger(par, ENVSXP);
	    OutInteger(par, R_EnvironmentIsLocked(s) ? 1 : 0);
	    WriteItem (par, ENCLOS(s));
	    WriteItem (par, FRAME(s));
            SEXP newtable = HASHTAB(s) == R_NilValue ? R_NilValue
                             : R_HashRehashOld(HASHTAB(s));
            PROTECT(newtable);
	    WriteItem (par, newtable);
            UNPROTECT(1);
	    WriteItem (par, ATTRIB(s));
	}
        return;
    }

    int flags, hastag, hasattr;

    hastag = FALSE;

    if((((1<<LISTSXP) + (1<<LANGSXP) + (1<<CLOSXP) + (1<<PROMSXP) + (1<<DOTSXP))
          >> TYPEOF(s)) & 1) {
        if (TAG(s) != R_NilValue)
            hastag = TRUE;
    }

    /* The CHARSXP cache chains are maintained through the ATTRIB
       field, so the content of that field must not be serialized, so
       we treat it as not there. */
    hasattr = ATTRIB(s) != R_NilValue && TYPEOF(s) != CHARSXP;

    flags = PackFlags(TYPEOF(s), LEVELS(s), OBJECT(s),
                      hasattr, hastag, nosharing ? 0 : IS_CONSTANT(s));

    OutInteger(par, flags);

    switch (TYPEOF(s)) {
    case LISTSXP:
    case LANGSXP:
    case CLOSXP:
    case PROMSXP:
    case DOTSXP:
        /* Dotted pair objects */
        /* These write their ATTRIB fields first to allow us to avoid
           recursion on the CDR */
        if (hasattr)
            WriteItem (par, ATTRIB(s));
        if (TAG(s) != R_NilValue)
            WriteItem (par, TAG(s));
        WriteItem (par, CAR(s));
        /* now do a tail call to WriteItem to handle the CDR */
        s = CDR(s);
        goto tailcall;
    case EXTPTRSXP:
        /* external pointers */
        HashAdd(s, ref_table);
        WriteItem (par, EXTPTR_PROT(s));
        WriteItem (par, EXTPTR_TAG(s));
        break;
    case WEAKREFSXP:
        /* Weak references */
        HashAdd(s, ref_table);
        break;
    case SPECIALSXP:
    case BUILTINSXP:
        /* Builtin functions */
        OutString(par, PRIMNAME(s), strlen(PRIMNAME(s)));
        break;
    case CHARSXP:
        if (s == NA_STRING)
            OutInteger(par, -1);
        else
            OutString(par, CHAR(s), LENGTH(s));
        break;
    case LGLSXP:
    case INTSXP:
        OutIntegerVec(par, s);
        break;
    case REALSXP:
        OutRealVec(par, s);
        break;
    case CPLXSXP:
        OutComplexVec(par, s);
        break;
    case STRSXP:
        OutInteger(par, LENGTH(s));
        for (ix = 0; ix < LENGTH(s); ix++)
            WriteItem (par, STRING_ELT(s, ix));
        break;
    case VECSXP:
    case EXPRSXP:
        OutInteger(par, LENGTH(s));
        for (ix = 0; ix < LENGTH(s); ix++)
            WriteItem (par, VECTOR_ELT(s, ix));
        break;
    case BCODESXP:
        WriteBC (par, s);
        break;
    case RAWSXP:
        OutInteger(par, LENGTH(s));
        switch (stream->type) {
        case R_pstream_xdr_format:
        case R_pstream_binary_format: {
            int done, this, len = LENGTH(s);
            for (done = 0; done < len; done += this) {
                this = min2(CHUNK_SIZE, len - done);
                stream->OutBytes(stream, RAW(s) + done, this);
            }
            break;
        }
        default:
            for (ix = 0; ix < LENGTH(s); ix++) 
                OutByte(par, RAW(s)[ix]);
        }
        break;
    case S4SXP:
      break; /* only attributes (i.e., slots) count */
    default:
        error(_("WriteItem: unknown type %i"), TYPEOF(s));
    }

    if (hasattr)
        WriteItem (par, ATTRIB(s));
}

static SEXP MakeCircleHashTable(void)
{
    return CONS(R_NilValue, allocVector(VECSXP, HASHSIZE_HERE));
}

static Rboolean AddCircleHash(SEXP item, SEXP ct)
{
    SEXP table, bucket, list;
    int pos;

    table = CDR(ct);
    pos = PTRHASH(item) % LENGTH(table);
    bucket = VECTOR_ELT(table, pos);
    for (list = bucket; list != R_NilValue; list = CDR(list))
	if (TAG(list) == item) {
	    if (CAR(list) == R_NilValue) {
		/* this is the second time; enter in list and mark */
		SETCAR(list, R_UnboundValue); /* anything different will do */
		SETCAR(ct, CONS(item, CAR(ct)));
	    }
	    return TRUE;
	}

    /* If we get here then this is a new item; enter in the table */
    bucket = CONS(R_NilValue, bucket);
    SET_TAG(bucket, item);
    SET_VECTOR_ELT(table, pos, bucket);
    return FALSE;
}

static void ScanForCircles1(SEXP s, SEXP ct)
{
    switch (TYPEOF(s)) {
    case LANGSXP:
    case LISTSXP:
	if (! AddCircleHash(s, ct)) {
	    ScanForCircles1(CAR(s), ct);
	    ScanForCircles1(CDR(s), ct);
	}
	break;
    case BCODESXP:
	{
	    int i, n;
	    SEXP consts = BCODE_CONSTS(s);
	    n = LENGTH(consts);
	    for (i = 0; i < n; i++)
		ScanForCircles1(VECTOR_ELT(consts, i), ct);
	}
	break;
    default: break;
    }
}

static SEXP ScanForCircles(SEXP s)
{
    SEXP ct;
    PROTECT(ct = MakeCircleHashTable());
    ScanForCircles1(s, ct);
    UNPROTECT(1);
    return CAR(ct);
}

static SEXP findrep(SEXP x, SEXP reps)
{
    for (; reps != R_NilValue; reps = CDR(reps))
	if (x == CAR(reps))
	    return reps;
    return R_NilValue;
}

static void WriteBCLang (struct outpar *par, SEXP s, SEXP reps)
{
    int type = TYPEOF(s);
    if (type == LANGSXP || type == LISTSXP) {
	SEXP r = findrep(s, reps);
	int output = TRUE;
	if (r != R_NilValue) {
	    /* we have a cell referenced more than once */
	    if (TAG(r) == R_NilValue) {
		/* this is the first reference, so update and register
		   the counter */
		int i = INTEGER(CAR(reps))[0]++;
		SET_TAG(r, allocVector1INT());
		INTEGER(TAG(r))[0] = i;
		OutInteger(par, BCREPDEF);
		OutInteger(par, i);
	    }
	    else {
		/* we've seen it before, so just put out the index */
		OutInteger(par, BCREPREF);
		OutInteger(par, INTEGER(TAG(r))[0]);
		output = FALSE;
	    }
	}
	if (output) {
	    SEXP attr = ATTRIB(s);
	    if (attr != R_NilValue) {
		switch(type) {
		case LANGSXP: type = ATTRLANGSXP; break;
		case LISTSXP: type = ATTRLISTSXP; break;
		}
	    }
	    OutInteger(par, type);
	    if (attr != R_NilValue)
		WriteItem (par, attr);
	    WriteItem (par, TAG(s));
	    WriteBCLang (par, CAR(s), reps);
	    WriteBCLang (par, CDR(s), reps);
	}
    }
    else {
	OutInteger(par, 0); /* pad */
	WriteItem (par, s);
    }
}

static void WriteBC1 (struct outpar *par, SEXP s, SEXP reps)
{
    int i, n;
    SEXP code, consts;
    PROTECT(code = R_bcDecode(BCODE_CODE(s)));
    WriteItem (par, code);
    consts = BCODE_CONSTS(s);
    n = LENGTH(consts);
    OutInteger(par, n);
    for (i = 0; i < n; i++) {
	SEXP c = VECTOR_ELT(consts, i);
	int type = TYPEOF(c);
	switch (type) {
	case BCODESXP:
	    OutInteger(par, type);
	    WriteBC1 (par, c, reps);
	    break;
	case LANGSXP:
	case LISTSXP:
	    WriteBCLang (par, c, reps);
	    break;
	default:
	    OutInteger(par, type);
	    WriteItem (par, c);
	}
    }
    UNPROTECT(1);
}

static void WriteBC (struct outpar *par, SEXP s)
{
    SEXP reps = ScanForCircles(s);
    PROTECT(reps = CONS(R_NilValue, reps));
    OutInteger(par, length(reps));
    SETCAR(reps, allocVector1INT());
    INTEGER(CAR(reps))[0] = 0;
    WriteBC1 (par, s, reps);
    UNPROTECT(1);
}

/* R_Serialize is accessible from outside.  R_Serialize_internal has the
   additional nosharing argument for use in this module. */

static void R_Serialize_internal (SEXP s, R_outpstream_t stream, int nosharing)
{
    struct outpar par;
    char buf [CBUF_SIZE];
    par.nosharing = nosharing;
    par.stream = stream;
    par.buf = buf;
    PROTECT(par.ref_table = MakeHashTable());

    int version = stream->version;

    OutFormat(stream);

    switch(version) {
    case 2:
	OutInteger(&par, version);
	OutInteger(&par, R_VERSION);
	OutInteger(&par, R_Version(2,3,0));
	break;
    default: error(_("version %d not supported"), version);
    }

    WriteItem (&par, s);

    UNPROTECT(1);
}

void R_Serialize (SEXP s, R_outpstream_t stream)
{
    R_Serialize_internal (s, stream, FALSE);
}


/*** UNSERIALIZE CODE ***/

#define INITIAL_REFREAD_TABLE_SIZE 250

static SEXP MakeReadRefTable(void)
{
    SEXP data = allocVector(VECSXP, INITIAL_REFREAD_TABLE_SIZE);
    SET_TRUELENGTH(data, 0);
    return CONS(data, R_NilValue);
}

static R_INLINE SEXP GetReadRef(SEXP table, int index)
{
    int i = index - 1;
    SEXP data = CAR(table);

    if (i < 0 || i >= LENGTH(data))
	error(_("reference index out of range"));
    return VECTOR_ELT(data, i);
}

static SEXP ExpandRefTable(SEXP table, SEXP value)
{
    SEXP data = CAR(table);
    int len = LENGTH(data);
    SEXP newdata;
    int i;

    PROTECT(value);
    newdata = allocVector(VECSXP, 2*len);
    for (i = 0; i < len; i++)
        SET_VECTOR_ELT(newdata, i, VECTOR_ELT(data, i));
    SETCAR(table, newdata);
    UNPROTECT(1);

    return newdata;
}

static R_INLINE void AddReadRef(SEXP table, SEXP value)
{
    SEXP data = CAR(table);
    int count = TRUELENGTH(data) + 1;
    if (count >= LENGTH(data)) 
        data = ExpandRefTable(table,value);
    SET_TRUELENGTH(data, count);
    SET_VECTOR_ELT(data, count - 1, value);
}

static attribute_noinline SEXP InStringVec (struct inpar *par)
{
    SEXP s;
    int i, len;
    if (InInteger(par) != 0)
	error(_("names in persistent strings are not supported yet"));
    len = InInteger(par);
    PROTECT(s = allocVector(STRSXP, len));
    for (i = 0; i < len; i++)
	SET_STRING_ELT(s, i, ReadItem(par));
    UNPROTECT(1);
    return s;
}

static attribute_noinline SEXP InCharSXP (struct inpar *par, int levs)
{
    R_inpstream_t stream = par->stream;
    char *buf = par->buf;

    int length = InInteger(par); /* suppose still limited to 2^31-1 bytes */

    if (length == -1) return NA_STRING;

    int enc = (levs & UTF8_MASK) ? CE_UTF8 
            : (levs & LATIN1_MASK) ? CE_LATIN1
            : (levs & BYTES_MASK) ? CE_BYTES
            : CE_NATIVE;

    SEXP s;

    if (length < CBUF_SIZE) {
        InString(stream, buf, length);
        buf[length] = '\0'; /* unnecessary? */
        s = mkCharLenCE(buf, length, enc);
    }
    else {
        char *big_cbuf = CallocCharBuf(length);
        InString(stream, big_cbuf, length);
        s = mkCharLenCE(big_cbuf, length, enc);
        Free(big_cbuf);
    }

    return s;
}

/* length, done could be a longer type */
static  attribute_noinline void InIntegerVec (struct inpar *par, SEXP obj, int length)
{
    R_inpstream_t stream = par->stream;
    char *buf = par->buf;

    switch (stream->type) {
    case R_pstream_xdr_format:
    {
	int done, this;
        for (done = 0; done < length; done += this) {
	    this = min2(CHUNK_SIZE, length - done);
	    stream->InBytes(stream, buf, sizeof(int) * this);
	    for (int cnt = 0; cnt < this; cnt++)
                INTEGER(obj)[done+cnt] = decode_integer(buf + sizeof(int)*cnt);
	}
	break;
    }
    case R_pstream_binary_format:
    {
	int done, this;
        for (done = 0; done < length; done += this) {
	    this = min2(CHUNK_SIZE, length - done);
	    stream->InBytes(stream, INTEGER(obj) + done, sizeof(int) * this);
	}
	break;
    }
    default:
	for (int cnt = 0; cnt < length; cnt++)
	    INTEGER(obj)[cnt] = InInteger(par);
    }
}

static attribute_noinline void InRealVec (struct inpar *par, SEXP obj, int length)
{
    R_inpstream_t stream = par->stream;
    char *buf = par->buf;

    switch (stream->type) {
    case R_pstream_xdr_format:
    {
	int done, this;
        for (done = 0; done < length; done += this) {
	    this = min2(CHUNK_SIZE, length - done);
	    stream->InBytes(stream, buf, sizeof(double) * this);
            decode_doubles (REAL(obj)+done, this, buf);
	}
	break;
    }
    case R_pstream_binary_format:
    {
	int done, this;
        for (done = 0; done < length; done += this) {
	    this = min2(CHUNK_SIZE, length - done);
	    stream->InBytes(stream, REAL(obj) + done, sizeof(double) * this);
	}
	break;
    }
    default:
	for (int cnt = 0; cnt < length; cnt++)
	    REAL(obj)[cnt] = InReal(par);
    }
}

static attribute_noinline void InComplexVec (struct inpar *par, SEXP obj, int length)
{
    R_inpstream_t stream = par->stream;
    char *buf = par->buf;

    switch (stream->type) {
    case R_pstream_xdr_format:
    {
	int done, this;
	for (done = 0; done < length; done += this) {
	    this = min2(CHUNK_SIZE, length - done);
	    stream->InBytes(stream, buf, sizeof(Rcomplex) * this);
            decode_doubles ((double*)(COMPLEX(obj)+done), 2*this, buf);
	}
	break;
    }
    case R_pstream_binary_format:
    {
	int done, this;
        for (done = 0; done < length; done += this) {
	    this = min2(CHUNK_SIZE, length - done);
	    stream->InBytes(stream, COMPLEX(obj) + done, 
			    sizeof(Rcomplex) * this);
	}
	break;
    }
    default:
	for (int cnt = 0; cnt < length; cnt++)
	    COMPLEX(obj)[cnt] = InComplex(par);
    }
}

static SEXP ReadItem (struct inpar *par)
{
    R_inpstream_t stream = par->stream;
    SEXP ref_table = par->ref_table;
    char *buf = par->buf;

    int len, flags, levs, objf, hasattr, hastag, isconstant;
    SEXPTYPE type;

    /* The result is stored in "s", which is returned at the end of this
       function (at label "ret").  However, for tail recursion elimination, 
       when "set_cdr" is not R_NoObject, "s" is instead stored in the CDR of
       "set_cdr", and "ss" is returned. */

    SEXP s, ss, set_cdr = R_NoObject;

  again: /* we jump back here instead of tail recursion for CDR of CONS type */

    flags = InInteger(par);
    UnpackFlags(flags, &type, &levs, &objf, &hasattr, &hastag, &isconstant);

    switch(type) {
    case NILVALUE_SXP:      s = R_NilValue;       goto ret;
    case EMPTYENV_SXP:	    s = R_EmptyEnv;       goto ret;
    case BASEENV_SXP:	    s = R_BaseEnv;        goto ret;
    case GLOBALENV_SXP:     s = R_GlobalEnv;      goto ret;
    case UNBOUNDVALUE_SXP:  s = R_UnboundValue;   goto ret;
    case MISSINGARG_SXP:    s = R_MissingArg;     goto ret;
    case MISSINGUNDER_SXP:  s = R_MissingUnder; goto ret;
    case BASENAMESPACE_SXP: s = R_BaseNamespace;  goto ret;
    case REFSXP:
	s = GetReadRef(ref_table, InRefIndex(par, flags));
        goto ret;
    case PERSISTSXP:
	PROTECT(s = InStringVec(par));
	s = PersistentRestore(stream, s);
	UNPROTECT(1);
	AddReadRef(ref_table, s);
	goto ret;
    case SYMSXP:
	PROTECT(s = ReadItem(par)); /* print name */
	s = installChar(s);
	AddReadRef(ref_table, s);
	UNPROTECT(1);
	goto ret;
    case PACKAGESXP:
	PROTECT(s = InStringVec(par));
	s = R_FindPackageEnv(s);
	UNPROTECT(1);
	AddReadRef(ref_table, s);
	goto ret;
    case NAMESPACESXP:
	PROTECT(s = InStringVec(par));
	s = R_FindNamespace(s);
	AddReadRef(ref_table, s);
	UNPROTECT(1);
	goto ret;
    case ENVSXP:
	{
	    int locked = InInteger(par);

	    PROTECT(s = allocSExp(ENVSXP));

	    /* MUST register before filling in */
	    AddReadRef(ref_table, s);

	    /* Now fill it in  */
	    SET_ENCLOS(s, ReadItem(par));
	    SET_FRAME(s, ReadItem(par));
	    SET_HASHTAB(s, ReadItem(par));
	    SET_ATTRIB(s, ReadItem(par));
	    if (ATTRIB(s) != R_NilValue &&
		getClassAttrib(s) != R_NilValue)
		/* We don't write out the object bit for environments,
		   so reconstruct it here if needed. */
		SET_OBJECT(s, 1);
            if (IS_HASHED(s)) {
                R_HashRehash(HASHTAB(s));
                R_RestoreHashCount(s);
            }
	    if (locked) R_LockEnvironment(s, FALSE);
	    /* Convert a NULL enclosure to baseenv() */
	    if (ENCLOS(s) == R_NilValue) SET_ENCLOS(s, R_BaseEnv);
	    UNPROTECT(1);
	    goto ret;
	}
    case LISTSXP:
        if (isconstant && !objf && !hasattr && !hastag && levs==0) {
            SEXP car, cdr;
            PROTECT(car = ReadItem (par));
            cdr = ReadItem (par);
            s = cdr == R_NilValue ? MaybeConstList1(car) : CONS(car,cdr);
            UNPROTECT(1); /* car */
            goto ret;
        }
        /* else fall through to code below for non-constants */
    case LANGSXP:
    case CLOSXP:
    case PROMSXP:
    case DOTSXP:
	PROTECT(s = allocSExp(type));
	SETLEVELS(s, levs);
	SET_OBJECT(s, objf);
        if (hasattr)
	    SET_ATTRIB(s, ReadItem(par));
        if (hastag)
	    SET_TAG(s, ReadItem(par));
	SETCAR(s, ReadItem(par));

        /* For reading closures and promises stored in earlier versions, 
           convert NULL env to baseenv() */
        if (type == CLOSXP) {
            SETCDR(s, ReadItem(par));  /* CDR == CLOENV */
            if (CLOENV(s) == R_NilValue) SET_CLOENV(s, R_BaseEnv);
        }
        else if (type == PROMSXP) {
            SETCDR(s, ReadItem(par));  /* CDR == PRENV */
            if (PRENV(s) == R_NilValue) SET_PRENV(s, R_BaseEnv);
        }
        else {  /* Eliminate tail recursion for CDR */
            if (set_cdr != R_NoObject) {
                SETCDR(set_cdr,s);
                UNPROTECT(1);  /* s, which is now protected through ss */
            }
            else {
                ss = s;  /* ss is protected, since s is, and since set_cdr will
                            no longer be R_NoObject, will UNPROTECT ss at ret */
            }
            set_cdr = s;
            goto again;
        }
        UNPROTECT(1); /* s */
        goto ret;
    default:
	/* These break out of the switch to have their ATTR,
	   LEVELS, and OBJECT fields filled in.  Each leaves the
	   newly allocated value PROTECTed */
	switch (type) {
	case EXTPTRSXP:
	    PROTECT(s = allocSExp(type));
	    AddReadRef(ref_table, s);
	    R_SetExternalPtrAddr(s, NULL);
	    R_SetExternalPtrProtected(s, ReadItem(par));
	    R_SetExternalPtrTag(s, ReadItem(par));
	    break;
	case WEAKREFSXP:
	    PROTECT(s = R_MakeWeakRef(R_NilValue, R_NilValue, R_NilValue,
				      FALSE));
	    AddReadRef(ref_table, s);
	    break;
	case SPECIALSXP:
	case BUILTINSXP:
	    {
		/* These are all short strings */
                len = InInteger(par);
		InString(stream, buf, len);
		buf[len] = '\0';
		int index = StrToInternal(buf);
		if (index == NA_INTEGER) {
		    warning(_("unrecognized internal function name \"%s\""),
                            buf); 
		    PROTECT(s = R_NilValue);
		} else
		    PROTECT(s = mkPRIMSXP(index, type == BUILTINSXP));
	    }
	    break;
	case CHARSXP:
            PROTECT(s = InCharSXP(par,levs));
	    break;
        case LGLSXP:
            len = InInteger(par);
            if (isconstant && len==1 && !objf && !hasattr && levs==0)
                PROTECT(s = ScalarLogicalMaybeConst(InInteger(par)));
            else {
                PROTECT(s = allocVector(type, len));
                InIntegerVec(par, s, len);
            }
            break;
        case INTSXP:
            len = InInteger(par);
            if (isconstant && len==1 && !objf && !hasattr && levs==0)
                PROTECT(s = ScalarIntegerMaybeConst(InInteger(par)));
            else {
                PROTECT(s = allocVector(type, len));
                InIntegerVec(par, s, len);
            }
            break;
        case REALSXP:
            len = InInteger(par);
            if (len==1) {
                double r = InReal(par);
                if (isconstant && !objf && !hasattr && levs==0)
                    PROTECT(s = ScalarRealMaybeConst(r));
                else
                    PROTECT(s = ScalarReal(r));
            }
            else {
                PROTECT(s = allocVector(type, len));
                InRealVec(par, s, len);
            }
            break;
        case CPLXSXP:
            len = InInteger(par);
            if (isconstant && len==1 && !objf && !hasattr && levs==0)
                PROTECT(s = ScalarComplexMaybeConst(InComplex(par)));
            else {
                PROTECT(s = allocVector(type, len));
                InComplexVec(par, s, len);
            }
            break;
        case STRSXP:
            len = InInteger(par);
            if (isconstant && len==1 && !objf && !hasattr && levs==0)
                PROTECT(s = ScalarStringMaybeConst(ReadItem(par)));
            else {
                PROTECT(s = allocVector(type, len));
                for (int count = 0; count < len; ++count)
                    SET_STRING_ELT(s, count, ReadItem(par));
            }
            break;
	case VECSXP:
	case EXPRSXP:
	    len = InInteger(par);
	    PROTECT(s = allocVector(type, len));
	    for (int count = 0; count < len; ++count)
		SET_VECTOR_ELT(s, count, ReadItem(par));
	    break;
	case BCODESXP:
	    PROTECT(s = ReadBC(par));
	    break;
	case CLASSREFSXP:
	    error(_("this version of R cannot read class references"));
	case GENERICREFSXP:
	    error(_("this version of R cannot read generic function references"));
        case RAWSXP:
            len = InInteger(par);
            if (isconstant && len==1 && !objf && !hasattr && levs==0) {
                Rbyte b;
                stream->InBytes (stream, &b, 1);
                PROTECT(s = ScalarRawMaybeConst(b));
            }
            else {
                int done, this;
                PROTECT(s = allocVector(type, len));
                for (done = 0; done < len; done += this) {
                    this = min2(CHUNK_SIZE, len - done);
                    stream->InBytes(stream, RAW(s) + done, this);
                }
            }
            break;
	case S4SXP:
	    PROTECT(s = allocS4Object());
	    break;
	default:
	    error(_("ReadItem: unknown type %i, perhaps written by later version of R"), type);
	}

	if (type != CHARSXP && type != SPECIALSXP && type != BUILTINSXP)
	    if (LEVELS(s)!=levs) SETLEVELS(s, levs); /* don't write to const! */
	SET_OBJECT(s, objf);
	if (TYPEOF(s) == CHARSXP) {
	    /* Since the CHARSXP cache is maintained through the ATTRIB
	       field, that field has already been filled in by the
	       mkChar/mkCharCE call above, so we need to leave it
	       alone.  If there is an attribute (as there might be if
	       the serialized data was created by an older version) we
	       read and ignore the value. */
	    if (hasattr) (void) ReadItem(par);
	}
	else if (hasattr)
	    SET_ATTRIB(s, ReadItem(par));
	UNPROTECT(1); /* s */
	goto ret;
    }

  ret:
    if (set_cdr != R_NoObject) {
        SETCDR(set_cdr, s);
        UNPROTECT(1);  /* ss */
        return ss;
    }
    else {
        return s;
    }
}

static SEXP ReadBC1 (struct inpar *par, SEXP reps);

static SEXP ReadBCLang (struct inpar *par, int type, SEXP reps)
{
    R_inpstream_t stream = par->stream;

    switch (type) {
    case BCREPREF:
	return VECTOR_ELT(reps, InInteger(par));
    case BCREPDEF:
    case LANGSXP:
    case LISTSXP:
    case ATTRLANGSXP:
    case ATTRLISTSXP:
	{
	    SEXP ans;
	    int pos = -1;
	    int hasattr = FALSE;
	    if (type == BCREPDEF) {
		pos = InInteger(par);
		type = InInteger(par);
	    }
	    switch (type) {
	    case ATTRLANGSXP: type = LANGSXP; hasattr = TRUE; break;
	    case ATTRLISTSXP: type = LISTSXP; hasattr = TRUE; break;
	    }
	    PROTECT(ans = allocSExp(type));
	    if (pos >= 0)
		SET_VECTOR_ELT(reps, pos, ans);
	    if (hasattr)
		SET_ATTRIB(ans, ReadItem(par));
	    SET_TAG(ans, ReadItem(par));
	    SETCAR(ans, ReadBCLang (par, InInteger(par), reps));
	    SETCDR(ans, ReadBCLang (par, InInteger(par), reps));
	    UNPROTECT(1);
	    return ans;
	}
    default: return ReadItem(par);
    }
}

static SEXP ReadBCConsts (struct inpar *par, SEXP reps)
{
    SEXP ans, c;
    int i, n;
    n = InInteger(par);
    PROTECT(ans = allocVector(VECSXP, n));
    for (i = 0; i < n; i++) {
	int type = InInteger(par);
	switch (type) {
	case BCODESXP:
	    c = ReadBC1 (par, reps);
	    SET_VECTOR_ELT(ans, i, c);
	    break;
	case LANGSXP:
	case LISTSXP:
	case BCREPDEF:
	case BCREPREF:
	case ATTRLANGSXP:
	case ATTRLISTSXP:
	    c = ReadBCLang (par, type, reps);
	    SET_VECTOR_ELT(ans, i, c);
	    break;
	default:
	    SET_VECTOR_ELT(ans, i, ReadItem(par));
	}
    }
    UNPROTECT(1);
    return ans;
}

static SEXP ReadBC1 (struct inpar *par, SEXP reps)
{
    SEXP s;
    PROTECT(s = allocSExp(BCODESXP));
    SETCAR(s, ReadItem(par)); /* code */
    SETCAR(s, R_bcEncode(CAR(s)));
    SETCDR(s, ReadBCConsts(par, reps)); /* consts */
    SET_TAG_NIL(s); /* expr */
    UNPROTECT(1);
    return s;
}

static SEXP ReadBC (struct inpar *par)
{
    SEXP reps, ans;
    PROTECT(reps = allocVector(VECSXP, InInteger(par)));
    ans = ReadBC1 (par, reps);
    UNPROTECT(1);
    return ans;
}

static void DecodeVersion(int packed, int *v, int *p, int *s)
{
    *v = packed / 65536; packed = packed % 65536;
    *p = packed / 256; packed = packed % 256;
    *s = packed;
}

SEXP R_Unserialize(R_inpstream_t stream)
{
    int version;
    int writer_version, release_version;
    SEXP obj;

    struct inpar par;
    char buf [CBUF_SIZE];
    par.stream = stream;
    par.buf = buf;
    PROTECT(par.ref_table = MakeReadRefTable());

    InFormat(stream);

    /* Read the version numbers */
    version = InInteger(&par);
    writer_version = InInteger(&par);
    release_version = InInteger(&par);
    switch (version) {
    case 2: break;
    default:
	if (version != 2) {
	    int vw, pw, sw;
	    DecodeVersion(writer_version, &vw, &pw, &sw);
	    if (release_version < 0)
		error(_("cannot read unreleased workspace version %d written by experimental R %d.%d.%d"), version, vw, pw, sw);
	    else {
		int vm, pm, sm;
		DecodeVersion(release_version, &vm, &pm, &sm);
		error(_("cannot read workspace version %d written by R %d.%d.%d; need R %d.%d.%d or newer"),
		      version, vw, pw, sw, vm, pm, sm);
	    }
	}
    }

    /* Read the actual object back */

    obj =  ReadItem(&par);

    UNPROTECT(1);
    return obj;
}


/*
 * Generic Persistent Stream Initializers
 */

void
R_InitInPStream(R_inpstream_t stream, R_pstream_data_t data,
		R_pstream_format_t type,
		int (*inchar)(R_inpstream_t),
		void (*inbytes)(R_inpstream_t, void *, int),
		SEXP (*phook)(SEXP, SEXP), SEXP pdata)
{
    stream->data = data;
    stream->type = type;
    stream->InChar = inchar;
    stream->InBytes = inbytes;
    stream->InPersistHookFunc = phook;
    stream->InPersistHookData = pdata;
}

void
R_InitOutPStream(R_outpstream_t stream, R_pstream_data_t data,
		 R_pstream_format_t type, int version,
		 void (*outchar)(R_outpstream_t, int),
		 void (*outbytes)(R_outpstream_t, void *, int),
		 SEXP (*phook)(SEXP, SEXP), SEXP pdata)
{
    stream->data = data;
    stream->type = type;
    stream->version = version != 0 ? version : R_DefaultSerializeVersion;
    stream->OutChar = outchar;
    stream->OutBytes = outbytes;
    stream->OutPersistHookFunc = phook;
    stream->OutPersistHookData = pdata;
}


/*
 * Persistent File Streams
 */

static void OutCharFile(R_outpstream_t stream, int c)
{
    FILE *fp = stream->data;
    fputc(c, fp);
}


static int InCharFile(R_inpstream_t stream)
{
    FILE *fp = stream->data;
    return fgetc(fp);
}

static void OutBytesFile(R_outpstream_t stream, void *buf, int length)
{
    FILE *fp = stream->data;
    size_t out = fwrite(buf, 1, length, fp);
    if (out != length) error(_("write failed"));
}

static void InBytesFile(R_inpstream_t stream, void *buf, int length)
{
    FILE *fp = stream->data;
    size_t in = fread(buf, 1, length, fp);
    if (in != length) error(_("read failed"));
}

void
R_InitFileOutPStream(R_outpstream_t stream, FILE *fp,
			  R_pstream_format_t type, int version,
			  SEXP (*phook)(SEXP, SEXP), SEXP pdata)
{
    R_InitOutPStream(stream, (R_pstream_data_t) fp, type, version,
		     OutCharFile, OutBytesFile, phook, pdata);
}

void
R_InitFileInPStream(R_inpstream_t stream, FILE *fp,
			 R_pstream_format_t type,
			 SEXP (*phook)(SEXP, SEXP), SEXP pdata)
{
    R_InitInPStream(stream, (R_pstream_data_t) fp, type,
		    InCharFile, InBytesFile, phook, pdata);
}


/* PERSISTENT CONNECTION STREAMS. */

#include <Rconnections.h>

static void CheckInConn(Rconnection con)
{
    if (! con->isopen)
	error(_("connection is not open"));
    if (! con->canread || con->read == NULL)
	error(_("cannot read from this connection"));
}

static void CheckOutConn(Rconnection con)
{
    if (! con->isopen)
	error(_("connection is not open"));
    if (! con->canwrite || con->write == NULL)
	error(_("cannot write to this connection"));
}

static void InBytesConn(R_inpstream_t stream, void *buf, int length)
{
    Rconnection con = (Rconnection) stream->data;
    CheckInConn(con);
    if (con->text) {
	int i;
	char *p = buf;
	for (i = 0; i < length; i++)
	    p[i] = Rconn_fgetc(con);
    }
    else {
	if (stream->type == R_pstream_ascii_format) {
	    char linebuf[4];
	    unsigned char *p = buf;
	    int i, ncread;
	    unsigned int res;
	    for (i = 0; i < length; i++) {
		ncread = Rconn_getline(con, linebuf, 3);
		if (ncread != 2)
		    error(_("error reading from ascii connection"));
		if (!sscanf(linebuf, "%02x", &res))
		    error(_("unexpected format in ascii connection"));
		*p++ = (unsigned char)res;
	    }
	} else {
	    if (length != con->read(buf, 1, length, con))
		error(_("error reading from connection"));
	}
    }
}

static int InCharConn(R_inpstream_t stream)
{
    char buf[1];
    Rconnection con = (Rconnection) stream->data;
    CheckInConn(con);
    if (con->text)
	return Rconn_fgetc(con);
    else {
	if (1 != con->read(buf, 1, 1, con))
	    error(_("error reading from connection"));
	return buf[0];
    }
}

static void OutBytesConn(R_outpstream_t stream, void *buf, int length)
{
    Rconnection con = (Rconnection) stream->data;
    CheckOutConn(con);
    if (con->text) {
	int i;
	char *p = buf;
	for (i = 0; i < length; i++)
	    Rconn_printf(con, "%c", p[i]);
    }
    else {
	if (length != con->write(buf, 1, length, con))
	    error(_("error writing to connection"));
    }
}

static void OutCharConn(R_outpstream_t stream, int c)
{
    Rconnection con = (Rconnection) stream->data;
    CheckOutConn(con);
    if (con->text)
	Rconn_printf(con, "%c", c);
    else {
	char buf[1];
	buf[0] = (char) c;
	if (1 != con->write(buf, 1, 1, con))
	    error(_("error writing to connection"));
    }
}

void R_InitConnOutPStream(R_outpstream_t stream, Rconnection con,
			  R_pstream_format_t type, int version,
			  SEXP (*phook)(SEXP, SEXP), SEXP pdata)
{
    CheckOutConn(con);
    if (con->text && type != R_pstream_ascii_format)
	error(_("only ascii format can be written to text mode connections"));
    R_InitOutPStream(stream, (R_pstream_data_t) con, type, version,
		     OutCharConn, OutBytesConn, phook, pdata);
}

void R_InitConnInPStream(R_inpstream_t stream,  Rconnection con,
			 R_pstream_format_t type,
			 SEXP (*phook)(SEXP, SEXP), SEXP pdata)
{
    CheckInConn(con);
    if (con->text) {
	if (type == R_pstream_any_format)
	    type = R_pstream_ascii_format;
	else if (type != R_pstream_ascii_format)
	    error(_("only ascii format can be read from text mode connections"));
    }
    R_InitInPStream(stream, (R_pstream_data_t) con, type,
		    InCharConn, InBytesConn, phook, pdata);
}

/* ought to quote the argument, but it should only be an ENVSXP or STRSXP */
static SEXP CallHook(SEXP x, SEXP fun)
{
    SEXP val, call;
    PROTECT(call = LCONS(fun, CONS(x, R_NilValue)));
    val = eval(call, R_GlobalEnv);
    UNPROTECT(1);
    return val;
}

static void con_cleanup(void *data)
{
    Rconnection con = data;
    if(con->isopen) con->close(con);
}

/* Used from saveRDS().
   This became public in R 2.13.0, and that version added support for
   connections internally */
SEXP attribute_hidden do_serializeToConn(SEXP call, SEXP op, SEXP args, SEXP env)
{
    /* serializeToConn(object, conn, ascii, version, hook) */

    SEXP object, fun;
    int ascii, wasopen, nosharing;
    int version;
    Rconnection con;
    struct R_outpstream_st out;
    R_pstream_format_t type;
    SEXP (*hook)(SEXP, SEXP);
    RCNTXT cntxt;

    checkArity(op, args);

    object = CAR(args);
    con = getConnection(asInteger(CADR(args)));

    if (TYPEOF(CADDR(args)) != LGLSXP)
	error(_("'ascii' must be logical"));
    ascii = LOGICAL(CADDR(args))[0];
    if (ascii) type = R_pstream_ascii_format;
    else type = R_pstream_xdr_format;

    if (CADDDR(args) == R_NilValue)
	version = R_DefaultSerializeVersion;
    else
	version = asInteger(CADDDR(args));
    if (version == NA_INTEGER || version <= 0)
	error(_("bad version value"));
    if (version < 2)
	error(_("cannot save to connections in version %d format"), version);

    fun = CAR(nthcdr(args,4));
    hook = fun != R_NilValue ? CallHook : NULL;

    if (TYPEOF(CAR(nthcdr(args,5))) != LGLSXP)
	error(_("'nosharing' must be logical"));
    nosharing = LOGICAL(CAR(nthcdr(args,5)))[0];

    /* Now we need to do some sanity checking of the arguments.
       A filename will already have been opened, so anything
       not open was specified as a connection directly.
     */
    wasopen = con->isopen;
    if(!wasopen) {
	char mode[5];
	strcpy(mode, con->mode);
	strcpy(con->mode, ascii ? "w" : "wb");
	if(!con->open(con)) error(_("cannot open the connection"));
	strcpy(con->mode, mode);
	/* Set up a context which will close the connection on error */
	begincontext(&cntxt, CTXT_CCODE, R_NilValue, R_BaseEnv, R_BaseEnv,
		     R_NilValue, R_NilValue);
	cntxt.cend = &con_cleanup;
	cntxt.cenddata = con;
    }
    if (!ascii && con->text)
	error(_("binary-mode connection required for ascii=FALSE"));
    if(!con->canwrite)
	error(_("connection not open for writing"));

    R_InitConnOutPStream(&out, con, type, version, hook, fun);
    R_Serialize_internal (object, &out, nosharing);
    if(!wasopen) {endcontext(&cntxt); con->close(con);}

    return R_NilValue;
}

/* Used from readRDS().
   This became public in R 2.13.0, and that version added support for
   connections internally */
SEXP attribute_hidden do_unserializeFromConn(SEXP call, SEXP op, SEXP args, SEXP env)
{
    /* unserializeFromConn(conn, hook) */

    struct R_inpstream_st in;
    Rconnection con;
    SEXP fun, ans;
    SEXP (*hook)(SEXP, SEXP);
    Rboolean wasopen;
    RCNTXT cntxt;

    checkArity(op, args);

    con = getConnection(asInteger(CAR(args)));

    fun = CADR(args);
    hook = fun != R_NilValue ? CallHook : NULL;

    /* Now we need to do some sanity checking of the arguments.
       A filename will already have been opened, so anything
       not open was specified as a connection directly.
     */
    wasopen = con->isopen;
    if(!wasopen) {
	char mode[5];
	strcpy(mode, con->mode);
	strcpy(con->mode, "rb");
	if(!con->open(con)) error(_("cannot open the connection"));
	strcpy(con->mode, mode);
	/* Set up a context which will close the connection on error */
	begincontext(&cntxt, CTXT_CCODE, R_NilValue, R_BaseEnv, R_BaseEnv,
		     R_NilValue, R_NilValue);
	cntxt.cend = &con_cleanup;
	cntxt.cenddata = con;
    }
    if(!con->canread) error(_("connection not open for reading"));

    R_InitConnInPStream(&in, con, R_pstream_any_format, hook, fun);
    PROTECT(ans = R_Unserialize(&in)); /* paranoia about next line */
    if(!wasopen) {endcontext(&cntxt); con->close(con);}
    UNPROTECT(1);
    return ans;
}


/*
 * Persistent Buffered Binary Connection Streams
 */

/**** should eventually come from a public header file */
size_t R_WriteConnection(Rconnection con, void *buf, size_t n);

#define BCONBUFSIZ 4096

typedef struct bconbuf_st {
    Rconnection con;
    int count;
    unsigned char buf[BCONBUFSIZ];
} *bconbuf_t;

static void flush_bcon_buffer(bconbuf_t bb)
{
    if (R_WriteConnection(bb->con, bb->buf, bb->count) != bb->count)
	error(_("error writing to connection"));
    bb->count = 0;
}

static void OutCharBB(R_outpstream_t stream, int c)
{
    bconbuf_t bb = stream->data;
    if (bb->count >= BCONBUFSIZ)
	flush_bcon_buffer(bb);
    bb->buf[bb->count++] = c;
}

static void OutBytesBB(R_outpstream_t stream, void *buf, int length)
{
    bconbuf_t bb = stream->data;
    if (bb->count + length > BCONBUFSIZ)
	flush_bcon_buffer(bb);
    if (length <= BCONBUFSIZ) {
	memcpy(bb->buf + bb->count, buf, length);
	bb->count += length;
    }
    else if (R_WriteConnection(bb->con, buf, length) != length)
	error(_("error writing to connection"));
}

static void InitBConOutPStream(R_outpstream_t stream, bconbuf_t bb,
			       Rconnection con,
			       R_pstream_format_t type, int version,
			       SEXP (*phook)(SEXP, SEXP), SEXP pdata)
{
    bb->count = 0;
    bb->con = con;
    R_InitOutPStream(stream, (R_pstream_data_t) bb, type, version,
		     OutCharBB, OutBytesBB, phook, pdata);
}

/* only for use by serialize(), with binary write to a socket connection */
SEXP attribute_hidden
R_serializeb (SEXP object, SEXP icon, SEXP xdr, SEXP Sversion, SEXP fun,
              SEXP nosharing_SEXP)
{
    struct R_outpstream_st out;
    SEXP (*hook)(SEXP, SEXP);
    struct bconbuf_st bbs;
    Rconnection con = getConnection(asInteger(icon));
    int version;

    if (Sversion == R_NilValue)
	version = R_DefaultSerializeVersion;
    else version = asInteger(Sversion);
    if (version == NA_INTEGER || version <= 0)
	error(_("bad version value"));

    hook = fun != R_NilValue ? CallHook : NULL;

    int nosharing = asLogical(nosharing_SEXP);

    InitBConOutPStream(&out, &bbs, con,
		       asLogical(xdr) ? R_pstream_xdr_format : R_pstream_binary_format,
		       version, hook, fun);
    R_Serialize_internal (object, &out, nosharing);
    flush_bcon_buffer(&bbs);
    return R_NilValue;
}


/*
 * Persistent Memory Streams
 */

typedef struct membuf_st {
    R_size_t size;
    R_size_t count;
    unsigned char *buf;
} *membuf_t;


#define INCR MAXELTSIZE
static attribute_noinline void resize_buffer (membuf_t mb, double dneeded)
{
    /* we need to store the result in a RAWSXP so limited to INT_MAX */
    if (dneeded > INT_MAX)
	error(_("serialization is too large to store in a raw vector"));

    R_size_t needed = dneeded;
    if(needed < 10000000) /* ca 10MB */
	needed = (1+2*needed/INCR) * INCR;
    if(needed < 1000000000) /* ca 1GB */
	needed = (1+1.2*needed/INCR) * INCR;
    else if(needed < INT_MAX - INCR)
	needed = (1+needed/INCR) * INCR;

    unsigned char *tmp = realloc(mb->buf, needed);
    if (tmp == NULL) {
	free(mb->buf); mb->buf = NULL;
	error(_("cannot allocate buffer"));
    }
    else mb->buf = tmp;

    mb->size = needed;
}

static void OutCharMem(R_outpstream_t stream, int c)
{
    membuf_t mb = stream->data;
    if (mb->count >= mb->size) {
        resize_buffer (mb, (double)mb->count + 1);
    }
    mb->buf[mb->count++] = c;
}

static void OutBytesMem(R_outpstream_t stream, void *buf, int length)
{
    membuf_t mb = stream->data;

    if (mb->count > (double)mb->size - length)  /* need to avoid overflow */
        resize_buffer (mb, mb->count + (double) length);

    char *p = mb->buf + mb->count;
    mb->count += length;

    if (length == sizeof(int))          /* a common case */
        memcpy (p, buf, sizeof(int));
    else if (length == sizeof(double))  /* another common case */
        memcpy (p, buf, sizeof(double));
    else 
        memcpy (p, buf, length);
}

static int InCharMem(R_inpstream_t stream)
{
    membuf_t mb = stream->data;
    if (mb->count >= mb->size)
	error(_("read error"));
    return mb->buf[mb->count++];
}

static void InBytesMem(R_inpstream_t stream, void *buf, int length)
{
    membuf_t mb = stream->data;
    if (mb->count + (R_size_t) length > mb->size)
	error(_("read error"));

    char *p = mb->buf + mb->count;
    mb->count += length;

    if (length == sizeof(int))          /* a common case */
        memcpy (buf, p, sizeof(int));
    else if (length == sizeof(double))  /* another common case */
        memcpy (buf, p, sizeof(double));
    else 
        memcpy (buf, p, length);
}

static void InitMemInPStream(R_inpstream_t stream, membuf_t mb,
			     void *buf, int length,
			     SEXP (*phook)(SEXP, SEXP), SEXP pdata)
{
    mb->count = 0;
    mb->size = length;
    mb->buf = buf;
    R_InitInPStream(stream, (R_pstream_data_t) mb, R_pstream_any_format,
		    InCharMem, InBytesMem, phook, pdata);
}

static void InitMemOutPStream(R_outpstream_t stream, membuf_t mb,
			      R_pstream_format_t type, int version,
			      SEXP (*phook)(SEXP, SEXP), SEXP pdata)
{
    mb->count = 0;
    mb->size = 0;
    mb->buf = NULL;
    R_InitOutPStream(stream, (R_pstream_data_t) mb, type, version,
		     OutCharMem, OutBytesMem, phook, pdata);
}

static void free_mem_buffer(void *data)
{
    membuf_t mb = data;
    if (mb->buf != NULL) {
	unsigned char *buf = mb->buf;
	mb->buf = NULL;
	free(buf);
    }
}

static SEXP CloseMemOutPStream(R_outpstream_t stream)
{
    SEXP val;
    membuf_t mb = stream->data;
    /* duplicate check, for future proofing */
    if(mb->count > INT_MAX)
	error(_("serialization is too large to store in a raw vector"));
    PROTECT(val = allocVector(RAWSXP, mb->count));
    memcpy(RAW(val), mb->buf, mb->count);
    free_mem_buffer(mb);
    UNPROTECT(1);
    return val;
}

SEXP attribute_hidden
R_serialize (SEXP object, SEXP icon, SEXP ascii, SEXP Sversion, SEXP fun,
             SEXP nosharing_SEXP)
{
    struct R_outpstream_st out;
    R_pstream_format_t type;
    SEXP (*hook)(SEXP, SEXP);
    int version;

    if (Sversion == R_NilValue)
	version = R_DefaultSerializeVersion;
    else version = asInteger(Sversion);
    if (version == NA_INTEGER || version <= 0)
	error(_("bad version value"));

    hook = fun != R_NilValue ? CallHook : NULL;

    int asc = asLogical(ascii);
    if (asc == NA_LOGICAL) type = R_pstream_binary_format;
    else if (asc) type = R_pstream_ascii_format;
    else type = R_pstream_xdr_format; /**** binary or ascii if no XDR? */

    int nosharing = asLogical(nosharing_SEXP);

    if (icon == R_NilValue) {
	RCNTXT cntxt;
	struct membuf_st mbs;
	SEXP val;

	/* set up a context which will free the buffer if there is an error */
	begincontext(&cntxt, CTXT_CCODE, R_NilValue, R_BaseEnv, R_BaseEnv,
		     R_NilValue, R_NilValue);
	cntxt.cend = &free_mem_buffer;
	cntxt.cenddata = &mbs;

	InitMemOutPStream(&out, &mbs, type, version, hook, fun);
	R_Serialize_internal (object, &out, nosharing);

	PROTECT(val = CloseMemOutPStream(&out));

	/* end the context after anything that could raise an error but before
	   calling OutTerm so it doesn't get called twice */
	endcontext(&cntxt);

	UNPROTECT(1); /* val */
	return val;
    }
    else {
	Rconnection con = getConnection(asInteger(icon));
	R_InitConnOutPStream(&out, con, type, 0, hook, fun);
	R_Serialize_internal (object, &out, nosharing);
	return R_NilValue;
    }
}


SEXP attribute_hidden R_unserialize(SEXP icon, SEXP fun)
{
    struct R_inpstream_st in;
    SEXP (*hook)(SEXP, SEXP);

    hook = fun != R_NilValue ? CallHook : NULL;

    if (TYPEOF(icon) == STRSXP && LENGTH(icon) > 0)
	/* was the format in R < 2.4.0, removed in R 2.8.0 */
	error("character vectors are no longer accepted by unserialize()");
    else if (TYPEOF(icon) == RAWSXP) {
	struct membuf_st mbs;
	void *data = RAW(icon);
	int length = LENGTH(icon);
	InitMemInPStream(&in, &mbs, data,  length, hook, fun);
	return R_Unserialize(&in);
    } 
    else {
	Rconnection con = getConnection(asInteger(icon));
	R_InitConnInPStream(&in, con, R_pstream_any_format, hook, fun);
	return R_Unserialize(&in);
    }
}


/*
 * Support Code for Lazy Loading of Packages
 */


#define IS_PROPER_STRING(s) (TYPEOF(s) == STRSXP && LENGTH(s) > 0)

/* Appends a raw vector to the end of a file using binary mode.
   Returns an integer vector of the initial offset of the string in
   the file and the length of the vector. */

static SEXP appendRawToFile(SEXP file, SEXP bytes)
{
    FILE *fp;
    size_t len, out;
    long pos;
    SEXP val;

    if (! IS_PROPER_STRING(file))
	error(_("not a proper file name"));
    if (TYPEOF(bytes) != RAWSXP)
	error(_("not a proper raw vector"));
#ifdef HAVE_WORKING_FTELL
    /* Windows' ftell returns position 0 with "ab" */
    if ((fp = R_fopen(CHAR(STRING_ELT(file, 0)), "ab")) == NULL) {
	error( _("cannot open file '%s': %s"), CHAR(STRING_ELT(file, 0)),
	       strerror(errno));
    }
#else
    if ((fp = R_fopen(CHAR(STRING_ELT(file, 0)), "r+b")) == NULL) {
	error( _("cannot open file '%s': %s"), CHAR(STRING_ELT(file, 0)),
	       strerror(errno));
    }
    fseek(fp, 0, SEEK_END);
#endif

    len = LENGTH(bytes);
    pos = ftell(fp);
    out = fwrite(RAW(bytes), 1, len, fp);
    fclose(fp);

    if (out != len) error(_("write failed"));
    if (pos == -1) error(_("could not determine file position"));

    val = allocVector(INTSXP, 2);
    INTEGER(val)[0] = pos;
    INTEGER(val)[1] = len;
    return val;
}

/* Interface to cache the pkg.rdb files */

#define NC 100
static int used = 0;
static char names[NC][PATH_MAX];
static char *ptr[NC];

SEXP attribute_hidden R_lazyLoadDBflush(SEXP file)
{
    int i;
    const char *cfile = CHAR(STRING_ELT(file, 0));

    /* fprintf(stderr, "flushing file %s", cfile); */
    for (i = 0; i < used; i++)
	if(strcmp(cfile, names[i]) == 0) {
	    strcpy(names[i], "");
	    free(ptr[i]);
	    /* fprintf(stderr, " found at pos %d in cache", i); */
	    break;
	}
    /* fprintf(stderr, "\n"); */
    return R_NilValue;
}


/* Reads, in binary mode, the bytes in the range specified by a
   position/length vector and returns them as raw vector. */

/* There are some large lazy-data examples, e.g. 80Mb for SNPMaP.cdm */
#define LEN_LIMIT 10*1048576
static SEXP readRawFromFile(SEXP file, SEXP key)
{
    FILE *fp;
    int offset, len, in, i, icache = -1, filelen;
    SEXP val;
    const char *cfile = CHAR(STRING_ELT(file, 0));

    if (! IS_PROPER_STRING(file))
	error(_("not a proper file name"));
    if (TYPEOF(key) != INTSXP || LENGTH(key) != 2)
	error(_("bad offset/length argument"));

    offset = INTEGER(key)[0];
    len = INTEGER(key)[1];

    val = allocVector(RAWSXP, len);
    /* Do we have this database cached? */
    for (i = 0; i < used; i++)
	if(strcmp(cfile, names[i]) == 0) {icache = i; break;}
    if (icache >= 0) {
	memcpy(RAW(val), ptr[icache]+offset, len);
	return val;
    }

    /* find a vacant slot? */
    for (i = 0; i < used; i++)
	if(strcmp("", names[i]) == 0) {icache = i; break;}
    if(icache < 0 && used < NC) icache = used++;

    if(icache >= 0) {
	if ((fp = R_fopen(cfile, "rb")) == NULL)
	    error(_("cannot open file '%s': %s"), cfile, strerror(errno));
	if (fseek(fp, 0, SEEK_END) != 0) {
	    fclose(fp);
	    error(_("seek failed on %s"), cfile);
	}
	filelen = ftell(fp);
	if (filelen < LEN_LIMIT) {
	    char *p;
	    /* fprintf(stderr, "adding file '%s' at pos %d in cache, length %d\n",
	       cfile, icache, filelen); */
	    p = (char *) malloc(filelen);
	    if (p) {
		strcpy(names[icache], cfile);
		ptr[icache] = p;
		if (fseek(fp, 0, SEEK_SET) != 0) {
		    fclose(fp);
		    error(_("seek failed on %s"), cfile);
		}
		in = fread(p, 1, filelen, fp);
		fclose(fp);
		if (filelen != in) error(_("read failed on %s"), cfile);
		memcpy(RAW(val), p+offset, len);
	    } else {
		if (fseek(fp, offset, SEEK_SET) != 0) {
		    fclose(fp);
		    error(_("seek failed on %s"), cfile);
		}
		in = fread(RAW(val), 1, len, fp);
		fclose(fp);
		if (len != in) error(_("read failed on %s"), cfile);
	    }
	    return val;
	} else {
	    if (fseek(fp, offset, SEEK_SET) != 0) {
		fclose(fp);
		error(_("seek failed on %s"), cfile);
	    }
	    in = fread(RAW(val), 1, len, fp);
	    fclose(fp);
	    if (len != in) error(_("read failed on %s"), cfile);
	    return val;
	}
    }

    if ((fp = R_fopen(cfile, "rb")) == NULL)
	error(_("cannot open file '%s': %s"), cfile, strerror(errno));
    if (fseek(fp, offset, SEEK_SET) != 0) {
	fclose(fp);
	error(_("seek failed on %s"), cfile);
    }
    in = fread(RAW(val), 1, len, fp);
    fclose(fp);
    if (len != in) error(_("read failed on %s"), cfile);
    return val;
}

/* Gets the binding values of variables from a frame and returns them
   as a list.  If the force argument is true, promises are forced;
   otherwise they are not. */

SEXP attribute_hidden R_getVarsFromFrame(SEXP vars, SEXP env, SEXP forcesxp)
{
    SEXP val, tmp, sym;
    Rboolean force;
    int i, len;

    if (TYPEOF(env) == NILSXP) {
	error(_("use of NULL environment is defunct"));
	env = R_BaseEnv;
    } else
    if (TYPEOF(env) != ENVSXP)
	error(_("bad environment"));
    if (TYPEOF(vars) != STRSXP)
	error(_("bad variable names"));
    force = asLogical(forcesxp);

    len = LENGTH(vars);
    PROTECT(val = allocVector(VECSXP, len));
    for (i = 0; i < len; i++) {
	sym = installChar(STRING_ELT(vars, i));

	tmp = findVarInFrame(env, sym);
	if (tmp == R_UnboundValue)
	    error(_("object '%s' not found"), CHAR(STRING_ELT(vars, i)));
	if (force && TYPEOF(tmp) == PROMSXP) {
	    PROTECT(tmp);
	    tmp = eval(tmp, R_GlobalEnv);
	    SET_NAMEDCNT_MAX(tmp);
	    UNPROTECT(1);
	}
	else if (NAMEDCNT_EQ_0(tmp))
	    SET_NAMEDCNT_1(tmp);
	SET_VECTOR_ELT(val, i, tmp);
    }
    setAttrib(val, R_NamesSymbol, vars);
    UNPROTECT(1);

    return val;
}

SEXP R_compress1(SEXP in);
SEXP R_decompress1(SEXP in);
SEXP R_compress2(SEXP in);
SEXP R_decompress2(SEXP in);
SEXP R_compress3(SEXP in);
SEXP R_decompress3(SEXP in);

/* Serializes and, optionally, compresses a value and appends the
   result to a file.  Returns the key position/length key for
   retrieving the value */

SEXP attribute_hidden
R_lazyLoadDBinsertValue(SEXP value, SEXP file, SEXP ascii,
			SEXP compsxp, SEXP hook)
{
    PROTECT_INDEX vpi;
    int compress = asInteger(compsxp);
    SEXP key;

    value = R_serialize(value, R_NilValue, ascii, R_NilValue, hook, 
                        ScalarLogicalMaybeConst(FALSE));
    PROTECT_WITH_INDEX(value, &vpi);
    if (compress == 3)
	REPROTECT(value = R_compress3(value), vpi);
    else if (compress == 2)
	REPROTECT(value = R_compress2(value), vpi);
    else if (compress)
	REPROTECT(value = R_compress1(value), vpi);
    key = appendRawToFile(file, value);
    UNPROTECT(1);
    return key;
}

/* Retrieves a sequence of bytes as specified by a position/length key
   from a file, optionally decompresses, and unserializes the bytes.
   If the result is a promise, then the promise is forced. */

static SEXP do_lazyLoadDBfetch(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP key, file, compsxp, hook;
    PROTECT_INDEX vpi;
    int compressed;
    SEXP val;

    checkArity(op, args);
    key = CAR(args); args = CDR(args);
    file = CAR(args); args = CDR(args);
    compsxp = CAR(args); args = CDR(args);
    hook = CAR(args);
    compressed = asInteger(compsxp);

    PROTECT_WITH_INDEX(val = readRawFromFile(file, key), &vpi);
    if (compressed == 3)
	REPROTECT(val = R_decompress3(val), vpi);
    else if (compressed == 2)
	REPROTECT(val = R_decompress2(val), vpi);
    else if (compressed)
	REPROTECT(val = R_decompress1(val), vpi);
    val = R_unserialize(val, hook);
    if (TYPEOF(val) == PROMSXP) {
	REPROTECT(val, vpi);
	val = eval(val, R_GlobalEnv);
	SET_NAMEDCNT_MAX(val);
    }
    UNPROTECT(1);
    return val;
}

/* FUNTAB entries defined in this source file. See names.c for documentation. */

attribute_hidden FUNTAB R_FunTab_serialize[] =
{
/* printname	c-entry		offset	eval	arity	pp-kind	     precedence	rightassoc */

{"serializeToConn",	do_serializeToConn,	0,	111,	6,	{PP_FUNCALL, PREC_FN,	0}},
{"unserializeFromConn",	do_unserializeFromConn,	0,	111,	2,	{PP_FUNCALL, PREC_FN,	0}},

{"lazyLoadDBfetch",do_lazyLoadDBfetch,0,1,	4,	{PP_FUNCALL, PREC_FN,	0}},

{NULL,		NULL,		0,	0,	0,	{PP_INVALID, PREC_FN,	0}}
};
