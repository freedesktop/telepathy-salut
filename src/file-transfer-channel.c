/*
 * file-transfer-channel.c - Source for SalutFileTransferChannel
 * Copyright (C) 2007 Marco Barisione <marco@barisione.org>
 * Copyright (C) 2005, 2007, 2008 Collabora Ltd.
 *   @author: Sjoerd Simons <sjoerd@luon.net>
 *   @author: Jonny Lamb <jonny.lamb@collabora.co.uk>
 *   @author: Guillaume Desmottes <guillaume.desmottes@collabora.co.uk>
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

#include <glib/gstdio.h>
#include <dbus/dbus-glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#define DEBUG_FLAG DEBUG_FT
#include "debug.h"

#include "file-transfer-channel.h"
#include "signals-marshal.h"

#include "connection.h"
#include "im-manager.h"
#include "contact.h"
#include "namespaces.h"

#include <wocky/wocky.h>
#include <gibber/gibber-file-transfer.h>
#include <gibber/gibber-oob-file-transfer.h>

#include <telepathy-glib/channel-iface.h>
#include <telepathy-glib/interfaces.h>
#include <telepathy-glib/dbus.h>
#include <telepathy-glib/svc-generic.h>
#include <telepathy-glib/gtypes.h>

static void
channel_iface_init (gpointer g_iface, gpointer iface_data);
static void
file_transfer_iface_init (gpointer g_iface, gpointer iface_data);

G_DEFINE_TYPE_WITH_CODE (SalutFileTransferChannel, salut_file_transfer_channel,
    G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_CHANNEL, channel_iface_init);
    G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_DBUS_PROPERTIES,
                           tp_dbus_properties_mixin_iface_init);
    G_IMPLEMENT_INTERFACE (TP_TYPE_EXPORTABLE_CHANNEL, NULL);
    G_IMPLEMENT_INTERFACE (TP_TYPE_CHANNEL_IFACE, NULL);
    G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_CHANNEL_TYPE_FILE_TRANSFER,
                           file_transfer_iface_init);
    G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_CHANNEL_INTERFACE_FILE_TRANSFER_METADATA,
                           NULL);
);

#define CHECK_STR_EMPTY(x) ((x) == NULL || (x)[0] == '\0')

#define SALUT_UNDEFINED_FILE_SIZE G_MAXUINT64

static const char *salut_file_transfer_channel_interfaces[] = { NULL };

/* properties */
enum
{
  PROP_OBJECT_PATH = 1,

   /* org.freedesktop.Telepathy.Channel D-Bus properties */
  PROP_CHANNEL_TYPE,
  PROP_INTERFACES,
  PROP_HANDLE,
  PROP_TARGET_ID,
  PROP_HANDLE_TYPE,
  PROP_REQUESTED,
  PROP_INITIATOR_HANDLE,
  PROP_INITIATOR_ID,

  PROP_CHANNEL_DESTROYED,
  PROP_CHANNEL_PROPERTIES,

  /* org.freedesktop.Telepathy.Channel.Type.FileTransfer D-Bus properties */
  PROP_STATE,
  PROP_CONTENT_TYPE,
  PROP_FILENAME,
  PROP_SIZE,
  PROP_CONTENT_HASH_TYPE,
  PROP_CONTENT_HASH,
  PROP_DESCRIPTION,
  PROP_DATE,
  PROP_AVAILABLE_SOCKET_TYPES,
  PROP_TRANSFERRED_BYTES,
  PROP_INITIAL_OFFSET,
  PROP_URI,

  /* Chan.I.FileTransfer.Metadata */
  PROP_SERVICE_NAME,
  PROP_METADATA,

  PROP_CONTACT,
  PROP_CONNECTION,
  LAST_PROPERTY
};

/* private structure */
struct _SalutFileTransferChannelPrivate {
  gboolean dispose_has_run;
  gboolean closed;
  gchar *object_path;
  TpHandle handle;
  SalutContact *contact;
  SalutConnection *connection;
  GibberFileTransfer *ft;
  GTimeVal last_transferred_bytes_emitted;
  guint progress_timer;
  GValue *socket_address;
  TpHandle initiator;
  gboolean remote_accepted;
  gchar *path;

  /* properties */
  TpFileTransferState state;
  gchar *content_type;
  gchar *filename;
  guint64 size;
  TpFileHashType content_hash_type;
  gchar *content_hash;
  gchar *description;
  GHashTable *available_socket_types;
  guint64 transferred_bytes;
  guint64 initial_offset;
  guint64 date;
  gchar *uri;
  gchar *service_name;
  GHashTable *metadata;
};

static void
salut_file_transfer_channel_do_close (SalutFileTransferChannel *self)
{
  if (self->priv->closed)
    return;

  DEBUG ("Emitting closed signal for %s", self->priv->object_path);
  tp_svc_channel_emit_closed (self);
  self->priv->closed = TRUE;
}

static void
salut_file_transfer_channel_init (SalutFileTransferChannel *obj)
{
  obj->priv = G_TYPE_INSTANCE_GET_PRIVATE (obj,
      SALUT_TYPE_FILE_TRANSFER_CHANNEL, SalutFileTransferChannelPrivate);

  /* allocate any data required by the object here */
  obj->priv->object_path = NULL;
  obj->priv->contact = NULL;
}

static void salut_file_transfer_channel_set_state (
    TpSvcChannelTypeFileTransfer *iface, TpFileTransferState state,
    TpFileTransferStateChangeReason reason);

static void
contact_lost_cb (SalutContact *contact,
                 SalutFileTransferChannel *self)
{
  g_assert (contact == self->priv->contact);

  if (self->priv->state != TP_FILE_TRANSFER_STATE_PENDING)
    {
      DEBUG ("%s was disconnected. Ignoring as there is still a chance to"
         " be able to complete the transfer", contact->name);
      return;
    }

  DEBUG ("%s was disconnected. Cancel file tranfer.", contact->name);
  salut_file_transfer_channel_set_state (
      TP_SVC_CHANNEL_TYPE_FILE_TRANSFER (self),
      TP_FILE_TRANSFER_STATE_CANCELLED,
      TP_FILE_TRANSFER_STATE_CHANGE_REASON_REMOTE_STOPPED);
}

