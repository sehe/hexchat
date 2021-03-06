/* X-Chat
 * Copyright (C) 1998 Peter Zelezny.
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

#include <string>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <boost/filesystem.hpp>
#include <boost/utility/string_ref.hpp>

#include "fe-gtk.hpp"

#include "../common/hexchat.hpp"
#include "../common/plugin.hpp"
#include "../common/util.hpp"
#include "../common/outbound.hpp"
#include "../common/fe.hpp"
#include "../common/hexchatc.hpp"
#include "../common/cfgfiles.hpp"
#include "gtkutil.hpp"
#include "maingui.hpp"

/* model for the plugin treeview */
enum
{
	NAME_COLUMN,
	VERSION_COLUMN,
	FILE_COLUMN,
	DESC_COLUMN,
	N_COLUMNS
};

static GtkWidget *plugin_window = NULL;


static GtkWidget *
plugingui_treeview_new (GtkWidget *box)
{
	GtkListStore *store;
	GtkWidget *view;
	GtkTreeViewColumn *col;
	int col_id;

	store = gtk_list_store_new (N_COLUMNS, G_TYPE_STRING, G_TYPE_STRING,
								G_TYPE_STRING, G_TYPE_STRING);
	g_return_val_if_fail (store != NULL, NULL);
	view = gtkutil_treeview_new (box, GTK_TREE_MODEL (store), NULL,
								 NAME_COLUMN, _("Name"),
								 VERSION_COLUMN, _("Version"),
								 FILE_COLUMN, _("File"),
								 DESC_COLUMN, _("Description"), -1);
	gtk_tree_view_set_rules_hint (GTK_TREE_VIEW (view), TRUE);
	for (col_id=0; (col = gtk_tree_view_get_column (GTK_TREE_VIEW (view), col_id));
		 col_id++)
			gtk_tree_view_column_set_alignment (col, 0.5);

	return view;
}

static char *
plugingui_getfilename (GtkTreeView *view)
{
	GtkTreeModel *model;
	GtkTreeSelection *sel;
	GtkTreeIter iter;
	GValue file;
	char *str;

	memset (&file, 0, sizeof (file));

	sel = gtk_tree_view_get_selection (view);
	if (gtk_tree_selection_get_selected (sel, &model, &iter))
	{
		gtk_tree_model_get_value (model, &iter, FILE_COLUMN, &file);

		str = g_value_dup_string (&file);
		g_value_unset (&file);

		return str;
	}

	return NULL;
}

static void
plugingui_close (GtkWidget *, gpointer)
{
	plugin_window = NULL;
}

extern GSList *plugin_list;

void
fe_pluginlist_update (void)
{
	namespace bfs = boost::filesystem;
	GSList *list;
	GtkTreeView *view;
	GtkListStore *store;
	GtkTreeIter iter;

	if (!plugin_window)
		return;

	view = static_cast<GtkTreeView*>(g_object_get_data (G_OBJECT (plugin_window), "view"));
	store = GTK_LIST_STORE (gtk_tree_view_get_model (view));
	gtk_list_store_clear (store);

	list = plugin_list;
	while (list)
	{
		auto pl = static_cast<hexchat_plugin_internal*>( list->data);
		if (!pl->version.empty())
		{
			gtk_list_store_append (store, &iter);
			gtk_list_store_set (store, &iter, NAME_COLUMN, pl->name.c_str(),
								VERSION_COLUMN, pl->version.c_str(),
								FILE_COLUMN, bfs::path(pl->filename).filename().string().c_str(),
								DESC_COLUMN, pl->desc.c_str(), -1);
		}
		list = list->next;
	}
}

static void
plugingui_load_cb (session *sess, const char *file)
{
	if (file)
	{
		std::string buf(strlen(file) + 9, '\0');

		if (strchr (file, ' '))
			snprintf(&buf[0], buf.size(), "LOAD \"%s\"", file);
		else
			snprintf(&buf[0], buf.size(), "LOAD %s", file);
		handle_command (sess, &buf[0], FALSE);
	}
}

