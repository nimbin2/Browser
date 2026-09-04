/*
 * browser-mini - minimal WebKitGTK 6.0 (GTK4) page viewer
 *
 * All of the behaviour lives in browser_core.c. This file exists to say
 * that minibrowser adds nothing to it. Run with -h for the option and
 * key list.
 */

#include "browser_core.h"

static const BrowserApp browser_mini = {
    .tagline        = "minimal WebKitGTK 6.0 / GTK4 page viewer",
    .default_title  = "Browser Mini",
    .default_app_id = "browser-mini",
    .usage_arg      = "URL",
};

int
main (int argc, char **argv)
{
    return browser_main (argc, argv, &browser_mini);
}
