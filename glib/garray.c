/* GLIB - Library of useful routines for C programming
 * Copyright (C) 1995-1997  Peter Mattis, Spencer Kimball and Josh MacDonald
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
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Modified by the GLib Team and others 1997-2000.  See the AUTHORS
 * file for a list of people on the GLib Team.  See the ChangeLog
 * files for a list of changes.  These files are distributed with
 * GLib at ftp://ftp.gtk.org/pub/gtk/. 
 */

/* 
 * MT safe
 */

#include "config.h"

#include <string.h>
#include <stdlib.h>

#include "garray.h"

#include "galloca.h"
#include "gbytes.h"
#include "ghash.h"
#include "gslice.h"
#include "gmem.h"
#include "gtestutils.h"
#include "gthread.h"
#include "gmessages.h"
#include "gqsort.h"
#include "grefcount.h"
#include "gutilsprivate.h"

#define MIN_ARRAY_SIZE  16

typedef struct _GRealArray  GRealArray;

/**
 * GArray:
 * @data: a pointer to the element data. The data may be moved as
 *     elements are added to the #GArray.
 * @len: the number of elements in the #GArray not including the
 *     possible terminating zero element.
 *
 * Contains the public fields of a GArray.
 */
struct _GRealArray
{
  guint8 *data;
  guint   len;
  guint   elt_capacity;
  guint   elt_size;
  guint   zero_terminated : 1;
  guint   clear : 1;
  gatomicrefcount ref_count;
  GDestroyNotify clear_func;
};

/**
 * g_array_index:
 * @a: a #GArray
 * @t: the type of the elements
 * @i: the index of the element to return
 *
 * Returns the element of a #GArray at the given index. The return
 * value is cast to the given type. This is the main way to read or write an
 * element in a #GArray.
 *
 * Writing an element is typically done by reference, as in the following
 * example. This example gets a pointer to an element in a #GArray, and then
 * writes to a field in it:
 * |[<!-- language="C" -->
 *   EDayViewEvent *event;
 *   // This gets a pointer to the 4th element in the array of
 *   // EDayViewEvent structs.
 *   event = &g_array_index (events, EDayViewEvent, 3);
 *   event->start_time = g_get_current_time ();
 * ]|
 *
 * This example reads from and writes to an array of integers:
 * |[<!-- language="C" -->
 *   g_autoptr(GArray) int_array = g_array_new (FALSE, FALSE, sizeof (guint));
 *   for (guint i = 0; i < 10; i++)
 *     g_array_append_val (int_array, i);
 *
 *   guint *my_int = &g_array_index (int_array, guint, 1);
 *   g_print ("Int at index 1 is %u; decrementing it\n", *my_int);
 *   *my_int = *my_int - 1;
 * ]|
 *
 * Returns: the element of the #GArray at the index given by @i
 */

#define g_array_elt_len(array,i) ((gsize)(array)->elt_size * (i))
#define g_array_elt_pos(array,i) ((array)->data + g_array_elt_len((array),(i)))
#define g_array_elt_zero(array, pos, len)                               \
  (memset (g_array_elt_pos ((array), pos), 0,  g_array_elt_len ((array), len)))
#define g_array_zero_terminate(array) G_STMT_START{                     \
  if ((array)->zero_terminated)                                         \
    g_array_elt_zero ((array), (array)->len, 1);                        \
}G_STMT_END

static void  g_array_maybe_expand (GRealArray *array,
                                   guint       len);

/**
 * g_array_new:
 * @zero_terminated: %TRUE if the array should have an extra element at
 *     the end which is set to 0
 * @clear_: %TRUE if #GArray elements should be automatically cleared
 *     to 0 when they are allocated
 * @element_size: the size of each element in bytes
 *
 * Creates a new #GArray with a reference count of 1.
 *
 * Returns: the new #GArray
 */
GArray*
g_array_new (gboolean zero_terminated,
             gboolean clear,
             guint    elt_size)
{
  g_return_val_if_fail (elt_size > 0, NULL);
#if (UINT_WIDTH / 8) >= GLIB_SIZEOF_SIZE_T
  g_return_val_if_fail (elt_size <= G_MAXSIZE / 2 - 1, NULL);
#endif

  return g_array_sized_new (zero_terminated, clear, elt_size, 0);
}

/**
 * g_array_new_take: (skip)
 * @data: (array length=len) (transfer full) (nullable): an array of
 *   elements of @element_size, or %NULL for an empty array
 * @len: the number of elements in @data
 * @clear: %TRUE if #GArray elements should be automatically cleared
 *     to 0 when they are allocated
 * @element_size: the size of each element in bytes
 *
 * Creates a new #GArray with @data as array data, @len as length and a
 * reference count of 1.
 *
 * This avoids having to copy the data manually, when it can just be
 * inherited.
 * After this call, @data belongs to the #GArray and may no longer be
 * modified by the caller. The memory of @data has to be dynamically
 * allocated and will eventually be freed with g_free().
 *
 * In case the elements need to be cleared when the array is freed, use
 * g_array_set_clear_func() to set a #GDestroyNotify function to perform
 * such task.
 *
 * Do not use it if @len or @element_size are greater than %G_MAXUINT.
 * #GArray stores the length of its data in #guint, which may be shorter
 * than #gsize.
 *
 * Returns: (transfer full): A new #GArray
 *
 * Since: 2.76
 */
GArray *
g_array_new_take (gpointer  data,
                  gsize     len,
                  gboolean  clear,
                  gsize     element_size)
{
  GRealArray *rarray;
  GArray *array;

  g_return_val_if_fail (data != NULL || len == 0, NULL);
  g_return_val_if_fail (len <= G_MAXUINT, NULL);
  g_return_val_if_fail (element_size > 0 && element_size <= G_MAXUINT, NULL);

  array = g_array_sized_new (FALSE, clear, element_size, 0);
  rarray = (GRealArray *) array;
  rarray->data = (guint8 *) g_steal_pointer (&data);
  rarray->len = len;
  rarray->elt_capacity = len;

  return array;
}

/**
 * g_array_new_take_zero_terminated: (skip)
 * @data: (array zero-terminated=1) (transfer full) (nullable): an array
 *     of elements of @element_size, %NULL terminated,
 *     or %NULL for an empty array
 * @clear: %TRUE if #GArray elements should be automatically cleared
 *     to 0 when they are allocated
 * @element_size: the size of each element in bytes
 *
 * Creates a new #GArray with @data as array data, computing the length of it
 * and setting the reference count to 1.
 *
 * This avoids having to copy the data manually, when it can just be
 * inherited.
 * After this call, @data belongs to the #GArray and may no longer be
 * modified by the caller. The memory of @data has to be dynamically
 * allocated and will eventually be freed with g_free().
 *
 * The length is calculated by iterating through @data until the first %NULL
 * element is found.
 *
 * In case the elements need to be cleared when the array is freed, use
 * g_array_set_clear_func() to set a #GDestroyNotify function to perform
 * such task.
 *
 * Do not use it if @data length or @element_size are greater than %G_MAXUINT.
 * #GArray stores the length of its data in #guint, which may be shorter
 * than #gsize.
 *
 * Returns: (transfer full): A new #GArray
 *
 * Since: 2.76
 */
GArray *
g_array_new_take_zero_terminated (gpointer  data,
                                  gboolean  clear,
                                  gsize     element_size)
{
  GRealArray *rarray;
  GArray *array;
  gsize len = 0;

  g_return_val_if_fail (element_size > 0 && element_size <= G_MAXUINT, NULL);

  if (data != NULL)
    {
      guint8 *array_data = data;

      for (gsize i = 0; ; ++i)
        {
          const guint8 *element_start = array_data + (i * element_size);

          if (*element_start == 0 &&
              memcmp (element_start, element_start + 1, element_size - 1) == 0)
            break;

          len += 1;
        }
    }

  g_return_val_if_fail (len < G_MAXUINT, NULL);

  array = g_array_new_take (data, len, clear, element_size);
  rarray = (GRealArray *) array;
  rarray->zero_terminated = TRUE;
  if (G_LIKELY (rarray->data != NULL))
    rarray->elt_capacity = len + 1;

  return array;
}

/**
 * g_array_steal:
 * @array: a #GArray.
 * @len: (optional) (out): pointer to retrieve the number of
 *    elements of the original array
 *
 * Frees the data in the array and resets the size to zero, while
 * the underlying array is preserved for use elsewhere and returned
 * to the caller.
 *
 * Note that if the array was created with the @zero_terminate
 * property set to %TRUE, this may still return %NULL if the length
 * of the array was zero and data was not yet allocated.
 *
 * If array elements contain dynamically-allocated memory,
 * the array elements should also be freed by the caller.
 *
 * A short example of use:
 * |[<!-- language="C" -->
 * ...
 * gpointer data;
 * gsize data_len;
 * data = g_array_steal (some_array, &data_len);
 * ...
 * ]|

 * Returns: (transfer full): the element data, which should be
 *     freed using g_free().
 *
 * Since: 2.64
 */
gpointer
g_array_steal (GArray *array,
               gsize *len)
{
  GRealArray *rarray;
  gpointer segment;

  g_return_val_if_fail (array != NULL, NULL);

  rarray = (GRealArray *) array;
  segment = (gpointer) rarray->data;

  if (len != NULL)
    *len = rarray->len;

  rarray->data  = NULL;
  rarray->len   = 0;
  rarray->elt_capacity = 0;
  return segment;
}

/**
 * g_array_sized_new:
 * @zero_terminated: %TRUE if the array should have an extra element at
 *     the end with all bits cleared
 * @clear_: %TRUE if all bits in the array should be cleared to 0 on
 *     allocation
 * @element_size: size of each element in the array
 * @reserved_size: number of elements preallocated
 *
 * Creates a new #GArray with @reserved_size elements preallocated and
 * a reference count of 1. This avoids frequent reallocation, if you
 * are going to add many elements to the array. Note however that the
 * size of the array is still 0.
 *
 * Returns: the new #GArray
 */
GArray*
g_array_sized_new (gboolean zero_terminated,
                   gboolean clear,
                   guint    elt_size,
                   guint    reserved_size)
{
  GRealArray *array;
  
  g_return_val_if_fail (elt_size > 0, NULL);
#if (UINT_WIDTH / 8) >= GLIB_SIZEOF_SIZE_T
  g_return_val_if_fail (elt_size <= G_MAXSIZE / 2 - 1, NULL);
#endif

  array = g_slice_new (GRealArray);

  array->data            = NULL;
  array->len             = 0;
  array->elt_capacity = 0;
  array->zero_terminated = (zero_terminated ? 1 : 0);
  array->clear           = (clear ? 1 : 0);
  array->elt_size        = elt_size;
  array->clear_func      = NULL;

  g_atomic_ref_count_init (&array->ref_count);

  if (array->zero_terminated || reserved_size != 0)
    {
      g_array_maybe_expand (array, reserved_size);
      g_assert (array->data != NULL);
      g_array_zero_terminate (array);
    }

  return (GArray*) array;
}

