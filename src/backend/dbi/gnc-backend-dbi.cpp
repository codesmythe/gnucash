/********************************************************************
 * gnc-backend-dbi.c: load and save data to SQL via libdbi          *
 *                                                                  *
 * This program is free software; you can redistribute it and/or    *
 * modify it under the terms of the GNU General Public License as   *
 * published by the Free Software Foundation; either version 2 of   *
 * the License, or (at your option) any later version.              *
 *                                                                  *
 * This program is distributed in the hope that it will be useful,  *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of   *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the    *
 * GNU General Public License for more details.                     *
 *                                                                  *
 * You should have received a copy of the GNU General Public License*
 * along with this program; if not, contact:                        *
 *                                                                  *
 * Free Software Foundation           Voice:  +1-617-542-5942       *
 * 51 Franklin Street, Fifth Floor    Fax:    +1-617-542-2652       *
 * Boston, MA  02110-1301,  USA       gnu@gnu.org                   *
\********************************************************************/
/** @file gnc-backend-dbi.c
 *  @brief load and save data to SQL
 *  @author Copyright (c) 2006-2008 Phil Longstaff <plongstaff@rogers.com>
 *
 * This file implements the top-level QofBackend API for saving/
 * restoring data to/from an SQL db using libdbi
 */
extern "C"
{
#include "config.h"

#include <platform.h>
#ifdef __STRICT_ANSI__
#undef __STRICT_ANSI__
#define __STRICT_ANSI_UNSET__ 1
#endif
#ifdef _NO_OLDNAMES
#undef _NO_OLDNAMES
#endif
#ifdef _UWIN
#undef _UWIN
#endif
#if PLATFORM(WINDOWS)
#include <windows.h>
#endif

#include <errno.h>
#include <glib.h>
#include <glib/gstdio.h>

#include "qof.h"
#include "qofquery-p.h"
#include "qofquerycore-p.h"
#include "Account.h"
#include "TransLog.h"
#include "gnc-engine.h"
#include "SX-book.h"
#include "Recurrence.h"

#include "gnc-uri-utils.h"
#include "gnc-filepath-utils.h"
#include <gnc-path.h>
#include "gnc-locale-utils.h"

#include "gnc-prefs.h"

#ifdef S_SPLINT_S
#include "splint-defs.h"
#endif

#ifdef G_OS_WIN32
#include <winsock2.h>
#define GETPID() GetCurrentProcessId()
#else
#include <limits.h>
#include <unistd.h>
#define GETPID() getpid()
#endif

    /* For direct access to dbi data structs, sadly needed for datetime */
#include <dbi/dbi-dev.h>
}
#include <boost/regex.hpp>
#include <string>

#include <gnc-datetime.hpp>
#include <gnc-backend-prov.hpp>
#include "gnc-backend-dbi.h"
#include "gnc-backend-dbi.hpp"

#if PLATFORM(WINDOWS)
#ifdef __STRICT_ANSI_UNSET__
#undef __STRICT_ANSI_UNSET__
#define __STRICT_ANSI__ 1
#endif
#endif

#if LIBDBI_VERSION >= 900
#define HAVE_LIBDBI_R 1
#define HAVE_LIBDBI_TO_LONGLONG 1
static dbi_inst dbi_instance = nullptr;
#else
#define HAVE_LIBDBI_R 0
#define HAVE_LIBDBI_TO_LONGLONG 0
#endif

#define GNC_HOST_NAME_MAX 255
#define TRANSACTION_NAME "trans"

static QofLogModule log_module = G_LOG_DOMAIN;

static gchar lock_table[] = "gnclock";

#define FILE_URI_TYPE "file"
#define FILE_URI_PREFIX (FILE_URI_TYPE "://")
#define SQLITE3_URI_TYPE "sqlite3"
#define SQLITE3_URI_PREFIX (SQLITE3_URI_TYPE "://")
#define PGSQL_DEFAULT_PORT 5432

#define SQLITE3_TIMESPEC_STR_FORMAT "%04d%02d%02d%02d%02d%02d"
#define MYSQL_TIMESPEC_STR_FORMAT "%04d%02d%02d%02d%02d%02d"
#define PGSQL_TIMESPEC_STR_FORMAT "%04d%02d%02d %02d%02d%02d"

static gboolean gnc_dbi_lock_database (QofBackend *qbe, gboolean ignore_lock);
static void gnc_dbi_unlock (QofBackend *qbe);
static gboolean save_may_clobber_data (QofBackend* qbe);

static std::string create_index_ddl (const GncSqlConnection* conn,
                                     const std::string& index_name,
                                     const std::string& table_name,
                                     const EntryVec& col_table);
static GncDbiTestResult conn_test_dbi_library (dbi_conn conn);

#define DBI_MAX_CONN_ATTEMPTS 5
enum class DbType
{
    DBI_SQLITE,
    DBI_MYSQL,
    DBI_PGSQL
};


template <DbType T>
class GncDbiProviderImpl : public GncDbiProvider
{
public:
    std::string create_table_ddl(const GncSqlConnection* conn,
                                 const std::string& table_name,
                                 const ColVec& info_vec);
    std::vector<std::string> get_table_list(dbi_conn conn,
                                            const std::string& dbname);
    void append_col_def(std::string& ddl, const GncSqlColumnInfo& info);
    std::vector<std::string> get_index_list (dbi_conn conn);
    void drop_index(dbi_conn conn, const std::string& index);
};

template <DbType T>
class QofDbiBackendProvider : public QofBackendProvider
{
public:
    QofDbiBackendProvider (const char* name, const char* type) :
        QofBackendProvider {name, type} {}
    QofDbiBackendProvider(QofDbiBackendProvider&) = delete;
    QofDbiBackendProvider operator=(QofDbiBackendProvider&) = delete;
    QofDbiBackendProvider(QofDbiBackendProvider&&) = delete;
    QofDbiBackendProvider operator=(QofDbiBackendProvider&&) = delete;
    ~QofDbiBackendProvider () = default;
    QofBackend* create_backend(void);
    bool type_check(const char* type) { return true; }
};

/* ================================================================= */

/* Check if the dbi connection is valid. If not attempt to re-establish it
 * Returns TRUE is there is a valid connection in the end or FALSE otherwise
 */
bool
GncDbiSqlConnection::verify () noexcept
{
    if (m_conn_ok)
        return true;

    /* We attempt to connect only once here. The error function will
     * automatically re-attempt up until DBI_MAX_CONN_ATTEMPTS time to connect
     * if this call fails.  After all these attempts, conn_ok will indicate if
     * there is a valid connection or not.
     */
    init_error ();
    m_conn_ok = true;
    (void)dbi_conn_connect (m_conn);

    return m_conn_ok;
}

bool
GncDbiSqlConnection::retry_connection(const char* msg)
    noexcept
{
    while (m_retry && m_error_repeat <= DBI_MAX_CONN_ATTEMPTS)
    {
        m_conn_ok = false;
        if (dbi_conn_connect(m_conn) == 0)
        {
            init_error();
            m_conn_ok = true;
            return true;
        }
#ifdef G_OS_WIN32
        const guint backoff_msecs = 1;
        Sleep (backoff_msecs * 2 << ++m_error_repeat);
#else
        const guint backoff_usecs = 1000;
        usleep (backoff_usecs * 2 << ++m_error_repeat);
#endif
        PINFO ("DBI error: %s - Reconnecting...\n", msg);

    }
    PERR ("DBI error: %s - Giving up after %d consecutive attempts.\n", msg,
                DBI_MAX_CONN_ATTEMPTS);
    m_conn_ok = false;
    return false;
}

/* ================================================================= */

static void
create_tables(const OBEEntry& entry, GncDbiBackend* be)
{
    std::string type;
    GncSqlObjectBackendPtr obe = nullptr;
    std::tie(type, obe) = entry;
    g_return_if_fail(obe->is_version (GNC_SQL_BACKEND_VERSION));

    obe->create_tables (&be->sql_be);
}

static void
sqlite3_error_fn (dbi_conn conn, void* user_data)
{
    const gchar* msg;
    GncDbiBackend *be = static_cast<decltype(be)>(user_data);
/* FIXME: GncSqlConnection doesn't have the error calls so we have to dynamic_cast from the connection stored in GncSqlBackend. Yuck. */
    GncDbiSqlConnection *dbi_conn =
        dynamic_cast<decltype(dbi_conn)>(be->sql_be.conn);
    int errnum = dbi_conn_error (conn, &msg);
    PERR ("DBI error: %s\n", msg);
    dbi_conn->set_error (ERR_BACKEND_MISC, 0, false);
}

static void
gnc_dbi_sqlite3_session_begin (QofBackend* qbe, QofSession* session,
                               const gchar* book_id, gboolean ignore_lock,
                               gboolean create, gboolean force)
{
    GncDbiBackend* be = (GncDbiBackend*)qbe;
    gint result;
    gchar* dirname = nullptr;
    gchar* basename = nullptr;
    gchar* filepath = nullptr;
    const char* msg = nullptr;
    gboolean file_exists;
    GncDbiTestResult dbi_test_result = GNC_DBI_PASS;

    g_return_if_fail (qbe != nullptr);
    g_return_if_fail (session != nullptr);
    g_return_if_fail (book_id != nullptr);

    ENTER (" ");

    /* Remove uri type if present */
    filepath = gnc_uri_get_path (book_id);
    GFileTest ftest = static_cast<decltype (ftest)> (
        G_FILE_TEST_IS_REGULAR | G_FILE_TEST_EXISTS) ;
    file_exists = g_file_test (filepath, ftest);
    if (!create && !file_exists)
    {
        qof_backend_set_error (qbe, ERR_FILEIO_FILE_NOT_FOUND);
        qof_backend_set_message (qbe, "Sqlite3 file %s not found", filepath);
        PWARN ("Sqlite3 file %s not found", filepath);
        goto exit;
    }

    if (create && !force && file_exists)
    {
        qof_backend_set_error (qbe, ERR_BACKEND_STORE_EXISTS);
        msg = "Might clobber, no force";
        PWARN ("%s", msg);
        goto exit;
    }


    if (be->conn != nullptr)
    {
        dbi_conn_close (be->conn);
    }

#if HAVE_LIBDBI_R
    if (dbi_instance)
        be->conn = dbi_conn_new_r ("sqlite3", dbi_instance);
    else
        PERR ("Attempt to connect with an uninitialized dbi_instance");
#else
    be->conn = dbi_conn_new ("sqlite3");
#endif

    if (be->conn == nullptr)
    {
        PERR ("Unable to create sqlite3 dbi connection\n");
        qof_backend_set_error (qbe, ERR_BACKEND_BAD_URL);
        goto exit;
    }

    dirname = g_path_get_dirname (filepath);
    basename = g_path_get_basename (filepath);
    dbi_conn_error_handler (be->conn, sqlite3_error_fn, be);
    /* dbi-sqlite3 documentation says that sqlite3 doesn't take a "host" option */
    result = dbi_conn_set_option (be->conn, "host", "localhost");
    if (result < 0)
    {
        PERR ("Error setting 'host' option\n");
        qof_backend_set_error (qbe, ERR_BACKEND_SERVER_ERR);
        goto exit;
    }
    result = dbi_conn_set_option (be->conn, "dbname", basename);
    if (result < 0)
    {
        PERR ("Error setting 'dbname' option\n");
        qof_backend_set_error (qbe, ERR_BACKEND_SERVER_ERR);
        goto exit;
    }
    result = dbi_conn_set_option (be->conn, "sqlite3_dbdir", dirname);
    if (result < 0)
    {
        PERR ("Error setting 'sqlite3_dbdir' option\n");
        qof_backend_set_error (qbe, ERR_BACKEND_SERVER_ERR);
        goto exit;
    }
    result = dbi_conn_connect (be->conn);

    if (result < 0)
    {
        PERR ("Unable to connect to %s: %d\n", book_id, result);
        qof_backend_set_error (qbe, ERR_BACKEND_BAD_URL);
        goto exit;
    }

    dbi_test_result = conn_test_dbi_library (be->conn);
    switch (dbi_test_result)
    {
    case GNC_DBI_PASS:
        break;

    case GNC_DBI_FAIL_SETUP:
        qof_backend_set_error (qbe, ERR_SQL_DBI_UNTESTABLE);
        qof_backend_set_message (qbe,
                                 "SQLite3: Failed to setup for large number test");
        break;

    case GNC_DBI_FAIL_TEST:
        qof_backend_set_error (qbe, ERR_SQL_BAD_DBI);
        qof_backend_set_message (qbe,
                                 "SQLite3 DBI library fails large number test");
        break;
    }
    if (dbi_test_result != GNC_DBI_PASS)
    {
        if (create && !file_exists)   /* File didn't exist before, but it */
        {
            /* does now, and we don't want to */
            dbi_conn_close (be->conn); /* leave it lying around. */
            be->conn = nullptr;
            g_unlink (filepath);
        }
        msg = "Bad DBI Library";
        goto exit;
    }
    if (!gnc_dbi_lock_database (qbe, ignore_lock))
    {
        qof_backend_set_error (qbe, ERR_BACKEND_LOCKED);
        msg = "Locked";
        goto exit;
    }

    if (be->sql_be.conn != nullptr)
    {
        delete (be->sql_be.conn);
    }
    be->sql_be.conn = new GncDbiSqlConnection (new GncDbiProviderImpl<DbType::DBI_SQLITE>,
                                               qbe, be->conn);
    be->sql_be.timespec_format = SQLITE3_TIMESPEC_STR_FORMAT;

    /* We should now have a proper session set up.
     * Let's start logging */
    xaccLogSetBaseName (filepath);
    PINFO ("logpath=%s", filepath ? filepath : "(null)");

exit:
    if (filepath != nullptr) g_free (filepath);
    if (basename != nullptr) g_free (basename);
    if (dirname != nullptr) g_free (dirname);
    LEAVE ("%s", msg);
}

