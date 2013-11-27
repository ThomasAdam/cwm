/*
 * calmwm - the calm window manager
 *
 * Copyright (c) 2004 Marius Aamodt Eriksen <marius@monkey.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * $OpenBSD$
 */

#include <sys/param.h>
#include "queue.h"

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "calmwm.h"

static struct client_ctx	*client_mrunext(struct client_ctx *);
static struct client_ctx	*client_mruprev(struct client_ctx *);
static void			 client_mtf(struct client_ctx *);
static void			 client_none(struct screen_ctx *);
static void			 client_placecalc(struct client_ctx *);
static void			 client_wm_protocols(struct client_ctx *);
static void			 client_getmwmhints(struct client_ctx *);
static int			 client_inbound(struct client_ctx *, int, int);

struct client_ctx	*_curcc = NULL;

struct client_ctx *
client_find(Window win)
{
	struct client_ctx	*cc;

	TAILQ_FOREACH(cc, &Clientq, entry)
		if (cc->win == win)
			return (cc);

	return (NULL);
}

struct client_ctx *
client_init(Window win, struct screen_ctx *sc, int mapped)
{
	struct client_ctx	*cc;
	XClassHint		 xch;
	XWindowAttributes	 wattr;
	XWMHints		*wmhints;
	int			 state;

	if (win == None)
		return (NULL);

	cc = xcalloc(1, sizeof(*cc));

	XGrabServer(X_Dpy);

	cc->state = mapped ? NormalState : IconicState;
	cc->sc = sc;
	cc->win = win;

	client_getsizehints(cc);

	TAILQ_INIT(&cc->nameq);
	client_setname(cc);

	conf_client(cc);

	if (XGetClassHint(X_Dpy, cc->win, &xch)) {
		cc->app_name = xch.res_name;
		cc->app_class = xch.res_class;
	}
	client_getmwmhints(cc);

	/* Saved pointer position */
	cc->ptr.x = -1;
	cc->ptr.y = -1;

	XGetWindowAttributes(X_Dpy, cc->win, &wattr);
	cc->geom.x = wattr.x;
	cc->geom.y = wattr.y;
	cc->geom.w = wattr.width;
	cc->geom.h = wattr.height;
	cc->colormap = wattr.colormap;

	if ((wmhints = XGetWMHints(X_Dpy, cc->win)) != NULL) {
		if (wmhints->flags & InputHint) {
			if (wmhints->input == 1)
				cc->flags |= CLIENT_INPUT;
		}
	}
	if (wattr.map_state != IsViewable) {
		client_placecalc(cc);
		client_move(cc);
		if ((wmhints) && (wmhints->flags & StateHint)) {
			cc->state = wmhints->initial_state;
			xu_set_wm_state(cc->win, cc->state);
		}
	}
	if (wmhints)
		XFree(wmhints);
	client_draw_border(cc);

	if (xu_get_wm_state(cc->win, &state) < 0)
		state = NormalState;

	XSelectInput(X_Dpy, cc->win, ColormapChangeMask | EnterWindowMask |
	    PropertyChangeMask | KeyReleaseMask);

	XAddToSaveSet(X_Dpy, cc->win);

	client_transient(cc);

	/* Notify client of its configuration. */
	client_config(cc);

	(state == IconicState) ? client_hide(cc) : client_unhide(cc);

	TAILQ_INSERT_TAIL(&sc->mruq, cc, mru_entry);
	TAILQ_INSERT_TAIL(&Clientq, cc, entry);

	xu_ewmh_net_client_list(sc);

	client_wm_protocols(cc);
	xu_ewmh_restore_net_wm_state(cc);

	if (mapped)
		group_autogroup(cc);

	XSync(X_Dpy, False);
	XUngrabServer(X_Dpy);

	return (cc);
}

void
client_update_xinerama(struct client_ctx *cc)
{
	if (cc->sc->xinerama == NULL)
		return;

	cc->xinerama = screen_get_xsi(cc->sc,
	    cc->geom.x + cc->geom.w / 2,
	    cc->geom.y + cc->geom.h / 2);
}


