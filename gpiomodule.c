/*
 * gpiomodule.c - Python bindings for Linux GPIO access through sysfs
 * Copyright (C) 2009 Volker Thoms <unconnected@gmx.de>
 * Copyright (C) 2012 Stephen Caudle <scaudle@doceme.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

/*
 * feature: lock
 * normally we don't hold a lock on the value/direction files
 * to prevent other applications accessing them
 * - this might become a feature in later releases
 */

#include <Python.h>
#include <pythread.h>
#include "structmember.h"
#include "fcntl.h"
#include "string.h"
#include <poll.h>

PyDoc_STRVAR(GPIO_module_doc,
	"This module defines an object type that allows GPIO transactions\n"
	"on hosts running the Linux kernel.  The host kernel must have GPIO\n"
	"support and GPIO sysfs support.\n"
	"All of these can be either built-in to the kernel, or loaded from\n"
	"modules.");

#define MAXPATH 48

#define GPIO_EXPORT "/sys/class/gpio/export"
#define GPIO_UNEXPORT "/sys/class/gpio/unexport"
#define GPIO_BASE_PATH "/sys/class/gpio/gpio"

#define GPIO_PATH(x,buf) \
				snprintf((buf), MAXPATH, "%s%d", GPIO_BASE_PATH, (x))
#define GPIO_DIREC(x,buf) \
				snprintf((buf), MAXPATH, "%s%d/direction", GPIO_BASE_PATH, (x))
#define GPIO_VALUE(x,buf) \
				snprintf((buf), MAXPATH, "%s%d/value", GPIO_BASE_PATH, (x))

// GPIO_EDGE: kernel needs to be patched with gpiolib-allow-poll-on-value - patch
// this patch seems to be in mainline kernel from 2.6.32 on
#define GPIO_EDGE(x,buf) \
				snprintf((buf), MAXPATH, "%s%d/edge", GPIO_BASE_PATH, (x))
typedef struct {
	PyObject_HEAD

	int gpio;	/* number of gpio */
	PyObject * direction;
	PyObject * trigger;
	PyObject * callback;

	char e_path[MAXPATH];
	char d_path[MAXPATH];
	char v_path[MAXPATH];

	int fd_val;	/* value fd */
	int fd_dir;	/* direction fd */
	int fd_edge;	/* interrupt edge fd  */
	
} GPIO;

PyInterpreterState* interp;
PyThread_type_lock lock;

//typedef struct poll_cb_info;

struct poll_cb_info {
	PyObject * callback;
	int	fd;
	int	idx;
	struct poll_cb_info * next;
};
struct poll_cb_info * global_poll_cb_info;

int global_nfds;
struct pollfd global_pfd[16];

// return 1 if deletion successful, 0 if not found


#ifdef DEBUG_POLL
static void dump_poll_cb() {
	printf("dump_poll_cb() starting at %p\n", global_poll_cb_info);
	struct poll_cb_info * poll_cb_info_iterator;
	poll_cb_info_iterator = global_poll_cb_info;
	int i = 0;

	while (poll_cb_info_iterator != NULL) {
		printf("dump_poll_cb(%i) fd = %i, callback = %p, next = %p\n", i, poll_cb_info_iterator->fd, poll_cb_info_iterator->callback, poll_cb_info_iterator->next );
		poll_cb_info_iterator = poll_cb_info_iterator->next;
		i++;
	}
}
#endif

static int del_poll_cb(int fd) {
	struct poll_cb_info * poll_cb_info_iterator, *tmp;

	poll_cb_info_iterator = global_poll_cb_info;

	while (poll_cb_info_iterator != NULL) {
		if (poll_cb_info_iterator->fd == fd) {

			global_pfd[poll_cb_info_iterator->idx].fd = global_pfd[global_nfds].fd;
			global_nfds--;

			if (poll_cb_info_iterator == global_poll_cb_info) {
				free(global_poll_cb_info);
				global_poll_cb_info = poll_cb_info_iterator->next;
				return 1;
			} else {
				tmp->next = poll_cb_info_iterator->next;
				free(poll_cb_info_iterator);
			//	poll_cb_info_iterator = NULL;
			//	printf("del_poll_cb() done; global_nfds now: %i\n", global_nfds);
				return 1;
			}
		} else {
			tmp = poll_cb_info_iterator;
			poll_cb_info_iterator = poll_cb_info_iterator->next;
		}
	}
	return 0;
}

static struct poll_cb_info * get_poll_cb(int fd) {
//	printf("get_poll_cb() for fd %i\n", fd);
	struct poll_cb_info * poll_cb_info_iterator;
	poll_cb_info_iterator = global_poll_cb_info;

