/*
 * salut-multicast-muc-transport.c - Source for SalutMulticastMucTransport
 * Copyright (C) 2006 Collabora Ltd.
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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

/* Networking stuff */
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>

#include "salut-connection.h"
#include "salut-muc-transport-iface.h"
#include "salut-multicast-muc-transport.h"

/* For CHANNEL_TEXT_ERROR... */
#include "text-mixin.h"

#include <loudmouth/loudmouth.h>
#include "lm-parser.h"
#include "telepathy-errors.h"

#define DEBUG_FLAG DEBUG_MUC
#include <debug.h>

#define BUFSIZE 1500

static void salut_multicast_muc_transport_iface_init(gpointer *g_iface,
                                                     gpointer *iface_data);

G_DEFINE_TYPE_WITH_CODE(SalutMulticastMucTransport, 
                        salut_multicast_muc_transport, 
                        G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(SALUT_TYPE_MUC_TRANSPORT_IFACE,
                                     salut_multicast_muc_transport_iface_init));

/* Properties */
enum { 
  PROP_CONNECTION = 1,
  PROP_MUC_NAME
};

/* Privates */
typedef struct _SalutMulticastMucTransportPrivate SalutMulticastMucTransportPrivate;

struct _SalutMulticastMucTransportPrivate
{
  gboolean dispose_has_run;
  SalutConnection *connection;
  gchar *muc_name;
  GIOChannel *channel;
  int fd;
  guint watch_in;
  guint watch_err;
  struct sockaddr_storage address;
  LmParser *parser;
};

#define SALUT_MULTICAST_MUC_TRANSPORT_GET_PRIVATE(o)     (G_TYPE_INSTANCE_GET_PRIVATE ((o), SALUT_TYPE_MULTICAST_MUC_TRANSPORT, SalutMulticastMucTransportPrivate))

static void
salut_multicast_muc_transport_init (SalutMulticastMucTransport *obj)
{
  SalutMulticastMucTransportPrivate *priv = SALUT_MULTICAST_MUC_TRANSPORT_GET_PRIVATE (obj);

  /* allocate any data required by the object here */
  priv->connection = NULL;
  priv->muc_name = NULL;
  priv->fd = -1;
  priv->watch_in = 0;
  priv->watch_err = 0;
  priv->parser = NULL;
  priv->channel = NULL;
}


static void
salut_multicast_muc_transport_get_property (GObject    *object,
                                            guint       property_id,
                                            GValue     *value,
                                            GParamSpec *pspec) {
  SalutMulticastMucTransport *transport = 
                  SALUT_MULTICAST_MUC_TRANSPORT (object);
  SalutMulticastMucTransportPrivate *priv = 
                  SALUT_MULTICAST_MUC_TRANSPORT_GET_PRIVATE (transport);

  switch (property_id) {
    case PROP_MUC_NAME:
      g_value_set_string (value, priv->muc_name);
      break;
    case PROP_CONNECTION:
      g_value_set_object (value, priv->connection);
      break;
  }
}

static void
salut_multicast_muc_transport_set_property (GObject     *object,
                                            guint        property_id,
                                            const GValue *value,
                                            GParamSpec   *pspec) {
  SalutMulticastMucTransport *transport = 
                  SALUT_MULTICAST_MUC_TRANSPORT (object);
  SalutMulticastMucTransportPrivate *priv = 
                  SALUT_MULTICAST_MUC_TRANSPORT_GET_PRIVATE (transport);

  switch (property_id) {
    case PROP_CONNECTION:
      priv->connection = g_value_get_object(value);
      break;
    case PROP_MUC_NAME:
      g_free(priv->muc_name);
      priv->muc_name = g_value_dup_string(value);
      break;
  }
}

static void salut_multicast_muc_transport_dispose (GObject *object);
static void salut_multicast_muc_transport_finalize (GObject *object);

static void
salut_multicast_muc_transport_class_init(SalutMulticastMucTransportClass *salut_multicast_muc_transport_class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (salut_multicast_muc_transport_class);

  g_type_class_add_private (salut_multicast_muc_transport_class, sizeof (SalutMulticastMucTransportPrivate));

  object_class->dispose = salut_multicast_muc_transport_dispose;
  object_class->finalize = salut_multicast_muc_transport_finalize;
  object_class->set_property = salut_multicast_muc_transport_set_property;
  object_class->get_property = salut_multicast_muc_transport_get_property;

  g_object_class_override_property(object_class, PROP_CONNECTION, "connection");
  g_object_class_override_property(object_class, PROP_MUC_NAME, "muc-name");
}

