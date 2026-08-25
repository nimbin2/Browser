/*
 * minibrowser - minimal WebKitGTK 6.0 (GTK4) page viewer
 *
 * All of the behaviour lives in browser_core.c. This file exists to say
 * that minibrowser adds nothing to it. Run with -h for the option and
 * key list.
 */

#include "browser_core.h"

static const BrowserApp minibrowser = {
    .tagline        = "minimal WebKitGTK 6.0 / GTK4 page viewer",
    .default_title  = "Minibrowser",
    .default_app_id = "minibrowser",
    .usage_arg      = "URL",
};

int
main (int argc, char **argv)
{
    return browser_main (argc, argv, &minibrowser);
}
