/* X-Chat
 * Copyright (C) 2002 Peter Zelezny.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#ifdef WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#endif

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <istream>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/format.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/utility/string_ref.hpp>

#ifdef WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "hexchat.hpp"
#include "fe.hpp"
#include "util.hpp"
#include "outbound.hpp"
#include "cfgfiles.hpp"
#include "ignore.hpp"
#include "server.hpp"
#include "servlist.hpp"
#include "modes.hpp"
#include "notify.hpp"
#include "text.hpp"
#include "dcc.hpp"
#include "userlist.hpp"
#include "session.hpp"

#define PLUGIN_C
#include "plugin.hpp"
#include "typedef.h"

#include "hexchatc.hpp"

/* the USE_PLUGIN define only removes libdl stuff */

#ifdef USE_PLUGIN
#include <gmodule.h>
#endif

#define DEBUG(x) {x;}
namespace dcc = hexchat::dcc;

/* crafted to be an even 32 bytes */
struct t_hexchat_hook
{
	hexchat_plugin_internal *pl;	/* the plugin to which it belongs */
	char *name;			/* "xdcc" */
	void (*callback)(void*);	/* pointer to xdcc_callback */
	char *help_text;	/* help_text for commands only */
	void *userdata;	/* passed to the callback */
	int tag;				/* for timers & FDs only */
	int type;			/* HOOK_* */
	int pri;	/* fd */	/* priority / fd for HOOK_FD only */
};

struct t_hexchat_list
{
	int type;			/* LIST_* */
	GSList *pos;		/* current pos */
	GSList *next;		/* next pos */
	GSList *head;		/* for LIST_USERS only */
	struct notify_per_server *notifyps;	/* notify_per_server * */
	bool is_vector;
	size_t loc;
	size_t length;
};

typedef int (hexchat_cmd_cb)(const char * const word[], const char * const word_eol[], void *user_data);
typedef int (hexchat_serv_cb)(const char * const word[], const char * const word_eol[], void *user_data);
typedef int (hexchat_print_cb)(const char * const word[], void *user_data);
typedef int (hexchat_serv_attrs_cb) (const char * const word[], const char * const word_eol[], hexchat_event_attrs *attrs, void *user_data);
typedef int (hexchat_print_attrs_cb)(const char * const word[], hexchat_event_attrs *attrs, void *user_data);
typedef int (hexchat_fd_cb) (int fd, int flags, void *user_data);
typedef int (hexchat_timer_cb) (void *user_data);

enum
{
	LIST_CHANNELS,
	LIST_DCC,
	LIST_IGNORE,
	LIST_NOTIFY,
	LIST_USERS
};

/* We use binary flags here because it makes it possible for plugin_hook_find()
 * to match several types of hooks.  This is used so that plugin_hook_run()
 * match both HOOK_SERVER and HOOK_SERVER_ATTRS hooks when plugin_emit_server()
 * is called.
 */
enum
{
	HOOK_COMMAND      = 1 << 0, /* /command */
	HOOK_SERVER       = 1 << 1, /* PRIVMSG, NOTICE, numerics */
	HOOK_SERVER_ATTRS = 1 << 2, /* same as above, with attributes */
	HOOK_PRINT        = 1 << 3, /* All print events */
	HOOK_PRINT_ATTRS  = 1 << 4, /* same as above, with attributes */
	HOOK_TIMER        = 1 << 5, /* timeouts */
	HOOK_FD           = 1 << 6, /* sockets & fds */
	HOOK_DELETED      = 1 << 7  /* marked for deletion */
};

GSList *plugin_list = NULL;  /* export for plugingui.c */
static GSList *hook_list = NULL;


extern struct prefs vars[];	/* cfgfiles.c */


/* unload a plugin and remove it from our linked list */

static int
plugin_free(hexchat_plugin_internal *pl, bool do_deinit, bool allow_refuse)
{
	GSList *list, *next;
	hexchat_hook *hook;
	plugin_deinit_func deinit_func;

	/* fake plugin added by hexchat_plugingui_add() */
	if (pl->fake)
		goto xit;

	/* run the plugin's deinit routine, if any */
	if (do_deinit && pl->deinit_callback != NULL)
	{
		deinit_func = pl->deinit_callback;
		if (!deinit_func (pl) && allow_refuse)
			return FALSE;
	}

	/* remove all of this plugin's hooks */
	list = hook_list;
	while (list)
	{
		hook = static_cast<hexchat_hook*>(list->data);
		next = list->next;
		if (hook->pl == pl)
			hexchat_unhook (NULL, hook);
		list = next;
	}

#ifdef USE_PLUGIN
	if (pl->handle)
		g_module_close (static_cast<GModule*>(pl->handle));
#endif

xit:

	plugin_list = g_slist_remove (plugin_list, pl);

	delete pl;

#ifdef USE_PLUGIN
	fe_pluginlist_update ();
#endif

	return TRUE;
}

static hexchat_plugin_internal *
plugin_list_add (hexchat_context *ctx,  const char filename[], const char *name,
					  const char *desc, const char *version, void *handle,
					  plugin_deinit_func deinit_func, bool fake, bool free_strings)
{
	auto pl = new hexchat_plugin_internal();
	pl->handle = handle;
	if (filename)
	{
		pl->filename = filename;
	}
	pl->context = ctx;
	if (name)
	{
		pl->name = name;
	}
	if (desc)
	{
		pl->desc = desc;
	}
	if (version)
	{
		pl->version = version;
	}
	
	pl->deinit_callback = deinit_func;
	pl->fake = fake;

	plugin_list = g_slist_prepend (plugin_list, pl);

	return pl;
}

static int
hexchat_dummy(hexchat_plugin *, void *, char *, int *)
{
	return -1;
}

#ifdef WIN32
static int
hexchat_read_fd (hexchat_plugin *ph, GIOChannel *source, char *buf, int *len)
{
	GError *error = NULL;

	g_io_channel_set_buffered (source, FALSE);
	g_io_channel_set_encoding (source, NULL, &error);

	if (g_io_channel_read_chars (source, buf, *len, (gsize*)len, &error) == G_IO_STATUS_NORMAL)
	{
		return 0;
	}
	else
	{
		return -1;
	}
}
#endif

/* Load a static plugin */

