/* -------------------------------------------------------------------------- */
/* Copyright 2002-2017, OpenNebula Project, OpenNebula Systems                */
/*                                                                            */
/* Licensed under the Apache License, Version 2.0 (the "License"); you may    */
/* not use this file except in compliance with the License. You may obtain    */
/* a copy of the License at                                                   */
/*                                                                            */
/* http://www.apache.org/licenses/LICENSE-2.0                                 */
/*                                                                            */
/* Unless required by applicable law or agreed to in writing, software        */
/* distributed under the License is distributed on an "AS IS" BASIS,          */
/* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.   */
/* See the License for the specific language governing permissions and        */
/* limitations under the License.                                             */
/* -------------------------------------------------------------------------- */

#include "LogDB.h"
#include "Nebula.h"
#include "NebulaUtil.h"
#include "ZoneServer.h"
#include "Callbackable.h"

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

const char * LogDB::table = "logdb";

const char * LogDB::db_names = "log_index, term, sqlcmd, timestamp, fed_index";

const char * LogDB::db_bootstrap = "CREATE TABLE IF NOT EXISTS "
    "logdb (log_index INTEGER PRIMARY KEY, term INTEGER, sqlcmd MEDIUMTEXT, "
    "timestamp INTEGER, fed_index INTEGER)";

/* -------------------------------------------------------------------------- */

int LogDB::bootstrap(SqlDB *_db)
{
    int rc;

    std::ostringstream oss(db_bootstrap);

    rc = _db->exec_local_wr(oss);

    // Create indexes
    oss.str("CREATE INDEX fed_index_idx on logdb (fed_index);");

    rc += _db->exec_local_wr(oss);

    oss.str("CREATE INDEX timestamp_idx on logdb (timestamp);");

    rc += _db->exec_local_wr(oss);

    return rc;
};

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