static void
salut_file_transfer_channel_get_property (GObject *object,
                                          guint property_id,
                                          GValue *value,
                                          GParamSpec *pspec)
{
  SalutFileTransferChannel *self = SALUT_FILE_TRANSFER_CHANNEL (object);
  TpBaseConnection *base_conn = (TpBaseConnection *) self->priv->connection;

  switch (property_id)
    {
      case PROP_OBJECT_PATH:
        g_value_set_string (value, self->priv->object_path);
        break;
      case PROP_CHANNEL_TYPE:
        g_value_set_static_string (value,
            TP_IFACE_CHANNEL_TYPE_FILE_TRANSFER);
        break;
      case PROP_HANDLE_TYPE:
        g_value_set_uint (value, TP_HANDLE_TYPE_CONTACT);
        break;
      case PROP_TARGET_ID:
        {
           TpHandleRepoIface *repo = tp_base_connection_get_handles (base_conn,
             TP_HANDLE_TYPE_CONTACT);

           g_value_set_string (value, tp_handle_inspect (repo,
                 self->priv->handle));
        }
        break;
      case PROP_HANDLE:
        g_value_set_uint (value, self->priv->handle);
        break;
      case PROP_REQUESTED:
        g_value_set_boolean (value, (self->priv->initiator ==
              base_conn->self_handle));
        break;
      case PROP_INITIATOR_HANDLE:
        g_value_set_uint (value, self->priv->initiator);
        break;
      case PROP_INITIATOR_ID:
          {
            TpHandleRepoIface *repo = tp_base_connection_get_handles (
                base_conn, TP_HANDLE_TYPE_CONTACT);

            g_assert (self->priv->initiator != 0);
            g_value_set_string (value,
                tp_handle_inspect (repo, self->priv->initiator));
          }
        break;
      case PROP_CONTACT:
        g_value_set_object (value, self->priv->contact);
        break;
      case PROP_CONNECTION:
        g_value_set_object (value, self->priv->connection);
        break;
      case PROP_INTERFACES:
        g_value_set_boxed (value, salut_file_transfer_channel_interfaces);
        break;
      case PROP_STATE:
        g_value_set_uint (value, self->priv->state);
        break;
      case PROP_CONTENT_TYPE:
        g_value_set_string (value, self->priv->content_type);
        break;
      case PROP_FILENAME:
        g_value_set_string (value, self->priv->filename);
        break;
      case PROP_SIZE:
        g_value_set_uint64 (value, self->priv->size);
        break;
      case PROP_CONTENT_HASH_TYPE:
        g_value_set_uint (value, self->priv->content_hash_type);
        break;
      case PROP_CONTENT_HASH:
        g_value_set_string (value, self->priv->content_hash);
        break;
      case PROP_DESCRIPTION:
        g_value_set_string (value, self->priv->description);
        break;
      case PROP_AVAILABLE_SOCKET_TYPES:
        g_value_set_boxed (value, self->priv->available_socket_types);
        break;
      case PROP_TRANSFERRED_BYTES:
        g_value_set_uint64 (value, self->priv->transferred_bytes);
        break;
      case PROP_INITIAL_OFFSET:
        g_value_set_uint64 (value, self->priv->initial_offset);
        break;
      case PROP_DATE:
        g_value_set_uint64 (value, self->priv->date);
        break;
      case PROP_URI:
        g_value_set_string (value,
            self->priv->uri != NULL ? self->priv->uri : "");
        break;
      case PROP_SERVICE_NAME:
        g_value_set_string (value,
            self->priv->service_name != NULL ? self->priv->service_name : "");
        break;
      case PROP_METADATA:
        {
          /* We're fine with priv->metadata being NULL but dbus-glib
           * doesn't like iterating NULL as if it was a hash table. */
          if (self->priv->metadata == NULL)
            {
              g_value_take_boxed (value,
                  g_hash_table_new (g_str_hash, g_str_equal));
            }
          else
            {
              g_value_set_boxed (value, self->priv->metadata);
            }
        }
        break;
      case PROP_CHANNEL_DESTROYED:
        g_value_set_boolean (value, self->priv->closed);
        break;
      case PROP_CHANNEL_PROPERTIES:
        {
          GHashTable *props;

          props = tp_dbus_properties_mixin_make_properties_hash (object,
              TP_IFACE_CHANNEL, "ChannelType",
              TP_IFACE_CHANNEL, "Interfaces",
              TP_IFACE_CHANNEL, "TargetHandle",
              TP_IFACE_CHANNEL, "TargetID",
              TP_IFACE_CHANNEL, "TargetHandleType",
              TP_IFACE_CHANNEL, "Requested",
              TP_IFACE_CHANNEL, "InitiatorHandle",
              TP_IFACE_CHANNEL, "InitiatorID",
              TP_IFACE_CHANNEL_TYPE_FILE_TRANSFER, "State",
              TP_IFACE_CHANNEL_TYPE_FILE_TRANSFER, "ContentType",
              TP_IFACE_CHANNEL_TYPE_FILE_TRANSFER, "Filename",
              TP_IFACE_CHANNEL_TYPE_FILE_TRANSFER, "Size",
              TP_IFACE_CHANNEL_TYPE_FILE_TRANSFER, "ContentHashType",
              TP_IFACE_CHANNEL_TYPE_FILE_TRANSFER, "ContentHash",
              TP_IFACE_CHANNEL_TYPE_FILE_TRANSFER, "Description",
              TP_IFACE_CHANNEL_TYPE_FILE_TRANSFER, "Date",
              TP_IFACE_CHANNEL_TYPE_FILE_TRANSFER, "AvailableSocketTypes",
              TP_IFACE_CHANNEL_TYPE_FILE_TRANSFER, "TransferredBytes",
              TP_IFACE_CHANNEL_TYPE_FILE_TRANSFER, "InitialOffset",
              TP_IFACE_CHANNEL_INTERFACE_FILE_TRANSFER_METADATA, "ServiceName",
              TP_IFACE_CHANNEL_INTERFACE_FILE_TRANSFER_METADATA, "Metadata",
              NULL);

          /* URI is immutable only for outgoing transfers */
          if (self->priv->initiator == base_conn->self_handle)
            {
              tp_dbus_properties_mixin_fill_properties_hash (object, props,
                  TP_IFACE_CHANNEL_TYPE_FILE_TRANSFER, "URI", NULL);
            }

          g_value_take_boxed (value, props);
        }
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
salut_file_transfer_channel_set_property (GObject *object,
                                          guint property_id,
                                          const GValue *value,
                                          GParamSpec *pspec)
{
  SalutFileTransferChannel *self = SALUT_FILE_TRANSFER_CHANNEL (object);

  switch (property_id)
    {
      case PROP_OBJECT_PATH:
        g_free (self->priv->object_path);
        self->priv->object_path = g_value_dup_string (value);
        break;
      case PROP_HANDLE:
        self->priv->handle = g_value_get_uint (value);
        break;
      case PROP_CONTACT:
        self->priv->contact = g_value_dup_object (value);
        g_signal_connect (self->priv->contact, "lost",
            G_CALLBACK (contact_lost_cb), self);
        break;
      case PROP_CONNECTION:
        self->priv->connection = g_value_get_object (value);
        break;
      case PROP_HANDLE_TYPE:
        /* 0 is the old tp-glib value of unset, TP_UNKNOWN_HANDLE_TYPE is the
         * new version */
        g_assert (g_value_get_uint (value) == 0
                  || g_value_get_uint (value) == TP_HANDLE_TYPE_CONTACT
                  || g_value_get_uint (value) == TP_UNKNOWN_HANDLE_TYPE);
        break;
      case PROP_CHANNEL_TYPE:
        /* these properties are writable in the interface, but not actually
         * meaningfully changeable on this channel, so we do nothing */
        break;
      case PROP_STATE:
        salut_file_transfer_channel_set_state (
            TP_SVC_CHANNEL_TYPE_FILE_TRANSFER (object),
            g_value_get_uint (value),
            TP_FILE_TRANSFER_STATE_CHANGE_REASON_NONE);
        break;
      case PROP_CONTENT_TYPE:
        g_free (self->priv->content_type);
        self->priv->content_type = g_value_dup_string (value);
        break;
      case PROP_FILENAME:
        g_free (self->priv->filename);
        self->priv->filename = g_value_dup_string (value);
        break;
      case PROP_SIZE:
        self->priv->size = g_value_get_uint64 (value);
        break;
      case PROP_CONTENT_HASH_TYPE:
        self->priv->content_hash_type = g_value_get_uint (value);
        break;
      case PROP_CONTENT_HASH:
        g_free (self->priv->content_hash);
        self->priv->content_hash = g_value_dup_string (value);
        break;
      case PROP_DESCRIPTION:
        g_free (self->priv->description);
        self->priv->description = g_value_dup_string (value);
        break;
      case PROP_INITIATOR_HANDLE:
        self->priv->initiator = g_value_get_uint (value);
        g_assert (self->priv->initiator != 0);
        break;
      case PROP_DATE:
        self->priv->date = g_value_get_uint64 (value);
        break;
      case PROP_INITIAL_OFFSET:
        self->priv->initial_offset = g_value_get_uint64 (value);
        break;
      case PROP_URI:
        g_assert (self->priv->uri == NULL); /* construct only */
        self->priv->uri = g_value_dup_string (value);
        break;
      case PROP_SERVICE_NAME:
        g_assert (self->priv->service_name == NULL); /* construct only */
        self->priv->service_name = g_value_dup_string (value);
        break;
      case PROP_METADATA:
        self->priv->metadata = g_value_dup_boxed (value);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
free_array (GArray *array)
{
  g_array_unref (array);
}

static GObject *
salut_file_transfer_channel_constructor (GType type,
                                         guint n_props,
                                         GObjectConstructParam *props)
{
  GObject *obj;
  SalutFileTransferChannel *self;
  TpDBusDaemon *bus;
  TpBaseConnection *base_conn;
  TpHandleRepoIface *contact_repo;
  GArray *unix_access;
  TpSocketAccessControl access_control;

  /* Parent constructor chain */
  obj = G_OBJECT_CLASS (salut_file_transfer_channel_parent_class)->
          constructor (type, n_props, props);

  self = SALUT_FILE_TRANSFER_CHANNEL (obj);

  /* Ref our handle */
  base_conn = TP_BASE_CONNECTION (self->priv->connection);

  /* ref our porter */
  wocky_meta_porter_hold (WOCKY_META_PORTER (self->priv->connection->porter),
      WOCKY_CONTACT (self->priv->contact));

  contact_repo = tp_base_connection_get_handles (base_conn,
      TP_HANDLE_TYPE_CONTACT);

  tp_handle_ref (contact_repo, self->priv->handle);

  self->priv->object_path = g_strdup_printf ("%s/FileTransferChannel/%p",
      base_conn->object_path, self);

  /* Connect to the bus */
  bus = tp_base_connection_get_dbus_daemon (base_conn);
  tp_dbus_daemon_register_object (bus, self->priv->object_path, obj);

  /* Initialise the available socket types hash table */
  self->priv->available_socket_types = g_hash_table_new_full (g_direct_hash,
      g_direct_equal, NULL, (GDestroyNotify) free_array);

  /* Socket_Address_Type_Unix */
  unix_access = g_array_sized_new (FALSE, FALSE, sizeof (TpSocketAccessControl),
      1);
  access_control = TP_SOCKET_ACCESS_CONTROL_LOCALHOST;
  g_array_append_val (unix_access, access_control);
  g_hash_table_insert (self->priv->available_socket_types,
      GUINT_TO_POINTER (TP_SOCKET_ADDRESS_TYPE_UNIX), unix_access);

  DEBUG ("New FT channel created: %s (contact: %s, initiator: %s, "
       "file: \"%s\", size: %" G_GUINT64_FORMAT ")",
       self->priv->object_path,
       tp_handle_inspect (contact_repo, self->priv->handle),
       tp_handle_inspect (contact_repo, self->priv->initiator),
       self->priv->filename, self->priv->size);

  if (self->priv->initiator != base_conn->self_handle)
    /* Incoming transfer, URI has to be set by the handler */
    g_assert (self->priv->uri == NULL);

  return obj;
}

static void
salut_file_transfer_channel_dispose (GObject *object);
static void
salut_file_transfer_channel_finalize (GObject *object);

static gboolean
file_transfer_channel_properties_setter (GObject *object,
    GQuark interface,
    GQuark name,
    const GValue *value,
    gpointer setter_data,
    GError **error)
{
  SalutFileTransferChannel *self = (SalutFileTransferChannel *) object;
  TpBaseConnection *base_conn = TP_BASE_CONNECTION (self->priv->connection);

  g_return_val_if_fail (interface == TP_IFACE_QUARK_CHANNEL_TYPE_FILE_TRANSFER,
      FALSE);

  /* There is only one property with write access. So TpDBusPropertiesMixin
   * already checked this. */
  g_assert (name == g_quark_from_static_string ("URI"));

  /* TpDBusPropertiesMixin already checked this */
  g_assert (G_VALUE_HOLDS_STRING (value));

  if (self->priv->uri != NULL)
    {
      g_set_error (error, TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
          "URI has already be set");
      return FALSE;
    }

  if (self->priv->initiator == base_conn->self_handle)
    {
      g_set_error (error, TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
          "Channel is not an incoming transfer");
      return FALSE;
    }

  if (self->priv->state != TP_FILE_TRANSFER_STATE_PENDING)
    {
      g_set_error (error, TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
        "State is not pending; cannot set URI");
      return FALSE;
    }

  self->priv->uri = g_value_dup_string (value);

  tp_svc_channel_type_file_transfer_emit_uri_defined (self, self->priv->uri);

  return TRUE;
}

static void
salut_file_transfer_channel_class_init (
    SalutFileTransferChannelClass *salut_file_transfer_channel_class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (
      salut_file_transfer_channel_class);
  GParamSpec *param_spec;

  static TpDBusPropertiesMixinPropImpl channel_props[] = {
    { "TargetHandleType", "handle-type", NULL },
    { "TargetHandle", "handle", NULL },
    { "TargetID", "target-id", NULL },
    { "ChannelType", "channel-type", NULL },
    { "Interfaces", "interfaces", NULL },
    { "Requested", "requested", NULL },
    { "InitiatorHandle", "initiator-handle", NULL },
    { "InitiatorID", "initiator-id", NULL },
    { NULL }
  };

  static TpDBusPropertiesMixinPropImpl file_props[] = {
    { "State", "state", NULL },
    { "ContentType", "content-type", "content-type" },
    { "Filename", "filename", "filename" },
    { "Size", "size", "size" },
    { "ContentHashType", "content-hash-type", "content-hash-type" },
    { "ContentHash", "content-hash", "content-hash" },
    { "Description", "description", "description" },
    { "AvailableSocketTypes", "available-socket-types", NULL },
    { "TransferredBytes", "transferred-bytes", NULL },
    { "InitialOffset", "initial-offset", NULL },
    { "Date", "date", "date" },
    { "URI", "uri", NULL },
    { NULL }
  };

  static TpDBusPropertiesMixinPropImpl file_metadata_props[] = {
    { "ServiceName", "service-name", NULL },
    { "Metadata", "metadata", NULL },
    { NULL }
  };

  static TpDBusPropertiesMixinIfaceImpl prop_interfaces[] = {
    { TP_IFACE_CHANNEL,
      tp_dbus_properties_mixin_getter_gobject_properties,
      NULL,
      channel_props
    },
    { TP_IFACE_CHANNEL_TYPE_FILE_TRANSFER,
      tp_dbus_properties_mixin_getter_gobject_properties,
      file_transfer_channel_properties_setter,
      file_props
    },
    { TP_IFACE_CHANNEL_INTERFACE_FILE_TRANSFER_METADATA,
      tp_dbus_properties_mixin_getter_gobject_properties,
      NULL,
      file_metadata_props
    },    { NULL }
  };

  g_type_class_add_private (salut_file_transfer_channel_class,
      sizeof (SalutFileTransferChannelPrivate));

  object_class->dispose = salut_file_transfer_channel_dispose;
  object_class->finalize = salut_file_transfer_channel_finalize;

  object_class->constructor = salut_file_transfer_channel_constructor;
  object_class->get_property = salut_file_transfer_channel_get_property;
  object_class->set_property = salut_file_transfer_channel_set_property;

  g_object_class_override_property (object_class, PROP_OBJECT_PATH,
      "object-path");
  g_object_class_override_property (object_class, PROP_CHANNEL_TYPE,
      "channel-type");
  g_object_class_override_property (object_class, PROP_HANDLE_TYPE,
      "handle-type");
  g_object_class_override_property (object_class, PROP_HANDLE, "handle");
  g_object_class_override_property (object_class, PROP_CHANNEL_DESTROYED,
      "channel-destroyed");
  g_object_class_override_property (object_class, PROP_CHANNEL_PROPERTIES,
      "channel-properties");

  param_spec = g_param_spec_string ("target-id", "Target JID",
      "The string obtained by inspecting this channel's handle",
      NULL,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_TARGET_ID, param_spec);

  param_spec = g_param_spec_boolean ("requested", "Requested?",
      "True if this channel was requested by the local user",
      FALSE,
      G_PARAM_READABLE |
      G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB | G_PARAM_STATIC_NAME);
  g_object_class_install_property (object_class, PROP_REQUESTED, param_spec);

 param_spec = g_param_spec_uint ("initiator-handle", "Initiator's handle",
      "The contact who initiated the channel",
      0, G_MAXUINT32, 0,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE |
      G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB | G_PARAM_STATIC_NAME);
  g_object_class_install_property (object_class, PROP_INITIATOR_HANDLE,
      param_spec);

  param_spec = g_param_spec_string ("initiator-id", "Initiator's bare JID",
      "The string obtained by inspecting the initiator-handle",
      NULL,
      G_PARAM_READABLE |
      G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB | G_PARAM_STATIC_NAME);
  g_object_class_install_property (object_class, PROP_INITIATOR_ID,
      param_spec);

  param_spec = g_param_spec_object ("contact",
      "SalutContact object",
      "Salut Contact to which this channel is dedicated",
      SALUT_TYPE_CONTACT,
      G_PARAM_CONSTRUCT_ONLY |
      G_PARAM_READWRITE |
      G_PARAM_STATIC_NICK |
      G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_CONTACT, param_spec);

  param_spec = g_param_spec_object ("connection",
      "SalutConnection object",
      "Salut Connection that owns the"
      "connection for this IM channel",
      SALUT_TYPE_CONNECTION,
      G_PARAM_CONSTRUCT_ONLY |
      G_PARAM_READWRITE |
      G_PARAM_STATIC_NICK |
      G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_CONNECTION, param_spec);

  param_spec = g_param_spec_boxed ("interfaces", "Extra D-Bus interfaces",
      "Additional Channel.Interface.* interfaces",
      G_TYPE_STRV,
      G_PARAM_READABLE |
      G_PARAM_STATIC_NICK |
      G_PARAM_STATIC_BLURB |
      G_PARAM_STATIC_NAME);
  g_object_class_install_property (object_class, PROP_INTERFACES, param_spec);

  param_spec = g_param_spec_uint (
      "state",
      "TpFileTransferState state",
      "State of the file transfer in this channel",
      0,
      G_MAXUINT,
      0,
      G_PARAM_CONSTRUCT_ONLY |
      G_PARAM_READWRITE |
      G_PARAM_STATIC_NICK |
      G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_STATE, param_spec);

  param_spec = g_param_spec_string (
      "content-type",
      "gchar *content-type",
      "ContentType of the file",
      "application/octet-stream",
      G_PARAM_CONSTRUCT_ONLY |
      G_PARAM_READWRITE |
      G_PARAM_STATIC_NICK |
      G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_CONTENT_TYPE,
      param_spec);

  param_spec = g_param_spec_string (
      "filename",
      "gchar *filename",
      "Name of the file",
      "",
      G_PARAM_CONSTRUCT_ONLY |
      G_PARAM_READWRITE |
      G_PARAM_STATIC_NICK |
      G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_FILENAME, param_spec);

  param_spec = g_param_spec_uint64 (
      "size",
      "guint size",
      "Size of the file in bytes",
      0,
      G_MAXUINT64,
      SALUT_UNDEFINED_FILE_SIZE,
      G_PARAM_CONSTRUCT_ONLY |
      G_PARAM_READWRITE |
      G_PARAM_STATIC_NICK |
      G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_SIZE, param_spec);

  param_spec = g_param_spec_uint (
      "content-hash-type",
      "SalutFileHashType content-hash-type",
      "Hash type",
      0,
      G_MAXUINT,
      TP_FILE_HASH_TYPE_NONE,
      G_PARAM_READWRITE |
      G_PARAM_CONSTRUCT_ONLY |
      G_PARAM_STATIC_NICK |
      G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_CONTENT_HASH_TYPE,
      param_spec);

  param_spec = g_param_spec_string (
      "content-hash",
      "gchar *content-hash",
      "Hash of the file contents",
      "",
      G_PARAM_CONSTRUCT_ONLY |
      G_PARAM_READWRITE |
      G_PARAM_STATIC_NICK |
      G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_CONTENT_HASH,
      param_spec);

  param_spec = g_param_spec_string (
      "description",
      "gchar *description",
      "Description of the file",
      "",
      G_PARAM_CONSTRUCT_ONLY |
      G_PARAM_READWRITE |
      G_PARAM_STATIC_NICK |
      G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_DESCRIPTION, param_spec);

  param_spec = g_param_spec_boxed (
      "available-socket-types",
      "SalutSupportedSocketMap available-socket-types",
      "Available socket types",
      dbus_g_type_get_map ("GHashTable", G_TYPE_UINT, DBUS_TYPE_G_UINT_ARRAY),
      G_PARAM_READABLE |
      G_PARAM_STATIC_NICK |
      G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_AVAILABLE_SOCKET_TYPES,
      param_spec);

  param_spec = g_param_spec_uint64 (
      "transferred-bytes",
      "guint64 transferred-bytes",
      "Bytes transferred",
      0,
      G_MAXUINT64,
      0,
      G_PARAM_READABLE |
      G_PARAM_STATIC_NICK |
      G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_TRANSFERRED_BYTES,
      param_spec);

  param_spec = g_param_spec_uint64 (
      "initial-offset",
      "guint64 initial_offset",
      "Offset set at the beginning of the transfer",
      0,
      G_MAXUINT64,
      0,
      G_PARAM_READWRITE |
      G_PARAM_STATIC_NICK |
      G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_INITIAL_OFFSET,
      param_spec);

  param_spec = g_param_spec_uint64 (
      "date",
      "Epoch time",
      "the last modification time of the file being transferred",
      0,
      G_MAXUINT64,
      0,
      G_PARAM_CONSTRUCT_ONLY |
      G_PARAM_READWRITE |
      G_PARAM_STATIC_NICK |
      G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_DATE,
      param_spec);

  param_spec = g_param_spec_string (
      "uri", "URI",
      "URI of the file being transferred",
      NULL,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_URI,
      param_spec);

  param_spec = g_param_spec_string ("service-name",
      "ServiceName",
      "The Metadata.ServiceName property of this channel",
      "",
      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_SERVICE_NAME,
      param_spec);

  param_spec = g_param_spec_boxed ("metadata",
      "Metadata",
      "The Metadata.Metadata property of this channel",
      TP_HASH_TYPE_METADATA,
      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_METADATA,
      param_spec);

  salut_file_transfer_channel_class->dbus_props_class.interfaces = \
      prop_interfaces;
  tp_dbus_properties_mixin_class_init (object_class,
      G_STRUCT_OFFSET (SalutFileTransferChannelClass, dbus_props_class));
}

void
salut_file_transfer_channel_dispose (GObject *object)
{
  SalutFileTransferChannel *self = SALUT_FILE_TRANSFER_CHANNEL (object);
  TpBaseConnection *base_conn = TP_BASE_CONNECTION (self->priv->connection);
  TpHandleRepoIface *handle_repo = tp_base_connection_get_handles (base_conn,
      TP_HANDLE_TYPE_CONTACT);

  if (self->priv->dispose_has_run)
    return;

  self->priv->dispose_has_run = TRUE;

  tp_handle_unref (handle_repo, self->priv->handle);

  salut_file_transfer_channel_do_close (self);

  if (self->priv->progress_timer != 0)
    {
      g_source_remove (self->priv->progress_timer);
      self->priv->progress_timer = 0;
    }

  if (self->priv->contact)
    {
      g_signal_handlers_disconnect_by_func (self->priv->contact,
          contact_lost_cb, self);
      g_object_unref (self->priv->contact);
      self->priv->contact = NULL;
    }

  if (self->priv->ft != NULL)
    {
      g_object_unref (self->priv->ft);
      self->priv->ft = NULL;
    }

  /* release any references held by the object here */

  if (G_OBJECT_CLASS (salut_file_transfer_channel_parent_class)->dispose)
    G_OBJECT_CLASS (salut_file_transfer_channel_parent_class)->dispose (object);
}

static void
salut_file_transfer_channel_finalize (GObject *object)
{
  SalutFileTransferChannel *self = SALUT_FILE_TRANSFER_CHANNEL (object);

  /* free any data held directly by the object here */
  g_free (self->priv->object_path);
  g_free (self->priv->filename);
  if (self->priv->socket_address != NULL)
    tp_g_value_slice_free (self->priv->socket_address);
  g_free (self->priv->content_type);
  g_free (self->priv->content_hash);
  g_free (self->priv->description);
  g_hash_table_unref (self->priv->available_socket_types);
  g_free (self->priv->uri);
  g_free (self->priv->service_name);
  if (self->priv->metadata != NULL)
    g_hash_table_unref (self->priv->metadata);

  if (self->priv->path != NULL)
    {
      g_unlink (self->priv->path);
      g_free (self->priv->path);
    }

  G_OBJECT_CLASS (salut_file_transfer_channel_parent_class)->finalize (object);
}


/**
 * salut_file_transfer_channel_close
 *
 * Implements DBus method Close
 * on interface org.freedesktop.Telepathy.Channel
 */
static void
salut_file_transfer_channel_close (TpSvcChannel *iface,
                                   DBusGMethodInvocation *context)
{
  SalutFileTransferChannel *self = SALUT_FILE_TRANSFER_CHANNEL (iface);

  if (self->priv->state != TP_FILE_TRANSFER_STATE_COMPLETED &&
      self->priv->state != TP_FILE_TRANSFER_STATE_CANCELLED)
    {
      gibber_file_transfer_cancel (self->priv->ft, 406);
      salut_file_transfer_channel_set_state (
          TP_SVC_CHANNEL_TYPE_FILE_TRANSFER (iface),
          TP_FILE_TRANSFER_STATE_CANCELLED,
          TP_FILE_TRANSFER_STATE_CHANGE_REASON_LOCAL_STOPPED);
    }

  salut_file_transfer_channel_do_close (SALUT_FILE_TRANSFER_CHANNEL (iface));
  tp_svc_channel_return_from_close (context);
}

/**
 * salut_file_transfer_channel_get_channel_type
 *
 * Implements DBus method GetChannelType
 * on interface org.freedesktop.Telepathy.Channel
 */
static void
salut_file_transfer_channel_get_channel_type (TpSvcChannel *iface,
                                              DBusGMethodInvocation *context)
{
  tp_svc_channel_return_from_get_channel_type (context,
      TP_IFACE_CHANNEL_TYPE_FILE_TRANSFER);
}

/**
 * salut_file_transfer_channel_get_handle
 *
 * Implements DBus method GetHandle
 * on interface org.freedesktop.Telepathy.Channel
 */
static void
salut_file_transfer_channel_get_handle (TpSvcChannel *iface,
                                        DBusGMethodInvocation *context)
{
  SalutFileTransferChannel *self = SALUT_FILE_TRANSFER_CHANNEL (iface);

  tp_svc_channel_return_from_get_handle (context, TP_HANDLE_TYPE_CONTACT,
                                         self->priv->handle);
}

/**
 * salut_file_transfer_channel_get_interfaces
 *
 * Implements DBus method GetInterfaces
 * on interface org.freedesktop.Telepathy.Channel
 */
static void
salut_file_transfer_channel_get_interfaces (TpSvcChannel *iface,
                                            DBusGMethodInvocation *context)
{
  tp_svc_channel_return_from_get_interfaces (context,
      salut_file_transfer_channel_interfaces);
}

static void
channel_iface_init (gpointer g_iface, gpointer iface_data)
{
  TpSvcChannelClass *klass = (TpSvcChannelClass *) g_iface;

#define IMPLEMENT(x) tp_svc_channel_implement_##x (\
    klass, salut_file_transfer_channel_##x)
  IMPLEMENT (close);
  IMPLEMENT (get_channel_type);
  IMPLEMENT (get_handle);
  IMPLEMENT (get_interfaces);
#undef IMPLEMENT
}

static void
error_cb (GibberFileTransfer *ft,
          guint domain,
          gint code,
          const gchar *message,
          SalutFileTransferChannel *self)
{
  TpBaseConnection *base_conn = (TpBaseConnection *) self->priv->connection;
  gboolean receiver;

  receiver = (self->priv->initiator != base_conn->self_handle);

  if (domain == GIBBER_FILE_TRANSFER_ERROR && code ==
      GIBBER_FILE_TRANSFER_ERROR_NOT_FOUND && receiver)
    {
      /* Inform the sender we weren't able to retrieve the file */
      gibber_file_transfer_cancel (self->priv->ft, 404);
    }

  salut_file_transfer_channel_set_state (
      TP_SVC_CHANNEL_TYPE_FILE_TRANSFER (self),
      TP_FILE_TRANSFER_STATE_CANCELLED,
      receiver ?
      TP_FILE_TRANSFER_STATE_CHANGE_REASON_LOCAL_ERROR :
      TP_FILE_TRANSFER_STATE_CHANGE_REASON_REMOTE_ERROR);
}

static void
ft_finished_cb (GibberFileTransfer *ft,
                SalutFileTransferChannel *self)
{
  SalutFileTransferChannelPrivate *priv = self->priv;
  WockyPorter *porter = priv->connection->porter;

  salut_file_transfer_channel_set_state (
      TP_SVC_CHANNEL_TYPE_FILE_TRANSFER (self),
      TP_FILE_TRANSFER_STATE_COMPLETED,
      TP_FILE_TRANSFER_STATE_CHANGE_REASON_NONE);

  wocky_meta_porter_unhold (WOCKY_META_PORTER (porter),
      WOCKY_CONTACT (self->priv->contact));
}

static void
ft_remote_cancelled_cb (GibberFileTransfer *ft,
                        SalutFileTransferChannel *self)
{
  SalutFileTransferChannelPrivate *priv = self->priv;
  WockyPorter *porter = priv->connection->porter;

  gibber_file_transfer_cancel (ft, 406);
  salut_file_transfer_channel_set_state (
      TP_SVC_CHANNEL_TYPE_FILE_TRANSFER (self),
      TP_FILE_TRANSFER_STATE_CANCELLED,
      TP_FILE_TRANSFER_STATE_CHANGE_REASON_REMOTE_STOPPED);

  wocky_meta_porter_unhold (WOCKY_META_PORTER (porter),
      WOCKY_CONTACT (self->priv->contact));
}

static void
remote_accepted_cb (GibberFileTransfer *ft,
                    SalutFileTransferChannel *self)
{
  self->priv->remote_accepted = TRUE;

  if (self->priv->socket_address != NULL)
    {
      /* ProvideFile has already been called. Channel is Open */
      tp_svc_channel_type_file_transfer_emit_initial_offset_defined (self,
          self->priv->initial_offset);

      salut_file_transfer_channel_set_state (
          TP_SVC_CHANNEL_TYPE_FILE_TRANSFER (self),
          TP_FILE_TRANSFER_STATE_OPEN,
          TP_FILE_TRANSFER_STATE_CHANGE_REASON_NONE);
    }
  else
    {
      /* Client has to call ProvideFile to open the channel */
      salut_file_transfer_channel_set_state (
          TP_SVC_CHANNEL_TYPE_FILE_TRANSFER (self),
          TP_FILE_TRANSFER_STATE_ACCEPTED,
          TP_FILE_TRANSFER_STATE_CHANGE_REASON_NONE);
    }

  g_signal_connect (ft, "finished", G_CALLBACK (ft_finished_cb), self);
}

static gboolean setup_local_socket (SalutFileTransferChannel *self);
static void ft_transferred_chunk_cb (GibberFileTransfer *ft, guint64 count,
    SalutFileTransferChannel *self);

static GList *
add_metadata_forms (SalutFileTransferChannel *self,
    GibberFileTransfer *ft)
{
  GError *error = NULL;
  GQueue queue = G_QUEUE_INIT;

  if (!tp_str_empty (self->priv->service_name))
    {
      WockyStanza *tmp = wocky_stanza_build (WOCKY_STANZA_TYPE_IQ,
          WOCKY_STANZA_SUB_TYPE_RESULT, NULL, NULL,
          '(', "x",
            ':', WOCKY_XMPP_NS_DATA,
            '@', "type", "result",
            '(', "field",
              '@', "var", "FORM_TYPE",
              '@', "type", "hidden",
              '(', "value",
                '$', NS_TP_FT_METADATA_SERVICE,
              ')',
            ')',
            '(', "field",
              '@', "var", "ServiceName",
              '(', "value",
                '$', self->priv->service_name,
              ')',
            ')',
          ')',
          NULL);
      WockyNode *x = wocky_node_get_first_child (wocky_stanza_get_top_node (tmp));
      WockyDataForm *form = wocky_data_form_new_from_node (x, &error);

      if (form == NULL)
        {
          DEBUG ("Failed to parse form (wat): %s", error->message);
          g_clear_error (&error);
        }
      else
        {
          g_queue_push_tail (&queue, form);
        }

      g_object_unref (tmp);
    }

  if (self->priv->metadata != NULL
      && g_hash_table_size (self->priv->metadata) > 0)
    {
      WockyStanza *tmp = wocky_stanza_build (WOCKY_STANZA_TYPE_IQ,
          WOCKY_STANZA_SUB_TYPE_RESULT, NULL, NULL,
          '(', "x",
            ':', WOCKY_XMPP_NS_DATA,
            '@', "type", "result",
            '(', "field",
              '@', "var", "FORM_TYPE",
              '@', "type", "hidden",
              '(', "value",
                '$', NS_TP_FT_METADATA,
              ')',
            ')',
          ')',
          NULL);
      WockyNode *x = wocky_node_get_first_child (wocky_stanza_get_top_node (tmp));
      WockyDataForm *form;
      GHashTableIter iter;
      gpointer key, val;

      g_hash_table_iter_init (&iter, self->priv->metadata);
      while (g_hash_table_iter_next (&iter, &key, &val))
        {
          const gchar * const *values = val;

          WockyNode *field = wocky_node_add_child (x, "field");
          wocky_node_set_attribute (field, "var", key);

          for (; values != NULL && *values != NULL; values++)
            {
              wocky_node_add_child_with_content (field, "value", *values);
            }
        }

      form = wocky_data_form_new_from_node (x, &error);

      if (form == NULL)
        {
          DEBUG ("Failed to parse form (wat): %s", error->message);
          g_clear_error (&error);
        }
      else
        {
          g_queue_push_tail (&queue, form);
        }

      g_object_unref (tmp);
    }

  return queue.head;
}

static void
send_file_offer (SalutFileTransferChannel *self)
{
  GibberFileTransfer *ft;

  ft = g_object_new (GIBBER_TYPE_OOB_FILE_TRANSFER,
      "self-id", self->priv->connection->name,
      "peer-id", self->priv->contact->name,
      "filename", self->priv->filename,
      "porter", self->priv->connection->porter,
      "contact", self->priv->contact,
      "description", self->priv->description,
      "content-type", self->priv->content_type,
      NULL);

  g_signal_connect (ft, "remote-accepted",
      G_CALLBACK (remote_accepted_cb), self);
  g_signal_connect (ft, "error", G_CALLBACK (error_cb), self);
  g_signal_connect (ft, "cancelled", G_CALLBACK (ft_remote_cancelled_cb), self);

  self->priv->ft = ft;

  g_signal_connect (ft, "transferred-chunk",
      G_CALLBACK (ft_transferred_chunk_cb), self);

  gibber_file_transfer_set_size (ft, self->priv->size);

  g_assert (ft->dataforms == NULL);
  ft->dataforms = add_metadata_forms (self, ft);

  gibber_file_transfer_offer (ft);
}

static void
salut_file_transfer_channel_set_state (
    TpSvcChannelTypeFileTransfer *iface,
    TpFileTransferState state,
    TpFileTransferStateChangeReason reason)
{
  SalutFileTransferChannel *self = SALUT_FILE_TRANSFER_CHANNEL (iface);

  if (self->priv->state == state)
    return;

  self->priv->state = state;
  tp_svc_channel_type_file_transfer_emit_file_transfer_state_changed (iface,
      state, reason);
}

static void
emit_progress_update (SalutFileTransferChannel *self)
{
  TpSvcChannelTypeFileTransfer *iface = \
      TP_SVC_CHANNEL_TYPE_FILE_TRANSFER (self);

  g_get_current_time (&self->priv->last_transferred_bytes_emitted);

  tp_svc_channel_type_file_transfer_emit_transferred_bytes_changed (
    iface, self->priv->transferred_bytes);

  if (self->priv->progress_timer != 0)
    {
      g_source_remove (self->priv->progress_timer);
      self->priv->progress_timer = 0;
    }
}

static gboolean
emit_progress_update_cb (gpointer user_data)
{
  SalutFileTransferChannel *self = \
      SALUT_FILE_TRANSFER_CHANNEL (user_data);

  emit_progress_update (self);

  return FALSE;
}

static void
ft_transferred_chunk_cb (GibberFileTransfer *ft,
                         guint64 count,
                         SalutFileTransferChannel *self)
{
  GTimeVal timeval;
  gint interval;

  self->priv->transferred_bytes += count;

  if (self->priv->transferred_bytes >= self->priv->size)
    {
      /* If the transfer has finished send an update right away */
      emit_progress_update (self);
      return;
    }

  if (self->priv->progress_timer != 0)
    {
      /* A progress update signal is already scheduled */
      return;
    }

  /* Only emit the TransferredBytes signal if it has been one second since its
   * last emission.
   */
  g_get_current_time (&timeval);
  interval = timeval.tv_sec -
    self->priv->last_transferred_bytes_emitted.tv_sec;

  if (interval > 1)
    {
      /* At least more then a second apart, emit right away */
      emit_progress_update (self);
      return;
    }

  /* Convert interval to milliseconds and calculate it more precisely */
  interval *= 1000;

  interval += (timeval.tv_usec -
    self->priv->last_transferred_bytes_emitted.tv_usec)/1000;

  /* Protect against clock skew, if the interval is negative the worst thing
   * that can happen is that we wait an extra second before emitting the signal
   */
  interval = ABS(interval);

  if (interval > 1000)
    emit_progress_update (self);
  else
    self->priv->progress_timer = g_timeout_add (1000 - interval,
       emit_progress_update_cb, self);
}

static gboolean
check_address_and_access_control (SalutFileTransferChannel *self,
                                  TpSocketAddressType address_type,
                                  TpSocketAccessControl access_control,
                                  const GValue *access_control_param,
                                  GError **error)
{
  GArray *access_arr;
  guint i;

  /* Do we support this AddressType? */
  access_arr = g_hash_table_lookup (self->priv->available_socket_types,
      GUINT_TO_POINTER (address_type));
  if (access_arr == NULL)
    {
      g_set_error (error, TP_ERRORS, TP_ERROR_NOT_IMPLEMENTED,
          "AddressType %u is not implemented", address_type);
      return FALSE;
    }

  /* Do we support this AccessControl? */
  for (i = 0; i < access_arr->len; i++)
    {
      TpSocketAccessControl control;

      control = g_array_index (access_arr, TpSocketAccessControl, i);
      if (control == access_control)
        return TRUE;
    }

  g_set_error (error, TP_ERRORS, TP_ERROR_NOT_IMPLEMENTED,
      "AccesControl %u is not implemented with AddressType %u",
      access_control, address_type);

  return FALSE;
}

gboolean
salut_file_transfer_channel_offer_file (SalutFileTransferChannel *self,
                                        GError **error)
{
  g_assert (!CHECK_STR_EMPTY (self->priv->filename));
  g_assert (self->priv->size != SALUT_UNDEFINED_FILE_SIZE);

  DEBUG ("Offering file transfer");

  send_file_offer (self);

  return TRUE;
}

/**
 * salut_file_transfer_channel_accept_file
 *
 * Implements D-Bus method AcceptFile
 * on interface org.freedesktop.Telepathy.Channel.Type.FileTransfer
 */
static void
salut_file_transfer_channel_accept_file (TpSvcChannelTypeFileTransfer *iface,
                                         guint address_type,
                                         guint access_control,
                                         const GValue *access_control_param,
                                         guint64 offset,
                                         DBusGMethodInvocation *context)
{
  SalutFileTransferChannel *self = SALUT_FILE_TRANSFER_CHANNEL (iface);
  GError *error = NULL;
  GibberFileTransfer *ft;

  ft = self->priv->ft;
  if (ft == NULL)
    {
      dbus_g_method_return_error (context, error);
      g_error_free (error);
    }

  if (self->priv->state != TP_FILE_TRANSFER_STATE_PENDING)
    {
      g_set_error (&error, TP_ERRORS, TP_ERROR_NOT_AVAILABLE,
        "State is not pending; cannot accept file");
      dbus_g_method_return_error (context, error);
      return;
    }

  if (!check_address_and_access_control (self, address_type, access_control,
        access_control_param, &error))
    {
      dbus_g_method_return_error (context, error);
      g_error_free (error);
      return;
    }

  g_signal_connect (ft, "finished", G_CALLBACK (ft_finished_cb), self);
  g_signal_connect (ft, "transferred-chunk",
      G_CALLBACK (ft_transferred_chunk_cb), self);
  g_signal_connect (ft, "cancelled", G_CALLBACK (ft_remote_cancelled_cb), self);

  if (!setup_local_socket (self))
    {
      DEBUG ("Could not set up local socket");
      g_set_error (&error, TP_ERRORS, TP_ERROR_NOT_AVAILABLE,
          "Could not set up local socket");
      dbus_g_method_return_error (context, error);
    }

  salut_file_transfer_channel_set_state (iface,
      TP_FILE_TRANSFER_STATE_ACCEPTED,
      TP_FILE_TRANSFER_STATE_CHANGE_REASON_REQUESTED);

  tp_svc_channel_type_file_transfer_return_from_accept_file (context,
      self->priv->socket_address);

  self->priv->initial_offset = 0;

  tp_svc_channel_type_file_transfer_emit_initial_offset_defined (self,
      self->priv->initial_offset);

  salut_file_transfer_channel_set_state (iface, TP_FILE_TRANSFER_STATE_OPEN,
      TP_FILE_TRANSFER_STATE_CHANGE_REASON_NONE);
}

/**
 * salut_file_transfer_channel_provide_file
 *
 * Implements D-Bus method ProvideFile
 * on interface org.freedesktop.Telepathy.Channel.Type.FileTransfer
 */
static void
salut_file_transfer_channel_provide_file (
    TpSvcChannelTypeFileTransfer *iface,
    guint address_type,
    guint access_control,
    const GValue *access_control_param,
    DBusGMethodInvocation *context)
{
  SalutFileTransferChannel *self = SALUT_FILE_TRANSFER_CHANNEL (iface);
  TpBaseConnection *base_conn = (TpBaseConnection *) self->priv->connection;
  GError *error = NULL;

  if (self->priv->initiator != base_conn->self_handle)
    {
      g_set_error (&error, TP_ERRORS, TP_ERROR_NOT_AVAILABLE,
          "Channel is not an outgoing transfer");
      dbus_g_method_return_error (context, error);
      return;
    }

  if (self->priv->socket_address != NULL)
    {
      g_set_error (&error, TP_ERRORS, TP_ERROR_NOT_AVAILABLE,
          "ProvideFile has already been called for this channel");
      dbus_g_method_return_error (context, error);
      return;
    }

  if (!check_address_and_access_control (self, address_type, access_control,
        access_control_param, &error))
    {
      dbus_g_method_return_error (context, error);
      g_error_free (error);
      return;
    }

  if (!setup_local_socket (self))
    {
      DEBUG ("Could not set up local socket");
      g_set_error (&error, TP_ERRORS, TP_ERROR_NOT_AVAILABLE,
          "Could not set up local socket");
      dbus_g_method_return_error (context, error);
    }

  if (self->priv->remote_accepted)
    {
      /* Remote already accepted the file. Channel is Open.
       * If not channel stay Pending. */
      tp_svc_channel_type_file_transfer_emit_initial_offset_defined (self,
          self->priv->initial_offset);

      salut_file_transfer_channel_set_state (iface,
          TP_FILE_TRANSFER_STATE_OPEN,
          TP_FILE_TRANSFER_STATE_CHANGE_REASON_REQUESTED);
    }

  tp_svc_channel_type_file_transfer_return_from_provide_file (context,
      self->priv->socket_address);
}

static void
file_transfer_iface_init (gpointer g_iface,
                          gpointer iface_data)
{
  TpSvcChannelTypeFileTransferClass *klass =
      (TpSvcChannelTypeFileTransferClass *) g_iface;

#define IMPLEMENT(x) tp_svc_channel_type_file_transfer_implement_##x (\
    klass, salut_file_transfer_channel_##x)
  IMPLEMENT (accept_file);
  IMPLEMENT (provide_file);
#undef IMPLEMENT
}

static gchar *
get_local_unix_socket_path (SalutFileTransferChannel *self)
{
  gchar *path = NULL;
  gint32 random_int;
  gchar *random_str;
  struct stat buf;

  while (TRUE)
    {
      random_int = g_random_int_range (0, G_MAXINT32);
      random_str = g_strdup_printf ("tp-ft-%i", random_int);
      path = g_build_filename (g_get_tmp_dir (), random_str, NULL);
      g_free (random_str);

      if (g_stat (path, &buf) != 0)
        break;

      g_free (path);
    }

  return path;
}

/*
 * Return a GIOChannel for the local unix socket path.
 */
static GIOChannel *
get_socket_channel (SalutFileTransferChannel *self)
{
  gint fd;
  gchar *path;
  size_t path_len;
  struct sockaddr_un addr;
  GIOChannel *io_channel;
  GArray *array;

  /* FIXME: should use the socket type and access control chosen by
   * the user. */
  path = get_local_unix_socket_path (self);

  array = g_array_sized_new (TRUE, FALSE, sizeof (gchar), strlen (path));
  g_array_insert_vals (array, 0, path, strlen (path));

  self->priv->socket_address = tp_g_value_slice_new (
          DBUS_TYPE_G_UCHAR_ARRAY);
  g_value_take_boxed (self->priv->socket_address, array);

  DEBUG ("local socket %s", path);

  fd = socket (PF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    {
      DEBUG("socket() failed");
      return NULL;
    }

  memset (&addr, 0, sizeof (addr));
  addr.sun_family = AF_UNIX;
  path_len = strlen (path);
  strncpy (addr.sun_path, path, path_len);
  g_unlink (path);

  /* save this so we can delete the actual socket later if it exists */
  self->priv->path = path;

  if (bind (fd, (struct sockaddr*) &addr,
        G_STRUCT_OFFSET (struct sockaddr_un, sun_path) + path_len) < 0)
    {
      DEBUG ("bind failed");
      close (fd);
      return NULL;
    }

  if (listen (fd, 1) < 0)
    {
      DEBUG ("listen failed");
      close (fd);
      return NULL;
    }

  io_channel = g_io_channel_unix_new (fd);
  g_io_channel_set_close_on_unref (io_channel, TRUE);
  return io_channel;
}

/*
 * Some client is connecting to the Unix socket.
 */
static gboolean
accept_local_socket_connection (GIOChannel *source,
                                GIOCondition condition,
                                gpointer user_data)
{
  GibberFileTransfer *ft;
  int new_fd;
  struct sockaddr_un addr;
  socklen_t addrlen;
  GIOChannel *channel;

  ft = SALUT_FILE_TRANSFER_CHANNEL (user_data)->priv->ft;

  g_assert (ft != NULL);

  if (condition & G_IO_IN)
    {
      DEBUG ("Client connected to local socket");

      addrlen = sizeof (addr);
      new_fd = accept (g_io_channel_unix_get_fd (source),
          (struct sockaddr *) &addr, &addrlen);
      if (new_fd < 0)
        {
          DEBUG ("accept() failed");
          return FALSE;
        }

      channel = g_io_channel_unix_new (new_fd);
      g_io_channel_set_close_on_unref (channel, TRUE);
      g_io_channel_set_encoding (channel, NULL, NULL);
      if (ft->direction == GIBBER_FILE_TRANSFER_DIRECTION_INCOMING)
        gibber_file_transfer_receive (ft, channel);
      else
        gibber_file_transfer_send (ft, channel);
      g_io_channel_unref (channel);
    }

  return FALSE;
}

static gboolean
setup_local_socket (SalutFileTransferChannel *self)
{
  GIOChannel *io_channel;

  io_channel = get_socket_channel (self);
  if (io_channel == NULL)
    {
      return FALSE;
    }

  g_io_add_watch (io_channel, G_IO_IN | G_IO_HUP,
      accept_local_socket_connection, self);
  g_io_channel_unref (io_channel);

  return TRUE;
}

static WockyDataForm *
find_data_form (GibberFileTransfer *ft,
    const gchar *form_type)
{
  GList *l;

  for (l = ft->dataforms; l != NULL; l = l->next)
    {
      WockyDataForm *form = l->data;
      WockyDataFormField *field;

      field = g_hash_table_lookup (form->fields, "FORM_TYPE");

      if (field == NULL)
        {
          DEBUG ("Data form doesn't have FORM_TYPE field!");
          continue;
        }

      /* found it! */
      if (!tp_strdiff (field->raw_value_contents[0], form_type))
        return form;
    }

  return NULL;
}

static gchar *
extract_service_name (GibberFileTransfer *ft)
{
  WockyDataForm *form = find_data_form (ft, NS_TP_FT_METADATA_SERVICE);
  WockyDataFormField *field;
  gchar *service_name = NULL;

  if (form == NULL)
    return NULL;

  field = g_hash_table_lookup (form->fields, "ServiceName");

  if (field == NULL)
    {
      DEBUG ("ServiceName propery not present in data form; odd...");
      goto out;
    }

  if (field->raw_value_contents == NULL
      || field->raw_value_contents[0] == NULL)
    {
      DEBUG ("ServiceName property doesn't have a real value; odd...");
    }
  else
    {
      service_name = g_strdup (field->raw_value_contents[0]);
    }

out:
  return service_name;
}

static GHashTable *
extract_metadata (GibberFileTransfer *ft)
{
  WockyDataForm *form = find_data_form (ft, NS_TP_FT_METADATA);
  GHashTable *metadata;
  GHashTableIter iter;
  gpointer key, value;

  if (form == NULL)
    return NULL;

  metadata = g_hash_table_new_full (g_str_hash, g_str_equal,
      g_free, (GDestroyNotify) g_strfreev);

  g_hash_table_iter_init (&iter, form->fields);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const gchar *var = key;
      WockyDataFormField *field = value;

      if (!tp_strdiff (var, "FORM_TYPE"))
        continue;

      g_hash_table_insert (metadata,
          g_strdup (var),
          g_strdupv (field->raw_value_contents));
    }

  return metadata;
}

SalutFileTransferChannel *
salut_file_transfer_channel_new (SalutConnection *conn,
                                 SalutContact *contact,
                                 TpHandle handle,
                                 TpHandle initiator_handle,
                                 TpFileTransferState state,
                                 const gchar *content_type,
                                 const gchar *filename,
                                 guint64 size,
                                 TpFileHashType content_hash_type,
                                 const gchar *content_hash,
                                 const gchar *description,
                                 guint64 date,
                                 guint64 initial_offset,
                                 const gchar *file_uri,
                                 const gchar *service_name,
                                 const GHashTable *metadata)
{
  return g_object_new (SALUT_TYPE_FILE_TRANSFER_CHANNEL,
      "connection", conn,
      "contact", contact,
      "handle", handle,
      "initiator-handle", initiator_handle,
      "state", state,
      "content-type", content_type,
      "filename", filename,
      "size", size,
      "content-hash-type", content_hash_type,
      "content-hash", content_hash,
      "description", description,
      "date", date,
      "initial-offset", initial_offset,
      "uri", file_uri,
      "service-name", service_name,
      "metadata", metadata,
      NULL);
}

SalutFileTransferChannel *
salut_file_transfer_channel_new_from_stanza (SalutConnection *connection,
                                             SalutContact *contact,
                                             TpHandle handle,
                                             TpFileTransferState state,
                                             WockyStanza *stanza)
{
  GError *error = NULL;
  GibberFileTransfer *ft;
  SalutFileTransferChannel *chan;
  gchar *service_name;
  GHashTable *metadata;

  ft = gibber_file_transfer_new_from_stanza_with_from (stanza, connection->porter,
      WOCKY_CONTACT (contact), contact->name, &error);

  if (ft == NULL)
    {
      /* Reply with an error */
      WockyStanza *reply;
      GError err = { WOCKY_XMPP_ERROR, WOCKY_XMPP_ERROR_BAD_REQUEST,
                      "failed to parse file offer" };

      DEBUG ("%s", error->message);
      reply = wocky_stanza_build_iq_error (stanza, NULL);
      wocky_stanza_error_to_node (&err, wocky_stanza_get_top_node (reply));

      wocky_porter_send (connection->porter, reply);

      g_object_unref (reply);
      g_clear_error (&error);
      return NULL;
    }

  /* Metadata */
  service_name = extract_service_name (ft);
  metadata = extract_metadata (ft);

  DEBUG ("Received file offer with id '%s'", ft->id);

  chan = g_object_new (SALUT_TYPE_FILE_TRANSFER_CHANNEL,
      "connection", connection,
      "contact", contact,
      "handle", handle,
      "initiator-handle", handle,
      "state", state,
      "filename", ft->filename,
      "size", gibber_file_transfer_get_size (ft),
      "description", ft->description,
      "content-type", ft->content_type,
      "service-name", service_name,
      "metadata", metadata,
      NULL);

  g_free (service_name);
  if (metadata != NULL)
    g_hash_table_unref (metadata);

  chan->priv->ft = ft;

  g_signal_connect (ft, "error", G_CALLBACK (error_cb), chan);

  return chan;
}
