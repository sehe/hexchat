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

#ifdef WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#endif

#include <atomic>
#include <random>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <chrono>
#include <new>
#include <sys/types.h>
#include <sys/stat.h>
#include <boost/utility/string_ref.hpp>

#define WANTSOCKET
#include "inet.hpp"

#ifdef WIN32
#include <boost/locale.hpp>
#include <boost/filesystem/path.hpp>
#include <windows.h>
#else
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#endif
#include "hexchat.hpp"
#include "fe.hpp"
#include "util.hpp"
#include "cfgfiles.hpp"
#include "chanopt.hpp"
#include "ignore.hpp"
#include "plugin.hpp"
#include "plugin-timer.hpp"
#include "notify.hpp"
#include "server.hpp"
#include "session.hpp"
#include "servlist.hpp"
#include "outbound.hpp"
#include "text.hpp"
#include "url.hpp"
#include "hexchatc.hpp"
#include "dcc.hpp"
#include "userlist.hpp"

#if ! GLIB_CHECK_VERSION (2, 36, 0)
#include <glib-object.h>			/* for g_type_init() */
#endif

#ifdef USE_LIBPROXY
#include <proxy.h>
#endif

namespace dcc = hexchat::dcc;

GSList *popup_list = 0;
GSList *button_list = 0;
GSList *dlgbutton_list = 0;
GSList *command_list = 0;
GSList *ctcp_list = 0;
GSList *replace_list = 0;
GSList *sess_list = 0;
GSList *dcc_list = 0;
GSList *usermenu_list = 0;
GSList *urlhandler_list = 0;
GSList *tabmenu_list = 0;

/*
 * This array contains 5 double linked lists, one for each priority in the
 * "interesting session" queue ("channel" stands for everything but
 * SESS_DIALOG):
 *
 * [0] queries with hilight
 * [1] queries
 * [2] channels with hilight
 * [3] channels with dialogue
 * [4] channels with other data
 *
 * Each time activity happens the corresponding session is put at the
 * beginning of one of the lists.  The aim is to be able to switch to the
 * session with the most important/recent activity.
 */
GList *sess_list_by_lastact[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};


static std::atomic_bool in_hexchat_exit = { false };
std::atomic_bool hexchat_is_quitting = { false };
/* command-line args */
int arg_dont_autoconnect = FALSE;
int arg_skip_plugins = FALSE;
char *arg_url = nullptr;
char **arg_urls = nullptr;
char *arg_command = nullptr;
gint arg_existing = FALSE;

#ifdef USE_DBUS
#include "dbus/dbus-client.h"
#include "dbus/dbus-plugin.h"
#endif /* USE_DBUS */

struct session *current_tab;
struct session *current_sess = 0;
struct hexchatprefs prefs;

#ifdef USE_LIBPROXY
pxProxyFactory *libproxy_factory;
#endif

int RAND_INT(int n)
{
	static std::random_device rd;
	static std::mt19937 twstr(rd());
	std::uniform_int_distribution<int> dist(0, n);
	return dist(twstr);
}

/*
 * Update the priority queue of the "interesting sessions"
 * (sess_list_by_lastact).
 */
void
lastact_update(session *sess)
{
	int oldidx = sess->lastact_idx;
	int newidx = LACT_NONE;
	bool dia = (sess->type == session::SESS_DIALOG);

	if (sess->nick_said)
		newidx = dia? LACT_QUERY_HI: LACT_CHAN_HI;
	else if (sess->msg_said)
		newidx = dia? LACT_QUERY: LACT_CHAN;
	else if (sess->new_data)
		newidx = dia? LACT_QUERY: LACT_CHAN_DATA;

	/* If already first at the right position, just return */
	if (oldidx == newidx &&
		 (newidx == LACT_NONE || g_list_index(sess_list_by_lastact[newidx], sess) == 0))
		return;

	/* Remove from the old position */
	if (oldidx != LACT_NONE)
		sess_list_by_lastact[oldidx] = g_list_remove(sess_list_by_lastact[oldidx], sess);

	/* Add at the new position */
	sess->lastact_idx = newidx;
	if (newidx != LACT_NONE)
		sess_list_by_lastact[newidx] = g_list_prepend(sess_list_by_lastact[newidx], sess);
	return;
}