template<> std::vector<std::string>
GncDbiProviderImpl<DbType::DBI_SQLITE>::get_index_list (dbi_conn conn)
{
    std::vector<std::string> retval;
    const char* errmsg;
    dbi_result result = dbi_conn_query (conn,
                                        "SELECT name FROM sqlite_master WHERE type = 'index' AND name NOT LIKE 'sqlite_autoindex%'");
    if (dbi_conn_error (conn, &errmsg) != DBI_ERROR_NONE)
    {
        PWARN ("Index Table Retrieval Error: %s\n", errmsg);
        return retval;
    }
    while (dbi_result_next_row (result) != 0)
    {
        std::string index_name {dbi_result_get_string_idx (result, 1)};
        retval.push_back(index_name);
    }
    dbi_result_free (result);
    return retval;
}

template <DbType P> void
GncDbiProviderImpl<P>::drop_index(dbi_conn conn, const std::string& index)
{
    dbi_result result = dbi_conn_queryf (conn, "DROP INDEX %s", index.c_str());
    if (result)
        dbi_result_free (result);
}

static void
mysql_error_fn (dbi_conn conn, void* user_data)
{
    GncDbiBackend* be = (GncDbiBackend*)user_data;
    GncDbiSqlConnection* dbi_conn =
        dynamic_cast<decltype(dbi_conn)>(be->sql_be.conn);
    const char* msg;

    auto err_num = dbi_conn_error (conn, &msg);

    /* Note: the sql connection may not have been initialized yet
     *       so let's be careful with using it
     */

    /* Database doesn't exist. When this error is triggered the
     * GncDbiSqlConnection may not exist yet either, so don't use it here
     */
    if (err_num == 1049)            // Database doesn't exist
    {
        PINFO ("DBI error: %s\n", msg);
        be->exists = FALSE;
        return;
    }

    /* All the other error handling code assumes the GncDbiSqlConnection
     *  has been initialized. So let's assert it exits here, otherwise
     * simply return.
     */
    if (!dbi_conn)
    {
        PINFO ("DBI error: %s\n", msg);
        PINFO ("Note: GbcDbiSqlConnection not yet initialized. Skipping further error processing.");
        return;
    }

    /* Test for other errors */
    if (err_num == 2006)       // Server has gone away
    {
        PINFO ("DBI error: %s - Reconnecting...\n", msg);
        if (dbi_conn)
            dbi_conn->set_error (ERR_BACKEND_CONN_LOST, 1, true);
        dbi_conn->retry_connection(msg);
    }
    else if (err_num == 2003)       // Unable to connect
    {
        dbi_conn->set_error (ERR_BACKEND_CANT_CONNECT, 1, true);
        dbi_conn->retry_connection (msg);
    }
    else                            // Any other error
    {
        PERR ("DBI error: %s\n", msg);
        if (dbi_conn)
            dbi_conn->set_error (ERR_BACKEND_MISC, 0, FALSE);
    }
}

/**
 * Sets standard db options in a dbi_conn.
 *
 * @param qbe QOF backend
 * @param conn dbi_conn connection
 * @param host Hostname
 * @param port Port number
 * @param dbname Database name
 * @param username User name
 * @param password Password
 * @return TRUE if successful, FALSE if error
 */
static gboolean
set_standard_connection_options (QofBackend* qbe, dbi_conn conn,
                                 const gchar* host, int port,
                                 const gchar* dbname, const gchar* username, const gchar* password)
{
    gint result;

    result = dbi_conn_set_option (conn, "host", host);
    if (result < 0)
    {
        PERR ("Error setting 'host' option\n");
        qof_backend_set_error (qbe, ERR_BACKEND_SERVER_ERR);
        return FALSE;
    }
    result = dbi_conn_set_option_numeric (conn, "port", port);
    if (result < 0)
    {
        PERR ("Error setting 'port' option\n");
        qof_backend_set_error (qbe, ERR_BACKEND_SERVER_ERR);
        return FALSE;
    }
    result = dbi_conn_set_option (conn, "dbname", dbname);
    if (result < 0)
    {
        PERR ("Error setting 'dbname' option\n");
        qof_backend_set_error (qbe, ERR_BACKEND_SERVER_ERR);
        return FALSE;
    }
    result = dbi_conn_set_option (conn, "username", username);
    if (result < 0)
    {
        PERR ("Error setting 'username' option\n");
        qof_backend_set_error (qbe, ERR_BACKEND_SERVER_ERR);
        return FALSE;
    }
    result = dbi_conn_set_option (conn, "password", password);
    if (result < 0)
    {
        PERR ("Error setting 'password' option\n");
        qof_backend_set_error (qbe, ERR_BACKEND_SERVER_ERR);
        return FALSE;
    }

    result = dbi_conn_set_option (conn, "encoding", "UTF-8");
    if (result < 0)
    {
        PERR ("Error setting 'encoding' option\n");
        qof_backend_set_error (qbe, ERR_BACKEND_SERVER_ERR);
        return FALSE;
    }

    return TRUE;
}


static gboolean
gnc_dbi_lock_database (QofBackend* qbe, gboolean ignore_lock)
{

    GncDbiBackend* qe = (GncDbiBackend*)qbe;
    dbi_conn dcon = qe->conn;
    dbi_result result;
    const gchar* dbname = dbi_conn_get_option (dcon, "dbname");
    /* Create the table if it doesn't exist */
    result = dbi_conn_get_table_list (dcon, dbname, lock_table);
    if (! (result && dbi_result_get_numrows (result)))
    {
        if (result)
        {
            dbi_result_free (result);
            result = nullptr;
        }
        result = dbi_conn_queryf (dcon,
                                  "CREATE TABLE %s ( Hostname varchar(%d), PID int )", lock_table,
                                  GNC_HOST_NAME_MAX);
        if (dbi_conn_error (dcon, nullptr))
        {
            const gchar* errstr;
            dbi_conn_error (dcon, &errstr);
            PERR ("Error %s creating lock table", errstr);
            qof_backend_set_error (qbe, ERR_BACKEND_SERVER_ERR);
            if (result)
            {
                dbi_result_free (result);
                result = nullptr;
            }
            return FALSE;
        }
        if (result)
        {
            dbi_result_free (result);
            result = nullptr;
        }
    }
    if (result)
    {
        dbi_result_free (result);
        result = nullptr;
    }

    /* Protect everything with a single transaction to prevent races */
    if ((result = dbi_conn_query (dcon, "BEGIN")))
    {
        /* Check for an existing entry; delete it if ignore_lock is true, otherwise fail */
        gchar hostname[ GNC_HOST_NAME_MAX + 1 ];
        if (result)
        {
            dbi_result_free (result);
            result = nullptr;
        }
        result = dbi_conn_queryf (dcon, "SELECT * FROM %s", lock_table);
        if (result && dbi_result_get_numrows (result))
        {
            dbi_result_free (result);
            result = nullptr;
            if (!ignore_lock)
            {
                qof_backend_set_error (qbe, ERR_BACKEND_LOCKED);
                /* FIXME: After enhancing the qof_backend_error mechanism, report in the dialog what is the hostname of the machine holding the lock. */
                dbi_conn_query (dcon, "ROLLBACK");
                return FALSE;
            }
            result = dbi_conn_queryf (dcon, "DELETE FROM %s", lock_table);
            if (!result)
            {
                qof_backend_set_error (qbe, ERR_BACKEND_SERVER_ERR);
                qof_backend_set_message (qbe, "Failed to delete lock record");
                result = dbi_conn_query (dcon, "ROLLBACK");
                if (result)
                {
                    dbi_result_free (result);
                    result = nullptr;
                }
                return FALSE;
            }
            if (result)
            {
                dbi_result_free (result);
                result = nullptr;
            }
        }
        /* Add an entry and commit the transaction */
        memset (hostname, 0, sizeof (hostname));
        gethostname (hostname, GNC_HOST_NAME_MAX);
        result = dbi_conn_queryf (dcon,
                                  "INSERT INTO %s VALUES ('%s', '%d')",
                                  lock_table, hostname, (int)GETPID ());
        if (!result)
        {
            qof_backend_set_error (qbe, ERR_BACKEND_SERVER_ERR);
            qof_backend_set_message (qbe, "Failed to create lock record");
            result = dbi_conn_query (dcon, "ROLLBACK");
            if (result)
            {
                dbi_result_free (result);
                result = nullptr;
            }
            return FALSE;
        }
        if (result)
        {
            dbi_result_free (result);
            result = nullptr;
        }
        result = dbi_conn_query (dcon, "COMMIT");
        if (result)
        {
            dbi_result_free (result);
            result = nullptr;
        }
        return TRUE;
    }
    /* Couldn't get a transaction (probably couldn't get a lock), so fail */
    qof_backend_set_error (qbe, ERR_BACKEND_SERVER_ERR);
    qof_backend_set_message (qbe, "SQL Backend failed to obtain a transaction");
    if (result)
    {
        dbi_result_free (result);
        result = nullptr;
    }
    return FALSE;
}
static void
gnc_dbi_unlock (QofBackend* qbe)
{
    GncDbiBackend* qe = (GncDbiBackend*)qbe;
    dbi_conn dcon = qe->conn;
    dbi_result result;
    const gchar* dbname = nullptr;

    g_return_if_fail (dcon != nullptr);
    g_return_if_fail (dbi_conn_error (dcon, nullptr) == 0);

    dbname = dbi_conn_get_option (dcon, "dbname");
    /* Check if the lock table exists */
    g_return_if_fail (dbname != nullptr);
    result = dbi_conn_get_table_list (dcon, dbname, lock_table);
    if (! (result && dbi_result_get_numrows (result)))
    {
        if (result)
        {
            dbi_result_free (result);
            result = nullptr;
        }
        PWARN ("No lock table in database, so not unlocking it.");
        return;
    }
    dbi_result_free (result);

    result = dbi_conn_query (dcon, "BEGIN");
    if (result)
    {
        /* Delete the entry if it's our hostname and PID */
        gchar hostname[ GNC_HOST_NAME_MAX + 1 ];

        dbi_result_free (result);
        result = nullptr;
        memset (hostname, 0, sizeof (hostname));
        gethostname (hostname, GNC_HOST_NAME_MAX);
        result = dbi_conn_queryf (dcon,
                                  "SELECT * FROM %s WHERE Hostname = '%s' AND PID = '%d'", lock_table, hostname,
                                  (int)GETPID ());
        if (result && dbi_result_get_numrows (result))
        {
            if (result)
            {
                dbi_result_free (result);
                result = nullptr;
            }
            result = dbi_conn_queryf (dcon, "DELETE FROM %s", lock_table);
            if (!result)
            {
                PERR ("Failed to delete the lock entry");
                qof_backend_set_error (qbe, ERR_BACKEND_SERVER_ERR);
                result = dbi_conn_query (dcon, "ROLLBACK");
                if (result)
                {
                    dbi_result_free (result);
                    result = nullptr;
                }
                return;
            }
            else
            {
                dbi_result_free (result);
                result = nullptr;
            }
            result = dbi_conn_query (dcon, "COMMIT");
            if (result)
            {
                dbi_result_free (result);
                result = nullptr;
            }
            return;
        }
        result = dbi_conn_query (dcon, "ROLLBACK");
        if (result)
        {
            dbi_result_free (result);
            result = nullptr;
        }
        PWARN ("There was no lock entry in the Lock table");
        return;
    }
    if (result)
    {
        dbi_result_free (result);
        result = nullptr;
    }
    PWARN ("Unable to get a lock on LOCK, so failed to clear the lock entry.");
    qof_backend_set_error (qbe, ERR_BACKEND_SERVER_ERR);
}

