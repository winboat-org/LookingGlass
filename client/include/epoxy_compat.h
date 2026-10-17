#pragma once

/* The portable build dispatches graphics calls through libepoxy and SoLo. */
#include <epoxy/gl.h>
#include <epoxy/egl.h>
// The EGL renderer does not need GLX headers (which leak Xlib macros).

/* Epoxy provides the GLES entry points as well. Avoid re-declaring them as
 * ordinary functions when upstream includes Khronos extension headers. */
#define __gles2_gl2ext_h_ 1
#define __gles2_gl3_h_ 1
#define __gles2_gl31_h_ 1
#define __gles2_gl32_h_ 1
#include <string.h>