/*
 * Extract the first session from the priority queue of sessions with recent
 * activity. Return NULL if no such session can be found.
 *
 * If filter is specified, skip a session if filter(session) returns 0. This
 * can be used for UI-specific needs, e.g. in fe-gtk we want to filter out
 * detached sessions.
 */
session *
lastact_getfirst(int (*filter) (session *sess))
{
	session *sess = nullptr;

	/* 5 is the number of priority classes LACT_ */
	for (int i = 0; i < 5 && !sess; i++)
	{
		auto curitem = sess_list_by_lastact[i];
		while (curitem && !sess)
		{
			sess = static_cast<session*>(g_list_nth_data(curitem, 0));
			if (!sess || (filter && !filter(sess)))
			{
				sess = nullptr;
				curitem = g_list_next(curitem);
			}
		}

		if (sess)
		{
			sess_list_by_lastact[i] = g_list_remove(sess_list_by_lastact[i], sess);
			sess->lastact_idx = LACT_NONE;
		}
	}
	
	return sess;
}

bool
is_session (session * sess)
{
	return g_slist_find(sess_list, sess) != nullptr;
}

session * find_dialog(const server &serv, const boost::string_ref &nick)
{
	GSList *list = sess_list;
	session *sess;

	while (list)
	{
		sess = static_cast<session*>(list->data);
		if (sess->server == &serv && sess->type == session::SESS_DIALOG)
		{
			if (!serv.compare (nick, sess->channel))
				return (sess);
		}
		list = list->next;
	}
	return nullptr;
}

session *find_channel(const server &serv, const boost::string_ref &chan)
{
	session *sess;
	GSList *list = sess_list;
	while (list)
	{
		sess = static_cast<session*>(list->data);
		if ((&serv == sess->server) && sess->type == session::SESS_CHANNEL)
		{
			if (!serv.compare(chan, sess->channel))
				return sess;
		}
		list = list->next;
	}
	return nullptr;
}

static void
lagcheck_update (void)
{
	GSList *list = serv_list;
	
	if (!prefs.hex_gui_lagometer)
		return;

	while (list)
	{
		auto serv = static_cast<server*>(list->data);
		if (serv->lag_sent)
			fe_set_lag (serv, -1);

		list = list->next;
	}
}

void
lag_check (void)
{
	using namespace boost;
	server *serv;
	GSList *list = serv_list;
	unsigned long tim;
	char tbuf[128];
	auto now = chrono::steady_clock::now();

	tim = make_ping_time ();

	while (list)
	{
		serv = static_cast<server*>(list->data);
		if (serv->connected && serv->end_of_motd)
		{
			auto seconds = chrono::duration_cast<chrono::seconds>(now - serv->ping_recv).count();
			if (prefs.hex_net_ping_timeout && seconds > prefs.hex_net_ping_timeout && seconds > 0)
			{
				snprintf (tbuf, sizeof(tbuf), "%d", seconds);
				EMIT_SIGNAL (XP_TE_PINGTIMEOUT, serv->server_session, tbuf, nullptr,
								 nullptr, nullptr, 0);
				if (prefs.hex_net_auto_reconnect)
					serv->auto_reconnect (false, -1);
			} else
			{
				snprintf (tbuf, sizeof (tbuf), "LAG%lu", tim);
				serv->p_ping ("", tbuf);
				
				if (!serv->lag_sent)
				{
					serv->lag_sent = tim;
					fe_set_lag (serv, -1);
				}
			}
		}
		list = list->next;
	}
}

