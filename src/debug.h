
#ifndef __DEBUG_H__
#define __DEBUG_H_

#include "config.h"

#include <glib.h>

G_BEGIN_DECLS

void debug_set_log_file_from_env (void);

G_END_DECLS

#ifdef ENABLE_DEBUG

typedef enum
{
  DEBUG_PRESENCE       = 1 << 0,
  DEBUG_GROUPS         = 1 << 1,
  DEBUG_CONTACTS       = 1 << 2,
  DEBUG_DISCO          = 1 << 3,
  DEBUG_PROPERTIES     = 1 << 4,
  DEBUG_ROOMLIST       = 1 << 5,
  DEBUG_MEDIA          = 1 << 6,
  DEBUG_MUC            = 1 << 7,
  DEBUG_MUC_CONNECTION = 1 << 8,
  DEBUG_CONNECTION     = 1 << 9,
  DEBUG_IM             = 1 << 10,
  DEBUG_PERSIST        = 1 << 11,
  DEBUG_NET            = 1 << 12,
  DEBUG_SELF           = 1 << 13
} DebugFlags;

void debug_set_flags_from_env ();
void debug_set_flags (DebugFlags flags);
gboolean debug_flag_is_set (DebugFlags flag);
void debug (DebugFlags flag, const gchar *format, ...)
    G_GNUC_PRINTF (2, 3);
void debug_stanza (DebugFlags flag, GibberXmppStanza *stanza, const gchar *format, ...)
    G_GNUC_PRINTF (3, 4);

#ifdef DEBUG_FLAG

#define DEBUG(format, ...) \
  debug(DEBUG_FLAG, "%s: " format, G_STRFUNC, ##__VA_ARGS__)

#define DEBUG_STANZA(stanza, format, ...) \
  debug_stanza(DEBUG_FLAG, stanza, "%s: " format, G_STRFUNC, ##__VA_ARGS__)

#define DEBUGGING debug_flag_is_set(DEBUG_FLAG)

#endif /* DEBUG_FLAG */

#else /* ENABLE_DEBUG */

#ifdef DEBUG_FLAG

#define DEBUG(format, ...)

#define DEBUG_STANZA(stamza, format, ...)

#define DEBUGGING 0

#define NODE_DEBUG(n, s)

#endif /* DEBUG_FLAG */

#endif /* ENABLE_DEBUG */

G_END_DECLS

#endif
