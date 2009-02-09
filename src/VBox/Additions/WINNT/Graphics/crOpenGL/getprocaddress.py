# Copyright (c) 2001, Stanford University
# All rights reserved.
#
# See the file LICENSE.txt for information on redistributing this software.

import sys

import apiutil

apiutil.CopyrightC()

print """
/* DO NOT EDIT - THIS FILE GENERATED BY THE getprocaddress.py SCRIPT */

#include "chromium.h"
#include "cr_string.h"
#include "cr_version.h"
#include "stub.h"


struct name_address {
  const char *name;
  CR_PROC address;
};

static struct name_address functions[] = {
"""


keys = apiutil.GetAllFunctions(sys.argv[1]+"/APIspec.txt")
for func_name in keys:
	if "Chromium" == apiutil.Category(func_name):
		continue
	if func_name == "BoundsInfoCR":
		continue
	if "GL_chromium" == apiutil.Category(func_name):
		pass #continue

	wrap = apiutil.GetCategoryWrapper(func_name)
	name = "gl" + func_name
	address = "gl" + func_name
	if wrap:
		print '#ifdef CR_%s' % wrap
	print '\t{ "%s", (CR_PROC) %s },' % (name, address)
	if wrap:
		print '#endif'


print "\t/* Chromium binding/glue functions */"

for func_name in keys:
	if (func_name == "Writeback" or
		func_name == "BoundsInfoCR"):
		continue
	if apiutil.Category(func_name) == "Chromium":
		print '\t{ "cr%s", (CR_PROC) cr%s },' % (func_name, func_name)


print """
	{ NULL, NULL }
};

DECLEXPORT(void) glXBindTexImageEXT(Display *dpy, GLXDrawable draw, int buffer, const int *attrib_list);
DECLEXPORT(void) glXReleaseTexImageEXT(Display *dpy, GLXDrawable draw, int buffer);
DECLEXPORT(void) glXQueryDrawable(Display *dpy, GLXDrawable draw, int attribute, unsigned int *value);
DECLEXPORT(GLXFBConfig *) glXGetFBConfigs(Display *dpy, int screen, int *nelements);
DECLEXPORT(int) glXGetFBConfigAttrib(Display *dpy, GLXFBConfig config, int attribute, int *value);
DECLEXPORT(GLXPixmap) glXCreatePixmap(Display *dpy, GLXFBConfig config, Pixmap pixmap, const int *attrib_list);


CR_PROC CR_APIENTRY crGetProcAddress( const char *name )
{
	int i;
	stubInit();

	for (i = 0; functions[i].name; i++) {
		if (crStrcmp(name, functions[i].name) == 0) {
			return functions[i].address;
		}
	}

    if (!crStrcmp( name, "glXBindTexImageEXT" )) return (CR_PROC) glXBindTexImageEXT;
    if (!crStrcmp( name, "glXReleaseTexImageEXT" )) return (CR_PROC) glXReleaseTexImageEXT;
    if (!crStrcmp( name, "glXQueryDrawable" )) return (CR_PROC) glXQueryDrawable;
    if (!crStrcmp( name, "glXGetFBConfigs" )) return (CR_PROC) glXGetFBConfigs;
    if (!crStrcmp( name, "glXGetFBConfigAttrib" )) return (CR_PROC) glXGetFBConfigAttrib;
    if (!crStrcmp( name, "glXCreatePixmap" )) return (CR_PROC) glXCreatePixmap;

    if (name) crDebug("Returning NULL for %s", name);
	return NULL;
}

"""



# XXX should crGetProcAddress really handle WGL/GLX functions???

print_foo = """
/* As these are Windows specific (i.e. wgl), define these now.... */
#ifdef WINDOWS
	{
		wglGetExtensionsStringEXTFunc_t wglGetExtensionsStringEXT = NULL;
		wglChoosePixelFormatFunc_t wglChoosePixelFormatEXT = NULL;
		wglGetPixelFormatAttribivEXTFunc_t wglGetPixelFormatAttribivEXT = NULL;
		wglGetPixelFormatAttribfvEXTFunc_t wglGetPixelFormatAttribfvEXT = NULL;
		if (!crStrcmp( name, "wglGetExtensionsStringEXT" )) return (CR_PROC) wglGetExtensionsStringEXT;
		if (!crStrcmp( name, "wglChoosePixelFormatEXT" )) return (CR_PROC) wglChoosePixelFormatEXT;
		if (!crStrcmp( name, "wglGetPixelFormatAttribivEXT" )) return (CR_PROC) wglGetPixelFormatAttribivEXT;
		if (!crStrcmp( name, "wglGetPixelFormatAttribfvEXT" )) return (CR_PROC) wglGetPixelFormatAttribfvEXT;
	}
#endif
"""