static int
away_check (void)
{
	session *sess;
	GSList *list;
	int sent, loop = 0;
	bool full;
	if (!prefs.hex_away_track)
		return 1;

doover:
	/* request an update of AWAY status of 1 channel every 30 seconds */
	full = true;
	sent = 0;	/* number of WHOs (users) requested */
	list = sess_list;
	while (list)
	{
		sess = static_cast<session*>(list->data);

		if (sess->server->connected &&
			sess->type == session::SESS_CHANNEL &&
			 sess->channel[0] &&
			 (sess->total <= prefs.hex_away_size_max || !prefs.hex_away_size_max))
		{
			if (!sess->done_away_check)
			{
				full = false;

				/* if we're under 31 WHOs, send another channels worth */
				if (sent < 31 && !sess->doing_who)
				{
					sess->done_away_check = TRUE;
					sess->doing_who = TRUE;
					/* this'll send a WHO #channel */
					sess->server->p_away_status (sess->channel);
					sent += sess->total;
				}
			}
		}

		list = list->next;
	}

	/* done them all, reset done_away_check to FALSE and start over unless we have away-notify */
	if (full)
	{
		list = sess_list;
		while (list)
		{
			sess = static_cast<session*>(list->data);
			if (!sess->server->have_awaynotify)
				sess->done_away_check = FALSE;
			list = list->next;
		}
		loop++;
		if (loop < 2)
			goto doover;
	}

	return 1;
}

static int
hexchat_misc_checks (void)		/* this gets called every 1/2 second */
{
	static int count = 0;

	count++;

	lagcheck_update ();			/* every 500ms */

	if (count % 2)
		dcc::dcc_check_timeouts ();	/* every 1 second */

	if (count >= 60)				/* every 30 seconds */
	{
		if (prefs.hex_gui_lagometer)
			lag_check ();
		count = 0;
	}

	return 1;
}

/* executed when the first irc window opens */

static void
irc_init (session *sess)
{
	static bool done_init = false;
	char *buf;
	int i;

	if (done_init)
		return;

	done_init = true;

	plugin_add(sess, nullptr, nullptr, timer_plugin_init, timer_plugin_deinit, nullptr, false);

#ifdef USE_PLUGIN
	if (!arg_skip_plugins)
		plugin_auto_load (sess);	/* autoload ~/.xchat *.so */
#endif

#ifdef USE_DBUS
	plugin_add (sess, nullptr, nullptr, dbus_plugin_init, nullptr, nullptr, false);
#endif

	if (prefs.hex_notify_timeout)
		notify_tag = fe_timeout_add (prefs.hex_notify_timeout * 1000,
											  (GSourceFunc)notify_checklist, 0);

	fe_timeout_add(prefs.hex_away_timeout * 1000, (GSourceFunc)away_check, 0);
	fe_timeout_add(500, (GSourceFunc)hexchat_misc_checks, 0);

	if (arg_url != nullptr)
	{
		buf = g_strdup_printf ("server %s", arg_url);
		g_free (arg_url);	/* from GOption */
		handle_command (sess, buf, FALSE);
		g_free (buf);
	}
	
	if (arg_urls != nullptr)
	{
		for (i = 0; i < g_strv_length(arg_urls); i++)
		{
			buf = g_strdup_printf ("%s %s", i==0? "server" : "newserver", arg_urls[i]);
			handle_command (sess, buf, FALSE);
			g_free (buf);
		}
		g_strfreev (arg_urls);
	}

	if (arg_command != nullptr)
	{
		handle_command (sess, arg_command, FALSE);
		g_free (arg_command);
	}

	/* load -e <xdir>/startup.txt */
	load_perform_file (sess, "startup.txt");
}

session::session(struct server *serv, const char *from, ::session::session_type type)
	:server(serv),
	logfd(-1),
	scrollfd(-1),
	scrollwritten(),
	type(type),
	alert_beep(SET_DEFAULT),
	alert_taskbar(SET_DEFAULT),
	alert_tray(SET_DEFAULT),

	text_hidejoinpart(SET_DEFAULT),
	text_logging(SET_DEFAULT),
	text_scrollback(SET_DEFAULT),
	text_strip(SET_DEFAULT),

	lastact_idx(LACT_NONE),
	me(nullptr),
	channel(),
	waitchannel(),
	willjoinchannel(),

	lastlog_sess(nullptr),
	running_exec(nullptr),
	gui(nullptr),
	res(nullptr),

	scrollback_replay_marklast(nullptr),

	ops(),
	hops(),
	voices(),
	total(),
	new_data(),
	nick_said(),
	msg_said(),
	ignore_date(),
	ignore_mode(),
	ignore_names(),
	end_of_names(),
	doing_who(),
	done_away_check(),
	lastlog_flags()
{
	if (from)
	{
		safe_strcpy(this->channel, from, CHANLEN);
		this->name = from;
	}
}

