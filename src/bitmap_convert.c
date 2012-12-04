/*         ______   ___    ___
 *        /\  _  \ /\_ \  /\_ \
 *        \ \ \L\ \\//\ \ \//\ \      __     __   _ __   ___
 *         \ \  __ \ \ \ \  \ \ \   /'__`\ /'_ `\/\`'__\/ __`\
 *          \ \ \/\ \ \_\ \_ \_\ \_/\  __//\ \L\ \ \ \//\ \L\ \
 *           \ \_\ \_\/\____\/\____\ \____\ \____ \ \_\\ \____/
 *            \/_/\/_/\/____/\/____/\/____/\/___L\ \/_/ \/___/
 *                                           /\____/
 *                                           \_/__/
 *
 *      Convert between bitmap types.
 *
 *      See LICENSE.txt for copyright information.
 */


#include "allegro5/allegro.h"
#include "allegro5/internal/aintern.h"
#include "allegro5/internal/aintern_bitmap.h"
#include "allegro5/internal/aintern_display.h"
#include "allegro5/internal/aintern_vector.h"

ALLEGRO_DEBUG_CHANNEL("bitmap")


struct TO_BE_CONVERTED_LIST {
   ALLEGRO_MUTEX *mutex;
   _AL_VECTOR bitmaps;
};


static struct TO_BE_CONVERTED_LIST to_be_converted;


static void cleanup_to_be_converted_bitmaps(void)
{
   _al_vector_free(&to_be_converted.bitmaps);
   al_destroy_mutex(to_be_converted.mutex);    
}


/* This is called in al_install_system. Exit functions are called in
 * al_uninstall_system.
 */
void _al_init_to_be_converted_bitmaps(void)
{
   to_be_converted.mutex = al_create_mutex_recursive();
   _al_vector_init(&to_be_converted.bitmaps, sizeof(ALLEGRO_BITMAP *));
   _al_add_exit_func(cleanup_to_be_converted_bitmaps,
      "cleanup_to_be_converted_bitmaps");
}


void _al_check_to_be_converted_list_add(ALLEGRO_BITMAP *bitmap)
{
   if (!(bitmap->flags & ALLEGRO_MEMORY_BITMAP))
      return;
   if (bitmap->flags & ALLEGRO_CONVERT_BITMAP) {
      ALLEGRO_BITMAP **back;
      al_lock_mutex(to_be_converted.mutex);
      back = _al_vector_alloc_back(&to_be_converted.bitmaps);
      *back = bitmap;
      al_unlock_mutex(to_be_converted.mutex);
   }
}


void _al_check_to_be_converted_list_remove(ALLEGRO_BITMAP *bitmap)
{
   if (!(bitmap->flags & ALLEGRO_MEMORY_BITMAP))
      return;
   if (bitmap->flags & ALLEGRO_CONVERT_BITMAP) {
      al_lock_mutex(to_be_converted.mutex);
      _al_vector_find_and_delete(&to_be_converted.bitmaps, &bitmap);
      al_unlock_mutex(to_be_converted.mutex);
   }
}


static void transfer_bitmap_data(ALLEGRO_BITMAP *src, ALLEGRO_BITMAP *dst)
{
   ALLEGRO_LOCKED_REGION *dst_region;
   ALLEGRO_LOCKED_REGION *src_region;

   if (!(src_region = al_lock_bitmap(src, ALLEGRO_PIXEL_FORMAT_ANY, ALLEGRO_LOCK_READONLY)))
      return;

   if (!(dst_region = al_lock_bitmap(dst, ALLEGRO_PIXEL_FORMAT_ANY, ALLEGRO_LOCK_WRITEONLY))) {
      al_unlock_bitmap(src);
      return;
   }

   _al_convert_bitmap_data(
      src_region->data, src_region->format, src_region->pitch,
      dst_region->data, dst_region->format, dst_region->pitch,
      0, 0, 0, 0, src->w, src->h);

   al_unlock_bitmap(src);
   al_unlock_bitmap(dst);
}


/* Function: al_clone_bitmap
 */
ALLEGRO_BITMAP *al_clone_bitmap(ALLEGRO_BITMAP *bitmap)
{
   ALLEGRO_BITMAP *clone;
   ASSERT(bitmap);

   clone = al_create_bitmap(bitmap->w, bitmap->h);
   if (!clone)
      return NULL;
   transfer_bitmap_data(bitmap, clone);
   return clone;
}


