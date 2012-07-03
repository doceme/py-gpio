# Python bindings for Linux GPIO access through sysfs
#
# Copyright (C) 2009  Volker Thoms <unconnected@gmx.de>
# Copyright (C) 2012  Stephen Caudle <scaudle@doceme.com>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.

PYTHON	?= python

all:
	$(PYTHON) setup.py build

install:
	$(PYTHON) setup.py install

clean:
	$(PYTHON) setup.py clean
	rm -rf build

cleandir distclean: clean
	$(PYTHON) setup.py clean -a