static session *
session_new (server *serv, const char *from, int type, int focus)
{
	session *sess = new session(serv, from, type);

	sess_list = g_slist_prepend (sess_list, sess);

	fe_new_window (sess, focus);

	return sess;
}

session *
new_ircwindow (server *serv, const char *name, ::session::session_type type, int focus)
{
	session *sess;

	switch (type)
	{
	case session::SESS_SERVER:
		serv = server_new ();
		if (!serv)
			return nullptr;
		if (prefs.hex_gui_tab_server)
			sess = session_new(serv, name, session::SESS_SERVER, focus);
		else
			sess = session_new(serv, name, session::SESS_CHANNEL, focus);
		serv->server_session = sess;
		serv->front_session = sess;
		break;
	case session::SESS_DIALOG:
		sess = session_new (serv, name, type, focus);
		log_open_or_close (sess);
		break;
	default:
/*	case SESS_CHANNEL:
	case SESS_NOTICES:
	case SESS_SNOTICES:*/
		sess = session_new (serv, name, type, focus);
		break;
	}

	irc_init (sess);
	chanopt_load (sess);
	scrollback_load (*sess);
	if (sess->scrollwritten && sess->scrollback_replay_marklast)
		sess->scrollback_replay_marklast (sess);
	plugin_emit_dummy_print (sess, "Open Context");

	return sess;
}

nbexec::nbexec(session *sess)
	:myfd(),
	childpid(),
	tochannel(),						/* making this int keeps the struct 4-byte aligned */
	iotag(),
	sess(sess){}

static void
exec_notify_kill (session * sess)
{
#ifndef WIN32
	struct nbexec *re;
	if (sess->running_exec != nullptr)
	{
		re = sess->running_exec;
		sess->running_exec = nullptr;
		kill (re->childpid, SIGKILL);
		waitpid (re->childpid, nullptr, WNOHANG);
		fe_input_remove (re->iotag);
		close (re->myfd);
		delete re;
	}
#endif
}

static void
send_quit_or_part (session * killsess)
{
	bool willquit = true;
	GSList *list;
	session *sess;
	server *killserv = killsess->server;

	/* check if this is the last session using this server */
	list = sess_list;
	while (list)
	{
		sess = (session *) list->data;
		if (sess->server == killserv && sess != killsess)
		{
			willquit = false;
			list = 0;
		} else
			list = list->next;
	}

	if (hexchat_is_quitting)
		willquit = true;

	if (killserv->connected)
	{
		if (willquit)
		{
			if (!killserv->sent_quit)
			{
				killserv->flush_queue ();
				server_sendquit (killsess);
				killserv->sent_quit = true;
			}
		} else
		{
			if (killsess->type == session::SESS_CHANNEL && killsess->channel[0] &&
				 !killserv->sent_quit)
			{
				server_sendpart (*killserv, killsess->channel, nullptr);
			}
			}
		}
	}

session::~session()
{
	if (this->type == session::SESS_CHANNEL)
		userlist_free(*this);

	exec_notify_kill(this);
}

void
session_free (session *killsess)
{
	server *killserv = killsess->server;
	session *sess;
	GSList *list;
	int oldidx;

	plugin_emit_dummy_print (killsess, "Close Context");

	if (current_tab == killsess)
		current_tab = nullptr;

	if (killserv->server_session == killsess)
		killserv->server_session = nullptr;

	if (killserv->front_session == killsess)
	{
		/* front_session is closed, find a valid replacement */
		killserv->front_session = nullptr;
		list = sess_list;
		while (list)
		{
			sess = static_cast<session *>( list->data);
			if (sess != killsess && sess->server == killserv)
			{
				killserv->front_session = sess;
				if (!killserv->server_session)
					killserv->server_session = sess;
				break;
			}
			list = list->next;
		}
	}

	if (!killserv->server_session)
		killserv->server_session = killserv->front_session;

	sess_list = g_slist_remove (sess_list, killsess);

	oldidx = killsess->lastact_idx;
	if (oldidx != LACT_NONE)
		sess_list_by_lastact[oldidx] = g_list_remove(sess_list_by_lastact[oldidx], killsess);

	log_close (*killsess);
	scrollback_close (*killsess);
	chanopt_save (killsess);

	send_quit_or_part (killsess);

	fe_session_callback (killsess);

	if (current_sess == killsess)
	{
		current_sess = nullptr;
		if (sess_list)
			current_sess = static_cast<session*>(sess_list->data);
	}

	delete killsess;

	if (!sess_list && !in_hexchat_exit)
		hexchat_exit ();						/* sess_list is empty, quit! */

	list = sess_list;
	while (list)
	{
		sess = (session *) list->data;
		if (sess->server == killserv)
			return;					  /* this server is still being used! */
		list = list->next;
	}

	server_free (killserv);
}

