/*
 * salut-connection.h - Header for SalutConnection
 * Copyright (C) 2005 Collabora Ltd.
 * Copyright (C) 2005 Nokia Corporation
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

#ifndef __SALUT_CONNECTION_H__
#define __SALUT_CONNECTION_H__

#include <glib-object.h>

G_BEGIN_DECLS

typedef struct _SalutConnection SalutConnection;
typedef struct _SalutConnectionClass SalutConnectionClass;

struct _SalutConnectionClass {
    GObjectClass parent_class;
};

struct _SalutConnection {
    GObject parent;

    gpointer priv;
};

GType salut_connection_get_type(void);

/* TYPE MACROS */
#define SALUT_TYPE_CONNECTION \
  (salut_connection_get_type())
#define SALUT_CONNECTION(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), SALUT_TYPE_CONNECTION, SalutConnection))
#define SALUT_CONNECTION_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass), SALUT_TYPE_CONNECTION, SalutConnectionClass))
#define SALUT_IS_CONNECTION(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), SALUT_TYPE_CONNECTION))
#define SALUT_IS_CONNECTION_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass), SALUT_TYPE_CONNECTION))
#define SALUT_CONNECTION_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj), SALUT_TYPE_CONNECTION, SalutConnectionClass))


gboolean
salut_connection_add_status (SalutConnection *self,
                             const gchar *status,
                             GHashTable *parms,
                             GError **error);

gboolean
salut_connection_clear_status (SalutConnection *self,
                               GError **error);

gboolean
salut_connection_connect (SalutConnection *self,
                          GError **error);

gboolean
salut_connection_disconnect (SalutConnection *self,
                             GError **error);

gboolean
salut_connection_get_alias_flags (SalutConnection *self,
                                  guint *ret,
                                  GError **error);

gboolean
salut_connection_get_interfaces (SalutConnection *self,
                                 gchar ***ret,
                                 GError **error);

gboolean
salut_connection_get_presence (SalutConnection *self,
                               const GArray *contacts,
                               GHashTable **ret,
                               GError **error);

gboolean
salut_connection_get_protocol (SalutConnection *self,
                               gchar **ret,
                               GError **error);

gboolean
salut_connection_get_self_handle (SalutConnection *self,
                                  guint *ret,
                                  GError **error);

gboolean
salut_connection_get_status (SalutConnection *self,
                             guint *ret,
                             GError **error);

gboolean
salut_connection_get_statuses (SalutConnection *self,
                               GHashTable **ret,
                               GError **error);

void
salut_connection_hold_handles (SalutConnection *self,
                               guint handle_type,
                               const GArray *handles,
                               DBusGMethodInvocation *context);

void
salut_connection_inspect_handles (SalutConnection *self,
                                  guint handle_type,
                                  const GArray *handles,
                                  DBusGMethodInvocation *context);

gboolean
salut_connection_list_channels (SalutConnection *self,
                                GPtrArray **ret,
                                GError **error);

void
salut_connection_release_handles (SalutConnection *self,
                                  guint handle_type,
                                  const GArray *handles,
                                  DBusGMethodInvocation *context);

gboolean
salut_connection_remove_status (SalutConnection *self,
                                const gchar *status,
                                GError **error);

gboolean
salut_connection_request_aliases (SalutConnection *self,
                                  const GArray *contacts,
                                  gchar ***ret,
                                  GError **error);

void
salut_connection_request_channel (SalutConnection *self,
                                  const gchar *type,
                                  guint handle_type,
                                  guint handle,
                                  gboolean suppress_handler,
                                  DBusGMethodInvocation *context);

void
salut_connection_request_handles (SalutConnection *self,
                                  guint handle_type,
                                  const gchar **names,
                                  DBusGMethodInvocation *context);

gboolean
salut_connection_request_presence (SalutConnection *self,
                                   const GArray *contacts,
                                   GError **error);

gboolean
salut_connection_set_aliases (SalutConnection *self,
                              GHashTable *aliases,
                              GError **error);

gboolean
salut_connection_set_last_activity_time (SalutConnection *self,
                                         guint time,
                                         GError **error);

gboolean
salut_connection_set_status (SalutConnection *self,
                             GHashTable *statuses,
                             GError **error);



G_END_DECLS

#endif /* #ifndef __SALUT_CONNECTION_H__*/
