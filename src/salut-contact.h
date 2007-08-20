/*
 * salut-contact.h - Header for SalutContact
 * Copyright (C) 2005 Collabora Ltd.
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

#ifndef __SALUT_CONTACT_H__
#define __SALUT_CONTACT_H__

#include "config.h"

#include <netinet/in.h>
#include <glib-object.h>

#include <telepathy-glib/handle-repo.h>

#include "salut-avahi-service-resolver.h"
#include "salut-avahi-enums.h"

#include "salut-presence.h"

G_BEGIN_DECLS


#define  SALUT_CONTACT_ALIAS_CHANGED  0x1
#define  SALUT_CONTACT_STATUS_CHANGED 0x2
#define  SALUT_CONTACT_AVATAR_CHANGED 0x4
#ifdef ENABLE_OLPC
#define  SALUT_CONTACT_OLPC_PROPERTIES 0x8
#define  SALUT_CONTACT_OLPC_CURRENT_ACTIVITY 0x10
#define  SALUT_CONTACT_OLPC_ACTIVITIES 0x20
#endif /* ENABLE_OLPC */

typedef struct _SalutContact SalutContact;
typedef struct _SalutContactClass SalutContactClass;

struct _SalutContactClass {
    GObjectClass parent_class;
};

struct _SalutContact {
    GObject parent;
    gchar *name;
    SalutPresenceId status;
    gchar *avatar_token;
    gchar *status_message;
    gchar *jid;
#ifdef ENABLE_OLPC
    GArray *olpc_key;
    gchar *olpc_color;
    gchar *olpc_cur_act;
    TpHandle olpc_cur_act_room;
#endif /* ENABLE_OLPC */
};

GType salut_contact_get_type(void);

/* TYPE MACROS */
#define SALUT_TYPE_CONTACT \
  (salut_contact_get_type())
#define SALUT_CONTACT(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), SALUT_TYPE_CONTACT, SalutContact))
#define SALUT_CONTACT_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass), SALUT_TYPE_CONTACT, SalutContactClass))
#define SALUT_IS_CONTACT(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), SALUT_TYPE_CONTACT))
#define SALUT_IS_CONTACT_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass), SALUT_TYPE_CONTACT))
#define SALUT_CONTACT_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj), SALUT_TYPE_CONTACT, SalutContactClass))

SalutContact *salut_contact_new(SalutAvahiClient *client,
    TpHandleRepoIface *room_repo, const gchar *name);

void
salut_contact_add_service(SalutContact *contact,
                          AvahiIfIndex interface, AvahiProtocol protocol,
                          const char *name, const char *type,
                          const char *domain);

void
salut_contact_remove_service(SalutContact *contact,
                          AvahiIfIndex interface, AvahiProtocol protocol,
                          const char *name, const char *type,
                          const char *domain);

typedef struct {
  struct sockaddr_storage address;
} salut_contact_address_t;

/* Returns an array of addresses on which the contact can be found */
GArray *
salut_contact_get_addresses(SalutContact *contact);

gboolean
salut_contact_has_address(SalutContact *contact,
                           struct sockaddr_storage *address);
const gchar *
salut_contact_get_alias(SalutContact *contact);

gboolean
salut_contact_has_services(SalutContact *contact);

typedef void (*salut_contact_get_avatar_callback)(SalutContact *contact,
                                                  guint8 *avatar,
                                                  gsize size,
                                                  gpointer user_data);

void
salut_contact_get_avatar(SalutContact *contact,
                         salut_contact_get_avatar_callback callback,
                         gpointer user_data1);

#ifdef ENABLE_OLPC
typedef void (*SalutContactOLPCActivityFunc)
    (const gchar *id, TpHandle handle, gpointer user_data);

void salut_contact_foreach_olpc_activity (SalutContact *self,
    SalutContactOLPCActivityFunc foreach, gpointer user_data);

void salut_contact_takes_part_olpc_activity (SalutContact *self,
    TpHandle room, const gchar *activity_id);
#endif

G_END_DECLS

#endif /* #ifndef __SALUT_CONTACT_H__*/