void
plugin_add (session *sess, const char *filename, void *handle, plugin_init_func init_func,
				plugin_deinit_func deinit_func, char *arg, bool fake)
{
	auto pl = plugin_list_add (sess, filename, filename, NULL, NULL, handle, deinit_func,
								 fake, false);

	if (!fake)
	{
		/* win32 uses these because it doesn't have --export-dynamic! */
		pl->hexchat_hook_command = hexchat_hook_command;
		pl->hexchat_hook_server = hexchat_hook_server;
		pl->hexchat_hook_print = hexchat_hook_print;
		pl->hexchat_hook_timer = hexchat_hook_timer;
		pl->hexchat_hook_fd = hexchat_hook_fd;
		pl->hexchat_unhook = hexchat_unhook;
		pl->hexchat_print = hexchat_print;
		pl->hexchat_printf = hexchat_printf;
		pl->hexchat_command = hexchat_command;
		pl->hexchat_commandf = hexchat_commandf;
		pl->hexchat_nickcmp = hexchat_nickcmp;
		pl->hexchat_set_context = hexchat_set_context;
		pl->hexchat_find_context = hexchat_find_context;
		pl->hexchat_get_context = hexchat_get_context;
		pl->hexchat_get_info = hexchat_get_info;
		pl->hexchat_get_prefs = hexchat_get_prefs;
		pl->hexchat_list_get = hexchat_list_get;
		pl->hexchat_list_free = hexchat_list_free;
		pl->hexchat_list_fields = hexchat_list_fields;
		pl->hexchat_list_str = hexchat_list_str;
		pl->hexchat_list_next = hexchat_list_next;
		pl->hexchat_list_int = hexchat_list_int;
		pl->hexchat_plugingui_add = hexchat_plugingui_add;
		pl->hexchat_plugingui_remove = hexchat_plugingui_remove;
		pl->hexchat_emit_print = hexchat_emit_print;
#ifdef WIN32
		pl->hexchat_read_fd = reinterpret_cast<int(*) (hexchat_plugin *,void *, char *,int *)>(hexchat_read_fd);
#else
		pl->hexchat_read_fd = hexchat_dummy;
#endif
		pl->hexchat_list_time = hexchat_list_time;
		pl->hexchat_gettext = hexchat_gettext;
		pl->hexchat_send_modes = hexchat_send_modes;
		pl->hexchat_strip = hexchat_strip;
		pl->hexchat_free = hexchat_free;
		pl->hexchat_pluginpref_set_str = hexchat_pluginpref_set_str;
		pl->hexchat_pluginpref_get_str = hexchat_pluginpref_get_str;
		pl->hexchat_pluginpref_set_int = hexchat_pluginpref_set_int;
		pl->hexchat_pluginpref_get_int = hexchat_pluginpref_get_int;
		pl->hexchat_pluginpref_delete = hexchat_pluginpref_delete;
		pl->hexchat_pluginpref_list = hexchat_pluginpref_list;
		pl->hexchat_hook_server_attrs = hexchat_hook_server_attrs;
		pl->hexchat_hook_print_attrs = hexchat_hook_print_attrs;
		pl->hexchat_emit_print_attrs = hexchat_emit_print_attrs;
		pl->hexchat_event_attrs_create = hexchat_event_attrs_create;
		pl->hexchat_event_attrs_free = hexchat_event_attrs_free;

		/* run hexchat_plugin_init, if it returns 0, close the plugin */
		char* name;
		char* desc;
		char* version;
		if ((init_func) (pl, &name, &desc, &version, arg) == 0)
		{
			plugin_free (pl, false, false);
			return;
		}
		pl->name = name;
		pl->desc = desc;
		pl->version = version;
	}

#ifdef USE_PLUGIN
	fe_pluginlist_update ();
#endif
}

/* kill any plugin by the given (file) name (used by /unload) */

int
plugin_kill (char *name, int by_filename)
{
	namespace bfs = boost::filesystem;
	GSList *list;
	
	list = plugin_list;
	while (list)
	{
		auto pl = static_cast<hexchat_plugin_internal*>(list->data);
		/* static-plugins (plugin-timer.c) have a NULL filename */
		if ((by_filename && !pl->filename.empty() && g_ascii_strcasecmp (name, pl->filename.c_str()) == 0) ||
			 (by_filename && !pl->filename.empty() && g_ascii_strcasecmp (name, bfs::path(pl->filename).filename().string().c_str()) == 0) ||
			(!by_filename && g_ascii_strcasecmp (name, pl->name.c_str()) == 0))
		{
			/* statically linked plugins have a NULL filename */
			if (!pl->filename.empty() && !pl->fake)
			{
				if (plugin_free (pl, true, true))
					return 1;
				return 2;
			}
		}
		list = list->next;
	}

	return 0;
}

/* kill all running plugins (at shutdown) */

void
plugin_kill_all (void)
{
	GSList *list, *next;

	list = plugin_list;
	while (list)
	{
		auto pl = static_cast<hexchat_plugin_internal*>(list->data);
		next = list->next;
		if (!pl->fake)
			plugin_free(pl, true, false);
		list = next;
	}
}

#ifdef USE_PLUGIN

/* load a plugin from a filename. Returns: NULL-success or an error string */

const char *
plugin_load (session *sess, const char *filename, char *arg)
{
	namespace fs = boost::filesystem;
	void *handle = nullptr;
	plugin_init_func init_func;
	plugin_deinit_func deinit_func;

	fs::path file_path(filename);
	/* get the filename without path */

	/* load the plugin */
	if (file_path == file_path.filename())
	{
		/* no path specified, it's just the filename, try to load from config dir */
		auto pluginpath = fs::path(get_xdir()) / "addons" / file_path;
		handle = g_module_open(pluginpath.string().c_str(), static_cast<GModuleFlags>(0));
	}
	else
	{
		/* try to load with absolute path */
		handle = g_module_open(filename, static_cast<GModuleFlags>(0));
	}

	if (!handle)
		return g_module_error ();

	/* find the init routine hexchat_plugin_init */
	if (!g_module_symbol (static_cast<GModule*>(handle), "hexchat_plugin_init", (gpointer *)&init_func))
	{
		g_module_close(static_cast<GModule*>(handle));
		return _("No hexchat_plugin_init symbol; is this really a HexChat plugin?");
	}

	/* find the plugin's deinit routine, if any */
	if (!g_module_symbol(static_cast<GModule*>(handle), "hexchat_plugin_deinit", (gpointer *)&deinit_func))
		deinit_func = NULL;

	/* add it to our linked list */
	plugin_add (sess, filename, handle, init_func, deinit_func, arg, false);

	return NULL;
}

static session *ps;

static void
plugin_auto_load_cb (const char *filename)
{
	const char *pMsg;

	pMsg = plugin_load (ps, filename, NULL);
	if (pMsg)
	{
		PrintTextf (ps, "AutoLoad failed for: %s\n", filename);
		PrintText (ps, pMsg);
	}
}

static const char *
plugin_get_libdir ()
{
	const char *libdir;

	libdir = g_getenv ("HEXCHAT_LIBDIR");
	if (libdir && *libdir)
		return libdir;
	else
		return HEXCHATLIBDIR;
}

void
plugin_auto_load (session *sess)
{
	ps = sess;

	const char *lib_dir = plugin_get_libdir();
	auto sub_dir = boost::filesystem::path(get_xdir()) / "addons";

#ifdef WIN32
	/* a long list of bundled plugins that should be loaded automatically,
	 * user plugins should go to <config>, leave Program Files alone! */
	for_files (lib_dir, "hcchecksum.dll", plugin_auto_load_cb);
	for_files (lib_dir, "hcdoat.dll", plugin_auto_load_cb);
	for_files (lib_dir, "hcexec.dll", plugin_auto_load_cb);
	for_files (lib_dir, "hcnotifications.dll", plugin_auto_load_cb);
	for_files (lib_dir, "hcfishlim.dll", plugin_auto_load_cb);
	for_files (lib_dir, "hcmpcinfo.dll", plugin_auto_load_cb);
	for_files (lib_dir, "hcperl.dll", plugin_auto_load_cb);
	for_files (lib_dir, "hcpython2.dll", plugin_auto_load_cb);
	for_files (lib_dir, "hcpython3.dll", plugin_auto_load_cb);
	for_files (lib_dir, "hcupd.dll", plugin_auto_load_cb);
	for_files (lib_dir, "hcwinamp.dll", plugin_auto_load_cb);
	for_files (lib_dir, "hcsysinfo.dll", plugin_auto_load_cb);
#else
	for_files (lib_dir, "*." G_MODULE_SUFFIX, plugin_auto_load_cb);
#endif

	for_files (sub_dir.string().c_str(), "*." G_MODULE_SUFFIX, plugin_auto_load_cb);
}