static void
free_sessions (void)
{
	GSList *list = sess_list;

	while (list)
	{
		fe_close_window (static_cast<session*>(list->data));
		list = sess_list;
	}
}


static const char defaultconf_ctcp[] =
	"NAME TIME\n"				"CMD nctcp %s TIME %t\n\n"\
	"NAME PING\n"				"CMD nctcp %s PING %d\n\n";

static const char defaultconf_replace[] =
	"NAME teh\n"				"CMD the\n\n";
/*	"NAME r\n"					"CMD are\n\n"\
	"NAME u\n"					"CMD you\n\n"*/

static const char defaultconf_commands[] =
	"NAME ACTION\n"		"CMD me &2\n\n"\
	"NAME AME\n"			"CMD allchan me &2\n\n"\
	"NAME ANICK\n"			"CMD allserv nick &2\n\n"\
	"NAME AMSG\n"			"CMD allchan say &2\n\n"\
	"NAME BANLIST\n"		"CMD quote MODE %c +b\n\n"\
	"NAME CHAT\n"			"CMD dcc chat %2\n\n"\
	"NAME DIALOG\n"		"CMD query %2\n\n"\
	"NAME DMSG\n"			"CMD msg =%2 &3\n\n"\
	"NAME EXIT\n"			"CMD quit\n\n"\
	"NAME GREP\n"			"CMD lastlog -r -- &2\n\n"\
	"NAME IGNALL\n"			"CMD ignore %2!*@* ALL\n\n"\
	"NAME J\n"				"CMD join &2\n\n"\
	"NAME KILL\n"			"CMD quote KILL %2 :&3\n\n"\
	"NAME LEAVE\n"			"CMD part &2\n\n"\
	"NAME M\n"				"CMD msg &2\n\n"\
	"NAME OMSG\n"			"CMD msg @%c &2\n\n"\
	"NAME ONOTICE\n"		"CMD notice @%c &2\n\n"\
	"NAME RAW\n"			"CMD quote &2\n\n"\
	"NAME SERVHELP\n"		"CMD quote HELP\n\n"\
	"NAME SPING\n"			"CMD ping\n\n"\
	"NAME SQUERY\n"		"CMD quote SQUERY %2 :&3\n\n"\
	"NAME SSLSERVER\n"	"CMD server -ssl &2\n\n"\
	"NAME SV\n"				"CMD echo HexChat %v %m\n\n"\
	"NAME UMODE\n"			"CMD mode %n &2\n\n"\
	"NAME UPTIME\n"		"CMD quote STATS u\n\n"\
	"NAME VER\n"			"CMD ctcp %2 VERSION\n\n"\
	"NAME VERSION\n"		"CMD ctcp %2 VERSION\n\n"\
	"NAME WALLOPS\n"		"CMD quote WALLOPS :&2\n\n"\
        "NAME WI\n"                     "CMD quote WHOIS %2\n\n"\
	"NAME WII\n"			"CMD quote WHOIS %2 %2\n\n";

static const char defaultconf_urlhandlers[] =
		"NAME Open Link in a new Firefox Window\n"		"CMD !firefox -new-window %s\n\n";

#ifdef USE_SIGACTION
/* Close and open log files on SIGUSR1. Usefull for log rotating */