/**
 * g_array_set_clear_func:
 * @array: A #GArray
 * @clear_func: a function to clear an element of @array
 *
 * Sets a function to clear an element of @array.
 *
 * The @clear_func will be called when an element in the array
 * data segment is removed and when the array is freed and data
 * segment is deallocated as well. @clear_func will be passed a
 * pointer to the element to clear, rather than the element itself.
 *
 * Note that in contrast with other uses of #GDestroyNotify
 * functions, @clear_func is expected to clear the contents of
 * the array element it is given, but not free the element itself.
 *
 * |[<!-- language="C" -->
 * typedef struct
 * {
 *   gchar *str;
 *   GObject *obj;
 * } ArrayElement;
 *
 * static void
 * array_element_clear (ArrayElement *element)
 * {
 *   g_clear_pointer (&element->str, g_free);
 *   g_clear_object (&element->obj);
 * }
 *
 * // main code
 * GArray *garray = g_array_new (FALSE, FALSE, sizeof (ArrayElement));
 * g_array_set_clear_func (garray, (GDestroyNotify) array_element_clear);
 * // assign data to the structure
 * g_array_free (garray, TRUE);
 * ]|
 *
 * Since: 2.32
 */
void
g_array_set_clear_func (GArray         *array,
                        GDestroyNotify  clear_func)
{
  GRealArray *rarray = (GRealArray *) array;

  g_return_if_fail (array != NULL);

  rarray->clear_func = clear_func;
}

/**
 * g_array_ref:
 * @array: A #GArray
 *
 * Atomically increments the reference count of @array by one.
 * This function is thread-safe and may be called from any thread.
 *
 * Returns: The passed in #GArray
 *
 * Since: 2.22
 */
GArray *
g_array_ref (GArray *array)
{
  GRealArray *rarray = (GRealArray*) array;
  g_return_val_if_fail (array, NULL);

  g_atomic_ref_count_inc (&rarray->ref_count);

  return array;
}

typedef enum
{
  FREE_SEGMENT = 1 << 0,
  PRESERVE_WRAPPER = 1 << 1
} ArrayFreeFlags;

static gchar *array_free (GRealArray *, ArrayFreeFlags);

/**
 * g_array_unref:
 * @array: A #GArray
 *
 * Atomically decrements the reference count of @array by one. If the
 * reference count drops to 0, the effect is the same as calling
 * g_array_free() with @free_segment set to %TRUE. This function is
 * thread-safe and may be called from any thread.
 *
 * Since: 2.22
 */
void
g_array_unref (GArray *array)
{
  GRealArray *rarray = (GRealArray*) array;
  g_return_if_fail (array);

  if (g_atomic_ref_count_dec (&rarray->ref_count))
    array_free (rarray, FREE_SEGMENT);
}

/**
 * g_array_get_element_size:
 * @array: A #GArray
 *
 * Gets the size of the elements in @array.
 *
 * Returns: Size of each element, in bytes
 *
 * Since: 2.22
 */
guint
g_array_get_element_size (GArray *array)
{
  GRealArray *rarray = (GRealArray*) array;

  g_return_val_if_fail (array, 0);

  return rarray->elt_size;
}

/**
 * g_array_free:
 * @array: a #GArray
 * @free_segment: if %TRUE the actual element data is freed as well
 *
 * Frees the memory allocated for the #GArray. If @free_segment is
 * %TRUE it frees the memory block holding the elements as well. Pass
 * %FALSE if you want to free the #GArray wrapper but preserve the
 * underlying array for use elsewhere. If the reference count of
 * @array is greater than one, the #GArray wrapper is preserved but
 * the size of  @array will be set to zero.
 *
 * If array contents point to dynamically-allocated memory, they should
 * be freed separately if @free_segment is %TRUE and no @clear_func
 * function has been set for @array.
 *
 * This function is not thread-safe. If using a #GArray from multiple
 * threads, use only the atomic g_array_ref() and g_array_unref()
 * functions.
 *
 * Returns: the element data if @free_segment is %FALSE, otherwise
 *     %NULL. The element data should be freed using g_free().
 */
gchar*
g_array_free (GArray   *farray,
              gboolean  free_segment)
{
  GRealArray *array = (GRealArray*) farray;
  ArrayFreeFlags flags;

  g_return_val_if_fail (array, NULL);

  flags = (free_segment ? FREE_SEGMENT : 0);

  /* if others are holding a reference, preserve the wrapper but do free/return the data */
  if (!g_atomic_ref_count_dec (&array->ref_count))
    flags |= PRESERVE_WRAPPER;

  return array_free (array, flags);
}

static gchar *
array_free (GRealArray     *array,
            ArrayFreeFlags  flags)
{
  gchar *segment;

  if (flags & FREE_SEGMENT)
    {
      if (array->clear_func != NULL)
        {
          guint i;

          for (i = 0; i < array->len; i++)
            array->clear_func (g_array_elt_pos (array, i));
        }

      g_free (array->data);
      segment = NULL;
    }
  else
    segment = (gchar*) array->data;

  if (flags & PRESERVE_WRAPPER)
    {
      array->data            = NULL;
      array->len             = 0;
      array->elt_capacity = 0;
    }
  else
    {
      g_slice_free1 (sizeof (GRealArray), array);
    }

  return segment;
}

/**
 * g_array_append_vals:
 * @array: a #GArray
 * @data: (not nullable): a pointer to the elements to append to the end of the array
 * @len: the number of elements to append
 *
 * Adds @len elements onto the end of the array.
 *
 * Returns: the #GArray
 */
/**
 * g_array_append_val:
 * @a: a #GArray
 * @v: the value to append to the #GArray
 *
 * Adds the value on to the end of the array. The array will grow in
 * size automatically if necessary.
 *
 * g_array_append_val() is a macro which uses a reference to the value
 * parameter @v. This means that you cannot use it with literal values
 * such as "27". You must use variables.
 *
 * Returns: the #GArray
 */
GArray*
g_array_append_vals (GArray       *farray,
                     gconstpointer data,
                     guint         len)
{
  GRealArray *array = (GRealArray*) farray;

  g_return_val_if_fail (array, NULL);

  if (len == 0)
    return farray;

  g_array_maybe_expand (array, len);

  memcpy (g_array_elt_pos (array, array->len), data, 
          g_array_elt_len (array, len));

  array->len += len;

  g_array_zero_terminate (array);

  return farray;
}

/**
 * g_array_prepend_vals:
 * @array: a #GArray
 * @data: (nullable): a pointer to the elements to prepend to the start of the array
 * @len: the number of elements to prepend, which may be zero
 *
 * Adds @len elements onto the start of the array.
 *
 * @data may be %NULL if (and only if) @len is zero. If @len is zero, this
 * function is a no-op.
 *
 * This operation is slower than g_array_append_vals() since the
 * existing elements in the array have to be moved to make space for
 * the new elements.
 *
 * Returns: the #GArray
 */
/**
 * g_array_prepend_val:
 * @a: a #GArray
 * @v: the value to prepend to the #GArray
 *
 * Adds the value on to the start of the array. The array will grow in
 * size automatically if necessary.
 *
 * This operation is slower than g_array_append_val() since the
 * existing elements in the array have to be moved to make space for
 * the new element.
 *
 * g_array_prepend_val() is a macro which uses a reference to the value
 * parameter @v. This means that you cannot use it with literal values
 * such as "27". You must use variables.
 *
 * Returns: the #GArray
 */
GArray*
g_array_prepend_vals (GArray        *farray,
                      gconstpointer  data,
                      guint          len)
{
  GRealArray *array = (GRealArray*) farray;

  g_return_val_if_fail (array, NULL);

  if (len == 0)
    return farray;

  g_array_maybe_expand (array, len);

  memmove (g_array_elt_pos (array, len), g_array_elt_pos (array, 0),
           g_array_elt_len (array, array->len));

  memcpy (g_array_elt_pos (array, 0), data, g_array_elt_len (array, len));

  array->len += len;

  g_array_zero_terminate (array);

  return farray;
}

/**
 * g_array_insert_vals:
 * @array: a #GArray
 * @index_: the index to place the elements at
 * @data: (nullable): a pointer to the elements to insert
 * @len: the number of elements to insert
 *
 * Inserts @len elements into a #GArray at the given index.
 *
 * If @index_ is greater than the array’s current length, the array is expanded.
 * The elements between the old end of the array and the newly inserted elements
 * will be initialised to zero if the array was configured to clear elements;
 * otherwise their values will be undefined.
 *
 * If @index_ is less than the array’s current length, new entries will be
 * inserted into the array, and the existing entries above @index_ will be moved
 * upwards.
 *
 * @data may be %NULL if (and only if) @len is zero. If @len is zero, this
 * function is a no-op.
 *
 * Returns: the #GArray
 */
/**
 * g_array_insert_val:
 * @a: a #GArray
 * @i: the index to place the element at
 * @v: the value to insert into the array
 *
 * Inserts an element into an array at the given index.
 *
 * g_array_insert_val() is a macro which uses a reference to the value
 * parameter @v. This means that you cannot use it with literal values
 * such as "27". You must use variables.
 *
 * Returns: the #GArray
 */
GArray*
g_array_insert_vals (GArray        *farray,
                     guint          index_,
                     gconstpointer  data,
                     guint          len)
{
  GRealArray *array = (GRealArray*) farray;

  g_return_val_if_fail (array, NULL);

  if (len == 0)
    return farray;

  /* Is the index off the end of the array, and hence do we need to over-allocate
   * and clear some elements? */
  if (index_ >= array->len)
    {
      g_array_maybe_expand (array, index_ - array->len + len);
      return g_array_append_vals (g_array_set_size (farray, index_), data, len);
    }

  g_array_maybe_expand (array, len);

  memmove (g_array_elt_pos (array, len + index_),
           g_array_elt_pos (array, index_),
           g_array_elt_len (array, array->len - index_));

  memcpy (g_array_elt_pos (array, index_), data, g_array_elt_len (array, len));

  array->len += len;

  g_array_zero_terminate (array);

  return farray;
}

/**
 * g_array_set_size:
 * @array: a #GArray
 * @length: the new size of the #GArray
 *
 * Sets the size of the array, expanding it if necessary. If the array
 * was created with @clear_ set to %TRUE, the new elements are set to 0.
 *
 * Returns: the #GArray
 */
GArray*
g_array_set_size (GArray *farray,
                  guint   length)
{
  GRealArray *array = (GRealArray*) farray;

  g_return_val_if_fail (array, NULL);

  if (length > array->len)
    {
      g_array_maybe_expand (array, length - array->len);
      
      if (array->clear)
        g_array_elt_zero (array, array->len, length - array->len);
    }
  else if (length < array->len)
    g_array_remove_range (farray, length, array->len - length);

  array->len = length;

  if (G_LIKELY (array->data != NULL))
    g_array_zero_terminate (array);

  return farray;
}

/**
 * g_array_remove_index:
 * @array: a #GArray
 * @index_: the index of the element to remove
 *
 * Removes the element at the given index from a #GArray. The following
 * elements are moved down one place.
 *
 * Returns: the #GArray
 */
GArray*
g_array_remove_index (GArray *farray,
                      guint   index_)
{
  GRealArray* array = (GRealArray*) farray;

  g_return_val_if_fail (array, NULL);

  g_return_val_if_fail (index_ < array->len, NULL);

  if (array->clear_func != NULL)
    array->clear_func (g_array_elt_pos (array, index_));

  if (index_ != array->len - 1)
    memmove (g_array_elt_pos (array, index_),
             g_array_elt_pos (array, index_ + 1),
             g_array_elt_len (array, array->len - index_ - 1));

  array->len -= 1;

  if (G_UNLIKELY (g_mem_gc_friendly))
    g_array_elt_zero (array, array->len, 1);
  else
    g_array_zero_terminate (array);

  return farray;
}

/**
 * g_array_remove_index_fast:
 * @array: a @GArray
 * @index_: the index of the element to remove
 *
 * Removes the element at the given index from a #GArray. The last
 * element in the array is used to fill in the space, so this function
 * does not preserve the order of the #GArray. But it is faster than
 * g_array_remove_index().
 *
 * Returns: the #GArray
 */
GArray*
g_array_remove_index_fast (GArray *farray,
                           guint   index_)
{
  GRealArray* array = (GRealArray*) farray;

  g_return_val_if_fail (array, NULL);

  g_return_val_if_fail (index_ < array->len, NULL);

  if (array->clear_func != NULL)
    array->clear_func (g_array_elt_pos (array, index_));

  if (index_ != array->len - 1)
    memcpy (g_array_elt_pos (array, index_),
            g_array_elt_pos (array, array->len - 1),
            g_array_elt_len (array, 1));
  
  array->len -= 1;

  if (G_UNLIKELY (g_mem_gc_friendly))
    g_array_elt_zero (array, array->len, 1);
  else
    g_array_zero_terminate (array);

  return farray;
}

/**
 * g_array_remove_range:
 * @array: a @GArray
 * @index_: the index of the first element to remove
 * @length: the number of elements to remove
 *
 * Removes the given number of elements starting at the given index
 * from a #GArray.  The following elements are moved to close the gap.
 *
 * Returns: the #GArray
 *
 * Since: 2.4
 */
GArray*
g_array_remove_range (GArray *farray,
                      guint   index_,
                      guint   length)
{
  GRealArray *array = (GRealArray*) farray;

  g_return_val_if_fail (array, NULL);
  g_return_val_if_fail (index_ <= array->len, NULL);
  g_return_val_if_fail (index_ <= G_MAXUINT - length, NULL);
  g_return_val_if_fail (index_ + length <= array->len, NULL);

  if (length == 0)
    return farray;

  if (array->clear_func != NULL)
    {
      guint i;

      for (i = 0; i < length; i++)
        array->clear_func (g_array_elt_pos (array, index_ + i));
    }

  if (index_ + length != array->len)
    memmove (g_array_elt_pos (array, index_),
             g_array_elt_pos (array, index_ + length),
             g_array_elt_len (array, array->len - (index_ + length)));

  array->len -= length;
  if (G_UNLIKELY (g_mem_gc_friendly))
    g_array_elt_zero (array, array->len, length);
  else
    g_array_zero_terminate (array);

  return farray;
}

/**
 * g_array_sort:
 * @array: a #GArray
 * @compare_func: comparison function
 *
 * Sorts a #GArray using @compare_func which should be a qsort()-style
 * comparison function (returns less than zero for first arg is less
 * than second arg, zero for equal, greater zero if first arg is
 * greater than second arg).
 *
 * This is guaranteed to be a stable sort since version 2.32.
 */
void
g_array_sort (GArray       *farray,
              GCompareFunc  compare_func)
{
  GRealArray *array = (GRealArray*) farray;

  g_return_if_fail (array != NULL);

  /* Don't use qsort as we want a guaranteed stable sort */
  if (array->len > 0)
    g_sort_array (array->data,
                  array->len,
                  array->elt_size,
                  (GCompareDataFunc) compare_func,
                  NULL);
}

/**
 * g_array_sort_with_data:
 * @array: a #GArray
 * @compare_func: comparison function
 * @user_data: data to pass to @compare_func
 *
 * Like g_array_sort(), but the comparison function receives an extra
 * user data argument.
 *
 * This is guaranteed to be a stable sort since version 2.32.
 *
 * There used to be a comment here about making the sort stable by
 * using the addresses of the elements in the comparison function.
 * This did not actually work, so any such code should be removed.
 */
void
g_array_sort_with_data (GArray           *farray,
                        GCompareDataFunc  compare_func,
                        gpointer          user_data)
{
  GRealArray *array = (GRealArray*) farray;

  g_return_if_fail (array != NULL);

  if (array->len > 0)
    g_sort_array (array->data,
                  array->len,
                  array->elt_size,
                  compare_func,
                  user_data);
}

/**
 * g_array_binary_search:
 * @array: a #GArray.
 * @target: a pointer to the item to look up.
 * @compare_func: A #GCompareFunc used to locate @target.
 * @out_match_index: (optional) (out): return location
 *    for the index of the element, if found.
 *
 * Checks whether @target exists in @array by performing a binary
 * search based on the given comparison function @compare_func which
 * get pointers to items as arguments. If the element is found, %TRUE
 * is returned and the element’s index is returned in @out_match_index
 * (if non-%NULL). Otherwise, %FALSE is returned and @out_match_index
 * is undefined. If @target exists multiple times in @array, the index
 * of the first instance is returned. This search is using a binary
 * search, so the @array must absolutely be sorted to return a correct
 * result (if not, the function may produce false-negative).
 *
 * This example defines a comparison function and search an element in a #GArray:
 * |[<!-- language="C" -->
 * static gint
 * cmpint (gconstpointer a, gconstpointer b)
 * {
 *   const gint *_a = a;
 *   const gint *_b = b;
 *
 *   return *_a - *_b;
 * }
 * ...
 * gint i = 424242;
 * guint matched_index;
 * gboolean result = g_array_binary_search (garray, &i, cmpint, &matched_index);
 * ...
 * ]|
 *
 * Returns: %TRUE if @target is one of the elements of @array, %FALSE otherwise.
 *
 * Since: 2.62
 */
gboolean
g_array_binary_search (GArray        *array,
                       gconstpointer  target,
                       GCompareFunc   compare_func,
                       guint         *out_match_index)
{
  gboolean result = FALSE;
  GRealArray *_array = (GRealArray *) array;
  guint left, middle = 0, right;
  gint val;

  g_return_val_if_fail (_array != NULL, FALSE);
  g_return_val_if_fail (compare_func != NULL, FALSE);

  if (G_LIKELY(_array->len))
    {
      left = 0;
      right = _array->len - 1;

      while (left <= right)
        {
          middle = left + (right - left) / 2;

          val = compare_func (g_array_elt_pos (_array, middle), target);
          if (val == 0)
            {
              result = TRUE;
              break;
            }
          else if (val < 0)
            left = middle + 1;
          else if (/* val > 0 && */ middle > 0)
            right = middle - 1;
          else
            break;  /* element not found */
        }
    }

  if (result && out_match_index != NULL)
    *out_match_index = middle;

  return result;
}

static void
g_array_maybe_expand (GRealArray *array,
                      guint       len)
{
  guint max_len, want_len;
 
  /* The maximum array length is derived from following constraints:
   * - The number of bytes must fit into a gsize / 2.
   * - The number of elements must fit into guint.
   * - zero terminated arrays must leave space for the terminating element
   */
  max_len = MIN (G_MAXSIZE / 2 / array->elt_size, G_MAXUINT) - array->zero_terminated;

  /* Detect potential overflow */
  if G_UNLIKELY ((max_len - array->len) < len)
    g_error ("adding %u to array would overflow", len);

  want_len = array->len + len + array->zero_terminated;
  if (want_len > array->elt_capacity)
    {
      gsize want_alloc = g_nearest_pow (g_array_elt_len (array, want_len));
      g_assert (want_alloc >= g_array_elt_len (array, want_len));
      want_alloc = MAX (want_alloc, MIN_ARRAY_SIZE);

      array->data = g_realloc (array->data, want_alloc);

      if (G_UNLIKELY (g_mem_gc_friendly))
        memset (g_array_elt_pos (array, array->elt_capacity), 0,
                g_array_elt_len (array, want_len - array->elt_capacity));

      array->elt_capacity = MIN (want_alloc / array->elt_size, G_MAXUINT);
    }
}

typedef struct _GRealPtrArray  GRealPtrArray;

/**
 * GPtrArray:
 * @pdata: points to the array of pointers, which may be moved when the
 *     array grows
 * @len: number of pointers in the array
 *
 * Contains the public fields of a pointer array.
 */
struct _GRealPtrArray
{
  gpointer       *pdata;
  guint           len;
  guint           alloc;
  gatomicrefcount ref_count;
  guint8          null_terminated : 1; /* always either 0 or 1, so it can be added to array lengths */
  GDestroyNotify  element_free_func;
};

/**
 * g_ptr_array_index:
 * @array: a #GPtrArray
 * @index_: the index of the pointer to return
 *
 * Returns the pointer at the given index of the pointer array.
 *
 * This does not perform bounds checking on the given @index_,
 * so you are responsible for checking it against the array length.
 *
 * Returns: the pointer at the given index
 */

static void g_ptr_array_maybe_expand (GRealPtrArray *array,
                                      guint          len);

static void
ptr_array_maybe_null_terminate (GRealPtrArray *rarray)
{
  if (G_UNLIKELY (rarray->null_terminated))
    rarray->pdata[rarray->len] = NULL;
}

static GPtrArray *
ptr_array_new (guint reserved_size,
               GDestroyNotify element_free_func,
               gboolean null_terminated)
{
  GRealPtrArray *array;

  array = g_slice_new (GRealPtrArray);

  array->pdata = NULL;
  array->len = 0;
  array->alloc = 0;
  array->null_terminated = null_terminated ? 1 : 0;
  array->element_free_func = element_free_func;

  g_atomic_ref_count_init (&array->ref_count);

  if (reserved_size != 0)
    {
      g_ptr_array_maybe_expand (array, reserved_size);
      g_assert (array->pdata != NULL);

      if (null_terminated)
        {
          /* don't use ptr_array_maybe_null_terminate(). It helps the compiler
           * to see when @null_terminated is false and thereby inline
           * ptr_array_new() and possibly remove the code entirely. */
          array->pdata[0] = NULL;
        }
    }

  return (GPtrArray *) array;
}

/**
 * g_ptr_array_new:
 *
 * Creates a new #GPtrArray with a reference count of 1.
 *
 * Returns: the new #GPtrArray
 */
GPtrArray*
g_ptr_array_new (void)
{
  return ptr_array_new (0, NULL, FALSE);
}

/**
 * g_ptr_array_new_take: (skip)
 * @data: (array length=len) (transfer full) (nullable): an array of pointers,
 *    or %NULL for an empty array
 * @len: the number of pointers in @data
 * @element_free_func: (nullable): A function to free elements on @array
 *   destruction or %NULL
 *
 * Creates a new #GPtrArray with @data as pointers, @len as length and a
 * reference count of 1.
 *
 * This avoids having to copy such data manually.
 * After this call, @data belongs to the #GPtrArray and may no longer be
 * modified by the caller. The memory of @data has to be dynamically
 * allocated and will eventually be freed with g_free().
 *
 * It also sets @element_free_func for freeing each element when the array is
 * destroyed either via g_ptr_array_unref(), when g_ptr_array_free() is called
 * with @free_segment set to %TRUE or when removing elements.
 *
 * Do not use it if @len is greater than %G_MAXUINT. #GPtrArray
 * stores the length of its data in #guint, which may be shorter than
 * #gsize.
 *
 * Returns: (transfer full): A new #GPtrArray
 *
 * Since: 2.76
 */
GPtrArray *
g_ptr_array_new_take (gpointer       *data,
                      gsize           len,
                      GDestroyNotify  element_free_func)
{
  GPtrArray *array;
  GRealPtrArray *rarray;

  g_return_val_if_fail (data != NULL || len == 0, NULL);
  g_return_val_if_fail (len <= G_MAXUINT, NULL);

  array = ptr_array_new (0, element_free_func, FALSE);
  rarray = (GRealPtrArray *)array;

  rarray->pdata = g_steal_pointer (&data);
  rarray->len = len;
  rarray->alloc = len;

  return array;
}

/**
 * g_ptr_array_new_take_null_terminated: (skip)
 * @data: (array zero-terminated=1) (transfer full) (nullable): an array
 *  of pointers, %NULL terminated, or %NULL for an empty array
 * @element_free_func: (nullable): a function to free elements on @array
 *   destruction or %NULL
 *
 * Creates a new #GPtrArray with @data as pointers, computing the length of it
 * and setting the reference count to 1.
 *
 * This avoids having to copy such data manually.
 * After this call, @data belongs to the #GPtrArray and may no longer be
 * modified by the caller. The memory of @data has to be dynamically
 * allocated and will eventually be freed with g_free().
 *
 * The length is calculated by iterating through @data until the first %NULL
 * element is found.
 *
 * It also sets @element_free_func for freeing each element when the array is
 * destroyed either via g_ptr_array_unref(), when g_ptr_array_free() is called
 * with @free_segment set to %TRUE or when removing elements.
 *
 * Do not use it if the @data length is greater than %G_MAXUINT. #GPtrArray
 * stores the length of its data in #guint, which may be shorter than
 * #gsize.
 *
 * Returns: (transfer full): A new #GPtrArray
 *
 * Since: 2.76
 */
GPtrArray *
g_ptr_array_new_take_null_terminated (gpointer       *data,
                                      GDestroyNotify  element_free_func)
{
  GRealPtrArray *rarray;
  GPtrArray *array;
  gsize len = 0;

  if (data != NULL)
    {
      for (gsize i = 0; data[i] != NULL; ++i)
        len += 1;
    }

  g_return_val_if_fail (len < G_MAXUINT, NULL);

  array = g_ptr_array_new_take (g_steal_pointer (&data), len, element_free_func);
  rarray = (GRealPtrArray *) array;
  rarray->null_terminated = TRUE;
  if (G_LIKELY (rarray->pdata != NULL))
    rarray->alloc = len + 1;

  return array;
}

static GPtrArray *
ptr_array_new_from_array (gpointer       *data,
                          gsize           len,
                          GCopyFunc       copy_func,
                          gpointer        copy_func_user_data,
                          GDestroyNotify  element_free_func,
                          gboolean        null_terminated)
{
  GPtrArray *array;
  GRealPtrArray *rarray;

  g_assert (data != NULL || len == 0);
  g_assert (len <= G_MAXUINT - (null_terminated ? 1 : 0));

  array = ptr_array_new (len, element_free_func, null_terminated);
  rarray = (GRealPtrArray *)array;

  if (copy_func != NULL)
    {
      for (gsize i = 0; i < len; i++)
        rarray->pdata[i] = copy_func (data[i], copy_func_user_data);
    }
  else if (len != 0)
    {
      memcpy (rarray->pdata, data, len * sizeof (gpointer));
    }

  if (null_terminated && rarray->pdata != NULL)
    rarray->pdata[len] = NULL;

  rarray->len = len;

  return array;
}

/**
 * g_ptr_array_new_from_array: (skip)
 * @data: (array length=len) (transfer none) (nullable): an array of pointers,
 * or %NULL for an empty array
 * @len: the number of pointers in @data
 * @copy_func: (nullable): a copy function used to copy every element in the
 *   array or %NULL.
 * @copy_func_user_data: user data passed to @copy_func, or %NULL
 * @element_free_func: (nullable): a function to free elements on @array
 *   destruction or %NULL
 *
 * Creates a new #GPtrArray, copying @len pointers from @data, and setting
 * the array’s reference count to 1.
 *
 * This avoids having to manually add each element one by one.
 *
 * If @copy_func is provided, then it is used to copy each element before
 * adding them to the new array. If it is %NULL then the pointers are copied
 * directly.
 *
 * It also sets @element_free_func for freeing each element when the array is
 * destroyed either via g_ptr_array_unref(), when g_ptr_array_free() is called
 * with @free_segment set to %TRUE or when removing elements.
 *
 * Do not use it if @len is greater than %G_MAXUINT. #GPtrArray
 * stores the length of its data in #guint, which may be shorter than
 * #gsize.
 *
 * Returns: (transfer full): A new #GPtrArray
 *
 * Since: 2.76
 */
GPtrArray *
g_ptr_array_new_from_array (gpointer       *data,
                            gsize           len,
                            GCopyFunc       copy_func,
                            gpointer        copy_func_user_data,
                            GDestroyNotify  element_free_func)
{
  g_return_val_if_fail (data != NULL || len == 0, NULL);
  g_return_val_if_fail (len <= G_MAXUINT, NULL);

  return ptr_array_new_from_array (
    data, len, copy_func, copy_func_user_data, element_free_func, FALSE);
}

/**
 * g_ptr_array_new_from_null_terminated_array: (skip)
 * @data: (array zero-terminated=1) (transfer none) (nullable): an array of
 *   pointers, %NULL terminated; or %NULL for an empty array
 * @copy_func: (nullable): a copy function used to copy every element in the
 *   array or %NULL.
 * @copy_func_user_data: user data passed to @copy_func, or %NULL
 * @element_free_func: (nullable): a function to free elements on @array
 *   destruction or %NULL
 *
 * Creates a new #GPtrArray copying the pointers from @data after having
 * computed the length of it and with a reference count of 1.
 * This avoids having to manually add each element one by one.
 * If @copy_func is provided, then it is used to copy the data in the new
 * array.
 * It also set @element_free_func for freeing each element when the array is
 * destroyed either via g_ptr_array_unref(), when g_ptr_array_free() is called
 * with @free_segment set to %TRUE or when removing elements.
 *
 * Do not use it if the @data has more than %G_MAXUINT elements. #GPtrArray
 * stores the length of its data in #guint, which may be shorter than
 * #gsize.
 *
 * Returns: (transfer full): A new #GPtrArray
 *
 * Since: 2.76
 */
GPtrArray *
g_ptr_array_new_from_null_terminated_array (gpointer       *data,
                                            GCopyFunc       copy_func,
                                            gpointer        copy_func_user_data,
                                            GDestroyNotify  element_free_func)
{
  gsize len = 0;

  if (data != NULL)
    {
      for (gsize i = 0; data[i] != NULL; ++i)
        len += 1;
    }

  g_assert (data != NULL || len == 0);
  g_return_val_if_fail (len < G_MAXUINT, NULL);

  return ptr_array_new_from_array (
    data, len, copy_func, copy_func_user_data, element_free_func, TRUE);
}

/**
 * g_ptr_array_steal:
 * @array: a #GPtrArray.
 * @len: (optional) (out): pointer to retrieve the number of
 *    elements of the original array
 *
 * Frees the data in the array and resets the size to zero, while
 * the underlying array is preserved for use elsewhere and returned
 * to the caller.
 *
 * Note that if the array is %NULL terminated this may still return
 * %NULL if the length of the array was zero and pdata was not yet
 * allocated.
 *
 * Even if set, the #GDestroyNotify function will never be called
 * on the current contents of the array and the caller is
 * responsible for freeing the array elements.
 *
 * An example of use:
 * |[<!-- language="C" -->
 * g_autoptr(GPtrArray) chunk_buffer = g_ptr_array_new_with_free_func (g_bytes_unref);
 *
 * // Some part of your application appends a number of chunks to the pointer array.
 * g_ptr_array_add (chunk_buffer, g_bytes_new_static ("hello", 5));
 * g_ptr_array_add (chunk_buffer, g_bytes_new_static ("world", 5));
 *
 * …
 *
 * // Periodically, the chunks need to be sent as an array-and-length to some
 * // other part of the program.
 * GBytes **chunks;
 * gsize n_chunks;
 *
 * chunks = g_ptr_array_steal (chunk_buffer, &n_chunks);
 * for (gsize i = 0; i < n_chunks; i++)
 *   {
 *     // Do something with each chunk here, and then free them, since
 *     // g_ptr_array_steal() transfers ownership of all the elements and the
 *     // array to the caller.
 *     …
 *
 *     g_bytes_unref (chunks[i]);
 *   }
 *
 * g_free (chunks);
 *
 * // After calling g_ptr_array_steal(), the pointer array can be reused for the
 * // next set of chunks.
 * g_assert (chunk_buffer->len == 0);
 * ]|
 *
 * Returns: (transfer full) (nullable) (array length=len): the element data,
 *   which should be freed using g_free(). This may be %NULL if the array
 *   doesn’t have any elements (i.e. if `*len` is zero).
 *
 * Since: 2.64
 */
gpointer *
g_ptr_array_steal (GPtrArray *array,
                   gsize *len)
{
  GRealPtrArray *rarray;
  gpointer *segment;

  g_return_val_if_fail (array != NULL, NULL);

  rarray = (GRealPtrArray *) array;
  segment = (gpointer *) rarray->pdata;

  if (len != NULL)
    *len = rarray->len;

  rarray->pdata = NULL;
  rarray->len   = 0;
  rarray->alloc = 0;
  return segment;
}

/**
 * g_ptr_array_copy:
 * @array: #GPtrArray to duplicate
 * @func: (nullable): a copy function used to copy every element in the array
 * @user_data: user data passed to the copy function @func, or %NULL
 *
 * Makes a full (deep) copy of a #GPtrArray.
 *
 * @func, as a #GCopyFunc, takes two arguments, the data to be copied
 * and a @user_data pointer. On common processor architectures, it's safe to
 * pass %NULL as @user_data if the copy function takes only one argument. You
 * may get compiler warnings from this though if compiling with GCC’s
 * `-Wcast-function-type` warning.
 *
 * If @func is %NULL, then only the pointers (and not what they are
 * pointing to) are copied to the new #GPtrArray.
 *
 * The copy of @array will have the same #GDestroyNotify for its elements as
 * @array. The copy will also be %NULL terminated if (and only if) the source
 * array is.
 *
 * Returns: (transfer full): a deep copy of the initial #GPtrArray.
 *
 * Since: 2.62
 **/
GPtrArray *
g_ptr_array_copy (GPtrArray *array,
                  GCopyFunc  func,
                  gpointer   user_data)
{
  GRealPtrArray *rarray = (GRealPtrArray *) array;
  GPtrArray *new_array;

  g_return_val_if_fail (array != NULL, NULL);

  new_array = ptr_array_new (0,
                             rarray->element_free_func,
                             rarray->null_terminated);

  if (rarray->alloc > 0)
    {
      g_ptr_array_maybe_expand ((GRealPtrArray *) new_array, array->len);

      if (array->len > 0)
        {
          if (func != NULL)
            {
              guint i;

              for (i = 0; i < array->len; i++)
                new_array->pdata[i] = func (array->pdata[i], user_data);
            }
          else
            {
              memcpy (new_array->pdata, array->pdata,
                      array->len * sizeof (*array->pdata));
            }

          new_array->len = array->len;
        }

      ptr_array_maybe_null_terminate ((GRealPtrArray *) new_array);
    }

  return new_array;
}

