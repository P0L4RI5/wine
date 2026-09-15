/*
 * MACDRV Cocoa OpenGL code
 *
 * Copyright 2012, 2013 Ken Thomases for CodeWeavers Inc.
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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl.h>

#include "macdrv_cocoa.h"
#include "cocoa_app.h"
#include "cocoa_event.h"

#pragma GCC diagnostic ignored "-Wdeclaration-after-statement"

/***********************************************************************
 *              macdrv_create_opengl_context
 *
 * Returns a Cocoa OpenGL context created from a CoreGL context.  The
 * caller is responsible for calling macdrv_dispose_opengl_context()
 * when done with the context object.
 */
NSOpenGLContext *macdrv_create_opengl_context(void* cglctx)
{
    return [[NSOpenGLContext alloc] initWithCGLContextObj:cglctx];
}

CGLContextObj macdrv_opengl_context_cgl(NSOpenGLContext *context)
{
    return context.CGLContextObj;
}

/***********************************************************************
 *              macdrv_dispose_opengl_context
 *
 * Destroys a Cocoa OpenGL context previously created by
 * macdrv_create_opengl_context();
 */
void macdrv_dispose_opengl_context(NSOpenGLContext *context)
{
    [context release];
}

void macdrv_opengl_context_set_view(NSOpenGLContext *context, WineContentView *view)
{
    OnMainThread(^{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        if (view) [context setView:(NSView *)view];
        else [context clearDrawable];
#pragma clang diagnostic pop
        [context update];
    });
}

/***********************************************************************
 *              macdrv_flush_opengl_context
 *
 * Performs an implicit glFlush() and then swaps the back buffer to the
 * front (if the context is double-buffered).
 */
void macdrv_flush_opengl_context(NSOpenGLContext *context)
{
    [context flushBuffer];
}