static void 
sigusr1_handler (int signal, siginfo_t *si, void *un)
{
	GSList *list = sess_list;
	session *sess;

	while (list)
	{
		sess = static_cast<session*>(list->data);
		log_open_or_close (sess);
		list = list->next;
	}
}

/* Execute /SIGUSR2 when SIGUSR2 received */

static void
sigusr2_handler (int signal, siginfo_t *si, void *un)
{
	session *sess = current_sess;

	if (sess)
		handle_command (sess, "SIGUSR2", FALSE);
}
#endif

static gint
xchat_auto_connect (gpointer userdata)
{
	servlist_auto_connect (nullptr);
	return 0;
}

static void
xchat_init (void)
{
	char buf[3068];
	const char *cs = nullptr;

#ifdef WIN32
	WSADATA wsadata;

#ifdef USE_IPV6
	if (WSAStartup(MAKEWORD(2, 2), &wsadata) != 0)
	{
		MessageBoxW (nullptr, L"Cannot find winsock 2.2+", L"Error", MB_OK);
		exit (0);
	}
#else
	WSAStartup(0x0101, &wsadata);
#endif	/* !USE_IPV6 */
#endif	/* !WIN32 */

#ifdef USE_SIGACTION
	struct sigaction act;

	/* ignore SIGPIPE's */
	act.sa_handler = SIG_IGN;
	act.sa_flags = 0;
	sigemptyset (&act.sa_mask);
	sigaction (SIGPIPE, &act, nullptr);

	/* Deal with SIGUSR1's & SIGUSR2's */
	act.sa_sigaction = sigusr1_handler;
	act.sa_flags = 0;
	sigemptyset (&act.sa_mask);
	sigaction (SIGUSR1, &act, nullptr);

	act.sa_sigaction = sigusr2_handler;
	act.sa_flags = 0;
	sigemptyset (&act.sa_mask);
	sigaction (SIGUSR2, &act, nullptr);
#else
#ifndef WIN32
	/* good enough for these old systems */
	signal (SIGPIPE, SIG_IGN);
#endif
#endif

	if (g_get_charset (&cs))
		prefs.utf8_locale = TRUE;

	load_text_events ();
	sound_load ();
	notify_load ();
	ignore_load ();

	snprintf (buf, sizeof (buf),
		"NAME %s~%s~\n"				"CMD query %%s\n\n"\
		"NAME %s~%s~\n"				"CMD send %%s\n\n"\
		"NAME %s~%s~\n"				"CMD whois %%s %%s\n\n"\
		"NAME %s~%s~\n"				"CMD notify -n ASK %%s\n\n"\
		"NAME %s~%s~\n"				"CMD ignore %%s!*@* ALL\n\n"\

		"NAME SUB\n"					"CMD %s\n\n"\
			"NAME %s\n"					"CMD op %%a\n\n"\
			"NAME %s\n"					"CMD deop %%a\n\n"\
			"NAME SEP\n"				"CMD \n\n"\
			"NAME %s\n"					"CMD voice %%a\n\n"\
			"NAME %s\n"					"CMD devoice %%a\n"\
			"NAME SEP\n"				"CMD \n\n"\
			"NAME SUB\n"				"CMD %s\n\n"\
				"NAME %s\n"				"CMD kick %%s\n\n"\
				"NAME %s\n"				"CMD ban %%s\n\n"\
				"NAME SEP\n"			"CMD \n\n"\
				"NAME %s *!*@*.host\n""CMD ban %%s 0\n\n"\
				"NAME %s *!*@domain\n""CMD ban %%s 1\n\n"\
				"NAME %s *!*user@*.host\n""CMD ban %%s 2\n\n"\
				"NAME %s *!*user@domain\n""CMD ban %%s 3\n\n"\
				"NAME SEP\n"			"CMD \n\n"\
				"NAME %s *!*@*.host\n""CMD kickban %%s 0\n\n"\
				"NAME %s *!*@domain\n""CMD kickban %%s 1\n\n"\
				"NAME %s *!*user@*.host\n""CMD kickban %%s 2\n\n"\
				"NAME %s *!*user@domain\n""CMD kickban %%s 3\n\n"\
			"NAME ENDSUB\n"			"CMD \n\n"\
		"NAME ENDSUB\n"				"CMD \n\n",

		_("_Open Dialog Window"), "gtk-go-up",
		_("_Send a File"), "gtk-floppy",
		_("_User Info (WhoIs)"), "gtk-info",
		_("_Add to Friends List"), "gtk-add",
		_("_Ignore"), "gtk-stop",
		_("O_perator Actions"),

		_("Give Ops"),
		_("Take Ops"),
		_("Give Voice"),
		_("Take Voice"),

		_("Kick/Ban"),
		_("Kick"),
		_("Ban"),
		_("Ban"),
		_("Ban"),
		_("Ban"),
		_("Ban"),
		_("KickBan"),
		_("KickBan"),
		_("KickBan"),
		_("KickBan"));

	list_loadconf ("popup.conf", &popup_list, buf);

	snprintf (buf, sizeof (buf),
		"NAME %s\n"				"CMD part\n\n"
		"NAME %s\n"				"CMD getstr # join \"%s\"\n\n"
		"NAME %s\n"				"CMD quote LINKS\n\n"
		"NAME %s\n"				"CMD ping\n\n"
		"NAME TOGGLE %s\n"	"CMD irc_hide_version\n\n",
				_("Leave Channel"),
				_("Join Channel..."),
				_("Enter Channel to Join:"),
				_("Server Links"),
				_("Ping Server"),
				_("Hide Version"));
	list_loadconf ("usermenu.conf", &usermenu_list, buf);

	snprintf (buf, sizeof (buf),
		"NAME %s\n"		"CMD op %%a\n\n"
		"NAME %s\n"		"CMD deop %%a\n\n"
		"NAME %s\n"		"CMD ban %%s\n\n"
		"NAME %s\n"		"CMD getstr \"%s\" \"kick %%s\" \"%s\"\n\n"
		"NAME %s\n"		"CMD send %%s\n\n"
		"NAME %s\n"		"CMD query %%s\n\n",
				_("Op"),
				_("DeOp"),
				_("Ban"),
				_("Kick"),
				_("bye"),
				_("Enter reason to kick %s:"),
				_("Sendfile"),
				_("Dialog"));
	list_loadconf ("buttons.conf", &button_list, buf);

	snprintf (buf, sizeof (buf),
		"NAME %s\n"				"CMD whois %%s %%s\n\n"
		"NAME %s\n"				"CMD send %%s\n\n"
		"NAME %s\n"				"CMD dcc chat %%s\n\n"
		"NAME %s\n"				"CMD clear\n\n"
		"NAME %s\n"				"CMD ping %%s\n\n",
				_("WhoIs"),
				_("Send"),
				_("Chat"),
				_("Clear"),
				_("Ping"));
	list_loadconf ("dlgbuttons.conf", &dlgbutton_list, buf);

	list_loadconf ("tabmenu.conf", &tabmenu_list, nullptr);
	list_loadconf ("ctcpreply.conf", &ctcp_list, defaultconf_ctcp);
	list_loadconf ("commands.conf", &command_list, defaultconf_commands);
	list_loadconf ("replace.conf", &replace_list, defaultconf_replace);
	list_loadconf ("urlhandlers.conf", &urlhandler_list,
						defaultconf_urlhandlers);

	servlist_init ();							/* load server list */

	/* if we got a URL, don't open the server list GUI */
	if (!prefs.hex_gui_slist_skip && !arg_url && !arg_urls)
		fe_serverlist_open (nullptr);

	/* turned OFF via -a arg or by passing urls */
	if (!arg_dont_autoconnect && !arg_urls)
	{
		/* do any auto connects */
		if (!servlist_have_auto ())	/* if no new windows open .. */
		{
			/* and no serverlist gui ... */
			if (prefs.hex_gui_slist_skip || arg_url || arg_urls)
				/* we'll have to open one. */
				new_ircwindow(nullptr, nullptr, session::SESS_SERVER, 0);
		} else
		{
			fe_idle_add (xchat_auto_connect, nullptr);
		}
	} else
	{
		if (prefs.hex_gui_slist_skip || arg_url || arg_urls)
			new_ircwindow(nullptr, nullptr, session::SESS_SERVER, 0);
	}
}