int plugin_reload (session *sess, const char *name, bool by_filename)
{
	namespace bfs = boost::filesystem;
	for(auto list = plugin_list; list; list = g_slist_next(list))
	{
		auto pl = static_cast<hexchat_plugin_internal*>(list->data);
		/* static-plugins (plugin-timer.c) have a NULL filename */
		if ((by_filename && !pl->filename.empty() && g_ascii_strcasecmp(name, pl->filename.c_str()) == 0) ||
			(by_filename && !pl->filename.empty() && g_ascii_strcasecmp(name, bfs::path(pl->filename).filename().string().c_str()) == 0) ||
			(!by_filename && g_ascii_strcasecmp(name, pl->name.c_str()) == 0))
		{
			
			/* statically linked plugins have a NULL filename */
			if (!pl->filename.empty() && !pl->fake)
			{
				auto filename = pl->filename;
				plugin_free (pl, true, false);
				auto ret = plugin_load (sess, filename.c_str(), NULL);
				if (ret == NULL)
					return 1;
				else
					return 0;
			}
			else
				return 2;
		}
	}

	return 0;
}

#endif

static GSList *
plugin_hook_find (GSList *list, int type, const char *name)
{
	hexchat_hook *hook;

	while (list)
	{
		hook = static_cast<hexchat_hook*>(list->data);
		if (hook && (hook->type & type))
		{
			if (g_ascii_strcasecmp (hook->name, name) == 0)
				return list;

			if ((type & HOOK_SERVER)
				&& g_ascii_strcasecmp (hook->name, "RAW LINE") == 0)
					return list;
		}
		list = list->next;
	}

	return NULL;
}

/* check for plugin hooks and run them */

static int
plugin_hook_run(session *sess, const char *name, const char *const word[], const char *const word_eol[],
				 hexchat_event_attrs *attrs, int type)
{
	GSList *list, *next;
	hexchat_hook *hook;
	int ret, eat = 0;

	list = hook_list;
	while (1)
	{
		list = plugin_hook_find (list, type, name);
		if (!list)
			goto xit;

		hook = static_cast<hexchat_hook*>(list->data);
		next = list->next;
		hook->pl->context = sess;

		/* run the plugin's callback function */
		switch (hook->type)
		{
		case HOOK_COMMAND:
			ret = ((hexchat_cmd_cb *)hook->callback) (word, word_eol, hook->userdata);
			break;
		case HOOK_PRINT_ATTRS:
			ret = ((hexchat_print_attrs_cb *)hook->callback) (word, attrs, hook->userdata);
			break;
		case HOOK_SERVER:
			ret = ((hexchat_serv_cb *)hook->callback) (word, word_eol, hook->userdata);
			break;
		case HOOK_SERVER_ATTRS:
			ret = ((hexchat_serv_attrs_cb *)hook->callback) (word, word_eol, attrs, hook->userdata);
			break;
		default: /*case HOOK_PRINT:*/
			ret = ((hexchat_print_cb *)hook->callback) (word, hook->userdata);
			break;
		}

		if ((ret & HEXCHAT_EAT_HEXCHAT) && (ret & HEXCHAT_EAT_PLUGIN))
		{
			eat = 1;
			goto xit;
		}
		if (ret & HEXCHAT_EAT_PLUGIN)
			goto xit;	/* stop running plugins */
		if (ret & HEXCHAT_EAT_HEXCHAT)
			eat = 1;	/* eventually we'll return 1, but continue running plugins */

		list = next;
	}

xit:
	/* really remove deleted hooks now */
	list = hook_list;
	while (list)
	{
		hook = static_cast<hexchat_hook*>(list->data);
		next = list->next;
		if (!hook || hook->type == HOOK_DELETED)
		{
			hook_list = g_slist_remove (hook_list, hook);
			delete hook;
		}
		list = next;
	}

	return eat;
}

/* execute a plugged in command. Called from outbound.c */

int
plugin_emit_command (session *sess, char *name, char *word[], char *word_eol[])
{
	return plugin_hook_run (sess, name, word, word_eol, NULL, HOOK_COMMAND);
}

hexchat_event_attrs *
hexchat_event_attrs_create (hexchat_plugin *ph)
{
	hexchat_event_attrs *attrs;

	attrs = static_cast<hexchat_event_attrs *>(g_malloc(sizeof(*attrs)));

	attrs->server_time_utc = (time_t) 0;

	return attrs;
}

void
hexchat_event_attrs_free (hexchat_plugin *ph, hexchat_event_attrs *attrs)
{
	g_free (attrs);
}

/* got a server PRIVMSG, NOTICE, numeric etc... */

int
plugin_emit_server (session *sess, char *name, char *word[], char *word_eol[],
					time_t server_time)
{
	hexchat_event_attrs attrs;

	attrs.server_time_utc = server_time;

	return plugin_hook_run (sess, name, word, word_eol, &attrs, 
							HOOK_SERVER | HOOK_SERVER_ATTRS);
}

/* see if any plugins are interested in this print event */

int
plugin_emit_print(session *sess, const char *const word[], time_t server_time)
{
	hexchat_event_attrs attrs;

	attrs.server_time_utc = server_time;

	return plugin_hook_run (sess, word[0], word, NULL, &attrs,
							HOOK_PRINT | HOOK_PRINT_ATTRS);
}

int
plugin_emit_dummy_print (session *sess, char *name)
{
	char *word[32];
	int i;

	word[0] = name;
	for (i = 1; i < 32; i++)
		word[i] = "\000";

	return plugin_hook_run (sess, name, word, NULL, NULL, HOOK_PRINT);
}

int
plugin_emit_keypress (session *sess, unsigned int state, unsigned int keyval,
							 int len, char *string)
{
	char *word[PDIWORDS];
	char keyval_str[16];
	char state_str[16];
	char len_str[16];
	int i;

	if (!hook_list)
		return 0;

	sprintf (keyval_str, "%u", keyval);
	sprintf (state_str, "%u", state);
	sprintf (len_str, "%d", len);

	word[0] = "Key Press";
	word[1] = keyval_str;
	word[2] = state_str;
	word[3] = string;
	word[4] = len_str;
	for (i = 5; i < PDIWORDS; i++)
		word[i] = "\000";

	return plugin_hook_run (sess, word[0], word, NULL, NULL, HOOK_PRINT);
}

static int
plugin_timeout_cb (hexchat_hook *hook)
{
	int ret;

	/* timer_cb's context starts as front-most-tab */
	hook->pl->context = current_sess;

	/* call the plugin's timeout function */
	ret = ((hexchat_timer_cb *)hook->callback) (hook->userdata);

	/* the callback might have already unhooked it! */
	if (!g_slist_find (hook_list, hook) || hook->type == HOOK_DELETED)
		return 0;

	if (ret == 0)
	{
		hook->tag = 0;	/* avoid fe_timeout_remove, returning 0 is enough! */
		hexchat_unhook (hook->pl, hook);
	}

	return ret;
}

/* insert a hook into hook_list according to its priority */

