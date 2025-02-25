/*
 * Copyright (C) 2016+     AzerothCore <www.azerothcore.org>
 * Copyright (C) 2008-2016 TrinityCore <http://www.trinitycore.org/>
 * Copyright (C) 2005-2009 MaNGOS <http://getmangos.com/>
 */


#include "Common.h"

#ifdef _WIN32
  #include <winsock2.h>
#endif
#include <mysql.h>
#include <mysqld_error.h>
#include <errmsg.h>
#include <chrono>
#include <thread>

#include "MySQLConnection.h"
#include "MySQLThreading.h"
#include "QueryResult.h"
#include "SQLOperation.h"
#include "PreparedStatement.h"
#include "DatabaseWorker.h"
#include "Timer.h"
#include "Log.h"

using namespace std::this_thread;
using namespace std::chrono;

MySQLConnection::MySQLConnection(MySQLConnectionInfo& connInfo) :
m_reconnecting(false),
m_prepareError(false),
m_queue(NULL),
m_worker(NULL),
m_Mysql(NULL),
m_connectionInfo(connInfo),
m_connectionFlags(CONNECTION_SYNCH)
{
}

MySQLConnection::MySQLConnection(ACE_Activation_Queue* queue, MySQLConnectionInfo& connInfo) :
m_reconnecting(false),
m_prepareError(false),
m_queue(queue),
m_Mysql(NULL),
m_connectionInfo(connInfo),
m_connectionFlags(CONNECTION_ASYNC)
{
    m_worker = new DatabaseWorker(m_queue, this);
}

MySQLConnection::~MySQLConnection()
{
    ASSERT (m_Mysql); /// MySQL context must be present at this point

    for (size_t i = 0; i < m_stmts.size(); ++i)
        delete m_stmts[i];

    mysql_close(m_Mysql);
}

void MySQLConnection::Close()
{
    /// Only close us if we're not operating
    delete this;
}

bool MySQLConnection::Open()
{
    MYSQL *mysqlInit;
    mysqlInit = mysql_init(NULL);
    if (!mysqlInit)
    {
        sLog->outError("Could not initialize Mysql connection to database `%s`", m_connectionInfo.database.c_str());
        return false;
    }

    int port;
    char const* unix_socket;
    //unsigned int timeout = 10;

    mysql_options(mysqlInit, MYSQL_SET_CHARSET_NAME, "utf8");
    //mysql_options(mysqlInit, MYSQL_OPT_READ_TIMEOUT, (char const*)&timeout);
    #ifdef _WIN32
    if (m_connectionInfo.host == ".")                                           // named pipe use option (Windows)
    {
        unsigned int opt = MYSQL_PROTOCOL_PIPE;
        mysql_options(mysqlInit, MYSQL_OPT_PROTOCOL, (char const*)&opt);
        port = 0;
        unix_socket = 0;
    }
    else                                                    // generic case
    {
        port = atoi(m_connectionInfo.port_or_socket.c_str());
        unix_socket = 0;
    }
    #else
    if (m_connectionInfo.host == ".")                                           // socket use option (Unix/Linux)
    {
        unsigned int opt = MYSQL_PROTOCOL_SOCKET;
        mysql_options(mysqlInit, MYSQL_OPT_PROTOCOL, (char const*)&opt);
        m_connectionInfo.host = "localhost";
        port = 0;
        unix_socket = m_connectionInfo.port_or_socket.c_str();
    }
    else                                                    // generic case
    {
        port = atoi(m_connectionInfo.port_or_socket.c_str());
        unix_socket = 0;
    }
    #endif

    // Possible improvement for future: make ATTEMPTS and SECONDS configurable values
    uint32 const ATTEMPTS = 180;
    uint32 const SECONDS = 10;

    uint32 count = 0;
    do {
        m_Mysql = mysql_real_connect(
            mysqlInit,
            m_connectionInfo.host.c_str(),
            m_connectionInfo.user.c_str(),
            m_connectionInfo.password.c_str(),
            m_connectionInfo.database.c_str(),
            port,
            unix_socket,
            0);

        if (m_Mysql)
        {
            if (!m_reconnecting)
            {
                sLog->outSQLDriver("MySQL client library: %s", mysql_get_client_info());
                sLog->outSQLDriver("MySQL server ver: %s ", mysql_get_server_info(m_Mysql));
                // MySQL version above 5.1 IS required in both client and server and there is no known issue with different versions above 5.1
                // if (mysql_get_server_version(m_Mysql) != mysql_get_client_version())
                //     sLog->outInfo(LOG_FILTER_SQL, "[WARNING] MySQL client/server version mismatch; may conflict with behaviour of prepared statements.");
            }

#if defined(ENABLE_EXTRAS) && defined(ENABLE_EXTRA_LOGS)
            sLog->outDetail("Connected to MySQL database at %s", m_connectionInfo.host.c_str());
#endif
            mysql_autocommit(m_Mysql, 1);

            // set connection properties to UTF8 to properly handle locales for different
            // server configs - core sends data in UTF8, so MySQL must expect UTF8 too
            mysql_set_character_set(m_Mysql, "utf8");
            return PrepareStatements();
        }
        else
        {
            count++;
            sLog->outError("Could not connect to MySQL database at %s: %s\n", m_connectionInfo.host.c_str(), mysql_error(mysqlInit));
            sLog->outError("Retrying in 10 seconds...\n\n");
            sleep_for(seconds(SECONDS));
        }
    } while (!m_Mysql && count < ATTEMPTS);

    sLog->outError(
        "Could not connect to MySQL database at %s: %s after %d attempts\n",
        m_connectionInfo.host.c_str(),
        mysql_error(mysqlInit),
        ATTEMPTS);
    mysql_close(mysqlInit);
    return false;
}