#define SQL_OPTION_TO_REMOVE "NO_ZERO_DATE"

/* Given an sql_options string returns a copy of the string adjusted as
 * necessary.  In particular if string the contains SQL_OPTION_TO_REMOVE it is
 * removed along with comma separator.
 */
std::string
adjust_sql_options_string(const std::string& str)
{
/* Regex that finds the SQL_OPTION_TO_REMOVE as the first, last, or middle of a
 * comma-delimited list.
 */
    boost::regex reg{"(?:," SQL_OPTION_TO_REMOVE "$|\\b"
            SQL_OPTION_TO_REMOVE "\\b,?)"};
    return regex_replace(str, reg, std::string{""});
}

/* checks mysql sql_options and adjusts if necessary */
static void
adjust_sql_options (dbi_conn connection)
{
    dbi_result result = dbi_conn_query( connection, "SELECT @@sql_mode");
    if (result)
    {
        dbi_result_first_row(result);
        std::string str{dbi_result_get_string_idx(result, 1)};
        if (str.empty())
        {
            const char* errmsg;
            int err = dbi_conn_error(connection, &errmsg);
            if (err)
                PERR("Unable to get sql_mode %d : %s", err, errmsg);
            else
                PINFO("Sql_mode isn't set.");
        }
        else
        {
            PINFO("Initial sql_mode: %s", str.c_str());
            if(str.find(SQL_OPTION_TO_REMOVE) != std::string::npos)
            {
                std::string adjusted_str{adjust_sql_options_string(str)};
                PINFO("Setting sql_mode to %s", adjusted_str.c_str());
                std::string set_str{"SET sql_mode=" + std::move(adjusted_str)};
                dbi_result set_result = dbi_conn_query(connection,
                                                       set_str.c_str());
                if (set_result)
                {
                    dbi_result_free(set_result);
                }
                else
                {
                    const char* errmsg;
                    int err = dbi_conn_error(connection, &errmsg);
                    PERR("Unable to set sql_mode %d : %s", err, errmsg);
                }
            }
        }
        dbi_result_free(result);
    }
    else
    {
        const char* errmsg;
        int err = dbi_conn_error(connection, &errmsg);
        PERR("Unable to read sql_mode %d : %s", err, errmsg);
    }
}

static void
gnc_dbi_mysql_session_begin (QofBackend* qbe, QofSession* session,
                             const gchar* book_id, gboolean ignore_lock,
                             gboolean create, gboolean force)
{
    GncDbiBackend* be = (GncDbiBackend*)qbe;
    gchar* protocol = nullptr;
    gchar* host = nullptr;
    gchar* dbname = nullptr;
    gchar* username = nullptr;
    gchar* password = nullptr;
    gchar* basename = nullptr;
    gchar* translog_path = nullptr;
    gint portnum = 0;
    gint result;
    gboolean success = FALSE;
    GncDbiTestResult dbi_test_result = GNC_DBI_PASS;

    g_return_if_fail (qbe != nullptr);
    g_return_if_fail (session != nullptr);
    g_return_if_fail (book_id != nullptr);

    ENTER (" ");

    /* Split the book-id
     * Format is protocol://username:password@hostname:port/dbname
     where username, password and port are optional) */
    gnc_uri_get_components (book_id, &protocol, &host, &portnum,
                            &username, &password, &dbname);

    // Try to connect to the db.  If it doesn't exist and the create
    // flag is TRUE, we'll need to connect to the 'mysql' db and execute the
    // CREATE DATABASE ddl statement there.
    if (be->conn != nullptr)
    {
        dbi_conn_close (be->conn);
    }
#if HAVE_LIBDBI_R
    if (dbi_instance)
        be->conn = dbi_conn_new_r ("mysql", dbi_instance);
    else
        PERR ("Attempt to connect with an uninitialized dbi_instance");
#else
    be->conn = dbi_conn_new ("mysql");
#endif
    if (be->conn == nullptr)
    {
        PERR ("Unable to create mysql dbi connection\n");
        qof_backend_set_error (qbe, ERR_BACKEND_BAD_URL);
        goto exit;
    }
    dbi_conn_error_handler (be->conn, mysql_error_fn, be);
    if (!set_standard_connection_options (qbe, be->conn, host, portnum, dbname,
                                          username, password))
    {
        goto exit;
    }
    be->exists = TRUE;
    result = dbi_conn_connect (be->conn);
    if (result == 0)
    {
        adjust_sql_options (be->conn);
        dbi_test_result = conn_test_dbi_library (be->conn);
        switch (dbi_test_result)
        {
        case GNC_DBI_PASS:
            break;

        case GNC_DBI_FAIL_SETUP:
            qof_backend_set_error (qbe, ERR_SQL_DBI_UNTESTABLE);
            qof_backend_set_message (qbe,
                                     "DBI library large number test incomplete");
            break;

        case GNC_DBI_FAIL_TEST:
            qof_backend_set_error (qbe, ERR_SQL_BAD_DBI);
            qof_backend_set_message (qbe,
                                     "DBI library fails large number test");
            break;
        }
        if (GNC_DBI_PASS != dbi_test_result)
        {
            goto exit;
        }
        if (create && !force && save_may_clobber_data (qbe))
        {
            qof_backend_set_error (qbe, ERR_BACKEND_STORE_EXISTS);
            PWARN ("Databse already exists, Might clobber it.");
            goto exit;
        }

        success = gnc_dbi_lock_database (qbe, ignore_lock);
    }
    else
    {

        if (be->exists)
        {
            PERR ("Unable to connect to database '%s'\n", dbname);
            qof_backend_set_error (qbe, ERR_BACKEND_SERVER_ERR);
            goto exit;
        }

        // The db does not already exist.  Connect to the 'mysql' db and try to create it.
        if (create)
        {
            dbi_result dresult;
            result = dbi_conn_set_option (be->conn, "dbname", "mysql");
            if (result < 0)
            {
                PERR ("Error setting 'dbname' option\n");
                qof_backend_set_error (qbe, ERR_BACKEND_SERVER_ERR);
                goto exit;
            }
            result = dbi_conn_connect (be->conn);
            if (result < 0)
            {
                PERR ("Unable to connect to 'mysql' database\n");
                qof_backend_set_error (qbe, ERR_BACKEND_SERVER_ERR);
                goto exit;
            }
            adjust_sql_options (be->conn);
            dresult = dbi_conn_queryf (be->conn, "CREATE DATABASE %s CHARACTER SET utf8",
                                       dbname);
            if (dresult == nullptr)
            {
                PERR ("Unable to create database '%s'\n", dbname);
                qof_backend_set_error (qbe, ERR_BACKEND_SERVER_ERR);
                goto exit;
            }
            dbi_conn_close (be->conn);

            // Try again to connect to the db
#if HAVE_LIBDBI_R
            if (dbi_instance)
                be->conn = dbi_conn_new_r ("mysql", dbi_instance);
            else
                PERR ("Attempt to connect with an uninitialized dbi_instance");
#else
            be->conn = dbi_conn_new ("mysql");
#endif

            if (be->conn == nullptr)
            {
                PERR ("Unable to create mysql dbi connection\n");
                qof_backend_set_error (qbe, ERR_BACKEND_BAD_URL);
                goto exit;
            }
            dbi_conn_error_handler (be->conn, mysql_error_fn, be);
            if (!set_standard_connection_options (qbe, be->conn, host, 0, dbname, username,
                                                  password))
            {
                goto exit;
            }
            result = dbi_conn_connect (be->conn);
            if (result < 0)
            {
                PERR ("Unable to create database '%s'\n", dbname);
                qof_backend_set_error (qbe, ERR_BACKEND_SERVER_ERR);
                goto exit;
            }
            adjust_sql_options (be->conn);
            dbi_test_result = conn_test_dbi_library (be->conn);
            switch (dbi_test_result)
            {
            case GNC_DBI_PASS:
                break;

            case GNC_DBI_FAIL_SETUP:
                qof_backend_set_error (qbe, ERR_SQL_DBI_UNTESTABLE);
                qof_backend_set_message (qbe,
                                         "MySql: Failed to setup for large number test");
                break;

            case GNC_DBI_FAIL_TEST:
                qof_backend_set_error (qbe, ERR_SQL_BAD_DBI);
                qof_backend_set_message (qbe,
                                         "MySql DBI library fails large number test");
                break;
            }
            if (dbi_test_result != GNC_DBI_PASS)
            {
                dbi_conn_queryf (be->conn, "DROP DATABASE %s", dbname);
                goto exit;
            }
            success = gnc_dbi_lock_database (qbe, ignore_lock);
        }
        else
        {
            qof_backend_set_error (qbe, ERR_BACKEND_NO_SUCH_DB);
            qof_backend_set_message (qbe, "Database %s not found", dbname);
        }
    }

    if (success)
    {
        dbi_result dresult;

        if (be->sql_be.conn != nullptr)
        {
            delete (be->sql_be.conn);
        }
        be->sql_be.conn = new GncDbiSqlConnection (new GncDbiProviderImpl<DbType::DBI_MYSQL>,
                                                   qbe, be->conn);
    }
    be->sql_be.timespec_format = MYSQL_TIMESPEC_STR_FORMAT;

    /* We should now have a proper session set up.
     * Let's start logging */
    basename = g_strjoin ("_", protocol, host, username, dbname, nullptr);
    translog_path = gnc_build_translog_path (basename);
    xaccLogSetBaseName (translog_path);
    PINFO ("logpath=%s", translog_path ? translog_path : "(null)");

exit:
    g_free (protocol);
    g_free (host);
    g_free (username);
    g_free (password);
    g_free (basename);
    g_free (translog_path);
    g_free (dbname);

    LEAVE (" ");
}