/**
 * g_ptr_array_sized_new:
 * @reserved_size: number of pointers preallocated
 *
 * Creates a new #GPtrArray with @reserved_size pointers preallocated
 * and a reference count of 1. This avoids frequent reallocation, if
 * you are going to add many pointers to the array. Note however that
 * the size of the array is still 0.
 *
 * Returns: the new #GPtrArray
 */
GPtrArray*
g_ptr_array_sized_new (guint reserved_size)
{
  return ptr_array_new (reserved_size, NULL, FALSE);
}

/**
 * g_array_copy:
 * @array: A #GArray.
 *
 * Create a shallow copy of a #GArray. If the array elements consist of
 * pointers to data, the pointers are copied but the actual data is not.
 *
 * Returns: (transfer container): A copy of @array.
 *
 * Since: 2.62
 **/
GArray *
g_array_copy (GArray *array)
{
  GRealArray *rarray = (GRealArray *) array;
  GRealArray *new_rarray;

  g_return_val_if_fail (rarray != NULL, NULL);

  new_rarray =
      (GRealArray *) g_array_sized_new (rarray->zero_terminated, rarray->clear,
                                        rarray->elt_size, rarray->len);
  new_rarray->len = rarray->len;
  if (rarray->len > 0)
    memcpy (new_rarray->data, rarray->data, g_array_elt_len (rarray, rarray->len));

  g_array_zero_terminate (new_rarray);

  return (GArray *) new_rarray;
}

/**
 * g_ptr_array_new_with_free_func:
 * @element_free_func: (nullable): A function to free elements with
 *     destroy @array or %NULL
 *
 * Creates a new #GPtrArray with a reference count of 1 and use
 * @element_free_func for freeing each element when the array is destroyed
 * either via g_ptr_array_unref(), when g_ptr_array_free() is called with
 * @free_segment set to %TRUE or when removing elements.
 *
 * Returns: (transfer full): A new #GPtrArray
 *
 * Since: 2.22
 */
GPtrArray*
g_ptr_array_new_with_free_func (GDestroyNotify element_free_func)
{
  return ptr_array_new (0, element_free_func, FALSE);
}

/**
 * g_ptr_array_new_full:
 * @reserved_size: number of pointers preallocated
 * @element_free_func: (nullable): A function to free elements with
 *     destroy @array or %NULL
 *
 * Creates a new #GPtrArray with @reserved_size pointers preallocated
 * and a reference count of 1. This avoids frequent reallocation, if
 * you are going to add many pointers to the array. Note however that
 * the size of the array is still 0. It also set @element_free_func
 * for freeing each element when the array is destroyed either via
 * g_ptr_array_unref(), when g_ptr_array_free() is called with
 * @free_segment set to %TRUE or when removing elements.
 *
 * Returns: (transfer full): A new #GPtrArray
 *
 * Since: 2.30
 */
GPtrArray*
g_ptr_array_new_full (guint          reserved_size,
                      GDestroyNotify element_free_func)
{
  return ptr_array_new (reserved_size, element_free_func, FALSE);
}

/**
 * g_ptr_array_new_null_terminated:
 * @reserved_size: number of pointers preallocated.
 *     If @null_terminated is %TRUE, the actually allocated
 *     buffer size is @reserved_size plus 1, unless @reserved_size
 *     is zero, in which case no initial buffer gets allocated.
 * @element_free_func: (nullable): A function to free elements with
 *     destroy @array or %NULL
 * @null_terminated: whether to make the array as %NULL terminated.
 *
 * Like g_ptr_array_new_full() but also allows to set the array to
 * be %NULL terminated. A %NULL terminated pointer array has an
 * additional %NULL pointer after the last element, beyond the
 * current length.
 *
 * #GPtrArray created by other constructors are not automatically %NULL
 * terminated.
 *
 * Note that if the @array's length is zero and currently no
 * data array is allocated, then pdata will still be %NULL.
 * %GPtrArray will only %NULL terminate pdata, if an actual
 * array is allocated. It does not guarantee that an array
 * is always allocated. In other words, if the length is zero,
 * then pdata may either point to a %NULL terminated array of length
 * zero or be %NULL.
 *
 * Returns: (transfer full): A new #GPtrArray
 *
 * Since: 2.74
 */
GPtrArray *
g_ptr_array_new_null_terminated (guint          reserved_size,
                                 GDestroyNotify element_free_func,
                                 gboolean       null_terminated)
{
  return ptr_array_new (reserved_size, element_free_func, null_terminated);
}

/**
 * g_ptr_array_set_free_func:
 * @array: A #GPtrArray
 * @element_free_func: (nullable): A function to free elements with
 *     destroy @array or %NULL
 *
 * Sets a function for freeing each element when @array is destroyed
 * either via g_ptr_array_unref(), when g_ptr_array_free() is called
 * with @free_segment set to %TRUE or when removing elements.
 *
 * Since: 2.22
 */
void
g_ptr_array_set_free_func (GPtrArray      *array,
                           GDestroyNotify  element_free_func)
{
  GRealPtrArray *rarray = (GRealPtrArray *)array;

  g_return_if_fail (array);

  rarray->element_free_func = element_free_func;
}

/**
 * g_ptr_array_is_null_terminated:
 * @array: the #GPtrArray
 *
 * Gets whether the @array was constructed as %NULL-terminated.
 *
 * This will only return %TRUE for arrays constructed by passing %TRUE to the
 * `null_terminated` argument of g_ptr_array_new_null_terminated(). It will not
 * return %TRUE for normal arrays which have had a %NULL element appended to
 * them.
 *
 * Returns: %TRUE if the array is made to be %NULL terminated.
 *
 * Since: 2.74
 */
gboolean
g_ptr_array_is_null_terminated (GPtrArray *array)
{
  g_return_val_if_fail (array, FALSE);

  return ((GRealPtrArray *) array)->null_terminated;
}

/**
 * g_ptr_array_ref:
 * @array: a #GPtrArray
 *
 * Atomically increments the reference count of @array by one.
 * This function is thread-safe and may be called from any thread.
 *
 * Returns: The passed in #GPtrArray
 *
 * Since: 2.22
 */
GPtrArray*
g_ptr_array_ref (GPtrArray *array)
{
  GRealPtrArray *rarray = (GRealPtrArray *)array;

  g_return_val_if_fail (array, NULL);

  g_atomic_ref_count_inc (&rarray->ref_count);

  return array;
}

static gpointer *ptr_array_free (GPtrArray *, ArrayFreeFlags);

/**
 * g_ptr_array_unref:
 * @array: A #GPtrArray
 *
 * Atomically decrements the reference count of @array by one. If the
 * reference count drops to 0, the effect is the same as calling
 * g_ptr_array_free() with @free_segment set to %TRUE. This function
 * is thread-safe and may be called from any thread.
 *
 * Since: 2.22
 */
void
g_ptr_array_unref (GPtrArray *array)
{
  GRealPtrArray *rarray = (GRealPtrArray *)array;

  g_return_if_fail (array);

  if (g_atomic_ref_count_dec (&rarray->ref_count))
    ptr_array_free (array, FREE_SEGMENT);
}

/**
 * g_ptr_array_free:
 * @array: a #GPtrArray
 * @free_segment: if %TRUE the actual pointer array is freed as well
 *
 * Frees the memory allocated for the #GPtrArray. If @free_segment is %TRUE
 * it frees the memory block holding the elements as well. Pass %FALSE
 * if you want to free the #GPtrArray wrapper but preserve the
 * underlying array for use elsewhere. If the reference count of @array
 * is greater than one, the #GPtrArray wrapper is preserved but the
 * size of @array will be set to zero.
 *
 * If array contents point to dynamically-allocated memory, they should
 * be freed separately if @free_segment is %TRUE and no #GDestroyNotify
 * function has been set for @array.
 *
 * Note that if the array is %NULL terminated and @free_segment is %FALSE
 * then this will always return an allocated %NULL terminated buffer.
 * If pdata is previously %NULL, a new buffer will be allocated.
 *
 * This function is not thread-safe. If using a #GPtrArray from multiple
 * threads, use only the atomic g_ptr_array_ref() and g_ptr_array_unref()
 * functions.
 *
 * Returns: (transfer full) (array) (nullable): the pointer array if
 *   @free_segment is %FALSE, otherwise %NULL. The pointer array should
 *   be freed using g_free().
 */
gpointer*
g_ptr_array_free (GPtrArray *array,
                  gboolean   free_segment)
{
  GRealPtrArray *rarray = (GRealPtrArray *)array;
  ArrayFreeFlags flags;

  g_return_val_if_fail (rarray, NULL);

  flags = (free_segment ? FREE_SEGMENT : 0);

  /* if others are holding a reference, preserve the wrapper but
   * do free/return the data
   *
   * Coverity doesn’t understand this and assumes it’s a leak, so comment this
   * out.
   */
#ifndef __COVERITY__
  if (!g_atomic_ref_count_dec (&rarray->ref_count))
    flags |= PRESERVE_WRAPPER;
#endif

  return ptr_array_free (array, flags);
}

static gpointer *
ptr_array_free (GPtrArray      *array,
                ArrayFreeFlags  flags)
{
  GRealPtrArray *rarray = (GRealPtrArray *)array;
  gpointer *segment;

  if (flags & FREE_SEGMENT)
    {
      /* Data here is stolen and freed manually. It is an
       * error to attempt to access the array data (including
       * mutating the array bounds) during destruction).
       *
       * https://bugzilla.gnome.org/show_bug.cgi?id=769064
       */
      gpointer *stolen_pdata = g_steal_pointer (&rarray->pdata);
      if (rarray->element_free_func != NULL)
        {
          guint i;

          for (i = 0; i < rarray->len; ++i)
            rarray->element_free_func (stolen_pdata[i]);
        }

      g_free (stolen_pdata);
      segment = NULL;
    }
  else
    {
      segment = rarray->pdata;
      if (!segment && rarray->null_terminated)
        segment = (gpointer *) g_new0 (char *, 1);
    }

  if (flags & PRESERVE_WRAPPER)
    {
      rarray->pdata = NULL;
      rarray->len = 0;
      rarray->alloc = 0;
    }
  else
    {
      g_slice_free1 (sizeof (GRealPtrArray), rarray);
    }

  return segment;
}

static void
g_ptr_array_maybe_expand (GRealPtrArray *array,
                          guint          len)
{
  guint max_len, want_len;

  /* The maximum array length is derived from following constraints:
   * - The number of bytes must fit into a gsize / 2.
   * - The number of elements must fit into guint.
   * - null terminated arrays must leave space for the terminating element
   */
  max_len = MIN (G_MAXSIZE / 2 / sizeof (gpointer), G_MAXUINT) - (array->null_terminated ? 1 : 0);

  /* Detect potential overflow */
  if G_UNLIKELY ((max_len - array->len) < len)
    g_error ("adding %u to array would overflow", len);

  want_len = array->len + len + (array->null_terminated ? 1 : 0);
  if (want_len > array->alloc)
    {
      guint old_alloc = array->alloc;
      gsize want_alloc = g_nearest_pow (sizeof (gpointer) * want_len);
      want_alloc = MAX (want_alloc, MIN_ARRAY_SIZE);
      array->alloc = MIN (want_alloc / sizeof (gpointer), G_MAXUINT);
      array->pdata = g_realloc (array->pdata, want_alloc);
      if (G_UNLIKELY (g_mem_gc_friendly))
        for ( ; old_alloc < array->alloc; old_alloc++)
          array->pdata [old_alloc] = NULL;
    }
}