	while (poll_cb_info_iterator != NULL) {
//		printf("get_poll_cb() is poll_cb_info_iterator->fd (%i) == fd (%i) ?\n", poll_cb_info_iterator->fd, fd );
		if (poll_cb_info_iterator->fd == fd) break;
		poll_cb_info_iterator = poll_cb_info_iterator->next;
	}
/*	if(poll_cb_info_iterator != NULL)
		printf("get_poll_cb() done; found\n");
	else
		printf("get_poll_cb() done; not found\n");
*/	return poll_cb_info_iterator;
}

static int add_poll_cb(PyObject * callback, int fd)
{
	struct poll_cb_info * poll_cb_info_iterator;

	if ( get_poll_cb(fd) != NULL ) return 0;

	poll_cb_info_iterator = malloc(sizeof(struct poll_cb_info));
	if (poll_cb_info_iterator == NULL) return -1;

//	printf("add_poll_cb(): setting next to %x\n", global_poll_cb_info);
	poll_cb_info_iterator->next = global_poll_cb_info;
//	printf("add_poll_cb(): setting global_poll_cb_info to %x\n", poll_cb_info_iterator);
	global_poll_cb_info = poll_cb_info_iterator;

	poll_cb_info_iterator->fd = fd;
	poll_cb_info_iterator->callback = callback;

	global_pfd[global_nfds].fd = fd;
	global_pfd[global_nfds].events = POLLPRI | POLLERR;
	poll_cb_info_iterator->idx = global_nfds;
	global_nfds++;
//	printf("add_poll_cb() done; global_nfds now: %i\n", global_nfds);
	return 0;
}


static int getl( int fd, char * res )
{
	int i = 0;
	char c;

	while(1) {
		lseek( fd, i, SEEK_SET);
		if (read( fd, &c, 1 ) != 1)
			break;
		if (c == '\n') {
			res[i] = '\0';
			break;
		}
		res[i] = c;
		i++;
	}
	return i;
}

static void t_bootstrap(void* rawself)
{
//	printf("Hello, iam Threadi :)\n");
	PyObject * callback;
	PyThreadState *tstate;
	PyObject* args = NULL;
	char buf;

	tstate = PyThreadState_New(interp);
	PyEval_AcquireThread(tstate);
	int retval, i;

	PyObject *res = NULL;

	while (global_nfds > 0) {

		Py_BEGIN_ALLOW_THREADS
		retval = poll(global_pfd, global_nfds, -1);
		Py_END_ALLOW_THREADS

		if (retval == 0) {
			// poll shouldn't timeout
			PyErr_SetFromErrno(PyExc_IOError);
			break;
		}

		if (retval < 0) {
 			PyErr_SetFromErrno(PyExc_IOError);
			break;
		}
//		printf("Threadi: nfds: %i\n", global_nfds);
		for (i = 0; i < global_nfds; i++) {
			if (global_pfd[i].revents != 0) {
//				fd_glow = global_pfd[i].fd;
				lseek(global_pfd[i].fd, 0, SEEK_SET);
				if (read(global_pfd[i].fd, &buf, 1 ) != 1) {
					PyErr_SetFromErrno(PyExc_IOError);
					break;
				}
				res = NULL;
				args = Py_BuildValue("(i)", buf - 48); // we got ASCII '0'
				callback = get_poll_cb(global_pfd[i].fd)->callback;
//				printf("Threadi: fd: %i, revents: %i\n", global_pfd[i].fd, global_pfd[i].revents);
				PyGILState_STATE s = PyGILState_Ensure();
				res = PyObject_CallObject(callback, args);
				PyGILState_Release(s);
//				printf("Threadi :5\n");
				Py_DECREF(args);
				if (res == NULL) {
					Py_XDECREF(res);
					break; /* Pass error back */
				}

				/* Here maybe use res
				  maybe exit loop at some defined return value (i.e. res == -1)
				 */
				Py_DECREF(res);
			}
		}
	}
	if (PyErr_Occurred()) {
		if (PyErr_ExceptionMatches(PyExc_SystemExit))
			PyErr_Clear();
		else {
			PySys_WriteStderr("Unhandled exception in poll thread:\n");
			PyErr_PrintEx(0);
		}
	}
	PyThread_release_lock(lock);
//	printf("terminating poll thread\n");
//	PyEval_ReleaseThread(tstate);
	lock = NULL;
	PyThreadState_Clear(tstate);
	PyThreadState_DeleteCurrent();
	PyThread_exit_thread();
}

