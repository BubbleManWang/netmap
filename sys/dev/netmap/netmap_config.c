/*
 * Copyright (C) 2014 Giuseppe Lettieri. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/* $FreeBSD: readp/sys/dev/netmap/netmap_pipe.c 261909 2014-02-15 04:53:04Z luigi $ */

#if defined(__FreeBSD__)
#include <sys/cdefs.h> /* prerequisite */

#include <sys/types.h>
#include <sys/errno.h>
#include <sys/param.h>	/* defines used in kernel.h */
#include <sys/kernel.h>	/* types used in module initialization */
#include <sys/malloc.h>
#include <sys/poll.h>
#include <sys/lock.h>
#include <sys/rwlock.h>
#include <sys/selinfo.h>
#include <sys/sysctl.h>
#include <sys/socket.h> /* sockaddrs */
#include <net/if.h>
#include <net/if_var.h>
#include <machine/bus.h>	/* bus_dmamap_* */
#include <sys/refcount.h>
#include <sys/uio.h>
#include <machine/stdarg.h>


#elif defined(linux)

#include "bsd_glue.h"

#elif defined(__APPLE__)

#warning OSX support is only partial
#include "osx_glue.h"

#else

#error	Unsupported platform

#endif /* unsupported */

/*
 * common headers
 */

#include <net/netmap.h>
#include <dev/netmap/netmap_kern.h>
#include <dev/netmap/netmap_mem2.h>
#include "jsonlr.h"

#ifdef WITH_NMCONF

#define NM_CBDATASIZ 1024
#define NM_CBDATAMAX 4

/* simple buffers for incoming/outgoing data on read()/write() */

struct nm_confb_data {
	struct nm_confb_data *chain;
	u_int size;
	char data[];
};

static void
nm_confb_trunc(struct nm_confb *cb)
{
	if (cb->writep)
		cb->writep->size = cb->next_w;
}

/* prepare for a write of req_size bytes;
 * returns a pointer to a buffer that can be used for writing,
 * or NULL if not enough space is available;
 * By passing in avl_size, the caller declares that it is
 * willing to accept a buffer with a smaller size than requested.
 */
static void*
nm_confb_pre_write(struct nm_confb *cb, u_int req_size, u_int *avl_size)
{
	struct nm_confb_data *d, *nd;
	u_int s = 0, b;
	void *ret;

	d = cb->writep;
	/* get the current available space */
	if (d)
		s = d->size - cb->next_w;
	if (s > 0 && (s >= req_size || avl_size)) {
		b = cb->next_w;
		goto out;
	}
	/* we need to expand the buffer, if possible */
	if (cb->n_data >= NM_CBDATAMAX)
		return NULL;
	s = NM_CBDATASIZ;
	if (req_size > s && avl_size == NULL)
		s = req_size;
	nd = malloc(sizeof(*d) + s, M_DEVBUF, M_NOWAIT);
	if (nd == NULL)
		return NULL;
	nd->size = s;
	nd->chain = NULL;
	if (d) {
		/* the caller is not willing to do a short write
		 * and the available space in the current chunk
		 * is not big enough. Truncate the chunk and
		 * move to the next one.
		 */
		nm_confb_trunc(cb);
		d->chain = nd;
	}
	cb->n_data++;
	if (cb->readp == NULL) {
		/* this was the first chunk, 
		 * initialize all pointers
		 */
		cb->readp = cb->writep = nd;
	}
	d = nd;
	b = 0;
out:
	if (s > req_size)
		s = req_size;
	if (avl_size)
		*avl_size = s;
	ret = d->data + b;
	return ret;
}

static void
nm_confb_post_write(struct nm_confb *cb, u_int size)
{
	if (cb->next_w == cb->writep->size) {
		cb->writep = cb->writep->chain;
		cb->next_w = 0;
	}
	cb->next_w += size;

}

