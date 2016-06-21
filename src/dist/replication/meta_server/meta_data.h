#pragma once

#include <memory>
#include <set>
#include <deque>
#include <vector>
#include <map>
#include <unordered_map>
#include <dsn/cpp/utils.h>
#include <dsn/dist/replication/replication_types.h>
#include <dsn/dist/replication/replication_other_types.h>
#include <dsn/cpp/json_helper.h>

namespace dsn { namespace replication {

enum class config_status
{
    not_pending,
    pending_proposal,
    pending_remote_sync,
    invalid_status
};

ENUM_BEGIN(config_status, config_status::invalid_status)
    ENUM_REG(config_status::not_pending)
    ENUM_REG(config_status::pending_proposal)
    ENUM_REG(config_status::pending_remote_sync)
ENUM_END(config_status)

enum class pc_status
{
    healthy,
    ill,
    dead,
    invalid
};

ENUM_BEGIN(pc_status, pc_status::invalid)
    ENUM_REG(pc_status::healthy)
    ENUM_REG(pc_status::ill)
    ENUM_REG(pc_status::dead)
ENUM_END(pc_status)

struct dropped_server
{
    rpc_address node;
    uint64_t time;
};

struct config_context
{
public:
    config_status stage;
    //for server state's update config management
    //[
    task_ptr pending_sync_task;
    dsn_message_t msg;
    //]

    //for load balancer's decision
    //[
    std::shared_ptr<configuration_balancer_request> balancer_proposal;
    std::deque<dropped_server> history;
    //]
public:
    bool empty_balancer_proposals() const { return balancer_proposal==nullptr || balancer_proposal->action_list.empty(); }
    void cancel_sync();
    void clear_proposal();
};

struct partition_configuration_stateless
{
    partition_configuration& config;
    partition_configuration_stateless(partition_configuration& pc): config(pc) {}
    std::vector<dsn::rpc_address>& workers() { return config.last_drops; }
    std::vector<dsn::rpc_address>& hosts() { return config.secondaries; }
    bool is_host(const rpc_address& node) const
    {
        return std::find(config.secondaries.begin(), config.secondaries.end(), node)!=config.secondaries.end();
    }
    bool is_worker(const rpc_address& node) const
    {
        return std::find(config.last_drops.begin(), config.last_drops.end(), node)!=config.last_drops.end();
    }
    bool is_member(const rpc_address& node) const
    {
        return is_host(node) || is_worker(node);
    }
};

class app_state;
class app_state_helper
{
public:
    app_state* owner;
    std::atomic_int available_partitions;
    std::vector<config_context> contexts;
public:
    app_state_helper(): owner(nullptr), available_partitions(0)
    {
        contexts.clear();
    }
    void on_init_partitions();
    void clear_proposals()
    {
        for (config_context& cc: contexts)
            cc.clear_proposal();
    }
};

class app_state: public app_info
{
public:
    app_state(const app_info& info);
public:
    std::shared_ptr<app_state_helper>    helpers;
    std::vector<partition_configuration> partitions;
    static std::shared_ptr<app_state> create(const app_info& info);
    dsn::blob encode_json_with_status(app_status::type temp_status)
    {
        std::swap(status, temp_status);
        //we use the app_info's encode
        dsn::blob result = dsn::json::json_forwarder<app_state>::encode(*this);
        std::swap(status, temp_status);
        return result;
    }
};

typedef std::map<int32_t, std::shared_ptr<app_state>> app_mapper;
typedef std::unordered_map<rpc_address, node_state> node_mapper;
typedef std::list< std::shared_ptr<configuration_balancer_request> > migration_list;

struct meta_view
{
    app_mapper* apps;
    node_mapper* nodes;
};

inline bool is_node_alive(const node_mapper& nodes, rpc_address addr)
{
    auto iter = nodes.find(addr);
    if (iter == nodes.end())
        return false;
    return iter->second.is_alive;
}

inline const partition_configuration* get_config(const app_mapper& apps, const dsn::gpid& gpid)
{
    auto iter = apps.find(gpid.get_app_id());
    if (iter==apps.end() || iter->second->status==app_status::AS_DROPPED)
        return nullptr;
    return &(iter->second->partitions[gpid.get_partition_index()]);
}

inline const config_context* get_config_context(const app_mapper& apps, const dsn::gpid& gpid)
{
    auto iter = apps.find(gpid.get_app_id());
    if (iter == apps.end() || iter->second->status==app_status::AS_DROPPED)
        return nullptr;
    return &(iter->second->helpers->contexts[gpid.get_partition_index()]);
}

inline bool walk_through_primary(const meta_view& view, const dsn::rpc_address& addr, const std::function<bool (const partition_configuration& pc)>& func)
{
    auto iter = view.nodes->find(addr);
    if (iter == view.nodes->end())
        return false;
    node_state& ns = iter->second;
    for (const dsn::gpid& gpid: ns.primaries)
    {
        const partition_configuration* pc = get_config(*view.apps, gpid);
        if (pc != nullptr && !func(*pc) )
            return false;
    }
    return true;
}

inline bool walk_through_partitions(const meta_view& view, const dsn::rpc_address& addr, const std::function<bool (const partition_configuration& pc)>& func)
{
    auto iter = view.nodes->find(addr);
    if (iter == view.nodes->end() )
        return false;
    node_state& ns = iter->second;
    for (const dsn::gpid& gpid: ns.partitions)
    {
        const partition_configuration* pc = get_config(*view.apps, gpid);
        if (pc != nullptr && !func(*pc) )
            return false;
    }
    return true;
}

inline int count_partitions(const app_mapper& apps)
{
    int result = 0;
    for (auto iter: apps)
        if (iter.second->status == app_status::AS_AVAILABLE)
            result += iter.second->partition_count;
    return result;
}

void maintain_drops(/*inout*/ std::vector<rpc_address>& drops, const rpc_address& node, bool is_add);

}}
