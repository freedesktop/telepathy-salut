/*
 * gibber-bytestream-oob.c - Source for GibberBytestreamOOB
 * Copyright (C) 2007 Collabora Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "gibber-bytestream-oob.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>

#include <glib.h>

#include "gibber-bytestream-iface.h"
#include "gibber-xmpp-connection.h"
#include "gibber-xmpp-stanza.h"
#include "gibber-namespaces.h"
#include "gibber-linklocal-transport.h"
#include "gibber-xmpp-error.h"
#include "gibber-iq-helper.h"

#define DEBUG_FLAG DEBUG_BYTESTREAM
#include "gibber-debug.h"

#include "signals-marshal.h"

static void
bytestream_iface_init (gpointer g_iface, gpointer iface_data);

G_DEFINE_TYPE_WITH_CODE (GibberBytestreamOOB, gibber_bytestream_oob,
    G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (GIBBER_TYPE_BYTESTREAM_IFACE,
      bytestream_iface_init));

/* signals */
enum
{
  DATA_RECEIVED,
  STATE_CHANGED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = {0};

/* properties */
enum
{
  PROP_XMPP_CONNECTION = 1,
  PROP_SELF_ID,
  PROP_PEER_ID,
  PROP_STREAM_ID,
  PROP_STREAM_INIT_ID,
  PROP_STATE,
  PROP_HOST,
  PROP_PROTOCOL,
  LAST_PROPERTY
};

typedef struct _GibberBytestreamIBBPrivate GibberBytestreamOOBPrivate;
struct _GibberBytestreamIBBPrivate
{
  GibberXmppConnection *xmpp_connection;
  gchar *self_id;
  gchar *peer_id;
  gchar *stream_id;
  gchar *stream_init_id;
  /* ID of the OOB opening stanza. We'll reply to
   * it when we the bytestream is closed */
  gchar *stream_open_id;
  GibberBytestreamState state;
  gchar *host;

  GIOChannel *listener;
  guint listener_watch;
  GIOChannel *data_channel;
  guint data_watch;
  GString *buffer;

  gboolean dispose_has_run;
};

#define GIBBER_BYTESTREAM_OOB_GET_PRIVATE(obj) \
    ((GibberBytestreamOOBPrivate *) (GibberBytestreamOOB *)obj->priv)

static void
gibber_bytestream_oob_init (GibberBytestreamOOB *self)
{
  GibberBytestreamOOBPrivate *priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      GIBBER_TYPE_BYTESTREAM_OOB, GibberBytestreamOOBPrivate);

  self->priv = priv;
  priv->dispose_has_run = FALSE;
}

static GibberXmppStanza *
make_iq_oob_sucess_response (const gchar *from,
                             const gchar *to,
                             const gchar *id)
{
  return gibber_xmpp_stanza_build (
      GIBBER_STANZA_TYPE_IQ, GIBBER_STANZA_SUB_TYPE_RESULT,
      from, to,
      GIBBER_NODE_ATTRIBUTE, "id", id,
      GIBBER_STANZA_END);
}

static gboolean
data_io_in_cb (GIOChannel *source,
               GIOCondition condition,
               gpointer user_data)
{
#define BUFF_SIZE 4096
  GibberBytestreamOOB *self = GIBBER_BYTESTREAM_OOB (user_data);
  GibberBytestreamOOBPrivate *priv = GIBBER_BYTESTREAM_OOB_GET_PRIVATE (self);
  int fd;
  gchar buff[BUFF_SIZE];
  gsize readed;
  GIOStatus status;
  GError *error = NULL;
  gboolean have_to_wait_for_reading = FALSE;

  if (! (condition & G_IO_IN))
    return TRUE;

  fd = g_io_channel_unix_get_fd (source);

  do
    {
      memset (&buff, 0, BUFF_SIZE);
      status = g_io_channel_read_chars (source, buff, BUFF_SIZE, &readed,
          &error);

      switch (status)
        {
          case G_IO_STATUS_NORMAL:
            if (priv->buffer == NULL)
              priv->buffer = g_string_new_len (buff, readed);
            else
              g_string_append_len (priv->buffer, buff, readed);
            break;

          case G_IO_STATUS_AGAIN:
            /* We have to wait before be able to finish the reading */
            have_to_wait_for_reading = TRUE;
            break;

          case G_IO_STATUS_EOF:
            DEBUG ("error reading from socket: EOF");
            goto reading_error;
            break;

          default:
            DEBUG ("error reading from socket: %s",
                error ? error->message : "");
            goto reading_error;
        }

    } while (readed == BUFF_SIZE && !have_to_wait_for_reading);

  if (!have_to_wait_for_reading)
    {
      DEBUG ("read %d bytes from socket", priv->buffer->len);

      g_signal_emit (G_OBJECT (self), signals[DATA_RECEIVED], 0, priv->peer_id,
          priv->buffer);

      g_string_free (priv->buffer, TRUE);
      priv->buffer = NULL;
    }

  return TRUE;

reading_error:
  gibber_bytestream_iface_close (GIBBER_BYTESTREAM_IFACE (self), NULL);

  if (priv->buffer != NULL)
    {
      g_string_free (priv->buffer, TRUE);
      priv->buffer = NULL;
    }

  if (error != NULL)
    g_error_free (error);

  return FALSE;
}

