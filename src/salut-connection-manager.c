/*
 * salut-connection-manager.c - Source for SalutConnectionManager
 * Copyright (C) 2005 Nokia Corporation
 * Copyright (C) 2006 Collabora Ltd.
 *   @author: Sjoerd Simons <sjoerd@luon.net>
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

#include <dbus/dbus-protocol.h>
#include <telepathy-glib/util.h>

#include "salut-connection-manager.h"
#include "salut-connection.h"

/* properties */
enum
{
  PROP_BACKEND = 1,
  LAST_PROPERTY
};

typedef struct _SalutConnectionManagerPrivate SalutConnectionManagerPrivate;
struct _SalutConnectionManagerPrivate
{
  GType backend_type;
};

#define SALUT_CONNECTION_MANAGER_GET_PRIVATE(obj) \
    ((SalutConnectionManagerPrivate *) ((SalutConnectionManager *)obj)->priv)


typedef struct {
  guint set_mask;

  gchar *nickname;
  gchar *first_name;
  gchar *last_name;
  gchar *email;
  gchar *jid;
  gchar *published_name;
} SalutParams;

enum {
  SALUT_PARAM_NICKNAME = 0,
  SALUT_PARAM_FIRST_NAME,
  SALUT_PARAM_LAST_NAME,
  SALUT_PARAM_JID,
  SALUT_PARAM_EMAIL,
  SALUT_PARAM_PUBLISHED_NAME,
  SALUT_NR_PARAMS
};

static const TpCMParamSpec salut_params[] = {
  { "nickname", DBUS_TYPE_STRING_AS_STRING, G_TYPE_STRING,
     0, NULL,
     G_STRUCT_OFFSET(SalutParams, nickname),
     tp_cm_param_filter_string_nonempty, NULL },
  { "first-name", DBUS_TYPE_STRING_AS_STRING, G_TYPE_STRING,
     TP_CONN_MGR_PARAM_FLAG_REQUIRED, NULL,
     G_STRUCT_OFFSET(SalutParams, first_name)},
  { "last-name", DBUS_TYPE_STRING_AS_STRING, G_TYPE_STRING,
     TP_CONN_MGR_PARAM_FLAG_REQUIRED, NULL,
     G_STRUCT_OFFSET(SalutParams, last_name)},
  { "jid", DBUS_TYPE_STRING_AS_STRING, G_TYPE_STRING, 0, NULL,
     G_STRUCT_OFFSET(SalutParams, jid)},
  { "email", DBUS_TYPE_STRING_AS_STRING, G_TYPE_STRING, 0, NULL,
     G_STRUCT_OFFSET(SalutParams, email)},
  { "published-name", DBUS_TYPE_STRING_AS_STRING, G_TYPE_STRING, 0, NULL,
     G_STRUCT_OFFSET(SalutParams, published_name),
     tp_cm_param_filter_string_nonempty, NULL },
  {NULL, NULL, 0, 0, NULL, 0}
};

static void *salut_params_new (void);
static void salut_params_free (void *params);

const TpCMProtocolSpec salut_protocols[] = {
  {"local-xmpp", salut_params, salut_params_new, salut_params_free },
  { NULL, NULL}
};

static TpBaseConnection *
salut_connection_manager_new_connection (TpBaseConnectionManager *self,
                                         const gchar *proto,
                                         TpIntSet *params_present,
                                         void *parsed_params,
                                         GError **error);


G_DEFINE_TYPE(SalutConnectionManager, salut_connection_manager,
              TP_TYPE_BASE_CONNECTION_MANAGER)

static void
salut_connection_manager_init (SalutConnectionManager *self)
{
  SalutConnectionManagerPrivate *priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      SALUT_TYPE_CONNECTION_MANAGER, SalutConnectionManagerPrivate);

  self->priv = priv;
}

