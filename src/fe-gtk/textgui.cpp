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
#include <array>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sstream>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <boost/format.hpp>
#include <boost/utility/string_ref.hpp>

#include "fe-gtk.hpp"

#include "../common/hexchat.hpp"
#include "../common/hexchatc.hpp"
#include "../common/cfgfiles.hpp"
#include "../common/outbound.hpp"
#include "../common/fe.hpp"
#include "../common/text.hpp"
#include "../common/util.hpp"
#include "gtkutil.hpp"
#include "xtext.hpp"
#include "maingui.hpp"
#include "palette.hpp"
#include "textgui.hpp"
#include "gtk_helpers.hpp"
typedef std::char_traits < unsigned char > uchar_traits;
extern const text_event te[];
extern std::array<std::string, NUM_XP> pntevts_text;
extern std::array<std::string, NUM_XP> pntevts;

static GtkWidget *pevent_dialog = NULL, *pevent_dialog_twid,
	*pevent_dialog_list, *pevent_dialog_hlist;

enum
{
	EVENT_COLUMN,
	TEXT_COLUMN,
	ROW_COLUMN,
	N_COLUMNS
};

/* this is only used in xtext.c for indented timestamping */
int
xtext_get_stamp_str (time_t tim, char **ret)
{
	return get_stamp_str (prefs.hex_stamp_text_format, tim, ret);
}

static void
PrintTextLine (xtext_buffer *xtbuf, unsigned char *text, int len, int indent, time_t timet)
{
	if (len == 0)
		len = 1;

	if (!indent)
	{
		if (prefs.hex_stamp_text)
		{
			int stamp_size;
			char *stamp;

			if (timet == 0)
				timet = time (0);

			stamp_size = get_stamp_str (prefs.hex_stamp_text_format, timet, &stamp);
			glib_string stamp_ptr(stamp);
			std::vector<unsigned char> new_text(len + stamp_size + 1);
			std::copy_n(stamp, stamp_size, new_text.begin());
			std::copy_n(text, len, new_text.begin() + stamp_size);
			gtk_xtext_append (xtbuf, new_text.data(), len + stamp_size, timet);
		} else
			gtk_xtext_append (xtbuf, text, len, timet);
		return;
	}

	auto tab = std::char_traits<unsigned char>::find(text, len, '\t');
	if (tab && tab < (text + len))
	{
		auto leftlen = tab - text;
		gtk_xtext_append_indent (xtbuf,
										 text, leftlen, tab + 1, len - (leftlen + 1), timet);
	} else
		gtk_xtext_append_indent (xtbuf, 0, 0, text, len, timet);
}

void
PrintTextRaw (void *xtbuf, unsigned char *text, int indent, time_t stamp)
{
	unsigned char *last_text = text;
	int len = 0;
	bool beep_done = false;

	/* split the text into separate lines */
	while (1)
	{
		switch (*text)
		{
		case 0:
			PrintTextLine (static_cast<xtext_buffer*>(xtbuf), last_text, len, indent, stamp);
			return;
		case '\n':
			PrintTextLine(static_cast<xtext_buffer*>(xtbuf), last_text, len, indent, stamp);
			text++;
			if (*text == 0)
				return;
			last_text = text;
			len = 0;
			break;
		case ATTR_BEEP:
			*text = ' ';
			if (!beep_done) /* beeps may be slow, so only do 1 per line */
			{
				beep_done = true;
				if (!prefs.hex_input_filter_beep)
					gdk_beep ();
			}
		default:
			text++;
			len++;
		}
	}
}

static void
pevent_dialog_close (GtkWidget *wid, gpointer arg)
{
	pevent_dialog = NULL;
	pevent_save (NULL);
}

static void
pevent_edited (GtkCellRendererText *render, gchar *pathstr, gchar *new_text, gpointer data)
{
	GtkTreeModel *model = gtk_tree_view_get_model (GTK_TREE_VIEW (pevent_dialog_list));
	GtkTreeIter iter;
	GtkXText *xtext = GTK_XTEXT (pevent_dialog_twid);
	int m;
	const char *text;
	int sig;

	if (!gtkutil_treeview_get_selected (GTK_TREE_VIEW (pevent_dialog_list),
											&iter, ROW_COLUMN, &sig, -1))
		return;

	text = new_text;
	auto len = strlen (new_text);
	std::string out;
	if (pevt_build_string (text, out, m) != 0)
	{
		fe_message (_("There was an error parsing the string"), FE_MSG_ERROR);
		return;
	}
	if (m > (te[sig].num_args & 0x7f))
	{
		std::ostringstream outbuf;
		outbuf << boost::format(_("This signal is only passed %d args, $%d is invalid")) % (te[sig].num_args & 0x7f) % m;
		fe_message (outbuf.str(), FE_MSG_WARN);
		return;
	}
	
	GtkTreePathPtr path(gtk_tree_path_new_from_string (pathstr));
	gtk_tree_model_get_iter (model, &iter, path.get());

	gtk_list_store_set (GTK_LIST_STORE (model), &iter, TEXT_COLUMN, new_text, -1);

	pntevts_text[sig] = text;
	pntevts[sig] = std::move(out);

	std::string buf(text, len);
	buf.push_back('\n');
	buf = check_special_chars (buf, true);

	PrintTextRaw(xtext->buffer, (unsigned char*)&buf[0], 0, 0);

	/* Scroll to bottom */
	gtk_adjustment_set_value (xtext->adj, gtk_adjustment_get_upper (xtext->adj));

	/* save this when we exit */
	prefs.save_pevents = 1;
}