void
client_delete(struct client_ctx *cc, int destroy)
{
	struct screen_ctx	*sc = cc->sc;
	struct winname		*wn;

	if (destroy) {
		XGrabServer(X_Dpy);
		cc->state = WithdrawnState;
		xu_set_wm_state(cc->win, cc->state);
		XRemoveFromSaveSet(X_Dpy, cc->win);
		XSync(X_Dpy, False);
		XUngrabServer(X_Dpy);
	}

	TAILQ_REMOVE(&sc->mruq, cc, mru_entry);
	TAILQ_REMOVE(&Clientq, cc, entry);

	xu_ewmh_net_client_list(sc);

	if (cc->group != NULL)
		TAILQ_REMOVE(&cc->group->clients, cc, group_entry);

	if (cc == client_current())
		client_none(sc);

	if (cc->app_name != NULL)
		XFree(cc->app_name);
	if (cc->app_class != NULL)
		XFree(cc->app_class);

	while ((wn = TAILQ_FIRST(&cc->nameq)) != NULL) {
		TAILQ_REMOVE(&cc->nameq, wn, entry);
		free(wn->name);
		free(wn);
	}

	free(cc);
}

void
client_leave(struct client_ctx *cc)
{
	if (cc == NULL)
		cc = client_current();
	if (cc == NULL)
		return;
}

void
client_setactive(struct client_ctx *cc, int fg)
{
	struct screen_ctx	*sc;

	if (cc == NULL)
		cc = client_current();
	if (cc == NULL)
		return;

	sc = cc->sc;

	if (fg) {
		XInstallColormap(X_Dpy, cc->colormap);
		if (cc->flags & CLIENT_INPUT) {
			XSetInputFocus(X_Dpy, cc->win,
			    RevertToPointerRoot, CurrentTime);
		}
		if (cc->xproto & _WM_TAKE_FOCUS)
			client_msg(cc, cwmh[WM_TAKE_FOCUS]);
		conf_grab_mouse(cc->win);
		/*
		 * If we're in the middle of alt-tabbing, don't change
		 * the order please.
		 */
		if (!sc->cycling)
			client_mtf(cc);
	} else
		client_leave(cc);

	if (fg && cc != client_current()) {
		client_setactive(NULL, 0);
		_curcc = cc;
		xu_ewmh_net_active_window(sc, cc->win);
	}

	cc->active = fg;
	client_draw_border(cc);
}

/*
 * set when there is no active client
 */
static void
client_none(struct screen_ctx *sc)
{
	Window none = None;

	xu_ewmh_net_active_window(sc, none);

	_curcc = NULL;
}

struct client_ctx *
client_current(void)
{
	return (_curcc);
}

void
client_freeze(struct client_ctx *cc)
{
	if (cc->flags & CLIENT_FREEZE)
		cc->flags &= ~CLIENT_FREEZE;
	else
		cc->flags |= CLIENT_FREEZE;
}

void
client_maximize(struct client_ctx *cc)
{
	struct screen_ctx	*sc = cc->sc;
	struct geom		 xine;

	if (cc->flags & CLIENT_FREEZE)
		return;

	if ((cc->flags & CLIENT_MAXFLAGS) == CLIENT_MAXIMIZED) {
		cc->flags &= ~CLIENT_MAXIMIZED;
		cc->geom = cc->savegeom;
		cc->bwidth = Conf.bwidth;
		goto resize;
	}

	if ((cc->flags & CLIENT_VMAXIMIZED) == 0) {
		cc->savegeom.h = cc->geom.h;
		cc->savegeom.y = cc->geom.y;
	}

	if ((cc->flags & CLIENT_HMAXIMIZED) == 0) {
		cc->savegeom.w = cc->geom.w;
		cc->savegeom.x = cc->geom.x;
	}

	/*
	 * pick screen that the middle of the window is on.
	 * that's probably more fair than if just the origin of
	 * a window is poking over a boundary
	 */
	xine = screen_find_xinerama(sc,
	    cc->geom.x + cc->geom.w / 2,
	    cc->geom.y + cc->geom.h / 2);

	cc->geom = xine;
	cc->bwidth = 0;
	cc->flags |= CLIENT_MAXIMIZED;

resize:
	client_resize(cc, 0);
	xu_ewmh_set_net_wm_state(cc);
}

