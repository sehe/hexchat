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
#define WANTSOCKET
#include "inet.hpp"				/* make it first to avoid macro redefinitions */

#define __APPLE_API_STRICT_CONFORMANCE

#define _FILE_OFFSET_BITS 64

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <iterator>
#include <locale>
#include <string>
#include <sstream>
#include <unordered_map>

#include <sys/types.h>
#include <sys/stat.h>
#include <boost/chrono.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/regex.hpp>
#include <boost/format.hpp>

#ifdef WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <sys/timeb.h>
#include <io.h>
#include <VersionHelpers.h>
#else
#include <unistd.h>
#include <pwd.h>
#include <sys/time.h>
#include <sys/utsname.h>
#endif

#include "../../config.h"
#include <fcntl.h>
#include <cerrno>
#include "hexchat.hpp"
#include "hexchatc.hpp"
#include "util.hpp"
#include "charset_helpers.hpp"
#include "base64.hpp"

#if defined (USING_FREEBSD) || defined (__APPLE__)
#include <sys/sysctl.h>
#endif
#ifdef SOCKS
#include <socks.h>
#endif

/* SASL mechanisms */
#ifdef USE_OPENSSL
#ifndef WIN32
#include <netinet/in.h>
#endif
#endif

#ifndef HAVE_SNPRINTF
#define snprintf g_snprintf
#endif

char *
file_part (char *file)
{
	char *filepart = file;
	if (!file)
		return "";
	while (1)
	{
		switch (*file)
		{
			case 0:
				return (filepart);
			case '/':
#ifdef WIN32
			case '\\':
#endif
				filepart = file + 1;
				break;
		}
		file++;
	}
}

void
path_part (char *file, char *path, int pathlen)
{
	unsigned char t;
	char *filepart = file_part (file);
	t = *filepart;
	*filepart = 0;
	safe_strcpy (path, file, pathlen);
	*filepart = t;
}

char *				/* like strstr(), but nocase */
nocasestrstr (const char *s, const char *wanted)
{
	register const int len = strlen (wanted);

	if (len == 0)
		return (char *)s;
	while (rfc_tolower(*s) != rfc_tolower(*wanted) || g_ascii_strncasecmp (s, wanted, len))
		if (*s++ == '\0')
			return (char *)NULL;
	return (char *)s;
}

char *
errorstring (int err)
{
	switch (err)
	{
	case -1:
		return "";
	case 0:
		return _("Remote host closed socket");
#ifndef WIN32
	}
#else
	case WSAECONNREFUSED:
		return _("Connection refused");
	case WSAENETUNREACH:
	case WSAEHOSTUNREACH:
		return _("No route to host");
	case WSAETIMEDOUT:
		return _("Connection timed out");
	case WSAEADDRNOTAVAIL:
		return _("Cannot assign that address");
	case WSAECONNRESET:
		return _("Connection reset by peer");
	}

	/* can't use strerror() on Winsock errors! */
	if (err >= WSABASEERR)
	{
		static char fbuf[384];
		std::wstring tbuf(384, '\0');

		/* FormatMessage works on WSA*** errors starting from Win2000 */
		if (IsWindowsXPOrGreater())
		{
			if (FormatMessageW (FORMAT_MESSAGE_FROM_SYSTEM |
									  FORMAT_MESSAGE_IGNORE_INSERTS |
									  FORMAT_MESSAGE_MAX_WIDTH_MASK,
									  NULL, err,
									  MAKELANGID (LANG_NEUTRAL, SUBLANG_DEFAULT),
									  &tbuf[0], tbuf.size(), NULL))
			{
				/* now convert to utf8 */
				auto utf8 = charset::narrow(tbuf);
				/* remove the cr-lf if present */ 
				auto crlf = utf8.find_first_of("\r\n");

				if (crlf != std::string::npos)
					utf8.erase(crlf);

				std::copy(utf8.cbegin(), utf8.cend(), std::begin(fbuf));
				return fbuf;
				}
		}	/* ! if (osvi.dwMajorVersion >= 5) */

		/* fallback to error number */
		sprintf (fbuf, "%s %d", _("Error"), err);
		return fbuf;
	} /* ! if (err >= WSABASEERR) */
#endif	/* ! WIN32 */

	return strerror (err);
}

int
waitline (int sok, char *buf, int bufsize, int use_recv)
{
	int i = 0;

	while (1)
	{
		if (use_recv)
		{
			if (recv (sok, &buf[i], 1, 0) < 1)
				return -1;
		} else
		{
			if (read (sok, &buf[i], 1) < 1)
				return -1;
		}
		if (buf[i] == '\n' || bufsize == i + 1)
		{
			buf[i] = 0;
			return i;
		}
		i++;
	}
}

#ifdef WIN32
/* waitline2 using win32 file descriptor and glib instead of _read. win32 can't _read() sok! */
int
waitline2 (GIOChannel *source, char *buf, int bufsize)
{
	int i = 0;
	gsize len;
	GError *error = NULL;

	while (1)
	{
		g_io_channel_set_buffered (source, FALSE);
		g_io_channel_set_encoding (source, NULL, &error);

		if (g_io_channel_read_chars (source, &buf[i], 1, &len, &error) != G_IO_STATUS_NORMAL)
		{
			return -1;
		}
		if (buf[i] == '\n' || bufsize == i + 1)
		{
			buf[i] = 0;
			return i;
		}
		i++;
	}
}
#endif

/* checks for "~" in a file and expands */

char *
expand_homedir (char *file)
{
#ifndef WIN32
	char *user;
	struct passwd *pw;

	if (file[0] == '~')
	{
		if (file[1] == '\0' || file[1] == '/')
			return g_strconcat (g_get_home_dir (), &file[1], NULL);

		char *slash_pos;

		user = g_strdup(file);

		slash_pos = strchr(user, '/');
		if (slash_pos != NULL)
			*slash_pos = '\0';

		pw = getpwnam(user + 1);
		g_free(user);

		if (pw == NULL)
			return g_strdup(file);

		slash_pos = strchr(file, '/');
		if (slash_pos == NULL)
			return g_strdup (pw->pw_dir);
		else
			return g_strconcat (pw->pw_dir, slash_pos, NULL);
	}
#endif
	return g_strdup (file);
}

