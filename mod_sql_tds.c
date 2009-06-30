/*
 * ProFTPD: mod_tds -- Support for connecting to TDS databases.
 *                     Microsoft SQL Server /  Sybase ASE
 *
 * Copyright (c) 2001-2008 Patrick Muldoon 
 * From code borrowed from mod_sql_mysql
 * Copyright (c) 2001 Andrew Houghton
 * Copyright (c) 2004-2005 TJ Saunders
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307, USA.
 *
 * As a special exemption, Public Flood Software/MacGyver aka Habeeb J. Dihu
 * and other respective copyright holders give permission to link this program
 * with OpenSSL, and distribute the resulting executable, without including
 * the source code for OpenSSL in the source distribution.
 */
/*
 * $Libraries: -lsybdb $
 */

/* INTRO:
 *
 * mod_sql_tds is a mod_sql module for ProFTPD that allow data to be 
 * stored in database's that speak the TDS protocol (MS SQLServer, Sybase)
 *
 * Updated versions can always be found at http://labratsoftware.com/mod_sql_tds/
 *
 * COMMENTS / QUESTIONS / BUGS(not like there will be any bugs right?):
 * 
 * Can be sent to me Patrick Muldoon(doon@inoc.net). 
 * 
 * Thanks to Noah Roberts From Reachone for his patch correcting an error in cmd_select
 *
 */


/* 
 * Internal define used for debug and logging.  
 */
#define MOD_SQL_TDS_VERSION "mod_sql_tds/4.11"

#include <sybfront.h>
#include <sybdb.h>
#include <syberror.h>

#include "conf.h"
#include "../contrib/mod_sql.h"

/* 
 * timer-handling code adds the need for a couple of forward declarations
 */
MODRET cmd_close( cmd_rec *cmd );
module sql_tds_module;

#define ARBITRARY_MAX 256;

/* 
 * db_conn_struct: 
 * 
 */
struct db_conn_struct {

  char *server;       /* server name from INTERFACE file  */
  char *user;         /* User to access the server        */
  char *pass;         /* Password                         */
  char *db;           /* What Database Are we using       */
  
  DBPROCESS *dbproc;  /* Our connection to the DB         */
};

typedef struct db_conn_struct db_conn_t;

struct rowdata{
  char *value;
  char *name;
};
typedef struct rowdata rowdata_t;

struct tempdata{
  char **data;
  struct tempdata *next;
};

typedef struct tempdata tempdata_t;

struct conn_entry_struct {
  char *name;
  void *data;

  /* timer handling */

  int timer;
  int ttl;

  /* connection handling */

  unsigned int connections;
};

typedef struct conn_entry_struct conn_entry_t;

#define DEF_CONN_POOL_SIZE 10

static array_header *conn_cache;
static pool *conn_pool;

/*
 *  _sql_get_connection: walks the connection cache looking for the named
 *   connection.  Returns NULL if unsuccessful, a pointer to the conn_entry_t
 *   if successful.
 */
static conn_entry_t *_sql_get_connection(char *name){
  conn_entry_t *entry = NULL;
  int cnt;

  if (name == NULL) return NULL;

  /* walk the array looking for our entry */
  for (cnt=0; cnt < conn_cache->nelts; cnt++) {
    entry = ((conn_entry_t **) conn_cache->elts)[cnt];
    if (!strcmp(name, entry->name)) {
      return entry;
    }
  }
  
  return NULL;
}

/* 
 * _sql_add_connection: internal helper function to maintain a cache of 
 *  connections.  Since we expect the number of named connections to
 *  be small, simply use an array header to hold them.  We don't allow 
 *  duplicate connection names.
 *
 * Returns: NULL if the insertion was unsuccessful, a pointer to the 
 *  conn_entry_t that was created if successful.
 */
static void *_sql_add_connection(pool *p, char *name, db_conn_t *conn){
  conn_entry_t *entry = NULL;

  if ((!name) || (!conn) || (!p)) return NULL;
  
  if (_sql_get_connection(name)) {
    /* duplicated name */
    return NULL;
  }

  entry = (conn_entry_t *) pcalloc( p, sizeof( conn_entry_t ));
  entry->name = name;
  entry->data = conn;

  *((conn_entry_t **) push_array(conn_cache)) = entry;

  return entry;
}

/* _sql_check_cmd: tests to make sure the cmd_rec is valid and is 
 *  properly filled in.  If not, it's grounds for the daemon to
 *  shutdown.
 */
static void _sql_check_cmd(cmd_rec *cmd, char *msg){
  if ((!cmd) || (!cmd->tmp_pool)) {
    pr_log_pri(PR_LOG_ERR, MOD_SQL_TDS_VERSION ": '%s' was passed an invalid cmd_rec. Shutting down.", msg);
    sql_log(DEBUG_WARN, "'%s' was passed an invalid cmd_rec. Shutting down.", msg);
    end_login(1);
  }    

  return;
}

/*
 * _sql_timer_callback: when a timer goes off, this is the function
 *  that gets called.  This function makes assumptions about the 
 *  db_conn_t members.
 */
static int _sql_timer_callback(CALLBACK_FRAME){
  conn_entry_t *entry = NULL;
  int cnt = 0;
  cmd_rec *cmd = NULL;
 
  for (cnt=0; cnt < conn_cache->nelts; cnt++) {
    entry = ((conn_entry_t **) conn_cache->elts)[cnt];

    if (entry->timer == p2) {
      sql_log(DEBUG_INFO, "%s", " timer expired for connection '%s'",
		entry->name);
      cmd = _sql_make_cmd( conn_pool, 2, entry->name, "1" );
      cmd_close( cmd );
      SQL_FREE_CMD( cmd );
      entry->timer = 0;
    }
  }

  return 0;
}


/* 
 * _build_error: constructs a modret_t filled with error information;
 */
static modret_t *_build_error( cmd_rec *cmd, db_conn_t *conn ){
  
  char num[20] = {'\0'};
  snprintf(num, 20, "%u", 1234);
  if (!conn){
    return ERROR_MSG(cmd, MOD_SQL_TDS_VERSION, "badly formed request");
  }
  
  return ERROR_MSG(cmd, num, "An Internal Error Occured");
}

/*
 * _build_data: both cmd_select and cmd_procedure potentially
 *  return data to mod_sql; this function builds a modret to return
 *  that data.  
 *  Once we get here, we have rows to return, do it here
 */
