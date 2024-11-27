/*
 * Copyright © 2024 GNOME Foundation Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
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
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Julian Sparber <jsparber@gnome.org>
 */

#include "gnotificationsound-private.h"
#include "gfile.h"
#include "gbytes.h"

/**
 * GNotificationSound:
 *
 * [class@Gio.NotificationSound] holds the sound that should be played when a notification
 * is displayed. Use [method@Gio.Notification.set_sound] to set it for a notification.
 *
 * Since: 2.84
 **/

typedef GObjectClass GNotificationSoundClass;

typedef enum
{
  SOUND_TYPE_DEFAULT,
  SOUND_TYPE_FILE,
  SOUND_TYPE_BYTES,
} SoundType;

struct _GNotificationSound
{
  GObject parent;

  SoundType sound_type;
  union {
    GFile *file;
    GBytes *bytes;
  } data;
};

G_DEFINE_TYPE (GNotificationSound, g_notification_sound, G_TYPE_OBJECT)

static void
g_notification_sound_dispose (GObject *object)
{
  GNotificationSound *sound = G_NOTIFICATION_SOUND (object);

  if (sound->sound_type == SOUND_TYPE_FILE)
    g_clear_object (&sound->data.file);
  else if (sound->sound_type == SOUND_TYPE_BYTES)
    g_clear_pointer (&sound->data.bytes, g_bytes_unref);

  G_OBJECT_CLASS (g_notification_sound_parent_class)->dispose (object);
}

static void
g_notification_sound_class_init (GNotificationSoundClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = g_notification_sound_dispose;
}

static void
g_notification_sound_init (GNotificationSound *notification)
{
}

/**
 * g_notification_sound_new_from_file:
 * @file: A [iface@Gio.File] containing a sound in a common audio format
 *
 * [class@Gio.Notification] using this [class@Gio.NotificationSound] will play the sound in @file
 * when displayed.
 *
 * The sound format `ogg/opus`, `ogg/vorbis` and `wav/pcm` are guaranteed to be supported.
 * Other audio formats may be supported in future.
 *
 * Returns: a new [class@Gio.NotificationSound] instance
 *
 * Since: 2.84
 */
GNotificationSound *
g_notification_sound_new_from_file (GFile *file)
{
  GNotificationSound *sound;

  g_return_val_if_fail (G_IS_FILE (file), NULL);

  sound = g_object_new (G_TYPE_NOTIFICATION_SOUND, NULL);
  sound->data.file = g_object_ref (file);
  sound->sound_type = SOUND_TYPE_FILE;

  return sound;
}

/**
 * g_notification_sound_new_from_bytes:
 * @bytes: [struct@GLib.Bytes] containing a sound in common audio format
 *
 * [class@Gio.Notification] using this [class@Gio.NotificationSound] will play the sound in @bytes
 * when displayed.
 *
 * The sound format `ogg/opus`, `ogg/vorbis` and `wav/pcm` are guaranteed to be supported.
 * Other audio formats may be supported in future.
 *
 * Returns: a new [class@Gio.NotificationSound] instance
 *
 * Since: 2.84
 */
GNotificationSound *
g_notification_sound_new_from_bytes (GBytes *bytes)
{
  GNotificationSound *sound;

  g_return_val_if_fail (bytes != NULL, NULL);

  sound = g_object_new (G_TYPE_NOTIFICATION_SOUND, NULL);
  sound->data.bytes = g_bytes_ref (bytes);
  sound->sound_type = SOUND_TYPE_BYTES;

  return sound;
}

/**
 * g_notification_sound_new_default:
 *
 * [class@Gio.Notification] using this [class@Gio.NotificationSound] will play the default sound when displayed.
 *
 * Returns: a new [class@Gio.NotificationSound] instance.
 *
 * Since: 2.84
 */
GNotificationSound *
g_notification_sound_new_default (void)
{
  GNotificationSound *sound;

  sound = g_object_new (G_TYPE_NOTIFICATION_SOUND, NULL);
  sound->sound_type = SOUND_TYPE_DEFAULT;

  return sound;
}

/*< private >
 * g_notification_sound_get_bytes:
 * @sound: a [class@Gio.NotificationSound]
 *
 * Returns: (nullable): (transfer none): the bytes associated with @sound
 */
GBytes *
g_notification_sound_get_bytes (GNotificationSound *sound)
{
  g_return_val_if_fail (G_IS_NOTIFICATION_SOUND (sound), NULL);

  if (sound->sound_type == SOUND_TYPE_BYTES)
    return sound->data.bytes;
  else
    return NULL;
}

/*< private >
 * g_notification_sound_get_file:
 * @sound: a [class@Gio.NotificationSound]
 *
 * Returns: (nullable): (transfer none): the [iface@Gio.File] associated with @sound
 */
GFile *
g_notification_sound_get_file (GNotificationSound *sound)
{
  g_return_val_if_fail (G_IS_NOTIFICATION_SOUND (sound), NULL);

  if (sound->sound_type == SOUND_TYPE_FILE)
    return sound->data.file;
  else
    return NULL;
}

/*< private >
 * g_notification_sound_is_default:
 * @sound: a [class@Gio.NotificationSound]
 *
 * Returns: whether this @sound is uses the default.
 */
gboolean
g_notification_sound_is_default (GNotificationSound *sound)
{
  g_return_val_if_fail (G_IS_NOTIFICATION_SOUND (sound), FALSE);

  return sound->sound_type == SOUND_TYPE_DEFAULT;
}