void
salut_multicast_muc_transport_dispose (GObject *object)
{
  SalutMulticastMucTransport *self = SALUT_MULTICAST_MUC_TRANSPORT (object);
  SalutMulticastMucTransportPrivate *priv = SALUT_MULTICAST_MUC_TRANSPORT_GET_PRIVATE (self);

  if (priv->dispose_has_run)
    return;

  priv->dispose_has_run = TRUE;
  if (priv->connection != NULL) {
    g_object_unref(priv->connection);
    priv->connection = NULL;
  }

  if (priv->channel) {
    g_io_channel_shutdown(priv->channel, FALSE, NULL);
    g_io_channel_unref(priv->channel);
    priv->channel = NULL;
  }

  if (priv->parser) {
    lm_parser_free(priv->parser);
    priv->parser = NULL;
  }

  /* release any references held by the object here */

  if (G_OBJECT_CLASS (salut_multicast_muc_transport_parent_class)->dispose)
    G_OBJECT_CLASS (salut_multicast_muc_transport_parent_class)->dispose (object);
}

void
salut_multicast_muc_transport_finalize (GObject *object)
{
  SalutMulticastMucTransport *self = SALUT_MULTICAST_MUC_TRANSPORT (object);
  SalutMulticastMucTransportPrivate *priv = SALUT_MULTICAST_MUC_TRANSPORT_GET_PRIVATE (self);

  /* free any data held directly by the object here */
  g_free(priv->muc_name);
  priv->muc_name = NULL;

  G_OBJECT_CLASS (salut_multicast_muc_transport_parent_class)->finalize (object);
}

static void
_message_parsed(LmParser *parser, LmMessage *message, gpointer data) {
  SalutMulticastMucTransport *self = 
    SALUT_MULTICAST_MUC_TRANSPORT(data);

   switch (lm_message_get_type(message)) {
     case LM_MESSAGE_TYPE_MESSAGE:
       g_signal_emit_by_name(self, "message-received::message", message);
       break;
     case LM_MESSAGE_TYPE_PRESENCE:
       g_signal_emit_by_name(self, "message-received::presence", message);
       break;
     case LM_MESSAGE_TYPE_IQ:
       g_signal_emit_by_name(self, "message-received",
                      g_quark_from_static_string("iq") , message);
       break;
     default:
       g_signal_emit_by_name(self, "message-received",
                      g_quark_from_static_string("unkown") , message);
   }
}

static gboolean
_channel_io_in(GIOChannel *source, GIOCondition condition, gpointer data) {
  SalutMulticastMucTransport *self = 
    SALUT_MULTICAST_MUC_TRANSPORT(data);
  SalutMulticastMucTransportPrivate *priv = 
    SALUT_MULTICAST_MUC_TRANSPORT_GET_PRIVATE(self);
  gchar buf[BUFSIZE + 1];

  struct sockaddr_storage from;
  int ret;
  socklen_t len = sizeof(struct sockaddr_storage);

  ret = recvfrom(priv->fd, buf, BUFSIZE, 0, (struct sockaddr *)&from, &len);
  if (ret < 0) {
    DEBUG("recv failed: %s", strerror(errno)); 
    /* FIXME should disconnect */
    return TRUE;
  }

  buf[ret]  = '\0';
  DEBUG("Received message: %s", buf);

  lm_parser_parse(priv->parser, buf);

  return TRUE;
}

static gboolean
_channel_io_err(GIOChannel *source, GIOCondition condition, gpointer data) {
  /* Either _HUP or _ERR */
  /* Should disconnect */
  DEBUG("ERR");
  return TRUE;
}

int
_open_multicast(SalutMulticastMucTransport *self) {
  SalutMulticastMucTransportPrivate *priv = 
    SALUT_MULTICAST_MUC_TRANSPORT_GET_PRIVATE(self);
  int ret;
  struct addrinfo hints;
  struct addrinfo *ans; 
  unsigned char yes = 1;
  unsigned char one = 1;
  unsigned char no = 0;
  int fd = -1;

  hints.ai_flags = AI_PASSIVE;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = IPPROTO_UDP;

  ret = getaddrinfo("239.192.0.42", "4266", &hints, &ans);
  if (ret < 0) {
    DEBUG("Getaddrinfo failed: %s", gai_strerror(ret));
    goto err;
  }

  if (ans == NULL) {
    DEBUG("Couldn't find address");
    goto err;
  }

  if (ans->ai_next != NULL) {
    DEBUG("Address wasn't unique! Ignoring");
    goto err;
  }

  memcpy(&(priv->address), ans->ai_addr, ans->ai_addrlen);

  /* Only try the first! */
  switch (ans->ai_family) {
    case AF_INET: {
      struct ip_mreq mreq;
      fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

      if (fd < 0) {
        DEBUG("Failed to open socket: %s", strerror(errno));
        goto err;
      }

      setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#ifdef SO_REUSEPORT
      setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
#endif
      setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &no, sizeof(no));
      setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &one, sizeof(one));

      mreq.imr_multiaddr = ((struct sockaddr_in *)ans->ai_addr)->sin_addr;
      mreq.imr_interface.s_addr = htonl(INADDR_ANY);
      if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, 
           sizeof(mreq)) < 0) {
        DEBUG("Failed to join multicast group: %s", strerror(errno));
        goto err;
      }

      if (bind(fd, ans->ai_addr, ans->ai_addrlen) != 0) {
        DEBUG("Failed to bind to socket: %s", strerror(errno));
        goto err;
      }

  /*    if (connect(fd, ans->ai_addr, ans->ai_addrlen) != 0) {
        DEBUG("Failed to connect to socket: %s", strerror(errno));
        goto err;
      }*/
      break;
    }
    case AF_INET6: {
      struct ipv6_mreq mreq6;
      fd = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);

      if (fd < 0) {
        DEBUG("Failed to open socket: %s", strerror(errno));
        goto err;
      }

      setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
      setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &one, sizeof(one));
      setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, &no, sizeof(no));

      mreq6.ipv6mr_multiaddr = ((struct sockaddr_in6 *)ans->ai_addr)->sin6_addr;
      mreq6.ipv6mr_interface = 0;
      if (setsockopt(fd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq6, 
           sizeof(mreq6)) < 0) {
        DEBUG("Failed to join multicast group: %s", strerror(errno));
        goto err;
      }

      if (bind(fd, ans->ai_addr, ans->ai_addrlen) != 0) {
        DEBUG("Failed to bind to socket: %s", strerror(errno));
        goto err;
      }
      break;
    }
    default:
      DEBUG("Address from an unsupported address family: %d", ans->ai_family);
  }

  goto out;