static void
plugin_insert_hook (hexchat_hook *new_hook)
{
	GSList *list;
	hexchat_hook *hook;
	int new_hook_type;
 
	switch (new_hook->type)
	{
		case HOOK_PRINT:
		case HOOK_PRINT_ATTRS:
			new_hook_type = HOOK_PRINT | HOOK_PRINT_ATTRS;
			break;
		case HOOK_SERVER:
		case HOOK_SERVER_ATTRS:
			new_hook_type = HOOK_SERVER | HOOK_PRINT_ATTRS;
			break;
		default:
			new_hook_type = new_hook->type;
	}

	list = hook_list;
	while (list)
	{
		hook = static_cast<hexchat_hook*>(list->data);
		if (hook && (hook->type & new_hook_type) && hook->pri <= new_hook->pri)
		{
			hook_list = g_slist_insert_before (hook_list, list, new_hook);
			return;
		}
		list = list->next;
	}

	hook_list = g_slist_append (hook_list, new_hook);
}

static gboolean
plugin_fd_cb (GIOChannel *source, GIOCondition condition, hexchat_hook *hook)
{
	int flags = 0, ret;
	typedef int (hexchat_fd_cb2) (int fd, int flags, void *user_data, GIOChannel *);

	if (condition & G_IO_IN)
		flags |= HEXCHAT_FD_READ;
	if (condition & G_IO_OUT)
		flags |= HEXCHAT_FD_WRITE;
	if (condition & G_IO_PRI)
		flags |= HEXCHAT_FD_EXCEPTION;

	ret = ((hexchat_fd_cb2 *)hook->callback) (hook->pri, flags, hook->userdata, source);

	/* the callback might have already unhooked it! */
	if (!g_slist_find (hook_list, hook) || hook->type == HOOK_DELETED)
		return 0;

	if (ret == 0)
	{
		hook->tag = 0; /* avoid fe_input_remove, returning 0 is enough! */
		hexchat_unhook (hook->pl, hook);
	}

	return ret;
}

/* allocate and add a hook to our list. Used for all 4 types */

static hexchat_hook *
plugin_add_hook (hexchat_plugin_internal *pl, int type, int pri, const char *name,
					  const  char *help_text, void (*callb)(void*), int timeout, void *userdata)
{
	hexchat_hook *hook;

	hook = new hexchat_hook();

	hook->type = type;
	hook->pri = pri;
	if (name)
		hook->name = new_strdup (name);
	if (help_text)
		hook->help_text = new_strdup (help_text);
	hook->callback = callb;
	hook->pl = pl;
	hook->userdata = userdata;

	/* insert it into the linked list */
	plugin_insert_hook (hook);

	if (type == HOOK_TIMER)
		hook->tag = fe_timeout_add(timeout, (GSourceFunc)plugin_timeout_cb, hook);

	return hook;
}

GList *
plugin_command_list(GList *tmp_list)
{
	hexchat_hook *hook;
	GSList *list = hook_list;

	while (list)
	{
		hook = static_cast<hexchat_hook*>(list->data);
		if (hook && hook->type == HOOK_COMMAND)
			tmp_list = g_list_prepend(tmp_list, hook->name);
		list = list->next;
	}
	return tmp_list;
}

void
plugin_command_foreach (session *sess, void *userdata,
			void (*cb) (session *sess, void *userdata, char *name, char *help))
{
	GSList *list;
	hexchat_hook *hook;

	list = hook_list;
	while (list)
	{
		hook = static_cast<hexchat_hook*>(list->data);
		if (hook && hook->type == HOOK_COMMAND && hook->name[0])
		{
			cb (sess, userdata, hook->name, hook->help_text);
		}
		list = list->next;
	}
}

int
plugin_show_help (session *sess, const char *cmd)
{
	GSList *list;
	hexchat_hook *hook;

	list = plugin_hook_find (hook_list, HOOK_COMMAND, cmd);
	if (list)
	{
		hook = static_cast<hexchat_hook*>(list->data);
		if (hook->help_text)
		{
			PrintText (sess, hook->help_text);
			return 1;
		}
	}

	return 0;
}

/* ========================================================= */
/* ===== these are the functions plugins actually call ===== */
/* ========================================================= */

void *
hexchat_unhook (hexchat_plugin *ph, hexchat_hook *hook)
{
	/* perl.c trips this */
	if (!g_slist_find (hook_list, hook) || hook->type == HOOK_DELETED)
		return NULL;

	if (hook->type == HOOK_TIMER && hook->tag != 0)
		fe_timeout_remove (hook->tag);

	if (hook->type == HOOK_FD && hook->tag != 0)
		fe_input_remove (hook->tag);

	hook->type = HOOK_DELETED;	/* expunge later */

	delete[] hook->name;	/* NULL for timers & fds */
	delete[] hook->help_text;	/* NULL for non-commands */

	return hook->userdata;
}

hexchat_hook *
hexchat_hook_command (hexchat_plugin *ph, const char *name, int pri,
						  hexchat_cmd_cb *callb, const char *help_text, void *userdata)
{
	return plugin_add_hook(static_cast<hexchat_plugin_internal*>(ph), HOOK_COMMAND, pri, name, help_text, (void(*)(void*))callb, 0,
									userdata);
}

hexchat_hook *
hexchat_hook_server (hexchat_plugin *ph, const char *name, int pri,
						 hexchat_serv_cb *callb, void *userdata)
{
	return plugin_add_hook(static_cast<hexchat_plugin_internal*>(ph), HOOK_SERVER, pri, name, 0, (void(*)(void*))callb, 0, userdata);
}

hexchat_hook *
hexchat_hook_server_attrs (hexchat_plugin *ph, const char *name, int pri,
						   hexchat_serv_attrs_cb *callb, void *userdata)
{
	return plugin_add_hook(static_cast<hexchat_plugin_internal*>(ph), HOOK_SERVER_ATTRS, pri, name, 0, (void(*)(void*))callb, 0,
							userdata);
}

hexchat_hook *
hexchat_hook_print (hexchat_plugin *ph, const char *name, int pri,
						hexchat_print_cb *callb, void *userdata)
{
	return plugin_add_hook(static_cast<hexchat_plugin_internal*>(ph), HOOK_PRINT, pri, name, 0, (void(*)(void*))callb, 0, userdata);
}

hexchat_hook *
hexchat_hook_print_attrs (hexchat_plugin *ph, const char *name, int pri,
						  hexchat_print_attrs_cb *callb, void *userdata)
{
	return plugin_add_hook(static_cast<hexchat_plugin_internal*>(ph), HOOK_PRINT_ATTRS, pri, name, 0, (void(*)(void*))callb, 0,
							userdata);
}

hexchat_hook *
hexchat_hook_timer (hexchat_plugin *ph, int timeout, hexchat_timer_cb *callb,
					   void *userdata)
{
	return plugin_add_hook(static_cast<hexchat_plugin_internal*>(ph), HOOK_TIMER, 0, 0, 0, (void(*)(void*))callb, timeout, userdata);
}

hexchat_hook *
hexchat_hook_fd (hexchat_plugin *ph, int fd, int flags,
					hexchat_fd_cb *callb, void *userdata)
{
	hexchat_hook *hook;

	hook = plugin_add_hook(static_cast<hexchat_plugin_internal*>(ph), HOOK_FD, 0, 0, 0, (void(*)(void*))callb, 0, userdata);
	if (!hook)
		return NULL;
	hook->pri = fd;
	/* plugin hook_fd flags correspond exactly to FIA_* flags (fe.hpp) */
	hook->tag = fe_input_add(fd, flags, (GIOFunc)plugin_fd_cb, hook);

	return hook;
}