static void
pevent_dialog_hfill (GtkWidget *list, int e)
{
	int i = 0;
	const char *text;
	GtkTreeIter iter;
	GtkListStore *store;

	store = GTK_LIST_STORE (gtk_tree_view_get_model (GTK_TREE_VIEW (list)));
	gtk_list_store_clear (store);

	while (i < (te[e].num_args & 0x7f))
	{
		text = _(te[e].help[i]);
		i++;
		if (text[0] == '\001')
			text++;
		gtk_list_store_insert_with_values (store, &iter, -1,
													  0, i,
													  1, text, -1);
	}
}

static void
pevent_selection_changed (GtkTreeSelection *sel, gpointer userdata)
{
	GtkTreeIter iter;
	int sig;

	if (!gtkutil_treeview_get_selected (GTK_TREE_VIEW (pevent_dialog_list),
										&iter, ROW_COLUMN, &sig, -1))
	{
		gtk_list_store_clear (GTK_LIST_STORE (gtk_tree_view_get_model (GTK_TREE_VIEW (pevent_dialog_hlist))));
		return;
	}

	pevent_dialog_hfill (pevent_dialog_hlist, sig);
}

static void
pevent_dialog_fill (GtkWidget *list)
{
	GtkListStore *store;
	GtkTreeIter iter;

	store = GTK_LIST_STORE (gtk_tree_view_get_model (GTK_TREE_VIEW (list)));
	gtk_list_store_clear (store);

	int i = NUM_XP;
	do
	{
		i--;
		gtk_list_store_insert_with_values (store, &iter, 0,
													  EVENT_COLUMN, te[i].name,
													  TEXT_COLUMN, pntevts_text[i].c_str(),
													  ROW_COLUMN, i, -1);
	}
	while (i != 0);
}

static void
pevent_save_req_cb (void *arg1, char *file)
{
	if (file)
		pevent_save (file);
}

static void
pevent_save_cb (GtkWidget * wid, void *data)
{
	if (data)
	{
		gtkutil_file_req (_("Print Texts File"), pevent_save_req_cb, NULL,
								NULL, NULL, FRF_WRITE);
		return;
	}
	pevent_save (NULL);
}

static void
pevent_load_req_cb (void *arg1, char *file)
{
	if (file)
	{
		pevent_load (file);
		pevent_make_pntevts ();
		pevent_dialog_fill (pevent_dialog_list);
		prefs.save_pevents = 1;
	}
}

static void
pevent_load_cb (GtkWidget * wid, void *data)
{
	gtkutil_file_req (_("Print Texts File"), pevent_load_req_cb, NULL, NULL, NULL, 0);
}

static void
pevent_ok_cb (GtkWidget * wid, void *data)
{
	gtk_widget_destroy (pevent_dialog);
}

static void
pevent_test_cb (GtkWidget * wid, GtkWidget * twid)
{
	for (int n = 0; n < NUM_XP; n++)
	{
		std::string out(_(pntevts_text[n].c_str()));
		out.push_back('\n');
		out = check_special_chars (out, true);
		PrintTextRaw (GTK_XTEXT (twid)->buffer, (unsigned char*)&out[0], 0, 0);
	}
}

static GtkWidget *
pevent_treeview_new (GtkWidget *box)
{
	GtkWidget *scroll;
	GtkListStore *store;
	GtkTreeViewColumn *col;
	GtkTreeSelection *sel;
	GtkWidget *view;
	GtkCellRenderer *render;

	scroll = gtk_scrolled_window_new (NULL, NULL);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scroll), GTK_SHADOW_IN);
	gtk_widget_set_size_request (GTK_WIDGET (scroll), -1, 250);

	store = gtk_list_store_new (N_COLUMNS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT);
	g_return_val_if_fail (store != NULL, NULL);

	view = gtk_tree_view_new_with_model (GTK_TREE_MODEL (store));
	gtk_tree_view_set_fixed_height_mode (GTK_TREE_VIEW (view), TRUE);
	gtk_tree_view_set_enable_search (GTK_TREE_VIEW (view), TRUE);
	gtk_tree_view_set_rules_hint (GTK_TREE_VIEW (view), TRUE);

	sel = gtk_tree_view_get_selection (GTK_TREE_VIEW (view));
	g_signal_connect (G_OBJECT (sel), "changed",
		G_CALLBACK (pevent_selection_changed), NULL);

	render = gtk_cell_renderer_text_new ();
	gtk_tree_view_insert_column_with_attributes (
			GTK_TREE_VIEW (view), EVENT_COLUMN,
			_("Event"), render,
			"text", EVENT_COLUMN,
			NULL);

	render = gtk_cell_renderer_text_new ();
	g_object_set (render, "editable", TRUE, NULL);
	g_signal_connect (G_OBJECT (render), "edited",
		G_CALLBACK (pevent_edited), NULL);
	gtk_tree_view_insert_column_with_attributes (
			GTK_TREE_VIEW (view), TEXT_COLUMN,
			_("Text"), render,
			"text", TEXT_COLUMN,
			NULL);

	col = gtk_tree_view_get_column (GTK_TREE_VIEW (view), EVENT_COLUMN);
	gtk_tree_view_column_set_sizing (col, GTK_TREE_VIEW_COLUMN_AUTOSIZE);
	gtk_tree_view_column_set_resizable (col, TRUE);
	gtk_tree_view_column_set_min_width (col, 100);

	gtk_container_add (GTK_CONTAINER (scroll), view);
	gtk_container_add (GTK_CONTAINER (box), scroll);

	return view;
}