static PyObject *
getDirection(GPIO * self) {

	PyObject * res;
	char buf[5];		// in, out, low or high

	getl(self->fd_dir, &buf[0]);

	res = PyBytes_FromString(&buf[0]);

	Py_INCREF(res);
	return res;
}

static PyObject *
setDirection(GPIO * self, PyObject * cdir) {
	int len;
	int res;

	len = strlen(PyBytes_AsString(cdir));
	lseek(self->fd_dir, 0, SEEK_SET);
	res = write ( self->fd_dir, PyBytes_AsString( cdir ), len );
	if ( res != len ) {
		PyErr_SetFromErrno( PyExc_IOError );
		return NULL;
	}

	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *
getTrigger(GPIO * self) {

	PyObject * res;
	char buf[10];

	getl(self->fd_edge, &buf[0]);

	res = PyBytes_FromString(&buf[0]);

	Py_INCREF(res);
	return res;
}

static PyObject *
setTrigger(GPIO * self, PyObject * ctrig) {
	int len;
	int res;

	len = strlen(PyBytes_AsString(ctrig));
	lseek(self->fd_edge, 0, SEEK_SET);
	res = write ( self->fd_edge, PyBytes_AsString( ctrig ), len );
	if ( res != len ) {
		PyErr_SetFromErrno( PyExc_IOError );
		return NULL;
	}

	Py_INCREF(Py_None);
	return Py_None;
}

int exportGpio( int gpio ) {
	char c_gpio[4];		// 4 chars should be enough for GPIO number
				// plus trailing '\0'
	int fd;

	if ((fd = open(GPIO_EXPORT, O_WRONLY ) ) < 0 ) {
		PyErr_SetFromErrno( PyExc_IOError );
		return -1;
	}

	sprintf(c_gpio, "%i", gpio);
	if (write( fd, &c_gpio, strlen(c_gpio) ) != strlen(c_gpio)) {
		PyErr_SetFromErrno( PyExc_IOError );
		close( fd );
		return -1;
	}
	close( fd );
	return 0;
}

static PyObject *
GPIO_get_value(GPIO * self) {
	char cval[2];
	PyObject * i;

	getl(self->fd_val, &cval[0]);
	i = PyInt_FromString(&cval[0], NULL, 10);
	
	// printf("\nget_value() returning %i (%i)\n", cval[0] - 48, PyInt_AsLong(i));
	return i;

}

static int
GPIO_set_value(GPIO * self, PyObject * cval) {
	char val;

	if (PyInt_AsLong(cval) == 0) {
		val = '0';
	} else {
		val = '1';
	}

	lseek(self->fd_val, 0, SEEK_SET);
	if ( write ( self->fd_val, &val, 1 ) != 1 ) {
		PyErr_SetFromErrno( PyExc_IOError );
		return -1;
	}
		
	return 0;
}

static PyObject *
GPIO_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
	GPIO *self;

	if ((self = (GPIO *)type->tp_alloc(type, 0)) == NULL)
		return NULL;

	self->callback = NULL;
	self->direction = NULL;
	self->trigger = NULL;
	self->gpio = -1;
	self->fd_val = -1;
	self->fd_dir = -1;
	self->fd_edge = -1;

	return (PyObject *)self;
}

static void
GPIO_dealloc(GPIO *self)
{
//	printf("%i[%x]:GPIO_dealloc() pre - global_nfds: %i\n", self->gpio, self, global_nfds);
	PyObject * cb = NULL;
	struct poll_cb_info * poll_cb;

	poll_cb = get_poll_cb(self->fd_val);
//	printf("%i[%x]:GPIO_dealloc() 1\n", self->gpio, self);
	if (poll_cb != NULL)
		cb = poll_cb->callback;
//	printf("%i[%x]:GPIO_dealloc() 2\n", self->gpio, self);
	if ( cb != NULL ) {
//		printf("pre-del:\n");
#ifdef DEBUG_POLL
		dump_poll_cb();
#endif
		del_poll_cb(self->fd_val);
//		printf("post-del:\n");
#ifdef DEBUG_POLL
		dump_poll_cb();
#endif
		Py_DECREF( cb );
	}
//	printf("%i[%x]:GPIO_dealloc() 3\n", self->gpio, self);
	Py_XDECREF(self->direction);
	Py_XDECREF(self->trigger);
	close(self->fd_val);
	close(self->fd_dir);
	close(self->fd_edge);
//	printf("%i[%x]:GPIO_dealloc() post - global_nfds: %i\n", self->gpio, self, global_nfds);
	PyObject_Del((PyObject*) self);
//	self->ob_type->tp_free((PyObject *)self);
}