static modret_t *_build_data( cmd_rec *cmd, db_conn_t *conn ){
  
  sql_data_t *sd = NULL;
  char **data = NULL;
  unsigned long cnt = 0;
  unsigned long index = 0;
  char **row = NULL;
  tempdata_t *td, *ptr;
  int x, rcount = 0;
    
  sql_log(DEBUG_FUNC, "%s", " >>> tds _build_data");
  if (!conn) 
    return ERROR_MSG(cmd, MOD_SQL_TDS_VERSION, "badly formed request");
  
  /* create a sql_data structure to eventually hold results */
  sd = (sql_data_t *) pcalloc(cmd->tmp_pool, sizeof(sql_data_t));
  
  sd->fnum = (unsigned long) dbnumcols(conn->dbproc); /* Number of columns in the result */
  sql_log(DEBUG_INFO, "%d columns in the result ", sd->fnum);
  
  /*create datastructure to hold the results */
  row = (char **) pcalloc(cmd->tmp_pool, sizeof(char *) * sd->fnum);
  
  /* setup the temp storage to hold all of our data here */
  td = (tempdata_t *)pcalloc(cmd->tmp_pool, sizeof(tempdata_t));
  
  /* make sure td points to the top of the list */
  ptr = td;
  
  ptr->data = pcalloc(cmd->tmp_pool,(sizeof(char *) * sd->fnum));
  ptr->next = NULL;
  
  /* need to bind the columns for our results */
  for (x=0;x<sd->fnum;x++){
    row[x] = (char *)pcalloc(cmd->tmp_pool, 256);
    dbbind(conn->dbproc, x+1, STRINGBIND, (DBINT) 0, row[x]);
  }
  
  /* return the rows from the query and populate temp list */
  while(dbnextrow(conn->dbproc) != NO_MORE_ROWS){
    if(rcount > 0){
      ptr->next = (tempdata_t *) pcalloc(cmd->tmp_pool, sizeof(tempdata_t));
      ptr = ptr->next;
      ptr->data = pcalloc(cmd->tmp_pool,(sizeof(char *) * sd->fnum));
      ptr->next = NULL;
      sql_log(DEBUG_INFO, "%s", " Created a new temp record");
    }
    
    /* add onto our temp record */
    for(x=0;x<sd->fnum;x++){
      ptr->data[x] = pstrdup(cmd->tmp_pool, row[x]);
    }
    rcount++; /* done with this row -- inc to the next */
  }
  
  sd->rnum = rcount;
  cnt = sd->rnum * sd->fnum; 
  data = (char **) pcalloc( cmd->tmp_pool, sizeof(char *) * (cnt + 1) );
  
  /* reset list ptr */
  ptr = td;
  while (ptr != NULL){
    for(x=0;x<sd->fnum;x++){
      data[index++] = pstrdup(cmd->tmp_pool, ptr->data[x]);
      sql_log(DEBUG_INFO, "copied %s to data[%d]",ptr->data[x], (index-1));
    }
    ptr = ptr->next;
  }
  data[index]=NULL;
  
  sd->data = data;
  return mod_create_data( cmd, (void *) sd );
}

/*
 * cmd_open: attempts to open a named connection to the database.
 *
 * Inputs:
 *  cmd->argv[0]: connection name
 *
 * Returns:
 *  either a properly filled error modret_t if a connection could not be
 *  opened, or a simple non-error modret_t.
 *
 */
MODRET cmd_open(cmd_rec *cmd){
  conn_entry_t *entry = NULL;
  db_conn_t *conn = NULL;
  LOGINREC *login;
  
  sql_log(DEBUG_FUNC, "%s", ">>> tds cmd_open");

  _sql_check_cmd(cmd, "cmd_open" );

  if (cmd->argc < 1) {
    sql_log(DEBUG_FUNC, "%s", "<<< tds cmd_open with argc < 1");
    return ERROR_MSG(cmd, MOD_SQL_TDS_VERSION, "badly formed request");
  }    

  /* get the named connection */

  if (!(entry = _sql_get_connection( cmd->argv[0]))) {
    sql_log(DEBUG_FUNC, "%s", "<<< tds cmd_open");
    return ERROR_MSG(cmd, MOD_SQL_TDS_VERSION, "Unknown Named Connection");  } 

  conn = (db_conn_t *) entry->data;
  
  /* if we're already open (connections > 0) increment connections 
   * reset our timer if we have one, and return HANDLED 
   */
  if (entry->connections > 0){ 
    /*FIXME: add check to see if connection still around*/
    entry->connections++;
    if (entry->timer) {
      pr_timer_reset( entry->timer, &sql_tds_module );
    }
    sql_log(DEBUG_INFO, "connection '%s' count is now %d", entry->name, entry->connections);
    sql_log(DEBUG_FUNC, "%s", "<<< tds cmd_open");
    return HANDLED(cmd);
  }
  
  if(dbinit() == FAIL){
    pr_log_pri(PR_LOG_ERR, MOD_SQL_TDS_VERSION  ": failed to init database. Shutting down.");
    sql_log(DEBUG_WARN, "%s", " failed to init tds database! Shutting down.");
    end_login(1);
  }
  
  sql_log(DEBUG_FUNC, "%s", "Attempting to call dblogin ");
  login = dblogin();
  DBSETLPWD(login,conn->pass);
  DBSETLAPP(login,"proftpd");
  DBSETLUSER(login,conn->user);
  sql_log(DEBUG_FUNC, "Adding user %s and password %s to login", conn->user,conn->pass);
  sql_log(DEBUG_FUNC, "%s", "calling dbopen");
 
  #ifdef PR_USE_NLS
	//We actually need to set the Char encoding before we open the connection
	// according to 
	// http://manuals.sybase.com:80/onlinebooks/group-cnarc/cng1110e/dblib/@Generic__BookTextView/37135;pt=37135#X
	//  The Client picks a default char and the server does conversions between the local and server sets.  
	// This should override any char set, that was set in your interfaces file
	if (pr_encode_get_encoding() != NULL){
		DBSETLCHARSET(login,pr_encode_get_charset());
		sql_log(DEBUG_FUNC,"Setting Client Character Set to '%s'",pr_encode_get_charset());
	}
   #endif /* !PR_USE_NLS */

  conn->dbproc = dbopen(login,conn->server);
  
  //free the login rec. 
  dbloginfree(login);
  sql_log(DEBUG_FUNC, "%s", "freeing our loginrec");
  if(!conn->dbproc){
    pr_log_pri(PR_LOG_ERR, MOD_SQL_TDS_VERSION ": failed to Login to DB server! Shutting down.");
    sql_log(DEBUG_WARN, "%s", " failed to Login to DB server. Shutting down.");
    end_login(1);
  }
  
  sql_log(DEBUG_FUNC, "attempting to switch to database: %s", conn->db);
  if(dbuse(conn->dbproc, conn->db) == FAIL){
    pr_log_pri(PR_LOG_ERR, MOD_SQL_TDS_VERSION ": failed to use database Shutting down.");
    sql_log(DEBUG_WARN, "%s", " failed to use database Shutting down.");
    end_login(1);
  }


  /* bump connections */
  entry->connections++;
  
  /* set up our timer if necessary */
  if (entry->ttl > 0) {
    entry->timer = pr_timer_add(entry->ttl, -1, 
			     &sql_tds_module, 
			     _sql_timer_callback,
				 "TDS Connection ttl"); 
    sql_log(DEBUG_INFO, "connection '%s' - %d second timer started",
	        entry->name, entry->ttl);

    /* timed connections get re-bumped so they don't go away when cmd_close
     * is called.
     */
    entry->connections++;
  }

  /* return HANDLED */
  
  sql_log(DEBUG_INFO, "connection '%s' opened count is now %d", entry->name, entry->connections);
  sql_log(DEBUG_FUNC, "%s", " <<< tds cmd_open");
  return HANDLED(cmd);
}