static GtkWidget *
pevent_hlist_treeview_new (GtkWidget *box)
{
	GtkWidget *scroll;
	GtkListStore *store;
	GtkTreeViewColumn *col;
	GtkWidget *view;
	GtkCellRenderer *render;

	scroll = gtk_scrolled_window_new (NULL, NULL);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scroll), GTK_SHADOW_IN);

	store = gtk_list_store_new (2, G_TYPE_INT, G_TYPE_STRING);
	g_return_val_if_fail (store != NULL, NULL);

	view = gtk_tree_view_new_with_model (GTK_TREE_MODEL (store));
	gtk_tree_view_set_fixed_height_mode (GTK_TREE_VIEW (view), TRUE);
	gtk_tree_view_set_enable_search (GTK_TREE_VIEW (view), FALSE);
	gtk_widget_set_can_focus (view, FALSE);

	render = gtk_cell_renderer_text_new ();
	gtk_tree_view_insert_column_with_attributes (
			GTK_TREE_VIEW (view), 0,
			_("$ Number"), render,
			"text", 0,
			NULL);

	render = gtk_cell_renderer_text_new ();
	gtk_tree_view_insert_column_with_attributes (
			GTK_TREE_VIEW (view), 1,
			_("Description"), render,
			"text", 1,
			NULL);

	col = gtk_tree_view_get_column (GTK_TREE_VIEW (view), 0);
	gtk_tree_view_column_set_sizing (col, GTK_TREE_VIEW_COLUMN_AUTOSIZE);

	gtk_container_add (GTK_CONTAINER (scroll), view);
	gtk_container_add (GTK_CONTAINER (box), scroll);

	return view;
}

void
pevent_dialog_show ()
{
	GtkWidget *vbox, *hbox, *wid, *pane;

	if (pevent_dialog)
	{
		mg_bring_tofront (pevent_dialog);
		return;
	}

	pevent_dialog =
			  mg_create_generic_tab ("edit events", _("Edit Events"),
			  TRUE, FALSE, G_CALLBACK(pevent_dialog_close), NULL,
											 600, 455, &vbox, 0);

	pane = gtk_vpaned_new ();
	gtk_box_pack_start (GTK_BOX (vbox), pane, TRUE, TRUE, 0);
	
	pevent_dialog_list = pevent_treeview_new (pane);
	pevent_dialog_fill (pevent_dialog_list);

	pevent_dialog_hlist = pevent_hlist_treeview_new (pane);

	wid = gtk_scrolled_window_new (NULL, NULL);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (wid), GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
	gtk_box_pack_start (GTK_BOX (vbox), wid, FALSE, TRUE, 0);

	pevent_dialog_twid = gtk_xtext_new (colors, false);
	gtk_widget_set_sensitive (pevent_dialog_twid, FALSE);
	gtk_widget_set_size_request (pevent_dialog_twid, -1, 75);
	gtk_container_add (GTK_CONTAINER (wid), pevent_dialog_twid);
	gtk_xtext_set_font (GTK_XTEXT (pevent_dialog_twid), prefs.hex_text_font);

	hbox = gtk_hbutton_box_new ();
	gtk_button_box_set_layout (GTK_BUTTON_BOX (hbox), GTK_BUTTONBOX_SPREAD);
	gtk_box_pack_start (GTK_BOX (vbox), hbox, FALSE, FALSE, 4);
	gtkutil_button(hbox, GTK_STOCK_SAVE_AS, NULL, G_CALLBACK(pevent_save_cb),
						 (void *) 1, _("Save As..."));
	gtkutil_button(hbox, GTK_STOCK_OPEN, NULL, G_CALLBACK(pevent_load_cb),
						 NULL, _("Load From..."));
	gtkutil_button(hbox, NULL, NULL, G_CALLBACK(pevent_test_cb),
						pevent_dialog_twid, _("Test All"));
	gtkutil_button(hbox, GTK_STOCK_OK, NULL, G_CALLBACK(pevent_ok_cb),
						NULL, _("OK"));

	gtk_widget_show_all (pevent_dialog);
}
