MOD_SQL_TDS
===========
This is a contributed Module For mod_sql that allows it to use an SQL Backend that speaks TDS.
  (Currently Sybase and Microsoft SQL Server).

As this is a backend module for mod_sql, all of the interesting documentation is in README.mod_sql.  So look there for 
any configuration information you may have.  

This is being released in the hopes that someone will find it useful.

As always bugfixes, patches, etc. would be helpful.  And a note that you are using it would be nice also :)

INSTALLING:
===========

Obtain and install TDS libraries.
We use FreeTDS <http://www.freetds.org>.

This module should work the the SYBASE ASE Libraries but since I don't have them, or a platform that they run on
I cannot test them.  If someone gets them to work, please let me know.

Copy mod_sql_tds.c to ${PROFTPD_SRC}/contrib/ 

cd ${PROFTPD_SRC} 

  ./configure --with-modules=mod_sql:mod_sql_tds \
	   --with-libraries=$SYBASE/lib  \
	    --with-includes=$SYABSE/include

then make && install as usual.

On my system this command looks like this

  ./configure --with-modules=mod_sql:mod_sql_tds \
	    --with-libraries=/usr/local/lib  \
      --with-includes=/usr/local/include 

DIRECTIVES
----------

All directives are the same as mod_sql.
Except for a slight change to SQLConnectInfo:

The change here is that server must be specified in your interfaces file.
If you omit server, the module will look to see if you have one specified in the DSQUERY enviroment variable
if that doesn't exist we will exit. 

CAVEAT:  Due to the way FreeTDS(and sybase) libs appear to work. PERCALL + A Default chroot will probably give you problems at best, or flat out not work.  The short reason is that the TDS libs need access to the interfaces or freetds.conf file, and once you chroot, the process cannot access the file, and will not be able to open the DB.  PERSESSION (the default) will work just fine as the DB connection is opened prior to the chroot.

My Conf looks like this 

##
## SQL Server Database Backend
##
AuthPAMAuthoritative Off
SQLConnectInfo INOC@sql0 username password
SQLAuthTypes Plaintext
SQLUserInfo tbl_ftp_user userid passwd uid gid home shell
RequireValidShell off
SQLGroupInfo tbl_ftp_groups groupname gid members
SQLAuthenticate on
SQLLog PASS updatecount
SQLNamedQuery updatecount UPDATE "count=count+1 WHERE userid='%u'" tbl_ftp_user
SQLDefaultGID 500
SQLMinUserGID 400


My Interface file for FreeTDS looks like this 

[SQL0]
        host = 10.0.0.xxx
        port = 1433
        tds version = 7.0
[SQL1]
        host = 10.1.0.xxx 
        port = 1433
        tds version = 7.0