/*
 * cmd_close: attempts to close the named connection.
 *
 * Inputs:
 *  cmd->argv[0]: connection name
 * Optional:
 *  cmd->argv[1]: close immediately
 *
 * Returns:
 *  either a properly filled error modret_t if a connection could not be
 *  closed, or a simple non-error modret_t.  For the case of mod_sql_mysql,
 *  there are no error codes returned by the close call; other backends
 *  may be able to return a useful error message.
 * 
 *  If argv[1] exists and is not NULL, the connection should be immediately
 *  closed and the connection count should be reset to 0.
 */
MODRET cmd_close(cmd_rec *cmd){
  conn_entry_t *entry = NULL;
  db_conn_t *conn = NULL;

  sql_log(DEBUG_FUNC, "%s", ">>> tds cmd_close");

  _sql_check_cmd(cmd, "cmd_close");
  
  if ((cmd->argc < 1) || (cmd->argc > 2)) {
    sql_log(DEBUG_FUNC, "%s", "<<< tds cmd_close --badly formed request");
    return ERROR_MSG(cmd, MOD_SQL_TDS_VERSION, "badly formed request");
  }

  /* get the named connection */
  if (!(entry = _sql_get_connection( cmd->argv[0] ))) {
    sql_log(DEBUG_FUNC, "%s", "<<< tds cmd_close -- unknown named connection");
    return ERROR_MSG(cmd, MOD_SQL_TDS_VERSION, "Unknown Named Connection");
  }
  
  conn = (db_conn_t *) entry->data;
  
  /* if we're closed already (connections == 0) return HANDLED */
  if (entry->connections == 0) {
    sql_log(DEBUG_INFO, "connection '%s' count is now %d", entry->name, entry->connections);
    
    sql_log(DEBUG_FUNC, "%s", "<<< tds cmd_close - connections = 0");
    
    return HANDLED(cmd);
  }

  /* decrement connections. If our count is 0 or we received a second arg
   * close the connection, explicitly set the counter to 0, and remove any
   * timers.
   */
  if (((--entry->connections) == 0 ) || ((cmd->argc == 2) && (cmd->argv[1]))) {
    /* need to close connection here */
    dbclose(conn->dbproc);
    conn->dbproc = NULL;
    entry->connections = 0;
    
    if (entry->timer) {
      pr_timer_remove( entry->timer, &sql_tds_module );
      entry->timer = 0;
      sql_log(DEBUG_INFO, "connection '%s' - timer stopped",
		entry->name );
    }
    
    sql_log(DEBUG_INFO, "connection '%s' closed", entry->name);
  }

  sql_log(DEBUG_INFO, "connection '%s' count is now %d", entry->name, entry->connections);
  sql_log(DEBUG_FUNC, "%s", "<<< tds cmd_close");
  
  return HANDLED(cmd);
}

/*
 * cmd_defineconnection: takes all information about a database
 *  connection and stores it for later use.
 *
 * Inputs:
 *  cmd->argv[0]: connection name
 *  cmd->argv[1]: username portion of the SQLConnectInfo directive
 *  cmd->argv[2]: password portion of the SQLConnectInfo directive
 *  cmd->argv[3]: info portion of the SQLConnectInfo directive
 * Optional:
 *  cmd->argv[4]: time-to-live in seconds
 *
 * Returns:
 *  either a properly filled error modret_t if the connection could not
 *  defined, or a simple non-error modret_t.
 *
 * Notes:
 *  time-to-live is the length of time to allow a connection to remain unused;
 *  once that amount of time has passed, a connection should be closed and 
 *  it's connection count should be reduced to 0.  If ttl is 0, or ttl is not 
 *  a number or ttl is negative, the connection will be assumed to have no
 *  associated timer.
 */