bool MySQLConnection::PrepareStatements()
{
    DoPrepareStatements();
    return !m_prepareError;
}

bool MySQLConnection::Execute(const char* sql)
{
    if (!m_Mysql)
        return false;

    {
        uint32 _s = 0;
        if (sLog->GetSQLDriverQueryLogging())
            _s = getMSTime();

        if (mysql_query(m_Mysql, sql))
        {
            uint32 lErrno = mysql_errno(m_Mysql);

            sLog->outSQLDriver("SQL: %s", sql);
            sLog->outSQLDriver("ERROR: [%u] %s", lErrno, mysql_error(m_Mysql));

            if (_HandleMySQLErrno(lErrno))  // If it returns true, an error was handled successfully (i.e. reconnection)
                return Execute(sql);       // Try again

            return false;
        }
        else if (sLog->GetSQLDriverQueryLogging())
        {
            sLog->outSQLDriver("[%u ms] SQL: %s", getMSTimeDiff(_s, getMSTime()), sql);
        }
    }

    return true;
}

bool MySQLConnection::Execute(PreparedStatement* stmt)
{
    if (!m_Mysql)
        return false;

    uint32 index = stmt->m_index;
    {
        MySQLPreparedStatement* m_mStmt = GetPreparedStatement(index);
        ASSERT(m_mStmt);            // Can only be null if preparation failed, server side error or bad query
        m_mStmt->m_stmt = stmt;     // Cross reference them for debug output
        stmt->m_stmt = m_mStmt;     // TODO: Cleaner way

        stmt->BindParameters();

        MYSQL_STMT* msql_STMT = m_mStmt->GetSTMT();
        MYSQL_BIND* msql_BIND = m_mStmt->GetBind();

        uint32 _s = 0;
        if (sLog->GetSQLDriverQueryLogging())
            _s = getMSTime();

        if (mysql_stmt_bind_param(msql_STMT, msql_BIND))
        {
            uint32 lErrno = mysql_errno(m_Mysql);
            sLog->outSQLDriver("SQL(p): %s\n [ERROR]: [%u] %s", m_mStmt->getQueryString(m_queries[index].first).c_str(), lErrno, mysql_stmt_error(msql_STMT));

            if (_HandleMySQLErrno(lErrno))  // If it returns true, an error was handled successfully (i.e. reconnection)
                return Execute(stmt);       // Try again

            m_mStmt->ClearParameters();
            return false;
        }

        if (mysql_stmt_execute(msql_STMT))
        {
            uint32 lErrno = mysql_errno(m_Mysql);
            sLog->outSQLDriver("SQL(p): %s\n [ERROR]: [%u] %s", m_mStmt->getQueryString(m_queries[index].first).c_str(), lErrno, mysql_stmt_error(msql_STMT));

            if (_HandleMySQLErrno(lErrno))  // If it returns true, an error was handled successfully (i.e. reconnection)
                return Execute(stmt);       // Try again

            m_mStmt->ClearParameters();
            return false;
        }

        if (sLog->GetSQLDriverQueryLogging())
            sLog->outSQLDriver("[%u ms] SQL(p): %s", getMSTimeDiff(_s, getMSTime()), m_mStmt->getQueryString(m_queries[index].first).c_str());

        m_mStmt->ClearParameters();
        return true;
    }
}