std::string strip_color(const std::string &text, strip_flags flags)
{
	auto new_str = strip_color2 (text, flags);

	if (flags & STRIP_ESCMARKUP)
	{
		glib_string esc(g_markup_escape_text (new_str.c_str(), -1));
		return std::string(esc.get());
	}

	return new_str;
}


std::string 
strip_color2(const std::string & src, strip_flags flags)
{
	int rcol = 0, bgcol = 0;
	auto src_itr = src.cbegin();
	int len = src.size();
	std::ostringstream dst_stream;
	std::ostream_iterator<char> dst_itr(dst_stream);
	std::locale locale;
	while (len-- > 0)
	{
		if (rcol > 0 && (std::isdigit (*src_itr, locale) ||
			(*src_itr == ',' && std::isdigit(src_itr[1], locale) && !bgcol)))
		{
			if (src_itr[1] != ',') rcol--;
			if (*src_itr == ',')
			{
				rcol = 2;
				bgcol = 1;
			}
		} else
		{
			rcol = bgcol = 0;
			switch (*src_itr)
			{
			case '\003':			  /*ATTR_COLOR: */
				if (!(flags & STRIP_COLOR)) goto pass_char;
				rcol = 2;
				break;
			case HIDDEN_CHAR:	/* CL: invisible text (for event formats only) */	/* this takes care of the topic */
				if (!(flags & STRIP_HIDDEN)) goto pass_char;
				break;
			case '\007':			  /*ATTR_BEEP: */
			case '\017':			  /*ATTR_RESET: */
			case '\026':			  /*ATTR_REVERSE: */
			case '\002':			  /*ATTR_BOLD: */
			case '\037':			  /*ATTR_UNDERLINE: */
			case '\035':			  /*ATTR_ITALICS: */
				if (!(flags & STRIP_ATTRIB)) goto pass_char;
				break;
			default:
			pass_char:
				*dst_itr++ = *src_itr;
			}
		}
		src_itr++;
	}

	return dst_stream.str();
}

int
strip_hidden_attribute(const std::string & src, char *dst)
{
	int len = 0;
	for (char sval : src)
	{
		if (sval != HIDDEN_CHAR)
		{
			*dst++ = sval;
			len++;
		}
	}
	return len;
}

#if defined (USING_LINUX) || defined (USING_FREEBSD) || defined (__APPLE__) || defined (__CYGWIN__)

static void
get_cpu_info (double *mhz, int *cpus)
{

#if defined(USING_LINUX) || defined (__CYGWIN__)

	char buf[256];
	int fh;

	*mhz = 0;
	*cpus = 0;

	fh = open ("/proc/cpuinfo", O_RDONLY);	/* linux 2.2+ only */
	if (fh == -1)
	{
		*cpus = 1;
		return;
	}

	while (1)
	{
		if (waitline (fh, buf, sizeof buf, FALSE) < 0)
			break;
		if (!strncmp (buf, "cycle frequency [Hz]\t:", 22))	/* alpha */
		{
			*mhz = atoi (buf + 23) / 1000000;
		} else if (!strncmp (buf, "cpu MHz\t\t:", 10))	/* i386 */
		{
			*mhz = atof (buf + 11) + 0.5;
		} else if (!strncmp (buf, "clock\t\t:", 8))	/* PPC */
		{
			*mhz = atoi (buf + 9);
		} else if (!strncmp (buf, "processor\t", 10))
		{
			(*cpus)++;
		}
	}
	close (fh);
	if (!*cpus)
		*cpus = 1;

#endif
#ifdef USING_FREEBSD

	int mib[2], ncpu;
	u_long freq;
	size_t len;

	freq = 0;
	*mhz = 0;
	*cpus = 0;

	mib[0] = CTL_HW;
	mib[1] = HW_NCPU;

	len = sizeof(ncpu);
	sysctl(mib, 2, &ncpu, &len, NULL, 0);

	len = sizeof(freq);
	sysctlbyname("machdep.tsc_freq", &freq, &len, NULL, 0);

	*cpus = ncpu;
	*mhz = (freq / 1000000);

#endif
#ifdef __APPLE__

	int mib[2], ncpu;
	unsigned long long freq;
	size_t len;

	freq = 0;
	*mhz = 0;
	*cpus = 0;

	mib[0] = CTL_HW;
	mib[1] = HW_NCPU;

	len = sizeof(ncpu);
	sysctl(mib, 2, &ncpu, &len, NULL, 0);

	len = sizeof(freq);
		sysctlbyname("hw.cpufrequency", &freq, &len, NULL, 0);

	*cpus = ncpu;
	*mhz = (freq / 1000000);

#endif

}
#endif

#ifdef WIN32