MODRET cmd_defineconnection(cmd_rec *cmd){
  char *info = NULL;
  char *name = NULL;
  
  char *db = NULL;
  char *server = NULL;
  char *haveserver = NULL;
  
  conn_entry_t *entry = NULL;
  db_conn_t *conn = NULL; 
  
  sql_log(DEBUG_FUNC, "%s", ">>> tds cmd_defineconnection");
  
  _sql_check_cmd(cmd, "cmd_defineconnection");
  
  if ((cmd->argc < 4) || (cmd->argc > 5) || (!cmd->argv[0])) {
    sql_log(DEBUG_FUNC, "%s", "<<< tds cmd_defineconnection - invalid argv count");
    return ERROR_MSG(cmd, MOD_SQL_TDS_VERSION, "badly formed request");
  }
  
  if (!conn_pool) {
    pr_log_pri(PR_LOG_WARNING, "warning: the mod_sql_tds module has not been "
      "properly initialized.  Please make sure your --with-modules configure "
      "option lists mod_sql *before* mod_sql_tds, and recompile.");

    sql_log(DEBUG_FUNC, "%s", "The mod_sql_tds module has not been properly "
      "initialized.  Please make sure your --with-modules configure option "
      "lists mod_sql *before* mod_sql_tds, and recompile.");
    sql_log(DEBUG_FUNC, "%s", "<<< tds cmd_defineconnection");

    return PR_ERROR_MSG(cmd, MOD_SQL_TDS_VERSION, "uninitialized module");
  }

  conn = (db_conn_t *) palloc(conn_pool, sizeof(db_conn_t));
  
  name = pstrdup(conn_pool, cmd->argv[0]);
  conn->user = pstrdup(conn_pool, cmd->argv[1]);
  conn->pass = pstrdup(conn_pool, cmd->argv[2]);
  
  info = cmd->argv[3];
  
  db = pstrdup(cmd->tmp_pool, info);

  haveserver = strchr(db, '@');
  
  if (haveserver) {
    server = haveserver + 1;
    *haveserver = '\0';
  } else {
    /* Didn't specify a server, they could have it set in DSQUERY, so lets check there */
    sql_log(DEBUG_WARN, "%s", "No Host Specified! \t Checking Enviroment Variable");
    server = getenv("DSQUERY");
    if(server == NULL){
      log_pri(PR_LOG_ERR, "%s", "NO Host Specified and DSQUERY Enviroment Variable NOT Found! "
	      "-- Shutting down!.");
      sql_log(DEBUG_WARN, "%s", "NO Host Specified and DSQUERY Enviroment Variable NOT Found! "
		"-- Shutting down!.");
      end_login(1);  /* is this the right command to use here? */
    }
  }
  
  conn->server = pstrdup(conn_pool, server);
  conn->db   = pstrdup(conn_pool, db);
  
  /* insert the new conn_info into the connection hash */
  if (!(entry = _sql_add_connection(conn_pool, name, (void *) conn))) {
    sql_log(DEBUG_FUNC, "%s", "<<< tds cmd_defineconnection");
    return ERROR_MSG(cmd, MOD_SQL_TDS_VERSION, "Named Connection Already Exists");
  }

  entry->ttl = (cmd->argc == 5) ? 
    (int) strtol(cmd->argv[4], (char **)NULL, 10) : 0;
  if (entry->ttl < 0) 
    entry->ttl = 0;

  entry->timer = 0;
  entry->connections = 0;

  sql_log(DEBUG_INFO, "    name: '%s'", entry->name);
  sql_log(DEBUG_INFO, "    user: '%s'", conn->user);
  sql_log(DEBUG_INFO, "  server: '%s'", conn->server);
  sql_log(DEBUG_INFO, "      db: '%s'", conn->db);
  sql_log(DEBUG_INFO, "     ttl: '%d'", entry->ttl);
  sql_log(DEBUG_FUNC, "%s", "<<< tds cmd_defineconnection");
  return HANDLED(cmd);
}

/* 
 * cmd_exit: walks the connection cache and closes every
 *  open connection, resetting their connection counts to 0.
 */
static modret_t *cmd_exit(cmd_rec *cmd) {
  register unsigned int cnt = 0;
  
  sql_log(DEBUG_FUNC,"%s",">>> tds cmd_exit");
  conn_entry_t *entry = NULL;

  for (cnt=0; cnt < conn_cache->nelts; cnt++) {
    entry = ((conn_entry_t **) conn_cache->elts)[cnt];

    if (entry->connections > 0) {
      cmd = _sql_make_cmd( conn_pool, 2, entry->name, "1" );
      cmd_close( cmd );
      SQL_FREE_CMD( cmd );
    }
  }
  dbexit();  /* magic cleanup routine will clean up any remaining dbprocess that we might have missed */
  sql_log(DEBUG_FUNC,"%s","<<< tds cmd_exit");
  return HANDLED(cmd);
}

/*
 * cmd_select: executes a SELECT query. properly constructing the query
 *  based on the inputs.  See mod_sql.h for the definition of the _sql_data
 *  structure which is used to return the result data.
 *
 * cmd_select takes either exactly two inputs, or more than two.  If only
 *  two inputs are given, the second is a monolithic query string.  See 
 *  the examples below.
 *
 * Inputs:
 *  cmd->argv[0]: connection name
 *  cmd->argv[1]: table 
 *  cmd->argv[2]: select string
 * Optional:
 *  cmd->argv[3]: where clause 
 *  cmd->argv[4]: requested number of return rows (LIMIT)
 *  
 *  etc.        : other options, such as "GROUP BY", "ORDER BY",
 *                and "DISTINCT" will start at cmd->arg[5].  All 
 *                backends MUST support 'DISTINCT', the other
 *                arguments are optional (but encouraged).         
 *
 * Returns:
 *  either a properly filled error modret_t if the select failed, or a 
 *  modret_t with the result data filled in.
 *
 *  
 *  argv[] = "default","user","userid, count", "userid='aah'","2"
 *  query  = "SELECT TOP 2 userid, count FROM user WHERE userid='aah'"
 *
 *  argv[] = "default","usr1, usr2","usr1.foo, usr2.bar"
 *  query  = "SELECT usr1.foo, usr2.bar FROM usr1, usr2"
 *
 *  argv[] = "default","usr1","foo",,,"DISTINCT"
 *  query  = "SELECT DISTINCT foo FROM usr1"
 *
 *  argv[] = "default","bar FROM usr1 WHERE tmp=1 ORDER BY bar"
 *  query  = "SELECT bar FROM usr1 WHERE tmp=1 ORDER BY bar"
 *
 */
