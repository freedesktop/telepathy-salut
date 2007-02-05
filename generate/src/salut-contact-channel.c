/*
 * salut-contact-channel.c - Source for SalutContactChannel
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

#include <dbus/dbus-glib.h>
#include <stdio.h>
#include <stdlib.h>

#include "salut-contact-channel.h"
#include "salut-contact-channel-signals-marshal.h"

#include "salut-contact-channel-glue.h"

G_DEFINE_TYPE(SalutContactChannel, salut_contact_channel, G_TYPE_OBJECT)

/* signal enum */
enum
{
    CLOSED,
    GROUP_FLAGS_CHANGED,
    MEMBERS_CHANGED,
    LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = {0};

/* private structure */
typedef struct _SalutContactChannelPrivate SalutContactChannelPrivate;

struct _SalutContactChannelPrivate
{
  gboolean dispose_has_run;
};

#define SALUT_CONTACT_CHANNEL_GET_PRIVATE(obj) \
    ((SalutContactChannelPrivate *)obj->priv)

static void
salut_contact_channel_init (SalutContactChannel *self)
{
  SalutContactChannelPrivate *priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      SALUT_TYPE_CONTACT_CHANNEL, SalutContactChannelPrivate);

  self->priv = priv;

  /* allocate any data required by the object here */
}

static void salut_contact_channel_dispose (GObject *object);
static void salut_contact_channel_finalize (GObject *object);

