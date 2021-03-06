/* HexChat
 * Copyright (C) 1998-2010 Peter Zelezny.
 * Copyright (C) 2009-2013 Berke Viktor.
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

/* per-channel/dialog settings :: /CHANOPT */
#ifdef WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/format.hpp>
#include <boost/utility/string_ref.hpp>

#include "hexchat.hpp"
#include "session.hpp"
#include "chanopt.hpp"

#include "text.hpp"
#include "server.hpp"
#include "util.hpp"
#include "hexchatc.hpp"
#include "filesystem.hpp"

namespace {
static bool chanopt_open = false;
static bool chanopt_changed = false;


struct channel_options
{
	const char *name;
	const char *alias;	/* old names from 2.8.4 */
	int offset;
};

#define STRUCT_OFFSET_STR(name, field) offsetof(name, field)
#define S_F(xx) STRUCT_OFFSET_STR(struct session,xx)

static const std::array<channel_options, 7> chanopt =
{ {
	{ "alert_beep",  "BEEP",  S_F(alert_beep)},
	{ "alert_taskbar", nullptr, S_F(alert_taskbar) },
	{ "alert_tray", "TRAY",  S_F(alert_tray) },

	{ "text_hidejoinpart", "CONFMODE", S_F(text_hidejoinpart) },
	{ "text_logging", nullptr, S_F(text_logging) },
	{ "text_scrollback", nullptr, S_F(text_scrollback) },
	{ "text_strip", nullptr, S_F(text_strip) },
}};

#undef S_F

static const char * chanopt_value (std::uint8_t val)
{
	switch (val)
	{
	case SET_OFF:
		return "OFF";
	case SET_ON:
		return "ON";
	default:
		return "{unset}";
	}
}
}
/* handle the /CHANOPT command */

int chanopt_command (session *sess, char *tbuf, char *word[], char *[])
{
	int offset = 2;
	bool quiet = false;

	if (!strcmp (word[2], "-quiet"))
	{
		quiet = true;
		offset++;
	}

	const char* find = word[offset++];
	int newval = -1;
	if (word[offset][0])
	{
		if (!g_ascii_strcasecmp (word[offset], "ON"))
			newval = 1;
		else if (!g_ascii_strcasecmp (word[offset], "OFF"))
			newval = 0;
		else if (word[offset][0] == 'u')
			newval = SET_DEFAULT;
		else
			newval = atoi (word[offset]);
	}

	if (!quiet)
		PrintTextf (sess, "\002Network\002: %s \002Channel\002: %s\n",
						sess->server->network ? sess->server->get_network (true).data() : _("<none>"),
						sess->channel[0] ? sess->channel : _("<none>"));

	for(const auto & op : chanopt)
	{
		if (find[0] == 0 || match(find, op.name) || (op.alias && match(find, op.alias)))
		{
			if (newval != -1)	/* set new value */
			{
				*(std::uint8_t*)G_STRUCT_MEMBER_P(sess, op.offset) = newval;
				chanopt_changed = true;
			}

			if (!quiet)	/* print value */
			{
				std::ostringstream buf;
				buf << op.name;
				char t = 3;
				buf.write(&t, 1);
				buf << '2';

				auto dots = 20 - std::strlen(op.name);
				std::ostream_iterator<char> itr(buf);
				for (size_t j = 0; j < dots; j++)
					*itr++ = '.';

				auto val = G_STRUCT_MEMBER(std::uint8_t, sess, op.offset);
				PrintTextf(sess, boost::format("%s\0033:\017 %s") % buf.str() % chanopt_value(val));
			}
		}
	}

	return TRUE;
}

/* is a per-channel setting set? Or is it UNSET and
 * the global version is set? */

gboolean
chanopt_is_set (unsigned int global, guint8 per_chan_setting)
{
	if (per_chan_setting == SET_ON || per_chan_setting == SET_OFF)
		return per_chan_setting;
	else
		return global;
}

namespace {
/* === below is LOADING/SAVING stuff only === */

struct chanopt_in_memory
{
	/* Per-Channel Alerts */
	/* use a byte, because we need a pointer to each element */
	std::uint8_t alert_beep;
	std::uint8_t alert_taskbar;
	std::uint8_t alert_tray;

	/* Per-Channel Settings */
	std::uint8_t text_hidejoinpart;
	std::uint8_t text_logging;
	std::uint8_t text_scrollback;
	std::uint8_t text_strip;

	std::string network;
	std::string channel;

