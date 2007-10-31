--[[

   Copyright (C) 2007 MySQL AB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

--]]

---
-- read_query() gets the client query before it reaches the server
--
-- @param packet the mysql-packet sent by client
--
-- the packet contains a command-packet:
--  * the first byte the type (e.g. proxy.COM_QUERY)
--  * the argument of the command
--
--   http://forge.mysql.com/wiki/MySQL_Internals_ClientServer_Protocol#Command_Packet
--
-- for a COM_QUERY it is the query itself in plain-text
--
function read_query( packet )
	if string.byte(packet) == proxy.COM_QUERY then
		print("we got a normal query: " .. string.sub(packet, 2))
	end
end