void
client_expand(struct client_ctx *cc)
{
	struct geom		 new_geom;
	struct screen_ctx	*sc;
	struct client_ctx	*ci;
	struct group_ctx	*gc = cc->group;
	XineramaScreenInfo	*info = NULL;
	int			 x_offset, y_offset;
	u_int			 has_xinerama;

	sc = cc->sc;
	x_offset = y_offset = 0;
	has_xinerama = (sc->xinerama != NULL);

	if (cc->flags & CLIENT_FREEZE)
		return;

	if (!(cc->flags & CLIENT_EXPANDED))
		memcpy(&cc->savegeom, &cc->geom, sizeof(cc->geom));

	if (cc->flags & CLIENT_EXPANDED) {
		cc->flags &= ~CLIENT_EXPANDED;
		cc->geom = cc->savegeom;
		cc->bwidth = Conf.bwidth;

		client_resize(cc, 0);

		return;
	}

	if (has_xinerama) {
		info = screen_find_ptr_xinerama();
		x_offset = info->x_org;
		y_offset = info->y_org;
		fprintf(stderr, "has_xinerama: x_offset: %d, y_offset: %d\n",
			x_offset, y_offset);
	}

	memcpy(&new_geom, &cc->geom, sizeof(new_geom));

	/* Go through all clients and move up. */
	TAILQ_FOREACH(ci, &gc->clients, group_entry) {
		if (has_xinerama && cc->xinerama != info)
			continue;
		if (ci->geom.y + ci->geom.h <= cc->geom.y &&
		    OVERLAPS(cc->geom.x, cc->geom.w, ci->geom.x, ci->geom.w))
			y_offset = MAX(y_offset, ci->geom.y + ci->geom.h);
	}
	new_geom.h += (cc->geom.y - y_offset) - (cc->bwidth * 2);
	new_geom.y = y_offset + (cc->bwidth * 2);

	fprintf(stderr, "up:  y_offset: %d, geom: %dx%d, %dx%d\n",
		y_offset, new_geom.x, new_geom.y, new_geom.w, new_geom.h);

	/* Go through all clients and move down. */
	y_offset = has_xinerama ? (sc->xinerama->y_org + sc->work.h) : 0;
	fprintf(stderr, "down: y_offset: %d\n", y_offset);
	TAILQ_FOREACH(ci, &gc->clients, group_entry) {
		if (has_xinerama && cc->xinerama != info)
			continue;
		if (ci->geom.y >= cc->geom.y + cc->geom.h &&
		    OVERLAPS(cc->geom.x, cc->geom.w, ci->geom.x, ci->geom.w)) {
			fprintf(stderr, "Comparing: y_offset: %d, geom.y: %d\n",
				y_offset, ci->geom.y);
			y_offset = MIN(y_offset, ci->geom.y);
		}
	}
	new_geom.h = (y_offset - cc->geom.y) - (cc->bwidth * 2);
	fprintf(stderr, "down:  y_offset: %d, geom: %dx%d, %dx%d\n",
		y_offset, new_geom.x, new_geom.y, new_geom.w, new_geom.h);

	/* Go through all clients and move left. */
	fprintf(stderr, "left: x_offset: %d\n", x_offset);
	TAILQ_FOREACH(ci, &gc->clients, group_entry) {
		if (has_xinerama && cc->xinerama != info)
			continue;
		if (ci->geom.x + ci->geom.w <= cc->geom.x &&
		    OVERLAPS(cc->geom.y, cc->geom.h, ci->geom.y, ci->geom.h))
			x_offset = MAX(x_offset, ci->geom.x + ci->geom.w);
	}
	new_geom.w += (cc->geom.x - x_offset) - (cc->bwidth * 2);
	new_geom.x = x_offset + (cc->bwidth);
	fprintf(stderr, "left: x_offset: %d, geom: %dx%d, %dx%d\n",
		x_offset, new_geom.x, new_geom.y, new_geom.w, new_geom.h);

	/* Go through all clients and move right, */
	x_offset = has_xinerama ? (sc->xinerama->x_org + sc->work.w) : 0;
	fprintf(stderr, "right: x_offset: %d\n", x_offset);
	TAILQ_FOREACH(ci, &gc->clients, group_entry) {
		if (has_xinerama && cc->xinerama != info)
			continue;
		if (ci->geom.x >= cc->geom.x + cc->geom.w &&
		    OVERLAPS(cc->geom.y, cc->geom.h, ci->geom.y, ci->geom.h))
			x_offset = MIN(x_offset, ci->geom.x);
	}
	new_geom.w = (x_offset - cc->geom.x) - (cc->bwidth * 2);
	fprintf(stderr, "right: x_offset: %d, geom: %dx%d, %dx%d\n",
		x_offset, new_geom.x, new_geom.y, new_geom.w, new_geom.h);

	cc->flags |= CLIENT_EXPANDED;
	cc->geom = new_geom;
	client_resize(cc, 0);
}

