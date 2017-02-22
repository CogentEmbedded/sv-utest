/*******************************************************************************
 *
 * Interface adapter for Renesas Electronics EGL extensions
 *
 * Copyright (c) 2017 Cogent Embedded Inc. ALL RIGHTS RESERVED.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *******************************************************************************/

#ifndef SV_SURROUNDVIEW_EGL_RENESAS_H
#define SV_SURROUNDVIEW_EGL_RENESAS_H

/*******************************************************************************
 * Includes
 ******************************************************************************/
#include <EGL/eglext_REL.h>

/*******************************************************************************
 * Definitions
 ******************************************************************************/
#define EGL_NATIVE_PIXFORMAT_NV12 (EGL_NATIVE_PIXFORMAT_NV12_REL)
/* NV16 is not supported on some boards */
#if defined (EGL_NATIVE_PIXFORMAT_NV16_REL)
#define EGL_NATIVE_PIXFORMAT_NV16 (EGL_NATIVE_PIXFORMAT_NV16_REL)
#else
#define EGL_NATIVE_PIXFORMAT_NV16 (-1)
#endif
#define EGL_NATIVE_PIXFORMAT_UYVY (EGL_NATIVE_PIXFORMAT_UYVY_REL)

#if defined (EGL_NATIVE_PIXFORMAT_I420_REL)
#define EGL_NATIVE_PIXFORMAT_I420 (EGL_NATIVE_PIXFORMAT_I420_REL)
#else
#define EGL_NATIVE_PIXFORMAT_I420 (-1)
#endif

#if defined (EGL_NATIVE_PIXFORMAT_R8_REL)
#define EGL_NATIVE_PIXFORMAT_R8 (EGL_NATIVE_PIXFORMAT_R8_REL)
#else
#define EGL_NATIVE_PIXFORMAT_R8 (13)
#endif

#define PIXMAP_INITIALIZER(w, h, f, d)          \
    {                                           \
        .width = (w),                           \
        .height = (h),                          \
        .stride = (w),                          \
        .usage = 0,                             \
        .format = (f),                          \
        .pixelData = (d)                        \
    }

#define DEFINE_PIXMAP(pixmap, w, h, f, d) \
    EGLNativePixmapTypeREL pixmap = PIXMAP_INITIALIZER(w, h, f, d)

#endif /* SV_SURROUNDVIEW_EGL_RENESAS_H */
