// Copyright 2010-2014 RethinkDB, all rights reserved.
#ifndef RDB_PROTOCOL_CONTEXT_HPP_
#define RDB_PROTOCOL_CONTEXT_HPP_

#include <map>
#include <set>
#include <string>
#include <vector>

#include "errors.hpp"
#include <boost/optional.hpp>
#include <boost/shared_ptr.hpp>

#include "concurrency/one_per_thread.hpp"
#include "concurrency/promise.hpp"
#include "containers/counted.hpp"
#include "containers/name_string.hpp"
#include "containers/scoped.hpp"
#include "containers/uuid.hpp"
#include "perfmon/perfmon.hpp"
#include "protocol_api.hpp"
#include "rdb_protocol/changefeed.hpp"
#include "rdb_protocol/datum.hpp"
#include "rdb_protocol/geo/distances.hpp"
#include "rdb_protocol/geo/lon_lat_types.hpp"
#include "rdb_protocol/shards.hpp"
#include "rdb_protocol/wire_func.hpp"

enum class return_changes_t {
    NO = 0,
    YES = 1
};
ARCHIVE_PRIM_MAKE_RANGED_SERIALIZABLE(
        return_changes_t, int8_t,
        return_changes_t::NO, return_changes_t::YES);

enum class sindex_rename_result_t {
    OLD_NAME_DOESNT_EXIST,
    NEW_NAME_EXISTS,
    SUCCESS
};

class auth_semilattice_metadata_t;
class ellipsoid_spec_t;
class extproc_pool_t;
class name_string_t;
class namespace_interface_t;
template <class> class semilattice_readwrite_view_t;

enum class sindex_multi_bool_t;
enum class sindex_geo_bool_t;

namespace ql {
class configured_limits_t;
class env_t;
class query_cache_t;
class db_t : public single_threaded_countable_t<db_t> {
public:
    db_t(uuid_u _id, const name_string_t &_name) : id(_id), name(_name) { }
    const uuid_u id;
    const name_string_t name;
};
} // namespace ql

class table_generate_config_params_t {
public:
    static table_generate_config_params_t make_default() {
        table_generate_config_params_t p;
        p.num_shards = 1;
        p.primary_replica_tag = name_string_t::guarantee_valid("default");
        p.num_replicas[p.primary_replica_tag] = 1;
        return p;
    }
    size_t num_shards;
    std::map<name_string_t, size_t> num_replicas;
    name_string_t primary_replica_tag;
};

enum class admin_identifier_format_t {
    /* Some parts of the code rely on the fact that `admin_identifier_format_t` can be
    mapped to `{0, 1}` using `static_cast`. */
    name = 0,
    uuid = 1
};

class base_table_t : public slow_atomic_countable_t<base_table_t> {
public:
    virtual ql::datum_t get_id() const = 0;
    virtual const std::string &get_pkey() const = 0;

    virtual ql::datum_t read_row(ql::env_t *env,
        ql::datum_t pval, bool use_outdated) = 0;
    virtual counted_t<ql::datum_stream_t> read_all(
        ql::env_t *env,
        const std::string &sindex,
        const ql::protob_t<const Backtrace> &bt,
        const std::string &table_name,   /* the table's own name, for display purposes */
        const ql::datum_range_t &range,
        sorting_t sorting,
        bool use_outdated) = 0;
    virtual counted_t<ql::datum_stream_t> read_changes(
        ql::env_t *env,
        const ql::datum_t &squash,
        ql::changefeed::keyspec_t::spec_t &&spec,
        const ql::protob_t<const Backtrace> &bt,
        const std::string &table_name) = 0;
    virtual counted_t<ql::datum_stream_t> read_intersecting(
        ql::env_t *env,
        const std::string &sindex,
        const ql::protob_t<const Backtrace> &bt,
        const std::string &table_name,
        bool use_outdated,
        const ql::datum_t &query_geometry) = 0;
    virtual ql::datum_t read_nearest(
        ql::env_t *env,
        const std::string &sindex,
        const std::string &table_name,
        bool use_outdated,
        lon_lat_point_t center,
        double max_dist,
        uint64_t max_results,
        const ellipsoid_spec_t &geo_system,
        dist_unit_t dist_unit,
        const ql::configured_limits_t &limits) = 0;