/**
 * g_ptr_array_set_size:
 * @array: a #GPtrArray
 * @length: the new length of the pointer array
 *
 * Sets the size of the array. When making the array larger,
 * newly-added elements will be set to %NULL. When making it smaller,
 * if @array has a non-%NULL #GDestroyNotify function then it will be
 * called for the removed elements.
 */
void
g_ptr_array_set_size  (GPtrArray *array,
                       gint       length)
{
  GRealPtrArray *rarray = (GRealPtrArray *)array;
  guint length_unsigned;

  g_return_if_fail (rarray);
  g_return_if_fail (rarray->len == 0 || (rarray->len != 0 && rarray->pdata != NULL));
  g_return_if_fail (length >= 0);

  length_unsigned = (guint) length;

  if (length_unsigned > rarray->len)
    {
      guint i;

      g_ptr_array_maybe_expand (rarray, length_unsigned - rarray->len);

      /* This is not
       *     memset (array->pdata + array->len, 0,
       *            sizeof (gpointer) * (length_unsigned - array->len));
       * to make it really portable. Remember (void*)NULL needn't be
       * bitwise zero. It of course is silly not to use memset (..,0,..).
       */
      for (i = rarray->len; i < length_unsigned; i++)
        rarray->pdata[i] = NULL;

      rarray->len = length_unsigned;

      ptr_array_maybe_null_terminate (rarray);
    }
  else if (length_unsigned < rarray->len)
    g_ptr_array_remove_range (array, length_unsigned, rarray->len - length_unsigned);
}

static gpointer
ptr_array_remove_index (GPtrArray *array,
                        guint      index_,
                        gboolean   fast,
                        gboolean   free_element)
{
  GRealPtrArray *rarray = (GRealPtrArray *) array;
  gpointer result;

  g_return_val_if_fail (rarray, NULL);
  g_return_val_if_fail (rarray->len == 0 || (rarray->len != 0 && rarray->pdata != NULL), NULL);

  g_return_val_if_fail (index_ < rarray->len, NULL);

  result = rarray->pdata[index_];

  if (rarray->element_free_func != NULL && free_element)
    rarray->element_free_func (rarray->pdata[index_]);

  if (index_ != rarray->len - 1 && !fast)
    memmove (rarray->pdata + index_, rarray->pdata + index_ + 1,
             sizeof (gpointer) * (rarray->len - index_ - 1));
  else if (index_ != rarray->len - 1)
    rarray->pdata[index_] = rarray->pdata[rarray->len - 1];

  rarray->len -= 1;

  if (rarray->null_terminated || G_UNLIKELY (g_mem_gc_friendly))
    rarray->pdata[rarray->len] = NULL;

  return result;
}

/**
 * g_ptr_array_remove_index:
 * @array: a #GPtrArray
 * @index_: the index of the pointer to remove
 *
 * Removes the pointer at the given index from the pointer array.
 * The following elements are moved down one place. If @array has
 * a non-%NULL #GDestroyNotify function it is called for the removed
 * element. If so, the return value from this function will potentially point
 * to freed memory (depending on the #GDestroyNotify implementation).
 *
 * Returns: (nullable): the pointer which was removed
 */
gpointer
g_ptr_array_remove_index (GPtrArray *array,
                          guint      index_)
{
  return ptr_array_remove_index (array, index_, FALSE, TRUE);
}

/**
 * g_ptr_array_remove_index_fast:
 * @array: a #GPtrArray
 * @index_: the index of the pointer to remove
 *
 * Removes the pointer at the given index from the pointer array.
 * The last element in the array is used to fill in the space, so
 * this function does not preserve the order of the array. But it
 * is faster than g_ptr_array_remove_index(). If @array has a non-%NULL
 * #GDestroyNotify function it is called for the removed element. If so, the
 * return value from this function will potentially point to freed memory
 * (depending on the #GDestroyNotify implementation).
 *
 * Returns: (nullable): the pointer which was removed
 */
gpointer
g_ptr_array_remove_index_fast (GPtrArray *array,
                               guint      index_)
{
  return ptr_array_remove_index (array, index_, TRUE, TRUE);
}

/**
 * g_ptr_array_steal_index:
 * @array: a #GPtrArray
 * @index_: the index of the pointer to steal
 *
 * Removes the pointer at the given index from the pointer array.
 * The following elements are moved down one place. The #GDestroyNotify for
 * @array is *not* called on the removed element; ownership is transferred to
 * the caller of this function.
 *
 * Returns: (transfer full) (nullable): the pointer which was removed
 * Since: 2.58
 */
gpointer
g_ptr_array_steal_index (GPtrArray *array,
                         guint      index_)
{
  return ptr_array_remove_index (array, index_, FALSE, FALSE);
}

/**
 * g_ptr_array_steal_index_fast:
 * @array: a #GPtrArray
 * @index_: the index of the pointer to steal
 *
 * Removes the pointer at the given index from the pointer array.
 * The last element in the array is used to fill in the space, so
 * this function does not preserve the order of the array. But it
 * is faster than g_ptr_array_steal_index(). The #GDestroyNotify for @array is
 * *not* called on the removed element; ownership is transferred to the caller
 * of this function.
 *
 * Returns: (transfer full) (nullable): the pointer which was removed
 * Since: 2.58
 */
gpointer
g_ptr_array_steal_index_fast (GPtrArray *array,
                              guint      index_)
{
  return ptr_array_remove_index (array, index_, TRUE, FALSE);
}

/**
 * g_ptr_array_remove_range:
 * @array: a @GPtrArray
 * @index_: the index of the first pointer to remove
 * @length: the number of pointers to remove
 *
 * Removes the given number of pointers starting at the given index
 * from a #GPtrArray. The following elements are moved to close the
 * gap. If @array has a non-%NULL #GDestroyNotify function it is
 * called for the removed elements.
 *
 * Returns: the @array
 *
 * Since: 2.4
 */
GPtrArray*
g_ptr_array_remove_range (GPtrArray *array,
                          guint      index_,
                          guint      length)
{
  GRealPtrArray *rarray = (GRealPtrArray *)array;
  guint i;

  g_return_val_if_fail (rarray != NULL, NULL);
  g_return_val_if_fail (rarray->len == 0 || (rarray->len != 0 && rarray->pdata != NULL), NULL);
  g_return_val_if_fail (index_ <= rarray->len, NULL);
  g_return_val_if_fail (index_ <= G_MAXUINT - length, NULL);
  g_return_val_if_fail (length == 0 || index_ + length <= rarray->len, NULL);

  if (length == 0)
    return array;

  if (rarray->element_free_func != NULL)
    {
      for (i = index_; i < index_ + length; i++)
        rarray->element_free_func (rarray->pdata[i]);
    }

  if (index_ + length != rarray->len)
    {
      memmove (&rarray->pdata[index_],
               &rarray->pdata[index_ + length],
               (rarray->len - (index_ + length)) * sizeof (gpointer));
    }

  rarray->len -= length;
  if (G_UNLIKELY (g_mem_gc_friendly))
    {
      for (i = 0; i < length; i++)
        rarray->pdata[rarray->len + i] = NULL;
    }
  else
    ptr_array_maybe_null_terminate (rarray);

  return array;
}

/**
 * g_ptr_array_remove:
 * @array: a #GPtrArray
 * @data: the pointer to remove
 *
 * Removes the first occurrence of the given pointer from the pointer
 * array. The following elements are moved down one place. If @array
 * has a non-%NULL #GDestroyNotify function it is called for the
 * removed element.
 *
 * It returns %TRUE if the pointer was removed, or %FALSE if the
 * pointer was not found.
 *
 * Returns: %TRUE if the pointer is removed, %FALSE if the pointer
 *     is not found in the array
 */
gboolean
g_ptr_array_remove (GPtrArray *array,
                    gpointer   data)
{
  guint i;

  g_return_val_if_fail (array, FALSE);
  g_return_val_if_fail (array->len == 0 || (array->len != 0 && array->pdata != NULL), FALSE);

  for (i = 0; i < array->len; i += 1)
    {
      if (array->pdata[i] == data)
        {
          g_ptr_array_remove_index (array, i);
          return TRUE;
        }
    }

  return FALSE;
}

/**
 * g_ptr_array_remove_fast:
 * @array: a #GPtrArray
 * @data: the pointer to remove
 *
 * Removes the first occurrence of the given pointer from the pointer
 * array. The last element in the array is used to fill in the space,
 * so this function does not preserve the order of the array. But it
 * is faster than g_ptr_array_remove(). If @array has a non-%NULL
 * #GDestroyNotify function it is called for the removed element.
 *
 * It returns %TRUE if the pointer was removed, or %FALSE if the
 * pointer was not found.
 *
 * Returns: %TRUE if the pointer was found in the array
 */
gboolean
g_ptr_array_remove_fast (GPtrArray *array,
                         gpointer   data)
{
  GRealPtrArray *rarray = (GRealPtrArray *)array;
  guint i;

  g_return_val_if_fail (rarray, FALSE);
  g_return_val_if_fail (rarray->len == 0 || (rarray->len != 0 && rarray->pdata != NULL), FALSE);

  for (i = 0; i < rarray->len; i += 1)
    {
      if (rarray->pdata[i] == data)
        {
          g_ptr_array_remove_index_fast (array, i);
          return TRUE;
        }
    }

  return FALSE;
}

/**
 * g_ptr_array_add:
 * @array: a #GPtrArray
 * @data: the pointer to add
 *
 * Adds a pointer to the end of the pointer array. The array will grow
 * in size automatically if necessary.
 */
void
g_ptr_array_add (GPtrArray *array,
                 gpointer   data)
{
  GRealPtrArray *rarray = (GRealPtrArray *)array;

  g_return_if_fail (rarray);
  g_return_if_fail (rarray->len == 0 || (rarray->len != 0 && rarray->pdata != NULL));

  g_ptr_array_maybe_expand (rarray, 1u);

  rarray->pdata[rarray->len++] = data;

  ptr_array_maybe_null_terminate (rarray);
}

/**
 * g_ptr_array_extend:
 * @array_to_extend: a #GPtrArray.
 * @array: (transfer none): a #GPtrArray to add to the end of @array_to_extend.
 * @func: (nullable): a copy function used to copy every element in the array
 * @user_data: user data passed to the copy function @func, or %NULL
 *
 * Adds all pointers of @array to the end of the array @array_to_extend.
 * The array will grow in size automatically if needed. @array_to_extend is
 * modified in-place.
 *
 * @func, as a #GCopyFunc, takes two arguments, the data to be copied
 * and a @user_data pointer. On common processor architectures, it's safe to
 * pass %NULL as @user_data if the copy function takes only one argument. You
 * may get compiler warnings from this though if compiling with GCC’s
 * `-Wcast-function-type` warning.
 *
 * If @func is %NULL, then only the pointers (and not what they are
 * pointing to) are copied to the new #GPtrArray.
 *
 * Whether @array_to_extend is %NULL terminated stays unchanged by this function.
 *
 * Since: 2.62
 **/
