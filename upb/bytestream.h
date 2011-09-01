/*
 * upb - a minimalist implementation of protocol buffers.
 *
 * Copyright (c) 2011 Google Inc.  See LICENSE for details.
 * Author: Josh Haberman <jhaberman@gmail.com>
 *
 * This file contains upb_bytesrc and upb_bytesink, which are abstractions of
 * stdio (fread()/fwrite()/etc) that provide useful buffering/sharing
 * semantics.  They are virtual base classes so concrete implementations
 * can get the data from a fd, a string, a cord, etc.
 *
 * Byte streams are NOT thread-safe!  (Like f{read,write}_unlocked())
 * This may change (in particular, bytesrc objects may be better thread-safe).
 */

#ifndef UPB_BYTESTREAM_H
#define UPB_BYTESTREAM_H

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "upb.h"

#ifdef __cplusplus
extern "C" {
#endif


/* upb_bytesrc ****************************************************************/

// A upb_bytesrc allows the consumer of a stream of bytes to obtain buffers as
// they become available, and to preserve some trailing amount of data, which
// is useful for lazy parsing (among other things).  If there is a submessage
// that we want to parse later we can take a reference on that region of the
// input buffer.  This will guarantee that the bytesrc keeps the submessage
// data around for later use, without requiring a copy out of the input
// buffers.
typedef size_t upb_bytesrc_fetch_func(void*, uint64_t, upb_status*);
typedef void upb_bytesrc_read_func(void*, uint64_t, size_t, char*);
typedef const char *upb_bytesrc_getptr_func(void*, uint64_t, size_t*);
typedef void upb_bytesrc_refregion_func(void*, uint64_t, size_t);
typedef void upb_bytesrc_ref_func(void*);
typedef struct _upb_bytesrc_vtbl {
  upb_bytesrc_fetch_func     *fetch;
  upb_bytesrc_read_func      *read;
  upb_bytesrc_getptr_func    *getptr;
  upb_bytesrc_refregion_func *refregion;
  upb_bytesrc_refregion_func *unrefregion;
  upb_bytesrc_ref_func       *ref;
  upb_bytesrc_ref_func       *unref;
} upb_bytesrc_vtbl;

typedef struct {
  upb_bytesrc_vtbl  *vtbl;
} upb_bytesrc;

INLINE void upb_bytesrc_init(upb_bytesrc *src, upb_bytesrc_vtbl *vtbl) {
  src->vtbl = vtbl;
}

// Fetches at least one byte starting at ofs, returning the actual number of
// bytes fetched (or 0 on error: see "s" for details).  A successful return
// gives caller a ref on the fetched region.
//
// If "ofs" may be greater or equal than the end of the already-fetched region.
// It may also be less than the end of the already-fetch region *if* either of
// the following is true:
//
// * the region is ref'd (this implies that the data is still in-memory)
// * the bytesrc is seekable (this implies that the data can be fetched again).
INLINE size_t upb_bytesrc_fetch(upb_bytesrc *src, uint64_t ofs, upb_status *s) {
  return src->vtbl->fetch(src, ofs, s);
}

// Copies "len" bytes of data from offset src_ofs to "dst", which must be at
// least "len" bytes long.  The caller must own a ref on the given region.
INLINE void upb_bytesrc_read(upb_bytesrc *src, uint64_t src_ofs, size_t len,
                             char *dst) {
  src->vtbl->read(src, src_ofs, len, dst);
}

// Returns a pointer to the bytesrc's internal buffer, storing in *len how much
// data is available.  The caller must own refs on the given region.  The
// returned buffer is valid for as long as the region remains ref'd.
//
// TODO: if more data is available than the caller has ref'd is it ok for the
// caller to read *len bytes?
INLINE const char *upb_bytesrc_getptr(upb_bytesrc *src, uint64_t ofs,
                                      size_t *len) {
  return src->vtbl->getptr(src, ofs, len);
}

// Gives the caller a ref on the given region.  The caller must know that the
// given region is already ref'd (for example, inside a upb_handlers callback
// that receives a upb_strref, the region is guaranteed to be ref'd -- this
// function allows that handler to take its own ref).
INLINE void upb_bytesrc_refregion(upb_bytesrc *src, uint64_t ofs, size_t len) {
  src->vtbl->refregion(src, ofs, len);
}

// Releases a ref on the given region, which the caller must have previously
// ref'd.
INLINE void upb_bytesrc_unrefregion(upb_bytesrc *src, uint64_t ofs, size_t len) {
  src->vtbl->unrefregion(src, ofs, len);
}

// Attempts to ref the bytesrc itself, returning false if this bytesrc is
// not ref-able.
INLINE bool upb_bytesrc_tryref(upb_bytesrc *src) {
  if (src->vtbl->ref) {
    src->vtbl->ref(src);
    return true;
  } else {
    return false;
  }
}

// Unref's the bytesrc itself.  May only be called when upb_bytesrc_tryref()
// has previously returned true.
INLINE void upb_bytesrc_unref(upb_bytesrc *src) {
  assert(src->vtbl->unref);
  src->vtbl->unref(src);
}


/* upb_strref *****************************************************************/

// The structure we pass to upb_handlers for a string value.
typedef struct _upb_strref {
  // Pointer to the string data.  NULL if the string spans multiple input
  // buffers (in which case upb_bytesrc_getptr() must be called to obtain
  // the actual pointers).
  const char *ptr;

  // Total length of the string.
  uint32_t len;

  // Offset in the bytesrc that represents the beginning of this string.
  uint32_t stream_offset;

  // Bytesrc from which this string data comes.  May be NULL if ptr is set.  If
  // non-NULL, the bytesrc is only guaranteed to be alive from inside the
  // callback; however if the handler knows more about its type and how to
  // prolong its life, it may do so.
  upb_bytesrc *bytesrc;

  // Possibly add optional members here like start_line, start_column, etc.
} upb_strref;

// Copies the contents of the strref into a newly-allocated, NULL-terminated
// string.
char *upb_strref_dup(struct _upb_strref *r);

INLINE void upb_strref_read(struct _upb_strref *r, char *buf) {
  if (r->ptr) {
    memcpy(buf, r->ptr, r->len);
  } else {
    assert(r->bytesrc);
    upb_bytesrc_read(r->bytesrc, r->stream_offset, r->len, buf);
  }
}


/* upb_bytesink ***************************************************************/

// A bytesink is an interface that allows the caller to push byte-wise data.
// It is very simple -- the only special capability is the ability to "rewind"
// the stream, which is really only a mechanism of having the bytesink ignore
// some subsequent calls.
typedef int upb_bytesink_write_func(void*, const void*, int);
typedef int upb_bytesink_vprintf_func(void*, const char *fmt, va_list args);

typedef struct {
  upb_bytesink_write_func   *write;
  upb_bytesink_vprintf_func *vprintf;
} upb_bytesink_vtbl;

typedef struct {
  upb_bytesink_vtbl *vtbl;
  upb_status status;
  uint64_t offset;
} upb_bytesink;

// Should be called by derived classes.
void upb_bytesink_init(upb_bytesink *sink, upb_bytesink_vtbl *vtbl);
void upb_bytesink_uninit(upb_bytesink *sink);

INLINE int upb_bytesink_write(upb_bytesink *s, const void *buf, int len) {
  return s->vtbl->write(s, buf, len);
}

INLINE int upb_bytesink_writestr(upb_bytesink *sink, const char *str) {
  return upb_bytesink_write(sink, str, strlen(str));
}

// Returns the number of bytes written or -1 on error.
INLINE int upb_bytesink_printf(upb_bytesink *sink, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  uint32_t ret = sink->vtbl->vprintf(sink, fmt, args);
  va_end(args);
  return ret;
}

INLINE int upb_bytesink_putc(upb_bytesink *sink, char ch) {
  return upb_bytesink_write(sink, &ch, 1);
}

INLINE int upb_bytesink_putrepeated(upb_bytesink *sink, char ch, int len) {
  char buf[len];
  memset(buf, ch, len);
  return upb_bytesink_write(sink, buf, len);
}

INLINE uint64_t upb_bytesink_getoffset(upb_bytesink *sink) {
  return sink->offset;
}

// Rewinds the stream to the given offset.  This cannot actually "unput" any
// data, it is for situations like:
//
// // If false is returned (because of error), call again later to resume.
// bool write_some_data(upb_bytesink *sink, int indent) {
//   uint64_t start_offset = upb_bytesink_getoffset(sink);
//   if (upb_bytesink_writestr(sink, "Some data") < 0) goto err;
//   if (upb_bytesink_putrepeated(sink, ' ', indent) < 0) goto err;
//   return true;
//  err:
//   upb_bytesink_rewind(sink, start_offset);
//   return false;
// }
//
// The subsequent bytesink writes *must* be identical to the writes that were
// rewinded past.
INLINE void upb_bytesink_rewind(upb_bytesink *sink, uint64_t offset) {
  // TODO
  (void)sink;
  (void)offset;
}

// OPT: add getappendbuf()
// OPT: add writefrombytesrc()
// TODO: add flush()


/* upb_stdio ******************************************************************/

// bytesrc/bytesink for ANSI C stdio, which is less efficient than posixfd, but
// more portable.
//
// Specifically, stdio functions acquire locks on every operation (unless you
// use the f{read,write,...}_unlocked variants, which are not standard) and
// performs redundant buffering (unless you disable it with setvbuf(), but we
// can only do this on newly-opened filehandles).

typedef struct {
  uint64_t ofs;
  uint32_t len;
  uint32_t refcount;
  char data[];
} upb_stdio_buf;

// We use a single object for both bytesrc and bytesink for simplicity.
// The object is still not thread-safe, and may only be used by one reader
// and one writer at a time.
typedef struct {
  upb_bytesrc src;
  upb_bytesink sink;
  FILE *file;
  bool should_close;
  upb_stdio_buf **bufs;
  uint32_t nbuf, szbuf;
} upb_stdio;

void upb_stdio_init(upb_stdio *stdio);
// Caller should call upb_stdio_flush prior to calling this to ensure that
// all data is flushed, otherwise data can be silently dropped if an error
// occurs flushing the remaining buffers.
void upb_stdio_uninit(upb_stdio *stdio);

// Resets the object to read/write to the given "file."  The caller is
// responsible for closing the file, which must outlive this object.
void upb_stdio_reset(upb_stdio *stdio, FILE *file);

// As an alternative to upb_stdio_reset(), initializes the object by opening a
// file, and will handle closing it.  This may result in more efficient I/O
// than the previous since we can call setvbuf() to disable buffering.
void upb_stdio_open(upb_stdio *stdio, const char *filename, const char *mode,
                    upb_status *s);

upb_bytesrc *upb_stdio_bytesrc(upb_stdio *stdio);
upb_bytesink *upb_stdio_bytesink(upb_stdio *stdio);


/* upb_stringsrc **************************************************************/

// bytesrc/bytesink for a simple contiguous string.

struct _upb_stringsrc {
  upb_bytesrc bytesrc;
  const char *str;
  size_t len;
};
typedef struct _upb_stringsrc upb_stringsrc;

// Create/free a stringsrc.
void upb_stringsrc_init(upb_stringsrc *s);
void upb_stringsrc_uninit(upb_stringsrc *s);

// Resets the stringsrc to a state where it will vend the given string.  The
// stringsrc will take a reference on the string, so the caller need not ensure
// that it outlives the stringsrc.  A stringsrc can be reset multiple times.
void upb_stringsrc_reset(upb_stringsrc *s, const char *str, size_t len);

// Returns the upb_bytesrc* for this stringsrc.
upb_bytesrc *upb_stringsrc_bytesrc(upb_stringsrc *s);


/* upb_stringsink *************************************************************/

struct _upb_stringsink {
  upb_bytesink bytesink;
  char *str;
  size_t len, size;
};
typedef struct _upb_stringsink upb_stringsink;

// Create/free a stringsrc.
void upb_stringsink_init(upb_stringsink *s);
void upb_stringsink_uninit(upb_stringsink *s);

// Resets the sink's string to "str", which the sink takes ownership of.
// "str" may be NULL, which will make the sink allocate a new string.
void upb_stringsink_reset(upb_stringsink *s, char *str, size_t size);

// Releases ownership of the returned string (which is "len" bytes long) and
// resets the internal string to be empty again (as if reset were called with
// NULL).
const char *upb_stringsink_release(upb_stringsink *s, size_t *len);

// Returns the upb_bytesink* for this stringsrc.  Invalidated by reset above.
upb_bytesink *upb_stringsink_bytesink(upb_stringsink *s);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif