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

#ifndef HEXCHAT_THROTTLED_CONNECTION_HPP
#define HEXCHAT_THROTTLED_CONNECTION_HPP

#include <cstddef>
#include <memory>
#include <boost/optional.hpp>
#include "tcpfwd.hpp"
#include "irc_client.hpp"

namespace io
{
	namespace irc
	{
		class throttled_connection : public ::irc::detail::filter
		{
			class p_impl;
			std::unique_ptr<p_impl> impl;
		public:
			typedef std::size_t size_type;
			throttled_connection();
			~throttled_connection();

			void input(const std::string & inbound);
			boost::optional<std::string> next();

			size_type queue_length() const;
		};
	}
}

#endif