MODRET cmd_select(cmd_rec *cmd){
  conn_entry_t *entry = NULL;
  db_conn_t *conn = NULL;
  modret_t *cmr = NULL;
  modret_t *dmr = NULL;
  char *query = NULL;
  int cnt = 0;
  cmd_rec *close_cmd;

  sql_log(DEBUG_FUNC, "%s", ">>> tds cmd_select");

  _sql_check_cmd(cmd, "cmd_select");

  if (cmd->argc < 2) {
    sql_log(DEBUG_FUNC, "%s", "<<< tds cmd_select - Argc < 2");
    return ERROR_MSG(cmd, MOD_SQL_TDS_VERSION, "badly formed request");
  }

  /* get the named connection */
  entry = _sql_get_connection( cmd->argv[0] );
  if (!entry) {
    sql_log(DEBUG_FUNC, "%s", "<<< tds cmd_select - Failed to get Entry");
   return ERROR_MSG(cmd, MOD_SQL_TDS_VERSION, "Unknown Named Connection");
  }
  
  conn = (db_conn_t *) entry->data;

  cmr = cmd_open(cmd);
  if (MODRET_ERROR(cmr)) {
    sql_log(DEBUG_FUNC, "%s", "<<< tds cmd_select - error in cmd_open");
    return cmr;
  }

  /* construct the query string */
  if (cmd->argc == 2) {
    query = pstrcat(cmd->tmp_pool, "SELECT ", cmd->argv[1], NULL);
  } else {
    query = pstrcat( cmd->tmp_pool, cmd->argv[2], " FROM ", 
		     cmd->argv[1], NULL );
    if ((cmd->argc > 3) && (cmd->argv[3]))
      query = pstrcat( cmd->tmp_pool, query, " WHERE ", cmd->argv[3], NULL );
    if ((cmd->argc > 4) && (cmd->argv[4]))
      query = pstrcat( cmd->tmp_pool, "TOP ", cmd->argv[4], " ",  query,  NULL );
    if (cmd->argc > 5) {

      /* handle the optional arguments -- they're rare, so in this case
       * we'll play with the already constructed query string, but in 
       * general we should probably take optional arguments into account 
       * and put the query string together later once we know what they are.
       */
    
      for (cnt=5; cnt < cmd->argc; cnt++) {
	if ((cmd->argv[cnt]) && !strcasecmp("DISTINCT",cmd->argv[cnt])) {
	  query = pstrcat( cmd->tmp_pool, "DISTINCT ", query, NULL);
	}
      }
    }

    query = pstrcat( cmd->tmp_pool, "SELECT ", query, NULL);    
  }

  /* log the query string */
  sql_log( DEBUG_INFO, "query \"%s\"", query);

  /* perform the query.  if it doesn't work, log the error, close the
   * connection then return the error from the query processing.
   */
  dbcmd(conn->dbproc, query);
  if(dbsqlexec(conn->dbproc) != SUCCEED){
    dmr = _build_error( cmd, conn );
    close_cmd = _sql_make_cmd( cmd->tmp_pool, 1, entry->name );
    cmd_close(close_cmd);
    SQL_FREE_CMD( close_cmd );

    sql_log(DEBUG_FUNC, "%s", "<<< tds cmd_select DBSQLEXEC != SUCCEED");
    return dmr;
  }

  if(dbresults(conn->dbproc) == FAIL){
    dmr = _build_error( cmd, conn );
    close_cmd = _sql_make_cmd( cmd->tmp_pool, 1, entry->name );
    cmd_close(close_cmd);
    SQL_FREE_CMD( close_cmd );
    
    sql_log(DEBUG_FUNC, "%s", "<<< tds cmd_select DBRESULTS == FAIL");
    return dmr;
  }
  
  /* get the data. if it doesn't work, log the error, close the
   * connection then return the error from the data processing.
   */
  dmr = _build_data( cmd, conn );
  if (MODRET_ERROR(dmr)) {
    sql_log(DEBUG_FUNC, "%s", "<<< tds cmd_select");
    
    close_cmd = _sql_make_cmd( cmd->tmp_pool, 1, entry->name );
    cmd_close(close_cmd);
    SQL_FREE_CMD( close_cmd );
    
    return dmr;
  }    
  
  /* close the connection, return the data. */
  close_cmd = _sql_make_cmd( cmd->tmp_pool, 1, entry->name );
  cmd_close(close_cmd);
  SQL_FREE_CMD( close_cmd );
  
  sql_log(DEBUG_FUNC, "%s", "<<< tds cmd_select (normal)");
  return dmr;
}

/*
 * cmd_insert: executes an INSERT query, properly constructing the query
 *  based on the inputs.
 *
 * cmd_insert takes either exactly two inputs, or exactly four.  If only
 *  two inputs are given, the second is a monolithic query string.  See 
 *  the examples below.
 *
 * Inputs:
 *  cmd->argv[0]: connection name
 *  cmd->argv[1]: table
 *  cmd->argv[2]: field string
 *  cmd->argv[3]: value string
 *
 * Returns:
 *  either a properly filled error modret_t if the insert failed, or a 
 *  simple non-error modret_t.
 *
 * Example:
 *  These are example queries that would be executed for MySQL; other
 *  backends will have different SQL syntax.
 *  
 *  argv[] = "default","log","userid, date, count", "'aah', now(), 2"
 *  query  = "INSERT INTO log (userid, date, count) VALUES ('aah', now(), 2)"
 *
 *  argv[] = "default"," INTO foo VALUES ('do','re','mi','fa')"
 *  query  = "INSERT INTO foo VALUES ('do','re','mi','fa')"
 *
 * Notes:
 *  none
 */
MODRET cmd_insert(cmd_rec *cmd){
  conn_entry_t *entry = NULL;
  db_conn_t *conn = NULL;
  modret_t *cmr = NULL;
  modret_t *dmr = NULL;
  char *query = NULL;
  cmd_rec *close_cmd;

  sql_log(DEBUG_FUNC, "%s", ">>> tds cmd_insert");

  _sql_check_cmd(cmd, "cmd_insert");

  if ((cmd->argc != 2) && (cmd->argc != 4)) {
    sql_log(DEBUG_FUNC, "%s", "<<< tds cmd_insert");
    return ERROR_MSG(cmd, MOD_SQL_TDS_VERSION, "badly formed request");
  }

  /* get the named connection */
  entry = _sql_get_connection( cmd->argv[0] );
  if (!entry) {
    sql_log(DEBUG_FUNC, "%s", "<<< tds cmd_insert");
    return ERROR_MSG(cmd, MOD_SQL_TDS_VERSION, "Unknown named connection");
  }

  conn = (db_conn_t *) entry->data;

  cmr = cmd_open(cmd);
  if (MODRET_ERROR(cmr)) {
    sql_log(DEBUG_FUNC, "%s", "<<< tds cmd_insert");
    return cmr;
  }
  
  /* construct the query string */
  if (cmd->argc == 2) {
    query = pstrcat(cmd->tmp_pool, "INSERT ", cmd->argv[1], NULL);
  } else {
    query = pstrcat( cmd->tmp_pool, "INSERT INTO ", cmd->argv[1], " (",
		     cmd->argv[2], ") VALUES (", cmd->argv[3], ")",
		     NULL );
  }
  
  /* log the query string */
  sql_log( DEBUG_INFO, "query \"%s\"", query);
  
  /* perform the query.  if it doesn't work, log the error, close the
   * connection (and log any errors there, too) then return the error
   * from the query processing.
   */
  dbcmd(conn->dbproc, query);
  dbsqlexec(conn->dbproc);
  if(dbresults(conn->dbproc) != SUCCEED){
    
    dmr = _build_error( cmd, conn );
    
    close_cmd = _sql_make_cmd( cmd->tmp_pool, 1, entry->name );
    cmd_close(close_cmd);
    SQL_FREE_CMD( close_cmd );

    sql_log(DEBUG_FUNC, "%s", "<<< tds cmd_insert");
    return dmr;
  }

  /* close the connection and return HANDLED. */
  close_cmd = _sql_make_cmd( cmd->tmp_pool, 1, entry->name );
  cmd_close(close_cmd);
  SQL_FREE_CMD( close_cmd );

  sql_log(DEBUG_FUNC, "%s", "<<< tds cmd_insert");
  return HANDLED(cmd);
}