template<> std::vector<std::string>
GncDbiProviderImpl<DbType::DBI_MYSQL>::get_index_list (dbi_conn conn)
{
    std::vector<std::string> retval;
    const char* errmsg;
    auto dbname = dbi_conn_get_option (conn, "dbname");
    auto table_list = dbi_conn_get_table_list (conn, dbname, nullptr);
    if (dbi_conn_error (conn, &errmsg) != DBI_ERROR_NONE)
    {
        PWARN ("Table Retrieval Error: %s\n", errmsg);
        return retval;
    }
    while (dbi_result_next_row (table_list) != 0)
    {
        auto table_name = dbi_result_get_string_idx (table_list, 1);
        auto result = dbi_conn_queryf (conn,
                                       "SHOW INDEXES IN %s WHERE Key_name != 'PRIMARY'",
                                       table_name);
        if (dbi_conn_error (conn, &errmsg) != DBI_ERROR_NONE)
        {
            PWARN ("Index Table Retrieval Error: %s on table %s\n",
                   errmsg, table_name);
            continue;
        }

        while (dbi_result_next_row (result) != 0)
        {
            std::string index_name {dbi_result_get_string_idx (result, 3)};
            retval.push_back(index_name + " " + table_name);
        }
        dbi_result_free (result);
    }

    return retval;
}

template<> void
GncDbiProviderImpl<DbType::DBI_MYSQL>::drop_index (dbi_conn conn, const std::string& index)
{
    unsigned int sep{0}, count{0};
    while ((sep = index.find(' ', sep)) != std::string::npos)
        ++count;
    if (count != 1)
    {
        PWARN("Drop index error: invalid MySQL index format (<index> <table>): %s",
              index.c_str());
        return;
    }

    auto result = dbi_conn_queryf (conn, "DROP INDEX %s ON %s",
                                   index.substr(0, sep).c_str(),
                                   index.substr(sep + 1).c_str());
    if (result)
        dbi_result_free (result);
}

static void
pgsql_error_fn (dbi_conn conn, void* user_data)
{
    GncDbiBackend* be = (GncDbiBackend*)user_data;
    GncDbiSqlConnection* dbi_conn =
        dynamic_cast<decltype(dbi_conn)>(be->sql_be.conn);
    const gchar* msg;

    (void)dbi_conn_error (conn, &msg);
    if (g_str_has_prefix (msg, "FATAL:  database") &&
        g_str_has_suffix (msg, "does not exist\n"))
    {
        PINFO ("DBI error: %s\n", msg);
        be->exists = FALSE;
        dbi_conn->set_error (ERR_BACKEND_NO_SUCH_DB, 0, FALSE);
    }
    else if (g_strrstr (msg,
                        "server closed the connection unexpectedly"))    // Connection lost
    {
        if (dbi_conn == nullptr)
        {
            PWARN ("DBI Error: Connection lost, connection pointer invalid");
            return;
        }
        PINFO ("DBI error: %s - Reconnecting...\n", msg);
        dbi_conn->set_error (ERR_BACKEND_CONN_LOST, 1, true);
        dbi_conn->retry_connection(msg);
    }
    else if (dbi_conn &&
             (g_str_has_prefix (msg, "connection pointer is NULL") ||
              g_str_has_prefix (msg, "could not connect to server")))       // No connection
    {
        dbi_conn->set_error(ERR_BACKEND_CANT_CONNECT, 1, true);
        dbi_conn->retry_connection (msg);
    }
    else
    {
        PERR ("DBI error: %s\n", msg);
        dbi_conn->set_error (ERR_BACKEND_MISC, 0, false);
    }
}

static void
gnc_dbi_postgres_session_begin (QofBackend* qbe, QofSession* session,
                                const gchar* book_id, gboolean ignore_lock,
                                gboolean create, gboolean force)
{
    GncDbiBackend* be = (GncDbiBackend*)qbe;
    gint result = 0;
    gchar* protocol = nullptr;
    gchar* host = nullptr;
    gchar* dbname = nullptr, *dbnamelc = nullptr;
    gchar* username = nullptr;
    gchar* password = nullptr;
    gchar* basename = nullptr;
    gchar* translog_path = nullptr;
    gboolean success = FALSE;
    gint portnum = 0;
    GncDbiTestResult dbi_test_result = GNC_DBI_PASS;

    g_return_if_fail (qbe != nullptr);
    g_return_if_fail (session != nullptr);
    g_return_if_fail (book_id != nullptr);

    ENTER (" ");

    /* Split the book-id
     * Format is protocol://username:password@hostname:port/dbname
     where username, password and port are optional) */
    gnc_uri_get_components (book_id, &protocol, &host, &portnum,
                            &username, &password, &dbname);
    if (portnum == 0)
        portnum = PGSQL_DEFAULT_PORT;
    /* Postgres's SQL interface coerces identifiers to lower case, but the
     * C interface is case-sensitive. This results in a mixed-case dbname
     * being created (with a lower case name) but then dbi can't conect to
     * it. To work around this, coerce the name to lowercase first. */
    dbnamelc = g_utf8_strdown (dbname, -1);

    // Try to connect to the db.  If it doesn't exist and the create
    // flag is TRUE, we'll need to connect to the 'postgres' db and execute the
    // CREATE DATABASE ddl statement there.
    if (be->conn != nullptr)
    {
        dbi_conn_close (be->conn);
    }

#if HAVE_LIBDBI_R
    if (dbi_instance)
        be->conn = dbi_conn_new_r ("pgsql", dbi_instance);
    else
        PERR ("Attempt to connect with an uninitialized dbi_instance");
#else
    be->conn = dbi_conn_new ("pgsql");
#endif

    if (be->conn == nullptr)
    {
        PERR ("Unable to create pgsql dbi connection\n");
        qof_backend_set_error (qbe, ERR_BACKEND_BAD_URL);
        goto exit;
    }
    dbi_conn_error_handler (be->conn, pgsql_error_fn, be);
    if (!set_standard_connection_options (qbe, be->conn, host, portnum, dbnamelc,
                                          username, password))
    {
        goto exit;
    }
    be->exists = TRUE;
    result = dbi_conn_connect (be->conn);
    if (result == 0)
    {
        dbi_test_result = conn_test_dbi_library (be->conn);
        switch (dbi_test_result)
        {
        case GNC_DBI_PASS:
            break;

        case GNC_DBI_FAIL_SETUP:
            qof_backend_set_error (qbe, ERR_SQL_DBI_UNTESTABLE);
            qof_backend_set_message (qbe,
                                     "Postgresql: Failed to setup for large number test");
            break;

        case GNC_DBI_FAIL_TEST:
            qof_backend_set_error (qbe, ERR_SQL_BAD_DBI);
            qof_backend_set_message (qbe,
                                     "Postgresql DBI library fails large number test");
            break;
        }
        if (dbi_test_result != GNC_DBI_PASS)
        {
            goto exit;
        }
        if (create && !force && save_may_clobber_data (qbe))
        {
            qof_backend_set_error (qbe, ERR_BACKEND_STORE_EXISTS);
            PWARN ("Databse already exists, Might clobber it.");
            goto exit;
        }

        success = gnc_dbi_lock_database (qbe, ignore_lock);
    }
    else
    {

        if (be->exists)
        {
            PERR ("Unable to connect to database '%s'\n", dbname);
            qof_backend_set_error (qbe, ERR_BACKEND_SERVER_ERR);
            goto exit;
        }

        // The db does not already exist.  Connect to the 'postgres' db and try to create it.
        if (create)
        {
            dbi_result dresult;
            result = dbi_conn_set_option (be->conn, "dbname", "postgres");
            if (result < 0)
            {
                PERR ("Error setting 'dbname' option\n");
                qof_backend_set_error (qbe, ERR_BACKEND_SERVER_ERR);
                goto exit;
            }
            result = dbi_conn_connect (be->conn);
            if (result < 0)
            {
                PERR ("Unable to connect to 'postgres' database\n");
                qof_backend_set_error (qbe, ERR_BACKEND_SERVER_ERR);
                goto exit;
            }
            dresult = dbi_conn_queryf (be->conn,
                                       "CREATE DATABASE %s WITH TEMPLATE template0 ENCODING 'UTF8'", dbnamelc);
            if (dresult == nullptr)
            {
                PERR ("Unable to create database '%s'\n", dbname);
                qof_backend_set_error (qbe, ERR_BACKEND_SERVER_ERR);
                goto exit;
            }
            dbi_conn_queryf (be->conn,
                             "ALTER DATABASE %s SET standard_conforming_strings TO on", dbnamelc);
            dbi_conn_close (be->conn);

            // Try again to connect to the db
#if HAVE_LIBDBI_R
            if (dbi_instance)
                be->conn = dbi_conn_new_r ("pgsql", dbi_instance);
            else
                PERR ("Attempt to connect with an uninitialized dbi_instance");
#else
            be->conn = dbi_conn_new ("pgsql");
#endif

            if (be->conn == nullptr)
            {
                PERR ("Unable to create pgsql dbi connection\n");
                qof_backend_set_error (qbe, ERR_BACKEND_BAD_URL);
                goto exit;
            }
            dbi_conn_error_handler (be->conn, pgsql_error_fn, be);
            if (!set_standard_connection_options (qbe, be->conn, host, PGSQL_DEFAULT_PORT,
                                                  dbnamelc, username, password))
            {
                goto exit;
            }
            result = dbi_conn_connect (be->conn);
            if (result < 0)
            {
                PERR ("Unable to create database '%s'\n", dbname);
                qof_backend_set_error (qbe, ERR_BACKEND_SERVER_ERR);
                goto exit;
            }
            dbi_test_result = conn_test_dbi_library (be->conn);
            switch (dbi_test_result)
            {
            case GNC_DBI_PASS:
                break;

            case GNC_DBI_FAIL_SETUP:
                qof_backend_set_error (qbe, ERR_SQL_DBI_UNTESTABLE);
                qof_backend_set_message (qbe,
                                         "DBI library large number test incomplete");
                break;

            case GNC_DBI_FAIL_TEST:
                qof_backend_set_error (qbe, ERR_SQL_BAD_DBI);
                qof_backend_set_message (qbe,
                                         "DBI library fails large number test");
                break;
            }
            if (GNC_DBI_PASS != dbi_test_result)
            {
                dbi_conn_select_db (be->conn, "template1");
                dbi_conn_queryf (be->conn, "DROP DATABASE %s", dbnamelc);
                goto exit;
            }
            success = gnc_dbi_lock_database (qbe, ignore_lock);
        }
        else
        {
            qof_backend_set_error (qbe, ERR_BACKEND_NO_SUCH_DB);
            qof_backend_set_message (qbe, "Database %s not found", dbname);
        }
    }
    if (success)
    {
        if (be->sql_be.conn != nullptr)
        {
            delete (be->sql_be.conn);
        }
        be->sql_be.conn = new GncDbiSqlConnection (new GncDbiProviderImpl<DbType::DBI_PGSQL>,
                                                   qbe, be->conn);
    }
    be->sql_be.timespec_format = PGSQL_TIMESPEC_STR_FORMAT;

    /* We should now have a proper session set up.
     * Let's start logging */
    basename = g_strjoin ("_", protocol, host, username, dbname, nullptr);
    translog_path = gnc_build_translog_path (basename);
    xaccLogSetBaseName (translog_path);
    PINFO ("logpath=%s", translog_path ? translog_path : "(null)");

exit:
    g_free (protocol);
    g_free (host);
    g_free (username);
    g_free (password);
    g_free (basename);
    g_free (translog_path);
    g_free (dbname);
    g_free (dbnamelc);

    LEAVE (" ");
}