    virtual ql::datum_t write_batched_replace(ql::env_t *env,
        const std::vector<ql::datum_t> &keys,
        const counted_t<const ql::func_t> &func,
        return_changes_t _return_changes, durability_requirement_t durability) = 0;
    virtual ql::datum_t write_batched_insert(ql::env_t *env,
        std::vector<ql::datum_t> &&inserts,
        std::vector<bool> &&pkey_was_autogenerated,
        conflict_behavior_t conflict_behavior, return_changes_t return_changes,
        durability_requirement_t durability) = 0;
    virtual bool write_sync_depending_on_durability(ql::env_t *env,
        durability_requirement_t durability) = 0;

    virtual bool sindex_create(ql::env_t *env, const std::string &id,
        counted_t<const ql::func_t> index_func, sindex_multi_bool_t multi,
        sindex_geo_bool_t geo) = 0;
    virtual bool sindex_drop(ql::env_t *env, const std::string &id) = 0;
    virtual sindex_rename_result_t sindex_rename(ql::env_t *env,
        const std::string &old_name, const std::string &new_name, bool overwrite) = 0;
    virtual std::vector<std::string> sindex_list(ql::env_t *env, bool use_outdated) = 0;
    virtual std::map<std::string, ql::datum_t> sindex_status(
        ql::env_t *env, const std::set<std::string> &sindexes) = 0;

    /* This must be public */
    virtual ~base_table_t() { }
};

class reql_cluster_interface_t {
public:
    /* All of these methods return `true` on success and `false` on failure; if they
    fail, they will set `*error_out` to a description of the problem. They can all throw
    `interrupted_exc_t`.

    These methods are safe to call from any thread, and the calls can overlap
    concurrently in arbitrary ways. By the time a method returns, any changes it makes
    must be visible on every thread. */

    /* From the user's point of view, many of these are methods on the table object. The
    reason they're internally defined on `reql_cluster_interface_t` rather than
    `base_table_t` is because their implementations fits better with the implementations
    of the other methods of `reql_cluster_interface_t` than `base_table_t`. */

    virtual bool db_create(const name_string_t &name,
            signal_t *interruptor, ql::datum_t *result_out, std::string *error_out) = 0;
    virtual bool db_drop(const name_string_t &name,
            signal_t *interruptor, ql::datum_t *result_out, std::string *error_out) = 0;
    virtual bool db_list(
            signal_t *interruptor,
            std::set<name_string_t> *names_out, std::string *error_out) = 0;
    virtual bool db_find(const name_string_t &name,
            signal_t *interruptor,
            counted_t<const ql::db_t> *db_out, std::string *error_out) = 0;
    virtual bool db_config(
            const counted_t<const ql::db_t> &db,
            const ql::protob_t<const Backtrace> &bt,
            ql::env_t *env,
            scoped_ptr_t<ql::val_t> *selection_out,
            std::string *error_out) = 0;

    /* `table_create()` won't return until the table is ready for reading */
    virtual bool table_create(const name_string_t &name, counted_t<const ql::db_t> db,
            const table_generate_config_params_t &config_params,
            const std::string &primary_key, write_durability_t durability,
            signal_t *interruptor, ql::datum_t *result_out, std::string *error_out) = 0;
    virtual bool table_drop(const name_string_t &name, counted_t<const ql::db_t> db,
            signal_t *interruptor, ql::datum_t *result_out, std::string *error_out) = 0;
    virtual bool table_list(counted_t<const ql::db_t> db,
            signal_t *interruptor, std::set<name_string_t> *names_out,
            std::string *error_out) = 0;
    virtual bool table_find(const name_string_t &name, counted_t<const ql::db_t> db,
            boost::optional<admin_identifier_format_t> identifier_format,
            signal_t *interruptor, counted_t<base_table_t> *table_out,
            std::string *error_out) = 0;
    virtual bool table_estimate_doc_counts(
            counted_t<const ql::db_t> db,
            const name_string_t &name,
            ql::env_t *env,
            std::vector<int64_t> *doc_counts_out,
            std::string *error_out) = 0;
    virtual bool table_config(
            counted_t<const ql::db_t> db,
            const name_string_t &name,
            const ql::protob_t<const Backtrace> &bt,
            ql::env_t *env,
            scoped_ptr_t<ql::val_t> *selection_out,
            std::string *error_out) = 0;
    virtual bool table_status(
            counted_t<const ql::db_t> db,
            const name_string_t &name,
            const ql::protob_t<const Backtrace> &bt,
            ql::env_t *env,
            scoped_ptr_t<ql::val_t> *selection_out,
            std::string *error_out) = 0;