void
g_ptr_array_extend (GPtrArray  *array_to_extend,
                    GPtrArray  *array,
                    GCopyFunc   func,
                    gpointer    user_data)
{
  GRealPtrArray *rarray_to_extend = (GRealPtrArray *) array_to_extend;

  g_return_if_fail (array_to_extend != NULL);
  g_return_if_fail (array != NULL);

  if (array->len == 0u)
    return;

  g_ptr_array_maybe_expand (rarray_to_extend, array->len);

  if (func != NULL)
    {
      guint i;

      for (i = 0; i < array->len; i++)
        rarray_to_extend->pdata[i + rarray_to_extend->len] =
          func (array->pdata[i], user_data);
    }
  else if (array->len > 0)
    {
      memcpy (rarray_to_extend->pdata + rarray_to_extend->len, array->pdata,
              array->len * sizeof (*array->pdata));
    }

  rarray_to_extend->len += array->len;

  ptr_array_maybe_null_terminate (rarray_to_extend);
}

/**
 * g_ptr_array_extend_and_steal:
 * @array_to_extend: (transfer none): a #GPtrArray.
 * @array: (transfer container): a #GPtrArray to add to the end of
 *     @array_to_extend.
 *
 * Adds all the pointers in @array to the end of @array_to_extend, transferring
 * ownership of each element from @array to @array_to_extend and modifying
 * @array_to_extend in-place. @array is then freed.
 *
 * As with g_ptr_array_free(), @array will be destroyed if its reference count
 * is 1. If its reference count is higher, it will be decremented and the
 * length of @array set to zero.
 *
 * Since: 2.62
 **/
void
g_ptr_array_extend_and_steal (GPtrArray  *array_to_extend,
                              GPtrArray  *array)
{
  gpointer *pdata;

  g_ptr_array_extend (array_to_extend, array, NULL, NULL);

  /* Get rid of @array without triggering the GDestroyNotify attached
   * to the elements moved from @array to @array_to_extend. */
  pdata = g_steal_pointer (&array->pdata);
  array->len = 0;
  ((GRealPtrArray *) array)->alloc = 0;
  g_ptr_array_unref (array);
  g_free (pdata);
}

/**
 * g_ptr_array_insert:
 * @array: a #GPtrArray
 * @index_: the index to place the new element at, or -1 to append
 * @data: the pointer to add.
 *
 * Inserts an element into the pointer array at the given index. The 
 * array will grow in size automatically if necessary.
 *
 * Since: 2.40
 */
void
g_ptr_array_insert (GPtrArray *array,
                    gint       index_,
                    gpointer   data)
{
  GRealPtrArray *rarray = (GRealPtrArray *)array;
  guint real_index;

  g_return_if_fail (rarray);
  g_return_if_fail (index_ >= -1);
  g_return_if_fail (index_ < 0 || (guint) index_ <= rarray->len);

  g_ptr_array_maybe_expand (rarray, 1u);

  real_index = (index_ >= 0) ? (guint) index_ : rarray->len;

  if (real_index < rarray->len)
    memmove (&(rarray->pdata[real_index + 1]),
             &(rarray->pdata[real_index]),
             (rarray->len - real_index) * sizeof (gpointer));

  rarray->len++;
  rarray->pdata[real_index] = data;

  ptr_array_maybe_null_terminate (rarray);
}

/* Please keep this doc-comment in sync with pointer_array_sort_example()
 * in glib/tests/array-test.c */
/**
 * g_ptr_array_sort:
 * @array: a #GPtrArray
 * @compare_func: comparison function
 *
 * Sorts the array, using @compare_func which should be a qsort()-style
 * comparison function (returns less than zero for first arg is less
 * than second arg, zero for equal, greater than zero if first arg is
 * greater than second arg).
 *
 * Note that the comparison function for g_ptr_array_sort() doesn't
 * take the pointers from the array as arguments, it takes pointers to
 * the pointers in the array.
 *
 * Use g_ptr_array_sort_values() if you want to use normal
 * #GCompareFuncs, otherwise here is a full example of use:
 *
 * |[<!-- language="C" -->
 * typedef struct
 * {
 *   gchar *name;
 *   gint size;
 * } FileListEntry;
 *
 * static gint
 * sort_filelist (gconstpointer a, gconstpointer b)
 * {
 *   const FileListEntry *entry1 = *((FileListEntry **) a);
 *   const FileListEntry *entry2 = *((FileListEntry **) b);
 *
 *   return g_ascii_strcasecmp (entry1->name, entry2->name);
 * }
 *
 * …
 * g_autoptr (GPtrArray) file_list = NULL;
 *
 * // initialize file_list array and load with many FileListEntry entries
 * ...
 * // now sort it with
 * g_ptr_array_sort (file_list, sort_filelist);
 * ]|
 *
 * This is guaranteed to be a stable sort since version 2.32.
 */
void
g_ptr_array_sort (GPtrArray    *array,
                  GCompareFunc  compare_func)
{
  g_return_if_fail (array != NULL);

  /* Don't use qsort as we want a guaranteed stable sort */
  if (array->len > 0)
    g_sort_array (array->pdata,
                  array->len,
                  sizeof (gpointer),
                  (GCompareDataFunc) compare_func,
                  NULL);
}

/* Please keep this doc-comment in sync with
 * pointer_array_sort_with_data_example() in glib/tests/array-test.c */
/**
 * g_ptr_array_sort_with_data:
 * @array: a #GPtrArray
 * @compare_func: comparison function
 * @user_data: data to pass to @compare_func
 *
 * Like g_ptr_array_sort(), but the comparison function has an extra
 * user data argument.
 *
 * Note that the comparison function for g_ptr_array_sort_with_data()
 * doesn't take the pointers from the array as arguments, it takes
 * pointers to the pointers in the array.
 *
 * Use g_ptr_array_sort_values_with_data() if you want to use normal
 * #GCompareDataFuncs, otherwise here is a full example of use:
 *
 * |[<!-- language="C" -->
 * typedef enum { SORT_NAME, SORT_SIZE } SortMode;
 *
 * typedef struct
 * {
 *   gchar *name;
 *   gint size;
 * } FileListEntry;
 *
 * static gint
 * sort_filelist (gconstpointer a, gconstpointer b, gpointer user_data)
 * {
 *   gint order;
 *   const SortMode sort_mode = GPOINTER_TO_INT (user_data);
 *   const FileListEntry *entry1 = *((FileListEntry **) a);
 *   const FileListEntry *entry2 = *((FileListEntry **) b);
 *
 *   switch (sort_mode)
 *     {
 *     case SORT_NAME:
 *       order = g_ascii_strcasecmp (entry1->name, entry2->name);
 *       break;
 *     case SORT_SIZE:
 *       order = entry1->size - entry2->size;
 *       break;
 *     default:
 *       order = 0;
 *       break;
 *     }
 *   return order;
 * }
 *
 * ...
 * g_autoptr (GPtrArray) file_list = NULL;
 * SortMode sort_mode;
 *
 * // initialize file_list array and load with many FileListEntry entries
 * ...
 * // now sort it with
 * sort_mode = SORT_NAME;
 * g_ptr_array_sort_with_data (file_list,
 *                             sort_filelist,
 *                             GINT_TO_POINTER (sort_mode));
 * ]|
 *
 * This is guaranteed to be a stable sort since version 2.32.
 */
void
g_ptr_array_sort_with_data (GPtrArray        *array,
                            GCompareDataFunc  compare_func,
                            gpointer          user_data)
{
  g_return_if_fail (array != NULL);

  if (array->len > 0)
    g_sort_array (array->pdata,
                  array->len,
                  sizeof (gpointer),
                  compare_func,
                  user_data);
}

static inline gint
compare_ptr_array_values (gconstpointer a, gconstpointer b, gpointer user_data)
{
  gconstpointer aa = *((gconstpointer *) a);
  gconstpointer bb = *((gconstpointer *) b);
  GCompareFunc compare_func = user_data;

  return compare_func (aa, bb);
}

/**
 * g_ptr_array_sort_values:
 * @array: a #GPtrArray
 * @compare_func: a #GCompareFunc comparison function
 *
 * Sorts the array, using @compare_func which should be a qsort()-style
 * comparison function (returns less than zero for first arg is less
 * than second arg, zero for equal, greater than zero if first arg is
 * greater than second arg).
 *
 * This is guaranteed to be a stable sort.
 *
 * Since: 2.76
 */
void
g_ptr_array_sort_values (GPtrArray    *array,
                         GCompareFunc  compare_func)
{
  g_ptr_array_sort_with_data (array, compare_ptr_array_values, compare_func);
}

typedef struct
{
  GCompareDataFunc compare_func;
  gpointer user_data;
} GPtrArraySortValuesData;

static inline gint
compare_ptr_array_values_with_data (gconstpointer a,
                                    gconstpointer b,
                                    gpointer      user_data)
{
  gconstpointer aa = *((gconstpointer *) a);
  gconstpointer bb = *((gconstpointer *) b);
  GPtrArraySortValuesData *data = user_data;

  return data->compare_func (aa, bb, data->user_data);
}

/**
 * g_ptr_array_sort_values_with_data:
 * @array: a #GPtrArray
 * @compare_func: a #GCompareDataFunc comparison function
 * @user_data: data to pass to @compare_func
 *
 * Like g_ptr_array_sort_values(), but the comparison function has an extra
 * user data argument.
 *
 * This is guaranteed to be a stable sort.
 *
 * Since: 2.76
 */
void
g_ptr_array_sort_values_with_data (GPtrArray        *array,
                                   GCompareDataFunc  compare_func,
                                   gpointer          user_data)
{
  g_ptr_array_sort_with_data (array, compare_ptr_array_values_with_data,
                              &(GPtrArraySortValuesData){
                                  .compare_func = compare_func,
                                  .user_data = user_data,
                              });
}

/**
 * g_ptr_array_foreach:
 * @array: a #GPtrArray
 * @func: the function to call for each array element
 * @user_data: user data to pass to the function
 * 
 * Calls a function for each element of a #GPtrArray. @func must not
 * add elements to or remove elements from the array.
 *
 * Since: 2.4
 */
void
g_ptr_array_foreach (GPtrArray *array,
                     GFunc      func,
                     gpointer   user_data)
{
  guint i;

  g_return_if_fail (array);

  for (i = 0; i < array->len; i++)
    (*func) (array->pdata[i], user_data);
}

/**
 * g_ptr_array_find: (skip)
 * @haystack: pointer array to be searched
 * @needle: pointer to look for
 * @index_: (optional) (out): return location for the index of
 *    the element, if found
 *
 * Checks whether @needle exists in @haystack. If the element is found, %TRUE is
 * returned and the element’s index is returned in @index_ (if non-%NULL).
 * Otherwise, %FALSE is returned and @index_ is undefined. If @needle exists
 * multiple times in @haystack, the index of the first instance is returned.
 *
 * This does pointer comparisons only. If you want to use more complex equality
 * checks, such as string comparisons, use g_ptr_array_find_with_equal_func().
 *
 * Returns: %TRUE if @needle is one of the elements of @haystack
 * Since: 2.54
 */