/*
 * cmd_update: executes an UPDATE query, properly constructing the query
 *  based on the inputs.
 *
 * cmd_update takes either exactly two, three, or four inputs.  If only
 *  two inputs are given, the second is a monolithic query string.  See 
 *  the examples below.
 *
 * Inputs:
 *  cmd->argv[0]: connection name
 *  cmd->argv[1]: table
 *  cmd->argv[2]: update string
 * Optional:
 *  cmd->argv[3]: where string
 *
 * Returns:
 *  either a properly filled error modret_t if the update failed, or a 
 *  simple non-error modret_t. *  
 *
 */
MODRET cmd_update(cmd_rec *cmd){
  conn_entry_t *entry = NULL;
  db_conn_t *conn = NULL;
  modret_t *cmr = NULL;
  modret_t *dmr = NULL;
  char *query = NULL;
  cmd_rec *close_cmd;

  sql_log(DEBUG_FUNC, "%s", ">>> tds cmd_update");

  _sql_check_cmd(cmd, "cmd_update");

  if ((cmd->argc < 2) || (cmd->argc > 4)) {
    sql_log(DEBUG_FUNC, "%s", "<<< tds cmd_update");
    return ERROR_MSG(cmd, MOD_SQL_TDS_VERSION, "badly formed request");  }

  /* get the named connection */
  entry = _sql_get_connection( cmd->argv[0] );
  if (!entry) {
    sql_log(DEBUG_FUNC, "%s", "<<< tds cmd_update");
    return ERROR_MSG(cmd, MOD_SQL_TDS_VERSION, "Unknown Named Connection");
  }

  conn = (db_conn_t *) entry->data;

  cmr = cmd_open(cmd);
  if (MODRET_ERROR(cmr)) {
    sql_log(DEBUG_FUNC, "%s", "<<< tds cmd_update");
    return cmr;
  }

  if (cmd->argc == 2) {
    query = pstrcat(cmd->tmp_pool, "UPDATE ", cmd->argv[1], NULL);
  } else {
    /* construct the query string */
    query = pstrcat( cmd->tmp_pool, "UPDATE ", cmd->argv[1], " SET ",
		     cmd->argv[2], NULL );
    if ((cmd->argc > 3) && (cmd->argv[3]))
      query = pstrcat( cmd->tmp_pool, query, " WHERE ", cmd->argv[3], NULL );
  }

  /* log the query string */
  sql_log( DEBUG_INFO, "query \"%s\"", query);

  /* perform the query.  if it doesn't work close the connection, then
   * return the error from the query processing.
   */
  dbcmd(conn->dbproc, query);
  dbsqlexec(conn->dbproc);
  if(dbresults(conn->dbproc) != SUCCEED){
    
    dmr = _build_error( cmd, conn );
    
    close_cmd = _sql_make_cmd( cmd->tmp_pool, 1, entry->name );
    cmd_close(close_cmd);
    SQL_FREE_CMD( close_cmd );
    
    sql_log(DEBUG_FUNC, "%s", "<<< tds cmd_update");
    return dmr;
  }
  
  /* close the connection, return HANDLED.  */
  close_cmd = _sql_make_cmd( cmd->tmp_pool, 1, entry->name );
  cmd_close(close_cmd);
  SQL_FREE_CMD( close_cmd );
  
  sql_log(DEBUG_FUNC, "%s", "<<< tds cmd_update");
  return HANDLED(cmd);
}

/*
 * cmd_procedure: executes a stored procedure.
 *
 * Inputs:
 *  cmd->argv[0]: connection name
 *  cmd->argv[1]: procedure name
 *  cmd->argv[2]: procedure string
 *
 * Returns:
 *  either a properly filled error modret_t if the procedure failed in
 *  some way, or a modret_t with the result data.  If a procedure
 *  returns data, it should be returned in the same way as cmd_select.
 *
 * Notes:
 *  not every backend will support stored procedures.  Backends which do
 *  not support stored procedures should return an error with a descriptive
 *  error message (something like 'backend does not support procedures').
 */
MODRET cmd_procedure(cmd_rec *cmd)
{
  sql_log(DEBUG_FUNC, "%s", ">>> tds cmd_procedure");

  _sql_check_cmd(cmd, "cmd_procedure");
  
  if (cmd->argc != 3) {
    sql_log(DEBUG_FUNC, "%s", "<<< tds cmd_procedure");
    return ERROR_MSG(cmd, MOD_SQL_TDS_VERSION, "badly formed request");  }

  /* TDS has procedures, just need to write code to do it */

  sql_log(DEBUG_FUNC, "%s", "<<< tds cmd_procedure");
  
  return ERROR_MSG(cmd, MOD_SQL_TDS_VERSION, "backend does not support procedures -- YET!");
}

/*
 * cmd_query: executes a freeform query string, with no syntax checking.
 *
 * cmd_query takes exactly two inputs, the connection and the query string.
 *
 * Inputs:
 *  cmd->argv[0]: connection name
 *  cmd->argv[1]: query string
 *
 * Returns:
 *  depending on the query type, returns a modret_t with data, a non-error
 *  modret_t, or a properly filled error modret_t if the query failed.
 *
 * Example:
 *  None.  The query should be passed directly to the backend database.
 *  
 * Notes:
 *  None.
 */