    virtual bool table_wait(
            counted_t<const ql::db_t> db,
            const name_string_t &name,
            table_readiness_t readiness,
            signal_t *interruptor,
            ql::datum_t *result_out,
            std::string *error_out) = 0;
    virtual bool db_wait(
            counted_t<const ql::db_t> db,
            table_readiness_t readiness,
            signal_t *interruptor,
            ql::datum_t *result_out,
            std::string *error_out) = 0;

    virtual bool table_reconfigure(
            counted_t<const ql::db_t> db,
            const name_string_t &name,
            const table_generate_config_params_t &params,
            bool dry_run,
            signal_t *interruptor,
            ql::datum_t *result_out,
            std::string *error_out) = 0;
    virtual bool db_reconfigure(
            counted_t<const ql::db_t> db,
            const table_generate_config_params_t &params,
            bool dry_run,
            signal_t *interruptor,
            ql::datum_t *result_out,
            std::string *error_out) = 0;

    virtual bool table_rebalance(
            counted_t<const ql::db_t> db,
            const name_string_t &name,
            signal_t *interruptor,
            ql::datum_t *result_out,
            std::string *error_out) = 0;
    virtual bool db_rebalance(
            counted_t<const ql::db_t> db,
            signal_t *interruptor,
            ql::datum_t *result_out,
            std::string *error_out) = 0;

protected:
    virtual ~reql_cluster_interface_t() { }   // silence compiler warnings
};

class mailbox_manager_t;

class rdb_context_t {
public:
    // Used by unit tests.
    rdb_context_t();
    // Also used by unit tests.
    rdb_context_t(extproc_pool_t *_extproc_pool,
                  reql_cluster_interface_t *_cluster_interface);

    // The "real" constructor used outside of unit tests.
    rdb_context_t(extproc_pool_t *_extproc_pool,
                  mailbox_manager_t *_mailbox_manager,
                  reql_cluster_interface_t *_cluster_interface,
                  boost::shared_ptr<
                    semilattice_readwrite_view_t<
                        auth_semilattice_metadata_t> > _auth_metadata,
                  perfmon_collection_t *global_stats,
                  const std::string &_reql_http_proxy);

    ~rdb_context_t();

    extproc_pool_t *extproc_pool;
    reql_cluster_interface_t *cluster_interface;

    boost::shared_ptr< semilattice_readwrite_view_t<auth_semilattice_metadata_t> >
        auth_metadata;

    mailbox_manager_t *manager;

    const std::string reql_http_proxy;

    class stats_t {
    public:
        explicit stats_t(perfmon_collection_t *global_stats);

        perfmon_collection_t qe_stats_collection;
        perfmon_membership_t qe_stats_membership;
        perfmon_counter_t client_connections;
        perfmon_membership_t client_connections_membership;
        perfmon_counter_t clients_active;
        perfmon_membership_t clients_active_membership;
        perfmon_rate_monitor_t queries_per_sec;
        perfmon_membership_t queries_per_sec_membership;
        perfmon_counter_t queries_total;
        perfmon_membership_t queries_total_membership;
    private:
        DISABLE_COPYING(stats_t);
    } stats;

    std::set<ql::query_cache_t *> *get_query_caches_for_this_thread();

private:
    one_per_thread_t<std::set<ql::query_cache_t *> > query_caches;

private:
    DISABLE_COPYING(rdb_context_t);
};

#endif /* RDB_PROTOCOL_CONTEXT_HPP_ */

