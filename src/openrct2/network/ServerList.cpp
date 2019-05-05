/*****************************************************************************
 * Copyright (c) 2014-2019 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "ServerList.h"

#include "../Context.h"
#include "../PlatformEnvironment.h"
#include "../config/Config.h"
#include "../core/FileStream.hpp"
#include "../core/Json.hpp"
#include "../core/Memory.hpp"
#include "../core/Path.hpp"
#include "../core/String.hpp"
#include "../network/Http.h"
#include "../platform/platform.h"
#include "UdpSocket.h"
#include "network.h"

#include <algorithm>
#include <numeric>

using namespace OpenRCT2;
using namespace OpenRCT2::Network;

int32_t ServerListEntry::CompareTo(const ServerListEntry& other) const
{
    const auto& a = *this;
    const auto& b = other;

    // Order by favourite
    if (a.favourite != b.favourite)
    {
        return a.favourite ? -1 : 1;
    }

    // Order by local
    if (a.local != b.local)
    {
        return a.local ? 1 : -1;
    }

    // Then by version
    bool serverACompatible = a.version == network_get_version();
    bool serverBCompatible = b.version == network_get_version();
    if (serverACompatible != serverBCompatible)
    {
        return serverACompatible ? 1 : -1;
    }

    // Then by password protection
    if (a.requiresPassword != b.requiresPassword)
    {
        return a.requiresPassword ? -1 : 1;
    }

    // Then by name
    return String::Compare(a.name, b.name, true);
}

bool ServerListEntry::IsVersionValid() const
{
    return version.empty() || version == network_get_version();
}

std::optional<ServerListEntry> ServerListEntry::FromJson(const json_t* server)
{
    auto port = json_object_get(server, "port");
    auto name = json_object_get(server, "name");
    auto description = json_object_get(server, "description");
    auto requiresPassword = json_object_get(server, "requiresPassword");
    auto version = json_object_get(server, "version");
    auto players = json_object_get(server, "players");
    auto maxPlayers = json_object_get(server, "maxPlayers");
    auto ip = json_object_get(server, "ip");
    auto ip4 = json_object_get(ip, "v4");
    auto addressIp = json_array_get(ip4, 0);

    if (name == nullptr || version == nullptr)
    {
        log_verbose("Cowardly refusing to add server without name or version specified.");
        return {};
    }
    else
    {
        ServerListEntry entry;
        entry.address = String::StdFormat("%s:%d", json_string_value(addressIp), (int32_t)json_integer_value(port));
        entry.name = (name == nullptr ? "" : json_string_value(name));
        entry.description = (description == nullptr ? "" : json_string_value(description));
        entry.version = json_string_value(version);
        entry.requiresPassword = json_is_true(requiresPassword);
        entry.players = (uint8_t)json_integer_value(players);
        entry.maxplayers = (uint8_t)json_integer_value(maxPlayers);
        return entry;
    }
}

void ServerList::Sort()
{
    std::sort(_serverEntries.begin(), _serverEntries.end(), [](const ServerListEntry& a, const ServerListEntry& b) {
        return a.CompareTo(b) > 0;
    });
}

ServerListEntry& ServerList::GetServer(size_t index)
{
    return _serverEntries[index];
}

size_t ServerList::GetCount() const
{
    return _serverEntries.size();
}

void ServerList::Add(const ServerListEntry& entry)
{
    _serverEntries.push_back(entry);
    Sort();
}

void ServerList::AddRange(const std::vector<ServerListEntry>& entries)
{
    _serverEntries.insert(_serverEntries.end(), entries.begin(), entries.end());
    Sort();
}

std::vector<ServerListEntry> ServerList::ReadFavourites()
{
    log_verbose("server_list_read(...)");
    std::vector<ServerListEntry> entries;
    try
    {
        auto env = GetContext()->GetPlatformEnvironment();
        auto path = env->GetFilePath(PATHID::NETWORK_SERVERS);
        if (platform_file_exists(path.c_str()))
        {
            auto fs = FileStream(path, FILE_MODE_OPEN);
            auto numEntries = fs.ReadValue<uint32_t>();
            for (size_t i = 0; i < numEntries; i++)
            {
                ServerListEntry serverInfo;
                serverInfo.address = fs.ReadStdString();
                serverInfo.name = fs.ReadStdString();
                serverInfo.requiresPassword = false;
                serverInfo.description = fs.ReadStdString();
                serverInfo.version = "";
                serverInfo.favourite = true;
                serverInfo.players = 0;
                serverInfo.maxplayers = 0;
                entries.push_back(std::move(serverInfo));
            }
        }
    }
    catch (const std::exception& e)
    {
        log_error("Unable to read server list: %s", e.what());
        entries = std::vector<ServerListEntry>();
    }
    return entries;
}

void ServerList::ReadAndAddFavourites()
{
    _serverEntries.erase(
        std::remove_if(
            _serverEntries.begin(), _serverEntries.end(), [](const ServerListEntry& entry) { return entry.favourite; }),
        _serverEntries.end());
    auto entries = ReadFavourites();
    AddRange(entries);
}

void ServerList::WriteFavourites()
{
    // Save just favourite servers
    std::vector<ServerListEntry> favouriteServers;
    std::copy_if(
        _serverEntries.begin(), _serverEntries.end(), std::back_inserter(favouriteServers),
        [](const ServerListEntry& entry) { return entry.favourite; });
    WriteFavourites(favouriteServers);
}

bool ServerList::WriteFavourites(const std::vector<ServerListEntry>& entries)
{
    log_verbose("server_list_write(%d, 0x%p)", entries.size(), entries.data());

    utf8 path[MAX_PATH];
    platform_get_user_directory(path, nullptr, sizeof(path));
    Path::Append(path, sizeof(path), "servers.cfg");

    try
    {
        auto fs = FileStream(path, FILE_MODE_WRITE);
        fs.WriteValue<uint32_t>((uint32_t)entries.size());
        for (const auto& entry : entries)
        {
            fs.WriteString(entry.address);
            fs.WriteString(entry.name);
            fs.WriteString(entry.description);
        }
        return true;
    }
    catch (const std::exception& e)
    {
        log_error("Unable to write server list: %s", e.what());
        return false;
    }
}

std::future<std::vector<ServerListEntry>> ServerList::FetchLocalServerListAsync()
{
    return std::async([] {
        constexpr auto RECV_DELAY_MS = 10;
        constexpr auto RECV_WAIT_MS = 2000;

        std::vector<ServerListEntry> entries;
        std::string msg = "Are you an OpenRCT2 server?";
        auto udpSocket = CreateUdpSocket();
        auto len = udpSocket->SendData("192.168.1.255", 11754, msg.data(), msg.size());
        if (len != msg.size())
        {
            throw std::runtime_error("Unable to broadcast server query.");
        }

        char buffer[1024]{};
        size_t recievedLen{};
        std::unique_ptr<INetworkEndpoint> endpoint;
        for (int i = 0; i < (RECV_WAIT_MS / RECV_DELAY_MS); i++)
        {
            auto p = udpSocket->ReceiveData(buffer, sizeof(buffer) - 1, &recievedLen, &endpoint);
            if (p == NETWORK_READPACKET_SUCCESS)
            {
                auto sender = endpoint->GetHostname();
                std::printf(">> Recieved packet from %s\n", sender.c_str());
                auto jinfo = Json::FromString(std::string_view(buffer));

                auto ip4 = json_array();
                json_array_append_new(ip4, json_string(sender.c_str()));
                auto ip = json_object();
                json_object_set_new(ip, "v4", ip4);
                json_object_set_new(jinfo, "ip", ip);

                auto entry = ServerListEntry::FromJson(jinfo);
                if (entry.has_value())
                {
                    entry.value().local = true;
                    entries.push_back(entry.value());
                }

                json_decref(jinfo);
            }
            platform_sleep(RECV_DELAY_MS);
        }

        return entries;
    });
}

std::future<std::vector<ServerListEntry>> ServerList::FetchOnlineServerListAsync()
{
#ifdef DISABLE_HTTP
    return std::async(std::launch::deferred, [] { return std::vector<ServerListEntry>(); });
#else
    auto p = std::make_shared<std::promise<std::vector<ServerListEntry>>>();
    auto f = p->get_future();

    std::string masterServerUrl = OPENRCT2_MASTER_SERVER_URL;
    if (!gConfigNetwork.master_server_url.empty())
    {
        masterServerUrl = gConfigNetwork.master_server_url;
    }

    Http::Request request;
    request.url = masterServerUrl;
    request.method = Http::Method::GET;
    request.header["Accept"] = "application/json";
    Http::DoAsync(request, [p](Http::Response& response) -> void {
        json_t* root{};
        try
        {
            if (response.status != Http::Status::OK)
            {
                throw MasterServerException(STR_SERVER_LIST_NO_CONNECTION);
            }

            root = Json::FromString(response.body);
            auto jsonStatus = json_object_get(root, "status");
            if (!json_is_number(jsonStatus))
            {
                throw MasterServerException(STR_SERVER_LIST_INVALID_RESPONSE_JSON_NUMBER);
            }

            auto status = (int32_t)json_integer_value(jsonStatus);
            if (status != 200)
            {
                throw MasterServerException(STR_SERVER_LIST_MASTER_SERVER_FAILED);
            }

            auto jServers = json_object_get(root, "servers");
            if (!json_is_array(jServers))
            {
                throw MasterServerException(STR_SERVER_LIST_INVALID_RESPONSE_JSON_ARRAY);
            }

            std::vector<ServerListEntry> entries;
            auto count = json_array_size(jServers);
            for (size_t i = 0; i < count; i++)
            {
                auto jServer = json_array_get(jServers, i);
                if (json_is_object(jServer))
                {
                    auto entry = ServerListEntry::FromJson(jServer);
                    if (entry.has_value())
                    {
                        entries.push_back(entry.value());
                    }
                }
            }

            p->set_value(entries);
        }
        catch (...)
        {
            p->set_exception(std::current_exception());
        }
        json_decref(root);
    });
    return f;
#endif
}

uint32_t ServerList::GetTotalPlayerCount() const
{
    return std::accumulate(_serverEntries.begin(), _serverEntries.end(), 0, [](uint32_t acc, const ServerListEntry& entry) {
        return acc + entry.players;
    });
}