/* This swaps the contents of the two bitmaps. */
static void _al_swap_bitmaps(ALLEGRO_BITMAP *bitmap, ALLEGRO_BITMAP *other)
{
   ALLEGRO_BITMAP temp = *bitmap;

   _al_check_to_be_converted_list_remove(bitmap);
   _al_check_to_be_converted_list_remove(other);

   *bitmap = *other;
   *other = temp;

   /* We are basically done already. Except we now have to update everything
    * possibly referencing any of the two bitmaps.
    */

   if (bitmap->display && !other->display) {
      /* This means before the swap, other was the display bitmap, and we
       * now should replace it with the swapped pointer.
       */
      ALLEGRO_BITMAP **back;
      int pos = _al_vector_find(&bitmap->display->bitmaps, &other);
      ASSERT(pos >= 0);
      back = _al_vector_ref(&bitmap->display->bitmaps, pos);
      *back = bitmap;
   }

   if (other->display && !bitmap->display) {
      ALLEGRO_BITMAP **back;
      int pos = _al_vector_find(&other->display->bitmaps, &bitmap);
      ASSERT(pos >= 0);
      back = _al_vector_ref(&other->display->bitmaps, pos);
      *back = other;
   }

   _al_check_to_be_converted_list_add(bitmap);
   _al_check_to_be_converted_list_add(other);

   if (bitmap->vt && bitmap->vt->bitmap_pointer_changed)
      bitmap->vt->bitmap_pointer_changed(bitmap, other);

   if (other->vt && other->vt->bitmap_pointer_changed)
      other->vt->bitmap_pointer_changed(other, bitmap);
}


/* Function: al_convert_bitmap
 */
void al_convert_bitmap(ALLEGRO_BITMAP *bitmap)
{
   ALLEGRO_BITMAP *clone;
   int bitmap_flags = bitmap->format;
   int new_bitmap_flags = al_get_new_bitmap_flags();
   bool want_memory = (new_bitmap_flags & ALLEGRO_MEMORY_BITMAP) != 0;
   bool clone_memory;
   
   bitmap_flags &= ~_ALLEGRO_INTERNAL_OPENGL;

   /* If a cloned bitmap would be identical, we can just do nothing. */
   if (bitmap->format == al_get_new_bitmap_format() &&
         bitmap_flags == new_bitmap_flags &&
         bitmap->display == al_get_current_display()) {
      return;
   }

   if (bitmap->parent) {
      bool parent_mem = (bitmap->parent->flags & ALLEGRO_MEMORY_BITMAP) != 0;
      if (parent_mem != want_memory) {
         al_convert_bitmap(bitmap->parent);
      }
      clone = al_create_sub_bitmap(bitmap->parent,
         bitmap->xofs, bitmap->yofs, bitmap->w, bitmap->h);
   }
   else {
      clone = al_clone_bitmap(bitmap);
   }

   if (!clone) {
      return;
   }

   clone_memory = (clone->flags & ALLEGRO_MEMORY_BITMAP) != 0;

   if (clone_memory != want_memory) {
      /* We cannot convert. */
      al_destroy_bitmap(clone);
      return;
   }

   _al_swap_bitmaps(bitmap, clone);

   /* Preserve bitmap state. */
   bitmap->cl = clone->cl;
   bitmap->ct = clone->ct;
   bitmap->cr_excl = clone->cr_excl;
   bitmap->cb_excl = clone->cb_excl;
   bitmap->transform = clone->transform;
   bitmap->inverse_transform = clone->inverse_transform;
   bitmap->inverse_transform_dirty = clone->inverse_transform_dirty;
   
   al_destroy_bitmap(clone);
}


/* Function: al_convert_bitmaps
 */
