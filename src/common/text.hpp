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

#ifndef HEXCHAT_TEXT_HPP
#define HEXCHAT_TEXT_HPP

#include <string>
#include <ctime>
#include <boost/format/format_fwd.hpp>
#include <boost/utility/string_ref_fwd.hpp>
#include "textenums.h"

/* timestamp is non-zero if we are using server-time */
#define EMIT_SIGNAL_TIMESTAMP(i, sess, a, b, c, d, e, timestamp) \
	text_emit(i, sess, a, b, c, d, timestamp)
#define EMIT_SIGNAL(i, sess, a, b, c, d, e) \
	text_emit(i, sess, a, b, c, d, 0)

struct text_event
{
	const char *name;
	const char * const *help;
	int num_args;
	const char *def;
};

void scrollback_close (session &sess);
void scrollback_load (session &sess);

int text_word_check (char *word, int len);
void PrintText(session *sess, const boost::string_ref & text);
void PrintTextTimeStamp(session *sess, const boost::string_ref & text, time_t timestamp);
void PrintTextf(session * sess, const boost::format & fmt);
void PrintTextf (session *sess, const char *format, ...) G_GNUC_PRINTF (2, 3);
void PrintTextTimeStampf (session *sess, time_t timestamp, const char *format, ...) G_GNUC_PRINTF (3, 4);
void log_close (session &sess);
void log_open_or_close (session *sess);
void load_text_events (void);
void pevent_save (const char file_name[]);
int pevt_build_string(const std::string& input, std::string & output, int &max_arg);
int pevent_load (const char *filename);
void pevent_make_pntevts (void);
int text_color_of(const boost::string_ref & name);
void text_emit (int index, session *sess, char *a, char *b, char *c, char *d,
		time_t timestamp);
int text_emit_by_name (char *name, session *sess, time_t timestamp,
					   char *a, char *b, char *c, char *d);
std::string text_validate (const boost::string_ref &);
gsize get_stamp_str (const char fmt[], time_t tim, char **ret);
void format_event (session *sess, int index, char **args, char *dst, size_t dstsize, unsigned int stripcolor_args);
const char *text_find_format_string (const char name[]);
 
void sound_play (const boost::string_ref & file, bool quiet);
void sound_play_event (int i);
void sound_beep (session *);
void sound_load ();
void sound_save ();


#endif
