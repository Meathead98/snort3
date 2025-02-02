//--------------------------------------------------------------------------
// Copyright (C) 2015-2019 Cisco and/or its affiliates. All rights reserved.
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License Version 2 as published
// by the Free Software Foundation.  You may not use, modify or distribute
// this program under any other version of the GNU General Public License.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//--------------------------------------------------------------------------

// host_tracker.h author Steve Chew <stechew@cisco.com>

#ifndef HOST_TRACKER_H
#define HOST_TRACKER_H

// The HostTracker class holds information known about a host (may be from
// configuration or dynamic discovery).  It provides a thread-safe API to
// set/get the host data.

#include <algorithm>
#include <cstring>
#include <list>
#include <mutex>
#include "framework/counts.h"
#include "main/snort_types.h"
#include "main/thread.h"
#include "network_inspectors/appid/application_ids.h"
#include "protocols/protocol_ids.h"
#include "sfip/sf_ip.h"
#include "target_based/snort_protocols.h"

//  FIXIT-M For now this emulates the Snort++ attribute table.
//  Need to add in host_tracker.h data eventually.

typedef uint16_t Protocol;
typedef uint8_t Policy;

struct HostTrackerStats
{
    PegCount service_adds;
    PegCount service_finds;
    PegCount service_removes;
};

extern THREAD_LOCAL struct HostTrackerStats host_tracker_stats;

struct HostApplicationEntry
{
    Port port = 0;
    Protocol ipproto = 0;
    SnortProtocolId snort_protocol_id = UNKNOWN_PROTOCOL_ID;

    HostApplicationEntry() = default;

    HostApplicationEntry(Protocol ipproto_param, Port port_param, SnortProtocolId protocol_param) :
        port(port_param),
        ipproto(ipproto_param),
        snort_protocol_id(protocol_param)
    {
    }

    inline bool operator==(const HostApplicationEntry& rhs) const
    {
        return ipproto == rhs.ipproto and port == rhs.port;
    }
};

struct AppMapping
{
    Port port;
    Protocol proto;
    AppId appid;
};

class HostTracker
{
private:
    std::mutex host_tracker_lock;     //  Ensure that updates to a
                                      //  shared object are safe.

    //  FIXIT-M do we need to use a host_id instead of SfIp as in sfrna?
    snort::SfIp ip_addr;
    std::vector< AppMapping > app_mappings; 

    //  Policies to apply to this host.
    Policy stream_policy = 0;
    Policy frag_policy = 0;

    std::list<HostApplicationEntry> services;
    std::list<HostApplicationEntry> clients;

public:
    HostTracker()
    {
        memset(&ip_addr, 0, sizeof(ip_addr));
    }

    HostTracker(const snort::SfIp& new_ip_addr)
    {
        std::memcpy(&ip_addr, &new_ip_addr, sizeof(ip_addr));
    }

    snort::SfIp get_ip_addr()
    {
        std::lock_guard<std::mutex> lck(host_tracker_lock);
        return ip_addr;
    }

    void set_ip_addr(const snort::SfIp& new_ip_addr)
    {
        std::lock_guard<std::mutex> lck(host_tracker_lock);
        std::memcpy(&ip_addr, &new_ip_addr, sizeof(ip_addr));
    }

    Policy get_stream_policy()
    {
        std::lock_guard<std::mutex> lck(host_tracker_lock);
        return stream_policy;
    }

    void set_stream_policy(const Policy& policy)
    {
        std::lock_guard<std::mutex> lck(host_tracker_lock);
        stream_policy = policy;
    }

    Policy get_frag_policy()
    {
        std::lock_guard<std::mutex> lck(host_tracker_lock);
        return frag_policy;
    }

    void set_frag_policy(const Policy& policy)
    {
        std::lock_guard<std::mutex> lck(host_tracker_lock);
        frag_policy = policy;
    }

    void add_app_mapping(Port port, Protocol proto, AppId appid)
    {
        std::lock_guard<std::mutex> lck(host_tracker_lock);
        AppMapping app_map = {port, proto, appid};

        app_mappings.push_back(app_map);
    }

    AppId find_app_mapping(Port port, Protocol proto)
    {
        std::lock_guard<std::mutex> lck(host_tracker_lock);
        for (std::vector<AppMapping>::iterator it=app_mappings.begin(); it!=app_mappings.end(); ++it)
        {
            if (it->port == port and it->proto ==proto)
            {
                return it->appid;
            }
        }
        return APP_ID_NONE;
    }

    bool find_else_add_app_mapping(Port port, Protocol proto, AppId appid)
    {
        std::lock_guard<std::mutex> lck(host_tracker_lock);
        for (std::vector<AppMapping>::iterator it=app_mappings.begin(); it!=app_mappings.end(); ++it)
        {
            if (it->port == port and it->proto ==proto)
            {
                return false; 
            }
        }
        AppMapping app_map = {port, proto, appid};

        app_mappings.push_back(app_map);
        return true;
    }

    //  Add host service data only if it doesn't already exist.  Returns
    //  false if entry exists already, and true if entry was added.
    bool add_service(const HostApplicationEntry& app_entry)
    {
        host_tracker_stats.service_adds++;

        std::lock_guard<std::mutex> lck(host_tracker_lock);

        auto iter = std::find(services.begin(), services.end(), app_entry);
        if (iter != services.end())
            return false;   //  Already exists.

        services.push_front(app_entry);
        return true;
    }

    //  Add host service data if it doesn't already exist.  If it does exist
    //  replace the previous entry with the new entry.
    void add_or_replace_service(const HostApplicationEntry& app_entry)
    {
        host_tracker_stats.service_adds++;

        std::lock_guard<std::mutex> lck(host_tracker_lock);

        auto iter = std::find(services.begin(), services.end(), app_entry);
        if (iter != services.end())
            services.erase(iter);

        services.push_front(app_entry);
    }

    //  Returns true and fills in copy of HostApplicationEntry when found.
    //  Returns false when not found.
    bool find_service(Protocol ipproto, Port port, HostApplicationEntry& app_entry)
    {
        HostApplicationEntry tmp_entry(ipproto, port, UNKNOWN_PROTOCOL_ID);
        host_tracker_stats.service_finds++;

        std::lock_guard<std::mutex> lck(host_tracker_lock);

        auto iter = std::find(services.begin(), services.end(), tmp_entry);
        if (iter != services.end())
        {
            app_entry = *iter;
            return true;
        }

        return false;
    }

    //  Removes HostApplicationEntry object associated with ipproto and port.
    //  Returns true if entry existed.  False otherwise.
    bool remove_service(Protocol ipproto, Port port)
    {
        HostApplicationEntry tmp_entry(ipproto, port, UNKNOWN_PROTOCOL_ID);
        host_tracker_stats.service_removes++;

        std::lock_guard<std::mutex> lck(host_tracker_lock);

        auto iter = std::find(services.begin(), services.end(), tmp_entry);
        if (iter != services.end())
        {
            services.erase(iter);
            return true;   //  Assumes only one matching entry.
        }

        return false;
    }
};

#endif