template<> std::vector<std::string>
GncDbiProviderImpl<DbType::DBI_PGSQL>::get_index_list (dbi_conn conn)
{
    std::vector<std::string> retval;
    const char* errmsg;
    PINFO ("Retrieving postgres index list\n");
    auto result = dbi_conn_query (conn,
                                  "SELECT relname FROM pg_class AS a INNER JOIN pg_index AS b ON (b.indexrelid = a.oid) INNER JOIN pg_namespace AS c ON (a.relnamespace = c.oid) WHERE reltype = '0' AND indisprimary = 'f' AND nspname = 'public'");
    if (dbi_conn_error (conn, &errmsg) != DBI_ERROR_NONE)
    {
        PWARN("Index Table Retrieval Error: %s\n", errmsg);
        return retval;
    }
    while (dbi_result_next_row (result) != 0)
    {
        std::string index_name {dbi_result_get_string_idx (result, 1)};
        retval.push_back(index_name);
    }
    dbi_result_free (result);
    return retval;
}

/* ================================================================= */

static void
gnc_dbi_session_end (QofBackend* be_start)
{
    GncDbiBackend* be = (GncDbiBackend*)be_start;

    g_return_if_fail (be_start != nullptr);

    ENTER (" ");

    if (be->conn != nullptr)
    {
        gnc_dbi_unlock (be_start);
        dbi_conn_close (be->conn);
        be->conn = nullptr;
    }
    if (be->sql_be.conn != nullptr)
    {
        delete (be->sql_be.conn);
        be->sql_be.conn = nullptr;
    }
    gnc_sql_finalize_version_info (&be->sql_be);

    LEAVE (" ");
}

static void
gnc_dbi_destroy_backend (QofBackend* be)
{
    g_return_if_fail (be != nullptr);

    /* Stop transaction logging */
    xaccLogSetBaseName (nullptr);

    qof_backend_destroy (be);

    g_free (be);
}

/* ================================================================= */

/* GNUCASH_RESAVE_VERSION indicates the earliest database version
 * compatible with this version of Gnucash; the stored value is the
 * earliest version of Gnucash conpatible with the database. If the
 * GNUCASH_RESAVE_VERSION for this Gnucash is newer than the Gnucash
 * version which created the database, a resave is offered. If the
 * version of this Gnucash is older than the saved resave version,
 * then the database will be loaded read-only. A resave will update
 * both values to match this version of Gnucash.
 */
static void
gnc_dbi_load (QofBackend* qbe,  QofBook* book, QofBackendLoadType loadType)
{
    GncDbiBackend* be = (GncDbiBackend*)qbe;

    g_return_if_fail (qbe != nullptr);
    g_return_if_fail (book != nullptr);

    ENTER ("be=%p, book=%p", be, book);

    if (loadType == LOAD_TYPE_INITIAL_LOAD)
    {
        g_assert (be->primary_book == nullptr);
        be->primary_book = book;

        // Set up table version information
        gnc_sql_init_version_info (&be->sql_be);

        // Call all object backends to create any required tables
        auto registry = gnc_sql_get_backend_registry();
        for (auto entry : registry)
            create_tables(entry, be);
    }

    gnc_sql_load (&be->sql_be, book, loadType);

    if (GNUCASH_RESAVE_VERSION > gnc_sql_get_table_version (&be->sql_be,
                                                            "Gnucash"))
    {
        /* The database was loaded with an older database schema or
         * data semantics. In order to ensure consistency, the whole
         * thing needs to be saved anew. */
        qof_backend_set_error (qbe, ERR_SQL_DB_TOO_OLD);
    }
    else if (GNUCASH_RESAVE_VERSION < gnc_sql_get_table_version (&be->sql_be,
                                                                 "Gnucash-Resave"))
    {
        /* Worse, the database was created with a newer version. We
         * can't safely write to this database, so the user will have
         * to do a "save as" to make one that we can write to.
         */
        qof_backend_set_error (qbe, ERR_SQL_DB_TOO_NEW);
    }


    LEAVE ("");
}

/* ================================================================= */

static gboolean
save_may_clobber_data (QofBackend* qbe)
{
    GncDbiBackend* be = (GncDbiBackend*)qbe;
    const gchar* dbname;
    dbi_result result;
    gboolean retval = FALSE;

    /* Data may be clobbered iff the number of tables != 0 */
    dbname = dbi_conn_get_option (be->conn, "dbname");
    result = dbi_conn_get_table_list (be->conn, dbname, nullptr);
    if (result)
    {
        retval =  dbi_result_get_numrows (result) > 0;
        dbi_result_free (result);
    }
    return retval;
}

dbi_result
GncDbiSqlConnection::table_manage_backup (const std::string& table_name,
                                          TableOpType op)
{
    auto new_name = table_name + "_back";
    dbi_result result = nullptr;
    switch (op)
    {
    case backup:
        result = dbi_conn_queryf (m_conn, "ALTER TABLE %s RENAME TO %s",
                                  table_name.c_str(), new_name.c_str());
        break;
    case rollback:
        result = dbi_conn_queryf (m_conn,
                                  "ALTER TABLE %s RENAME TO %s",
                                  new_name.c_str(), table_name.c_str());
        break;
    case drop_backup:
        result = dbi_conn_queryf (m_conn, "DROP TABLE %s",
                                  new_name.c_str());
        break;
    default:
        break;
    }
    return result;
}

/**
 * Perform a specified SQL operation on every table in a
 * database. Possible operations are:
 * * drop: to DROP all tables from the database
 * * empty: to DELETE all records from each table in the database.
 * * backup: Rename every table from "name" to "name_back"
 * * drop_backup: DROP the backup tables.
 * * rollback: DROP the new table "name" and rename "name_back" to
 *   "name", restoring the database to its previous state.
 *
 * The intent of the last two is to be able to move an existing table
 * aside, query its contents with a transformation (in 2.4.x this is
 * already done as the contents are loaded completely when a Qof
 * session is started), save them to a new table according to a new
 * database format, and finally drop the backup table; if there's an
 * error during the process, rollback allows returning the table to
 * its original state.
 *
 * @param sql_conn: The sql connection (via dbi) to which the
 * transactions will be sent
 * @param tables: GList of tables to operate on.
 * @param op: The operation to perform.
 * @return Success (TRUE) or failure.
 */

gboolean
conn_table_operation (GncSqlConnection* sql_conn,
                      std::vector<std::string> table_name_list,
                      TableOpType op)
{
    gboolean result = TRUE;
    GncDbiSqlConnection* conn = (GncDbiSqlConnection*) (sql_conn);
    const gchar* dbname = dbi_conn_get_option (conn->m_conn, "dbname");

    g_return_val_if_fail (!table_name_list.empty(), FALSE);

    for (auto table : table_name_list)
    {
        dbi_result result;
        auto table_name = table.c_str();
        /* Ignore the lock table */
        if (g_strcmp0 (table_name, lock_table) == 0)
        {
            continue;
        }
        do
        {
            conn->init_error ();
            switch (op)
            {
            case rollback:
            {
                auto full_table_name_list =
                    conn->m_provider->get_table_list (conn->m_conn, dbname);
                if (std::find (full_table_name_list.begin(),
                               full_table_name_list.end(),
                               table_name) != full_table_name_list.end())
                {
                    result = dbi_conn_queryf (conn->m_conn, "DROP TABLE %s",
                                              table_name);
                    if (result)
                        break;
                }
            }
            /* Fall through */
            case backup:
            case drop_backup:
                result = conn->table_manage_backup (table_name, op);
                break;
            case empty:
                result = dbi_conn_queryf (conn->m_conn, "DELETE FROM TABLE %s",
                                          table_name);
                break;
            case drop:
            default:
                result = dbi_conn_queryf (conn->m_conn, "DROP TABLE %s",
                                          table_name);
                break;
            }
        }
        while (conn->m_retry);
        if (result != nullptr)
        {
            if (dbi_result_free (result) < 0)
            {
                PERR ("Error in dbi_result_free() result\n");
                result = FALSE;
            }
        }
    }
    return result;
}

/**
 * Safely resave a database by renaming all of its tables, recreating
 * everything, and then dropping the backup tables only if there were
 * no errors. If there are errors, drop the new tables and restore the
 * originals.
 *
 * @param qbe: QofBackend for the session.
 * @param book: QofBook to be saved in the database.
 */
void
gnc_dbi_safe_sync_all (QofBackend* qbe, QofBook* book)
{
    GncDbiBackend* be = (GncDbiBackend*)qbe;
    GncDbiSqlConnection* conn = (GncDbiSqlConnection*) (((GncSqlBackend*)
                                                         be)->conn);
    const gchar* dbname = nullptr;

    g_return_if_fail (be != nullptr);
    g_return_if_fail (book != nullptr);

    ENTER ("book=%p, primary=%p", book, be->primary_book);
    dbname = dbi_conn_get_option (be->conn, "dbname");
    auto table_list = conn->m_provider->get_table_list (conn->m_conn, dbname);
    if (!conn_table_operation (conn, table_list, backup))
    {
        qof_backend_set_error (qbe, ERR_BACKEND_SERVER_ERR);
        conn_table_operation (conn, table_list, rollback);
        LEAVE ("Failed to rename tables");
        return;
    }
    auto index_list = conn->m_provider->get_index_list (conn->m_conn);
    for (auto index : index_list)
    {
        const char* errmsg;
        conn->m_provider->drop_index (conn->m_conn, index);
        if (DBI_ERROR_NONE != dbi_conn_error (conn->m_conn, &errmsg))
        {
            qof_backend_set_error (qbe, ERR_BACKEND_SERVER_ERR);
            conn_table_operation (conn, table_list, rollback);
            LEAVE ("Failed to drop indexes %s", errmsg);
            return;
        }
    }
    be->is_pristine_db = TRUE;
    be->primary_book = book;

    gnc_sql_sync_all (&be->sql_be, book);
    if (qof_backend_check_error (qbe))
    {
        conn_table_operation (conn, table_list, rollback);
        LEAVE ("Failed to create new database tables");
        return;
    }
    conn_table_operation (conn, table_list, drop_backup);
    LEAVE ("book=%p", book);
}
/* ================================================================= */
static void
gnc_dbi_begin_edit (QofBackend* qbe, QofInstance* inst)
{
    GncDbiBackend* be = (GncDbiBackend*)qbe;

    g_return_if_fail (be != nullptr);
    g_return_if_fail (inst != nullptr);

    gnc_sql_begin_edit (&be->sql_be, inst);
}