static gboolean
connect_to_url (GibberBytestreamOOB *self,
                const gchar *url)
{
  GibberBytestreamOOBPrivate *priv = GIBBER_BYTESTREAM_OOB_GET_PRIVATE (self);
  gchar **tokens;
  const gchar *host, *port;
  int fd, ret;
  struct addrinfo req, *ans = NULL;

  if (!g_str_has_prefix (url, "x-tcp://"))
    {
      DEBUG ("URL is not a TCP url: %s", url);
      return FALSE;
    }

  url += strlen ("x-tcp://");
  tokens = g_strsplit (url, ":", 2);
  host = tokens[0];
  port = tokens[1];

  memset (&req, 0, sizeof(req));
  req.ai_flags = AI_PASSIVE;
  req.ai_family = AF_UNSPEC;
  req.ai_socktype = SOCK_STREAM;
  req.ai_protocol = IPPROTO_TCP;

  ret = getaddrinfo (host, port, &req, &ans);
  if (ret != 0)
    {
      DEBUG ("getaddrinfo failed: %s", gai_strerror (ret));
      return FALSE;
    }

  fd = socket (ans->ai_family, ans->ai_socktype, ans->ai_protocol);
  if (fd == -1)
    {
      DEBUG ("socket failed: %s", g_strerror (errno));
      freeaddrinfo (ans);
      return FALSE;
    }

  if (connect (fd, ans->ai_addr, ans->ai_addrlen) == -1)
    {
      DEBUG ("connect to %s:%s failed: %s", host, port, g_strerror (errno));
      freeaddrinfo (ans);
      return FALSE;
    }

  DEBUG ("connected to %s:%s", host, port);
  priv->data_channel = g_io_channel_unix_new (fd);
  g_io_channel_set_encoding (priv->data_channel, NULL, NULL);
  g_io_channel_set_buffered (priv->data_channel, FALSE);
  g_io_channel_set_close_on_unref (priv->data_channel, TRUE);

  priv->data_watch = g_io_add_watch (priv->data_channel, G_IO_IN,
      data_io_in_cb, self);

  g_strfreev (tokens);
  freeaddrinfo (ans);
  return TRUE;
}

static gboolean
parse_oob_init_iq (GibberBytestreamOOB *self,
                   GibberXmppStanza *stanza)
{
  GibberBytestreamOOBPrivate *priv = GIBBER_BYTESTREAM_OOB_GET_PRIVATE (self);
  GibberXmppNode *query_node, *url_node;
  GibberStanzaType type;
  GibberStanzaSubType sub_type;
  const gchar *stream_id, *url;

  gibber_xmpp_stanza_get_type_info (stanza, &type, &sub_type);

  if (type != GIBBER_STANZA_TYPE_IQ ||
      sub_type != GIBBER_STANZA_SUB_TYPE_SET)
    return FALSE;

  query_node = gibber_xmpp_node_get_child_ns (stanza->node, "query",
      GIBBER_XMPP_NS_OOB);
  if (query_node == NULL)
    return FALSE;

  stream_id = gibber_xmpp_node_get_attribute (query_node, "sid");
  if (stream_id == NULL || strcmp (stream_id, priv->stream_id) != 0)
    return FALSE;

  url_node = gibber_xmpp_node_get_child (query_node, "url");
  if (url_node == NULL)
    return FALSE;
  url = url_node->content;

  priv->stream_open_id = g_strdup (gibber_xmpp_node_get_attribute (
        stanza->node, "id"));

  if (connect_to_url (self, url))
    {
      g_object_set (self, "state", GIBBER_BYTESTREAM_STATE_OPEN,
          NULL);
    }
  else
    {
      GibberXmppStanza *reply;

      reply = gibber_iq_helper_new_error_reply (stanza,
          XMPP_ERROR_ITEM_NOT_FOUND, NULL);

      gibber_xmpp_connection_send (priv->xmpp_connection, stanza, NULL);
      g_object_unref (reply);
    }

  return TRUE;
}