void
client_vmaximize(struct client_ctx *cc)
{
	struct screen_ctx	*sc = cc->sc;
	struct geom		 xine;

	if (cc->flags & CLIENT_FREEZE)
		return;

	if (cc->flags & CLIENT_VMAXIMIZED) {
		cc->geom.y = cc->savegeom.y;
		cc->geom.h = cc->savegeom.h;
		cc->bwidth = Conf.bwidth;
		if (cc->flags & CLIENT_HMAXIMIZED)
			cc->geom.w -= cc->bwidth * 2;
		cc->flags &= ~CLIENT_VMAXIMIZED;
		goto resize;
	}

	cc->savegeom.y = cc->geom.y;
	cc->savegeom.h = cc->geom.h;

	/* if this will make us fully maximized then remove boundary */
	if ((cc->flags & CLIENT_MAXFLAGS) == CLIENT_HMAXIMIZED) {
		cc->geom.w += cc->bwidth * 2;
		cc->bwidth = 0;
	}

	xine = screen_find_xinerama(sc,
	    cc->geom.x + cc->geom.w / 2,
	    cc->geom.y + cc->geom.h / 2);

	cc->geom.y = xine.y;
	cc->geom.h = xine.h - (cc->bwidth * 2);
	cc->flags |= CLIENT_VMAXIMIZED;

resize:
	client_resize(cc, 0);
	xu_ewmh_set_net_wm_state(cc);
}

void
client_hmaximize(struct client_ctx *cc)
{
	struct screen_ctx	*sc = cc->sc;
	struct geom		 xine;

	if (cc->flags & CLIENT_FREEZE)
		return;

	if (cc->flags & CLIENT_HMAXIMIZED) {
		cc->geom.x = cc->savegeom.x;
		cc->geom.w = cc->savegeom.w;
		cc->bwidth = Conf.bwidth;
		if (cc->flags & CLIENT_VMAXIMIZED)
			cc->geom.h -= cc->bwidth * 2;
		cc->flags &= ~CLIENT_HMAXIMIZED;
		goto resize;
	}

	cc->savegeom.x = cc->geom.x;
	cc->savegeom.w = cc->geom.w;

	/* if this will make us fully maximized then remove boundary */
	if ((cc->flags & CLIENT_MAXFLAGS) == CLIENT_VMAXIMIZED) {
		cc->geom.h += cc->bwidth * 2;
		cc->bwidth = 0;
	}

	xine = screen_find_xinerama(sc,
	    cc->geom.x + cc->geom.w / 2,
	    cc->geom.y + cc->geom.h / 2);

	cc->geom.x = xine.x;
	cc->geom.w = xine.w - (cc->bwidth * 2);
	cc->flags |= CLIENT_HMAXIMIZED;

resize:
	client_resize(cc, 0);
	xu_ewmh_set_net_wm_state(cc);
}

void
client_resize(struct client_ctx *cc, int reset)
{
	if (reset) {
		cc->flags &= ~CLIENT_MAXIMIZED;
		cc->bwidth = Conf.bwidth;
		xu_ewmh_set_net_wm_state(cc);
	}

	XMoveResizeWindow(X_Dpy, cc->win, cc->geom.x,
	    cc->geom.y, cc->geom.w, cc->geom.h);
	client_config(cc);
}

void
client_move(struct client_ctx *cc)
{
	XMoveWindow(X_Dpy, cc->win, cc->geom.x, cc->geom.y);
	client_config(cc);
}

void
client_lower(struct client_ctx *cc)
{
	XLowerWindow(X_Dpy, cc->win);
}

void
client_raise(struct client_ctx *cc)
{
	XRaiseWindow(X_Dpy, cc->win);
}

void
client_config(struct client_ctx *cc)
{
	XConfigureEvent	 cn;

	bzero(&cn, sizeof(cn));
	cn.type = ConfigureNotify;
	cn.event = cc->win;
	cn.window = cc->win;
	cn.x = cc->geom.x;
	cn.y = cc->geom.y;
	cn.width = cc->geom.w;
	cn.height = cc->geom.h;
	cn.border_width = cc->bwidth;
	cn.above = None;
	cn.override_redirect = 0;

	XSendEvent(X_Dpy, cc->win, False, StructureNotifyMask, (XEvent *)&cn);
	client_update_xinerama(cc);
}

void
client_ptrwarp(struct client_ctx *cc)
{
	int	 x = cc->ptr.x, y = cc->ptr.y;

	if (x == -1 || y == -1) {
		x = cc->geom.w / 2;
		y = cc->geom.h / 2;
	}

	(cc->state == IconicState) ? client_unhide(cc) : client_raise(cc);
	xu_ptr_setpos(cc->win, x, y);
}

void
client_ptrsave(struct client_ctx *cc)
{
	int	 x, y;

	xu_ptr_getpos(cc->win, &x, &y);
	if (client_inbound(cc, x, y)) {
		cc->ptr.x = x;
		cc->ptr.y = y;
	} else {
		cc->ptr.x = -1;
		cc->ptr.y = -1;
	}
}