bool MySQLConnection::_Query(PreparedStatement* stmt, MYSQL_RES **pResult, uint64* pRowCount, uint32* pFieldCount)
{
    if (!m_Mysql)
        return false;

    uint32 index = stmt->m_index;
    {
        MySQLPreparedStatement* m_mStmt = GetPreparedStatement(index);
        ASSERT(m_mStmt);            // Can only be null if preparation failed, server side error or bad query
        m_mStmt->m_stmt = stmt;     // Cross reference them for debug output
        stmt->m_stmt = m_mStmt;     // TODO: Cleaner way

        stmt->BindParameters();

        MYSQL_STMT* msql_STMT = m_mStmt->GetSTMT();
        MYSQL_BIND* msql_BIND = m_mStmt->GetBind();

        uint32 _s = 0;
        if (sLog->GetSQLDriverQueryLogging())
            _s = getMSTime();

        if (mysql_stmt_bind_param(msql_STMT, msql_BIND))
        {
            uint32 lErrno = mysql_errno(m_Mysql);
            sLog->outSQLDriver("SQL(p): %s\n [ERROR]: [%u] %s", m_mStmt->getQueryString(m_queries[index].first).c_str(), lErrno, mysql_stmt_error(msql_STMT));

            if (_HandleMySQLErrno(lErrno))  // If it returns true, an error was handled successfully (i.e. reconnection)
                return _Query(stmt, pResult, pRowCount, pFieldCount);       // Try again

            m_mStmt->ClearParameters();
            return false;
        }

        if (mysql_stmt_execute(msql_STMT))
        {
            uint32 lErrno = mysql_errno(m_Mysql);
            sLog->outSQLDriver("SQL(p): %s\n [ERROR]: [%u] %s",
                m_mStmt->getQueryString(m_queries[index].first).c_str(), lErrno, mysql_stmt_error(msql_STMT));

            if (_HandleMySQLErrno(lErrno))  // If it returns true, an error was handled successfully (i.e. reconnection)
                return _Query(stmt, pResult, pRowCount, pFieldCount);      // Try again

            m_mStmt->ClearParameters();
            return false;
        }

        if (sLog->GetSQLDriverQueryLogging())
            sLog->outSQLDriver("[%u ms] SQL(p): %s", getMSTimeDiff(_s, getMSTime()), m_mStmt->getQueryString(m_queries[index].first).c_str());

        m_mStmt->ClearParameters();

        *pResult = mysql_stmt_result_metadata(msql_STMT);
        *pRowCount = mysql_stmt_num_rows(msql_STMT);
        *pFieldCount = mysql_stmt_field_count(msql_STMT);

        return true;

    }
}

ResultSet* MySQLConnection::Query(const char* sql)
{
    if (!sql)
        return NULL;

    MYSQL_RES *result = NULL;
    MYSQL_FIELD *fields = NULL;
    uint64 rowCount = 0;
    uint32 fieldCount = 0;

    if (!_Query(sql, &result, &fields, &rowCount, &fieldCount))
        return NULL;

    return new ResultSet(result, fields, rowCount, fieldCount);
}