void
hexchat_print (hexchat_plugin *ph, const char *text)
{
	hexchat_plugin_internal *pi = static_cast<hexchat_plugin_internal*>(ph);
	if (!is_session (pi->context))
	{
		DEBUG(PrintTextf(0, "%s\thexchat_print called without a valid context.\n", pi->name.c_str()));
		return;
	}

	PrintText (pi->context, text);
}

void
hexchat_printf (hexchat_plugin *ph, const char *format, ...)
{
	va_list args;

	va_start (args, format);
	glib_string buf(g_strdup_vprintf (format, args));
	va_end (args);

	hexchat_print (ph, buf.get());
}

void
hexchat_command (hexchat_plugin *ph, const char *command)
{
	hexchat_plugin_internal * pi = static_cast<hexchat_plugin_internal*>(ph);
	if (!g_utf8_validate(command, -1, 0))
	{
		PrintTextf(nullptr, boost::format(_("Plugin %s sent in a non UTF-8 string this has been ignored to prevent a crash\n")) % pi->name);
		return;
	}
	
	if (!is_session (pi->context))
	{
		DEBUG(PrintTextf(0, "%s\thexchat_command called without a valid context.\n", pi->name.c_str()));
		return;
	}
	/* scripts/plugins continue to send non-UTF8... *sigh* */
	
	glib_string mutable_command(g_strdup(command));
	handle_command(pi->context, mutable_command.get(), FALSE);
}

void
hexchat_commandf (hexchat_plugin *ph, const char *format, ...)
{
	va_list args;

	va_start (args, format);
	glib_string buf(g_strdup_vprintf (format, args));
	va_end (args);

	hexchat_command (ph, buf.get());
}

int
hexchat_nickcmp (hexchat_plugin *ph, const char *s1, const char *s2)
{
	return static_cast<hexchat_plugin_internal*>(ph)->context->server->p_cmp(s1, s2);
}

hexchat_context *
hexchat_get_context (hexchat_plugin *ph)
{
	return static_cast<hexchat_plugin_internal*>(ph)->context;
}

int
hexchat_set_context (hexchat_plugin *ph, hexchat_context *context)
{
	if (is_session (context))
	{
		static_cast<hexchat_plugin_internal*>(ph)->context = context;
		return 1;
	}
	return 0;
}

hexchat_context *
hexchat_find_context (hexchat_plugin *ph, const char *servname, const char *channel)
{
	if (servname == NULL && channel == NULL)
		return current_sess;

	auto pi = static_cast<hexchat_plugin_internal*>(ph);
	GSList *sessions = NULL;
	for(auto slist = serv_list; slist; slist = g_slist_next(slist))
	{
		auto serv = static_cast<server*>(slist->data);
		auto netname = serv->get_network (true);

		if (servname == NULL ||
			 rfc_casecmp (servname, serv->servername) == 0 ||
			 g_ascii_strcasecmp (servname, serv->hostname) == 0 ||
			 g_ascii_strcasecmp (servname, netname.data()) == 0)
		{
			if (channel == NULL)
				return serv->front_session;

			for(auto clist = sess_list; clist; clist = g_slist_next(clist))
			{
				auto sess = static_cast<session*>(clist->data);
				if (sess->server == serv)
				{
					if (rfc_casecmp (channel, sess->channel) == 0)
					{
						if (sess->server == pi->context->server)
						{
							g_slist_free (sessions);
							return sess;
						} else
						{
							sessions = g_slist_prepend (sessions, sess);
						}
					}
				}
			}
		}
	}

	if (sessions)
	{
		sessions = g_slist_reverse (sessions);
		auto sess = static_cast<session*>(sessions->data);
		g_slist_free (sessions);
		return sess;
	}

	return NULL;
}

const char *
hexchat_get_info (hexchat_plugin *ph, const char *id)
{
	/*                 1234567890 */
	if (!strncmp (id, "event_text", 10))
	{
		char *e = (char *)id + 10;
		if (*e == ' ') e++;	/* 2.8.0 only worked without a space */
		return text_find_format_string (e);
	}

	auto hash = str_hash (id);
	/* do the session independant ones first */
	switch (hash)
	{
		case 0x325acab5:	/* libdirfs */
#ifdef USE_PLUGIN
			return plugin_get_libdir ();
#else
			return NULL;
#endif

		case 0x14f51cd8: /* version */
			return PACKAGE_VERSION;

		case 0xdd9b1abd:	/* xchatdir */
		case 0xe33f6c4a:	/* xchatdirfs */
		case 0xd00d220b:	/* configdir */
			return get_xdir ();
	}
	auto pi = static_cast<hexchat_plugin_internal*>(ph);
	auto sess = pi->context;
	if (!is_session (sess))
	{
		DEBUG(PrintTextf(0, "%s\thexchat_get_info called without a valid context.\n", pi->name.c_str()));
		return NULL;
	}

	switch (hash)
	{
	case 0x2de2ee: /* away */
		if (sess->server->is_away)
			return sess->server->last_away_reason.c_str();
		return NULL;

	case 0x2c0b7d03: /* channel */
		return sess->channel;

	case 0x2c0d614c: /* charset */
		{
			if (sess->server->encoding)
				return sess->server->encoding->c_str();

			const char* locale = NULL;
			g_get_charset (&locale);
			return locale;
		}

	case 0x30f5a8: /* host */
		return sess->server->hostname;

	case 0x1c0e99c1: /* inputbox */
		return fe_get_inputbox_contents (sess);

	case 0x633fb30:	/* modes */
		return sess->current_modes.c_str();

	case 0x6de15a2e:	/* network */
		return sess->server->get_network(false).data();

	case 0x339763: /* nick */
		return sess->server->nick;

	case 0x4889ba9b: /* password */
	case 0x438fdf9: /* nickserv */
		if (sess->server->network)
			return sess->server->network->pass;
		return NULL;

	case 0xca022f43: /* server */
		if (!sess->server->connected)
			return NULL;
		return sess->server->servername;

	case 0x696cd2f: /* topic */
		return sess->topic.c_str();

	case 0x3419f12d: /* gtkwin_ptr */
		return (char*)fe_gui_info_ptr (sess, 1);

	case 0x506d600b: /* native win_ptr */
		return (char*)fe_gui_info_ptr (sess, 0);

	case 0x6d3431b5: /* win_status */
		switch (fe_gui_info (sess, 0))	/* check window status */
		{
		case 0: return "normal";
		case 1: return "active";
		case 2: return "hidden";
		}
		return NULL;
	}

	return NULL;
}

int
hexchat_get_prefs (hexchat_plugin *ph, const char *name, const char **string, int *integer)
{
	int i = 0;
	auto pi = static_cast<hexchat_plugin_internal*>(ph);
	/* some special run-time info (not really prefs, but may aswell throw it in here) */
	switch (str_hash (name))
	{
		case 0xf82136c4: /* state_cursor */
			*integer = fe_get_inputbox_cursor (pi->context);
			return 2;

		case 0xd1b: /* id */
			*integer = pi->context->server->id;
			return 2;
	}
	
	do
	{
		if (!g_ascii_strcasecmp (name, vars[i].name))
		{
			switch (vars[i].type)
			{
			case TYPE_STR:
				*string = ((char *) &prefs + vars[i].offset);
				return 1;

			case TYPE_INT:
				*integer = *((int *) &prefs + vars[i].offset);
				return 2;

			default:
			/*case TYPE_BOOL:*/
				if (*((int *) &prefs + vars[i].offset))
					*integer = 1;
				else
					*integer = 0;
				return 3;
			}
		}
		i++;
	}
	while (vars[i].name);

	return 0;
}

