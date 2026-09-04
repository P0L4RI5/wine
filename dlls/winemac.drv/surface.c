/*
 * Mac driver window surface implementation
 *
 * Copyright 1993, 1994, 2011 Alexandre Julliard
 * Copyright 2006 Damjan Jovanovic
 * Copyright 2012, 2013 Ken Thomases for CodeWeavers, Inc.
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

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include "macdrv.h"
#include "winuser.h"

WINE_DEFAULT_DEBUG_CHANNEL(bitblt);

static inline int get_dib_stride(int width, int bpp)
{
    return ((width * bpp + 31) >> 3) & ~3;
}

static inline int get_dib_image_size(const BITMAPINFO *info)
{
    return get_dib_stride(info->bmiHeader.biWidth, info->bmiHeader.biBitCount)
        * abs(info->bmiHeader.biHeight);
}


struct macdrv_window_surface
{
    struct window_surface   header;
    macdrv_window           window;
    IOSurfaceRef            front_buffer;
    IOSurfaceRef            back_buffer;
    BOOL                    shape_changed;
};

static struct macdrv_window_surface *get_mac_surface(struct window_surface *surface);

static IOSurfaceRef create_io_surface(int width, int height)
{
    CFStringRef keys[] = { kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfaceBytesPerElement, kIOSurfacePixelFormat };
    CFNumberRef values[4];
    CFDictionaryRef properties;
    IOSurfaceRef surface;
    uint32_t surface_width = width, surface_height = height;
    uint32_t bytes_per_element = 4, pixel_format = 'BGRA';

    values[0] = CFNumberCreate(NULL, kCFNumberSInt32Type, &surface_width);
    values[1] = CFNumberCreate(NULL, kCFNumberSInt32Type, &surface_height);
    values[2] = CFNumberCreate(NULL, kCFNumberSInt32Type, &bytes_per_element);
    values[3] = CFNumberCreate(NULL, kCFNumberSInt32Type, &pixel_format);
    properties = CFDictionaryCreate(NULL, (const void **)keys, (const void **)values, ARRAY_SIZE(keys),
                                    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    surface = IOSurfaceCreate(properties);
    CFRelease(properties);
    CFRelease(values[0]);
    CFRelease(values[1]);
    CFRelease(values[2]);
    CFRelease(values[3]);
    return surface;
}

static void fill_io_surface(IOSurfaceRef surface, DWORD color)
{
    BYTE *base;
    size_t row_bytes, y, height;

    IOSurfaceLock(surface, 0, NULL);
    base = IOSurfaceGetBaseAddress(surface);
    row_bytes = IOSurfaceGetBytesPerRow(surface);
    height = IOSurfaceGetHeight(surface);
    for (y = 0; y < height; y++)
        memset_pattern4(base + y * row_bytes, &color, IOSurfaceGetWidth(surface) * 4);
    IOSurfaceUnlock(surface, 0, NULL);
}

/***********************************************************************
 *              macdrv_surface_set_clip
 */
static void macdrv_surface_set_clip(struct window_surface *window_surface, const RECT *rects, UINT count)
{
}

static void apply_shape_alpha(IOSurfaceRef io_surface, const BITMAPINFO *shape_info, const void *shape_bits)
{
    BYTE *dst = IOSurfaceGetBaseAddress(io_surface);
    UINT width = IOSurfaceGetWidth(io_surface), height = IOSurfaceGetHeight(io_surface);
    UINT dst_row_bytes = IOSurfaceGetBytesPerRow(io_surface);
    UINT x, y;

    if (!shape_bits)
    {
        Pixel_8888 alpha1 = { 0, 0, 0, 255 };
        vImage_Buffer buf = { dst, height, width, dst_row_bytes };
        vImageOverwriteChannelsWithPixel_ARGB8888(alpha1, &buf, &buf, 0x1, kvImageNoFlags);
        return;
    }

    {
        const BYTE *src = shape_bits;
        UINT src_row_bytes = shape_info->bmiHeader.biSizeImage / abs(shape_info->bmiHeader.biHeight);

        for (y = 0; y < height; y++)
        {
            const BYTE *src_row = src + y * src_row_bytes;
            BYTE *dst_row = dst + y * dst_row_bytes;
            for (x = 0; x < width; x++)
            {
                BYTE bit = (src_row[x / 8] >> (7 - (x % 8))) & 1;
                if (bit)
                    dst_row[x * 4 + 3] = 255;
                else
                    ((DWORD *)dst_row)[x] = 0;
            }
        }
    }
}

/***********************************************************************
 *              macdrv_surface_flush
 */
