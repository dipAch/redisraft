/*
 * This file is part of RedisRaft.
 *
 * Copyright (c) 2020 Redis Labs
 *
 * RedisRaft is dual licensed under the GNU Affero General Public License version 3
 * (AGPLv3) or the Redis Source Available License (RSAL).
 */

#ifndef _REDISRAFT_H
#define _REDISRAFT_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>

#define REDISMODULE_EXPERIMENTAL_API
#include "uv.h"
#include "hiredis/hiredis.h"
#include "hiredis/async.h"
#include "redismodule.h"
#include "raft.h"
#include "queue.h"

#include "version.h"

#define UNUSED(x)   ((void) x)

/* --------------- Forward declarations -------------- */

struct RaftReq;
struct EntryCache;
struct RedisRaftConfig;
struct Node;
struct Connection;
struct ShardingInfo;
struct ShardGroup;

/* --------------- RedisModule_Log levels used -------------- */

#define REDIS_RAFT_DATATYPE_NAME     "redisraft"
#define REDIS_RAFT_DATATYPE_ENCVER   1

/* --------------- RedisModule_Log levels used -------------- */

#define REDIS_WARNING   "warning"
#define REDIS_NOTICE    "notice"
#define REDIS_VERBOSE   "verbose"

/* -------------------- Logging macros -------------------- */

/*
 * We use our own logging mechanism because most log output is generated by
 * the Raft thread which cannot use Redis logging.
 *
 * TODO Migrate to RedisModule_Log when it's capable of logging using a
 * Thread Safe context.
 */

extern int redis_raft_loglevel;
extern FILE *redis_raft_logfile;

extern const char *redis_raft_log_levels[];
extern RedisModuleCtx *redis_raft_log_ctx;

#define LOGLEVEL_ERROR           0
#define LOGLEVEL_INFO            1
#define LOGLEVEL_VERBOSE         2
#define LOGLEVEL_DEBUG           3