gboolean
g_ptr_array_find (GPtrArray     *haystack,
                  gconstpointer  needle,
                  guint         *index_)
{
  return g_ptr_array_find_with_equal_func (haystack, needle, NULL, index_);
}

/**
 * g_ptr_array_find_with_equal_func: (skip)
 * @haystack: pointer array to be searched
 * @needle: pointer to look for
 * @equal_func: (nullable): the function to call for each element, which should
 *    return %TRUE when the desired element is found; or %NULL to use pointer
 *    equality
 * @index_: (optional) (out): return location for the index of
 *    the element, if found
 *
 * Checks whether @needle exists in @haystack, using the given @equal_func.
 * If the element is found, %TRUE is returned and the element’s index is
 * returned in @index_ (if non-%NULL). Otherwise, %FALSE is returned and @index_
 * is undefined. If @needle exists multiple times in @haystack, the index of
 * the first instance is returned.
 *
 * @equal_func is called with the element from the array as its first parameter,
 * and @needle as its second parameter. If @equal_func is %NULL, pointer
 * equality is used.
 *
 * Returns: %TRUE if @needle is one of the elements of @haystack
 * Since: 2.54
 */
gboolean
g_ptr_array_find_with_equal_func (GPtrArray     *haystack,
                                  gconstpointer  needle,
                                  GEqualFunc     equal_func,
                                  guint         *index_)
{
  guint i;

  g_return_val_if_fail (haystack != NULL, FALSE);

  if (equal_func == NULL)
    equal_func = g_direct_equal;

  for (i = 0; i < haystack->len; i++)
    {
      if (equal_func (g_ptr_array_index (haystack, i), needle))
        {
          if (index_ != NULL)
            *index_ = i;
          return TRUE;
        }
    }

  return FALSE;
}

/**
 * GByteArray:
 * @data: a pointer to the element data. The data may be moved as
 *     elements are added to the #GByteArray
 * @len: the number of elements in the #GByteArray
 *
 * Contains the public fields of a GByteArray.
 */

/**
 * g_byte_array_new:
 *
 * Creates a new #GByteArray with a reference count of 1.
 *
 * Returns: (transfer full): the new #GByteArray
 */
GByteArray*
g_byte_array_new (void)
{
  return (GByteArray *)g_array_sized_new (FALSE, FALSE, 1, 0);
}

/**
 * g_byte_array_steal:
 * @array: a #GByteArray.
 * @len: (optional) (out): pointer to retrieve the number of
 *    elements of the original array
 *
 * Frees the data in the array and resets the size to zero, while
 * the underlying array is preserved for use elsewhere and returned
 * to the caller.
 *
 * Returns: (transfer full) (array length=len): the element data,
 *   which should be freed using g_free().
 *
 * Since: 2.64
 */
guint8 *
g_byte_array_steal (GByteArray *array,
                    gsize *len)
{
  return (guint8 *) g_array_steal ((GArray *) array, len);
}

/**
 * g_byte_array_new_take:
 * @data: (transfer full) (array length=len): byte data for the array
 * @len: length of @data
 *
 * Creates a byte array containing the @data.
 * After this call, @data belongs to the #GByteArray and may no longer be
 * modified by the caller. The memory of @data has to be dynamically
 * allocated and will eventually be freed with g_free().
 *
 * Do not use it if @len is greater than %G_MAXUINT. #GByteArray
 * stores the length of its data in #guint, which may be shorter than
 * #gsize.
 *
 * Since: 2.32
 *
 * Returns: (transfer full): a new #GByteArray
 */
GByteArray*
g_byte_array_new_take (guint8 *data,
                       gsize   len)
{
  GByteArray *array;
  GRealArray *real;

  g_return_val_if_fail (len <= G_MAXUINT, NULL);
  array = g_byte_array_new ();
  real = (GRealArray *)array;
  g_assert (real->data == NULL);
  g_assert (real->len == 0);

  real->data = data;
  real->len = len;
  real->elt_capacity = len;

  return array;
}

/**
 * g_byte_array_sized_new:
 * @reserved_size: number of bytes preallocated
 *
 * Creates a new #GByteArray with @reserved_size bytes preallocated.
 * This avoids frequent reallocation, if you are going to add many
 * bytes to the array. Note however that the size of the array is still
 * 0.
 *
 * Returns: (transfer full): the new #GByteArray
 */
GByteArray*
g_byte_array_sized_new (guint reserved_size)
{
  return (GByteArray *)g_array_sized_new (FALSE, FALSE, 1, reserved_size);
}

/**
 * g_byte_array_free:
 * @array: a #GByteArray
 * @free_segment: if %TRUE the actual byte data is freed as well
 *
 * Frees the memory allocated by the #GByteArray. If @free_segment is
 * %TRUE it frees the actual byte data. If the reference count of
 * @array is greater than one, the #GByteArray wrapper is preserved but
 * the size of @array will be set to zero.
 *
 * Returns: (nullable) (array) (transfer full): the element data if
 *   @free_segment is %FALSE, otherwise %NULL.  The element data
 *   should be freed using g_free().
 */
guint8*
g_byte_array_free (GByteArray *array,
                   gboolean    free_segment)
{
  return (guint8 *)g_array_free ((GArray *)array, free_segment);
}

/**
 * g_byte_array_free_to_bytes:
 * @array: (transfer full): a #GByteArray
 *
 * Transfers the data from the #GByteArray into a new immutable #GBytes.
 *
 * The #GByteArray is freed unless the reference count of @array is greater
 * than one, the #GByteArray wrapper is preserved but the size of @array
 * will be set to zero.
 *
 * This is identical to using g_bytes_new_take() and g_byte_array_free()
 * together.
 *
 * Since: 2.32
 *
 * Returns: (transfer full): a new immutable #GBytes representing same
 *     byte data that was in the array
 */
GBytes*
g_byte_array_free_to_bytes (GByteArray *array)
{
  gsize length;

  g_return_val_if_fail (array != NULL, NULL);

  length = array->len;
  return g_bytes_new_take (g_byte_array_free (array, FALSE), length);
}

/**
 * g_byte_array_ref:
 * @array: A #GByteArray
 *
 * Atomically increments the reference count of @array by one.
 * This function is thread-safe and may be called from any thread.
 *
 * Returns: (transfer full): The passed in #GByteArray
 *
 * Since: 2.22
 */
GByteArray*
g_byte_array_ref (GByteArray *array)
{
  return (GByteArray *)g_array_ref ((GArray *)array);
}

/**
 * g_byte_array_unref:
 * @array: A #GByteArray
 *
 * Atomically decrements the reference count of @array by one. If the
 * reference count drops to 0, all memory allocated by the array is
 * released. This function is thread-safe and may be called from any
 * thread.
 *
 * Since: 2.22
 */
void
g_byte_array_unref (GByteArray *array)
{
  g_array_unref ((GArray *)array);
}

/**
 * g_byte_array_append:
 * @array: a #GByteArray
 * @data: (array length=len): the byte data to be added
 * @len: the number of bytes to add
 *
 * Adds the given bytes to the end of the #GByteArray.
 * The array will grow in size automatically if necessary.
 *
 * Returns: (transfer none): the #GByteArray
 */
GByteArray*
g_byte_array_append (GByteArray   *array,
                     const guint8 *data,
                     guint         len)
{
  g_array_append_vals ((GArray *)array, (guint8 *)data, len);

  return array;
}

/**
 * g_byte_array_prepend:
 * @array: a #GByteArray
 * @data: (array length=len): the byte data to be added
 * @len: the number of bytes to add
 *
 * Adds the given data to the start of the #GByteArray.
 * The array will grow in size automatically if necessary.
 *
 * Returns: (transfer none): the #GByteArray
 */
GByteArray*
g_byte_array_prepend (GByteArray   *array,
                      const guint8 *data,
                      guint         len)
{
  g_array_prepend_vals ((GArray *)array, (guint8 *)data, len);

  return array;
}

/**
 * g_byte_array_set_size:
 * @array: a #GByteArray
 * @length: the new size of the #GByteArray
 *
 * Sets the size of the #GByteArray, expanding it if necessary.
 *
 * Returns: (transfer none): the #GByteArray
 */
GByteArray*
g_byte_array_set_size (GByteArray *array,
                       guint       length)
{
  g_array_set_size ((GArray *)array, length);

  return array;
}

/**
 * g_byte_array_remove_index:
 * @array: a #GByteArray
 * @index_: the index of the byte to remove
 *
 * Removes the byte at the given index from a #GByteArray.
 * The following bytes are moved down one place.
 *
 * Returns: (transfer none): the #GByteArray
 **/
GByteArray*
g_byte_array_remove_index (GByteArray *array,
                           guint       index_)
{
  g_array_remove_index ((GArray *)array, index_);

  return array;
}

/**
 * g_byte_array_remove_index_fast:
 * @array: a #GByteArray
 * @index_: the index of the byte to remove
 *
 * Removes the byte at the given index from a #GByteArray. The last
 * element in the array is used to fill in the space, so this function
 * does not preserve the order of the #GByteArray. But it is faster
 * than g_byte_array_remove_index().
 *
 * Returns: (transfer none): the #GByteArray
 */
GByteArray*
g_byte_array_remove_index_fast (GByteArray *array,
                                guint       index_)
{
  g_array_remove_index_fast ((GArray *)array, index_);

  return array;
}

/**
 * g_byte_array_remove_range:
 * @array: a @GByteArray
 * @index_: the index of the first byte to remove
 * @length: the number of bytes to remove
 *
 * Removes the given number of bytes starting at the given index from a
 * #GByteArray.  The following elements are moved to close the gap.
 *
 * Returns: (transfer none): the #GByteArray
 *
 * Since: 2.4
 */
GByteArray*
g_byte_array_remove_range (GByteArray *array,
                           guint       index_,
                           guint       length)
{
  g_return_val_if_fail (array, NULL);
  g_return_val_if_fail (index_ <= array->len, NULL);
  g_return_val_if_fail (index_ <= G_MAXUINT - length, NULL);
  g_return_val_if_fail (index_ + length <= array->len, NULL);

  return (GByteArray *)g_array_remove_range ((GArray *)array, index_, length);
}

/**
 * g_byte_array_sort:
 * @array: a #GByteArray
 * @compare_func: (scope call): comparison function
 *
 * Sorts a byte array, using @compare_func which should be a
 * qsort()-style comparison function (returns less than zero for first
 * arg is less than second arg, zero for equal, greater than zero if
 * first arg is greater than second arg).
 *
 * If two array elements compare equal, their order in the sorted array
 * is undefined. If you want equal elements to keep their order (i.e.
 * you want a stable sort) you can write a comparison function that,
 * if two elements would otherwise compare equal, compares them by
 * their addresses.
 */
void
g_byte_array_sort (GByteArray   *array,
                   GCompareFunc  compare_func)
{
  g_array_sort ((GArray *)array, compare_func);
}

/**
 * g_byte_array_sort_with_data:
 * @array: a #GByteArray
 * @compare_func: (scope call): comparison function
 * @user_data: data to pass to @compare_func
 *
 * Like g_byte_array_sort(), but the comparison function takes an extra
 * user data argument.
 */
void
g_byte_array_sort_with_data (GByteArray       *array,
                             GCompareDataFunc  compare_func,
                             gpointer          user_data)
{
  g_array_sort_with_data ((GArray *)array, compare_func, user_data);
}