void
client_hide(struct client_ctx *cc)
{
	XUnmapWindow(X_Dpy, cc->win);

	cc->active = 0;
	cc->flags |= CLIENT_HIDDEN;
	cc->state = IconicState;
	xu_set_wm_state(cc->win, cc->state);

	if (cc == client_current())
		client_none(cc->sc);
}

void
client_unhide(struct client_ctx *cc)
{
	XMapRaised(X_Dpy, cc->win);

	cc->flags &= ~CLIENT_HIDDEN;
	cc->state = NormalState;
	xu_set_wm_state(cc->win, cc->state);
}

void
client_draw_border(struct client_ctx *cc)
{
	struct screen_ctx	*sc = cc->sc;
	unsigned long		 pixel;

	if (cc->active)
		switch (cc->flags & CLIENT_HIGHLIGHT) {
		case CLIENT_GROUP:
			pixel = sc->xftcolor[CWM_COLOR_BORDER_GROUP].pixel;
			break;
		case CLIENT_UNGROUP:
			pixel = sc->xftcolor[CWM_COLOR_BORDER_UNGROUP].pixel;
			break;
		default:
			pixel = sc->xftcolor[CWM_COLOR_BORDER_ACTIVE].pixel;
			break;
		}
	else
		pixel = sc->xftcolor[CWM_COLOR_BORDER_INACTIVE].pixel;

	XSetWindowBorderWidth(X_Dpy, cc->win, cc->bwidth);
	XSetWindowBorder(X_Dpy, cc->win, pixel);
}

static void
client_wm_protocols(struct client_ctx *cc)
{
	Atom	*p;
	int	 i, j;

	if (XGetWMProtocols(X_Dpy, cc->win, &p, &j)) {
		for (i = 0; i < j; i++) {
			if (p[i] == cwmh[WM_DELETE_WINDOW])
				cc->xproto |= _WM_DELETE_WINDOW;
			else if (p[i] == cwmh[WM_TAKE_FOCUS])
				cc->xproto |= _WM_TAKE_FOCUS;
		}
		XFree(p);
	}
}

void
client_msg(struct client_ctx *cc, Atom proto)
{
	XClientMessageEvent	 cm;

	bzero(&cm, sizeof(cm));
	cm.type = ClientMessage;
	cm.window = cc->win;
	cm.message_type = cwmh[WM_PROTOCOLS];
	cm.format = 32;
	cm.data.l[0] = proto;
	cm.data.l[1] = CurrentTime;

	XSendEvent(X_Dpy, cc->win, False, NoEventMask, (XEvent *)&cm);
}

void
client_send_delete(struct client_ctx *cc)
{
	if (cc->xproto & _WM_DELETE_WINDOW)
		client_msg(cc, cwmh[WM_DELETE_WINDOW]);
	else
		XKillClient(X_Dpy, cc->win);
}

void
client_setname(struct client_ctx *cc)
{
	struct winname	*wn;
	char		*newname;

	if (!xu_getstrprop(cc->win, ewmh[_NET_WM_NAME], &newname))
		if (!xu_getstrprop(cc->win, XA_WM_NAME, &newname))
			newname = xstrdup("");

	TAILQ_FOREACH(wn, &cc->nameq, entry)
		if (strcmp(wn->name, newname) == 0) {
			/* Move to the last since we got a hit. */
			TAILQ_REMOVE(&cc->nameq, wn, entry);
			TAILQ_INSERT_TAIL(&cc->nameq, wn, entry);
			goto match;
		}

	wn = xmalloc(sizeof(*wn));
	wn->name = newname;
	TAILQ_INSERT_TAIL(&cc->nameq, wn, entry);
	cc->nameqlen++;

match:
	cc->name = wn->name;

	/* Now, do some garbage collection. */
	if (cc->nameqlen > CLIENT_MAXNAMEQLEN) {
		wn = TAILQ_FIRST(&cc->nameq);
		assert(wn != NULL);
		TAILQ_REMOVE(&cc->nameq, wn, entry);
		free(wn->name);
		free(wn);
		cc->nameqlen--;
	}
}

