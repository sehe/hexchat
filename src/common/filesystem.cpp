/* HexChat
* Copyright (C) 2014 Leetsoftwerx.
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

#include <fcntl.h>
#include <iosfwd>
#include <string>
#include <vector>
#include <boost/filesystem.hpp>
#include <boost/iostreams/device/file_descriptor.hpp>
#include <boost/iostreams/stream.hpp>
#include "charset_helpers.hpp"
#include "filesystem.hpp"
#include "cfgfiles.hpp"

#ifdef WIN32
#define WIN32_LEAN_AND_MEAN
#define STRICT_TYPED_ITEMIDS
#define NOMINMAX
#include <io.h>
#else
#include <unistd.h>
#define HEXCHAT_DIR "hexchat"
#endif

namespace bio = boost::iostreams;
namespace bfs = boost::filesystem;

namespace io
{
	namespace fs
	{
		bfs::path make_config_path(const bfs::path &path)
		{
			static bfs::path config_dir(config::config_dir());
			return config_dir / path;
		}

		bfs::path make_path(const std::string & path)
		{
#ifdef WIN32
			return charset::widen(path);
#else
			return path;
#endif
		}

		boost::filesystem::path make_path(const std::vector<std::string>& segments)
		{
			if (segments.empty())
				return {};
#ifdef WIN32
			bfs::path path(charset::widen(segments.front()));
			for (auto it = segments.cbegin() + 1; it != segments.cend(); ++it)
				path /= charset::widen(*it);
#else
			bfs::path path(segments.front());
			for (auto it = segments.cbegin() + 1; it != segments.cend(); ++it)
				path /= *it;
#endif
			return path;
		}

		bio::file_descriptor
			open_stream(const std::string& file, std::ios_base::openmode flags, int mode, int xof_flags)
		{

			bfs::path file_path = make_path(file);
			if (!(xof_flags & XOF_FULLPATH))
				file_path = make_path(config::config_dir()) / file_path;
			if (xof_flags & XOF_DOMODE)
			{
				create_file_with_mode(file_path, mode);
			}

			return open_stream(file_path, flags);
		}

		boost::iostreams::file_descriptor
			open_stream(const boost::filesystem::path &file_path, std::ios::openmode flags)
		{
#ifdef WIN32
			return bio::file_descriptor(file_path, flags | std::ios::binary);
#else
			return bio::file_descriptor(file_path.string(), flags | std::ios::binary);
#endif
		}

		bool create_file_with_mode(const bfs::path& path, int mode)
		{
			int tfd;
#ifdef WIN32
			tfd = _wopen(path.c_str(), _O_CREAT, mode);
#else
			tfd = open(path.c_str(), O_CREAT, mode);
#endif
			bool succeeded = tfd != -1;
			close(tfd);
			return succeeded;
		}

		bool exists(const std::string & path)
		{
			return bfs::exists(make_path(path));
		}
	}
}