static int
GPIO_init(GPIO *self, PyObject *args, PyObject *kwds)
{
	int fd = -1;
	int gpio = -1;
	
	PyObject * direction = NULL;
	PyObject * trigger = NULL;
	PyObject * tmp = NULL;

	static char *kwlist[] = { "gpio", "direction", "trigger", NULL };

	if ( !PyArg_ParseTupleAndKeywords(args, kwds, "i|OO:__init__",
			kwlist, &gpio, &direction, &trigger ) )
		return -1;

	if (gpio < 0)
		return -1;

	self->gpio = gpio;

	GPIO_VALUE( gpio, self->v_path );
	if ( ( fd = open( self->v_path, O_RDWR, 0 ) ) == -1 ) {
		// try to get gpio exported:
		if ( exportGpio( gpio ) == 0 ) {
			// check if export was (really) successful
			
			if ( ( fd = open( self->v_path, O_RDWR ) ) == -1) {
				// export failed
				PyErr_SetFromErrno( PyExc_IOError );
				return -1;
			}
		} else {
			PyErr_SetString( PyExc_StandardError,
				"Export failed." );
			return -1;
		}
	}

	GPIO_EDGE(gpio, self->e_path);
	GPIO_DIREC(gpio, self->d_path);

	self->fd_val = fd;
	if ( ( self->fd_dir = open( self->d_path, O_RDWR ) ) == -1 ) {
		return -1;
	}

	if ( ( self->fd_edge = open( self->e_path, O_RDWR ) ) == -1 ) {
		return -1;
	}

	if (direction) {
		setDirection( self, direction );
		tmp = self->direction;
		Py_INCREF(direction);
		self->direction = direction;
		Py_XDECREF(tmp);
	} else {
		// no direction requested, use current
		Py_XDECREF(self->direction);
		self->direction = getDirection( self );
		Py_INCREF(self->direction);
	}

	if (trigger) {
		setTrigger( self, trigger );
		tmp = self->trigger;
		Py_INCREF(trigger);
		self->trigger = trigger;
		Py_XDECREF(tmp);
	} else {
		// no trigger requested, use current
		Py_XDECREF(self->trigger);
		self->trigger = getTrigger( self );
		Py_INCREF(self->trigger);
	}

	return 0;
}

PyDoc_STRVAR( GPIO_type_doc,
	"GPIO(gpio) -> GPIO\n\n"
	"Return a new GPIO object that is connected to the\n"
	"specified GPIO via sysfs interface.\n");

static PyObject *
GPIO_get_direction( GPIO *self, void *closure )
{
	PyObject * result = ( PyObject * ) self->direction ;
	Py_INCREF( result );
	return result;
}

static int
GPIO_set_direction(GPIO *self, PyObject *val, void *closure)
{
	if (val == NULL) {
		PyErr_SetString(PyExc_TypeError,
			"Cannot delete attribute");
		return -1;
	}

	if ( setDirection(self, val) == NULL )
		return -1;

	if ( PyObject_Compare(val, getDirection( self ) ) != 0 ) {
		PyErr_SetString(PyExc_IOError, "setting direction failed");
		return -1;
	}

	self->direction = val;

	return 0;
}


static PyObject *
GPIO_get_trigger( GPIO *self, void *closure )
{
	PyObject * result = ( PyObject * ) self->trigger;
	Py_INCREF( result );
	return result;
}

static int
GPIO_set_trigger(GPIO *self, PyObject *val, void *closure)
{
	if (val == NULL) {
		PyErr_SetString(PyExc_TypeError,
			"Cannot delete attribute");
		return -1;
	}

	if ( setTrigger(self, val) == NULL )
		return -1;

	if ( PyObject_Compare(val, getTrigger( self ) ) != 0 ) {
		PyErr_SetString(PyExc_IOError, "setting trigger failed");
		return -1;
	}

	self->trigger = val;

	return 0;
}

static PyObject *
GPIO_get_callback(GPIO *self, void *closure)
{
	PyObject * result = get_poll_cb(self->fd_val)->callback;
	if (result == NULL)
		result = Py_None;
	Py_INCREF(result);
	return result;
}