static void
gnc_dbi_rollback_edit (QofBackend* qbe, QofInstance* inst)
{
    GncDbiBackend* be = (GncDbiBackend*)qbe;

    g_return_if_fail (be != nullptr);
    g_return_if_fail (inst != nullptr);

    gnc_sql_rollback_edit (&be->sql_be, inst);
}

static void
gnc_dbi_commit_edit (QofBackend* qbe, QofInstance* inst)
{
    GncDbiBackend* be = (GncDbiBackend*)qbe;

    g_return_if_fail (be != nullptr);
    g_return_if_fail (inst != nullptr);

    gnc_sql_commit_edit (&be->sql_be, inst);
}

/* ================================================================= */

static void
init_sql_backend (GncDbiBackend* dbi_be)
{
    QofBackend* be;

    be = (QofBackend*)dbi_be;

    be->session_end = gnc_dbi_session_end;
    be->destroy_backend = gnc_dbi_destroy_backend;

    be->load = gnc_dbi_load;

    /* The gda backend treats accounting periods transactionally. */
    be->begin = gnc_dbi_begin_edit;
    be->commit = gnc_dbi_commit_edit;
    be->rollback = gnc_dbi_rollback_edit;

    /* The SQL/DBI backend doesn't need to be synced until it is
     * configured for multiuser access. */
    be->sync = gnc_dbi_safe_sync_all;
    be->safe_sync = gnc_dbi_safe_sync_all;
    /* CoA Export function not implemented for the SQL backend. */
    be->export_fn = nullptr;

    gnc_sql_init (&dbi_be->sql_be);

    dbi_be->sql_be.conn = nullptr;
    dbi_be->sql_be.book = nullptr;
}

static QofBackend*
new_backend (void (*session_begin) (QofBackend*, QofSession*, const gchar*,
                                    gboolean,
                                    gboolean,
                                    gboolean))
{
    GncDbiBackend* dbi_be;
    QofBackend* be;

    dbi_be = g_new0 (GncDbiBackend, 1);
    g_assert (dbi_be != nullptr);

    be = (QofBackend*)dbi_be;
    qof_backend_init (be);

    be->session_begin = session_begin;
    init_sql_backend (dbi_be);

    return be;
}

template<> QofBackend*
QofDbiBackendProvider<DbType::DBI_SQLITE>::create_backend()
{
    return new_backend (gnc_dbi_sqlite3_session_begin);
}

template<> QofBackend*
QofDbiBackendProvider<DbType::DBI_MYSQL>::create_backend()
{
    return new_backend (gnc_dbi_mysql_session_begin);
}

template<> QofBackend*
QofDbiBackendProvider<DbType::DBI_PGSQL>::create_backend()
{
    return new_backend (gnc_dbi_postgres_session_begin);
}


/*
 * Checks to see whether the file is an sqlite file or not
 *1980
 */
template<> bool
QofDbiBackendProvider<DbType::DBI_SQLITE>::type_check(const char *uri)
{
    FILE* f;
    gchar buf[50];
    G_GNUC_UNUSED size_t chars_read;
    gint status;
    gchar* filename;

    // BAD if the path is null
    g_return_val_if_fail (uri != nullptr, FALSE);

    filename = gnc_uri_get_path (uri);
    f = g_fopen (filename, "r");
    g_free (filename);

    // OK if the file doesn't exist - new file
    if (f == nullptr)
    {
        PINFO ("doesn't exist (errno=%d) -> DBI", errno);
        return TRUE;
    }

    // OK if file has the correct header
    chars_read = fread (buf, sizeof (buf), 1, f);
    status = fclose (f);
    if (status < 0)
    {
        PERR ("Error in fclose(): %d\n", errno);
    }
    if (g_str_has_prefix (buf, "SQLite format 3"))
    {
        PINFO ("has SQLite format string -> DBI");
        return TRUE;
    }
    PINFO ("exists, does not have SQLite format string -> not DBI");

    // Otherwise, BAD
    return FALSE;
}

void
gnc_module_init_backend_dbi (void)
{
    const gchar* driver_dir;
    int num_drivers;
    gboolean have_sqlite3_driver = FALSE;
    gboolean have_mysql_driver = FALSE;
    gboolean have_pgsql_driver = FALSE;

    /* Initialize libdbi and see which drivers are available.  Only register qof backends which
       have drivers available. */
    driver_dir = g_getenv ("GNC_DBD_DIR");
    if (driver_dir == nullptr)
    {
        PINFO ("GNC_DBD_DIR not set: using libdbi built-in default\n");
    }

    /* dbi_initialize returns -1 in case of errors */
#if HAVE_LIBDBI_R
    if (dbi_instance)
        return;
    num_drivers = dbi_initialize_r (driver_dir, &dbi_instance);
#else
    num_drivers = dbi_initialize (driver_dir);
#endif
    if (num_drivers <= 0)
    {
        gchar* dir = g_build_filename (gnc_path_get_libdir (), "dbd", nullptr);
#if HAVE_LIBDBI_R
        if (dbi_instance)
            return;
        num_drivers = dbi_initialize_r (dir, &dbi_instance);
#else
        num_drivers = dbi_initialize (dir);
#endif
        g_free (dir);
    }
    if (num_drivers <= 0)
    {
        PWARN ("No DBD drivers found\n");
    }
    else
    {
        dbi_driver driver = nullptr;
        PINFO ("%d DBD drivers found\n", num_drivers);

        do
        {
#if HAVE_LIBDBI_R
            driver = dbi_driver_list_r (driver, dbi_instance);
#else
            driver = dbi_driver_list (driver);
#endif

            if (driver != nullptr)
            {
                const gchar* name = dbi_driver_get_name (driver);

                PINFO ("Driver: %s\n", name);
                if (strcmp (name, "sqlite3") == 0)
                {
                    have_sqlite3_driver = TRUE;
                }
                else if (strcmp (name, "mysql") == 0)
                {
                    have_mysql_driver = TRUE;
                }
                else if (strcmp (name, "pgsql") == 0)
                {
                    have_pgsql_driver = TRUE;
                }
            }
        }
        while (driver != nullptr);
    }

    if (have_sqlite3_driver)
    {
        const char* name = "GnuCash Libdbi (SQLITE3) Backend";
        auto prov = QofBackendProvider_ptr(new QofDbiBackendProvider<DbType::DBI_SQLITE>{name, FILE_URI_TYPE});
        qof_backend_register_provider(std::move(prov));
        prov = QofBackendProvider_ptr(new QofDbiBackendProvider<DbType::DBI_SQLITE>{name, SQLITE3_URI_TYPE});
        qof_backend_register_provider(std::move(prov));
    }

    if (have_mysql_driver)
    {
        const char *name = "GnuCash Libdbi (MYSQL) Backend";
        auto prov = QofBackendProvider_ptr(new QofDbiBackendProvider<DbType::DBI_MYSQL>{name, "mysql"});
        qof_backend_register_provider(std::move(prov));
    }

    if (have_pgsql_driver)
    {
        const char* name = "GnuCash Libdbi (POSTGRESQL) Backend";
        auto prov = QofBackendProvider_ptr(new QofDbiBackendProvider<DbType::DBI_PGSQL>{name, "postgres"});
        qof_backend_register_provider(std::move(prov));
    }

    /* If needed, set log level to DEBUG so that SQl statements will be put into
       the gnucash.trace file. */
    /*    qof_log_set_level( log_module, QOF_LOG_DEBUG ); */
}

#ifndef GNC_NO_LOADABLE_MODULES
G_MODULE_EXPORT void
qof_backend_module_init (void)
{
    gnc_module_init_backend_dbi ();
}

G_MODULE_EXPORT void
qof_backend_module_finalize (void)
{
    gnc_module_finalize_backend_dbi ();
}
#endif /* GNC_NO_LOADABLE_MODULES */

void
gnc_module_finalize_backend_dbi (void)
{
#if HAVE_LIBDBI_R
    if (dbi_instance)
    {
        dbi_shutdown_r (dbi_instance);
        dbi_instance = nullptr;
    }
#else
    dbi_shutdown ();
#endif
}

/* --------------------------------------------------------- */
GncSqlRow&
GncDbiSqlResult::IteratorImpl::operator++()
{
    int status = dbi_result_next_row (m_inst->m_dbi_result);
    if (status)
        return m_inst->m_row;
    int error = m_inst->dberror();
    if (error == DBI_ERROR_BADIDX || error == 0) //ran off the end of the results
        return m_inst->m_sentinel;
    PERR("Error %d incrementing results iterator.", error);
    qof_backend_set_error (m_inst->m_conn->qbe(), ERR_BACKEND_SERVER_ERR);
    return m_inst->m_sentinel;
}

int64_t
GncDbiSqlResult::IteratorImpl::get_int_at_col(const char* col) const
{
    auto type = dbi_result_get_field_type (m_inst->m_dbi_result, col);
    if(type != DBI_TYPE_INTEGER)
        throw (std::invalid_argument{"Requested integer from non-integer column."});
    return dbi_result_get_longlong (m_inst->m_dbi_result, col);
}

float
GncDbiSqlResult::IteratorImpl::get_float_at_col(const char* col) const
{
    auto type = dbi_result_get_field_type (m_inst->m_dbi_result, col);
    auto attrs = dbi_result_get_field_attribs (m_inst->m_dbi_result, col);
    if(type != DBI_TYPE_DECIMAL ||
       (attrs & DBI_DECIMAL_SIZEMASK) != DBI_DECIMAL_SIZE4)
        throw (std::invalid_argument{"Requested float from non-float column."});
    gnc_push_locale (LC_NUMERIC, "C");
    auto retval =  dbi_result_get_float(m_inst->m_dbi_result, col);
    gnc_pop_locale (LC_NUMERIC);
    return retval;
}

double
GncDbiSqlResult::IteratorImpl::get_double_at_col(const char* col) const
{
    auto type = dbi_result_get_field_type (m_inst->m_dbi_result, col);
    auto attrs = dbi_result_get_field_attribs (m_inst->m_dbi_result, col);
    if(type != DBI_TYPE_DECIMAL ||
       (attrs & DBI_DECIMAL_SIZEMASK) != DBI_DECIMAL_SIZE8)
        throw (std::invalid_argument{"Requested double from non-double column."});
    gnc_push_locale (LC_NUMERIC, "C");
    auto retval =  dbi_result_get_double(m_inst->m_dbi_result, col);
    gnc_pop_locale (LC_NUMERIC);
    return retval;
}