void
client_cycle(struct screen_ctx *sc, int flags)
{
	struct client_ctx	*oldcc, *newcc;
	int			 again = 1;

	oldcc = client_current();

	/* If no windows then you cant cycle */
	if (TAILQ_EMPTY(&sc->mruq))
		return;

	if (oldcc == NULL)
		oldcc = (flags & CWM_RCYCLE ?
		    TAILQ_LAST(&sc->mruq, cycle_entry_q) :
		    TAILQ_FIRST(&sc->mruq));

	newcc = oldcc;
	while (again) {
		again = 0;

		newcc = (flags & CWM_RCYCLE ? client_mruprev(newcc) :
		    client_mrunext(newcc));

		/* Only cycle visible and non-ignored windows. */
		if ((newcc->flags & (CLIENT_HIDDEN|CLIENT_IGNORE))
			|| ((flags & CWM_INGROUP) && (newcc->group != oldcc->group)))
			again = 1;

		/* Is oldcc the only non-hidden window? */
		if (newcc == oldcc) {
			if (again)
				return;	/* No windows visible. */

			break;
		}
	}

	/* reset when cycling mod is released. XXX I hate this hack */
	sc->cycling = 1;
	client_ptrsave(oldcc);
	client_ptrwarp(newcc);
}

void
client_cycle_leave(struct screen_ctx *sc, struct client_ctx *cc)
{
	sc->cycling = 0;

	client_mtf(NULL);
	if (cc) {
		group_sticky_toggle_exit(cc);
		XUngrabKeyboard(X_Dpy, CurrentTime);
	}
}

static struct client_ctx *
client_mrunext(struct client_ctx *cc)
{
	struct screen_ctx	*sc = cc->sc;
	struct client_ctx	*ccc;

	return ((ccc = TAILQ_NEXT(cc, mru_entry)) != NULL ?
	    ccc : TAILQ_FIRST(&sc->mruq));
}

static struct client_ctx *
client_mruprev(struct client_ctx *cc)
{
	struct screen_ctx	*sc = cc->sc;
	struct client_ctx	*ccc;

	return ((ccc = TAILQ_PREV(cc, cycle_entry_q, mru_entry)) != NULL ?
	    ccc : TAILQ_LAST(&sc->mruq, cycle_entry_q));
}

static void
client_placecalc(struct client_ctx *cc)
{
	struct screen_ctx	*sc = cc->sc;
	int			 xslack, yslack;

	if (cc->hint.flags & (USPosition|PPosition)) {
		/*
		 * Ignore XINERAMA screens, just make sure it's somewhere
		 * in the virtual desktop. else it stops people putting xterms
		 * at startup in the screen the mouse doesn't start in *sigh*.
		 * XRandR bits mean that {x,y}max shouldn't be outside what's
		 * currently there.
		 */
		xslack = sc->view.w - cc->geom.w - cc->bwidth * 2;
		yslack = sc->view.h - cc->geom.h - cc->bwidth * 2;
		cc->geom.x = MIN(cc->geom.x, xslack);
		cc->geom.y = MIN(cc->geom.y, yslack);
	} else {
		struct geom		 xine;
		int			 xmouse, ymouse;

		xu_ptr_getpos(sc->rootwin, &xmouse, &ymouse);
		xine = screen_find_xinerama(sc, xmouse, ymouse);
		xine.w += xine.x;
		xine.h += xine.y;
		xmouse = MAX(xmouse, xine.x) - cc->geom.w / 2;
		ymouse = MAX(ymouse, xine.y) - cc->geom.h / 2;

		xmouse = MAX(xmouse, xine.x);
		ymouse = MAX(ymouse, xine.y);

		xslack = xine.w - cc->geom.w - cc->bwidth * 2;
		yslack = xine.h - cc->geom.h - cc->bwidth * 2;

		if (xslack >= xine.x) {
			cc->geom.x = MAX(MIN(xmouse, xslack), xine.x);
		} else {
			cc->geom.x = xine.x;
			cc->geom.w = xine.w;
		}
		if (yslack >= xine.y) {
			cc->geom.y = MAX(MIN(ymouse, yslack), xine.y);
		} else {
			cc->geom.y = xine.y;
			cc->geom.h = xine.h;
		}
	}
}

static void
client_mtf(struct client_ctx *cc)
{
	struct screen_ctx	*sc;

	if (cc == NULL)
		cc = client_current();
	if (cc == NULL)
		return;

	sc = cc->sc;
	TAILQ_REMOVE(&sc->mruq, cc, mru_entry);
	TAILQ_INSERT_HEAD(&sc->mruq, cc, mru_entry);
}