MODRET cmd_query(cmd_rec *cmd){
  conn_entry_t *entry = NULL;
  db_conn_t *conn = NULL;
  modret_t *cmr = NULL;
  modret_t *dmr = NULL;
  char *query = NULL;
  cmd_rec *close_cmd;

  sql_log(DEBUG_FUNC, "%s", ">>> tds cmd_query");

  _sql_check_cmd(cmd, "cmd_query");

  if (cmd->argc != 2) {
    sql_log(DEBUG_FUNC, "%s", "<<< tds cmd_query");
    return ERROR_MSG(cmd, MOD_SQL_TDS_VERSION, "badly formed request");
  }

  /* get the named connection */
  entry = _sql_get_connection( cmd->argv[0] );
  if (!entry) {
    sql_log(DEBUG_FUNC, "%s", "<<< tds cmd_query");
    return ERROR_MSG(cmd, MOD_SQL_TDS_VERSION, "unknown named connection");
  }
  
  conn = (db_conn_t *) entry->data;
  
  cmr = cmd_open(cmd);
  if (MODRET_ERROR(cmr)) {
    sql_log(DEBUG_FUNC, "%s", "<<< tds cmd_query");
    return cmr;
  }
  
  query = pstrcat(cmd->tmp_pool, cmd->argv[1], NULL);

  /* log the query string */
  sql_log( DEBUG_INFO, "query \"%s\"", query);

  /* perform the query.  if it doesn't work close the connection, then
   * return the error from the query processing.
   */
  dbcmd(conn->dbproc, query);
  dbsqlexec(conn->dbproc);
  if(dbresults(conn->dbproc) != SUCCEED){
    
    dmr = _build_error( cmd, conn );
    
    close_cmd = _sql_make_cmd( cmd->tmp_pool, 1, entry->name );
    cmd_close(close_cmd);
    SQL_FREE_CMD( close_cmd );
    
    sql_log(DEBUG_FUNC, "%s", "<<< tds cmd_query");
    return dmr;
  }

  /* get data if necessary. if it doesn't work, log the error, close the
   * connection then return the error from the data processing.
   */
  
  if ( dbnumcols( conn->dbproc ) ) {
    dmr = _build_data( cmd, conn );
    if (MODRET_ERROR(dmr)) {
      sql_log(DEBUG_FUNC, "%s", "<<< tds cmd_query");
    }
  } else {
    dmr = HANDLED(cmd);
  }
  
  /* close the connection, return the data. */
  close_cmd = _sql_make_cmd( cmd->tmp_pool, 1, entry->name );
  cmd_close(close_cmd);
  SQL_FREE_CMD( close_cmd );
  
  sql_log(DEBUG_FUNC, "%s", "<<< tds cmd_query");
  return dmr;
}

/*
 * cmd_escapestring: certain strings sent to a database should be properly
 *  escaped -- for instance, quotes need to be escaped to insure that 
 *  a query string is properly formatted.  cmd_escapestring does whatever
 *  is necessary to escape the special characters in a string. 
 *
 * Inputs:
 *  cmd->argv[0]: connection name
 *  cmd->argv[1]: string to escape
 *
 * Returns:
 *  DBSafe String (escapes both ' and ") 
 */
MODRET cmd_escapestring(cmd_rec * cmd){

  conn_entry_t *entry = NULL;
  db_conn_t *conn = NULL;
  cmd_rec *close_cmd;
  modret_t *cmr = NULL;
  char *unescaped = NULL;
  char *escaped = NULL;
  
  sql_log(DEBUG_FUNC, "%s", ">>> tds cmd_escapestring");

  _sql_check_cmd(cmd, "cmd_escapestring");

  if (cmd->argc != 2) {
    sql_log(DEBUG_FUNC, "%s", "<<< tds cmd_escapestring");
    return ERROR_MSG(cmd, MOD_SQL_TDS_VERSION, "badly formed request");
  }

  /* get the named connection */
  entry = _sql_get_connection( cmd->argv[0] );
  if (!entry) {
    sql_log(DEBUG_FUNC, "%s", "<<< tds cmd_escapestring");
   return ERROR_MSG(cmd, MOD_SQL_TDS_VERSION, "unknown named connectiont");
  }

  conn = (db_conn_t *) entry->data;
  
  /* Make sure the connection is opened */ 
  cmr = cmd_open(cmd);
  if (MODRET_ERROR(cmr)) {
    sql_log(DEBUG_FUNC, "%s", "<<< tds cmd_escapestring");
    return cmr;
  }

  /** 
   * Pass the unescaped to dbsafestr() to make it safe 
   */
  unescaped = cmd->argv[1];
  escaped = (char *) pcalloc(cmd->tmp_pool, sizeof(char) * (strlen(unescaped) * 2) + 1);
  dbsafestr(conn->dbproc,unescaped,-1,escaped,-1,DBBOTH);
  
  sql_log(DEBUG_FUNC, "before: '%s' after '%s'", unescaped,escaped);
  sql_log(DEBUG_FUNC, "%s", "<<< tds cmd_escapestring");
  
  /* close the connection, return the data. */
  close_cmd = _sql_make_cmd( cmd->tmp_pool, 1, entry->name );
  cmd_close(close_cmd);
  SQL_FREE_CMD( close_cmd );
  return mod_create_data(cmd, (void *) escaped);
}

/*
 * cmd_checkauth: some backend databases may provide backend-specific
 *  methods to check passwords.  This function takes a cleartext password
 *  and a hashed password and checks to see if they are the same.
 *
 * Inputs:
 *  cmd->argv[0]: connection name
 *  cmd->argv[1]: cleartext string
 *  cmd->argv[2]: hashed string
 *
 * Returns:
 *  HANDLED(cmd)                   -- passwords match
 *  ERROR_INT(cmd,AUTH_NOPWD)      -- missing password
 *  ERROR_INT(cmd,AUTH_BADPWD)     -- passwords don't match
 *  ERROR_INT(cmd,AUTH_DISABLEPWD) -- password is disabled
 *  ERROR_INT(cmd,AUTH_AGEPWD)     -- password is aged
 *  ERROR(cmd)                     -- unknown error
 *
 * Notes:
 *  If this backend does not provide this functionality, this cmd *must*
 *  return ERROR.
 */
MODRET cmd_checkauth(cmd_rec * cmd){
  sql_log(DEBUG_FUNC, "%s", ">>> tds cmd_checkauth");
  _sql_check_cmd(cmd, "cmd_checkauth");
  /* don't support this at the moment */
  return ERROR_MSG(cmd, MOD_SQL_TDS_VERSION, "backend does not support check_auth");
}

