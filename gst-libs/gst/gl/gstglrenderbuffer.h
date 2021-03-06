/*
 * GStreamer
 * Copyright (C) 2015 Matthew Waters <matthew@centricular.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef _GST_GL_RENDERBUFFER_H_
#define _GST_GL_RENDERBUFFER_H_

#include <gst/gst.h>
#include <gst/gstallocator.h>
#include <gst/gstmemory.h>

#include <gst/gl/gstglbasememory.h>

G_BEGIN_DECLS

#define GST_TYPE_GL_RENDERBUFFER_ALLOCATOR (gst_gl_renderbuffer_allocator_get_type())
GType gst_gl_renderbuffer_allocator_get_type(void);

#define GST_IS_GL_RENDERBUFFER_ALLOCATOR(obj)              (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GST_TYPE_GL_RENDERBUFFER_ALLOCATOR))
#define GST_IS_GL_RENDERBUFFER_ALLOCATOR_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE ((klass), GST_TYPE_GL_RENDERBUFFER_ALLOCATOR))
#define GST_GL_RENDERBUFFER_ALLOCATOR_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS ((obj), GST_TYPE_GL_RENDERBUFFER_ALLOCATOR, GstGLRenderbufferAllocatorClass))
#define GST_GL_RENDERBUFFER_ALLOCATOR(obj)                 (G_TYPE_CHECK_INSTANCE_CAST ((obj), GST_TYPE_GL_RENDERBUFFER_ALLOCATOR, GstGLRenderbufferAllocator))
#define GST_GL_RENDERBUFFER_ALLOCATOR_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST ((klass), GST_TYPE_GL_RENDERBUFFER_ALLOCATOR, GstGLRenderbufferAllocatorClass))
#define GST_GL_RENDERBUFFER_ALLOCATOR_CAST(obj)            ((GstGLRenderbufferAllocator *)(obj))

#define GST_GL_RENDERBUFFER_CAST(obj) ((GstGLRenderbuffer *) obj)

/**
 * GST_GL_RENDERBUFFER_ALLOCATOR_NAME:
 *
 * The name of the GL renderbuffer allocator
 */
#define GST_GL_RENDERBUFFER_ALLOCATOR_NAME   "GLRenderbuffer"

/**
 * GstGLRenderbuffer:
 * @mem: the parent object
 * @renderbuffer_id: the GL texture id for this memory
 * @renderbuffer_type: the texture type
 * @width: the width
 * @height: the height
 *
 * Represents information about a GL renderbuffer
 */
struct _GstGLRenderbuffer
{
  GstGLBaseMemory           mem;

  guint                     renderbuffer_id;
  GstVideoGLTextureType     renderbuffer_type;
  guint                     width;
  guint                     height;

  /* <protected> */
  gboolean                  renderbuffer_wrapped;
};

/**
 * GstGLRenderbufferAllocator
 *
 * Opaque #GstGLRenderbufferAllocator struct
 */
struct _GstGLRenderbufferAllocator
{
  GstGLBaseMemoryAllocator parent;
};

/**
 * GstGLRenderbufferAllocatorClass:
 *
 * The #GstGLRenderbufferAllocatorClass only contains private data
 */
struct _GstGLRenderbufferAllocatorClass
{
  GstGLBaseMemoryAllocatorClass             parent_class;
};

#include <gst/gl/gstglbasememory.h>

typedef struct
{
  GstGLAllocationParams parent;

  GstVideoGLTextureType renderbuffer_type;
  guint width;
  guint height;
} GstGLRenderbufferAllocationParams;

GstGLRenderbufferAllocationParams *     gst_gl_renderbuffer_allocation_params_new           (GstGLContext * context,
                                                                                             GstAllocationParams * alloc_params,
                                                                                             GstVideoGLTextureType renderbuffer_type,
                                                                                             guint width,
                                                                                             guint height);

GstGLRenderbufferAllocationParams *     gst_gl_renderbuffer_allocation_params_new_wrapped   (GstGLContext * context,
                                                                                             GstAllocationParams * alloc_params,
                                                                                             GstVideoGLTextureType renderbuffer_type,
                                                                                             guint width,
                                                                                             guint height,
                                                                                             gpointer gl_handle,
                                                                                             gpointer user_data,
                                                                                             GDestroyNotify notify);

void            gst_gl_renderbuffer_init_once   (void);
gboolean        gst_is_gl_renderbuffer          (GstMemory * mem);

/* accessors */
gint                    gst_gl_renderbuffer_get_width     (GstGLRenderbuffer * gl_mem);
gint                    gst_gl_renderbuffer_get_height    (GstGLRenderbuffer * gl_mem);
GstVideoGLTextureType   gst_gl_renderbuffer_get_type      (GstGLRenderbuffer * gl_mem);
guint                   gst_gl_renderbuffer_get_id        (GstGLRenderbuffer * gl_mem);

G_END_DECLS

#endif /* _GST_GL_RENDERBUFFER_H_ */