static void
salut_connection_manager_get_property (GObject *object,
                                       guint property_id,
                                       GValue *value,
                                       GParamSpec *pspec)
{
  SalutConnectionManager *self = SALUT_CONNECTION_MANAGER (object);
  SalutConnectionManagerPrivate *priv = SALUT_CONNECTION_MANAGER_GET_PRIVATE
      (self);

  switch (property_id)
    {
      case PROP_BACKEND:
        g_value_set_gtype (value, priv->backend_type);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
salut_connection_manager_set_property (GObject *object,
                                       guint property_id,
                                       const GValue *value,
                                       GParamSpec *pspec)
{
  SalutConnectionManager *self = SALUT_CONNECTION_MANAGER (object);
  SalutConnectionManagerPrivate *priv = SALUT_CONNECTION_MANAGER_GET_PRIVATE
      (self);

  switch (property_id)
    {
      case PROP_BACKEND:
        priv->backend_type = g_value_get_gtype (value);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
salut_connection_manager_class_init (
    SalutConnectionManagerClass *salut_connection_manager_class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (salut_connection_manager_class);
  TpBaseConnectionManagerClass *base_cm_class =
    TP_BASE_CONNECTION_MANAGER_CLASS(salut_connection_manager_class);
  GParamSpec *param_spec;

  g_type_class_add_private (salut_connection_manager_class,
      sizeof (SalutConnectionManagerPrivate));

  object_class->get_property = salut_connection_manager_get_property;
  object_class->set_property = salut_connection_manager_set_property;

  param_spec = g_param_spec_gtype (
      "backend-type",
      "backend type",
      "a G_TYPE_GTYPE of the backend to use",
      G_TYPE_NONE,
      G_PARAM_READWRITE |
      G_PARAM_STATIC_NAME |
      G_PARAM_STATIC_NICK |
      G_PARAM_STATIC_BLURB);
  g_object_class_install_property (object_class, PROP_BACKEND,
      param_spec);

  base_cm_class->cm_dbus_name = "salut";
  base_cm_class->protocol_params = salut_protocols;
  base_cm_class->new_connection = salut_connection_manager_new_connection;

}

static void *salut_params_new (void) {
  return g_slice_new0(SalutParams);
};

static void salut_params_free (void *params)
{
  SalutParams *p = (SalutParams *)params;

  g_free (p->nickname);
  g_free (p->first_name);
  g_free (p->last_name);
  g_free (p->email);
  g_free (p->jid);
  g_free (p->published_name);

  g_slice_free (SalutParams, params);
};

#define SET_PROPERTY_IF_PARAM_SET(prop, param, member) \
  if (tp_intset_is_member (params_present, param)) \
    { \
      g_object_set (conn, prop, member, NULL); \
    }

static TpBaseConnection *
salut_connection_manager_new_connection (TpBaseConnectionManager *self,
                                         const gchar *proto,
                                         TpIntSet *params_present,
                                         void *parsed_params,
                                         GError **error)
{
  SalutConnectionManagerPrivate *priv = SALUT_CONNECTION_MANAGER_GET_PRIVATE
      (self);
  SalutConnection *conn;
  SalutParams *params = (SalutParams *)parsed_params;

  g_assert (!tp_strdiff (proto, "local-xmpp"));

  conn = g_object_new (SALUT_TYPE_CONNECTION,
      "protocol", proto,
      "backend-type", priv->backend_type,
      NULL);

  SET_PROPERTY_IF_PARAM_SET ("nickname", SALUT_PARAM_NICKNAME,
                              params->nickname);
  SET_PROPERTY_IF_PARAM_SET ("first-name", SALUT_PARAM_FIRST_NAME,
                              params->first_name);
  SET_PROPERTY_IF_PARAM_SET ("last-name", SALUT_PARAM_LAST_NAME,
                              params->last_name);
  SET_PROPERTY_IF_PARAM_SET ("jid", SALUT_PARAM_EMAIL, params->jid);
  SET_PROPERTY_IF_PARAM_SET ("email", SALUT_PARAM_JID, params->email);
  SET_PROPERTY_IF_PARAM_SET ("published-name", SALUT_PARAM_PUBLISHED_NAME,
                             params->published_name);

  return TP_BASE_CONNECTION (conn);
}