hexchat_list *
hexchat_list_get (hexchat_plugin *ph, const char *name)
{
	hexchat_list *list;

	list = new hexchat_list();
	list->pos = NULL;
	auto pi = static_cast<hexchat_plugin_internal*>(ph);
	switch (str_hash (name))
	{
	case 0x556423d0: /* channels */
		list->type = LIST_CHANNELS;
		list->next = sess_list;
		break;

	case 0x183c4:	/* dcc */
		list->type = LIST_DCC;
		list->next = dcc_list;
		break;

	case 0xb90bfdd2:	/* ignore */
		list->type = LIST_IGNORE;
		list->is_vector = true;
		list->loc = 0;
		list->length = get_ignore_list().size();
		break;

	case 0xc2079749:	/* notify */
		list->type = LIST_NOTIFY;
		list->next = notify_list;
		list->head = reinterpret_cast<GSList*>(pi->context);	/* reuse this pointer */
		break;

	case 0x6a68e08: /* users */
		if (is_session (pi->context))
		{
			list->type = LIST_USERS;
			list->head = list->next = userlist_flat_list (pi->context);
			fe_userlist_set_selected (pi->context);
			break;
		}	/* fall through */

	default:
		delete list;
		return NULL;
	}

	return list;
}

void
hexchat_list_free (hexchat_plugin *ph, hexchat_list *xlist)
{
	if (xlist->type == LIST_USERS)
		g_slist_free (xlist->head);
	delete xlist;
}

int
hexchat_list_next (hexchat_plugin *ph, hexchat_list *xlist)
{
	if (xlist->is_vector)
	{
		if (xlist->loc < xlist->length)
		{
			xlist->loc++;
			return 1;
		}
		else
		{
			return 0;
		}
	}
	if (xlist->next == NULL)
		return 0;

	xlist->pos = xlist->next;
	xlist->next = xlist->pos->next;

	/* NOTIFY LIST: Find the entry which matches the context
		of the plugin when list_get was originally called. */
	if (xlist->type == LIST_NOTIFY)
	{
		xlist->notifyps = notify_find_server_entry (static_cast<notify*>(xlist->pos->data),
													*((session *)xlist->head)->server);
		if (!xlist->notifyps)
			return 0;
	}

	return 1;
}

const char * const *
hexchat_list_fields (hexchat_plugin *ph, const char *name)
{
	static const char * const dcc_fields[] =
	{
		"iaddress32","icps",		"sdestfile","sfile",		"snick",	"iport",
		"ipos", "iposhigh", "iresume", "iresumehigh", "isize", "isizehigh", "istatus", "itype", NULL
	};
	static const char * const channels_fields[] =
	{
		"schannel",	"schannelkey", "schantypes", "pcontext", "iflags", "iid", "ilag", "imaxmodes",
		"snetwork", "snickmodes", "snickprefixes", "iqueue", "sserver", "itype", "iusers",
		NULL
	};
	static const char * const ignore_fields[] =
	{
		"iflags", "smask", NULL
	};
	static const char * const notify_fields[] =
	{
		"iflags", "snetworks", "snick", "toff", "ton", "tseen", NULL
	};
	static const char * const users_fields[] =
	{
		"saccount", "iaway", "shost", "tlasttalk", "snick", "sprefix", "srealname", "iselected", NULL
	};
	static const char * const list_of_lists[] =
	{
		"channels",	"dcc", "ignore", "notify", "users", NULL
	};

	switch (str_hash (name))
	{
	case 0x556423d0:	/* channels */
		return channels_fields;
	case 0x183c4:		/* dcc */
		return dcc_fields;
	case 0xb90bfdd2:	/* ignore */
		return ignore_fields;
	case 0xc2079749:	/* notify */
		return notify_fields;
	case 0x6a68e08:	/* users */
		return users_fields;
	case 0x6236395:	/* lists */
		return list_of_lists;
	}

	return NULL;
}

time_t
hexchat_list_time (hexchat_plugin *ph, hexchat_list *xlist, const char *name)
{
	guint32 hash = str_hash (name);
	gpointer data;

	switch (xlist->type)
	{
	case LIST_NOTIFY:
		if (!xlist->notifyps)
			return (time_t) -1;
		switch (hash)
		{
		case 0x1ad6f:	/* off */
			return xlist->notifyps->lastoff;
		case 0xddf:	/* on */
			return xlist->notifyps->laston;
		case 0x35ce7b:	/* seen */
			return xlist->notifyps->lastseen;
		}
		break;

	case LIST_USERS:
		data = xlist->pos->data;
		switch (hash)
		{
		case 0xa9118c42:	/* lasttalk */
			return ((struct User *)data)->lasttalk;
		}
	}

	return (time_t) -1;
}

const char *
hexchat_list_str (hexchat_plugin *ph, hexchat_list *xlist, const char *name)
{
	guint32 hash = str_hash (name);
	gpointer data = static_cast<hexchat_plugin_internal*>(ph)->context;
	int type = LIST_CHANNELS;

	/* a NULL xlist is a shortcut to current "channels" context */
	if (xlist)
	{
		data = xlist->pos->data;
		type = xlist->type;
	}

	switch (type)
	{
	case LIST_CHANNELS:
		switch (hash)
		{
		case 0x2c0b7d03: /* channel */
			return ((session *)data)->channel;
		case 0x8cea5e7c: /* channelkey */
			return ((session *)data)->channelkey;
		case 0x577e0867: /* chantypes */
			return ((session *)data)->server->chantypes.c_str();
		case 0x38b735af: /* context */
			return static_cast<const char*>(data);	/* this is a session * */
		case 0x6de15a2e: /* network */
			return ((session *)data)->server->get_network(false).data();
		case 0x8455e723: /* nickprefixes */
			return ((session *)data)->server->nick_prefixes.c_str();
		case 0x829689ad: /* nickmodes */
			return ((session *)data)->server->nick_modes.c_str();
		case 0xca022f43: /* server */
			return ((session *)data)->server->servername;
		}
		break;

	case LIST_DCC:
		switch (hash)
		{
		case 0x3d9ad31e:	/* destfile */
			return ((dcc::DCC *)data)->destfile;
		case 0x2ff57c:	/* file */
			return ((dcc::DCC *)data)->file;
		case 0x339763: /* nick */
			return ((dcc::DCC *)data)->nick;
		}
		break;

	case LIST_IGNORE:
		switch (hash)
		{
		case 0x3306ec:	/* mask */
			return get_ignore_list()[xlist->loc].mask.c_str();
		}
		break;

	case LIST_NOTIFY:
		switch (hash)
		{
		case 0x4e49ec05:	/* networks */
			return boost::join(((struct notify *)data)->networks, ",").c_str();
		case 0x339763: /* nick */
			return ((struct notify *)data)->name.c_str();
		}
		break;

	case LIST_USERS:
		switch (hash)
		{
		case 0xb9d38a2d: /* account */
			return ((struct User *)data)->account ? ((struct User *)data)->account->c_str() : nullptr;
		case 0x339763: /* nick */
			return ((struct User *)data)->nick.c_str();
		case 0x30f5a8: /* host */
			return ((struct User *)data)->hostname ? ((struct User *)data)->hostname->c_str() : nullptr;
		case 0xc594b292: /* prefix */
			return ((struct User *)data)->prefix;
		case 0xccc6d529: /* realname */
			return ((struct User *)data)->realname ? ((struct User *)data)->realname->c_str() : nullptr;
		}
		break;
	}

	return NULL;
}