void
plugingui_load (void)
{
	glib_string sub_dir(g_build_filename(get_xdir(), "addons", NULL));

	gtkutil_file_req (_("Select a Plugin or Script to load"), (filereqcallback) plugingui_load_cb, current_sess,
							sub_dir.get(), "*." G_MODULE_SUFFIX ";*.lua;*.pl;*.py;*.tcl;*.js", FRF_FILTERISINITIAL|FRF_EXTENSIONS);
}

static void
plugingui_loadbutton_cb (GtkWidget *, gpointer)
{
	plugingui_load ();
}

static void
plugingui_unload (GtkWidget *, gpointer)
{
	char *modname, *file;
	GtkTreeView *view;
	GtkTreeIter iter;
	
	view = static_cast<GtkTreeView*>(g_object_get_data(G_OBJECT(plugin_window), "view"));
	if (!gtkutil_treeview_get_selected (view, &iter, NAME_COLUMN, &modname,
										FILE_COLUMN, &file, -1))
		return;
	glib_string file_ptr(file);
	glib_string modname_ptr(modname);
	if (g_str_has_suffix (file, "." G_MODULE_SUFFIX))
	{
		if (plugin_kill (modname, FALSE) == 2)
			fe_message (_("That plugin is refusing to unload.\n"), FE_MSG_ERROR);
	} else
	{
		/* let python.so or perl.so handle it */
		std::string buf(strlen(file) + 10, '\0');
		if (strchr (file, ' '))
			snprintf (&buf[0], buf.size(), "UNLOAD \"%s\"", file);
		else
			snprintf(&buf[0], buf.size(), "UNLOAD %s", file);
		handle_command(current_sess, &buf[0], FALSE);
	}
}

static void
plugingui_reloadbutton_cb (GtkWidget *, GtkTreeView *view)
{
	glib_string file(plugingui_getfilename(view));

	if (file)
	{
		std::string buf(strlen(file.get()) + 9, '\0');
		if (strchr (file.get(), ' '))
			snprintf (&buf[0], buf.size(), _("RELOAD \"%s\""), file.get());
		else
			snprintf(&buf[0], buf.size(), _("RELOAD %s"), file.get());
		handle_command(current_sess, &buf[0], FALSE);
	}
}

void
plugingui_open (void)
{
	GtkWidget *view;
	GtkWidget *vbox, *hbox;

	if (plugin_window)
	{
		mg_bring_tofront (plugin_window);
		return;
	}

	plugin_window = mg_create_generic_tab ("Addons", _(DISPLAY_NAME": Plugins and Scripts"),
		FALSE, TRUE, G_CALLBACK(plugingui_close), NULL,
														 500, 250, &vbox, 0);
	gtkutil_destroy_on_esc (plugin_window);

	view = plugingui_treeview_new (vbox);
	g_object_set_data (G_OBJECT (plugin_window), "view", view);


	hbox = gtk_hbutton_box_new ();
	gtk_button_box_set_layout (GTK_BUTTON_BOX (hbox), GTK_BUTTONBOX_SPREAD);
	gtk_container_set_border_width (GTK_CONTAINER (hbox), 5);
	gtk_box_pack_end (GTK_BOX (vbox), hbox, 0, 0, 0);

	gtkutil_button (hbox, GTK_STOCK_REVERT_TO_SAVED, NULL,
		G_CALLBACK(plugingui_loadbutton_cb), NULL, _("_Load..."));

	gtkutil_button (hbox, GTK_STOCK_DELETE, NULL,
		G_CALLBACK(plugingui_unload), NULL, _("_Unload"));

	gtkutil_button (hbox, GTK_STOCK_REFRESH, NULL,
		G_CALLBACK(plugingui_reloadbutton_cb), view, _("_Reload"));

	fe_pluginlist_update ();

	gtk_widget_show_all (plugin_window);
}