static void
salut_contact_channel_class_init (SalutContactChannelClass *salut_contact_channel_class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (salut_contact_channel_class);

  g_type_class_add_private (salut_contact_channel_class, sizeof (SalutContactChannelPrivate));

  object_class->dispose = salut_contact_channel_dispose;
  object_class->finalize = salut_contact_channel_finalize;

  signals[CLOSED] =
    g_signal_new ("closed",
                  G_OBJECT_CLASS_TYPE (salut_contact_channel_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  g_cclosure_marshal_VOID__VOID,
                  G_TYPE_NONE, 0);

  signals[GROUP_FLAGS_CHANGED] =
    g_signal_new ("group-flags-changed",
                  G_OBJECT_CLASS_TYPE (salut_contact_channel_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  salut_contact_channel_marshal_VOID__UINT_UINT,
                  G_TYPE_NONE, 2, G_TYPE_UINT, G_TYPE_UINT);

  signals[MEMBERS_CHANGED] =
    g_signal_new ("members-changed",
                  G_OBJECT_CLASS_TYPE (salut_contact_channel_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  salut_contact_channel_marshal_VOID__STRING_BOXED_BOXED_BOXED_BOXED_UINT_UINT,
                  G_TYPE_NONE, 7, G_TYPE_STRING, DBUS_TYPE_G_UINT_ARRAY, DBUS_TYPE_G_UINT_ARRAY, DBUS_TYPE_G_UINT_ARRAY, DBUS_TYPE_G_UINT_ARRAY, G_TYPE_UINT, G_TYPE_UINT);

  dbus_g_object_type_install_info (G_TYPE_FROM_CLASS (salut_contact_channel_class), &dbus_glib_salut_contact_channel_object_info);
}

void
salut_contact_channel_dispose (GObject *object)
{
  SalutContactChannel *self = SALUT_CONTACT_CHANNEL (object);
  SalutContactChannelPrivate *priv = SALUT_CONTACT_CHANNEL_GET_PRIVATE (self);

  if (priv->dispose_has_run)
    return;

  priv->dispose_has_run = TRUE;

  /* release any references held by the object here */

  if (G_OBJECT_CLASS (salut_contact_channel_parent_class)->dispose)
    G_OBJECT_CLASS (salut_contact_channel_parent_class)->dispose (object);
}

void
salut_contact_channel_finalize (GObject *object)
{
  SalutContactChannel *self = SALUT_CONTACT_CHANNEL (object);
  SalutContactChannelPrivate *priv = SALUT_CONTACT_CHANNEL_GET_PRIVATE (self);

  /* free any data held directly by the object here */

  G_OBJECT_CLASS (salut_contact_channel_parent_class)->finalize (object);
}



/**
 * salut_contact_channel_add_members
 *
 * Implements D-Bus method AddMembers
 * on interface org.freedesktop.Telepathy.Channel.Interface.Group
 *
 * @error: Used to return a pointer to a GError detailing any error
 *         that occurred, D-Bus will throw the error only if this
 *         function returns FALSE.
 *
 * Returns: TRUE if successful, FALSE if an error was thrown.
 */
gboolean
salut_contact_channel_add_members (SalutContactChannel *self,
                                   const GArray *contacts,
                                   const gchar *message,
                                   GError **error)
{
  return TRUE;
}


/**
 * salut_contact_channel_close
 *
 * Implements D-Bus method Close
 * on interface org.freedesktop.Telepathy.Channel
 *
 * @error: Used to return a pointer to a GError detailing any error
 *         that occurred, D-Bus will throw the error only if this
 *         function returns FALSE.
 *
 * Returns: TRUE if successful, FALSE if an error was thrown.
 */
gboolean
salut_contact_channel_close (SalutContactChannel *self,
                             GError **error)
{
  return TRUE;
}


/**
 * salut_contact_channel_get_all_members
 *
 * Implements D-Bus method GetAllMembers
 * on interface org.freedesktop.Telepathy.Channel.Interface.Group
 *
 * @error: Used to return a pointer to a GError detailing any error
 *         that occurred, D-Bus will throw the error only if this
 *         function returns FALSE.
 *
 * Returns: TRUE if successful, FALSE if an error was thrown.
 */
gboolean
salut_contact_channel_get_all_members (SalutContactChannel *self,
                                       GArray **ret,
                                       GArray **ret1,
                                       GArray **ret2,
                                       GError **error)
{
  return TRUE;
}


/**
 * salut_contact_channel_get_channel_type
 *
 * Implements D-Bus method GetChannelType
 * on interface org.freedesktop.Telepathy.Channel
 *
 * @error: Used to return a pointer to a GError detailing any error
 *         that occurred, D-Bus will throw the error only if this
 *         function returns FALSE.
 *
 * Returns: TRUE if successful, FALSE if an error was thrown.
 */
gboolean
salut_contact_channel_get_channel_type (SalutContactChannel *self,
                                        gchar **ret,
                                        GError **error)
{
  return TRUE;
}


/**
 * salut_contact_channel_get_group_flags
 *
 * Implements D-Bus method GetGroupFlags
 * on interface org.freedesktop.Telepathy.Channel.Interface.Group
 *
 * @error: Used to return a pointer to a GError detailing any error
 *         that occurred, D-Bus will throw the error only if this
 *         function returns FALSE.
 *
 * Returns: TRUE if successful, FALSE if an error was thrown.
 */
gboolean
salut_contact_channel_get_group_flags (SalutContactChannel *self,
                                       guint *ret,
                                       GError **error)
{
  return TRUE;
}


/**
 * salut_contact_channel_get_handle
 *
 * Implements D-Bus method GetHandle
 * on interface org.freedesktop.Telepathy.Channel
 *
 * @error: Used to return a pointer to a GError detailing any error
 *         that occurred, D-Bus will throw the error only if this
 *         function returns FALSE.
 *
 * Returns: TRUE if successful, FALSE if an error was thrown.
 */
gboolean
salut_contact_channel_get_handle (SalutContactChannel *self,
                                  guint *ret,
                                  guint *ret1,
                                  GError **error)
{
  return TRUE;
}


/**
 * salut_contact_channel_get_handle_owners
 *
 * Implements D-Bus method GetHandleOwners
 * on interface org.freedesktop.Telepathy.Channel.Interface.Group
 *
 * @error: Used to return a pointer to a GError detailing any error
 *         that occurred, D-Bus will throw the error only if this
 *         function returns FALSE.
 *
 * Returns: TRUE if successful, FALSE if an error was thrown.
 */
gboolean
salut_contact_channel_get_handle_owners (SalutContactChannel *self,
                                         const GArray *handles,
                                         GArray **ret,
                                         GError **error)
{
  return TRUE;
}


/**
 * salut_contact_channel_get_interfaces
 *
 * Implements D-Bus method GetInterfaces
 * on interface org.freedesktop.Telepathy.Channel
 *
 * @error: Used to return a pointer to a GError detailing any error
 *         that occurred, D-Bus will throw the error only if this
 *         function returns FALSE.
 *
 * Returns: TRUE if successful, FALSE if an error was thrown.
 */
gboolean
salut_contact_channel_get_interfaces (SalutContactChannel *self,
                                      gchar ***ret,
                                      GError **error)
{
  return TRUE;
}


/**
 * salut_contact_channel_get_local_pending_members
 *
 * Implements D-Bus method GetLocalPendingMembers
 * on interface org.freedesktop.Telepathy.Channel.Interface.Group
 *
 * @error: Used to return a pointer to a GError detailing any error
 *         that occurred, D-Bus will throw the error only if this
 *         function returns FALSE.
 *
 * Returns: TRUE if successful, FALSE if an error was thrown.
 */
gboolean
salut_contact_channel_get_local_pending_members (SalutContactChannel *self,
                                                 GArray **ret,
                                                 GError **error)
{
  return TRUE;
}


/**
 * salut_contact_channel_get_local_pending_members_with_info
 *
 * Implements D-Bus method GetLocalPendingMembersWithInfo
 * on interface org.freedesktop.Telepathy.Channel.Interface.Group
 *
 * @error: Used to return a pointer to a GError detailing any error
 *         that occurred, D-Bus will throw the error only if this
 *         function returns FALSE.
 *
 * Returns: TRUE if successful, FALSE if an error was thrown.
 */
gboolean
salut_contact_channel_get_local_pending_members_with_info (SalutContactChannel *self,
                                                           GPtrArray **ret,
                                                           GError **error)
{
  return TRUE;
}


/**
 * salut_contact_channel_get_members
 *
 * Implements D-Bus method GetMembers
 * on interface org.freedesktop.Telepathy.Channel.Interface.Group
 *
 * @error: Used to return a pointer to a GError detailing any error
 *         that occurred, D-Bus will throw the error only if this
 *         function returns FALSE.
 *
 * Returns: TRUE if successful, FALSE if an error was thrown.
 */
gboolean
salut_contact_channel_get_members (SalutContactChannel *self,
                                   GArray **ret,
                                   GError **error)
{
  return TRUE;
}


/**
 * salut_contact_channel_get_remote_pending_members
 *
 * Implements D-Bus method GetRemotePendingMembers
 * on interface org.freedesktop.Telepathy.Channel.Interface.Group
 *
 * @error: Used to return a pointer to a GError detailing any error
 *         that occurred, D-Bus will throw the error only if this
 *         function returns FALSE.
 *
 * Returns: TRUE if successful, FALSE if an error was thrown.
 */
gboolean
salut_contact_channel_get_remote_pending_members (SalutContactChannel *self,
                                                  GArray **ret,
                                                  GError **error)
{
  return TRUE;
}


/**
 * salut_contact_channel_get_self_handle
 *
 * Implements D-Bus method GetSelfHandle
 * on interface org.freedesktop.Telepathy.Channel.Interface.Group
 *
 * @error: Used to return a pointer to a GError detailing any error
 *         that occurred, D-Bus will throw the error only if this
 *         function returns FALSE.
 *
 * Returns: TRUE if successful, FALSE if an error was thrown.
 */
gboolean
salut_contact_channel_get_self_handle (SalutContactChannel *self,
                                       guint *ret,
                                       GError **error)
{
  return TRUE;
}


/**
 * salut_contact_channel_remove_members
 *
 * Implements D-Bus method RemoveMembers
 * on interface org.freedesktop.Telepathy.Channel.Interface.Group
 *
 * @error: Used to return a pointer to a GError detailing any error
 *         that occurred, D-Bus will throw the error only if this
 *         function returns FALSE.
 *
 * Returns: TRUE if successful, FALSE if an error was thrown.
 */
gboolean
salut_contact_channel_remove_members (SalutContactChannel *self,
                                      const GArray *contacts,
                                      const gchar *message,
                                      GError **error)
{
  return TRUE;
}