#define LOG(level, fmt, ...) \
    do { if (redis_raft_loglevel >= level) \
            RedisModule_Log(redis_raft_log_ctx, redis_raft_log_levels[level], fmt, ##__VA_ARGS__); \
    } while(0)

#define LOG_ERROR(fmt, ...) LOG(LOGLEVEL_ERROR, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) LOG(LOGLEVEL_INFO, fmt, ##__VA_ARGS__)
#define LOG_VERBOSE(fmt, ...) LOG(LOGLEVEL_VERBOSE, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) LOG(LOGLEVEL_DEBUG, fmt, ##__VA_ARGS__)

#define PANIC(fmt, ...) \
    do {  LOG_ERROR("\n\n" \
                    "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n" \
                    "REDIS RAFT PANIC\n" \
                    "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n\n" \
                    fmt, ##__VA_ARGS__); abort(); } while (0)

#ifdef ENABLE_TRACE
#define TRACE(fmt, ...) \
    LOG(LOGLEVEL_DEBUG, "%s:%d: " fmt, __FILE__, __LINE__, ##__VA_ARGS__)
#define NODE_TRACE(node, fmt, ...) \
    LOG(LOGLEVEL_DEBUG, "%s:%d {node:%p/%d} " fmt, \
            __FILE__, __LINE__, \
            (node), \
            (node) ? (node->id) : 0, \
            ##__VA_ARGS__)
#else
#define NODE_TRACE(node, fmt, ...) do {} while (0)
#define TRACE(fmt, ...) do {} while (0)
#endif

#define NODE_LOG(level, node, fmt, ...) \
    LOG(level, "{node:%d} " fmt, (node) ? (node)->id : 0, ##__VA_ARGS__)

#define NODE_LOG_ERROR(node, fmt, ...) NODE_LOG(LOGLEVEL_ERROR, node, fmt, ##__VA_ARGS__)
#define NODE_LOG_INFO(node, fmt, ...) NODE_LOG(LOGLEVEL_INFO, node, fmt, ##__VA_ARGS__)
#define NODE_LOG_VERBOSE(node, fmt, ...) NODE_LOG(LOGLEVEL_VERBOSE, node, fmt, ##__VA_ARGS__)
#define NODE_LOG_DEBUG(node, fmt, ...) NODE_LOG(LOGLEVEL_DEBUG, node, fmt, ##__VA_ARGS__)

/* -------------------- Connections -------------------- */

/* Longest length of a NodeAddr string, including null terminator */
#define NODEADDR_MAXLEN      (255 + 1 + 5 + 1)

/* Node address specifier. */
typedef struct node_addr {
    uint16_t port;
    char host[256];             /* Hostname or IP address */
} NodeAddr;

/* A singly linked list of NodeAddr elements */
typedef struct NodeAddrListElement {
    NodeAddr addr;
    struct NodeAddrListElement *next;
} NodeAddrListElement;

typedef enum ConnState {
    CONN_DISCONNECTED,
    CONN_RESOLVING,
    CONN_CONNECTING,
    CONN_CONNECTED,
    CONN_CONNECT_ERROR
} ConnState;

typedef void (*ConnectionCallbackFunc)(struct Connection *conn);
typedef void (*ConnectionFreeFunc)(void *privdata);

/* Connection flags for Connection.flags */
#define CONN_TERMINATING    (1 << 0)

/* A connection represents a single outgoing Redis connection, such as the
 * one used to communicate with another node.
 *
 * Essentially it is a wrapper around a hiredis asyncRedisContext, providing
 * additional capabilities such as handling asynchronous DNS resolution,
 * dropped connections and re-connects, etc.
 */

typedef struct Connection {
    unsigned long id;                   /* Unique connection ID */
    ConnState state;
    unsigned int flags;
    NodeAddr addr;                      /* Address of last ConnConnect() */
    char ipaddr[INET6_ADDRSTRLEN+1];    /* Resolved IP address */
    redisAsyncContext *rc;              /* hiredis async context */
    uv_getaddrinfo_t uv_resolver;       /* libuv resolver context */
    struct RedisRaftCtx *rr;            /* Pointer back to redis_raft */
    long long last_connected_time;      /* Last connection time */
    unsigned long int connect_oks;      /* Successful connects */
    unsigned long int connect_errors;   /* Connection errors since last connection */
    struct timeval timeout;             /* Timeout to use if not null */
    void *privdata;                     /* User provided pointer */

    /* Connect callback is guaranteed after ConnConnect(); Callback should check
     * connection state as it will also be called on error.
     */
    ConnectionCallbackFunc connect_callback;

    /* Idle callback is called periodically for every connection that is in idle
     * state, i.e. CONN_DISCONNECTED or CONN_CONNECT_ERROR.
     *
     * Typically it is used to either (re-)establish the connection using ConnConnect()
     * or destroy the connection.
     */
    ConnectionCallbackFunc idle_callback;

    /* Free callback is called when the connection gets freed, and as a last chance
     * to free privdata.
     */
    ConnectionFreeFunc free_callback;

    /* Linkage to global connections list */
    LIST_ENTRY(Connection) entries;
} Connection;

/* -------------------- Global Raft Context -------------------- */

/* General state of the module */
typedef enum RedisRaftState {
    REDIS_RAFT_UNINITIALIZED,       /* Waiting for RAFT.CLUSTER command */
    REDIS_RAFT_UP,                  /* Up and running */
    REDIS_RAFT_LOADING,             /* Loading (or attempting) RDB/Raft Log on startup */
    REDIS_RAFT_JOINING              /* Processing a RAFT.CLUSTER JOIN command */
} RedisRaftState;

/* A node configuration entry that describes the known configuration of a specific
 * node at the time of snapshot.
 */
typedef struct SnapshotCfgEntry {
    raft_node_id_t  id;
    int             voting;
    NodeAddr        addr;
    struct SnapshotCfgEntry *next;
} SnapshotCfgEntry;

typedef struct NodeIdEntry {
    raft_node_id_t id;
    struct NodeIdEntry *next;
} NodeIdEntry;

#define RAFT_DBID_LEN               32
#define RAFT_SHARDGROUP_NODEID_LEN  40  /* Combined DBID_LEN + 32-bit node id */

/* Snapshot metadata.  There is a single instance of this struct available at all times,
 * which is accessed as follows:
 * 1. During cluster setup, it is initialized (e.g. with a unique dbid).
 * 2. The last applied term and index fields are updated every time we apply a log entry
 *    into the dataset, to reflect the real-time state of the snapshot.
 * 3. On rdbsave, the record gets serialized (using a dummy key for now; TODO use a global
 *    state mechanism when Redis Module API supports it).
 * 4. On rdbload, the record gets loaded and the loaded flag is set.
 */
typedef struct RaftSnapshotInfo {
    bool loaded;
    char dbid[RAFT_DBID_LEN+1];
    raft_term_t last_applied_term;
    raft_index_t last_applied_idx;
    SnapshotCfgEntry *cfg;
    NodeIdEntry *used_node_ids;  /* All node ids that are, or have ever been, part of this cluster */
} RaftSnapshotInfo;

/* Global Raft context */
typedef struct RedisRaftCtx {
    void *raft;                 /* Raft library context */
    RedisModuleCtx *ctx;        /* Redis module thread-safe context; only used to push commands
                                   we get from the leader. */
    RedisRaftState state;       /* Raft module state */
    uv_thread_t thread;         /* Raft I/O thread */
    uv_loop_t *loop;            /* Raft I/O loop */
    uv_async_t rqueue_sig;      /* A signal we have something on rqueue */
    uv_timer_t raft_periodic_timer;     /* Invoke Raft periodic func */
    uv_timer_t node_reconnect_timer;    /* Handle connection issues */
    uv_mutex_t rqueue_mutex;    /* Mutex protecting rqueue access */
    STAILQ_HEAD(rqueue, RaftReq) rqueue;     /* Requests queue (Redis thread -> Raft thread) */
    struct RaftLog *log;        /* Raft persistent log; May be NULL if not used */
    struct EntryCache *logcache;
    struct RedisRaftConfig *config;     /* User provided configuration */
    bool snapshot_in_progress;  /* Indicates we're creating a snapshot in the background */
    raft_index_t last_snapshot_idx;
    raft_term_t last_snapshot_term;
    struct RaftReq *debug_req;    /* Current RAFT.DEBUG request context, if processing one */
    bool callbacks_set;         /* TODO: Needed? */
    int snapshot_child_fd;      /* Pipe connected to snapshot child process */
    RaftSnapshotInfo snapshot_info; /* Current snapshot info */
    RedisModuleCommandFilter *registered_filter;
    struct ShardingInfo *sharding_info; /* Information about sharding, when cluster mode is enabled */
    /* General stats */
    unsigned long client_attached_entries;      /* Number of log entries attached to user connections */
    unsigned long long proxy_reqs;              /* Number of proxied requests */
    unsigned long long proxy_failed_reqs;       /* Number of failed proxy requests, i.e. did not send */
    unsigned long long proxy_failed_responses;  /* Number of failed proxy responses, i.e. did not complete */
    unsigned long proxy_outstanding_reqs;       /* Number of proxied requests pending */
    unsigned long snapshots_loaded;             /* Number of snapshots loaded */
} RedisRaftCtx;

extern RedisRaftCtx redis_raft;

extern raft_log_impl_t RaftLogImpl;

#define REDIS_RAFT_DEFAULT_LOG_FILENAME             "redisraft.db"
#define REDIS_RAFT_DEFAULT_INTERVAL                 100 /* usec */
#define REDIS_RAFT_DEFAULT_REQUEST_TIMEOUT          200 /* usec */
#define REDIS_RAFT_DEFAULT_ELECTION_TIMEOUT         1000 /* usec */
#define REDIS_RAFT_DEFAULT_CONNECTION_TIMEOUT       3000 /* usec */
#define REDIS_RAFT_DEFAULT_JOIN_TIMEOUT             120 /* seconds */
#define REDIS_RAFT_DEFAULT_RECONNECT_INTERVAL       100
#define REDIS_RAFT_DEFAULT_PROXY_RESPONSE_TIMEOUT   10000
#define REDIS_RAFT_DEFAULT_RAFT_RESPONSE_TIMEOUT    1000
#define REDIS_RAFT_DEFAULT_LOG_MAX_CACHE_SIZE       8*1000*1000
#define REDIS_RAFT_DEFAULT_LOG_MAX_FILE_SIZE        64*1000*1000

#define REDIS_RAFT_HASH_SLOTS                       16384
#define REDIS_RAFT_HASH_MIN_SLOT                    0
#define REDIS_RAFT_HASH_MAX_SLOT                    16383
#define REDIS_RAFT_DEFAULT_SHARDGROUP_UPDATE_INTERVAL 5000

static inline bool HashSlotValid(int slot)
{
    return (slot >= REDIS_RAFT_HASH_MIN_SLOT && slot <= REDIS_RAFT_HASH_MAX_SLOT);
}

static inline bool HashSlotRangeValid(int start_slot, int end_slot)
{
    return (HashSlotValid(start_slot) && HashSlotValid(end_slot) &&
            start_slot <= end_slot);
}

typedef struct RedisRaftConfig {
    raft_node_id_t id;          /* Local node Id */
    NodeAddr addr;              /* Address of local node, if specified */
    char *rdb_filename;         /* Original Redis dbfilename */
    char *raft_log_filename;    /* Raft log file name, derived from dbfilename */
    bool follower_proxy;        /* Do follower nodes proxy requests to leader? */
    bool quorum_reads;          /* Reads have to go through quorum */
    /* Tuning */
    int raft_interval;
    int request_timeout;
    int election_timeout;
    int connection_timeout;
    int join_timeout;
    int reconnect_interval;
    int proxy_response_timeout;
    int raft_response_timeout;
    /* Cache and file compaction */
    unsigned long raft_log_max_cache_size;
    unsigned long raft_log_max_file_size;
    bool raft_log_fsync;
    /* Cluster mode */
    bool sharding;                      /* Are we running in a sharding configuration? */
    int sharding_start_hslot;           /* First cluster hash slot */
    int sharding_end_hslot;             /* Last cluster hash slot */
    int shardgroup_update_interval;     /* Milliseconds between shardgroup updates */
} RedisRaftConfig;

typedef struct PendingResponse {
    bool proxy;
    int id;
    long long request_time;
    STAILQ_ENTRY(PendingResponse) entries;
} PendingResponse;

/* Maintains all state about peer nodes */
typedef struct Node {
    raft_node_id_t id;              /* Raft unique node ID */
    RedisRaftCtx *rr;               /* RedisRaftCtx handle */
    Connection *conn;               /* Connection to node */
    NodeAddr addr;                  /* Node's address */
    bool load_snapshot_in_progress; /* Are we currently pushing a snapshot? */
    raft_index_t load_snapshot_idx; /* Index of snapshot we're pushing */
    time_t load_snapshot_last_time; /* Last time we pushed a snapshot */
    uv_fs_t uv_snapshot_req;        /* libuv handle managing snapshot loading from disk */
    uv_file uv_snapshot_file;       /* libuv handle for snapshot file */
    size_t snapshot_size;           /* Size of snapshot we're pushing */
    char *snapshot_buf;             /* Snapshot buffer; TODO: Currently we buffer the entire RDB
                                     * because hiredis will not stream it for us. */
    uv_buf_t uv_snapshot_buf;       /* libuv wrapper for snapshot_buf */
    long pending_raft_response_num;     /* Number of pending Raft responses */
    long pending_proxy_response_num;    /* Number of pending proxy responses */
    STAILQ_HEAD(pending_responses, PendingResponse) pending_responses;
    LIST_ENTRY(Node) entries;
} Node;

typedef void (*RaftReqHandler)(RedisRaftCtx *, struct RaftReq *);

/* General purpose status code.  Convention is this:
 * In redisraft.c (Redis Module wrapper) we generally use REDISMODULE_OK/REDISMODULE_ERR.
 * Elsewhere we stick to it.
 */
typedef enum RRStatus {
    RR_OK       = 0,
    RR_ERROR
} RRStatus;

/* Request types.  Note that these must match the order in RaftReqHandlers! */
enum RaftReqType {
    RR_CLUSTER_INIT = 1,
    RR_CLUSTER_JOIN,
    RR_CFGCHANGE_ADDNODE,
    RR_CFGCHANGE_REMOVENODE,
    RR_APPENDENTRIES,
    RR_REQUESTVOTE,
    RR_REDISCOMMAND,
    RR_INFO,
    RR_LOADSNAPSHOT,
    RR_DEBUG,
    RR_CLIENT_DISCONNECT,
    RR_SHARDGROUP_ADD,
    RR_SHARDGROUP_GET,
    RR_SHARDGROUP_LINK
};

extern const char *RaftReqTypeStr[];

typedef struct {
    raft_node_id_t id;
    NodeAddr addr;
} RaftCfgChange;

typedef struct {
    int argc;
    RedisModuleString **argv;
} RaftRedisCommand;

typedef struct {
    int size;           /* Size of allocated array */
    int len;            /* Number of elements in array */
    RaftRedisCommand **commands;
} RaftRedisCommandArray;

/* Max length of a ShardGroupNode string, including newline and null terminator */
#define SHARDGROUPNODE_MAXLEN   (RAFT_SHARDGROUP_NODEID_LEN+1 + NODEADDR_MAXLEN + 2)

/* Describes a node in a ShardGroup (foreign RedisRaft cluster). */
typedef struct ShardGroupNode {
    char node_id[RAFT_SHARDGROUP_NODEID_LEN+1]; /* Combined dbid + node_id */
    NodeAddr addr;                              /* Node address and port */
} ShardGroupNode;

/* Max length of a ShardGroup string, including newline and null terminator
 * but excluding nodes */
#define SHARDGROUP_MAXLEN       (10 + 1 + 10 + 1 + 10 + 1 + 1)

/* Describes a ShardGroup. A ShardGroup is a RedisRaft cluster that
 * is assigned with a specific range of hash slots.
 */
typedef struct ShardGroup {
    /* Configuration */
    unsigned int id;                     /* Local shardgroup identifier */
    unsigned int start_slot;             /* First slot, inclusive */
    unsigned int end_slot;               /* Last slot, inclusive */
    unsigned int nodes_num;              /* Number of nodes listed */
    ShardGroupNode *nodes;               /* Nodes array */

    /* Runtime state */
    unsigned int next_redir;             /* Round-robin -MOVED index */

    /* Synchronization state */
    unsigned int node_conn_idx;          /* Next node to connect to, when looking for a live one */
    NodeAddr conn_addr;                  /* Address to use on next connect, if use_conn_addr is set */
    bool use_conn_addr;                  /* Should we use conn_addr? Otherwise iterate node_conn_idx? */
    Connection *conn;                    /* Connection we use */
    long long last_updated;              /* Last time of successful update (mstime) */
    bool update_in_progress;             /* Are we currently updating? */
} ShardGroup;

#define RAFT_LOGTYPE_ADD_SHARDGROUP     (RAFT_LOGTYPE_NUM+1)
#define RAFT_LOGTYPE_UPDATE_SHARDGROUP  (RAFT_LOGTYPE_NUM+2)

/* Sharding information, used when cluster_mode is enabled and multiple
 * RedisRaft clusters operate together to perform sharding.
 */
typedef struct ShardingInfo {
    unsigned int shard_groups_num;       /* Number of shard groups */
    ShardGroup **shard_groups;           /* Shard groups array */

    /* Maps hash slots to ShardGroups indexes.
     *
     * Note that a one-based index into the shard_groups array is used,
     * since a zero value indicates the slot is unassigned. The index
     * should therefore be adjusted before refering the array.
     */
    int hash_slots_map[REDIS_RAFT_HASH_SLOTS];
} ShardingInfo;

/* Debug message structure, used for RAFT.DEBUG / RR_DEBUG
 * requests.
 */
enum RaftDebugReqType {
    RR_DEBUG_COMPACT,
    RR_DEBUG_NODECFG,
    RR_DEBUG_SENDSNAPSHOT
};

typedef struct RaftDebugReq {
    enum RaftDebugReqType type;
    union {
        struct {
            int delay;
        } compact;
        struct {
            raft_node_id_t id;
            char *str;
        } nodecfg;
        struct {
            raft_node_id_t id;
        } sendsnapshot;
    } d;
} RaftDebugReq;

typedef struct RaftReq {
    int type;
    STAILQ_ENTRY(RaftReq) entries;
    RedisModuleBlockedClient *client;
    RedisModuleCtx *ctx;
    union {
        struct {
            NodeAddrListElement *addr;
        } cluster_join;
        RaftCfgChange cfgchange;
        struct {
            raft_node_id_t src_node_id;
            msg_appendentries_t msg;
        } appendentries;
        struct {
            raft_node_id_t src_node_id;
            msg_requestvote_t msg;
        } requestvote;
        struct {
            Node *proxy_node;
            int hash_slot;
            RaftRedisCommandArray cmds;
            msg_entry_response_t response;
        } redis;
        struct {
            raft_term_t term;
            raft_index_t idx;
            RedisModuleString *snapshot;
        } loadsnapshot;
        struct {
            unsigned long long client_id;
        } client_disconnect;
        struct ShardGroup shardgroup_add;
        struct {
            NodeAddr addr;
        } shardgroup_link;
        RaftDebugReq debug;
    } r;
} RaftReq;

#define RAFTLOG_VERSION     1

/* Flags for RaftLogOpen */
#define RAFTLOG_KEEP_INDEX  1                   /* Index was written by this process, safe to use. */

typedef struct RaftLog {
    uint32_t            version;                /* Log file format version */
    char                dbid[RAFT_DBID_LEN+1];  /* DB unique ID */
    raft_node_id_t      node_id;                /* Node ID */
    bool                fsync;                  /* Should fsync every append? */
    unsigned long int   num_entries;            /* Entries in log */
    raft_term_t         snapshot_last_term;     /* Last term included in snapshot */
    raft_index_t        snapshot_last_idx;      /* Last index included in snapshot */
    raft_index_t        index;                  /* Index of last entry */
    raft_term_t         term;                   /* Last term we're aware of */
    raft_node_id_t      vote;                   /* Our vote in the last term, or -1 */
    size_t              file_size;              /* File size at the time of last write */
    const char          *filename;
    FILE                *file;
    FILE                *idxfile;
} RaftLog;


#define SNAPSHOT_RESULT_MAGIC    0x70616e73  /* "snap" */
typedef struct SnapshotResult {
    int magic;
    int success;
    char rdb_filename[256];
    char err[256];
} SnapshotResult;

/* Entry type for the internal command table used by RedisRaft,
 * used to determine how different intercepted Redis commands are
 * handled.
 */
typedef struct {
    char *name;                 /* Command name */
    unsigned int flags;         /* Command flags, see CMD_SPEC_* */
} CommandSpec;

#define CMD_SPEC_READONLY       (1<<1)      /* Command is a read-only command */
#define CMD_SPEC_WRITE          (1<<2)      /* Command is a (potentially) write command */
#define CMD_SPEC_UNSUPPORTED    (1<<3)      /* Command is not supported, should be rejected */
#define CMD_SPEC_DONT_INTERCEPT (1<<4)      /* Command should not be intercepted to RAFT */

/* Command filtering re-entrancy counter handling.
 *
 * This mechanism tracks calls from Redis Raft into Redis and used by the
 * command filtering hook to avoid raftizing commands as they're pushed from the log
 * to the FSM.
 *
 * Redis Module API provides the REDISMODULE_CMDFILTER_NOSELF flag which does
 * the same thing, but does not apply to executions from a thread safe context.
 *
 * This must wrap every call to RedisModule_Call(), after the Redis lock has been
 * acquired, and unless the called command is known to be excluded from raftizing.
 */

extern int redis_raft_in_rm_call;   /* defined in common.c */

static void inline enterRedisModuleCall(void) {
    redis_raft_in_rm_call++;
}

static void inline exitRedisModuleCall(void) {
    redis_raft_in_rm_call--;
}

static int inline checkInRedisModuleCall(void) {
    return redis_raft_in_rm_call;
}

typedef struct JoinLinkState {
    NodeAddrListElement *addr;
    NodeAddrListElement *addr_iter;
    Connection *conn;
    time_t start;                       /* Time we initiated the join, to enable it to fail if it takes too long */
    RaftReq *req;                       /* Original RaftReq, so we can return a reply */
    bool failed;                        /* unrecoverable failure */
    char *type;                         /* error message to print if exhaust time */
    ConnectionCallbackFunc connect_callback;
} JoinLinkState;

/* common.c */
void joinLinkIdleCallback(Connection *conn);
void joinLinkFreeCallback(void *privdata);
const char *getStateStr(RedisRaftCtx *rr);
const char *raft_logtype_str(int type);
void replyRaftError(RedisModuleCtx *ctx, int error);
RRStatus checkLeader(RedisRaftCtx *rr, RaftReq *req, Node **ret_leader);
RRStatus checkRaftNotLoading(RedisRaftCtx *rr, RaftReq *req);
RRStatus checkRaftState(RedisRaftCtx *rr, RaftReq *req);
RRStatus setRaftizeMode(RedisRaftCtx *rr, RedisModuleCtx *ctx, bool flag);
void replyRedirect(RedisRaftCtx *rr, RaftReq *req, NodeAddr *addr);
bool parseMovedReply(const char *str, NodeAddr *addr);

/* node_addr.c */
bool NodeAddrParse(const char *node_addr, size_t node_addr_len, NodeAddr *result);
bool NodeAddrEqual(const NodeAddr *a1, const NodeAddr *a2);
void NodeAddrListAddElement(NodeAddrListElement **head, const NodeAddr *addr);
void NodeAddrListConcat(NodeAddrListElement **head, const NodeAddrListElement *other);
void NodeAddrListFree(NodeAddrListElement *head);

/* node.c */
Node *NodeCreate(RedisRaftCtx *rr, int id, const NodeAddr *addr);
void HandleNodeStates(RedisRaftCtx *rr);
void NodeAddPendingResponse(Node *node, bool proxy);
void NodeDismissPendingResponse(Node *node);

/* serialization.c */
raft_entry_t *RaftRedisCommandArraySerialize(const RaftRedisCommandArray *source);
size_t RaftRedisCommandDeserialize(RaftRedisCommand *target, const void *buf, size_t buf_size);
RRStatus RaftRedisCommandArrayDeserialize(RaftRedisCommandArray *target, const void *buf, size_t buf_size);
void RaftRedisCommandArrayFree(RaftRedisCommandArray *array);
void RaftRedisCommandFree(RaftRedisCommand *r);
RaftRedisCommand *RaftRedisCommandArrayExtend(RaftRedisCommandArray *target);
void RaftRedisCommandArrayMove(RaftRedisCommandArray *target, RaftRedisCommandArray *source);

/* raft.c */
RRStatus RedisRaftInit(RedisModuleCtx *ctx, RedisRaftCtx *rr, RedisRaftConfig *config);
RRStatus RedisRaftStart(RedisModuleCtx *ctx, RedisRaftCtx *rr);
void RaftReqFree(RaftReq *req);
RaftReq *RaftReqInit(RedisModuleCtx *ctx, enum RaftReqType type);
RaftReq *RaftDebugReqInit(RedisModuleCtx *ctx, enum RaftDebugReqType type);
void RaftReqSubmit(RedisRaftCtx *rr, RaftReq *req);
void RaftReqHandleQueue(uv_async_t *handle);
void addUsedNodeId(RedisRaftCtx *rr, raft_node_id_t node_id);
bool hasNodeIdBeenUsed(RedisRaftCtx *rr, raft_node_id_t node_id);

/* util.c */
int RedisModuleStringToInt(RedisModuleString *str, int *value);
char *catsnprintf(char *strbuf, size_t *strbuf_len, const char *fmt, ...);
int stringmatchlen(const char *pattern, int patternLen, const char *string, int stringLen, int nocase);
int stringmatch(const char *pattern, const char *string, int nocase);
int RedisInfoIterate(const char **info_ptr, size_t *info_len, const char **key, size_t *keylen, const char **value, size_t *valuelen);
char *RedisInfoGetParam(RedisRaftCtx *rr, const char *section, const char *param);
RRStatus parseMemorySize(const char *value, unsigned long *result);
RRStatus formatExactMemorySize(unsigned long value, char *buf, size_t buf_size);

/* log.c */
RaftLog *RaftLogCreate(const char *filename, const char *dbid, raft_term_t snapshot_term, raft_index_t snapshot_index, raft_term_t current_term, raft_node_id_t last_vote, RedisRaftConfig *config);
RaftLog *RaftLogOpen(const char *filename, RedisRaftConfig *config, int flags);
void RaftLogClose(RaftLog *log);
RRStatus RaftLogAppend(RaftLog *log, raft_entry_t *entry);
RRStatus RaftLogSetVote(RaftLog *log, raft_node_id_t vote);
RRStatus RaftLogSetTerm(RaftLog *log, raft_term_t term, raft_node_id_t vote);
int RaftLogLoadEntries(RaftLog *log, int (*callback)(void *, raft_entry_t *, raft_index_t), void *callback_arg);
RRStatus RaftLogWriteEntry(RaftLog *log, raft_entry_t *entry);
RRStatus RaftLogSync(RaftLog *log);
raft_entry_t *RaftLogGet(RaftLog *log, raft_index_t idx);
RRStatus RaftLogDelete(RaftLog *log, raft_index_t from_idx, func_entry_notify_f cb, void *cb_arg);
RRStatus RaftLogReset(RaftLog *log, raft_index_t index, raft_term_t term);
raft_index_t RaftLogCount(RaftLog *log);
raft_index_t RaftLogFirstIdx(RaftLog *log);
raft_index_t RaftLogCurrentIdx(RaftLog *log);
long long int RaftLogRewrite(RedisRaftCtx *rr, const char *filename, raft_index_t last_idx, raft_term_t last_term);
void RaftLogRemoveFiles(const char *filename);
void RaftLogArchiveFiles(RedisRaftCtx *rr);
RRStatus RaftLogRewriteSwitch(RedisRaftCtx *rr, RaftLog *new_log, unsigned long new_log_entries);

typedef struct EntryCache {
    unsigned long int size;             /* Size of ptrs */
    unsigned long int len;              /* Number of entries in cache */
    unsigned long int start_idx;        /* Log index of first entry */
    unsigned long int start;            /* ptrs array index of first entry */
    unsigned long int entries_memsize;  /* Total memory used by entries */
    raft_entry_t **ptrs;
} EntryCache;

EntryCache *EntryCacheNew(unsigned long initial_size);
void EntryCacheFree(EntryCache *cache);
void EntryCacheAppend(EntryCache *cache, raft_entry_t *ety, raft_index_t idx);
raft_entry_t *EntryCacheGet(EntryCache *cache, raft_index_t idx);
long EntryCacheDeleteHead(EntryCache *cache, raft_index_t idx);
long EntryCacheDeleteTail(EntryCache *cache, raft_index_t index);
long EntryCacheCompact(EntryCache *cache, size_t max_memory);

/* config.c */
void ConfigInit(RedisModuleCtx *ctx, RedisRaftConfig *config);
RRStatus ConfigParseArgs(RedisModuleCtx *ctx, RedisModuleString **argv, int argc, RedisRaftConfig *target);
void handleConfigSet(RedisRaftCtx *rr, RedisModuleCtx *ctx, RedisModuleString **argv, int argc);
void handleConfigGet(RedisModuleCtx *ctx, RedisRaftConfig *config, RedisModuleString **argv, int argc);
RRStatus ConfigReadFromRedis(RedisRaftCtx *rr);
RRStatus ConfigureRedis(RedisModuleCtx *ctx);

/* snapshot.c */
extern RedisModuleTypeMethods RedisRaftTypeMethods;
extern RedisModuleType *RedisRaftType;
void initializeSnapshotInfo(RedisRaftCtx *rr);
void handleLoadSnapshot(RedisRaftCtx *rr, RaftReq *req);
void checkLoadSnapshotProgress(RedisRaftCtx *rr);
RRStatus initiateSnapshot(RedisRaftCtx *rr);
RRStatus finalizeSnapshot(RedisRaftCtx *rr, SnapshotResult *sr);
void cancelSnapshot(RedisRaftCtx *rr, SnapshotResult *sr);
void handleCompact(RedisRaftCtx *rr, RaftReq *req);
int pollSnapshotStatus(RedisRaftCtx *rr, SnapshotResult *sr);
void configRaftFromSnapshotInfo(RedisRaftCtx *rr);
int raftSendSnapshot(raft_server_t *raft, void *user_data, raft_node_t *raft_node);
void archiveSnapshot(RedisRaftCtx *rr);

/* proxy.c */
RRStatus ProxyCommand(RedisRaftCtx *rr, RaftReq *req, Node *leader);

/* connection.c */
Connection *ConnCreate(RedisRaftCtx *rr, void *privdata, ConnectionCallbackFunc idle_cb, ConnectionFreeFunc free_cb);
RRStatus ConnConnect(Connection *conn, const NodeAddr *addr, ConnectionCallbackFunc connect_callback);
void ConnAsyncTerminate(Connection *conn);
void ConnMarkDisconnected(Connection *conn);
void HandleIdleConnections(RedisRaftCtx *rr);
void *ConnGetPrivateData(Connection *conn);
RedisRaftCtx *ConnGetRedisRaftCtx(Connection *conn);
redisAsyncContext *ConnGetRedisCtx(Connection *conn);
bool ConnIsIdle(Connection *conn);
bool ConnIsConnected(Connection *conn);
const char *ConnGetStateStr(Connection *conn);

/* cluster.c */
char *ShardGroupSerialize(ShardGroup *sg);
RRStatus ShardGroupDeserialize(const char *buf, size_t buf_len, ShardGroup *sg);
void ShardGroupFree(ShardGroup *sg);
RRStatus ShardGroupParse(RedisModuleCtx *ctx, RedisModuleString **argv, int argc, ShardGroup *sg);

RRStatus computeHashSlot(RedisRaftCtx *rr, RaftReq *req);
void handleClusterCommand(RedisRaftCtx *rr, RaftReq *req);
void ShardingInfoInit(RedisRaftCtx *rr);
void ShardingInfoReset(RedisRaftCtx *rr);
RRStatus ShardingInfoValidateShardGroup(RedisRaftCtx *rr, ShardGroup *new_sg);
RRStatus ShardingInfoAddShardGroup(RedisRaftCtx *rr, ShardGroup *new_sg);
RRStatus ShardingInfoUpdateShardGroup(RedisRaftCtx *rr, ShardGroup *new_sg);
void ShardingInfoRDBSave(RedisModuleIO *rdb);
void ShardingInfoRDBLoad(RedisModuleIO *rdb);
void ShardingPeriodicCall(RedisRaftCtx *rr);
RRStatus ShardGroupAppendLogEntry(RedisRaftCtx *rr, ShardGroup *sg, int type, void *user_data);
void handleShardGroupLink(RedisRaftCtx *rr, RaftReq *req);

/* join.c */
void HandleClusterJoinCompleted(RedisRaftCtx *rr, RaftReq *pReq);
void handleClusterJoin(RedisRaftCtx *rr, RaftReq *req);

/* commands.c */
RRStatus CommandSpecInit(RedisModuleCtx *ctx);
unsigned int CommandSpecGetAggregateFlags(RaftRedisCommandArray *array, unsigned int default_flags);
const CommandSpec *CommandSpecGet(const RedisModuleString *cmd);

#endif  /* _REDISRAFT_H */