/*
 * cmd_identify: returns API information and an identification string for 
 *  the backend handler.  mod_sql will call this at initialization and 
 *  display the identification string.  The API version information is 
 *  used by mod_sql to identify available command handlers.
 *
 * Inputs:
 *  None.  The cmd->tmp_pool can be used to construct the return data, but
 *  do not depend on any other portion of the cmd_rec to be useful in any way.
 *
 * Returns:
 *  A sql_data_t of *exactly* this form:
 *   sql_data_t->rnum    = 1;
 *   sql_data_t->fnum    = 2;
 *   sql_data_t->data[0] = "identification string"
 *   sql_data_t->data[0] = "API version"
 *
 * Notes:
 *  See mod_sql.h for currently accepted APIs.
 */
MODRET cmd_identify(cmd_rec * cmd){
  sql_data_t *sd = NULL;

  _sql_check_cmd(cmd, "cmd_identify");

  sd = (sql_data_t *) pcalloc( cmd->tmp_pool, sizeof(sql_data_t));
  sd->data = (char **) pcalloc( cmd->tmp_pool, sizeof(char *) * 2);

  sd->rnum = 1;
  sd->fnum = 2;

  sd->data[0] = MOD_SQL_TDS_VERSION;
  sd->data[1] = MOD_SQL_API_V1;

  return mod_create_data(cmd, (void *) sd);
}  

/*
 * cmd_cleanup: cleans up any initialisations made during module preparations
 *  (see cmd_prepre).
 *
 * Inputs:
 *  None.
 *
 * Returns:
 *  Success.
 */
MODRET cmd_cleanup(cmd_rec *cmd) {
	destroy_pool(conn_pool);
	conn_pool = NULL;
	conn_cache = NULL;
	return mod_create_data(cmd, NULL);
}

/*
 * cmd_prepare: prepares this mod_sql_mysql module for running.
 *
 * Inputs:
 *  cmd->argv[0]:  A pool to be used for any necessary preparations.
 *
 * Returns:
 *  Success.
 */
MODRET cmd_prepare(cmd_rec *cmd) {
	if (cmd->argc != 1) {
		return PR_ERROR(cmd);
	}
 
	conn_pool = (pool *) cmd->argv[0];
	if (conn_cache == NULL) {
		conn_cache = make_array((pool *) cmd->argv[0], DEF_CONN_POOL_SIZE,
		   sizeof(conn_entry_t *));
	} 
	return mod_create_data(cmd, NULL);
}

/* 
 * sql_tds_cmdtable: mod_sql requires each backend module to define a cmdtable
 *  with this exact name. ALL these functions must be defined; mod_sql checks
 *  that they all exist on startup and ProFTPD will refuse to start if they
 *  aren't defined.
 */
cmdtable sql_tds_cmdtable[] = {
  { CMD, "sql_open",             G_NONE, cmd_open,             FALSE, FALSE },
  { CMD, "sql_close",            G_NONE, cmd_close,            FALSE, FALSE },
  { CMD, "sql_exit",             G_NONE, cmd_exit,             FALSE, FALSE },
  { CMD, "sql_defineconnection", G_NONE, cmd_defineconnection, FALSE, FALSE },
  { CMD, "sql_select",           G_NONE, cmd_select,           FALSE, FALSE },
  { CMD, "sql_insert",           G_NONE, cmd_insert,           FALSE, FALSE },
  { CMD, "sql_update",           G_NONE, cmd_update,           FALSE, FALSE },
  { CMD, "sql_procedure",        G_NONE, cmd_procedure,        FALSE, FALSE },
  { CMD, "sql_query",            G_NONE, cmd_query,            FALSE, FALSE },
  { CMD, "sql_escapestring",     G_NONE, cmd_escapestring,     FALSE, FALSE },
  { CMD, "sql_checkauth",        G_NONE, cmd_checkauth,        FALSE, FALSE },
  { CMD, "sql_identify",         G_NONE, cmd_identify,         FALSE, FALSE },
  { CMD, "sql_cleanup",		 G_NONE, cmd_cleanup,	       FALSE, FALSE },
  { CMD, "sql_prepare",		 G_NONE, cmd_prepare,	       FALSE, FALSE },

  { 0, NULL }
};

static void sql_tds_mod_load_ev(const void *event_data, void *user_data) {

  if (strcmp("mod_sql_tds.c", (const char *) event_data) == 0) {
    /* Register ourselves with mod_sql. */
    if (sql_register_backend("tds", sql_tds_cmdtable) < 0) {
      pr_log_pri(PR_LOG_NOTICE, MOD_SQL_TDS_VERSION
        ": notice: error registering backend: %s", strerror(errno));
      end_login(1);
    }
  }
}

static void sql_tds_mod_unload_ev(const void *event_data, void *user_data) {

  if (strcmp("mod_sql_tds.c", (const char *) event_data) == 0) {
    /* Unegister ourselves with mod_sql. */
    if (sql_unregister_backend("tds") < 0) {
      pr_log_pri(PR_LOG_NOTICE, MOD_SQL_TDS_VERSION
        ": notice: error unregistering backend: %s", strerror(errno));
      end_login(1);
    }

    /* Unregister ourselves from all events. */
    pr_event_unregister(&sql_tds_module, NULL, NULL);
  }
}


/* Initialization routines
 */

static int sql_tds_init(void) {

  /* Register listeners for the load and unload events. */
  pr_event_register(&sql_tds_module, "core.module-load",
    sql_tds_mod_load_ev, NULL);
  pr_event_register(&sql_tds_module, "core.module-unload",
    sql_tds_mod_unload_ev, NULL);

  return 0;
}


/*
 * sql_tds_init: Used to initialize the connection cache and register
 *  the exit handler.
 */
static int sql_tds_sess_init(void){
  conn_pool = make_sub_pool(session.pool);
  if( conn_cache == NULL ) {
  	conn_cache = make_array(make_sub_pool(session.pool), DEF_CONN_POOL_SIZE,
            sizeof(conn_entry_t *));
  }

  return 0;
}

/*
 * sql_tds_module: The standard module struct for all ProFTPD modules.
 *  We use the pre-fork handler to initialize the conn_cache array header.
 *  Other backend modules may not need any init functions, or may need
 *  to extend the init functions to initialize other internal variables.
 */
module sql_tds_module = {
  NULL, NULL,                   /* Always NULL */
  0x20,                         /* API Version 2.0 */
  "sql_tds",
  /* Module Config Directive */
  NULL,                   
  /* Module Command Handlers */
  NULL,                        
  /* Module Authentication Handlers */
  NULL,                  
  /* Module Init */
  sql_tds_init,
  /* Session Init */
  sql_tds_sess_init,
  /* Module Version */
  MOD_SQL_TDS_VERSION
  };