static int
get_mhz (void)
{
	HKEY hKey;
	int result, data;

	if (RegOpenKeyExW (HKEY_LOCAL_MACHINE, L"Hardware\\Description\\System\\"
		L"CentralProcessor\\0", 0, KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS)
	{
		DWORD dataSize = sizeof (data);
		result = RegQueryValueExW (hKey, L"~MHz", 0, 0, (LPBYTE)&data, &dataSize);
		RegCloseKey (hKey);
		if (result == ERROR_SUCCESS)
			return data;
	}
	return 0;	/* fails on Win9x */
}

int
get_cpu_arch (void)
{
	SYSTEM_INFO si;

	GetSystemInfo (&si);

	if (si.wProcessorArchitecture == 9)
	{
		return 64;
	}
	else
	{
		return 86;
	}
}

const char * get_sys_str (bool with_cpu)
{
	static std::string sys_str;
	if (!sys_str.empty())
		return sys_str.c_str();
	std::ostringstream buffer("Windows ", std::ios::ate);

	if (IsWindows8Point1OrGreater ())
	{
		if (IsWindowsServer ())
		{
			buffer << "Server 2012 R2";
		}
		else
		{
			buffer << "8.1";
		}
	}
	else if (IsWindows8OrGreater ())
	{
		if (IsWindowsServer ())
		{
			buffer << "Server 2012";
		}
		else
		{
			buffer << "8";
		}
	}
	else if (IsWindows7SP1OrGreater ())
	{
		if (IsWindowsServer ())
		{
			buffer << "Server 2008 R2 SP1";
		}
		else
		{
			buffer << "7 SP1";
		}
	}
	else if (IsWindows7OrGreater ())
	{
		if (IsWindowsServer ())
		{
			buffer << "Server 2008 R2";
		}
		else
		{
			buffer << "7";
		}
	}
	else if (IsWindowsVistaSP2OrGreater ())
	{
		if (IsWindowsServer ())
		{
			buffer << "Server 2008 SP2";
		}
		else
		{
			buffer << "Vista SP2";
		}
	}
	else if (IsWindowsVistaSP1OrGreater ())
	{
		if (IsWindowsServer ())
		{
			buffer << "Server 2008 SP1";
		}
		else
		{
			buffer << "Vista SP1";
		}
	}
	else if (IsWindowsVistaOrGreater ())
	{
		if (IsWindowsServer ())
		{
			buffer << "Server 2008";
		}
		else
		{
			buffer << "Vista";
		}
	}
	else
	{
		buffer << "Unknown";
	}

	double mhz = get_mhz ();
	if (mhz && with_cpu)
	{
		double cpuspeed = ( mhz > 1000 ) ? mhz / 1000 : mhz;
		const char *cpuspeedstr = ( mhz > 1000 ) ? "GHz" : "MHz";
		buffer << boost::format(" [%.2f%s]") % cpuspeed % cpuspeedstr;
	}
	
	sys_str = buffer.str();
	
	return sys_str.c_str();
}

#else

const char * get_sys_str (bool with_cpu)
{
	static std::string buf;
	if (buf.empty())
		return buf.c_str();

	struct utsname un;
	uname (&un);

	std::ostringstream buffer;
#if defined (USING_LINUX) || defined (USING_FREEBSD) || defined (__APPLE__)
	double mhz;
	int cpus = 1;
	get_cpu_info (&mhz, &cpus);
	if (mhz && with_cpu)
	{
		double cpuspeed = ( mhz > 1000 ) ? mhz / 1000 : mhz;
		const char *cpuspeedstr = ( mhz > 1000 ) ? "GHz" : "MHz";
		buffer << boost::format((cpus == 1) ? "%s %s [%s/%.2f%s]" : "%s %s [%s/%.2f%s/SMP]")
			% un.sysname % un.release % un.machine % cpuspeed % cpuspeedstr;
	}
	else
#endif
		buffer << un.sysname << ' ' << un.release;

	buf = buffer.str();

	return buf.c_str();
}

#endif

int
buf_get_line (char *ibuf, char **buf, int *position, int len)
{
	int pos = *position, spos = pos;

	if (pos == len)
		return 0;

	while (ibuf[pos++] != '\n')
	{
		if (pos == len)
			return 0;
	}
	pos--;
	ibuf[pos] = 0;
	*buf = &ibuf[spos];
	pos++;
	*position = pos;
	return 1;
}

void escape_regex(std::string &regex)
{
	boost::replace_all(regex, "\\", "\\\\");
	boost::replace_all(regex, "^", "\\^");
	boost::replace_all(regex, ".", "\\.");
	boost::replace_all(regex, "$", "\\$");
	boost::replace_all(regex, "|", "\\|");
	boost::replace_all(regex, "(", "\\(");
	boost::replace_all(regex, ")", "\\)");
	boost::replace_all(regex, "[", "\\[");
	boost::replace_all(regex, "]", "\\]");
	boost::replace_all(regex, "*", "\\*");
	boost::replace_all(regex, "+", "\\+");
	boost::replace_all(regex, "?", "\\?");
	boost::replace_all(regex, "/", "\\/");
}

bool match_with_wildcards(const std::string &text, std::string wildcardPattern, bool caseSensitive /*= true*/)
{
	// Escape all regex special chars
	escape_regex(wildcardPattern);

	// Convert chars '*?' back to their regex equivalents
	boost::replace_all(wildcardPattern, "\\?", ".");
	boost::replace_all(wildcardPattern, "\\*", ".*");

	boost::regex pattern(wildcardPattern, caseSensitive ? boost::regex::normal : boost::regex::icase);

	return regex_match(text, pattern);
}

bool
match(const char *mask, const char *string)
{
	const char *m = mask, *s = string;
	char ch;
	const char *bm, *bs;		/* Will be reg anyway on a decent CPU/compiler */

	/* Process the "head" of the mask, if any */
	while ((ch = *m++) && (ch != '*'))
	{
		switch (ch)
		{
		case '\\':
			if (*m == '?' || *m == '*')
				ch = *m++;
		default:
			if (rfc_tolower(*s) != rfc_tolower(ch))
				return false;
		case '?':
			if (!*s++)
				return false;
		}
	}
	if (!ch)
		return !(*s);

	/* We got a star: quickly find if/where we match the next char */
got_star:
	bm = m;			/* Next try rollback here */
	while ((ch = *m++))
	{
		switch (ch)
		{
		case '?':
			if (!*s++)
				return false;
		case '*':
			bm = m;
			continue;		/* while */
		case '\\':
			if (*m == '?' || *m == '*')
				ch = *m++;
		default:
			goto break_while;	/* C is structured ? */
		}
	}
break_while:
	if (!ch)
		return true;			/* mask ends with '*', we got it */
	ch = rfc_tolower(ch);
	while (rfc_tolower(*s++) != ch)
		if (!*s)
			return false;
	bs = s;			/* Next try start from here */

	/* Check the rest of the "chunk" */
	while ((ch = *m++))
	{
		switch (ch)
		{
		case '*':
			goto got_star;
		case '\\':
			if (*m == '?' || *m == '*')
				ch = *m++;
		default:
			if (rfc_tolower(*s) != rfc_tolower(ch))
			{
				if (!*s)
					return false;
				m = bm;
				s = bs;
				goto got_star;
			}
		case '?':
			if (!*s++)
				return false;
		}
	}
	if (*s)
	{
		m = bm;
		s = bs;
		goto got_star;
	};
	return true;
}

void
for_files(const char *dirname, const char *mask, const std::function<void(char* file)>& callback)
{
	namespace fs = boost::filesystem;

	fs::path dir(dirname);
	boost::system::error_code ec;
	if (fs::exists(dir, ec) && fs::is_directory(dir, ec))
	{
		for(fs::directory_iterator itr(dir), end; itr != end; ++itr)
		{
			auto file_status = itr->status(ec);
			if (fs::exists(file_status) && fs::is_regular_file(file_status))
			{
				if (match_with_wildcards(itr->path().filename().string(), mask, false))// match (mask, itr->path().filename().string().c_str()))
				{
					std::string mutable_buffer = itr->path().string();
					callback (&mutable_buffer[0]);
				}
			}
		}
	}
}

/*void
tolowerStr (char *str)
{
	while (*str)
	{
		*str = rfc_tolower (*str);
		str++;
	}
}*/

//static int
//country_compare (const void *a, const void *b)
//{
//	return g_ascii_strcasecmp (static_cast<const gchar*>(a), ((domain_t *)b)->code.c_str());
//}

static const std::unordered_map<std::string, std::string> domain =
{ {
		{"AC", N_("Ascension Island") },
		{"AD", N_("Andorra") },
		{"AE", N_("United Arab Emirates") },
		{"AERO", N_("Aviation-Related Fields") },
		{"AF", N_("Afghanistan") },
		{"AG", N_("Antigua and Barbuda") },
		{"AI", N_("Anguilla") },
		{"AL", N_("Albania") },
		{"AM", N_("Armenia") },
		{"AN", N_("Netherlands Antilles") },
		{"AO", N_("Angola") },
		{"AQ", N_("Antarctica") },
		{"AR", N_("Argentina") },
		{"ARPA", N_("Reverse DNS") },
		{"AS", N_("American Samoa") },
		{"ASIA", N_("Asia-Pacific Region") },
		{"AT", N_("Austria") },
		{"ATO", N_("Nato Fiel") },
		{"AU", N_("Australia") },
		{"AW", N_("Aruba") },
		{"AX", N_("Aland Islands") },
		{"AZ", N_("Azerbaijan") },
		{"BA", N_("Bosnia and Herzegovina") },
		{"BB", N_("Barbados") },
		{"BD", N_("Bangladesh") },
		{"BE", N_("Belgium") },
		{"BF", N_("Burkina Faso") },
		{"BG", N_("Bulgaria") },
		{"BH", N_("Bahrain") },
		{"BI", N_("Burundi") },
		{"BIZ", N_("Businesses"), },
		{"BJ", N_("Benin") },
		{"BM", N_("Bermuda") },
		{"BN", N_("Brunei Darussalam") },
		{"BO", N_("Bolivia") },
		{"BR", N_("Brazil") },
		{"BS", N_("Bahamas") },
		{"BT", N_("Bhutan") },
		{"BV", N_("Bouvet Island") },
		{"BW", N_("Botswana") },
		{"BY", N_("Belarus") },
		{"BZ", N_("Belize") },
		{"CA", N_("Canada") },
		{"CAT", N_("Catalan") },
		{"CC", N_("Cocos Islands") },
		{"CD", N_("Democratic Republic of Congo") },
		{"CF", N_("Central African Republic") },
		{"CG", N_("Congo") },
		{"CH", N_("Switzerland") },
		{"CI", N_("Cote d'Ivoire") },
		{"CK", N_("Cook Islands") },
		{"CL", N_("Chile") },
		{"CM", N_("Cameroon") },
		{"CN", N_("China") },
		{"CO", N_("Colombia") },
		{"COM", N_("Internic Commercial") },
		{"COOP", N_("Cooperatives") },
		{"CR", N_("Costa Rica") },
		{"CS", N_("Serbia and Montenegro") },
		{"CU", N_("Cuba") },
		{"CV", N_("Cape Verde") },
		{"CX", N_("Christmas Island") },
		{"CY", N_("Cyprus") },
		{"CZ", N_("Czech Republic") },
		{"DD", N_("East Germany") },
		{"DE", N_("Germany") },
		{"DJ", N_("Djibouti") },
		{"DK", N_("Denmark") },
		{"DM", N_("Dominica") },
		{"DO", N_("Dominican Republic") },
		{"DZ", N_("Algeria") },
		{"EC", N_("Ecuador") },
		{"EDU", N_("Educational Institution") },
		{"EE", N_("Estonia") },
		{"EG", N_("Egypt") },
		{"EH", N_("Western Sahara") },
		{"ER", N_("Eritrea") },
		{"ES", N_("Spain") },
		{"ET", N_("Ethiopia") },
		{"EU", N_("European Union") },
		{"FI", N_("Finland") },
		{"FJ", N_("Fiji") },
		{"FK", N_("Falkland Islands") },
		{"FM", N_("Micronesia") },
		{"FO", N_("Faroe Islands") },
		{"FR", N_("France") },
		{"GA", N_("Gabon") },
		{"GB", N_("Great Britain") },
		{"GD", N_("Grenada") },
		{"GE", N_("Georgia") },
		{"GF", N_("French Guiana") },
		{"GG", N_("British Channel Isles") },
		{"GH", N_("Ghana") },
		{"GI", N_("Gibraltar") },
		{"GL", N_("Greenland") },
		{"GM", N_("Gambia") },
		{"GN", N_("Guinea") },
		{"GOV", N_("Government") },
		{"GP", N_("Guadeloupe") },
		{"GQ", N_("Equatorial Guinea") },
		{"GR", N_("Greece") },
		{"GS", N_("S. Georgia and S. Sandwich Isles") },
		{"GT", N_("Guatemala") },
		{"GU", N_("Guam") },
		{"GW", N_("Guinea-Bissau") },
		{"GY", N_("Guyana") },
		{"HK", N_("Hong Kong") },
		{"HM", N_("Heard and McDonald Islands") },
		{"HN", N_("Honduras") },
		{"HR", N_("Croatia") },
		{"HT", N_("Haiti") },
		{"HU", N_("Hungary") },
		{"ID", N_("Indonesia") },
		{"IE", N_("Ireland") },
		{"IL", N_("Israel") },
		{"IM", N_("Isle of Man") },
		{"IN", N_("India") },
		{"INFO", N_("Informational") },
		{"INT", N_("International") },
		{"IO", N_("British Indian Ocean Territory") },
		{"IQ", N_("Iraq") },
		{"IR", N_("Iran") },
		{"IS", N_("Iceland") },
		{"IT", N_("Italy") },
		{"JE", N_("Jersey") },
		{"JM", N_("Jamaica") },
		{"JO", N_("Jordan") },
		{"JOBS", N_("Company Jobs") },
		{"JP", N_("Japan") },
		{"KE", N_("Kenya") },
		{"KG", N_("Kyrgyzstan") },
		{"KH", N_("Cambodia") },
		{"KI", N_("Kiribati") },
		{"KM", N_("Comoros") },
		{"KN", N_("St. Kitts and Nevis") },
		{"KP", N_("North Korea") },
		{"KR", N_("South Korea") },
		{"KW", N_("Kuwait") },
		{"KY", N_("Cayman Islands") },
		{"KZ", N_("Kazakhstan") },
		{"LA", N_("Laos") },
		{"LB", N_("Lebanon") },
		{"LC", N_("Saint Lucia") },
		{"LI", N_("Liechtenstein") },
		{"LK", N_("Sri Lanka") },
		{"LR", N_("Liberia") },
		{"LS", N_("Lesotho") },
		{"LT", N_("Lithuania") },
		{"LU", N_("Luxembourg") },
		{"LV", N_("Latvia") },
		{"LY", N_("Libya") },
		{"MA", N_("Morocco") },
		{"MC", N_("Monaco") },
		{"MD", N_("Moldova") },
		{"ME", N_("Montenegro") },
		{"MED", N_("United States Medical") },
		{"MG", N_("Madagascar") },
		{"MH", N_("Marshall Islands") },
		{"MIL", N_("Military") },
		{"MK", N_("Macedonia") },
		{"ML", N_("Mali") },
		{"MM", N_("Myanmar") },
		{"MN", N_("Mongolia") },
		{"MO", N_("Macau") },
		{"MOBI", N_("Mobile Devices") },
		{"MP", N_("Northern Mariana Islands") },
		{"MQ", N_("Martinique") },
		{"MR", N_("Mauritania") },
		{"MS", N_("Montserrat") },
		{"MT", N_("Malta") },
		{"MU", N_("Mauritius") },
		{"MUSEUM", N_("Museums") },
		{"MV", N_("Maldives") },
		{"MW", N_("Malawi") },
		{"MX", N_("Mexico") },
		{"MY", N_("Malaysia") },
		{"MZ", N_("Mozambique") },
		{"NA", N_("Namibia") },
		{"NAME", N_("Individual's Names") },
		{"NC", N_("New Caledonia") },
		{"NE", N_("Niger") },
		{"NET", N_("Internic Network") },
		{"NF", N_("Norfolk Island") },
		{"NG", N_("Nigeria") },
		{"NI", N_("Nicaragua") },
		{"NL", N_("Netherlands") },
		{"NO", N_("Norway") },
		{"NP", N_("Nepal") },
		{"NR", N_("Nauru") },
		{"NU", N_("Niue") },
		{"NZ", N_("New Zealand") },
		{"OM", N_("Oman") },
		{"ORG", N_("Internic Non-Profit Organization") },
		{"PA", N_("Panama") },
		{"PE", N_("Peru") },
		{"PF", N_("French Polynesia") },
		{"PG", N_("Papua New Guinea") },
		{"PH", N_("Philippines") },
		{"PK", N_("Pakistan") },
		{"PL", N_("Poland") },
		{"PM", N_("St. Pierre and Miquelon") },
		{"PN", N_("Pitcairn") },
		{"PR", N_("Puerto Rico") },
		{"PRO", N_("Professions") },
		{"PS", N_("Palestinian Territory") },
		{"PT", N_("Portugal") },
		{"PW", N_("Palau") },
		{"PY", N_("Paraguay") },
		{"QA", N_("Qatar") },
		{"RE", N_("Reunion") },
		{"RO", N_("Romania") },
		{"RPA", N_("Old School ARPAnet") },
		{"RS", N_("Serbia") },
		{"RU", N_("Russian Federation") },
		{"RW", N_("Rwanda") },
		{"SA", N_("Saudi Arabia") },
		{"SB", N_("Solomon Islands") },
		{"SC", N_("Seychelles") },
		{"SD", N_("Sudan") },
		{"SE", N_("Sweden") },
		{"SG", N_("Singapore") },
		{"SH", N_("St. Helena") },
		{"SI", N_("Slovenia") },
		{"SJ", N_("Svalbard and Jan Mayen Islands") },
		{"SK", N_("Slovak Republic") },
		{"SL", N_("Sierra Leone") },
		{"SM", N_("San Marino") },
		{"SN", N_("Senegal") },
		{"SO", N_("Somalia") },
		{"SR", N_("Suriname") },
		{"SS", N_("South Sudan") },
		{"ST", N_("Sao Tome and Principe") },
		{"SU", N_("Former USSR") },
		{"SV", N_("El Salvador") },
		{"SY", N_("Syria") },
		{"SZ", N_("Swaziland") },
		{"TC", N_("Turks and Caicos Islands") },
		{"TD", N_("Chad") },
		{"TEL", N_("Internet Communication Services") },
		{"TF", N_("French Southern Territories") },
		{"TG", N_("Togo") },
		{"TH", N_("Thailand") },
		{"TJ", N_("Tajikistan") },
		{"TK", N_("Tokelau") },
		{"TL", N_("East Timor") },
		{"TM", N_("Turkmenistan") },
		{"TN", N_("Tunisia") },
		{"TO", N_("Tonga") },
		{"TP", N_("East Timor") },
		{"TR", N_("Turkey") },
		{"TRAVEL", N_("Travel and Tourism") },
		{"TT", N_("Trinidad and Tobago") },
		{"TV", N_("Tuvalu") },
		{"TW", N_("Taiwan") },
		{"TZ", N_("Tanzania") },
		{"UA", N_("Ukraine") },
		{"UG", N_("Uganda") },
		{"UK", N_("United Kingdom") },
		{"US", N_("United States of America") },
		{"UY", N_("Uruguay") },
		{"UZ", N_("Uzbekistan") },
		{"VA", N_("Vatican City State") },
		{"VC", N_("St. Vincent and the Grenadines") },
		{"VE", N_("Venezuela") },
		{"VG", N_("British Virgin Islands") },
		{"VI", N_("US Virgin Islands") },
		{"VN", N_("Vietnam") },
		{"VU", N_("Vanuatu") },
		{"WF", N_("Wallis and Futuna Islands") },
		{"WS", N_("Samoa") },
		{"XXX", N_("Adult Entertainment") },
		{"YE", N_("Yemen") },
		{"YT", N_("Mayotte") },
		{"YU", N_("Yugoslavia") },
		{"ZA", N_("South Africa") },
		{"ZM", N_("Zambia") },
		{"ZW", N_("Zimbabwe") },
}};

const char *
country (const std::string &hostname)
{
	std::locale loc;

	if (!hostname.empty() || std::isdigit(hostname[hostname.size() - 1], loc))
	{
		return nullptr;
	}
	auto dot_loc = hostname.find_last_of('.');
	std::string host = dot_loc != std::string::npos ? hostname.substr(dot_loc + 1) : hostname;

	boost::algorithm::to_upper(host, loc);
	auto dom = domain.find(host);

	if (dom == domain.cend())
		return nullptr;

	return _(dom->second.c_str());
}

void
country_search (char *pattern, session *ud, void (*print)(session *, const char [], ...))
{
	for (const auto & bucket : domain)
	{
		if (match (pattern, bucket.first.c_str()) || match (pattern, _(bucket.second.c_str())))
		{
			print(ud, "%s = %s\n", bucket.first.c_str(), _(bucket.second.c_str()));
		}
	}
}

void
util_exec (const char *cmd)
{
	g_spawn_command_line_async (cmd, NULL);
}

unsigned long
make_ping_time (void)
{
	auto now = boost::chrono::steady_clock::now();
	auto seconds = boost::chrono::duration_cast<boost::chrono::seconds>(now.time_since_epoch()).count();
	auto usec = boost::chrono::duration_cast<boost::chrono::microseconds>(now.time_since_epoch()).count();
	return static_cast<unsigned long>((seconds - 50000ll) * 1000ll + usec / 1000ll);
}


/************************************************************************
 *    This technique was borrowed in part from the source code to 
 *    ircd-hybrid-5.3 to implement case-insensitive string matches which
 *    are fully compliant with Section 2.2 of RFC 1459, the copyright
 *    of that code being (C) 1990 Jarkko Oikarinen and under the GPL.
 *    
 *    A special thanks goes to Mr. Okarinen for being the one person who
 *    seems to have ever noticed this section in the original RFC and
 *    written code for it.  Shame on all the rest of you (myself included).
 *    
 *        --+ Dagmar d'Surreal
 */

int
rfc_casecmp (const char *s1, const char *s2)
{
	unsigned char *str1 = (unsigned char *) s1;
	unsigned char *str2 = (unsigned char *) s2;
	int res;

	while ((res = rfc_tolower (*str1) - rfc_tolower (*str2)) == 0)
	{
		if (*str1 == '\0')
			return 0;
		str1++;
		str2++;
	}
	return (res);
}

int
rfc_ncasecmp (const char *str1, const char *str2, size_t n)
{
	const unsigned char *s1 = (unsigned char *) str1;
	const unsigned char *s2 = (unsigned char *) str2;
	int res;

	while ((res = rfc_tolower (*s1) - rfc_tolower (*s2)) == 0)
	{
		s1++;
		s2++;
		n--;
		if (n == 0 || (*s1 == '\0' && *s2 == '\0'))
			return 0;
	}
	return (res);
}

namespace
{
	struct rfc_collate : public std::collate < char >
	{
	protected:
		int do_compare(const char * low1, const char * high1,
			const char * low2, const char* high2) const
		{
			return rfc_casecmp(low1, low2);
		}
	};

} // end anonymous namespace

std::locale rfc_locale(const std::locale& locale)
{
	return std::locale(locale, new rfc_collate);
}

const unsigned char rfc_tolowertab[] =
	{ 0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 0x8, 0x9, 0xa,
	0xb, 0xc, 0xd, 0xe, 0xf, 0x10, 0x11, 0x12, 0x13, 0x14,
	0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d,
	0x1e, 0x1f,
	' ', '!', '"', '#', '$', '%', '&', 0x27, '(', ')',
	'*', '+', ',', '-', '.', '/',
	'0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
	':', ';', '<', '=', '>', '?',
	'@', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i',
	'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's',
	't', 'u', 'v', 'w', 'x', 'y', 'z', '{', '|', '}', '~',
	'_',
	'`', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i',
	'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's',
	't', 'u', 'v', 'w', 'x', 'y', 'z', '{', '|', '}', '~',
	0x7f,
	0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
	0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f,
	0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99,
	0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f,
	0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9,
	0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf,
	0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9,
	0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf,
	0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9,
	0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf,
	0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9,
	0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf,
	0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9,
	0xea, 0xeb, 0xec, 0xed, 0xee, 0xef,
	0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9,
	0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff
};

/*static unsigned char touppertab[] =
	{ 0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 0x8, 0x9, 0xa,
	0xb, 0xc, 0xd, 0xe, 0xf, 0x10, 0x11, 0x12, 0x13, 0x14,
	0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d,
	0x1e, 0x1f,
	' ', '!', '"', '#', '$', '%', '&', 0x27, '(', ')',
	'*', '+', ',', '-', '.', '/',
	'0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
	':', ';', '<', '=', '>', '?',
	'@', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I',
	'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S',
	'T', 'U', 'V', 'W', 'X', 'Y', 'Z', '[', '\\', ']', '^',
	0x5f,
	'`', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I',
	'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S',
	'T', 'U', 'V', 'W', 'X', 'Y', 'Z', '[', '\\', ']', '^',
	0x7f,
	0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
	0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f,
	0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99,
	0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f,
	0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9,
	0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf,
	0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9,
	0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf,
	0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9,
	0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf,
	0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9,
	0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf,
	0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9,
	0xea, 0xeb, 0xec, 0xed, 0xee, 0xef,
	0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9,
	0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff
};*/

/*static int
rename_utf8 (char *oldname, char *newname)
{
	int sav, res;
	char *fso, *fsn;

	fso = hexchat_filename_from_utf8 (oldname, -1, 0, 0, 0);
	if (!fso)
		return FALSE;
	fsn = hexchat_filename_from_utf8 (newname, -1, 0, 0, 0);
	if (!fsn)
	{
		g_free (fso);
		return FALSE;
}

	res = rename (fso, fsn);
	sav = errno;
	g_free (fso);
	g_free (fsn);
	errno = sav;
	return res;
}

static int
unlink_utf8 (char *fname)
{
	int res;
	char *fs;

	fs = hexchat_filename_from_utf8 (fname, -1, 0, 0, 0);
	if (!fs)
		return FALSE;

	res = unlink (fs);
	g_free (fs);
	return res;
}*/

static bool
copy_file (const char *dl_src, const char *dl_dest, int permissions)
{
	int tmp_src, tmp_dest;
	bool ok = false;
	char dl_tmp[4096];
	int return_tmp, return_tmp2;

	if ((tmp_src = g_open (dl_src, O_RDONLY | OFLAGS, 0600)) == -1)
	{
		g_fprintf (stderr, "Unable to open() file '%s' (%s) !", dl_src,
				  strerror (errno));
		return false;
	}

	if ((tmp_dest =
		 g_open (dl_dest, O_WRONLY | O_CREAT | O_TRUNC | OFLAGS, permissions)) < 0)
	{
		close (tmp_src);
		g_fprintf (stderr, "Unable to create file '%s' (%s) !", dl_src,
				  strerror (errno));
		return false;
	}

	for (;;)
	{
		return_tmp = read (tmp_src, dl_tmp, sizeof (dl_tmp));

		if (!return_tmp)
		{
			ok = true;
			break;
		}

		if (return_tmp < 0)
		{
			fprintf (stderr, "download_move_to_completed_dir(): "
				"error reading while moving file to save directory (%s)",
				 strerror (errno));
			break;
		}

		return_tmp2 = write (tmp_dest, dl_tmp, return_tmp);

		if (return_tmp2 < 0)
		{
			fprintf (stderr, "download_move_to_completed_dir(): "
				"error writing while moving file to save directory (%s)",
				 strerror (errno));
			break;
		}

		if (return_tmp < sizeof (dl_tmp))
		{
			ok = true;
			break;
		}
	}

	close (tmp_dest);
	close (tmp_src);
	return ok;
}

/* Takes care of moving a file from a temporary download location to a completed location. */
void
move_file (const std::string& src_dir, const std::string & dst_dir, const std::string& fname, int dccpermissions)
{
	/* if dcc_dir and dcc_completed_dir are the same then we are done */
	if (src_dir == dst_dir || dst_dir.empty())
		return;			/* Already in "completed dir" */

	auto file_name = boost::filesystem::path(fname);
	auto src = boost::filesystem::path(src_dir) / file_name;
	auto dst_dir_path = boost::filesystem::path(dst_dir);
	auto dst = dst_dir_path / file_name;

	/* already exists in completed dir? Append a number */
	if (boost::filesystem::exists(dst))
	{
		for (int i = 0; ; i++)
		{
			dst = (dst_dir_path / file_name).replace_extension(std::to_string(i));
			if (!boost::filesystem::exists(dst))
				break;
		}
	}

	/* first try a simple rename move */
	boost::system::error_code ec;
	boost::filesystem::rename(src, dst, ec);

	if (ec && (ec.value() == boost::system::errc::cross_device_link || ec.value() == boost::system::errc::operation_not_permitted))
	{
		/* link failed because either the two paths aren't on the */
		/* same filesystem or the filesystem doesn't support hard */
		/* links, so we have to do a copy. */
		if (copy_file (src.string().c_str(), dst.string().c_str(), dccpermissions))
			g_unlink (src.string().c_str());
		chmod(dst.string().c_str(), dccpermissions);
	}
}

/* separates a string according to a 'sep' char, then calls the callback
   function for each token found */

int
token_foreach (char *str, char sep,
					int (*callback) (char *str, void *ud), void *ud)
{
	char t, *start = str;

	while (1)
	{
		if (*str == sep || *str == 0)
		{
			t = *str;
			*str = 0;
			if (callback (start, ud) < 1)
			{
				*str = t;
				return FALSE;
			}
			*str = t;

			if (*str == 0)
				break;
			str++;
			start = str;

		} else
		{
			/* chars $00-$7f can never be embedded in utf-8 */
			str++;
		}
	}

	return TRUE;
}

/* 31 bit string hash functions */

guint32
str_hash (const char *key)
{
	const char *p = key;
	guint32 h = *p;

	if (h)
		for (p += 1; *p != '\0'; p++)
			h = (h << 5) - h + *p;

	return h;
}

guint32
str_ihash (const unsigned char *key)
{
	const char *p = (const char*)key;
	guint32 h = rfc_tolowertab [(guint)*p];

	if (h)
		for (p += 1; *p != '\0'; p++)
			h = (h << 5) - h + rfc_tolowertab [(guint)*p];

	return h;
}

/* features: 1. "src" must be valid, NULL terminated UTF-8 */
/*           2. "dest" will be left with valid UTF-8 - no partial chars! */

void
safe_strcpy (char *dest, const char *src, std::size_t bytes_left)
{
	int mbl;

	while (1)
	{
		mbl = g_utf8_skip[*((unsigned char *)src)];

		if (bytes_left < (mbl + 1)) /* can't fit with NULL? */
		{
			*dest = 0;
			break;
		}

		if (mbl == 1)	/* one byte char */
		{
			*dest = *src;
			if (*src == 0)
				break;	/* it all fit */
			dest++;
			src++;
			bytes_left--;
		}
		else				/* multibyte char */
		{
			std::copy_n(src, mbl, dest);
			dest += mbl;
			src += mbl;
			bytes_left -= mbl;
		}
	}
}

void
canonalize_key (char *key)
{
	char token;
	std::locale locale;
	for (auto pos = key; (token = *pos) != 0; pos++)
	{
		if (token != '_' && (token < '0' || token > '9') && (token < 'A' || token > 'Z') && (token < 'a' || token > 'z'))
		{
			*pos = '_';
		}
		else
		{
			*pos = std::tolower(token, locale);
		}
	}
}

bool
portable_mode ()
{
#ifdef WIN32
	namespace bfs = boost::filesystem;
	boost::system::error_code ec;
	return bfs::exists("portable-mode", ec);
#else
	return false;
#endif
}

bool
unity_mode (void)
{
#ifdef G_OS_UNIX
	const char *env = g_getenv("XDG_CURRENT_DESKTOP");
	if (env && (strcmp (env, "Unity") == 0
			|| strcmp (env, "Pantheon") == 0))
		return true;
#endif
	return false;
}

char* new_strdup(const char in[], std::size_t len)
{
	std::unique_ptr<char[]> new_str(new char[len + 1]());
	std::copy_n(in, len, new_str.get());
	return new_str.release();
}

char* new_strdup(const char in[])
{
	return new_strdup(in, std::strlen(in));
}

#ifdef USE_OPENSSL
static std::string str_sha256hash (const std::string & string)
{
	unsigned char hash[SHA256_DIGEST_LENGTH];
	char buf[SHA256_DIGEST_LENGTH * 2 + 1];		/* 64 digit hash + '\0' */
	SHA256_CTX sha256;

	SHA256_Init (&sha256);
	SHA256_Update (&sha256, string.c_str(), string.size());
	SHA256_Final (hash, &sha256);

	for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
	{
		sprintf (buf + (i * 2), "%02x", hash[i]);
	}

	buf[SHA256_DIGEST_LENGTH * 2] = 0;

	return buf;
}

/**
 * \brief Generate CHALLENGEAUTH response for QuakeNet login.
 *
 * \param username QuakeNet user name
 * \param password password for the user
 * \param challenge the CHALLENGE response we got from Q
 *
 * After a successful connection to QuakeNet a CHALLENGE is requested from Q.
 * Generate the CHALLENGEAUTH response from this CHALLENGE and our user
 * credentials as per the
 * <a href="http://www.quakenet.org/development/challengeauth">CHALLENGEAUTH</a>
 * docs. As for using OpenSSL HMAC, see
 * <a href="http://www.askyb.com/cpp/openssl-hmac-hasing-example-in-cpp/">example 1</a>,
 * <a href="http://stackoverflow.com/questions/242665/understanding-engine-initialization-in-openssl">example 2</a>.
 */
std::string challengeauth_response(const std::string & username, const std::string &password, const std::string &challenge)
{
	std::string user(username);
	for (auto & c : user)
{
		c = rfc_tolower(c);			/* convert username to lowercase as per the RFC */
	}

	std::string pass(password);
	pass.resize(10, '\0'); /* truncate to 10 characters */
	auto passhash = str_sha256hash (pass.c_str());

	glib_string key(g_strdup_printf("%s:%s", user.c_str(), passhash.c_str()));

	auto keyhash = str_sha256hash (key.get());

	std::vector<unsigned char> digest(EVP_MAX_MD_SIZE);

	unsigned int digest_len = digest.size();
	HMAC (
		EVP_sha256 (),
		keyhash.c_str(),
		keyhash.size(),
		(const unsigned char *) challenge.c_str(),
		challenge.size(),
		&digest[0],
		&digest_len);

	std::ostringstream buf;
	for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i)
	{
		buf << boost::format("%02x") % (unsigned int)digest[i];
	}

	return buf.str();
}
#endif