int LogDBRecord::select_cb(void *nil, int num, char **values, char **names)
{
    if ( !values || !values[0] || !values[1] || !values[2] || !values[3] ||
            !values[4] || !values[5] || !values[6] || num != 7 )
    {
        return -1;
    }

    std::string zsql;

    std::string * _sql;

    index = static_cast<unsigned int>(atoi(values[0]));
    term  = static_cast<unsigned int>(atoi(values[1]));
    zsql  = values[2];

    timestamp  = static_cast<unsigned int>(atoi(values[3]));

    fed_index  = static_cast<unsigned int>(atoi(values[4]));

    prev_index = static_cast<unsigned int>(atoi(values[5]));
    prev_term  = static_cast<unsigned int>(atoi(values[6]));

    _sql = one_util::zlib_decompress(zsql, true);

    if ( _sql == 0 )
    {

        std::ostringstream oss;

        oss << "Error zlib inflate for " << index << ", " << fed_index
            << ", " << zsql;

        NebulaLog::log("DBM", Log::ERROR, oss);

        return -1;
    }

    sql = *_sql;

    delete _sql;

    return 0;
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

LogDB::LogDB(SqlDB * _db, bool _solo, unsigned int _lret):solo(_solo), db(_db),
    next_index(0), last_applied(-1), last_index(-1), last_term(-1),
    log_retention(_lret)
{
    int r, i;

    pthread_mutex_init(&mutex, 0);

    LogDBRecord lr;

    if ( get_log_record(0, lr) != 0 )
    {
        std::ostringstream oss;

        oss << time(0);

        insert_log_record(0, 0, oss, time(0), -1);
    }

    setup_index(r, i);
};

LogDB::~LogDB()
{
    delete db;
};

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

int LogDB::setup_index(int& _last_applied, int& _last_index)
{
    int rc = 0;

    std::ostringstream oss;

    single_cb<int> cb;

    LogDBRecord lr;

    _last_applied = 0;
    _last_index   = -1;

    pthread_mutex_lock(&mutex);

    cb.set_callback(&_last_index);

    oss << "SELECT MAX(log_index) FROM logdb";

    rc += db->exec_rd(oss, &cb);

    cb.unset_callback();

    if ( rc == 0 )
    {
        next_index = _last_index + 1;
        last_index = _last_index;
    }

    oss.str("");

    cb.set_callback(&_last_applied);

    oss << "SELECT MAX(log_index) FROM logdb WHERE timestamp != 0";

    rc += db->exec_rd(oss, &cb);

    cb.unset_callback();

    if ( rc == 0 )
    {
        last_applied = _last_applied;
    }

    rc += get_log_record(last_index, lr);

    if ( rc == 0 )
    {
        last_term = lr.term;
    }

    build_federated_index();

    pthread_mutex_unlock(&mutex);

    return rc;
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

int LogDB::get_log_record(unsigned int index, LogDBRecord& lr)
{
    ostringstream oss;

    unsigned int prev_index = index - 1;

    if ( index == 0 )
    {
        prev_index = 0;
    }

    lr.index = index + 1;

    oss << "SELECT c.log_index, c.term, c.sqlcmd,"
        << " c.timestamp, c.fed_index, p.log_index, p.term"
        << " FROM logdb c, logdb p WHERE c.log_index = " << index
        << " AND p.log_index = " << prev_index;

    lr.set_callback();

    int rc = db->exec_rd(oss, &lr);

    lr.unset_callback();

    if ( lr.index != index )
    {
        rc = -1;
    }

    return rc;
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

void LogDB::get_last_record_index(unsigned int& _i, unsigned int& _t)
{
    pthread_mutex_lock(&mutex);

    _i = last_index;
    _t = last_term;

    pthread_mutex_unlock(&mutex);
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

int LogDB::get_raft_state(std::string &raft_xml)
{
    ostringstream oss;

    single_cb<std::string> cb;

    oss << "SELECT sqlcmd FROM logdb WHERE log_index = -1 AND term = -1";

    cb.set_callback(&raft_xml);

    int rc = db->exec_rd(oss, &cb);

    cb.unset_callback();

    if ( raft_xml.empty() )
    {
        rc = -1;
    }

    return rc;
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

int LogDB::update_raft_state(std::string& raft_xml)
{
    std::ostringstream oss;

    char * sql_db = db->escape_str(raft_xml.c_str());

    if ( sql_db == 0 )
    {
        return -1;
    }

    oss << "UPDATE logdb SET sqlcmd ='" << sql_db << "' WHERE log_index = -1";

    db->free_str(sql_db);

    return db->exec_wr(oss);
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

int LogDB::insert(int index, int term, const std::string& sql, time_t tstamp,
        int fed_index)
{
    std::ostringstream oss;

    std::string * zsql;

    zsql = one_util::zlib_compress(sql, true);

    if ( zsql == 0 )
    {
        return -1;
    }

    char * sql_db = db->escape_str(zsql->c_str());

    delete zsql;

    if ( sql_db == 0 )
    {
        return -1;
    }

    oss << "INSERT INTO " << table << " ("<< db_names <<") VALUES ("
        << index << "," << term << "," << "'" << sql_db << "'," << tstamp
        << "," << fed_index << ")";

    int rc = db->exec_wr(oss);

    if ( rc != 0 )
    {
        //Check for duplicate (leader retrying i.e. xmlrpc client timeout)
        LogDBRecord lr;

        if ( get_log_record(index, lr) == 0 )
        {
            NebulaLog::log("DBM", Log::ERROR, "Duplicated log record");
            rc = 0;
        }
        else
        {
            rc = -1;
        }
    }

    db->free_str(sql_db);

    return rc;
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

int LogDB::apply_log_record(LogDBRecord * lr)
{
    ostringstream oss_sql;

    oss_sql.str(lr->sql);

    int rc = db->exec_wr(oss_sql);

    if ( rc == 0 )
    {
        std::ostringstream oss;

        oss << "UPDATE logdb SET timestamp = " << time(0) << " WHERE "
            << "log_index = " << lr->index << " AND timestamp = 0";

        if ( db->exec_wr(oss) != 0 )
        {
            NebulaLog::log("DBM", Log::ERROR, "Cannot update log record");
        }

        last_applied = lr->index;
    }

    return rc;
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

int LogDB::insert_log_record(unsigned int term, std::ostringstream& sql,
        time_t timestamp, int fed_index)
{
    pthread_mutex_lock(&mutex);

    unsigned int index = next_index;

    int _fed_index;

    if ( fed_index == 0 )
    {
        _fed_index = index;
    }
    else
    {
        _fed_index = fed_index;
    }

    if ( insert(index, term, sql.str(), timestamp, _fed_index) != 0 )
    {
        NebulaLog::log("DBM", Log::ERROR, "Cannot insert log record in DB");

        pthread_mutex_unlock(&mutex);

        return -1;
    }

    last_index = next_index;

    last_term  = term;

    next_index++;

    if ( fed_index != -1 )
    {
        fed_log.insert(_fed_index);
    }

    pthread_mutex_unlock(&mutex);

    return index;
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

int LogDB::insert_log_record(unsigned int index, unsigned int term,
        std::ostringstream& sql, time_t timestamp, int fed_index)
{
    int rc;

    pthread_mutex_lock(&mutex);

    rc = insert(index, term, sql.str(), timestamp, fed_index);

    if ( rc == 0 )
    {
        if ( index > last_index )
        {
            last_index = index;

            last_term  = term;

            next_index = last_index + 1;
        }

        if ( fed_index != -1 )
        {
            fed_log.insert(fed_index);
        }
    }

    pthread_mutex_unlock(&mutex);

    return rc;
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

int LogDB::_exec_wr(ostringstream& cmd, int federated_index)
{
    int rc;

    RaftManager * raftm = Nebula::instance().get_raftm();

    // -------------------------------------------------------------------------
    // OpenNebula was started in solo mode
    // -------------------------------------------------------------------------
    if ( solo )
    {
        rc = db->exec_wr(cmd);

        if ( rc == 0 && Nebula::instance().is_federation_enabled() )
        {
            insert_log_record(0, cmd, time(0), federated_index);
        }

        return rc;
    }
    else if ( raftm == 0 || !raftm->is_leader() )
    {
        NebulaLog::log("DBM", Log::ERROR,"Tried to modify DB being a follower");
        return -1;
    }

    // -------------------------------------------------------------------------
    // Insert log entry in the database and replicate on followers
    // -------------------------------------------------------------------------
    int rindex = insert_log_record(raftm->get_term(), cmd, 0, federated_index);

    if ( rindex == -1 )
    {
        return -1;
    }

    ReplicaRequest rr(rindex);

    raftm->replicate_log(&rr);

    // Wait for completion
    rr.wait();

    if ( !raftm->is_leader() ) // Check we are still leaders before applying
    {
        NebulaLog::log("DBM", Log::ERROR, "Not applying log record, oned is"
                " now a follower");
        rc = -1;
    }
    else if ( rr.result == true ) //Record replicated on majority of followers
    {
		rc = apply_log_records(rindex);
    }
    else
    {
        std::ostringstream oss;

        oss << "Cannot replicate log record on followers: " << rr.message;

        NebulaLog::log("DBM", Log::ERROR, oss);

        rc = -1;
    }

    return rc;
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

int LogDB::delete_log_records(unsigned int start_index)
{
    std::ostringstream oss;
    int rc;

    pthread_mutex_lock(&mutex);

    oss << "DELETE FROM " << table << " WHERE log_index >= " << start_index;

    rc = db->exec_wr(oss);

    if ( rc == 0 )
    {
    	LogDBRecord lr;

        next_index = start_index;

        last_index = start_index - 1;

		if ( get_log_record(last_index, lr) == 0 )
        {
            last_term = lr.term;
        }
    }

    pthread_mutex_unlock(&mutex);

	return rc;
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

int LogDB::apply_log_records(unsigned int commit_index)
{
    pthread_mutex_lock(&mutex);

	while (last_applied < commit_index )
	{
    	LogDBRecord lr;

		if ( get_log_record(last_applied + 1, lr) != 0 )
		{
            pthread_mutex_unlock(&mutex);
			return -1;
		}

		if ( apply_log_record(&lr) != 0 )
		{
            pthread_mutex_unlock(&mutex);
			return -1;
		}
	}

    pthread_mutex_unlock(&mutex);

	return 0;
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

int LogDB::purge_log()
{
    std::ostringstream oss;

    pthread_mutex_lock(&mutex);

    if ( last_index < log_retention )
    {
        pthread_mutex_unlock(&mutex);
        return 0;
    }

    unsigned int delete_index = last_applied - log_retention;

    // keep the last "log_retention" records as well as those not applied to DB
    oss << "DELETE FROM logdb WHERE timestamp > 0 AND log_index >= 0 "
        << "AND log_index < "  << delete_index;

    int rc = db->exec_wr(oss);

    pthread_mutex_unlock(&mutex);

    return rc;
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */
int LogDB::index_cb(void *null, int num, char **values, char **names)
{
    if ( num == 0 || values == 0 || values[0] == 0 )
    {
        return -1;
    }

    fed_log.insert(atoi(values[0]));

    return 0;
}

void LogDB::build_federated_index()
{
    std::ostringstream oss;

    fed_log.clear();

    set_callback(static_cast<Callbackable::Callback>(&LogDB::index_cb), 0);

    oss << "SELECT fed_index FROM " << table << " WHERE fed_index != -1 ";

    db->exec_rd(oss, this);

    unset_callback();
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

int LogDB::last_federated()
{
    pthread_mutex_lock(&mutex);

    int findex = -1;

    if ( !fed_log.empty() )
    {
        set<int>::reverse_iterator rit;

        rit = fed_log.rbegin();

        findex = *rit;
    }

    pthread_mutex_unlock(&mutex);

    return findex;
}

/* -------------------------------------------------------------------------- */

int LogDB::previous_federated(int i)
{
    set<int>::iterator it;

    pthread_mutex_lock(&mutex);

    int findex = -1;

    it = fed_log.find(i);

    if ( it != fed_log.end() && it != fed_log.begin() )
    {
        findex = *(--it);
    }

    pthread_mutex_unlock(&mutex);

    return findex;
}

/* -------------------------------------------------------------------------- */

int LogDB::next_federated(int i)
{
    set<int>::iterator it;

    pthread_mutex_lock(&mutex);

    int findex = -1;

    it = fed_log.find(i);

    if ( it != fed_log.end() && it != --fed_log.end() )
    {
        findex = *(++it);
    }

    pthread_mutex_unlock(&mutex);

    return findex;
}

/* -------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------- */

int FedLogDB::exec_wr(ostringstream& cmd)
{
    FedReplicaManager * frm = Nebula::instance().get_frm();

    int rc = _logdb->exec_federated_wr(cmd);

    if ( rc != 0 )
    {
        return rc;
    }

    frm->replicate(cmd.str());

    return rc;
}

