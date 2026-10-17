/* Use host font and keymap configuration when running outside the build store. */
#include <stdlib.h>

__attribute__((constructor)) static void portable_runtime_defaults(void)
{
  setenv("FONTCONFIG_PATH", "/etc/fonts", 0);
  setenv("FONTCONFIG_FILE", "/etc/fonts/fonts.conf", 0);
  setenv("XKB_CONFIG_ROOT", "/usr/share/X11/xkb", 0);
}