void
client_getsizehints(struct client_ctx *cc)
{
	long		 tmp;
	XSizeHints	*size;

	if ((size = XAllocSizeHints()) == NULL)
		warnx("XAllocSizeHints failure");

	if (!XGetWMNormalHints(X_Dpy, cc->win, size, &tmp))
		size->flags = 0;

	cc->hint.flags = size->flags;

	if (size->flags & PBaseSize) {
		cc->hint.basew = size->base_width;
		cc->hint.baseh = size->base_height;
	} else if (size->flags & PMinSize) {
		cc->hint.basew = size->min_width;
		cc->hint.baseh = size->min_height;
	}
	if (size->flags & PMinSize) {
		cc->hint.minw = size->min_width;
		cc->hint.minh = size->min_height;
	} else if (size->flags & PBaseSize) {
		cc->hint.minw = size->base_width;
		cc->hint.minh = size->base_height;
	}
	if (size->flags & PMaxSize) {
		cc->hint.maxw = size->max_width;
		cc->hint.maxh = size->max_height;
	}
	if (size->flags & PResizeInc) {
		cc->hint.incw = size->width_inc;
		cc->hint.inch = size->height_inc;
	}
	cc->hint.incw = MAX(1, cc->hint.incw);
	cc->hint.inch = MAX(1, cc->hint.inch);

	if (size->flags & PAspect) {
		if (size->min_aspect.x > 0)
			cc->hint.mina = (float)size->min_aspect.y /
			    size->min_aspect.x;
		if (size->max_aspect.y > 0)
			cc->hint.maxa = (float)size->max_aspect.x /
			    size->max_aspect.y;
	}

	if (size)
		XFree(size);
}

void
client_applysizehints(struct client_ctx *cc)
{
	Bool		 baseismin;

	baseismin = (cc->hint.basew == cc->hint.minw) &&
	    (cc->hint.baseh == cc->hint.minh);

	/* temporarily remove base dimensions, ICCCM 4.1.2.3 */
	if (!baseismin) {
		cc->geom.w -= cc->hint.basew;
		cc->geom.h -= cc->hint.baseh;
	}

	/* adjust for aspect limits */
	if (cc->hint.mina > 0 && cc->hint.maxa > 0) {
		if (cc->hint.maxa <
		    (float)cc->geom.w / cc->geom.h)
			cc->geom.w = cc->geom.h * cc->hint.maxa;
		else if (cc->hint.mina <
		    (float)cc->geom.h / cc->geom.w)
			cc->geom.h = cc->geom.w * cc->hint.mina;
	}

	/* remove base dimensions for increment */
	if (baseismin) {
		cc->geom.w -= cc->hint.basew;
		cc->geom.h -= cc->hint.baseh;
	}

	/* adjust for increment value */
	cc->geom.w -= cc->geom.w % cc->hint.incw;
	cc->geom.h -= cc->geom.h % cc->hint.inch;

	/* restore base dimensions */
	cc->geom.w += cc->hint.basew;
	cc->geom.h += cc->hint.baseh;

	/* adjust for min width/height */
	cc->geom.w = MAX(cc->geom.w, cc->hint.minw);
	cc->geom.h = MAX(cc->geom.h, cc->hint.minh);

	/* adjust for max width/height */
	if (cc->hint.maxw)
		cc->geom.w = MIN(cc->geom.w, cc->hint.maxw);
	if (cc->hint.maxh)
		cc->geom.h = MIN(cc->geom.h, cc->hint.maxh);
}

static void
client_getmwmhints(struct client_ctx *cc)
{
	struct mwm_hints	*mwmh;

	if (xu_getprop(cc->win, cwmh[_MOTIF_WM_HINTS], cwmh[_MOTIF_WM_HINTS],
	    PROP_MWM_HINTS_ELEMENTS, (unsigned char **)&mwmh) == MWM_NUMHINTS)
		if (mwmh->flags & MWM_HINTS_DECORATIONS &&
		    !(mwmh->decorations & MWM_DECOR_ALL) &&
		    !(mwmh->decorations & MWM_DECOR_BORDER))
			cc->bwidth = 0;
}

void
client_transient(struct client_ctx *cc)
{
	struct client_ctx	*tc;
	Window			 trans;

	if (XGetTransientForHint(X_Dpy, cc->win, &trans)) {
		if ((tc = client_find(trans)) && tc->group) {
			group_movetogroup(cc, tc->group->shortcut);
			if (tc->flags & CLIENT_IGNORE)
				cc->flags |= CLIENT_IGNORE;
		}
	}
}

static int
client_inbound(struct client_ctx *cc, int x, int y)
{
	return (x < cc->geom.w && x >= 0 &&
	    y < cc->geom.h && y >= 0);
}