static gboolean
parse_oob_close_iq (GibberBytestreamOOB *self,
                    GibberXmppStanza *stanza)
{
  GibberBytestreamOOBPrivate *priv = GIBBER_BYTESTREAM_OOB_GET_PRIVATE (self);
  GibberStanzaType type;
  GibberStanzaSubType sub_type;
  const gchar *id;

  gibber_xmpp_stanza_get_type_info (stanza, &type, &sub_type);

  if (type != GIBBER_STANZA_TYPE_IQ ||
      sub_type != GIBBER_STANZA_SUB_TYPE_RESULT)
    return FALSE;

  id = gibber_xmpp_node_get_attribute (stanza->node, "id");

  if (id == NULL || strcmp (id, priv->stream_open_id) != 0)
    return FALSE;

  DEBUG ("received OOB close stanza. Bytestream closed");

  g_object_set (self, "state", GIBBER_BYTESTREAM_STATE_CLOSED,
      NULL);

  return TRUE;
}

static void
xmpp_connection_received_stanza_cb (GibberXmppConnection *conn,
                                    GibberXmppStanza *stanza,
                                    gpointer user_data)
{
  GibberBytestreamOOB *self = (GibberBytestreamOOB *) user_data;
  const gchar *from;

  /* discard invalid stanza */
  from = gibber_xmpp_node_get_attribute (stanza->node, "from");
  if (from == NULL)
    {
      DEBUG ("got a message without a from field");
      return;
    }

  if (parse_oob_init_iq (self, stanza))
    return;

  if (parse_oob_close_iq (self, stanza))
    return;
}

static void
xmpp_connection_stream_closed_cb (GibberXmppConnection *connection,
                                  gpointer userdata)
{
  GibberBytestreamOOB *self = GIBBER_BYTESTREAM_OOB (userdata);
  DEBUG ("XMPP connection: stream closed. Close the OOB bytestream");
  g_object_set (self, "state", GIBBER_BYTESTREAM_STATE_CLOSED, NULL);
}

static void
xmpp_connection_transport_disconnected_cb (GibberLLTransport *transport,
                                      gpointer userdata)
{
  GibberBytestreamOOB *self = GIBBER_BYTESTREAM_OOB (userdata);
  DEBUG ("XMPP connection transport closed. Close the OOB bytestream");
  g_object_set (self, "state", GIBBER_BYTESTREAM_STATE_CLOSED, NULL);
}

static void
xmpp_connection_parse_error_cb (GibberXmppConnection *connection,
                                gpointer userdata)
{
  GibberBytestreamOOB *self = GIBBER_BYTESTREAM_OOB (userdata);
  DEBUG ("XMPP connection: parse error. Close the OOB bytestream");
  g_object_set (self, "state", GIBBER_BYTESTREAM_STATE_CLOSED, NULL);
}

static void
gibber_bytestream_oob_dispose (GObject *object)
{
  GibberBytestreamOOB *self = GIBBER_BYTESTREAM_OOB (object);
  GibberBytestreamOOBPrivate *priv = GIBBER_BYTESTREAM_OOB_GET_PRIVATE (self);

  if (priv->dispose_has_run)
    return;

  priv->dispose_has_run = TRUE;

  if (priv->state != GIBBER_BYTESTREAM_STATE_CLOSED)
    {
      gibber_bytestream_iface_close (GIBBER_BYTESTREAM_IFACE (self), NULL);
    }

  if (priv->listener != NULL)
    {
      g_io_channel_unref (priv->listener);
      priv->listener = NULL;
      g_source_remove (priv->listener_watch);
    }

  if (priv->data_channel != NULL)
    {
      g_io_channel_unref (priv->data_channel);
      priv->data_channel = NULL;
      g_source_remove (priv->data_watch);
    }

  G_OBJECT_CLASS (gibber_bytestream_oob_parent_class)->dispose (object);
}