void
hexchat_exit (void)
{
	hexchat_is_quitting = true;
	in_hexchat_exit = true;
	plugin_kill_all ();
	fe_cleanup ();

	save_config ();
	if (prefs.save_pevents)
	{
		pevent_save (nullptr);
	}

	sound_save ();
	notify_save ();
	ignore_save ();
	free_sessions ();
	chanopt_save_all ();
	servlist_cleanup ();
	fe_exit ();
}

void
hexchat_exec (const char *cmd)
{
	util_exec (cmd);
}


static void
set_locale (void)
{
#ifdef WIN32
	char hexchat_lang[13] = { 0 };	/* LC_ALL= plus 5 chars of hex_gui_lang and trailing \0 */

	strcpy (hexchat_lang, "LC_ALL=");

	if (0 <= prefs.hex_gui_lang && prefs.hex_gui_lang < LANGUAGES_LENGTH)
		strcat (hexchat_lang, languages[prefs.hex_gui_lang]);
	else
		strcat (hexchat_lang, "en");

	putenv (hexchat_lang);

	// Create and install global locale
	std::locale::global(boost::locale::generator().generate(""));
	// Make boost.filesystem use it
	boost::filesystem::path::imbue(std::locale());

#endif
}

int
main (int argc, char *argv[])
{
	int i;
	int ret;

	srand (time (0));	/* CL: do this only once! */

	/* We must check for the config dir parameter, otherwise load_config() will behave incorrectly.
	 * load_config() must come before fe_args() because fe_args() calls gtk_init() which needs to
	 * know the language which is set in the config. The code below is copy-pasted from fe_args()
	 * for the most part. */
	if (argc >= 2)
	{
		for (i = 1; i < argc; i++)
		{
			if ((strcmp (argv[i], "-d") == 0 || strcmp (argv[i], "--cfgdir") == 0)
				&& i + 1 < argc)
			{
				xdir = new_strdup (argv[i + 1]);
			}
			else if (strncmp (argv[i], "--cfgdir=", 9) == 0)
			{
				xdir = new_strdup (argv[i] + 9);
			}

			if (xdir != NULL)
			{
				if (xdir[strlen (xdir) - 1] == G_DIR_SEPARATOR)
				{
					xdir[strlen (xdir) - 1] = 0;
				}
				break;
			}
		}
	}

#if ! GLIB_CHECK_VERSION (2, 36, 0)
	g_type_init ();
#endif

	if (check_config_dir () == 0)
	{
		if (load_config () != 0)
			load_default_config ();
	} else
	{
		/* this is probably the first run */
		load_default_config ();
		make_config_dirs ();
		make_dcc_dirs ();
	}

	/* we MUST do this after load_config () AND before fe_init (thus gtk_init) otherwise it will fail */
	set_locale ();

#ifdef SOCKS
	SOCKSinit (argv[0]);
#endif

	ret = fe_args (argc, argv);
	if (ret != -1)
		return ret;
	
#ifdef USE_DBUS
	hexchat_remote ();
#endif

#ifdef USE_LIBPROXY
	libproxy_factory = px_proxy_factory_new();
#endif

	fe_init ();

	/* This is done here because cfgfiles.c is too early in
	* the startup process to use gtk functions. */
	if (g_access (get_xdir (), W_OK) != 0)
	{
		char buf[2048];

		g_snprintf (buf, sizeof(buf),
			_("You do not have write access to %s. Nothing from this session can be saved."),
			get_xdir ());
		fe_message (buf, FE_MSG_ERROR);
	}

#ifndef WIN32
#ifndef __EMX__
	/* OS/2 uses UID 0 all the time */
	if (getuid () == 0)
		fe_message (_("* Running IRC as root is stupid! You should\n"
			      "  create a User Account and use that to login.\n"), FE_MSG_WARN|FE_MSG_WAIT);
#endif
#endif /* !WIN32 */

	xchat_init ();

	fe_main ();

#ifdef USE_LIBPROXY
	px_proxy_factory_free(libproxy_factory);
#endif

#ifdef WIN32
	WSACleanup ();
#endif

	return 0;
}