int
client_snapcalc(int n0, int n1, int e0, int e1, int snapdist)
{
	int	 s0, s1;

	s0 = s1 = 0;

	if (abs(e0 - n0) <= snapdist)
		s0 = e0 - n0;

	if (abs(e1 - n1) <= snapdist)
		s1 = e1 - n1;

	/* possible to snap in both directions */
	if (s0 != 0 && s1 != 0)
		if (abs(s0) < abs(s1))
			return (s0);
		else
			return (s1);
	else if (s0 != 0)
		return (s0);
	else if (s1 != 0)
		return (s1);
	else
		return (0);
}

void
client_htile(struct client_ctx *cc)
{
	struct client_ctx	*ci;
	struct group_ctx 	*gc = cc->group;
	struct screen_ctx 	*sc = cc->sc;
	struct geom 		 xine;
	int 			 i, n, mh, x, h, w;

	if (!gc)
		return;
	i = n = 0;

	TAILQ_FOREACH(ci, &gc->clients, group_entry) {
		if (ci->flags & CLIENT_HIDDEN ||
		    ci->flags & CLIENT_IGNORE || (ci == cc))
			continue;
		n++;
	}
	if (n == 0)
		return;

	xine = screen_find_xinerama(sc,
	    cc->geom.x + cc->geom.w / 2,
	    cc->geom.y + cc->geom.h / 2);

	if (cc->flags & CLIENT_VMAXIMIZED ||
	    cc->geom.h + (cc->bwidth * 2) >= xine.h)
		return;

	cc->flags &= ~CLIENT_HMAXIMIZED;
	cc->geom.x = xine.x;
	cc->geom.y = xine.y;
	cc->geom.w = xine.w - (cc->bwidth * 2);
	client_resize(cc, 1);
	client_ptrwarp(cc);

	mh = cc->geom.h + (cc->bwidth * 2);
	x = xine.x;
	w = xine.w / n;
	h = xine.h - mh;
	TAILQ_FOREACH(ci, &gc->clients, group_entry) {
		if (ci->flags & CLIENT_HIDDEN ||
		    ci->flags & CLIENT_IGNORE || (ci == cc))
			continue;
		ci->bwidth = Conf.bwidth;
		ci->geom.y = xine.y + mh;
		ci->geom.x = x;
		ci->geom.h = h - (ci->bwidth * 2);
		ci->geom.w = w - (ci->bwidth * 2);
		if (i + 1 == n)
			ci->geom.w = xine.x + xine.w -
			    ci->geom.x - (ci->bwidth * 2);
		x += w;
		client_resize(ci, 1);
		i++;
	}
}

void
client_vtile(struct client_ctx *cc)
{
	struct client_ctx	*ci;
	struct group_ctx 	*gc = cc->group;
	struct screen_ctx 	*sc = cc->sc;
	struct geom 		 xine;
	int 			 i, n, mw, y, h, w;

	if (!gc)
		return;
	i = n = 0;

	TAILQ_FOREACH(ci, &gc->clients, group_entry) {
		if (ci->flags & CLIENT_HIDDEN ||
		    ci->flags & CLIENT_IGNORE || (ci == cc))
			continue;
		n++;
	}
	if (n == 0)
		return;

	xine = screen_find_xinerama(sc,
	    cc->geom.x + cc->geom.w / 2,
	    cc->geom.y + cc->geom.h / 2);

	if (cc->flags & CLIENT_HMAXIMIZED ||
	    cc->geom.w + (cc->bwidth * 2) >= xine.w)
		return;

	cc->flags &= ~CLIENT_VMAXIMIZED;
	cc->geom.x = xine.x;
	cc->geom.y = xine.y;
	cc->geom.h = xine.h - (cc->bwidth * 2);
	client_resize(cc, 1);
	client_ptrwarp(cc);

	mw = cc->geom.w + (cc->bwidth * 2);
	y = xine.y;
	h = xine.h / n;
	w = xine.w - mw;
	TAILQ_FOREACH(ci, &gc->clients, group_entry) {
		if (ci->flags & CLIENT_HIDDEN ||
		    ci->flags & CLIENT_IGNORE || (ci == cc))
			continue;
		ci->bwidth = Conf.bwidth;
		ci->geom.y = y;
		ci->geom.x = xine.x + mw;
		ci->geom.h = h - (ci->bwidth * 2);
		ci->geom.w = w - (ci->bwidth * 2);
		if (i + 1 == n)
			ci->geom.h = xine.y + xine.h -
			    ci->geom.y - (ci->bwidth * 2);
		y += h;
		client_resize(ci, 1);
		i++;
	}
}