bool MySQLConnection::_Query(const char *sql, MYSQL_RES **pResult, MYSQL_FIELD **pFields, uint64* pRowCount, uint32* pFieldCount)
{
    if (!m_Mysql)
        return false;

    {
        uint32 _s = 0;
        if (sLog->GetSQLDriverQueryLogging())
            _s = getMSTime();

        if (mysql_query(m_Mysql, sql))
        {
            uint32 lErrno = mysql_errno(m_Mysql);
            sLog->outSQLDriver("SQL: %s", sql);
            sLog->outSQLDriver("ERROR: [%u] %s", lErrno, mysql_error(m_Mysql));

            if (_HandleMySQLErrno(lErrno))      // If it returns true, an error was handled successfully (i.e. reconnection)
                return _Query(sql, pResult, pFields, pRowCount, pFieldCount);    // We try again

            return false;
        }
        else if (sLog->GetSQLDriverQueryLogging())
        {
            sLog->outSQLDriver("[%u ms] SQL: %s", getMSTimeDiff(_s, getMSTime()), sql);
        }

        *pResult = mysql_store_result(m_Mysql);
        *pRowCount = mysql_affected_rows(m_Mysql);
        *pFieldCount = mysql_field_count(m_Mysql);
    }

    if (!*pResult )
        return false;

    if (!*pRowCount)
    {
        mysql_free_result(*pResult);
        return false;
    }

    *pFields = mysql_fetch_fields(*pResult);

    return true;
}

void MySQLConnection::BeginTransaction()
{
    Execute("START TRANSACTION");
}

void MySQLConnection::RollbackTransaction()
{
    Execute("ROLLBACK");
}

void MySQLConnection::CommitTransaction()
{
    Execute("COMMIT");
}

bool MySQLConnection::ExecuteTransaction(SQLTransaction& transaction)
{
    std::list<SQLElementData> const& queries = transaction->m_queries;
    if (queries.empty())
        return false;

    BeginTransaction();

    std::list<SQLElementData>::const_iterator itr;
    for (itr = queries.begin(); itr != queries.end(); ++itr)
    {
        SQLElementData const& data = *itr;
        switch (itr->type)
        {
            case SQL_ELEMENT_PREPARED:
            {
                PreparedStatement* stmt = data.element.stmt;
                ASSERT(stmt);
                if (!Execute(stmt))
                {
                    sLog->outSQLDriver("[Warning] Transaction aborted. %u queries not executed.", (uint32)queries.size());
                    RollbackTransaction();
                    return false;
                }
            }
            break;
            case SQL_ELEMENT_RAW:
            {
                const char* sql = data.element.query;
                ASSERT(sql);
                if (!Execute(sql))
                {
                    sLog->outSQLDriver("[Warning] Transaction aborted. %u queries not executed.", (uint32)queries.size());
                    RollbackTransaction();
                    return false;
                }
            }
            break;
        }
    }

    // we might encounter errors during certain queries, and depending on the kind of error
    // we might want to restart the transaction. So to prevent data loss, we only clean up when it's all done.
    // This is done in calling functions DatabaseWorkerPool<T>::DirectCommitTransaction and TransactionTask::Execute,
    // and not while iterating over every element.

    CommitTransaction();
    return true;
}

MySQLPreparedStatement* MySQLConnection::GetPreparedStatement(uint32 index)
{
    ASSERT(index < m_stmts.size());
    MySQLPreparedStatement* ret = m_stmts[index];
    if (!ret)
        sLog->outSQLDriver("ERROR: Could not fetch prepared statement %u on database `%s`, connection type: %s.",
            index, m_connectionInfo.database.c_str(), (m_connectionFlags & CONNECTION_ASYNC) ? "asynchronous" : "synchronous");

    return ret;
}

