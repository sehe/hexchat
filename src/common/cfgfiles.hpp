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

/* cfgfiles.hpp */

#ifndef HEXCHAT_CFGFILES_HPP
#define HEXCHAT_CFGFILES_HPP

#include <cstdio>
#include <string>
#include <boost/iostreams/device/file_descriptor.hpp>
#include <boost/iostreams/stream.hpp>

#include "sessfwd.hpp"
#include "hexchat.hpp"

enum{ LANGUAGES_LENGTH = 53 };

extern char *xdir;
extern const char * const languages[LANGUAGES_LENGTH];
namespace config
{
	const ::std::string& config_dir();
}
char *cfg_get_str (char *cfg, const char *var, char *dest, int dest_len);
int cfg_get_bool (const char *var);
int cfg_get_int_with_result (char *cfg, const char *var, bool &result);
int cfg_get_int (char *cfg, char *var);
int cfg_put_int (int fh, int value, const char *var);
bool cfg_get_color (char *cfg, const char var[], int &r, int &g, int &b);
bool cfg_put_color (int fh, int r, int g, int b, const char var[]);
char *get_xdir (void);
int check_config_dir (void);
void load_default_config (void);
bool make_config_dirs (void);
int make_dcc_dirs (void);
int load_config (void);
bool save_config (void);
void list_free (GSList ** list);
void list_loadconf (const char *file, GSList ** list, const char *defaultconf);
bool list_delentry (GSList ** list,const char name[]);
// deliberately pass by value to force any allocation exceptions to happen before the allocation inside
void list_addentry (GSList ** list, std::string cmd, std::string name);
int cmd_set (session *sess, char *tbuf, char *word[], char *word_eol[]);
int hexchat_open_file (const char *file, int flags, int mode, int xof_flags);
FILE *hexchat_fopen_file (const char *file, const char *mode, int xof_flags);

enum xof {
	XOF_DOMODE = 1,
	XOF_FULLPATH = 2
};

#define STRUCT_OFFSET_STR(type,field) \
( (unsigned int) (((char *) (&(((type *) NULL)->field)))- ((char *) NULL)) )

#define STRUCT_OFFSET_INT(type,field) \
( (unsigned int) (((int *) (&(((type *) NULL)->field)))- ((int *) NULL)) )

#define P_OFFSET(field) STRUCT_OFFSET_STR(struct hexchatprefs, field),sizeof(prefs.field)
#define P_OFFSETNL(field) STRUCT_OFFSET_STR(struct hexchatprefs, field)
#define P_OFFINT(field) STRUCT_OFFSET_INT(struct hexchatprefs, field),0
#define P_OFFINTNL(field) STRUCT_OFFSET_INT(struct hexchatprefs, field)

enum pref_type : unsigned short{
	TYPE_STR,
	TYPE_INT,
	TYPE_BOOL
};

struct prefs
{
	const char *name;
	unsigned int offset;
	unsigned short len;
	pref_type type;
};

#define HEXCHAT_SOUND_DIR "sounds"

#endif