int
hexchat_list_int (hexchat_plugin *ph, hexchat_list *xlist, const char *name)
{
	guint32 hash = str_hash (name);
	gpointer data = static_cast<hexchat_plugin_internal*>(ph)->context;
	int tmp = 0;
	int type = LIST_CHANNELS;

	/* a NULL xlist is a shortcut to current "channels" context */
	if (xlist)
	{
		data = xlist->pos->data;
		type = xlist->type;
	}

	switch (type)
	{
	case LIST_DCC:
		switch (hash)
		{
		case 0x34207553: /* address32 */
			return ((dcc::DCC *)data)->addr;
		case 0x181a6: /* cps */
			return ((dcc::DCC *)data)->cps;
		case 0x349881: /* port */
			return ((dcc::DCC *)data)->port;
		case 0x1b254: /* pos */
			return ((dcc::DCC *)data)->pos & 0xffffffff;
		case 0xe8a945f6: /* poshigh */
			return (((dcc::DCC *)data)->pos >> 32) & 0xffffffff;
		case 0xc84dc82d: /* resume */
			return ((dcc::DCC *)data)->resumable & 0xffffffff;
		case 0xded4c74f: /* resumehigh */
			return (((dcc::DCC *)data)->resumable >> 32) & 0xffffffff;
		case 0x35e001: /* size */
			return ((dcc::DCC *)data)->size & 0xffffffff;
		case 0x3284d523: /* sizehigh */
			return (((dcc::DCC *)data)->size >> 32) & 0xffffffff;
		case 0xcacdcff2: /* status */
			return ((dcc::DCC *)data)->dccstat;
		case 0x368f3a: /* type */
			return static_cast<int>(((dcc::DCC *)data)->type);
		}
		break;

	case LIST_IGNORE:
		switch (hash)
		{
		case 0x5cfee87:	/* flags */
			return get_ignore_list()[xlist->loc].type;
		}
		break;

	case LIST_CHANNELS:
		switch (hash)
		{
		case 0xd1b:	/* id */
			return ((struct session *)data)->server->id;
		case 0x5cfee87:	/* flags */
			/* used if text_strip is unset */                    /* 16 */
			tmp <<= 1;
			tmp |= ((struct session *)data)->text_strip;          /* 15 */
			tmp <<= 1;
			/* used if text_scrollback is unset */               /* 14 */
			tmp <<= 1;
			tmp |= ((struct session *)data)->text_scrollback;    /* 13 */
			tmp <<= 1;
			/* used if text_logging is unset */                  /* 12 */
			tmp <<= 1;
			tmp |= ((struct session *)data)->text_logging;       /* 11 */
			tmp <<= 1;
			tmp |= ((struct session *)data)->alert_taskbar;      /* 10 */
			tmp <<= 1;
			tmp |= ((struct session *)data)->alert_tray;         /* 9 */
			tmp <<= 1;
			tmp |= ((struct session *)data)->alert_beep;         /* 8 */
			tmp <<= 1;
			/* used if text_hidejoinpart is unset */              /* 7 */
			tmp <<= 1;
			tmp |= ((struct session *)data)->text_hidejoinpart;   /* 6 */
			tmp <<= 1;
			tmp |= ((struct session *)data)->server->have_idmsg ? 1 : 0; /* 5 */
			tmp <<= 1;
			tmp |= ((struct session *)data)->server->have_whox ? 1 : 0;  /* 4 */
			tmp <<= 1;
			tmp |= ((struct session *)data)->server->end_of_motd ? 1 : 0;/* 3 */
			tmp <<= 1;
			tmp |= ((struct session *)data)->server->is_away ? 1 : 0;    /* 2 */
			tmp <<= 1;
			tmp |= ((struct session *)data)->server->connecting ? 1 : 0; /* 1 */
			tmp <<= 1;
			tmp |= ((struct session *)data)->server->connected ? 1 : 0;  /* 0 */
			return tmp;
		case 0x1a192: /* lag */
			return ((struct session *)data)->server->lag;
		case 0x1916144c: /* maxmodes */
			return ((struct session *)data)->server->modes_per_line;
		case 0x66f1911: /* queue */
			return ((struct session *)data)->server->sendq_len;
		case 0x368f3a:	/* type */
			return ((struct session *)data)->type;
		case 0x6a68e08: /* users */
			return ((struct session *)data)->total;
		}
		break;

	case LIST_NOTIFY:
		if (!xlist->notifyps)
			return -1;
		switch (hash)
		{
		case 0x5cfee87: /* flags */
			return xlist->notifyps->ison;
		}

	case LIST_USERS:
		switch (hash)
		{
		case 0x2de2ee:	/* away */
			return ((struct User *)data)->away;
		case 0x4705f29b: /* selected */
			return ((struct User *)data)->selected;
		}
		break;

	}

	return -1;
}

void *
hexchat_plugingui_add (hexchat_plugin *ph, const char *filename,
							const char *name, const char *desc,
							const char *version, char *reserved)
{
#ifdef USE_PLUGIN
	ph = plugin_list_add (NULL, filename, name, desc,
								 new_strdup (version), NULL, NULL, true, true);
	fe_pluginlist_update ();
#endif

	return ph;
}

void
hexchat_plugingui_remove (hexchat_plugin *, void *handle)
{
#ifdef USE_PLUGIN
	plugin_free (static_cast<hexchat_plugin_internal*>(handle), false, false);
#endif
}

int
hexchat_emit_print (hexchat_plugin *ph, const char *event_name, ...)
{
	va_list args;
	/* currently only 4 because no events use more than 4.
		This can be easily expanded without breaking the API. */
	char *argv[4] = {NULL, NULL, NULL, NULL};
	int i = 0;

	va_start (args, event_name);
	while (1)
	{
		argv[i] = va_arg (args, char *);
		if (!argv[i])
			break;
		i++;
		if (i >= 4)
			break;
	}

	i = text_emit_by_name((char *)event_name, static_cast<hexchat_plugin_internal*>(ph)->context, (time_t)0,
						   argv[0], argv[1], argv[2], argv[3]);
	va_end (args);

	return i;
}

int
hexchat_emit_print_attrs (hexchat_plugin *ph, hexchat_event_attrs *attrs,
						  const char *event_name, ...)
{
	va_list args;
	/* currently only 4 because no events use more than 4.
		This can be easily expanded without breaking the API. */
	char *argv[4] = {NULL, NULL, NULL, NULL};
	int i = 0;

	va_start (args, event_name);
	while (1)
	{
		argv[i] = va_arg (args, char *);
		if (!argv[i])
			break;
		i++;
		if (i >= 4)
			break;
	}

	i = text_emit_by_name((char *)event_name, static_cast<hexchat_plugin_internal*>(ph)->context, attrs->server_time_utc,
						   argv[0], argv[1], argv[2], argv[3]);
	va_end (args);

	return i;
}

char *
hexchat_gettext (hexchat_plugin *ph, const char *msgid)
{
	/* so that plugins can use HexChat's internal gettext strings. */
	/* e.g. The EXEC plugin uses this on Windows. */
	return _(msgid);
}

void
hexchat_send_modes (hexchat_plugin *ph, const char **targets, int ntargets, int modes_per_line, char sign, char mode)
{
	auto targets_v = to_vector_strings(targets, ntargets);
	send_channel_modes (static_cast<hexchat_plugin_internal*>(ph)->context, targets_v, 0, ntargets, sign, mode, modes_per_line);
}