static BOOL macdrv_surface_flush(struct window_surface *window_surface, const RECT *rect, const RECT *dirty,
                                 const BITMAPINFO *color_info, const void *color_bits, BOOL shape_changed,
                                 const BITMAPINFO *shape_info, const void *shape_bits)
{
    struct macdrv_window_surface *surface = get_mac_surface(window_surface);
    IOSurfaceRef io_surface = surface->back_buffer;

    surface->back_buffer = surface->front_buffer;
    surface->front_buffer = io_surface;
    IOSurfaceLock(io_surface, 0, NULL);

    /* color_bits are BGRX. vImage "ARGB8888" channel bits map onto that
     * memory as: 0x8=byte0(B), 0x4=byte1(G), 0x2=byte2(R), 0x1=byte3(X).
     * Copy RGB only; alpha comes from the 1-bit shape mask. */
    {
        vImage_Buffer src = {
            .data = (void *)color_bits,
            .height = IOSurfaceGetHeight(io_surface),
            .width = IOSurfaceGetWidth(io_surface),
            .rowBytes = color_info->bmiHeader.biSizeImage / abs(color_info->bmiHeader.biHeight),
        };
        vImage_Buffer dst = {
            .data = IOSurfaceGetBaseAddress(io_surface),
            .height = IOSurfaceGetHeight(io_surface),
            .width = IOSurfaceGetWidth(io_surface),
            .rowBytes = IOSurfaceGetBytesPerRow(io_surface),
        };
        vImageSelectChannels_ARGB8888(&src, &dst, &dst, 0x8 | 0x4 | 0x2, kvImageNoFlags);
    }

    if (shape_changed || surface->shape_changed)
    {
        surface->shape_changed = FALSE;
        apply_shape_alpha(io_surface, shape_info, shape_bits);
    }

    IOSurfaceUnlock(io_surface, 0, NULL);
    macdrv_window_set_io_surface(surface->window, io_surface, cgrect_from_rect(*rect), cgrect_from_rect(*dirty));

    if (shape_changed)
    {
        surface->shape_changed = TRUE;
        macdrv_window_shape_changed(surface->window, !!shape_bits);
    }

    return TRUE;
}

/***********************************************************************
 *              macdrv_surface_destroy
 */
static void macdrv_surface_destroy(struct window_surface *window_surface)
{
    struct macdrv_window_surface *surface = get_mac_surface(window_surface);

    TRACE("freeing %p\n", surface);
    if (surface->back_buffer) CFRelease(surface->back_buffer);
    if (surface->front_buffer) CFRelease(surface->front_buffer);
}

static const struct window_surface_funcs macdrv_surface_funcs =
{
    macdrv_surface_set_clip,
    macdrv_surface_flush,
    macdrv_surface_destroy,
};

static struct macdrv_window_surface *get_mac_surface(struct window_surface *surface)
{
    if (!surface || surface->funcs != &macdrv_surface_funcs) return NULL;
    return (struct macdrv_window_surface *)surface;
}

/***********************************************************************
 *              create_surface
 */
static struct window_surface *create_surface(HWND hwnd, macdrv_window window, const RECT *rect)
{
    struct macdrv_window_surface *surface;
    int width = rect->right - rect->left, height = rect->bottom - rect->top;
    DWORD window_background;
    char buffer[FIELD_OFFSET(BITMAPINFO, bmiColors[256])];
    BITMAPINFO *info = (BITMAPINFO *)buffer;
    struct window_surface *window_surface;
    IOSurfaceRef io_surface1 = NULL, io_surface2 = NULL;
    HBITMAP bitmap;
    void *bits;

    memset(info, 0, sizeof(*info));
    info->bmiHeader.biSize        = sizeof(info->bmiHeader);
    info->bmiHeader.biWidth       = width;
    info->bmiHeader.biHeight      = -height; /* top-down */
    info->bmiHeader.biPlanes      = 1;
    info->bmiHeader.biBitCount    = 32;
    info->bmiHeader.biSizeImage   = get_dib_image_size(info);
    info->bmiHeader.biCompression = BI_RGB;

    if (!(bitmap = NtGdiCreateDIBSection(0, NULL, 0, info, DIB_RGB_COLORS, 0, 0, 0, &bits)))
        return NULL;

    window_background = macdrv_window_background_color();

    if (!(io_surface1 = create_io_surface(width, height)) ||
        !(io_surface2 = create_io_surface(width, height)))
    {
        if (io_surface1) CFRelease(io_surface1);
        NtGdiDeleteObjectApp(bitmap);
        return NULL;
    }

    fill_io_surface(io_surface1, window_background);
    fill_io_surface(io_surface2, window_background);

    if (!(window_surface = window_surface_create(sizeof(*surface), &macdrv_surface_funcs, hwnd, rect, info, bitmap)))
    {
        NtGdiDeleteObjectApp(bitmap);
        CFRelease(io_surface1);
        CFRelease(io_surface2);
    }
    else
    {
        surface = get_mac_surface(window_surface);
        surface->window = window;
        surface->front_buffer = io_surface1;
        surface->back_buffer = io_surface2;
        surface->shape_changed = FALSE;
        window_background &= 0x00ffffff;
        memset_pattern4(bits, &window_background, info->bmiHeader.biSizeImage);
    }

    return window_surface;
}


/***********************************************************************
 *              CreateWindowSurface   (MACDRV.@)
 */
BOOL macdrv_CreateWindowSurface(HWND hwnd, BOOL layered, const RECT *surface_rect, struct window_surface **surface)
{
    struct window_surface *previous;
    struct macdrv_win_data *data;

    TRACE("hwnd %p, layered %u, surface_rect %s, surface %p\n", hwnd, layered, wine_dbgstr_rect(surface_rect), surface);

    if ((previous = *surface) && previous->funcs == &macdrv_surface_funcs) return TRUE;
    if (!(data = get_win_data(hwnd))) return TRUE; /* use default surface */
    if (previous) window_surface_release(previous);

    if (layered)
    {
        data->layered = TRUE;
        data->ulw_layered = TRUE;
    }

    *surface = create_surface(hwnd, data->cocoa_window, surface_rect);

    release_win_data(data);
    return TRUE;
}