std::string
GncDbiSqlResult::IteratorImpl::get_string_at_col(const char* col) const
{
    auto type = dbi_result_get_field_type (m_inst->m_dbi_result, col);
    auto attrs = dbi_result_get_field_attribs (m_inst->m_dbi_result, col);
    if(type != DBI_TYPE_STRING)
        throw (std::invalid_argument{"Requested string from non-string column."});
    gnc_push_locale (LC_NUMERIC, "C");
    auto strval = dbi_result_get_string(m_inst->m_dbi_result, col);
    if (strval == nullptr)
    {
        gnc_pop_locale (LC_NUMERIC);
        throw (std::invalid_argument{"Column empty."});
    }
    auto retval =  std::string{strval};
    gnc_pop_locale (LC_NUMERIC);
    return retval;
}
time64
GncDbiSqlResult::IteratorImpl::get_time64_at_col (const char* col) const
{
    auto type = dbi_result_get_field_type (m_inst->m_dbi_result, col);
    auto attrs = dbi_result_get_field_attribs (m_inst->m_dbi_result, col);
    if (type != DBI_TYPE_DATETIME)
        throw (std::invalid_argument{"Requested double from non-double column."});
    gnc_push_locale (LC_NUMERIC, "C");
#if HAVE_LIBDBI_TO_LONGLONG
    /* A less evil hack than the one equrie by libdbi-0.8, but
     * still necessary to work around the same bug.
     */
    auto retval = dbi_result_get_as_longlong(dbi_row->result,
                                             col_name);
#else
    /* A seriously evil hack to work around libdbi bug #15
     * https://sourceforge.net/p/libdbi/bugs/15/. When libdbi
     * v0.9 is widely available this can be replaced with
     * dbi_result_get_as_longlong.
     * Note: 0.9 is available in Debian Jessie and Fedora 21.
     */
    auto result = (dbi_result_t*) (m_inst->m_dbi_result);
    auto row = dbi_result_get_currow (result);
    auto idx = dbi_result_get_field_idx (result, col) - 1;
    time64 retval = result->rows[row]->field_values[idx].d_datetime;
    if (retval < MINTIME || retval > MAXTIME)
        retval = 0;
#endif //HAVE_LIBDBI_TO_LONGLONG
    gnc_pop_locale (LC_NUMERIC);
    return retval;
}


/* --------------------------------------------------------- */

GncDbiSqlResult::~GncDbiSqlResult()
{
    int status = dbi_result_free (m_dbi_result);

    if (status == 0)
        return;

    PERR ("Error %d in dbi_result_free() result.", dberror() );
    qof_backend_set_error (m_conn->qbe(), ERR_BACKEND_SERVER_ERR);
}

GncSqlRow&
GncDbiSqlResult::begin()
{

    if (m_dbi_result == nullptr ||
        dbi_result_get_numrows(m_dbi_result) == 0)
        return m_sentinel;
    int status = dbi_result_first_row(m_dbi_result);
    if (status)
        return m_row;
    int error = dberror(); //

    if (error != DBI_ERROR_BADIDX) //otherwise just an empty result set
    {
        PERR ("Error %d in dbi_result_first_row()", dberror());
        qof_backend_set_error (m_conn->qbe(), ERR_BACKEND_SERVER_ERR);
    }
    return m_sentinel;
}

int
GncDbiSqlResult::dberror()
{
    return dbi_conn_error(m_conn->conn(), nullptr);
}

uint64_t
GncDbiSqlResult::size() const noexcept
{
    return dbi_result_get_numrows(m_dbi_result);
}

/* --------------------------------------------------------- */
class GncDbiSqlStatement : public GncSqlStatement
{
public:
    GncDbiSqlStatement(const GncSqlConnection* conn, const std::string& sql) :
        m_conn{conn}, m_sql {sql} {}
    ~GncDbiSqlStatement() {}
    const char* to_sql() const override;
    void add_where_cond(QofIdTypeConst, const PairVec&) override;

private:
    const GncSqlConnection* m_conn;
    std::string m_sql;
};


const char*
GncDbiSqlStatement::to_sql() const
{
    return m_sql.c_str();
}

void
GncDbiSqlStatement::add_where_cond(QofIdTypeConst type_name,
                                   const PairVec& col_values)
{
    m_sql += " WHERE ";
    for (auto colpair : col_values)
    {
        if (colpair != *col_values.begin())
            m_sql += " AND ";
        m_sql += colpair.first + " = " +
            m_conn->quote_string (colpair.second.c_str());
    }
}

/* --------------------------------------------------------- */
GncSqlResultPtr
GncDbiSqlConnection::execute_select_statement (const GncSqlStatementPtr& stmt)
    noexcept
{
    dbi_result result;

    DEBUG ("SQL: %s\n", stmt->to_sql());
    gnc_push_locale (LC_NUMERIC, "C");
    do
    {
        init_error ();
        result = dbi_conn_query (m_conn, stmt->to_sql());
    }
    while (m_retry);
    if (result == nullptr)
        PERR ("Error executing SQL %s\n", stmt->to_sql());
    gnc_pop_locale (LC_NUMERIC);
    return GncSqlResultPtr(new GncDbiSqlResult (this, result));
}

int
GncDbiSqlConnection::execute_nonselect_statement (const GncSqlStatementPtr& stmt)
    noexcept
{
    dbi_result result;

    DEBUG ("SQL: %s\n", stmt->to_sql());
    do
    {
        init_error ();
        result = dbi_conn_query (m_conn, stmt->to_sql());
    }
    while (m_retry);
    if (result == nullptr && m_last_error)
    {
        PERR ("Error executing SQL %s\n", stmt->to_sql());
        return -1;
    }
    if (!result)
        return 0;
    auto num_rows = (gint)dbi_result_get_numrows_affected (result);
    auto status = dbi_result_free (result);
    if (status < 0)
    {
        PERR ("Error in dbi_result_free() result\n");
        qof_backend_set_error (m_qbe, ERR_BACKEND_SERVER_ERR);
    }
    return num_rows;
}

GncSqlStatementPtr
GncDbiSqlConnection::create_statement_from_sql (const std::string& sql)
    const noexcept
{
    return std::unique_ptr<GncSqlStatement>{new GncDbiSqlStatement (this, sql)};
}

bool
GncDbiSqlConnection::does_table_exist (const std::string& table_name)
    const noexcept
{
    auto dbname = dbi_conn_get_option (m_conn, "dbname");
    auto tables = dbi_conn_get_table_list (m_conn, dbname, table_name.c_str());
    auto nTables = dbi_result_get_numrows (tables);
    auto status = dbi_result_free (tables);
    if (status < 0)
    {
        PERR ("Error in dbi_result_free() result\n");
        qof_backend_set_error (m_qbe, ERR_BACKEND_SERVER_ERR);
    }

    return nTables == 1;
}

bool
GncDbiSqlConnection::begin_transaction () noexcept
{
    dbi_result result;

    DEBUG ("BEGIN\n");

    if (!verify ())
    {
        PERR ("gnc_dbi_verify_conn() failed\n");
        qof_backend_set_error (m_qbe, ERR_BACKEND_SERVER_ERR);
        return FALSE;
    }

    do
    {
        init_error ();
        result = dbi_conn_queryf (m_conn, "BEGIN");
    }
    while (m_retry);

    auto success = (result != nullptr);
    auto status = dbi_result_free (result);
    if (status < 0)
    {
        PERR ("Error in dbi_result_free() result\n");
        qof_backend_set_error (m_qbe, ERR_BACKEND_SERVER_ERR);
    }
    if (!success)
    {
        PERR ("BEGIN transaction failed()\n");
        qof_backend_set_error (m_qbe, ERR_BACKEND_SERVER_ERR);
    }

    return success;
}

bool
GncDbiSqlConnection::rollback_transaction () const noexcept
{
    DEBUG ("ROLLBACK\n");
    const char* command =  "ROLLBACK";
    auto result = dbi_conn_query (m_conn, command);
    auto success = (result != nullptr);

    auto status = dbi_result_free (result);
    if (status < 0)
    {
        PERR ("Error in dbi_result_free() result\n");
        qof_backend_set_error (m_qbe, ERR_BACKEND_SERVER_ERR);
    }
    if (!success)
    {
        PERR ("Error in conn_rollback_transaction()\n");
        qof_backend_set_error (m_qbe, ERR_BACKEND_SERVER_ERR);
    }

    return success;
}

bool
GncDbiSqlConnection::commit_transaction () const noexcept
{
    DEBUG ("COMMIT\n");
    auto result = dbi_conn_queryf (m_conn, "COMMIT");
    auto success = (result != nullptr);

    auto status = dbi_result_free (result);
    if (status < 0)
    {
        PERR ("Error in dbi_result_free() result\n");
        qof_backend_set_error (m_qbe, ERR_BACKEND_SERVER_ERR);
    }
    if (!success)
    {
        PERR ("Error in conn_commit_transaction()\n");
        qof_backend_set_error (m_qbe, ERR_BACKEND_SERVER_ERR);
    }

    return success;
}

static std::string
create_index_ddl (const GncSqlConnection* conn, const std::string& index_name,
                  const std::string& table_name, const EntryVec& col_table)
{
    std::string ddl;
    ddl += "CREATE INDEX " + index_name + " ON " + table_name + "(";
    for (auto const table_row : col_table)
    {
        if (table_row != *col_table.begin())
        {
            ddl =+ ", ";
        }
        ddl += table_row->name();
    }
    ddl += ")";
    return ddl;
}

std::string
add_columns_ddl(const GncSqlConnection* conn,
                const std::string& table_name,
                const ColVec& info_vec)
{
    std::string ddl;
    const GncDbiSqlConnection* dbi_conn = dynamic_cast<decltype(dbi_conn)>(conn);

    g_return_val_if_fail (conn != nullptr, nullptr);
    ddl += "ALTER TABLE " + table_name;
    for (auto const& info : info_vec)
    {
        if (info != *info_vec.begin())
        {
            ddl += ", ";
        }
        ddl += "ADD COLUMN ";
        dbi_conn->m_provider->append_col_def (ddl, info);
    }
    return ddl;
}

template<> void
GncDbiProviderImpl<DbType::DBI_SQLITE>::append_col_def(std::string& ddl,
                                           const GncSqlColumnInfo& info)
{
    const char* type_name = nullptr;

    if (info.m_type == BCT_INT)
    {
        type_name = "integer";
    }
    else if (info.m_type == BCT_INT64)
    {
        type_name = "bigint";
    }
    else if (info.m_type == BCT_DOUBLE)
    {
        type_name = "float8";
    }
    else if (info.m_type == BCT_STRING || info.m_type == BCT_DATE
              || info.m_type == BCT_DATETIME)
    {
        type_name = "text";
    }
    else
    {
        PERR ("Unknown column type: %d\n", info.m_type);
        type_name = "";
    }
    ddl += (info.m_name + " " + type_name);
    if (info.m_size != 0)
    {
        ddl += "(" + std::to_string(info.m_size) + ")";
    }
    if (info.m_primary_key)
    {
        ddl += " PRIMARY KEY";
    }
    if (info.m_autoinc)
    {
        ddl += " AUTOINCREMENT";
    }
    if (info.m_not_null)
    {
        ddl += " NOT NULL";
    }
}