void MySQLConnection::PrepareStatement(uint32 index, const char* sql, ConnectionFlags flags)
{
    m_queries.insert(PreparedStatementMap::value_type(index, std::make_pair(sql, flags)));

    // For reconnection case
    if (m_reconnecting)
        delete m_stmts[index];

    // Check if specified query should be prepared on this connection
    // i.e. don't prepare async statements on synchronous connections
    // to save memory that will not be used.
    if (!(m_connectionFlags & flags))
    {
        m_stmts[index] = NULL;
        return;
    }

    MYSQL_STMT* stmt = mysql_stmt_init(m_Mysql);
    if (!stmt)
    {
        sLog->outSQLDriver("[ERROR]: In mysql_stmt_init() id: %u, sql: \"%s\"", index, sql);
        sLog->outSQLDriver("[ERROR]: %s", mysql_error(m_Mysql));
        m_prepareError = true;
    }
    else
    {
        if (mysql_stmt_prepare(stmt, sql, static_cast<unsigned long>(strlen(sql))))
        {
            sLog->outSQLDriver("[ERROR]: In mysql_stmt_prepare() id: %u, sql: \"%s\"", index, sql);
            sLog->outSQLDriver("[ERROR]: %s", mysql_stmt_error(stmt));
            mysql_stmt_close(stmt);
            m_prepareError = true;
        }
        else
        {
            MySQLPreparedStatement* mStmt = new MySQLPreparedStatement(stmt);
            m_stmts[index] = mStmt;
        }
    }
}

PreparedResultSet* MySQLConnection::Query(PreparedStatement* stmt)
{
    MYSQL_RES *result = NULL;
    uint64 rowCount = 0;
    uint32 fieldCount = 0;

    if (!_Query(stmt, &result, &rowCount, &fieldCount))
        return NULL;

    if (mysql_more_results(m_Mysql))
    {
        mysql_next_result(m_Mysql);
    }
    return new PreparedResultSet(stmt->m_stmt->GetSTMT(), result, rowCount, fieldCount);
}

bool MySQLConnection::_HandleMySQLErrno(uint32 errNo)
{
    switch (errNo)
    {
        case CR_SERVER_GONE_ERROR:
        case CR_SERVER_LOST:
        case CR_SERVER_LOST_EXTENDED:
#if !defined MARIADB
        case CR_INVALID_CONN_HANDLE:
#endif
        {
            m_reconnecting = true;
            uint64 oldThreadId = mysql_thread_id(GetHandle());
            mysql_close(GetHandle());
            if (this->Open())                           // Don't remove 'this' pointer unless you want to skip loading all prepared statements....
            {
                sLog->outSQLDriver("Connection to the MySQL server is active.");
                if (oldThreadId != mysql_thread_id(GetHandle()))
                    sLog->outSQLDriver("Successfully reconnected to %s @%s:%s (%s).",
                        m_connectionInfo.database.c_str(), m_connectionInfo.host.c_str(), m_connectionInfo.port_or_socket.c_str(),
                            (m_connectionFlags & CONNECTION_ASYNC) ? "asynchronous" : "synchronous");

                m_reconnecting = false;
                return true;
            }

            uint32 lErrno = mysql_errno(GetHandle());   // It's possible this attempted reconnect throws 2006 at us. To prevent crazy recursive calls, sleep here.
            ACE_OS::sleep(3);                           // Sleep 3 seconds
            return _HandleMySQLErrno(lErrno);           // Call self (recursive)
        }

        case ER_LOCK_DEADLOCK:
            return false;    // Implemented in TransactionTask::Execute and DatabaseWorkerPool<T>::DirectCommitTransaction
        // Query related errors - skip query
        case ER_WRONG_VALUE_COUNT:
        case ER_DUP_ENTRY:
            return false;

        // Outdated table or database structure - terminate core
        case ER_BAD_FIELD_ERROR:
        case ER_NO_SUCH_TABLE:
            sLog->outError("Your database structure is not up to date. Please make sure you've executed all queries in the sql/updates folders.");
            ACE_OS::sleep(10);
            std::abort();
            return false;
        case ER_PARSE_ERROR:
            sLog->outError("Error while parsing SQL. Core fix required.");
            ACE_OS::sleep(10);
            std::abort();
            return false;
        default:
            sLog->outError("Unhandled MySQL errno %u. Unexpected behaviour possible.", errNo);
            return false;
    }
}
