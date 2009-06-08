/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Adapted from soup-cookie-jar.c
 *
 * Copyright (C) 2008 Red Hat, Inc.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <string.h>

#include <libsoup/soup-cookie.h>
#include "cookie-handler.h"
#include <libsoup/soup-date.h>
//#include <libsoup/soup-marshal.h>
#include <libsoup/soup-message.h>
#include <libsoup/soup-session-feature.h>
#include <libsoup/soup-uri.h>

/**
 * SECTION:soup-cookie-handler
 * @short_description: Automatic cookie handling for #SoupSession
 *
 * A #CookieHandler stores #SoupCookie<!-- -->s and arrange for them
 * to be sent with the appropriate #SoupMessage<!-- -->s.
 * #CookieHandler implements #SoupSessionFeature, so you can add a
 * cookie handler to a session with soup_session_add_feature() or
 * soup_session_add_feature_by_type().
 *
 * Note that the base #CookieHandler class does not support any form
 * of long-term cookie persistence.
 **/

static void cookie_handler_session_feature_init (SoupSessionFeatureInterface *feature_interface, gpointer interface_data);
static void request_queued (SoupSessionFeature *feature, SoupSession *session,
			    SoupMessage *msg);
static void request_started (SoupSessionFeature *feature, SoupSession *session,
			     SoupMessage *msg, SoupSocket *socket);
static void request_unqueued (SoupSessionFeature *feature, SoupSession *session,
			      SoupMessage *msg);

G_DEFINE_TYPE_WITH_CODE (CookieHandler, cookie_handler, G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (SOUP_TYPE_SESSION_FEATURE,
						cookie_handler_session_feature_init))

enum {
	CHANGED,
	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

enum {
	PROP_0,

	PROP_HANDLER,