template <DbType P> std::string
GncDbiProviderImpl<P>::create_table_ddl (const GncSqlConnection* conn,
                                              const std::string& table_name,
                                              const ColVec& info_vec)
{
    std::string ddl;
    unsigned int col_num = 0;

    g_return_val_if_fail (conn != nullptr, ddl);
    ddl += "CREATE TABLE " + table_name + "(";
    for (auto const& info : info_vec)
    {
        if (col_num++ != 0)
        {
            ddl += ", ";
        }
        append_col_def (ddl, info);
    }
    ddl += ")";

    return ddl;
}

template<> void
GncDbiProviderImpl<DbType::DBI_MYSQL>::append_col_def (std::string& ddl,
                                           const GncSqlColumnInfo& info)
{
    const char* type_name = nullptr;

    if (info.m_type == BCT_INT)
    {
        type_name = "integer";
    }
    else if (info.m_type == BCT_INT64)
    {
        type_name = "bigint";
    }
    else if (info.m_type == BCT_DOUBLE)
    {
        type_name = "double";
    }
    else if (info.m_type == BCT_STRING)
    {
        type_name = "varchar";
    }
    else if (info.m_type == BCT_DATE)
    {
        type_name = "date";
    }
    else if (info.m_type == BCT_DATETIME)
    {
        type_name = "TIMESTAMP NULL DEFAULT 0";
    }
    else
    {
        PERR ("Unknown column type: %d\n", info.m_type);
        type_name = "";
    }
    ddl += info.m_name + " " + type_name;
    if (info.m_size != 0 && info.m_type == BCT_STRING)
    {
        ddl += std::to_string(info.m_size);
    }
    if (info.m_unicode)
    {
        ddl += " CHARACTER SET utf8";
    }
    if (info.m_primary_key)
    {
        ddl += " PRIMARY KEY";
    }
    if (info.m_autoinc)
    {
        ddl += " AUTO_INCREMENT";
    }
    if (info.m_not_null)
    {
        ddl += " NOT NULL";
    }
}


template<> void
GncDbiProviderImpl<DbType::DBI_PGSQL>::append_col_def (std::string& ddl,
                                           const GncSqlColumnInfo& info)
{
    const char* type_name = nullptr;

    if (info.m_type == BCT_INT)
    {
        if (info.m_autoinc)
        {
            type_name = "serial";
        }
        else
        {
            type_name = "integer";
        }
    }
    else if (info.m_type == BCT_INT64)
    {
        type_name = "int8";
    }
    else if (info.m_type == BCT_DOUBLE)

    {
        type_name = "double precision";
    }
    else if (info.m_type == BCT_STRING)
    {
        type_name = "varchar";
    }
    else if (info.m_type == BCT_DATE)
    {
        type_name = "date";
    }
    else if (info.m_type == BCT_DATETIME)
    {
        type_name = "timestamp without time zone";
    }
    else
    {
        PERR ("Unknown column type: %d\n", info.m_type);
        type_name = "";
    }
    ddl += info.m_name + " " + type_name;
    if (info.m_size != 0 && info.m_type == BCT_STRING)
    {
        ddl += "(" + std::to_string(info.m_size) + ")";
    }
    if (info.m_primary_key)
    {
        ddl += " PRIMARY KEY";
    }
    if (info.m_not_null)
    {
        ddl += " NOT NULL";
    }
}

bool
GncDbiSqlConnection::create_table (const std::string& table_name,
                                   const ColVec& info_vec) const noexcept
{
    auto ddl = m_provider->create_table_ddl(this, table_name, info_vec);
    if (ddl.empty())
        return false;

    DEBUG ("SQL: %s\n", ddl.c_str());
    auto result = dbi_conn_query (m_conn, ddl.c_str());
    auto status = dbi_result_free (result);
    if (status < 0)
    {
        PERR ("Error in dbi_result_free() result\n");
        qof_backend_set_error (m_qbe, ERR_BACKEND_SERVER_ERR);
    }

    return true;
}

bool
GncDbiSqlConnection::create_index(const std::string& index_name,
                                  const std::string& table_name,
                                  const EntryVec& col_table) const noexcept
{
    auto ddl = create_index_ddl (this, index_name, table_name, col_table);
    if (ddl.empty())
        return false;
    DEBUG ("SQL: %s\n", ddl.c_str());
    auto result = dbi_conn_query (m_conn, ddl.c_str());
    auto status = dbi_result_free (result);
    if (status < 0)
    {
        PERR ("Error in dbi_result_free() result\n");
        qof_backend_set_error (m_qbe, ERR_BACKEND_SERVER_ERR);
    }

    return true;
}

bool
GncDbiSqlConnection::add_columns_to_table(const std::string& table_name,
                                          const ColVec& info_vec)
    const noexcept
{
    auto ddl = add_columns_ddl(this, table_name, info_vec);
    if (ddl.empty())
        return false;

    DEBUG ("SQL: %s\n", ddl.c_str());
    auto result = dbi_conn_query (m_conn, ddl.c_str());
    auto status = dbi_result_free (result);
    if (status < 0)
    {
        PERR( "Error in dbi_result_free() result\n" );
        qof_backend_set_error(m_qbe, ERR_BACKEND_SERVER_ERR );
    }

    return true;
}

std::string
GncDbiSqlConnection::quote_string (const std::string& unquoted_str)
    const noexcept
{
    gchar* quoted_str;
    size_t size;

    size = dbi_conn_quote_string_copy (m_conn, unquoted_str.c_str(),
                                       &quoted_str);
    if (size != 0)
    {
        return std::string{quoted_str};
    }
    else
    {
        return std::string{""};
    }
}

static std::vector<std::string>
conn_get_table_list (dbi_conn conn, const std::string& dbname)
{
    std::vector<std::string> retval;
    auto tables = dbi_conn_get_table_list (conn, dbname.c_str(), nullptr);
    while (dbi_result_next_row (tables) != 0)
    {
        std::string table_name {dbi_result_get_string_idx (tables, 1)};
        retval.push_back(table_name);
    }
    dbi_result_free (tables);
    return retval;
}

template<> std::vector<std::string>
GncDbiProviderImpl<DbType::DBI_SQLITE>::get_table_list (dbi_conn conn,
                                            const std::string& dbname)
{
    /* Return the list, but remove the tables that sqlite3 adds for
     * its own use. */
    auto list = conn_get_table_list (conn, dbname);
    auto end = std::remove(list.begin(), list.end(), "sqlite_sequence");
    list.erase(end, list.end());
    return list;
}

template<> std::vector<std::string>
GncDbiProviderImpl<DbType::DBI_MYSQL>::get_table_list (dbi_conn conn,
                                               const std::string& dbname)
{
    return conn_get_table_list (conn, dbname);
}

template<> std::vector<std::string>
GncDbiProviderImpl<DbType::DBI_PGSQL>::get_table_list (dbi_conn conn,
                                           const std::string& dbname)
{
    auto list = conn_get_table_list (conn, dbname);
    auto end = std::remove_if (list.begin(), list.end(),
                               [](std::string& table_name){
                                   return table_name == "sql_features" ||
                                   table_name == "sql_implementation_info" ||
                                   table_name == "sql_languages" ||
                                   table_name == "sql_packages" ||
                                   table_name == "sql_parts" ||
                                   table_name == "sql_sizing" ||
                                   table_name == "sql_sizing_profiles";
                               });
    list.erase(end, list.end());
    return list;
}

/** Users discovered a bug in some distributions of libdbi, where if
 * it is compiled on certain versions of gcc with the -ffast-math
 * compiler option it fails to correctly handle saving of 64-bit
 * values. This function tests for the problem.
 * @param: conn: The just-opened dbi_conn
 * @returns: GNC_DBI_PASS if the dbi library is safe to use,
 * GNC_DBI_FAIL_SETUP if the test could not be completed, or
 * GNC_DBI_FAIL_TEST if the bug was found.
 */
static GncDbiTestResult
conn_test_dbi_library (dbi_conn conn)
{
    gint64 testlonglong = -9223372036854775807LL, resultlonglong = 0;
    guint64 testulonglong = 9223372036854775807LLU, resultulonglong = 0;
    gdouble testdouble = 1.7976921348623157E+307, resultdouble = 0.0;
    dbi_result result;
    gchar doublestr[G_ASCII_DTOSTR_BUF_SIZE], *querystr;
    GncDbiTestResult retval = GNC_DBI_PASS;
    memset (doublestr, 0, sizeof (doublestr));

    result = dbi_conn_query (conn, "CREATE TEMPORARY TABLE numtest "
                             "( test_int BIGINT, test_unsigned BIGINT,"
                             " test_double FLOAT8 )");
    if (result == nullptr)
    {
        PWARN ("Test_DBI_Library: Create table failed");
        return GNC_DBI_FAIL_SETUP;
    }
    dbi_result_free (result);
    g_ascii_dtostr (doublestr, sizeof (doublestr), testdouble);
    querystr = g_strdup_printf ("INSERT INTO numtest VALUES (%" G_GINT64_FORMAT
                                ", %" G_GUINT64_FORMAT ", %s)",
                                testlonglong, testulonglong, doublestr);
    result = dbi_conn_query (conn, querystr);
    g_free (querystr);
    if (result == nullptr)
    {
        PWARN ("Test_DBI_Library: Failed to insert test row into table");
        return GNC_DBI_FAIL_SETUP;
    }
    dbi_result_free (result);
    gnc_push_locale (LC_NUMERIC, "C");
    result = dbi_conn_query (conn, "SELECT * FROM numtest");
    if (result == nullptr)
    {
        const char* errmsg;
        dbi_conn_error (conn, &errmsg);
        PWARN ("Test_DBI_Library: Failed to retrieve test row into table: %s",
               errmsg);
        dbi_conn_query (conn, "DROP TABLE numtest");
        gnc_pop_locale (LC_NUMERIC);
        return GNC_DBI_FAIL_SETUP;
    }
    while (dbi_result_next_row (result))
    {
        resultlonglong = dbi_result_get_longlong (result, "test_int");
        resultulonglong = dbi_result_get_ulonglong (result, "test_unsigned");
        resultdouble = dbi_result_get_double (result, "test_double");
    }
    gnc_pop_locale (LC_NUMERIC);
    if (testlonglong != resultlonglong)
    {
        PWARN ("Test_DBI_Library: LongLong Failed %" G_GINT64_FORMAT " != % "
               G_GINT64_FORMAT,
               testlonglong, resultlonglong);
        retval = GNC_DBI_FAIL_TEST;
    }
    if (testulonglong != resultulonglong)
    {
        PWARN ("Test_DBI_Library: Unsigned longlong Failed %" G_GUINT64_FORMAT " != %"
               G_GUINT64_FORMAT,
               testulonglong, resultulonglong);
        retval = GNC_DBI_FAIL_TEST;
    }
    /* A bug in libdbi stores only 7 digits of precision */
    if (testdouble >= resultdouble + 0.000001e307 ||
        testdouble <= resultdouble - 0.000001e307)
    {
        PWARN ("Test_DBI_Library: Double Failed %17e != %17e",
               testdouble, resultdouble);
        retval = GNC_DBI_FAIL_TEST;
    }
    return retval;
}


/* ========================== END OF FILE ===================== */
