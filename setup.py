#!/usr/bin/env python

from distutils.core import setup, Extension

setup(	name="gpio",
	version="0.0",
	description="Python bindings for Linux GPIO access through sysfs",
	author="Volker Thoms",
	author_email="unconnected@gmx.de",
	maintainer="Stephen Caudle",
	maintainer_email="scaudle@doceme.com",
	license="GPLv2",
	url="http://github.com/doceme/py-gpio",
	ext_modules=[Extension("gpio", ["gpiomodule.c"])])