/**
 * \brief Wrapper around strftime for Windows
 *
 * Prevents crashing when using an invalid format by escaping them.
 *
 * Behaves the same as strftime with the addition that
 * it returns 0 if the escaped format string is too large.
 *
 * Based upon work from znc-msvc project.
 *
 * This assumes format is a locale-encoded string. For utf-8 strings, use strftime_utf8
 */
size_t
strftime_validated (char *dest, size_t destsize, const char *format, const struct tm *time)
{
#ifndef WIN32
	return strftime (dest, destsize, format, time);
#else
	char safe_format[64] = { 0 };
	const char *p = format;
	int i = 0;

	if (strlen (format) >= sizeof(safe_format))
		return 0;

	while (*p)
	{
		if (*p == '%')
		{
			int has_hash = (*(p + 1) == '#');
			char c = *(p + (has_hash ? 2 : 1));

			if (i >= sizeof (safe_format))
				return 0;

			switch (c)
			{
			case 'a': case 'A': case 'b': case 'B': case 'c': case 'd': case 'H': case 'I': case 'j': case 'm': case 'M':
			case 'p': case 'S': case 'U': case 'w': case 'W': case 'x': case 'X': case 'y': case 'Y': case 'z': case 'Z':
			case '%':
				/* formatting code is fine */
				break;
			default:
				/* replace bad formatting code with itself, escaped, e.g. "%V" --> "%%V" */
				g_strlcat (safe_format, "%%", sizeof(safe_format));
				i += 2;
				p++;
				break;
			}

			/* the current loop run will append % (and maybe #) and the next one will do the actual char. */
			if (has_hash)
			{
				safe_format[i] = *p;
				p++;
				i++;
			}
			if (c == '%')
			{
				safe_format[i] = *p;
				p++;
				i++;
			}
		}

		if (*p)
		{
			safe_format[i] = *p;
			p++;
			i++;
		}
	}

	return strftime (dest, destsize, safe_format, time);
#endif
}

/**
 * \brief Similar to strftime except it works with utf-8 formats, since strftime treats the format as locale-encoded.
 */
gsize
strftime_utf8 (char *dest, gsize destsize, const char *format, time_t time)
{
	std::unique_ptr<GDate, decltype(&g_date_free)>date(g_date_new (), g_date_free);
	g_date_set_time_t (date.get(), time);
	auto result = g_date_strftime (dest, destsize, format, date.get());
	return result;
}

std::vector<std::string> to_vector_strings(const char *const in[], size_t len)
{
	if (!in)
		throw std::invalid_argument("inbound list of strings cannot be null");
	std::vector<std::string> ret;
	ret.reserve(len);
	for (size_t i = 0; i < len; ++i)
	{
			ret.emplace_back(in[i] ? std::string(in[i]) : std::string());

	}
	return ret;
}