static int
GPIO_set_callback(GPIO *self, PyObject *val, void *closure)
{
	long ident;
	PyObject *cb;
	struct poll_cb_info * poll_cb;

	cb = NULL;
	poll_cb = get_poll_cb(self->fd_val);
	if (poll_cb != NULL)
		cb = poll_cb->callback;

	if ( cb == val ) return 0;
	if ( cb != NULL ) {
//		printf("pre-del:\n");
#ifdef DEBUG_POLL
		dump_poll_cb();
#endif
		del_poll_cb(self->fd_val);
//		printf("post-del:\n");
#ifdef DEBUG_POLL
		dump_poll_cb();
#endif
		Py_DECREF( cb );
	}
	
	if ( val == Py_None ) return 0;
	
	if ( !PyCallable_Check( val ) != 0 ) {
		PyErr_SetString(PyExc_TypeError, "set_callback: parameter must be a callable python object (function, method, class) or None");
		return -1;
	}

	Py_INCREF(val);
//	printf("pre-add:\n");
#ifdef DEBUG_POLL
	dump_poll_cb();
#endif
	add_poll_cb(val, self->fd_val);
//	printf("post-add:\n");
#ifdef DEBUG_POLL
	dump_poll_cb();
#endif
	
	if (global_nfds == 1 && lock == NULL) {
		PyEval_InitThreads(); /* Start the interpreter's thread-awareness */
		interp = PyThreadState_Get()->interp;
		lock = PyThread_allocate_lock();
		if (lock == NULL) 
			return -1;
		ident = PyThread_start_new_thread(t_bootstrap, (void*) self);
		if (ident == -1) {
			PyErr_SetString(PyExc_RuntimeError, "can't start new thread");
			return -1;
		}
	}

	return 0;
}

//+	{ "none",    0 },
//+	{ "falling", BIT(FLAG_TRIG_FALL) },
//+	{ "rising",  BIT(FLAG_TRIG_RISE) },
//+	{ "both",    BIT(FLAG_TRIG_FALL) | BIT(FLAG_TRIG_RISE) },

static PyGetSetDef GPIO_getset[] = {
	{"value", (getter)GPIO_get_value, (setter)GPIO_set_value,
			"set value if configured as output"},
	{"direction", (getter)GPIO_get_direction, (setter)GPIO_set_direction,
			"Defines GPIO direction (in, out, low or high)"},
	{"trigger", (getter)GPIO_get_trigger, (setter)GPIO_set_trigger,
			"Defines interrupt level (none, falling, rising or both)"},
	{"callback", (getter)GPIO_get_callback, (setter)GPIO_set_callback,
			"set callable python object to be called on interrupt"},
	{NULL},
};

static PyTypeObject GPIO_type = {
	PyObject_HEAD_INIT(NULL)
	0,				/* ob_size */
	"GPIO",				/* tp_name */
	sizeof(GPIO),			/* tp_basicsize */
	0,				/* tp_itemsize */
	(destructor)GPIO_dealloc,	/* tp_dealloc */
	0,				/* tp_print */
	0,				/* tp_getattr */
	0,				/* tp_setattr */
	0,				/* tp_compare */
	0,				/* tp_repr */
	0,				/* tp_as_number */
	0,				/* tp_as_sequence */
	0,				/* tp_as_mapping */
	0,				/* tp_hash */
	0,				/* tp_call */
	0,				/* tp_str */
	0,				/* tp_getattro */
	0,				/* tp_setattro */
	0,				/* tp_as_buffer */
	Py_TPFLAGS_DEFAULT,		/* tp_flags */
	GPIO_type_doc,			/* tp_doc */
	0,				/* tp_traverse */
	0,				/* tp_clear */
	0,				/* tp_richcompare */
	0,				/* tp_weaklistoffset */
	0,				/* tp_iter */
	0,				/* tp_iternext */
	0,			/* tp_methods */
	0,				/* tp_members */
	GPIO_getset,			/* tp_getset */
	0,				/* tp_base */
	0,				/* tp_dict */
	0,				/* tp_descr_get */
	0,				/* tp_descr_set */
	0,				/* tp_dictoffset */
	(initproc)GPIO_init,		/* tp_init */
	0,				/* tp_alloc */
	GPIO_new,			/* tp_new */
};

#ifndef PyMODINIT_FUNC	/* declarations for DLL import/export */
#define PyMODINIT_FUNC void
#endif
PyMODINIT_FUNC
initgpio(void)
{
	PyObject* m;

	if (PyType_Ready(&GPIO_type) < 0)
		return;

	m = Py_InitModule3("gpio", 0, GPIO_module_doc);

	Py_INCREF(&GPIO_type);
	PyModule_AddObject(m, "GPIO", (PyObject *)&GPIO_type);
	lock = NULL;
	global_poll_cb_info = NULL;
	global_nfds = 0;
}