err:
  if (fd > 0) {
    close(fd);
  }
  fd = -1;
out:
  if (ans != NULL) {
    freeaddrinfo(ans);
  }
  return fd;
}

gboolean 
salut_multicast_muc_transport_connect (SalutMucTransportIface *iface, 
                                       GError **error) {
  SalutMulticastMucTransport *self = 
    SALUT_MULTICAST_MUC_TRANSPORT(iface);
  SalutMulticastMucTransportPrivate *priv = 
    SALUT_MULTICAST_MUC_TRANSPORT_GET_PRIVATE(self);
  int fd = _open_multicast(self);

  if (fd < 0 ) {
    return FALSE;
  }

  priv->fd = fd;

  priv->channel = g_io_channel_unix_new(fd);

  g_io_channel_set_close_on_unref(priv->channel, TRUE);
  g_io_channel_set_encoding(priv->channel, NULL, NULL);
  g_io_channel_set_buffered(priv->channel, FALSE);

  priv->watch_in = 
    g_io_add_watch(priv->channel, G_IO_IN, _channel_io_in, iface);
  priv->watch_err = 
    g_io_add_watch(priv->channel, G_IO_ERR|G_IO_HUP, _channel_io_err, iface);

  if (priv->parser) {
    lm_parser_free(priv->parser);
  };
  priv->parser = lm_parser_new(_message_parsed, self, NULL);

  g_signal_emit_by_name(self, "connected", priv->connection->self_handle);
  return TRUE;
}

void 
salut_multicast_muc_transport_close (SalutMucTransportIface *iface) {
  SalutMulticastMucTransport *self = 
    SALUT_MULTICAST_MUC_TRANSPORT(iface);
  SalutMulticastMucTransportPrivate *priv = 
    SALUT_MULTICAST_MUC_TRANSPORT_GET_PRIVATE(self);

  /* Ensure we're connected */
  g_assert(priv->fd > 0);

  if (priv->channel != NULL) { 
    g_io_channel_shutdown(priv->channel, FALSE, NULL);
    g_io_channel_unref(priv->channel);
  }
  priv->fd = -1;

  lm_parser_free(priv->parser);
  priv->parser = NULL;

  g_signal_emit_by_name(self, "disconnected", priv->connection->self_handle);
}

gboolean 
salut_multicast_muc_transport_send (SalutMucTransportIface *iface, 
                                    LmMessage *message, GError **error,
                                    gint *texterror) {
  SalutMulticastMucTransport *self = 
    SALUT_MULTICAST_MUC_TRANSPORT(iface);
  SalutMulticastMucTransportPrivate *priv = 
    SALUT_MULTICAST_MUC_TRANSPORT_GET_PRIVATE(self);
  int len;
  gchar *text;

  text = lm_message_node_to_string(message->node);
  len = strlen(text);

  if (len > BUFSIZE) {
    DEBUG("Message too long");
    *error = g_error_new(TELEPATHY_ERRORS, NotAvailable, "Message too long");
    *texterror = CHANNEL_TEXT_SEND_ERROR_TOO_LONG;
    return FALSE;
  }

  if (sendto(priv->fd, text, len, 0, 
             (struct sockaddr *)&(priv->address), 
             sizeof(struct sockaddr_storage)) < 0) {
    DEBUG("send failed: %s", strerror(errno));
    if (texterror != NULL) {
      *texterror = CHANNEL_TEXT_SEND_ERROR_UNKNOWN;
    }
    if (error != NULL) {
      *error = 
        g_error_new(TELEPATHY_ERRORS, NetworkError, "%s", strerror(errno));
    } 
  }

  return TRUE;
}


static void 
salut_multicast_muc_transport_iface_init(gpointer *g_iface,  
                                         gpointer *iface_data) {
  SalutMucTransportIfaceClass *klass = (SalutMucTransportIfaceClass *)g_iface;
  klass->connect = salut_multicast_muc_transport_connect;
  klass->close = salut_multicast_muc_transport_close;
  klass->send = salut_multicast_muc_transport_send;
}
