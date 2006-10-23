/*
 * salut-avahi-service-resolver.c - Source for SalutAvahiServiceResolver
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

#include <dbus/dbus-glib.h>
#include <stdio.h>
#include <stdlib.h>

#include "salut-avahi-service-resolver.h"
#include "salut-avahi-service-resolver-signals-marshal.h"

#include "salut-avahi-errors.h"

#include "salut-avahi-enums.h"
#include "salut-avahi-enums-enumtypes.h"

#include <avahi-client/lookup.h>

G_DEFINE_TYPE(SalutAvahiServiceResolver, salut_avahi_service_resolver, G_TYPE_OBJECT)

/* signal enum */
enum
{
  FOUND,
  FAILURE,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = {0};

/* properties */
enum {
  PROP_PROTOCOL = 1,
  PROP_IFINDEX,
  PROP_NAME,
  PROP_TYPE,
  PROP_DOMAIN,
  PROP_FLAGS,
  PROP_APROTOCOL
};

/* private structure */
typedef struct _SalutAvahiServiceResolverPrivate SalutAvahiServiceResolverPrivate;

struct _SalutAvahiServiceResolverPrivate
{
  SalutAvahiClient *client;
  AvahiServiceResolver *resolver;
  AvahiIfIndex interface;
  AvahiProtocol protocol;
  AvahiAddress address;
  uint16_t port;
  char *name;
  char *type;
  char *domain;
  AvahiProtocol aprotocol;
  AvahiLookupFlags flags;
  gboolean dispose_has_run;
};

#define SALUT_AVAHI_SERVICE_RESOLVER_GET_PRIVATE(o)     (G_TYPE_INSTANCE_GET_PRIVATE ((o), SALUT_TYPE_AVAHI_SERVICE_RESOLVER, SalutAvahiServiceResolverPrivate))

static void
salut_avahi_service_resolver_init (SalutAvahiServiceResolver *obj)
{
  SalutAvahiServiceResolverPrivate *priv = SALUT_AVAHI_SERVICE_RESOLVER_GET_PRIVATE (obj);

  /* allocate any data required by the object here */
  priv->client = NULL;
  priv->resolver = NULL;
  priv->name = NULL;
  priv->type = NULL;
  priv->domain = NULL;
  priv->port = 0;
}

static void salut_avahi_service_resolver_dispose (GObject *object);
static void salut_avahi_service_resolver_finalize (GObject *object);


static void
salut_avahi_service_resolver_set_property (GObject *object,
                                           guint property_id,
                                           const GValue *value,
                                           GParamSpec *pspec) {
 SalutAvahiServiceResolver *resolver = SALUT_AVAHI_SERVICE_RESOLVER(object);
 SalutAvahiServiceResolverPrivate *priv = 
   SALUT_AVAHI_SERVICE_RESOLVER_GET_PRIVATE(resolver);

  g_assert(priv->resolver == NULL);
  switch (property_id) {
    case PROP_PROTOCOL:
      priv->protocol = g_value_get_enum(value);
      break;
    case PROP_APROTOCOL:
      priv->aprotocol = g_value_get_enum(value);
      break;
    case PROP_IFINDEX:
      priv->interface = g_value_get_int(value);
      break;
    case PROP_NAME:
      priv->name = g_strdup(g_value_get_string(value));
      break;
    case PROP_TYPE:
      priv->type = g_strdup(g_value_get_string(value));
      break;
    case PROP_DOMAIN:
      priv->domain = g_strdup(g_value_get_string(value));
      break;
    case PROP_FLAGS:
      priv->flags = g_value_get_enum(value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

static void
salut_avahi_service_resolver_get_property (GObject *object,
                                           guint property_id,
                                           GValue *value,
                                           GParamSpec *pspec) {
  SalutAvahiServiceResolver *resolver = SALUT_AVAHI_SERVICE_RESOLVER(object);
  SalutAvahiServiceResolverPrivate *priv = 
    SALUT_AVAHI_SERVICE_RESOLVER_GET_PRIVATE(resolver);

  switch (property_id) {
    case PROP_APROTOCOL:
      g_value_set_enum(value, priv->aprotocol);
      break;
    case PROP_PROTOCOL:
      g_value_set_enum(value, priv->protocol);
      break;
    case PROP_IFINDEX:
      g_value_set_int(value, priv->interface);
      break;
    case PROP_NAME:
      g_value_set_string(value, priv->type);
      break;
    case PROP_TYPE:
      g_value_set_string(value, priv->type);
      break;
    case PROP_DOMAIN:
      g_value_set_string(value, priv->domain);
      break;
    case PROP_FLAGS:
      g_value_set_enum(value, priv->flags);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}


static void
salut_avahi_service_resolver_class_init (SalutAvahiServiceResolverClass *salut_avahi_service_resolver_class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (salut_avahi_service_resolver_class);
  GParamSpec *param_spec;

  g_type_class_add_private (salut_avahi_service_resolver_class, sizeof (SalutAvahiServiceResolverPrivate));

  object_class->set_property = salut_avahi_service_resolver_set_property;
  object_class->get_property = salut_avahi_service_resolver_get_property;

  object_class->dispose = salut_avahi_service_resolver_dispose;
  object_class->finalize = salut_avahi_service_resolver_finalize;

  signals[FOUND] =
    g_signal_new("found",
                 G_OBJECT_CLASS_TYPE(salut_avahi_service_resolver_class),
                 G_SIGNAL_RUN_LAST,
                 0,
                 NULL, NULL,
                 salut_avahi_service_resolver_marshal_VOID__INT_INT_STRING_STRING_STRING_STRING_POINTER_INT_POINTER_INT,
                 G_TYPE_NONE, 10, 
                 G_TYPE_INT,
                 SALUT_TYPE_AVAHI_PROTOCOL,
                 G_TYPE_STRING,
                 G_TYPE_STRING,
                 G_TYPE_STRING,
                 G_TYPE_STRING,
                 G_TYPE_POINTER,
                 G_TYPE_INT,
                 G_TYPE_POINTER,
                 SALUT_TYPE_AVAHI_LOOKUP_RESULT_FLAGS);

  signals[FAILURE] =
    g_signal_new("failure",
                 G_OBJECT_CLASS_TYPE(salut_avahi_service_resolver_class),
                 G_SIGNAL_RUN_LAST,
                 0,
                 NULL, NULL,
                 g_cclosure_marshal_VOID__POINTER,
                 G_TYPE_NONE, 1, G_TYPE_POINTER);

  param_spec = g_param_spec_enum("protocol", "Avahi protocol to resolve on",
                                "Avahi protocol to resolve on",
                                 SALUT_TYPE_AVAHI_PROTOCOL,
                                 SALUT_AVAHI_PROTOCOL_UNSPEC,
                                 G_PARAM_READWRITE  |
                                 G_PARAM_STATIC_NAME |
                                 G_PARAM_STATIC_BLURB);
  g_object_class_install_property(object_class, PROP_PROTOCOL, param_spec);

  param_spec = g_param_spec_enum("aprotocol", "Address protocol",
                                "Avahi protocol of the address to be resolved",
                                 SALUT_TYPE_AVAHI_PROTOCOL,
                                 SALUT_AVAHI_PROTOCOL_UNSPEC,
                                 G_PARAM_READWRITE  |
                                 G_PARAM_STATIC_NAME |
                                 G_PARAM_STATIC_BLURB);
  g_object_class_install_property(object_class, PROP_APROTOCOL, param_spec);

  param_spec = g_param_spec_int("interface", "interface index",
                                "Interface use for resolver",
                                AVAHI_IF_UNSPEC,
                                G_MAXINT,
                                AVAHI_IF_UNSPEC,
                                G_PARAM_READWRITE  |
                                G_PARAM_STATIC_NAME |
                                G_PARAM_STATIC_BLURB);
  g_object_class_install_property(object_class, PROP_IFINDEX, param_spec);

  param_spec = g_param_spec_string("name", "service name",
                                   "name to resolve",
                                   NULL,
                                   G_PARAM_READWRITE  |
                                   G_PARAM_STATIC_NAME |
                                   G_PARAM_STATIC_BLURB);
  g_object_class_install_property(object_class, PROP_NAME, param_spec);

  param_spec = g_param_spec_string("type", "service type",
                                   "Service type to browse for",
                                   NULL,
                                   G_PARAM_READWRITE  |
                                   G_PARAM_STATIC_NAME |
                                   G_PARAM_STATIC_BLURB);
  g_object_class_install_property(object_class, PROP_TYPE, param_spec);

  param_spec = g_param_spec_string("domain", "service domain",
                                   "Domain to browse in",
                                   NULL,
                                   G_PARAM_READWRITE  |
                                   G_PARAM_STATIC_NAME |
                                   G_PARAM_STATIC_BLURB);
  g_object_class_install_property(object_class, PROP_DOMAIN, param_spec);

  param_spec = g_param_spec_enum("flags", "Lookup flags for the resolver",
                                "Resolver lookup flags",
                                 SALUT_TYPE_AVAHI_PROTOCOL,
                                 SALUT_AVAHI_LOOKUP_NO_FLAGS,
                                 G_PARAM_READWRITE  |
                                 G_PARAM_STATIC_NAME |
                                 G_PARAM_STATIC_BLURB);
  g_object_class_install_property(object_class, PROP_FLAGS, param_spec);
}

void
salut_avahi_service_resolver_dispose (GObject *object)
{
  SalutAvahiServiceResolver *self = SALUT_AVAHI_SERVICE_RESOLVER (object);
  SalutAvahiServiceResolverPrivate *priv = SALUT_AVAHI_SERVICE_RESOLVER_GET_PRIVATE (self);

  if (priv->dispose_has_run)
    return;

  priv->dispose_has_run = TRUE;

  if (priv->client) 
    g_object_unref(priv->client);
  priv->client = NULL;

  if (priv->resolver) 
    avahi_service_resolver_free(priv->resolver);
  priv->resolver = NULL;

  /* release any references held by the object here */

  if (G_OBJECT_CLASS (salut_avahi_service_resolver_parent_class)->dispose)
    G_OBJECT_CLASS (salut_avahi_service_resolver_parent_class)->dispose (object);
}

void
salut_avahi_service_resolver_finalize (GObject *object)
{
  SalutAvahiServiceResolver *self = SALUT_AVAHI_SERVICE_RESOLVER (object);
  SalutAvahiServiceResolverPrivate *priv = SALUT_AVAHI_SERVICE_RESOLVER_GET_PRIVATE (self);

  /* free any data held directly by the object here */
  g_free(priv->name);
  priv->name = NULL;

  g_free(priv->type);
  priv->type = NULL;

  g_free(priv->domain);
  priv->domain = NULL;

  G_OBJECT_CLASS (salut_avahi_service_resolver_parent_class)->finalize (object);
}

static void
_avahi_service_resolver_cb(AvahiServiceResolver *resolver, 
                           AvahiIfIndex interface,
                           AvahiProtocol protocol,
                           AvahiResolverEvent event,
                           const char *name, const char *type,
                           const char *domain, const char *host_name,
                           const AvahiAddress *a,
                           uint16_t port,
                           AvahiStringList *txt,
                           AvahiLookupResultFlags flags,
                           void *userdata) {
  SalutAvahiServiceResolver *self = SALUT_AVAHI_SERVICE_RESOLVER (userdata);
  SalutAvahiServiceResolverPrivate *priv = 
    SALUT_AVAHI_SERVICE_RESOLVER_GET_PRIVATE (self);

  switch (event) {
    case AVAHI_RESOLVER_FOUND: {
      /* Double check if this address is always the same */
      printf("--> %p\n", self); 
      priv->address = *a;
      priv->port = port;
      g_signal_emit(self, signals[FOUND], 0,
                    interface, protocol, 
                    name, type,
                    domain, host_name,
                    a, port, txt, flags);
      break;
    }
    case AVAHI_RESOLVER_FAILURE: {
      GError *error;
      int aerrno = avahi_client_errno(priv->client->avahi_client);
      error = g_error_new(SALUT_AVAHI_ERRORS,  aerrno,
                          "Resolving failed: %s", avahi_strerror(aerrno));
      g_signal_emit(self, signals[FAILURE], 0, error);
      g_error_free(error);
      break;
    }
  }
}


SalutAvahiServiceResolver *
salut_avahi_service_resolver_new(const gchar *name, const gchar *type, 
                                 const gchar *domain, 
                                 AvahiProtocol address_protocol,
                                 SalutAvahiLookupFlags flags) {
  return salut_avahi_service_resolver_new_full(AVAHI_IF_UNSPEC,
                                               AVAHI_PROTO_UNSPEC,
                                               name, type, domain,
                                               address_protocol, flags);
}
SalutAvahiServiceResolver *
salut_avahi_service_resolver_new_full(AvahiIfIndex interface, 
                                      AvahiProtocol protocol,
                                      const gchar *name, const gchar *type, 
                                      const gchar *domain, 
                                      AvahiProtocol address_protocol,
                                      SalutAvahiLookupFlags flags) {
  return g_object_new(SALUT_TYPE_AVAHI_SERVICE_RESOLVER,
                      "interface", interface,
                      "protocol", protocol,
                      "name", name,
                      "type", type,
                      "domain", domain,
                      "aprotocol", address_protocol,
                      "flags", flags,
                      NULL);
}

gboolean 
salut_avahi_service_resolver_attach(SalutAvahiServiceResolver *resolver,
                                   SalutAvahiClient *client, GError  **error) {
  SalutAvahiServiceResolverPrivate *priv = 
                              SALUT_AVAHI_SERVICE_RESOLVER_GET_PRIVATE (resolver);

  g_assert(client != NULL);
  g_object_ref(client);
  printf("Resolver attached: %p (%d),  for %s\n", resolver, 
                                                  priv->aprotocol,
                                                  priv->name);

  priv->client = client;

  priv->resolver = avahi_service_resolver_new(client->avahi_client, 
                                            priv->interface, 
                                            priv->protocol, 
                                            priv->name, 
                                            priv->type, priv->domain, 
                                            priv->aprotocol,
                                            priv->flags, 
                                            _avahi_service_resolver_cb, 
                                            resolver);
  if (priv->resolver == NULL) {
    if (error != NULL ) {
      int aerrno = avahi_client_errno(client->avahi_client);
      *error = g_error_new(SALUT_AVAHI_ERRORS, aerrno,
                           "Attaching group failed: %s", 
                           avahi_strerror(aerrno));
    }
    printf("Failed to add resolver\n");
    return FALSE;
  }
  return TRUE;
}

gboolean
salut_avahi_service_resolver_get_address(SalutAvahiServiceResolver *resolver,
                                         AvahiAddress *address,
                                         uint16_t *port) {
  SalutAvahiServiceResolverPrivate *priv = 
                              SALUT_AVAHI_SERVICE_RESOLVER_GET_PRIVATE (resolver);
  if (priv->port == 0) {
    printf("PORT == 0\n");
    return FALSE;
  }
  
  *address = priv->address;
  *port = priv->port;
  return TRUE;
}