void al_convert_bitmaps(void)
{
   ALLEGRO_STATE backup;
   ALLEGRO_DISPLAY *display = al_get_current_display();
   _AL_VECTOR copy;
   size_t i;
   if (!display) return;
   
   al_store_state(&backup, ALLEGRO_STATE_NEW_BITMAP_PARAMETERS);

   al_lock_mutex(to_be_converted.mutex);
   
   _al_vector_init(&copy, sizeof(ALLEGRO_BITMAP *));
   for (i = 0; i <  _al_vector_size(&to_be_converted.bitmaps); i++) {
      ALLEGRO_BITMAP **bptr, **bptr2;
      bptr = _al_vector_ref(&to_be_converted.bitmaps, i);
      bptr2 = _al_vector_alloc_back(&copy);
      *bptr2 = *bptr;
   }
   _al_vector_free(&to_be_converted.bitmaps);
   _al_vector_init(&to_be_converted.bitmaps, sizeof(ALLEGRO_BITMAP *));
   for (i = 0; i < _al_vector_size(&copy); i++) {
      ALLEGRO_BITMAP **bptr;
      int flags;
      bptr = _al_vector_ref(&copy, i);
      flags = (*bptr)->flags;
      flags &= ~ALLEGRO_MEMORY_BITMAP;
      al_set_new_bitmap_flags(flags);
      al_set_new_bitmap_format((*bptr)->format);
      
      ALLEGRO_DEBUG("converting memory bitmap %p to display bitmap\n", *bptr);
      
      al_convert_bitmap(*bptr);
   }

   _al_vector_free(&copy);

   al_unlock_mutex(to_be_converted.mutex);

   al_restore_state(&backup);
}


/* Converts a memory bitmap to a display bitmap preserving its contents.
 * The created bitmap belongs to the current display.
 * 
 * If this is called for a sub-bitmap, the parent also is converted.
 */
void _al_convert_to_display_bitmap(ALLEGRO_BITMAP *bitmap)
{
   ALLEGRO_STATE backup;
   /* Do nothing if it is a display bitmap already. */
   if (!(bitmap->flags & ALLEGRO_MEMORY_BITMAP))
      return;

   ALLEGRO_DEBUG("converting memory bitmap %p to display bitmap\n", bitmap);

   al_store_state(&backup, ALLEGRO_STATE_NEW_BITMAP_PARAMETERS);
   al_set_new_bitmap_flags(ALLEGRO_VIDEO_BITMAP);
   al_set_new_bitmap_format(bitmap->format);
   al_convert_bitmap(bitmap);
   al_restore_state(&backup);
}


/* Converts a display bitmap to a memory bitmap preserving its contents.
 * If this is called for a sub-bitmap, the parent also is converted.
 */
void _al_convert_to_memory_bitmap(ALLEGRO_BITMAP *bitmap)
{
   ALLEGRO_STATE backup;
   bool is_any = (bitmap->flags & ALLEGRO_CONVERT_BITMAP) != 0;

   /* Do nothing if it is a memory bitmap already. */
   if (bitmap->flags & ALLEGRO_MEMORY_BITMAP)
      return;

   ALLEGRO_DEBUG("converting display bitmap %p to memory bitmap\n", bitmap);

   al_store_state(&backup, ALLEGRO_STATE_NEW_BITMAP_PARAMETERS);
   al_set_new_bitmap_flags(ALLEGRO_MEMORY_BITMAP);
   al_set_new_bitmap_format(bitmap->format);
   al_convert_bitmap(bitmap);
   if (is_any) {
      /* We force-converted to memory above, but we still want to
       * keep the ANY flag if it was set so the bitmap can be
       * back-converted later.
       */
      bitmap->flags |= ALLEGRO_CONVERT_BITMAP;
      _al_check_to_be_converted_list_add(bitmap);
   }
   al_restore_state(&backup);
}


void _al_convert_bitmap_data(
   void *src, int src_format, int src_pitch,
   void *dst, int dst_format, int dst_pitch,
   int sx, int sy, int dx, int dy, int width, int height)
{
   ASSERT(src);
   ASSERT(dst);
   ASSERT(_al_pixel_format_is_real(dst_format));

   /* Use memcpy if no conversion is needed. */
   if (src_format == dst_format) {
      int y;
      int size = al_get_pixel_size(src_format);
      char *src_ptr = ((char *)src) + sy * src_pitch + sx * size;
      char *dst_ptr = ((char *)dst) + dy * dst_pitch + dx * size;
      width *= size;
      for (y = 0; y < height; y++) {
         memcpy(dst_ptr, src_ptr, width);
         src_ptr += src_pitch;
         dst_ptr += dst_pitch;
      }
      return;
   }

   (_al_convert_funcs[src_format][dst_format])(src, src_pitch,
      dst, dst_pitch, sx, sy, dx, dy, width, height);
}


/* vim: set ts=8 sts=3 sw=3 et: */