	chanopt_in_memory()
		:alert_beep(SET_DEFAULT),
		alert_taskbar(SET_DEFAULT),
		alert_tray(SET_DEFAULT),
		text_hidejoinpart(SET_DEFAULT),
		text_logging(SET_DEFAULT),
		text_scrollback(SET_DEFAULT),
		text_strip(SET_DEFAULT){}
	chanopt_in_memory(std::string network, const std::string & channel)
		:alert_beep(SET_DEFAULT),
		alert_taskbar(SET_DEFAULT),
		alert_tray(SET_DEFAULT),
		text_hidejoinpart(SET_DEFAULT),
		text_logging(SET_DEFAULT),
		text_scrollback(SET_DEFAULT),
		text_strip(SET_DEFAULT),
		network(network),
		channel(channel)
	{
	}
	friend std::istream& operator>> (std::istream& i, chanopt_in_memory& chanop);
	friend std::ostream& operator<< (std::ostream& o, const chanopt_in_memory& chanop);
};

/* network = <network name>
 * channel = <channel name>
 * alert_taskbar = <1/0>
 */
std::istream& 
operator>> (std::istream& i, chanopt_in_memory& chanop)
{
	chanop = chanopt_in_memory();
	// get network
	std::string line;
	if (!std::getline(i, line, '\n'))
		return i;
	do{
		auto loc_eq = line.find_first_of('=');
		if (loc_eq == std::string::npos) // data corruption
			return i;

		std::string first_part, second_part = line.substr(loc_eq + 2);
		if (loc_eq && line[loc_eq - 1] == ' ')
			first_part = line.substr(0, loc_eq - 1);

		if (first_part == "network")
			chanop.network = second_part;
		else if (first_part == "channel")
			chanop.channel = second_part;
		else
		{
			int value = std::stoi(second_part);
			for (const auto & op : chanopt)
			{
				if (first_part == op.name || (op.alias && first_part == op.alias))
				{
					*(std::uint8_t *)G_STRUCT_MEMBER_P(&chanop, op.offset) = value;
					break;
				}
			}
		}
		// if the next character is n it's a network and we shouldn't continue
	} while (i.peek() != 'n' && std::getline(i, line, '\n'));
	
	/* we should always leave the stream in a good state if we got this far
	 * otherwise the reader will assume we've failed even though we haven't
	 */
	if (!i)
	{
		i.clear();
	}
	return i;
}

std::ostream&
operator<< (std::ostream& o, const chanopt_in_memory& chanop)
{
	bool something_saved = false;
	std::ostringstream buffer;
	buffer << "network = " << chanop.network << "\n";
	buffer << "channel = " << chanop.channel << "\n";
	for (const auto& op : chanopt){
		std::uint8_t val = G_STRUCT_MEMBER(std::uint8_t, &chanop, op.offset);
		if (val != SET_DEFAULT)
		{
			buffer << op.name << " = " << std::to_string(val) << "\n";
			something_saved = true;
		}
	}
	if (something_saved)
		o << buffer.str();
	return o;
}

static std::vector<chanopt_in_memory> chanopts;

static std::vector<chanopt_in_memory>::iterator 
chanopt_find (const boost::string_ref & network, const std::string& channel)
{
	return std::find_if(
		chanopts.begin(), chanopts.end(),
		[&network, &channel](const chanopt_in_memory& c){
		return !g_ascii_strcasecmp(c.channel.c_str(), channel.c_str()) &&
			!g_ascii_strcasecmp(c.network.c_str(), network.data());
		});
}

/* load chanopt.conf from disk into our chanopt_list GSList */

static void
chanopt_load_all (void)
{
	namespace bfs = boost::filesystem;
	bfs::ifstream stream(io::fs::make_config_path("chanopt.conf"), std::ios::in | std::ios::binary);
	for (chanopt_in_memory current; stream >> current;)
	{
		chanopts.push_back(current);
		chanopt_changed = true;
	}
}
}

void
chanopt_load (session *sess)
{
	if (sess->name.empty())
		return;

	auto network = sess->server->get_network(false);
	if (network.empty())
		return;

	if (!chanopt_open)
	{
		chanopt_open = true;
		chanopt_load_all ();
	}

	auto itr = chanopt_find (network, sess->name);
	if (itr == chanopts.end())
		return;

	/* fill in all the sess->xxxxx fields */
	for (const auto & op : chanopt)
	{
		auto val = G_STRUCT_MEMBER(std::uint8_t, &(*itr), op.offset);
		*(std::uint8_t *)G_STRUCT_MEMBER_P(sess, op.offset) = val;
	}
}

void
chanopt_save (session *sess)
{
	if (sess->name.empty())
		return;

	auto network = sess->server->get_network(FALSE);
	if (network.empty())
		return;

	/* 2. reconcile sess with what we loaded from disk */
	auto itr = chanopt_find (network, sess->name);
	auto co = itr != chanopts.end() ? *itr : chanopt_in_memory(network.to_string(), sess->name);

	for (const auto& op : chanopt)
	{
		auto vals = G_STRUCT_MEMBER(std::uint8_t, sess, op.offset);
		auto valm = G_STRUCT_MEMBER(std::uint8_t, &co, op.offset);

		if (vals != valm)
		{
			*(std::uint8_t *)G_STRUCT_MEMBER_P(&co, op.offset) = vals;
			chanopt_changed = true;
		}
	}
	if (itr == chanopts.end())
	{
		chanopts.push_back(co);
		chanopt_changed = true;
	}
	else
	{
		*itr = co;
	}
}

void
chanopt_save_all (void)
{
	namespace bfs = boost::filesystem;
	if (chanopts.empty() || !chanopt_changed)
	{
		return;
	}

	auto file_path = io::fs::make_config_path("chanopt.conf");
	io::fs::create_file_with_mode(file_path, 0600);
	bfs::ofstream stream(file_path, std::ios::trunc | std::ios::out | std::ios::binary);
	for (const auto& co : chanopts)
	{
		stream << co;
	}
	stream.flush();
	chanopt_open = false;
	chanopt_changed = false;
}