static int
nm_confb_printf(struct nm_confb *cb, const char *format, ...)
{
	va_list ap;
	int rv;
        u_int size = 64, *psz = &size;
	void *p;

	for (;;) {
		p = nm_confb_pre_write(cb, size, psz);
		if (p == NULL)
			return ENOMEM;
		va_start(ap, format);
		rv = vsnprintf(p, size, format, ap);
		va_end(ap);
		if (rv < size)
			break;
		D("rv %d size %u: retry", rv, size);
		size = rv + 1;
		psz = NULL;
	}
	if (rv >= 0)
		nm_confb_post_write(cb, rv);
	return 0;
}

#define nm_confb_iprintf(cb, i, fmt, ...)					\
	({									\
		int __j, __rv = 0;						\
		for (__j = 0; __j < (i); __j++)	{				\
			__rv = nm_confb_printf(cb, "    ");			\
	 		if (__rv)						\
	 			break;						\
	 	}								\
	 	if (__rv == 0)							\
			__rv = nm_confb_printf(cb, fmt, ##__VA_ARGS__);		\
	 	__rv;								\
	 })

/* prepare for a read of size bytes;
 * returns a pointer to a buffer which is at least size bytes big.
 * Note that, on return, size may be smaller than asked for;
 * if size is 0, no other bytes can be read.
 */
static void*
nm_confb_pre_read(struct nm_confb *cb, u_int *size)
{
	struct nm_confb_data *d;
	u_int n;

	d = cb->readp;
	n = cb->next_r;
	for (;;) {
		if (d == NULL) {
			*size = 0;
			return NULL;
		}
		if (d->size > n) {
			/* there is something left to read
			 * in this chunk
			 */
			u_int s = d->size - n;
			void *ret = d->data + n;
			if (*size < s)
				s = *size;
			else
				*size = s;
			return ret;
		}
		/* chunk exausted, move to the next one */
		d = d->chain;
		n = 0;
	}
}

static void
nm_confb_post_read(struct nm_confb *cb, u_int size)
{
	if (cb->next_r == cb->readp->size) {
		struct nm_confb_data *ocb = cb->readp;
		cb->readp = cb->readp->chain;
		cb->next_r = 0;
		free(ocb, M_DEVBUF);
		cb->n_data--;
	}
	cb->next_r += size;
}

static int
nm_confb_empty(struct nm_confb *cb)
{
	u_int sz = 1;
	return (nm_confb_pre_read(cb, &sz) == NULL);
}

struct nm_jp_stream {
	struct _jp_stream stream;
	struct nm_confb *cb;
};

static int
nm_confb_peek(struct _jp_stream *jp)
{
	struct nm_jp_stream *n = (struct nm_jp_stream *)jp;
	struct nm_confb *cb = n->cb;
	u_int s = 1;
	void *p = nm_confb_pre_read(cb, &s);
	if (p == NULL)
		return 0;
	return *(char *)p;
}

static void
nm_confb_consume(struct _jp_stream *jp)
{
	struct nm_jp_stream *n = (struct nm_jp_stream *)jp;
	struct nm_confb *cb = n->cb;
	nm_confb_post_read(cb, 1);
}

static void
nm_confb_destroy(struct nm_confb *cb)
{
	struct nm_confb_data *d = cb->readp;

	while (d) {
		struct nm_confb_data *nd = d->chain;
		free(d, M_DEVBUF);
		d = nd;
	}
	memset(cb, 0, sizeof(*cb));
}

static int nm_conf_dump_json(const char *pool, struct _jpo*,
		struct nm_confb *);
static int nm_conf_dump_flat(const char *pool, struct _jpo*,
		struct nm_confb *);
extern int nm_conf_flat_mode;
void
nm_conf_init(struct nm_conf *c)
{
	NM_MTX_INIT(c->mux);
	c->dump = (nm_conf_flat_mode ?
	           nm_conf_dump_flat :
		   nm_conf_dump_json);
}

const char *
nm_conf_get_output_mode(struct nm_conf *c)
{
	return (c->dump == nm_conf_dump_json ? "json" :
	       (c->dump == nm_conf_dump_flat ? "flat" :
		"unknown"));
}

void
nm_conf_uninit(struct nm_conf *c, int locked)
{
	int i;
	
	(void)nm_conf_parse(c, locked);
	for (i = 0; i < 2; i++)
		nm_confb_destroy(c->buf + i);
	NM_MTX_DESTROY(c->mux);
}

static int
nm_conf_dump_json_rec(const char *pool, struct _jpo *r,
		struct nm_confb *out, int ind, int cont)
{
	int i, error = 0;
again:
	switch (r->ty) {
	case JPO_NUM:
		return nm_confb_iprintf(out, (cont ? 0 : ind),
				"%ld", jslr_get_num(pool, *r));
		break;
	case JPO_STRING:
		return nm_confb_iprintf(out, (cont ? 0 : ind),
				"\"%s\"", jslr_get_string(pool, *r));
		break;
	case JPO_ARRAY:
		error = nm_confb_iprintf(out, (cont ? 0 : ind), "[");
		for (i = 0; !error && i < r->len; i++) {
			if (i)
				error = nm_confb_printf(out, ",");
			if (!error)
				error = nm_confb_printf(out, "\n");
			if (!error)
				error = nm_conf_dump_json_rec(pool, r + 1 + i,
					out, ind + 1, 0);
		}
		if (!error)
			error = nm_confb_printf(out, "\n");
		if (!error)
			error = nm_confb_iprintf(out, ind, "]");
		break;
	case JPO_OBJECT:
		error = nm_confb_iprintf(out, (cont ? 0: ind), "{");
		for (i = 0; !error && (i < 2 * r->len); i += 2) {
			if (i)
				error = nm_confb_printf(out, ",");
			if (!error)
				error = nm_confb_printf(out, "\n");
			if (!error)
				error = nm_confb_iprintf(out, ind + 1,
					"\"%s\": ",
					jslr_get_string(pool, *(r + 1 + i)));
			if (!error)
				error = nm_conf_dump_json_rec(pool, r + 2 + i,
					out, ind + 1, 1);
		}
		if (!error)
			error = nm_confb_printf(out, "\n");
		if (!error)
			nm_confb_iprintf(out, ind, "}");
		break;
	case JPO_PTR:
		switch (r->len) {
		case JPO_ARRAY:
			r = jslr_get_array(pool, *r);
			break;
		case JPO_OBJECT:
			r = jslr_get_object(pool, *r);
			break;
		default:
			return EINVAL;
		}
		goto again;
	default:
		error = EINVAL;
		break;
	}
	return error;
}

static int
nm_conf_dump_json(const char *pool, struct _jpo* r,
		struct nm_confb *cb)
{
	int error;

	error = nm_conf_dump_json_rec(pool, r, cb, 0, 0);
	if (error)
		return error;
	nm_confb_printf(cb, "\n");
	return 0;
}

struct nm_flat_prefix {
	char *base;
	char *append;
	size_t avail;
};

static int
nm_flat_prefix_append(struct nm_flat_prefix *st, const char *fmt, ...)
{
	int n;
	va_list ap;

	va_start(ap, fmt);
	n = vsnprintf(st->append, st->avail, fmt, ap);
	va_end(ap);
	if (n < 0 || n >= st->avail)
		return ENOMEM;
	st->append += n;
	st->avail -= n;
	return 0;
}

static int
nm_conf_dump_flat_rec(const char *pool, struct _jpo *r,
		struct nm_confb *out, const struct nm_flat_prefix *st)
{
	int i, error = 0;
	struct nm_flat_prefix lst;
again:
	switch (r->ty) {
	case JPO_NUM:
		return nm_confb_printf(out, "%s: %ld\n",
				st->base, jslr_get_num(pool, *r));
		break;
	case JPO_STRING:
		return nm_confb_printf(out, "%s: \"%s\"\n",
				st->base, jslr_get_string(pool, *r));
		break;
	case JPO_ARRAY:
		for (i = 0; !error && i < r->len; i++) {
			lst = *st;
			error = nm_flat_prefix_append(&lst, ".%d", i);
			if (!error)
				error = nm_conf_dump_flat_rec(pool, r + 1 + i,
					out, &lst);
		}
		break;
	case JPO_OBJECT:
		for (i = 0; !error && (i < 2 * r->len); i += 2) {
			lst = *st;
			error = nm_flat_prefix_append(&lst, ".%s",
					jslr_get_string(pool, *(r + 1 + i)));
			if (!error)
				error = nm_conf_dump_flat_rec(pool, r + 2 + i,
					out, &lst);
		}
		break;
	case JPO_PTR:
		switch (r->len) {
		case JPO_ARRAY:
			r = jslr_get_array(pool, *r);
			break;
		case JPO_OBJECT:
			r = jslr_get_object(pool, *r);
			break;
		default:
			return EINVAL;
		}
		goto again;
	default:
		error = EINVAL;
		break;
	}
	return error;
}

static int
nm_conf_dump_flat(const char *pool, struct _jpo *r,
		struct nm_confb *cb)
{
	char buf[128];
	struct nm_flat_prefix lst = {
		.base = buf,
		.append = buf,
		.avail = 128
	};

	return nm_conf_dump_flat_rec(pool, r, cb, &lst);
}

#define NETMAP_CONFIG_POOL_SIZE (1<<12)

static struct _jpo nm_jp_interp(struct nm_jp *,
		struct _jpo, struct nm_conf *c);

int
nm_conf_parse(struct nm_conf *c, int locked)
{
	uint32_t pool_len = NETMAP_CONFIG_POOL_SIZE;
	struct nm_confb *i = &c->buf[0],
			      *o = &c->buf[1];
	struct nm_jp_stream njs = {
		.stream = {
			.peek = nm_confb_peek,
			.consume = nm_confb_consume,
		},
		.cb = i,
	};
	struct _jpo r;
	int error = 0;

	nm_confb_trunc(i);
	if (nm_confb_empty(i))
		return 0;

	c->pool = malloc(pool_len, M_DEVBUF, M_ZERO);
	if (c->pool == NULL)
		return ENOMEM;
	r = jslr_parse(&njs.stream, c->pool, pool_len);
	if (r.ty == JPO_ERR) {
		D("parse error: %d", r.ptr);
		nm_confb_destroy(i);
		goto out;
	}
	D("parse OK: ty %u len %u ptr %u", r.ty, r.len, r.ptr);
	if (!locked)
		NMG_LOCK();
	r = nm_jp_interp(&nm_jp_root.up, r, c);
	if (!locked)
		NMG_UNLOCK();
	error = c->dump(c->pool, &r, o);
	nm_confb_trunc(o);
out:
	free(c->pool, M_DEVBUF);
	c->pool = NULL;
	return error;
}

int
nm_conf_write(struct nm_conf *c, struct uio *uio)
{
	int ret = 0;
	struct nm_confb *i = &c->buf[0],
			      *o = &c->buf[1];

	NM_MTX_LOCK(c->mux);

	nm_confb_destroy(o);

	while (uio->uio_resid > 0) {
		int s = uio->uio_resid;
		void *p = nm_confb_pre_write(i, s, &s);
		if (p == NULL) {
			ND("NULL p from confbuf_pre_write");
			ret = ENOMEM;
			goto out;
		}
		ND("s %d", s);
		ret = uiomove(p, s, uio);
		if (ret)
			goto out;
		nm_confb_post_write(i, s);
		c->written = 1;
	}

out:
	NM_MTX_UNLOCK(c->mux);
	return ret;
}

int
nm_conf_read(struct nm_conf *c, struct uio *uio)
{
	int ret = 0;
	struct nm_confb *i = &c->buf[0],
			      *o = &c->buf[1];

	NM_MTX_LOCK(c->mux);

	if (!c->written) {
		nm_confb_printf(i, "dump");
		c->written = 1;
	}

	ret = nm_conf_parse(c, 0 /* not locked */);
	if (ret)
		goto out;

	while (uio->uio_resid > 0) {
		int s = uio->uio_resid;
		void *p = nm_confb_pre_read(o, &s);
		if (p == NULL) {
			goto out;
		}
		ret = uiomove(p, s, uio);
		if (ret)
			goto out;
		nm_confb_post_read(o, s);
	}

out:
	NM_MTX_UNLOCK(c->mux);

	return ret;
}


struct _jpo
nm_jp_error(char *pool, const char *format, ...)
{
	va_list ap;
	struct _jpo r, *o;
#define NM_INTERP_ERRSIZE 128
	char buf[NM_INTERP_ERRSIZE + 1];
	int rv;

	r = jslr_new_object(pool, 1);
	if (r.ty == JPO_ERR)
		return r;
	o = jslr_get_object(pool, r);
	o++;
	*o = jslr_new_string(pool, "err");
	if (o->ty == JPO_ERR)
		return *o;
	o++;
	va_start(ap, format);
	rv = vsnprintf(buf, NM_INTERP_ERRSIZE, format, ap);
	va_end(ap);
	if (rv < 0 || rv >= NM_INTERP_ERRSIZE)
		return (struct _jpo) {.ty = JPO_ERR};
	*o = jslr_new_string(pool, buf);
	if (o->ty == JPO_ERR)
		return *o;
	return r;
#undef	NM_INTERP_ERRSIZE
}

static int
nm_jp_streq(struct _jpo r, char *pool, const char *str1)
{
	const char *str;

	if (r.ty != JPO_STRING)
		return 0;

	str = jslr_get_string(pool, r);
	return (strcmp(str1, str) == 0);
}

static int
nm_jp_is_dump(struct _jpo r, char *pool)
{
	return nm_jp_streq(r, pool, "dump");
}

static void
nm_jp_bracket(struct nm_jp *ip, int stage, struct nm_conf *c)
{
	if (ip->bracket)
		ip->bracket(ip, stage, c);
}

static struct _jpo
nm_jp_interp(struct nm_jp *ip, struct _jpo r, struct nm_conf *c)
{
	nm_jp_bracket(ip, 0, c);
	if (nm_jp_is_dump(r, c->pool) || ip->interp == NULL) {
		r = ip->dump(ip, c);
	} else {
		r = ip->interp(ip, r, c);
	}
	nm_jp_bracket(ip, 2, c);
	return r;
}

static struct _jpo
nm_jp_dump(struct nm_jp *ip, struct nm_conf *c)
{
	struct _jpo r;

	nm_jp_bracket(ip, 0, c);
	r = ip->dump(ip, c);
	nm_jp_bracket(ip, 2, c);
	return r;
}

static struct _jpo
nm_jp_ldelete(struct nm_jp_list *il, struct nm_jp_lelem *e,
		char *pool)
{
	if (il->delete == NULL)
		return nm_jp_error(pool, "'delete' not supported");
	if (!e->have_ref)
		return nm_jp_error(pool, "busy");
	il->delete(e->ip);
	e->have_ref = 0;
	return jslr_new_object(pool, 0);
}

static struct nm_jp_lelem *
nm_jp_lsearch(struct nm_jp_list *il, const char *name);

static struct _jpo
nm_jp_lnew(struct nm_jp_list *il, struct _jpo *pn, struct nm_conf *c)
{
	struct nm_jp_lelem *e = NULL;
	struct nm_jp *ip;
	struct _jpo o;
	int error;

	if (il->new == NULL) {
		o = nm_jp_error(c->pool, "not supported");
		goto out;
	}
	e = nm_jp_lnew_elem(il);
	if (e == NULL) {
		o = nm_jp_error(c->pool, "out of memory");
		goto out;
	}
	error = il->new(e);
	if (error || e->ip == NULL) {
		o = nm_jp_error(c->pool, "error: %d", error);
		goto out;
	}
	*pn++ = jslr_new_string(c->pool, e->name);
	ip = e->ip;
	nm_jp_bracket(ip, 0, c);
	if (ip->interp) {
		o = ip->interp(ip, *pn, c);
		if (o.ty == JPO_ERR)
			goto leave;
		nm_jp_bracket(ip, 1, c);
	}
	o = ip->dump(ip, c);
leave:
	nm_jp_bracket(ip, 2, c);
	e->have_ref = 1;
out:
	return o;
}



static struct _jpo
nm_jp_linterp(struct nm_jp *ip, struct _jpo r, struct nm_conf *c)
{
	struct _jpo *po;
	int i, len, ty = r.len;
	struct nm_jp_list *il = (struct nm_jp_list *)ip;
	char *pool = c->pool;

	if (r.ty != JPO_PTR || ty != JPO_OBJECT) {
		r = nm_jp_error(pool, "need object");
		goto out;
	}

	po = jslr_get_object(c->pool, r);
	if (po == NULL || po->ty != ty) {
		r = nm_jp_error(pool, "internal error");
		goto out;
	}

	len = po->len;
	po++;
	for (i = 0; i < len; i++) {
		struct _jpo r1;
		const char *name = jslr_get_string(pool, *po);
		struct nm_jp_lelem *e;

		if (name == NULL) {
			r = nm_jp_error(pool, "internal error");
			goto out;
		}
		if (strcmp(name, "new") == 0) {
			r1 = nm_jp_lnew(il, po, c);
			po++;
			goto next;
		}
		e = nm_jp_lsearch(il, name);
		if (e == NULL) {
			po++;
			r1 = nm_jp_error(pool, "%s: not found", name);
			goto next;
		}
		po++;
		D("found %s", name);
		if (nm_jp_streq(*po, pool, "delete")) {
			r1 = nm_jp_ldelete(il, e, pool);
			goto next;
		}
		r1 = nm_jp_interp(e->ip, *po, c);
	next:
		*po++ = r1;
	}

out:
	return r;
}

static struct _jpo
nm_jp_ldump(struct nm_jp *ip, struct nm_conf *c)
{
	struct _jpo *po, r;
	struct nm_jp_list *il = (struct nm_jp_list *)ip;
	int i, len = il->nextfree;
	char *pool = c->pool;

	r = jslr_new_object(pool, len);
	if (r.ty == JPO_ERR)
		return r;
	po = jslr_get_object(pool, r);
	po++;
	for (i = 0; i < len; i++) {
		struct nm_jp_lelem *e = &il->list[i];
		*po = jslr_new_string(pool, e->name);
		if (po->ty == JPO_ERR)
			return *po;
		po++;
		*po = nm_jp_dump(e->ip, c);
		if (po->ty == JPO_ERR)
			return *po;
		po++;
	}
	return r;
}

int
nm_jp_linit(struct nm_jp_list *il, u_int nelem)
{
	il->up.interp = nm_jp_linterp;
	il->up.dump = nm_jp_ldump;
	il->minelem = nelem;
	il->list = malloc(sizeof(*il->list) * nelem, M_DEVBUF, M_ZERO);
	if (il->list == NULL)
		return ENOMEM;
	il->nelem = nelem;
	il->nextfree = 0;
	return 0;
}

void
nm_jp_luninit(struct nm_jp_list *il)
{
	free(il->list, M_DEVBUF);
	memset(il, 0, sizeof(*il));
}

struct nm_jp_lelem *
nm_jp_lnew_elem(struct nm_jp_list *il)
{
	struct nm_jp_lelem *newlist;

	if (il->nextfree >= il->nelem) {
		u_int newnelem = il->nelem * 2;
		newlist = realloc(il->list, sizeof(*il->list) * newnelem,
				M_DEVBUF, M_ZERO);
		if (newlist == NULL)
			return NULL;
		il->list = newlist;
		il->nelem = newnelem;
	}
	return &il->list[il->nextfree++];
}

int
nm_jp_lelem_fill(struct nm_jp_lelem *e,
		struct nm_jp *ip,
		const char *fmt, ...)
{
	va_list ap;
	int n;

	e->ip = ip;
	va_start(ap, fmt);
	n = vsnprintf(e->name, NETMAP_CONFIG_MAXNAME, fmt, ap);
	va_end(ap);

	if (n >= NETMAP_CONFIG_MAXNAME)
		return ENAMETOOLONG;

	return 0;
}

static int
_nm_jp_ldel(struct nm_jp_list *il, struct nm_jp_lelem *e1)
{
	struct nm_jp_lelem *e2;

	il->nextfree--;
	e2 = &il->list[il->nextfree];
	if (e1 != e2) {
		strncpy(e1->name, e2->name, NETMAP_CONFIG_MAXNAME);
		e1->ip = e2->ip;
	}
	memset(e2, 0, sizeof(*e2));
	if (il->nelem > il->minelem && il->nextfree < il->nelem / 2) {
		struct nm_jp_lelem *newlist;
		u_int newnelem = il->nelem / 2;
		if (newnelem < il->minelem)
			newnelem = il->minelem;
		newlist = realloc(il->list, newnelem, M_DEVBUF, M_ZERO);
		if (newlist == NULL) {
			D("out of memory when trying to release memory?");
			return 0; /* not fatal */
		}
		il->list = newlist;
		il->nelem = newnelem;
	}
	return 0;
}

int
nm_jp_ldel(struct nm_jp_list *il, struct nm_jp *ip)
{
	struct nm_jp_lelem *e;

	for (e = il->list; e != il->list + il->nextfree; e++)
		if (e->ip == ip)
			goto found;
	return ENOENT;
found:
	return _nm_jp_ldel(il, e);
}


static struct nm_jp_lelem *
nm_jp_lsearch(struct nm_jp_list *il, const char *name)
{
	int i;
	for (i = 0; i < il->nextfree; i++) {
		struct nm_jp_lelem *e = &il->list[i];
		if (strncmp(name, e->name, NETMAP_CONFIG_MAXNAME) == 0)
			break;
	}
	if (i == il->nextfree)
		return NULL;
	return &il->list[i];
}

static int64_t
nm_jp_ngetvar(struct nm_jp_num *in)
{
	switch (in->size) {
	case 0:
		return ((nm_jp_nreader)in->var)(in);
	case 1:
		return *(int8_t*)in->var;
	case 2:
		return *(int16_t*)in->var;
	case 4:
		return *(int32_t*)in->var;
	case 8:
		return *(int64_t*)in->var;
	default:
		D("unsupported size %zd", in->size);
		return 0;
	}
}

int
nm_jp_nupdate(struct nm_jp_num *in, int64_t v)
{
	switch (in->size) {
	case 1:
		*(int8_t*)in->var = (int8_t)v;
		break;
	case 2:
		*(int16_t*)in->var = (int16_t)v;
		break;
	case 4:
		*(int32_t*)in->var = (int32_t)v;
		break;
	case 8:
		*(int64_t*)in->var = (int64_t)v;
		break;
	default:
		return EINVAL;
	}
	return 0;
}

static struct _jpo
nm_jp_ninterp(struct nm_jp *ip, struct _jpo r, struct nm_conf *c)
{
	int64_t v, nv;
	struct nm_jp_num *in = (struct nm_jp_num *)ip;
	int error;
	char *pool = c->pool;

	if (r.ty != JPO_NUM) {
		r = nm_jp_error(pool, "need number");
		goto done;
	}

	nv = jslr_get_num(pool, r);
	v = nm_jp_ngetvar(in);
	if (v == nv)
		goto done;
	if (in->update == NULL) {
		r = nm_jp_error(pool, "read-only");
		goto done;
	}
	error = in->update(in, nv);
	if (error)
		r = nm_jp_error(pool, "invalid; %ld", nv);
	r = ip->dump(ip, c);
done:
	return r;
}

static struct _jpo
nm_jp_ndump(struct nm_jp *ip, struct nm_conf *c)
{
	struct nm_jp_num *in = (struct nm_jp_num*)ip;
	int64_t v = nm_jp_ngetvar(in);

	return jslr_new_num(c->pool, v);
}


void
nm_jp_ninit(struct nm_jp_num *in, void *var, size_t size,
		int (*update)(struct nm_jp_num *, int64_t))
{
	in->up.interp = nm_jp_ninterp;
	in->up.dump = nm_jp_ndump;
	in->var = var;
	in->size = size;
	in->update = update;
}

#endif /* WITH_NMCONF */