static void
gibber_bytestream_oob_finalize (GObject *object)
{
  GibberBytestreamOOB *self = GIBBER_BYTESTREAM_OOB (object);
  GibberBytestreamOOBPrivate *priv = GIBBER_BYTESTREAM_OOB_GET_PRIVATE (self);

  g_free (priv->stream_id);
  g_free (priv->stream_init_id);
  g_free (priv->stream_open_id);
  g_free (priv->host);
  g_free (priv->self_id);
  g_free (priv->peer_id);

  if (priv->buffer != NULL)
    g_string_free (priv->buffer, TRUE);

  G_OBJECT_CLASS (gibber_bytestream_oob_parent_class)->finalize (object);
}

static void
gibber_bytestream_oob_get_property (GObject *object,
                                    guint property_id,
                                    GValue *value,
                                    GParamSpec *pspec)
{
  GibberBytestreamOOB *self = GIBBER_BYTESTREAM_OOB (object);
  GibberBytestreamOOBPrivate *priv = GIBBER_BYTESTREAM_OOB_GET_PRIVATE (self);

  switch (property_id)
    {
      case PROP_XMPP_CONNECTION:
        g_value_set_object (value, priv->xmpp_connection);
        break;
      case PROP_SELF_ID:
        g_value_set_string (value, priv->self_id);
        break;
      case PROP_PEER_ID:
        g_value_set_string (value, priv->peer_id);
        break;
      case PROP_STREAM_ID:
        g_value_set_string (value, priv->stream_id);
        break;
      case PROP_STREAM_INIT_ID:
        g_value_set_string (value, priv->stream_init_id);
        break;
      case PROP_STATE:
        g_value_set_uint (value, priv->state);
        break;
      case PROP_HOST:
        g_value_set_string (value, priv->host);
        break;
      case PROP_PROTOCOL:
        g_value_set_string (value, GIBBER_XMPP_NS_OOB);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
gibber_bytestream_oob_set_property (GObject *object,
                                    guint property_id,
                                    const GValue *value,
                                    GParamSpec *pspec)
{
  GibberBytestreamOOB *self = GIBBER_BYTESTREAM_OOB (object);
  GibberBytestreamOOBPrivate *priv = GIBBER_BYTESTREAM_OOB_GET_PRIVATE (self);

  switch (property_id)
    {
      case PROP_XMPP_CONNECTION:
        priv->xmpp_connection = g_value_get_object (value);
        g_signal_connect (priv->xmpp_connection, "received-stanza",
            G_CALLBACK (xmpp_connection_received_stanza_cb), self);
        g_signal_connect (priv->xmpp_connection, "stream-closed",
            G_CALLBACK (xmpp_connection_stream_closed_cb), self);
        g_signal_connect (priv->xmpp_connection->transport, "disconnected",
           G_CALLBACK (xmpp_connection_transport_disconnected_cb), self);
        g_signal_connect (priv->xmpp_connection, "parse-error",
           G_CALLBACK (xmpp_connection_parse_error_cb), self);
        break;
      case PROP_SELF_ID:
        g_free (priv->self_id);
        priv->self_id = g_value_dup_string (value);
        break;
      case PROP_PEER_ID:
        g_free (priv->peer_id);
        priv->peer_id = g_value_dup_string (value);
        break;
      case PROP_STREAM_ID:
        g_free (priv->stream_id);
        priv->stream_id = g_value_dup_string (value);
        break;
      case PROP_STREAM_INIT_ID:
        g_free (priv->stream_init_id);
        priv->stream_init_id = g_value_dup_string (value);
        break;
      case PROP_STATE:
        if (priv->state != g_value_get_uint (value))
            {
              priv->state = g_value_get_uint (value);
              g_signal_emit (object, signals[STATE_CHANGED], 0, priv->state);
            }
        break;
      case PROP_HOST:
        g_free (priv->host);
        priv->host = g_value_dup_string (value);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static GObject *
gibber_bytestream_oob_constructor (GType type,
                                   guint n_props,
                                   GObjectConstructParam *props)
{
  GObject *obj;
  GibberBytestreamOOBPrivate *priv;

  obj = G_OBJECT_CLASS (gibber_bytestream_oob_parent_class)->
           constructor (type, n_props, props);

  priv = GIBBER_BYTESTREAM_OOB_GET_PRIVATE (GIBBER_BYTESTREAM_OOB (obj));

  g_assert (priv->self_id != NULL);
  g_assert (priv->peer_id != NULL);
  g_assert (priv->stream_id != NULL);
  g_assert (priv->xmpp_connection != NULL);

  return obj;
}

static void
gibber_bytestream_oob_class_init (
    GibberBytestreamOOBClass *gibber_bytestream_oob_class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (gibber_bytestream_oob_class);
  GParamSpec *param_spec;

  g_type_class_add_private (gibber_bytestream_oob_class,
      sizeof (GibberBytestreamOOBPrivate));

  object_class->dispose = gibber_bytestream_oob_dispose;
  object_class->finalize = gibber_bytestream_oob_finalize;

  object_class->get_property = gibber_bytestream_oob_get_property;
  object_class->set_property = gibber_bytestream_oob_set_property;
  object_class->constructor = gibber_bytestream_oob_constructor;

  g_object_class_override_property (object_class, PROP_SELF_ID,
      "self-id");
  g_object_class_override_property (object_class, PROP_PEER_ID,
      "peer-id");
  g_object_class_override_property (object_class, PROP_STREAM_ID,
      "stream-id");
  g_object_class_override_property (object_class, PROP_STATE,
      "state");
  g_object_class_override_property (object_class, PROP_PROTOCOL,
      "protocol");

  param_spec = g_param_spec_object (
      "xmpp-connection",
      "GibberXmppConnection object",
      "Gibber XMPP connection object used for communication by this "
      "bytestream if it's a private one",
      GIBBER_TYPE_XMPP_CONNECTION,
      G_PARAM_CONSTRUCT_ONLY |
      G_PARAM_READWRITE |
      G_PARAM_STATIC_NAME |
      G_PARAM_STATIC_NICK |
      G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_XMPP_CONNECTION,
      param_spec);

  param_spec = g_param_spec_string (
      "stream-init-id",
      "stream init ID",
      "the iq ID of the SI request, if any",
      "",
      G_PARAM_CONSTRUCT_ONLY |
      G_PARAM_READWRITE |
      G_PARAM_STATIC_NAME |
      G_PARAM_STATIC_NICK |
      G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_STREAM_INIT_ID,
      param_spec);

  param_spec = g_param_spec_string (
      "host",
      "host",
      "The host to use in the OOB URL",
      "",
      G_PARAM_CONSTRUCT_ONLY |
      G_PARAM_READWRITE |
      G_PARAM_STATIC_NAME |
      G_PARAM_STATIC_NICK |
      G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_HOST,
      param_spec);

  signals[DATA_RECEIVED] =
    g_signal_new ("data-received",
                  G_OBJECT_CLASS_TYPE (gibber_bytestream_oob_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  _gibber_signals_marshal_VOID__STRING_POINTER,
                  G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_POINTER);

  signals[STATE_CHANGED] =
    g_signal_new ("state-changed",
                  G_OBJECT_CLASS_TYPE (gibber_bytestream_oob_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  g_cclosure_marshal_VOID__UINT,
                  G_TYPE_NONE, 1, G_TYPE_UINT);
}

/*
 * gibber_bytestream_oob_send
 *
 * Implements gibber_bytestream_iface_send on GibberBytestreamIface
 */
static gboolean
gibber_bytestream_oob_send (GibberBytestreamIface *bytestream,
                            guint len,
                            const gchar *str)
{
  GibberBytestreamOOB *self = GIBBER_BYTESTREAM_OOB (bytestream);
  GibberBytestreamOOBPrivate *priv = GIBBER_BYTESTREAM_OOB_GET_PRIVATE (self);
  GIOStatus status;
  gsize written;
  GError *error = NULL;

 if (priv->state != GIBBER_BYTESTREAM_STATE_OPEN)
    {
      DEBUG ("can't send data through a not open bytestream (state: %d)",
          priv->state);
      return FALSE;
    }

  status = g_io_channel_write_chars (priv->data_channel, str, len, &written,
      &error);
  if (status == G_IO_STATUS_NORMAL)
    {
      DEBUG ("%d bytes send", written);
    }
  else
    {
      DEBUG ("error writing to socket: %s", error ? error->message : "");
    }

  return TRUE;
}

static GibberXmppStanza *
create_si_accept_iq (GibberBytestreamOOB *self)
{
  GibberBytestreamOOBPrivate *priv = GIBBER_BYTESTREAM_OOB_GET_PRIVATE (self);

  return gibber_xmpp_stanza_build (
      GIBBER_STANZA_TYPE_IQ, GIBBER_STANZA_SUB_TYPE_RESULT,
      priv->self_id, priv->peer_id,
      GIBBER_NODE_ATTRIBUTE, "id", priv->stream_init_id,
      GIBBER_NODE, "si",
        GIBBER_NODE_XMLNS, GIBBER_XMPP_NS_SI,
        GIBBER_NODE, "feature",
          GIBBER_NODE_XMLNS, GIBBER_XMPP_NS_FEATURENEG,
          GIBBER_NODE, "x",
            GIBBER_NODE_XMLNS, GIBBER_XMPP_NS_DATA,
            GIBBER_NODE_ATTRIBUTE, "type", "submit",
            GIBBER_NODE, "field",
              GIBBER_NODE_ATTRIBUTE, "var", "stream-method",
              GIBBER_NODE, "value",
                GIBBER_NODE_TEXT, GIBBER_XMPP_NS_OOB,
              GIBBER_NODE_END,
            GIBBER_NODE_END,
          GIBBER_NODE_END,
        GIBBER_NODE_END,
      GIBBER_NODE_END, GIBBER_STANZA_END);
}

/*
 * gibber_bytestream_oob_accept
 *
 * Implements gibber_bytestream_iface_accept on GibberBytestreamIface
 */
static void
gibber_bytestream_oob_accept (GibberBytestreamIface *bytestream,
                              GibberBytestreamAugmentSiAcceptReply func,
                              gpointer user_data)
{
  GibberBytestreamOOB *self = GIBBER_BYTESTREAM_OOB (bytestream);
  GibberBytestreamOOBPrivate *priv = GIBBER_BYTESTREAM_OOB_GET_PRIVATE (self);
  GibberXmppStanza *stanza;
  GibberXmppNode *si;

  if (priv->state != GIBBER_BYTESTREAM_STATE_LOCAL_PENDING)
    {
      /* The stream was previoulsy or automatically accepted */
      DEBUG ("stream was already accepted");
      return;
    }

  stanza = create_si_accept_iq (self);
  si = gibber_xmpp_node_get_child_ns (stanza->node, "si", GIBBER_XMPP_NS_SI);
  g_assert (si != NULL);

  /* let the caller add his profile specific data */
  func (si, user_data);

  gibber_xmpp_connection_send (priv->xmpp_connection, stanza, NULL);

  DEBUG ("stream is now accepted");
  g_object_set (self, "state", GIBBER_BYTESTREAM_STATE_ACCEPTED, NULL);
}

static void
gibber_bytestream_oob_decline (GibberBytestreamOOB *self,
                               GError *error)
{
  GibberBytestreamOOBPrivate *priv = GIBBER_BYTESTREAM_OOB_GET_PRIVATE (self);
  GibberXmppStanza *stanza;

  g_return_if_fail (priv->state == GIBBER_BYTESTREAM_STATE_LOCAL_PENDING);

  stanza = gibber_xmpp_stanza_build (
      GIBBER_STANZA_TYPE_IQ, GIBBER_STANZA_SUB_TYPE_ERROR,
      priv->self_id, priv->peer_id,
      GIBBER_NODE_ATTRIBUTE, "id", priv->stream_init_id,
      GIBBER_STANZA_END);

  if (error != NULL && error->domain == GIBBER_XMPP_ERROR)
    {
      gibber_xmpp_error_to_node (error->code, stanza->node, error->message);
    }
  else
    {
      gibber_xmpp_error_to_node (XMPP_ERROR_FORBIDDEN, stanza->node,
          "Offer Declined");
    }

  gibber_xmpp_connection_send (priv->xmpp_connection, stanza, NULL);

  g_object_unref (stanza);
}

/*
 * gibber_bytestream_oob_close
 *
 * Implements gibber_bytestream_iface_close on GibberBytestreamIface
 */
static void
gibber_bytestream_oob_close (GibberBytestreamIface *bytestream,
                             GError *error)
{
  GibberBytestreamOOB *self = GIBBER_BYTESTREAM_OOB (bytestream);
  GibberBytestreamOOBPrivate *priv = GIBBER_BYTESTREAM_OOB_GET_PRIVATE (self);

  if (priv->state == GIBBER_BYTESTREAM_STATE_CLOSED)
     /* bytestream already closed, do nothing */
     return;

  if (priv->state == GIBBER_BYTESTREAM_STATE_LOCAL_PENDING)
    {
      /* Stream was created using SI so we decline the request */
      gibber_bytestream_oob_decline (self, error);
    }

  if (priv->xmpp_connection->stream_flags ==
      GIBBER_XMPP_CONNECTION_STREAM_FULLY_OPEN)
    {
      GibberXmppStanza *stanza;

      /* As described in the XEP, we send result IQ when we have
       * finished to use the OOB */
      stanza = make_iq_oob_sucess_response (priv->self_id,
          priv->peer_id, priv->stream_open_id);

      DEBUG ("send OOB close stanza");

      gibber_xmpp_connection_send (priv->xmpp_connection, stanza, NULL);
      g_object_unref (stanza);
    }
  else
    {
      DEBUG ("XMPP connection is closed. Don't send OOB close stanza");
    }

  g_object_set (self, "state", GIBBER_BYTESTREAM_STATE_CLOSED, NULL);
}

static GibberXmppStanza *
make_oob_init_iq (const gchar *from,
                  const gchar *to,
                  const gchar *stream_id,
                  const gchar *url)
{
  return gibber_xmpp_stanza_build (
      GIBBER_STANZA_TYPE_IQ, GIBBER_STANZA_SUB_TYPE_SET,
      from, to,
      GIBBER_NODE, "query",
        GIBBER_NODE_XMLNS, GIBBER_XMPP_NS_OOB,
        GIBBER_NODE_ATTRIBUTE, "sid", stream_id,
        GIBBER_NODE, "url",
          GIBBER_NODE_TEXT, url,
        GIBBER_NODE_END,
      GIBBER_NODE_END, GIBBER_STANZA_END);
}

static void
normalize_address (struct sockaddr_storage *addr)
{
  struct sockaddr_in *s4 = (struct sockaddr_in *) addr;
  struct sockaddr_in6 *s6 = (struct sockaddr_in6 *) addr;

  if (s6->sin6_family == AF_INET6 && IN6_IS_ADDR_V4MAPPED (&(s6->sin6_addr)))
    {
      /* Normalize to ipv4 address */
      s4->sin_family = AF_INET;
      s4->sin_addr.s_addr = s6->sin6_addr.s6_addr32[3];
    }
}

static gboolean
listener_io_in_cb (GIOChannel *source,
                   GIOCondition condition,
                   gpointer user_data)
{
  GibberBytestreamOOB *self = GIBBER_BYTESTREAM_OOB (user_data);
  GibberBytestreamOOBPrivate *priv = GIBBER_BYTESTREAM_OOB_GET_PRIVATE (self);
  int listen_fd, fd, ret;
  char host[NI_MAXHOST];
  char port[NI_MAXSERV];
  struct sockaddr_storage addr;
  socklen_t addrlen = sizeof (struct sockaddr_storage);

  listen_fd = g_io_channel_unix_get_fd (source);
  fd = accept (listen_fd, (struct sockaddr *) &addr, &addrlen);
  normalize_address (&addr);

  ret = getnameinfo ((struct sockaddr *) &addr, addrlen,
      host, NI_MAXHOST, port, NI_MAXSERV,
      NI_NUMERICHOST | NI_NUMERICSERV);

  if (ret == 0)
    DEBUG("New connection from %s port %s", host, port);
  else
    DEBUG("New connection..");

  /* FIXME: we should probably check if it's the right host */

  priv->data_channel = g_io_channel_unix_new (fd);
  g_io_channel_set_encoding (priv->data_channel, NULL, NULL);
  g_io_channel_set_buffered (priv->data_channel, FALSE);
  g_io_channel_set_close_on_unref (priv->data_channel, TRUE);

  priv->data_watch = g_io_add_watch (priv->data_channel, G_IO_IN,
      data_io_in_cb, self);

  DEBUG ("bytestream is now open");
  g_object_set (self, "state", GIBBER_BYTESTREAM_STATE_OPEN, NULL);

  return FALSE;
}

static int
start_listen_for_connection (GibberBytestreamOOB *self)
{
  GibberBytestreamOOBPrivate *priv = GIBBER_BYTESTREAM_OOB_GET_PRIVATE (self);
  int port;
  int fd = -1, ret, yes = 1;
  struct addrinfo req, *ans = NULL;
  struct sockaddr *addr;
  struct sockaddr_in addr4;
  struct sockaddr_in6 addr6;
  socklen_t len;
  #define BACKLOG 1

  memset (&req, 0, sizeof(req));
  req.ai_flags = AI_PASSIVE;
  req.ai_family = AF_UNSPEC;
  req.ai_socktype = SOCK_STREAM;
  req.ai_protocol = IPPROTO_TCP;

  ret = getaddrinfo (priv->host, "0", &req, &ans);
  if (ret != 0)
    {
      DEBUG ("getaddrinfo failed: %s", gai_strerror (ret));
      goto error;
    }

  ((struct sockaddr_in *) ans->ai_addr)->sin_port = 0;

  fd = socket (ans->ai_family, ans->ai_socktype, ans->ai_protocol);
  if (fd == -1)
    {
      DEBUG ("socket failed: %s", g_strerror (errno));
      goto error;
    }

  ret = setsockopt (fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof (int));
  if (ret == -1)
    {
      DEBUG ("setsockopt failed: %s", g_strerror (errno));
      goto error;
    }

  ret = bind (fd, ans->ai_addr, ans->ai_addrlen);
  if (ret  < 0)
    {
      DEBUG ("bind failed: %s", g_strerror (errno));
      goto error;
    }

  if (ans->ai_family == AF_INET)
    {
      len = sizeof (struct sockaddr_in);
      addr = (struct sockaddr *) &addr4;
    }
  else
    {
      len = sizeof (struct sockaddr_in6);
      addr = (struct sockaddr *) &addr6;
    }

  if (getsockname (fd, addr, &len) == -1)
  {
    DEBUG ("getsockname failed: %s", g_strerror (errno));
    goto error;
  }

  if (ans->ai_family == AF_INET)
    {
      port = ntohs (addr4.sin_port);
    }
  else
    {
      port = ntohs (addr6.sin6_port);
    }

  ret = listen (fd, BACKLOG);
  if (ret == -1)
    {
      DEBUG ("listen failed: %s", g_strerror (errno));
      goto error;
    }

  DEBUG ("listen on %s:%d", priv->host, port);

  priv->listener = g_io_channel_unix_new (fd);
  g_io_channel_set_close_on_unref (priv->listener, TRUE);
  priv->listener_watch = g_io_add_watch (priv->listener, G_IO_IN,
      listener_io_in_cb, self);

  freeaddrinfo (ans);
  return port;

error:
  if (fd > 0)
    close(fd);

  if (ans != NULL)
    freeaddrinfo (ans);
  return -1;
}

/*
 * gibber_bytestream_oob_initiate
 *
 * Implements gibber_bytestream_iface_initiate on GibberBytestreamIface
 */
gboolean
gibber_bytestream_oob_initiate (GibberBytestreamIface *bytestream)
{
  GibberBytestreamOOB *self = GIBBER_BYTESTREAM_OOB (bytestream);
  GibberBytestreamOOBPrivate *priv = GIBBER_BYTESTREAM_OOB_GET_PRIVATE (self);
  GibberXmppStanza *stanza;
  GError *error = NULL;
  const gchar *id;
  int port;
  gchar *url;

  if (priv->state != GIBBER_BYTESTREAM_STATE_INITIATING)
    {
      DEBUG ("bytestream is not is the initiating state (state %d)",
          priv->state);
      return FALSE;
    }
  g_assert (priv->host != NULL);

  port = start_listen_for_connection (self);
  if (port == -1)
    {
      DEBUG ("can't listen for incoming connection");
      return FALSE;
    }

  url = g_strdup_printf ("x-tcp://%s:%d", priv->host, port);

  stanza = make_oob_init_iq (priv->self_id, priv->peer_id,
      priv->stream_id, url);
  g_free (url);

  id = gibber_xmpp_node_get_attribute (stanza->node, "id");
  if (id == NULL)
    {
      priv->stream_open_id = gibber_xmpp_connection_new_id (
          priv->xmpp_connection);
      gibber_xmpp_node_set_attribute (stanza->node, "id",
          priv->stream_open_id);
    }
  else
    {
      priv->stream_open_id = g_strdup (id);
    }

  if (!gibber_xmpp_connection_send (priv->xmpp_connection, stanza, &error))
    {
      DEBUG ("can't send OOB init stanza: %s", error->message);
      return FALSE;
    }

  g_object_unref (stanza);

  return TRUE;
}

static void
bytestream_iface_init (gpointer g_iface,
                       gpointer iface_data)
{
  GibberBytestreamIfaceClass *klass = (GibberBytestreamIfaceClass *) g_iface;

  klass->initiate = gibber_bytestream_oob_initiate;
  klass->send = gibber_bytestream_oob_send;
  klass->close = gibber_bytestream_oob_close;
  klass->accept = gibber_bytestream_oob_accept;
}