char *
hexchat_strip (hexchat_plugin *ph, const char *str, int len, int flags)
{
	return g_strdup(strip_color (str, static_cast<strip_flags>(flags)).c_str());
}

void
hexchat_free (hexchat_plugin *ph, void *ptr)
{
	g_free (ptr);
}

namespace {
	enum class set_mode
	{
		del,
		save
	};
}
static int
hexchat_pluginpref_set_str_real(hexchat_plugin *pl, const char *var, const char *value, set_mode mode) /* mode: 0 = delete, 1 = save */
{
	namespace bfs = boost::filesystem;
	glib_string canon(g_strdup(static_cast<hexchat_plugin_internal*>(pl)->name.c_str()));
	canonalize_key (canon.get());
	glib_string confname(g_strdup_printf ("addon_%s.conf", canon.get()));
	glib_string confname_tmp(g_strdup_printf ("%s.new", confname.get()));
	auto fhOut = hexchat_open_file (confname_tmp.get(), O_TRUNC | O_WRONLY | O_CREAT, 0600, XOF_DOMODE);
	
	if (fhOut == -1)		/* unable to save, abort */
	{
		return FALSE;
	}
	auto fpIn = hexchat_fopen_file(confname.get(), "r", 0);
	auto config_dir = bfs::path(config::config_dir());
	auto confpath = config_dir / confname.get();
	auto confoldpath = config_dir / confname_tmp.get();
	if (fpIn == NULL)	/* no previous config file, no parsing */
	{
		if (mode == set_mode::save)
		{
			glib_string escaped_value(g_strescape (value, NULL));
			glib_string buffer(g_strdup_printf ("%s = %s\n", var, escaped_value.get()));
			write (fhOut, buffer.get(), strlen (buffer.get()));
			close (fhOut);

#ifdef WIN32
			g_unlink (confpath.string().c_str());
#endif
			boost::system::error_code ec;
			bfs::rename(confoldpath, confpath, ec);
			return !ec ? TRUE : FALSE;
		}
		else
		{
			/* mode = 0, we want to delete but the config file and thus the given setting does not exist, we're ready */
			close (fhOut);
			return TRUE;
		}
	}
	else	/* existing config file, preserve settings and find & replace current var value if any */
	{
		bool prevSetting = false;
		char line_buffer[512] = { 0 };		/* the same as in cfg_put_str */
		char *line_bufp = line_buffer;
		auto var_len = std::strlen(var);

		while (fscanf (fpIn, " %[^\n]", line_bufp) != EOF)	/* read whole lines including whitespaces */
		{
			glib_string buffer_tmp(g_strdup_printf ("%s ", var));	/* add one space, this way it works against var - var2 checks too */
			glib_string buffer;
			if (strncmp (buffer_tmp.get(), line_buffer, var_len + 1) == 0)	/* given setting already exists */
			{
				if (mode == set_mode::save)									/* overwrite the existing matching setting if we are in save mode */
				{
					glib_string escaped_value(g_strescape (value, NULL));
					buffer.reset(g_strdup_printf ("%s = %s\n", var, escaped_value.get()));
				}
				else										/* erase the setting in delete mode */
				{
					buffer.reset(g_strdup (""));
				}

				prevSetting = true;
			}
			else
			{
				buffer.reset(g_strdup_printf ("%s\n", line_buffer));	/* preserve the existing different settings */
			}

			write (fhOut, buffer.get(), strlen (buffer.get()));
		}

		fclose (fpIn);

		if (!prevSetting && mode == set_mode::save)	/* var doesn't exist currently, append if we're in save mode */
		{
			glib_string escaped_value(g_strescape (value, NULL));
			glib_string buffer(g_strdup_printf ("%s = %s\n", var, escaped_value.get()));
			write (fhOut, buffer.get(), strlen (buffer.get()));
		}

		close (fhOut);

#ifdef WIN32
		g_unlink (confpath.string().c_str());
#endif

		boost::system::error_code ec;
		bfs::rename(confoldpath, confpath, ec);

		return !ec ? TRUE : FALSE;
	}
}

int
hexchat_pluginpref_set_str (hexchat_plugin *pl, const char *var, const char *value)
{
	return hexchat_pluginpref_set_str_real (static_cast<hexchat_plugin_internal*>(pl), var, value, set_mode::save);
}

static int
hexchat_pluginpref_get_str_real (hexchat_plugin_internal *pl, const char *var, char *dest, int dest_len)
{
	glib_string canon(g_strdup (pl->name.c_str()));
	canonalize_key (canon.get());
	glib_string confname( g_strdup_printf ("%s%caddon_%s.conf", get_xdir(), G_DIR_SEPARATOR, canon.get()));

	char *cfg;
	if (!g_file_get_contents (confname.get(), &cfg, NULL, NULL))
	{
		return FALSE;
	}
	glib_string cfg_ptr(cfg);

	char buf[512];
	if (!cfg_get_str (cfg, var, buf, sizeof(buf)))
	{
		return FALSE;
	}

	glib_string unescaped_value(g_strcompress (buf));
	g_strlcpy (dest, unescaped_value.get(), dest_len);

	return TRUE;
}

int
hexchat_pluginpref_get_str (hexchat_plugin *pl, const char *var, char *dest)
{
	/* All users of this must ensure dest is >= 512... */
	return hexchat_pluginpref_get_str_real(static_cast<hexchat_plugin_internal*>(pl), var, dest, 512);
}

int
hexchat_pluginpref_set_int (hexchat_plugin *pl, const char *var, int value)
{
	char buffer[12];

	snprintf (buffer, sizeof (buffer), "%d", value);
	return hexchat_pluginpref_set_str_real(static_cast<hexchat_plugin_internal*>(pl), var, buffer, set_mode::save);
}

int
hexchat_pluginpref_get_int (hexchat_plugin *pl, const char *var)
{
	char buffer[12];

	if (hexchat_pluginpref_get_str_real(static_cast<hexchat_plugin_internal*>(pl), var, buffer, sizeof(buffer)))
	{
		return atoi (buffer);
	}
	else
	{
		return -1;
	}
}

int
hexchat_pluginpref_delete (hexchat_plugin *pl, const char *var)
{
	return hexchat_pluginpref_set_str_real (pl, var, nullptr, set_mode::del);
}

int
hexchat_pluginpref_list (hexchat_plugin *pl, char* dest)
{
	namespace bfs = boost::filesystem;
	char confname[64];

	{
		glib_string token(g_strdup(static_cast<hexchat_plugin_internal*>(pl)->name.c_str()));
		canonalize_key(token.get());
		snprintf(confname, sizeof(confname), "addon_%s.conf", token.get());
	}

	boost::system::error_code ec;
	if (!bfs::exists(confname, ec)) /* no existing config file, no parsing */
		return 0;

	/* clean up garbage */
	strcpy(dest, "");
	bfs::ifstream stream(confname, std::ios::in | std::ios::binary);
	for (std::string line; std::getline(stream, line, '\n');)
	{
		auto eq = line.find_first_of('=');
		if (eq == std::string::npos)
			continue;
		auto part1 = line.substr(0, eq);
		auto part2 = line.substr(eq);
		boost::algorithm::trim(part1);
		boost::algorithm::trim(part2);
		// we have no idea how long dest is...
		g_strlcat(dest, part1.c_str(), 4096);
		g_strlcat(dest, ",", 4096);
		g_strlcat(dest, part2.c_str(), 4096);
		g_strlcat(dest, ",", 4096);
	}

	return 1;
}