	LAST_PROP
};

typedef struct {
	gboolean constructed;
        char *handler;
	GHashTable *domains;
} CookieHandlerPrivate;
#define COOKIE_HANDLER_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), SOUP_TYPE_COOKIE_HANDLER, CookieHandlerPrivate))

static void set_property (GObject *object, guint prop_id,
			  const GValue *value, GParamSpec *pspec);
static void get_property (GObject *object, guint prop_id,
			  GValue *value, GParamSpec *pspec);

static void
cookie_handler_init (CookieHandler *handler)
{
	CookieHandlerPrivate *priv = COOKIE_HANDLER_GET_PRIVATE (handler);

	priv->domains = g_hash_table_new_full (g_str_hash, g_str_equal,
					       g_free, NULL);
}

static void
constructed (GObject *object)
{
	CookieHandlerPrivate *priv = COOKIE_HANDLER_GET_PRIVATE (object);

	priv->constructed = TRUE;
}

static void
finalize (GObject *object)
{
	CookieHandlerPrivate *priv = COOKIE_HANDLER_GET_PRIVATE (object);
	GHashTableIter iter;
	gpointer key, value;

	g_hash_table_iter_init (&iter, priv->domains);
	while (g_hash_table_iter_next (&iter, &key, &value))
		soup_cookies_free (value);
	g_hash_table_destroy (priv->domains);

	G_OBJECT_CLASS (cookie_handler_parent_class)->finalize (object);
}

static void
cookie_handler_class_init (CookieHandlerClass *handler_class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (handler_class);

	g_type_class_add_private (handler_class, sizeof (CookieHandlerPrivate));

	object_class->constructed = constructed;
	object_class->finalize = finalize;
	object_class->set_property = set_property;
	object_class->get_property = get_property;

	/**
	 * CookieHandler::changed
	 * @handler: the #CookieHandler
	 * @old_cookie: the old #SoupCookie value
	 * @new_cookie: the new #SoupCookie value
	 *
	 * Emitted when @handler changes. If a cookie has been added,
	 * @new_cookie will contain the newly-added cookie and
	 * @old_cookie will be %NULL. If a cookie has been deleted,
	 * @old_cookie will contain the to-be-deleted cookie and
	 * @new_cookie will be %NULL. If a cookie has been changed,
	 * @old_cookie will contain its old value, and @new_cookie its
	 * new value.
	 **/
	signals[CHANGED] =
		g_signal_new ("changed",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_FIRST,
			      G_STRUCT_OFFSET (CookieHandlerClass, changed),
			      NULL, NULL,
                              NULL,
			      G_TYPE_NONE, 2, 
			      SOUP_TYPE_COOKIE | G_SIGNAL_TYPE_STATIC_SCOPE,
			      SOUP_TYPE_COOKIE | G_SIGNAL_TYPE_STATIC_SCOPE);

	/**
	 * COOKIE_HANDLER_HANDLER:
	 *
	 * Alias for the #CookieHandler:read-only property. (Whether
	 * or not the cookie handler is read-only.)
	 **/
	g_object_class_install_property (
		object_class, PROP_HANDLER,
		g_param_spec_string (COOKIE_HANDLER_HANDLER,
				      "Handler",
				      "The programme to handle cookies",
				      NULL,
				      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
}

static void
cookie_handler_session_feature_init (SoupSessionFeatureInterface *feature_interface,
				      gpointer interface_data)
{
	feature_interface->request_queued = request_queued;
	feature_interface->request_started = request_started;
	feature_interface->request_unqueued = request_unqueued;
}

static void
set_property (GObject *object, guint prop_id,
	      const GValue *value, GParamSpec *pspec)
{
	CookieHandlerPrivate *priv =
		COOKIE_HANDLER_GET_PRIVATE (object);

	switch (prop_id) {
	case PROP_HANDLER:
		priv->handler = g_value_dup_string (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
get_property (GObject *object, guint prop_id,
	      GValue *value, GParamSpec *pspec)
{
	CookieHandlerPrivate *priv =
		COOKIE_HANDLER_GET_PRIVATE (object);

	switch (prop_id) {
	case PROP_HANDLER:
		g_value_set_string (value, priv->handler);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

/**
 * cookie_handler_new:
 *
 * Creates a new #CookieHandler. The base #CookieHandler class does
 * not support persistent storage of cookies; use a subclass for that.
 *
 * Returns: a new #CookieHandler
 *
 * Since: 2.24
 **/
CookieHandler *
cookie_handler_new (const char *handler)
{
	g_return_val_if_fail (handler != NULL, NULL);

	return g_object_new (SOUP_TYPE_COOKIE_HANDLER,
			     COOKIE_HANDLER_HANDLER, handler,
			     NULL);
}

static void
cookie_handler_changed (CookieHandler *handler,
			 SoupCookie *old, SoupCookie *new)
{
	CookieHandlerPrivate *priv = COOKIE_HANDLER_GET_PRIVATE (handler);

	if (!priv->handler || !priv->constructed)
		return;

	g_signal_emit (handler, signals[CHANGED], 0, old, new);
}

/**
 * cookie_handler_get_cookies:
 * @handler: a #CookieHandler
 * @uri: a #SoupURI
 * @for_http: whether or not the return value is being passed directly
 * to an HTTP operation
 *
 * Retrieves (in Cookie-header form) the list of cookies that would
 * be sent with a request to @uri.
 *
 * If @for_http is %TRUE, the return value will include cookies marked
 * "HttpOnly" (that is, cookies that the server wishes to keep hidden
 * from client-side scripting operations such as the JavaScript
 * document.cookies property). Since #CookieHandler sets the Cookie
 * header itself when making the actual HTTP request, you should
 * almost certainly be setting @for_http to %FALSE if you are calling
 * this.
 *
 * Return value: the cookies, in string form, or %NULL if there are no
 * cookies for @uri.
 *
 * Since: 2.24
 **/
char *
cookie_handler_get_cookies (CookieHandler *handler, SoupURI *uri,
			     gboolean for_http)
{
	CookieHandlerPrivate *priv;
	GSList *cookies, *domain_cookies;
	char *domain, *cur, *next_domain, *result;
	GSList *new_head, *cookies_to_remove = NULL, *p;

	g_return_val_if_fail (SOUP_IS_COOKIE_HANDLER (handler), NULL);
	priv = COOKIE_HANDLER_GET_PRIVATE (handler);

	/* The logic here is a little weird, but the plan is that if
	 * uri->host is "www.foo.com", we will end up looking up
	 * cookies for ".www.foo.com", "www.foo.com", ".foo.com", and
	 * ".com", in that order. (Logic stolen from Mozilla.)
	 */
	cookies = NULL;
	domain = cur = g_strdup_printf (".%s", uri->host);
	next_domain = domain + 1;
	do {
		new_head = domain_cookies = g_hash_table_lookup (priv->domains, cur);
		while (domain_cookies) {
			GSList *next = domain_cookies->next;
			SoupCookie *cookie = domain_cookies->data;

			if (cookie->expires && soup_date_is_past (cookie->expires)) {
				cookies_to_remove = g_slist_append (cookies_to_remove,
								    cookie);
				new_head = g_slist_delete_link (new_head, domain_cookies);
				g_hash_table_insert (priv->domains,
						     g_strdup (cur),
						     new_head);
			} else if (soup_cookie_applies_to_uri (cookie, uri) &&
				   (for_http || !cookie->http_only))
				cookies = g_slist_append (cookies, cookie);

			domain_cookies = next;
		}
		cur = next_domain;
		if (cur)
			next_domain = strchr (cur + 1, '.');
	} while (cur);
	g_free (domain);

	for (p = cookies_to_remove; p; p = p->next) {
		SoupCookie *cookie = p->data;

		cookie_handler_changed (handler, cookie, NULL);
		soup_cookie_free (cookie);
	}
	g_slist_free (cookies_to_remove);

	if (cookies) {
		/* FIXME: sort? */
		result = soup_cookies_to_cookie_header (cookies);
		g_slist_free (cookies);
		return result;
	} else
		return NULL;
}

/**
 * cookie_handler_add_cookie:
 * @handler: a #CookieHandler
 * @cookie: a #SoupCookie
 *
 * Adds @cookie to @handler, emitting the 'changed' signal if we are modifying
 * an existing cookie or adding a valid new cookie ('valid' means
 * that the cookie's expire date is not in the past).
 *
 * @cookie will be 'stolen' by the handler, so don't free it afterwards.
 *
 * Since: 2.24
 **/
void
cookie_handler_add_cookie (CookieHandler *handler, SoupCookie *cookie)
{
	CookieHandlerPrivate *priv;
	GSList *old_cookies, *oc, *prev = NULL;
	SoupCookie *old_cookie;

	g_return_if_fail (SOUP_IS_COOKIE_HANDLER (handler));
	g_return_if_fail (cookie != NULL);

	priv = COOKIE_HANDLER_GET_PRIVATE (handler);
	old_cookies = g_hash_table_lookup (priv->domains, cookie->domain);
	for (oc = old_cookies; oc; oc = oc->next) {
		old_cookie = oc->data;
		if (!strcmp (cookie->name, old_cookie->name) &&
		    !g_strcmp0 (cookie->path, old_cookie->path)) {
			if (cookie->expires && soup_date_is_past (cookie->expires)) {
				/* The new cookie has an expired date,
				 * this is the way the the server has
				 * of telling us that we have to
				 * remove the cookie.
				 */
				old_cookies = g_slist_delete_link (old_cookies, oc);
				g_hash_table_insert (priv->domains,
						     g_strdup (cookie->domain),
						     old_cookies);
				cookie_handler_changed (handler, old_cookie, NULL);
				soup_cookie_free (old_cookie);
				soup_cookie_free (cookie);
			} else {
				oc->data = cookie;
				cookie_handler_changed (handler, old_cookie, cookie);
				soup_cookie_free (old_cookie);
			}

			return;
		}
		prev = oc;
	}

	/* The new cookie is... a new cookie */
	if (cookie->expires && soup_date_is_past (cookie->expires)) {
		soup_cookie_free (cookie);
		return;
	}

	if (prev)
		prev = g_slist_append (prev, cookie);
	else {
		old_cookies = g_slist_append (NULL, cookie);
		g_hash_table_insert (priv->domains, g_strdup (cookie->domain),
				     old_cookies);
	}

	cookie_handler_changed (handler, NULL, cookie);
}

/**
 * cookie_handler_set_cookie:
 * @handler: a #CookieHandler
 * @uri: the URI setting the cookie
 * @cookie: the stringified cookie to set
 *
 * Adds @cookie to @handler, exactly as though it had appeared in a
 * Set-Cookie header returned from a request to @uri.
 *
 * Since: 2.24
 **/
void
cookie_handler_set_cookie (CookieHandler *handler, SoupURI *uri,
			    const char *cookie)
{
	SoupCookie *soup_cookie;

	g_return_if_fail (SOUP_IS_COOKIE_HANDLER (handler));
	g_return_if_fail (cookie != NULL);

	soup_cookie = soup_cookie_parse (cookie, uri);
	if (soup_cookie) {
		/* will steal or free soup_cookie */
		cookie_handler_add_cookie (handler, soup_cookie);
	}
}

static void
process_set_cookie_header (SoupMessage *msg, gpointer user_data)
{
	CookieHandler *handler = user_data;
	GSList *new_cookies, *nc;

	new_cookies = soup_cookies_from_response (msg);
	for (nc = new_cookies; nc; nc = nc->next)
		cookie_handler_add_cookie (handler, nc->data);
	g_slist_free (new_cookies);
}

static void
request_queued (SoupSessionFeature *feature, SoupSession *session,
		SoupMessage *msg)
{
	soup_message_add_header_handler (msg, "got-headers",
					 "Set-Cookie",
					 G_CALLBACK (process_set_cookie_header),
					 feature);
}

static void
request_started (SoupSessionFeature *feature, SoupSession *session,
		 SoupMessage *msg, SoupSocket *socket)
{
	CookieHandler *handler = COOKIE_HANDLER (feature);
	char *cookies;

	cookies = cookie_handler_get_cookies (handler, soup_message_get_uri (msg), TRUE);
	if (cookies) {
		soup_message_headers_replace (msg->request_headers,
					      "Cookie", cookies);
		g_free (cookies);
	} else
		soup_message_headers_remove (msg->request_headers, "Cookie");
}

static void
request_unqueued (SoupSessionFeature *feature, SoupSession *session,
		  SoupMessage *msg)
{
	g_signal_handlers_disconnect_by_func (msg, process_set_cookie_header, feature);
}

/**
 * cookie_handler_all_cookies:
 * @handler: a #CookieHandler
 *
 * Constructs a #GSList with every cookie inside the @handler.
 * The cookies in the list are a copy of the original, so
 * you have to free them when you are done with them.
 *
 * Return value: a #GSList with all the cookies in the @handler.
 *
 * Since: 2.24
 **/
GSList *
cookie_handler_all_cookies (CookieHandler *handler)
{
	CookieHandlerPrivate *priv;
	GHashTableIter iter;
	GSList *l = NULL;
	gpointer key, value;

	g_return_val_if_fail (SOUP_IS_COOKIE_HANDLER (handler), NULL);

	priv = COOKIE_HANDLER_GET_PRIVATE (handler);

	g_hash_table_iter_init (&iter, priv->domains);

	while (g_hash_table_iter_next (&iter, &key, &value)) {
		GSList *p, *cookies = value;
		for (p = cookies; p; p = p->next)
			l = g_slist_prepend (l, soup_cookie_copy (p->data));
	}

	return l;
}

/**
 * cookie_handler_delete_cookie:
 * @handler: a #CookieHandler
 * @cookie: a #SoupCookie
 *
 * Deletes @cookie from @handler, emitting the 'changed' signal.
 *
 * Since: 2.24
 **/
void
cookie_handler_delete_cookie (CookieHandler *handler,
			       SoupCookie    *cookie)
{
	CookieHandlerPrivate *priv;
	GSList *cookies, *p;
	char *domain;

	g_return_if_fail (SOUP_IS_COOKIE_HANDLER (handler));
	g_return_if_fail (cookie != NULL);

	priv = COOKIE_HANDLER_GET_PRIVATE (handler);

	domain = g_strdup (cookie->domain);

	cookies = g_hash_table_lookup (priv->domains, domain);
	if (cookies == NULL)
		return;

	for (p = cookies; p; p = p->next ) {
		SoupCookie *c = (SoupCookie*)p->data;
		if (soup_cookie_equal (cookie, c)) {
			cookies = g_slist_delete_link (cookies, p);
			g_hash_table_insert (priv->domains,
					     domain,
					     cookies);
			cookie_handler_changed (handler, c, NULL);
			soup_cookie_free (c);
			return;
		}
	}
}
