// cloner.cpp - copy a database (export/import basically)

/**
*    Copyright (C) 2008 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#include "mongo/platform/basic.h"

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/bson/util/builder.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/cloner.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/copydb.h"
#include "mongo/db/commands/rename_collection.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index_builder.h"
#include "mongo/db/instance.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/isself.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplogreader.h"
#include "mongo/db/storage_options.h"
#include "mongo/util/log.h"

namespace mongo {

    MONGO_LOG_DEFAULT_COMPONENT_FILE(::mongo::logger::LogComponent::kStorage);

    BSONElement getErrField(const BSONObj& o);

    /* for index info object:
         { "name" : "name_1" , "ns" : "foo.index3" , "key" :  { "name" : 1.0 } }
       we need to fix up the value in the "ns" parameter so that the name prefix is correct on a
       copy to a new name.
    */
    BSONObj fixindex(const string& newDbName, BSONObj o) {
        BSONObjBuilder b;
        BSONObjIterator i(o);
        while ( i.moreWithEOO() ) {
            BSONElement e = i.next();
            if ( e.eoo() )
                break;

            // for now, skip the "v" field so that v:0 indexes will be upgraded to v:1
            if ( string("v") == e.fieldName() ) {
                continue;
            }

            if ( string("ns") == e.fieldName() ) {
                uassert( 10024 , "bad ns field for index during dbcopy", e.type() == String);
                const char *p = strchr(e.valuestr(), '.');
                uassert( 10025 , "bad ns field for index during dbcopy [2]", p);
                string newname = newDbName + p;
                b.append("ns", newname);
            }
            else {
                b.append(e);
            }
        }

        BSONObj res= b.obj();

        return res;
    }

    Cloner::Cloner() { }

    struct Cloner::Fun {
        Fun(OperationContext* txn, const string& dbName)
            :lastLog(0),
             txn(txn),
             _dbName(dbName)
        {}

        void operator()( DBClientCursorBatchIterator &i ) {
            invariant(from_collection.coll() != "system.indexes");

            // XXX: can probably take dblock instead
            Lock::GlobalWrite lk(txn->lockState());

            // Make sure database still exists after we resume from the temp release
            bool unused;
            Database* db = dbHolder().getOrCreate(txn, _dbName, unused);

            bool createdCollection = false;
            Collection* collection = NULL;

            collection = db->getCollection( txn, to_collection );
            if ( !collection ) {
                massert( 17321,
                         str::stream()
                         << "collection dropped during clone ["
                         << to_collection.ns() << "]",
                         !createdCollection );
                WriteUnitOfWork wunit(txn->recoveryUnit());
                createdCollection = true;
                collection = db->createCollection( txn, to_collection.ns() );
                verify( collection );
                wunit.commit();
            }

            while( i.moreInCurrentBatch() ) {
                if ( numSeen % 128 == 127 ) {
                    time_t now = time(0);
                    if( now - lastLog >= 60 ) {
                        // report progress
                        if( lastLog )
                            log() << "clone " << to_collection << ' ' << numSeen << endl;
                        lastLog = now;
                    }
                }

                BSONObj tmp = i.nextSafe();

                /* assure object is valid.  note this will slow us down a little. */
                const Status status = validateBSON(tmp.objdata(), tmp.objsize());
                if (!status.isOK()) {
                    log() << "Cloner: skipping corrupt object from " << from_collection
                          << ": " << status.reason();
                    continue;
                }

                ++numSeen;
                WriteUnitOfWork wunit(txn->recoveryUnit());

                BSONObj js = tmp;

                StatusWith<DiskLoc> loc = collection->insertDocument( txn, js, true );
                if ( !loc.isOK() ) {
                    error() << "error: exception cloning object in " << from_collection
                            << ' ' << loc.toString() << " obj:" << js;
                }
                uassertStatusOK( loc.getStatus() );
                if (logForRepl)
                    repl::logOp(txn, "i", to_collection.ns().c_str(), js);

                wunit.commit();
                txn->recoveryUnit()->commitIfNeeded();

                RARELY if ( time( 0 ) - saveLast > 60 ) {
                    log() << numSeen << " objects cloned so far from collection " << from_collection;
                    saveLast = time( 0 );
                }
            }
        }

        time_t lastLog;
        OperationContext* txn;
        const string _dbName;

        int64_t numSeen;
        NamespaceString from_collection;
        NamespaceString to_collection;
        time_t saveLast;
        list<BSONObj> *indexesToBuild;  // deferred query results (e.g. index insert/build)
        bool logForRepl;
        bool _mayYield;
        bool _mayBeInterrupted;
    };

    /* copy the specified collection
       isindex - if true, this is system.indexes collection, in which we do some transformation when copying.
    */
    void Cloner::copy(OperationContext* txn,
                      const string& toDBName,
                      const NamespaceString& from_collection,
                      const NamespaceString& to_collection,
                      bool logForRepl,
                      bool masterSameProcess,
                      bool slaveOk,
                      bool mayYield,
                      bool mayBeInterrupted,
                      Query query) {

        list<BSONObj> indexesToBuild;
        LOG(2) << "\t\tcloning collection " << from_collection << " to " << to_collection << " on " << _conn->getServerAddress() << " with filter " << query.toString() << endl;

        Fun f(txn, toDBName);
        f.numSeen = 0;
        f.from_collection = from_collection;
        f.to_collection = to_collection;
        f.saveLast = time( 0 );
        f.indexesToBuild = &indexesToBuild;
        f.logForRepl = logForRepl;
        f._mayYield = mayYield;
        f._mayBeInterrupted = mayBeInterrupted;

        int options = QueryOption_NoCursorTimeout | ( slaveOk ? QueryOption_SlaveOk : 0 );
        {
            Lock::TempRelease tempRelease(txn->lockState());
            _conn->query(stdx::function<void(DBClientCursorBatchIterator &)>(f), from_collection,
                         query, 0, options);
        }

        // We are under lock here again, so reload the database in case it may have disappeared
        // during the temp release
        bool unused;
        Database* db = dbHolder().getOrCreate(txn, toDBName, unused);

        if ( indexesToBuild.size() ) {
            for (list<BSONObj>::const_iterator i = indexesToBuild.begin();
                 i != indexesToBuild.end();
                 ++i) {

                BSONObj spec = *i;
                string ns = spec["ns"].String(); // this was fixed when pulled off network
                Collection* collection = db->getCollection( txn, ns );
                if ( !collection ) {
                    collection = db->createCollection( txn, ns );
                    verify( collection );
                }

                Status status = collection->getIndexCatalog()->createIndex(txn, spec, mayBeInterrupted);
                if ( status.code() == ErrorCodes::IndexAlreadyExists ) {
                    // no-op
                }
                else if ( !status.isOK() ) {
                    error() << "error creating index when cloning spec: " << spec
                            << " error: " << status.toString();
                    uassertStatusOK( status );
                }

                if (logForRepl)
                    repl::logOp(txn, "i", to_collection.ns().c_str(), spec);

                txn->recoveryUnit()->commitIfNeeded();

            }
        }
    }

    void Cloner::copyIndexes(OperationContext* txn,
                             const string& toDBName,
                             const NamespaceString& from_collection,
                             const NamespaceString& to_collection,
                             bool logForRepl,
                             bool masterSameProcess,
                             bool slaveOk,
                             bool mayYield,
                             bool mayBeInterrupted) {

        LOG(2) << "\t\t copyIndexes " << from_collection << " to " << to_collection
               << " on " << _conn->getServerAddress();

        list<BSONObj> indexesToBuild;

        {
            Lock::TempRelease tempRelease(txn->lockState());
            indexesToBuild = _conn->getIndexSpecs( from_collection,
                                                   slaveOk ? QueryOption_SlaveOk : 0 );
        }

        // We are under lock here again, so reload the database in case it may have disappeared
        // during the temp release
        bool unused;
        Database* db = dbHolder().getOrCreate(txn, toDBName, unused);

        if ( indexesToBuild.size() ) {
            for (list<BSONObj>::const_iterator i = indexesToBuild.begin();
                 i != indexesToBuild.end();
                 ++i) {

                BSONObj spec = fixindex( to_collection.db().toString(), *i );
                string ns = spec["ns"].String();
                Collection* collection = db->getCollection( txn, ns );
                if ( !collection ) {
                    collection = db->createCollection( txn, ns );
                    verify( collection );
                }

                Status status = collection->getIndexCatalog()->createIndex(txn,
                                                                           spec,
                                                                           mayBeInterrupted);
                if ( status.code() == ErrorCodes::IndexAlreadyExists ) {
                    // no-op
                }
                else if ( !status.isOK() ) {
                    error() << "error creating index when cloning spec: " << spec
                            << " error: " << status.toString();
                    uassertStatusOK( status );
                }

                if (logForRepl)
                    repl::logOp(txn, "i", to_collection.ns().c_str(), spec);

                txn->recoveryUnit()->commitIfNeeded();

            }
        }
    }

    /**
     * validate the cloner query was successful
     * @param cur   Cursor the query was executed on
     * @param errCode out  Error code encountered during the query
     * @param errmsg out  Error message encountered during the query
     */
    bool validateQueryResults(const auto_ptr<DBClientCursor>& cur,
                                      int32_t* errCode,
                                      string& errmsg) {
        if ( cur.get() == 0 )
            return false;
        if ( cur->more() ) {
            BSONObj first = cur->next();
            BSONElement errField = getErrField(first);
            if(!errField.eoo()) {
                errmsg = errField.str();
                if (errCode)
                    *errCode = first.getIntField("code");
                return false;
            }
            cur->putBack(first);
        }
        return true;
    }

    bool Cloner::copyCollection(OperationContext* txn,
                                const string& ns,
                                const BSONObj& query,
                                string& errmsg,
                                bool mayYield,
                                bool mayBeInterrupted,
                                bool shouldCopyIndexes,
                                bool logForRepl) {

        const NamespaceString nss(ns);
        Lock::DBWrite dbWrite(txn->lockState(), nss.db());
        WriteUnitOfWork wunit(txn->recoveryUnit());

        const string dbName = nss.db().toString();

        bool unused;
        Database* db = dbHolder().getOrCreate(txn, dbName, unused);

        // config
        string temp = dbName + ".system.namespaces";
        BSONObj config = _conn->findOne(temp , BSON("name" << ns));
        if (config["options"].isABSONObj()) {
            Status status = userCreateNS(txn, db, ns, config["options"].Obj(), logForRepl, 0);
            if ( !status.isOK() ) {
                errmsg = status.toString();
                return false;
            }
        }

        // main data
        copy(txn, dbName,
             nss, nss,
             logForRepl, false, true, mayYield, mayBeInterrupted,
             Query(query).snapshot());

        /* TODO : copyIndexes bool does not seem to be implemented! */
        if(!shouldCopyIndexes) {
            log() << "ERROR copy collection shouldCopyIndexes not implemented? " << ns << endl;
        }

        // indexes
        copyIndexes(txn, dbName,
                    NamespaceString(ns), NamespaceString(ns),
                    logForRepl, false, true, mayYield,
                    mayBeInterrupted);

        wunit.commit();
        txn->recoveryUnit()->commitIfNeeded();
        return true;
    }

    extern bool inDBRepair;

    bool Cloner::go(OperationContext* txn,
                    const std::string& toDBName,
                    const string& masterHost,
                    const CloneOptions& opts,
                    set<string>* clonedColls,
                    string& errmsg,
                    int* errCode) {

        if ( errCode ) {
            *errCode = 0;
        }
        massert( 10289 ,  "useReplAuth is not written to replication log", !opts.useReplAuth || !opts.logForRepl );

        const ConnectionString cs = ConnectionString::parse(masterHost, errmsg);
        if (!cs.isValid()) {
            if (errCode)
                *errCode = ErrorCodes::FailedToParse;
            return false;
        }

        bool masterSameProcess = false;
        std::vector<HostAndPort> csServers = cs.getServers();
        for (std::vector<HostAndPort>::const_iterator iter = csServers.begin();
             iter != csServers.end(); ++iter) {

#if !defined(_WIN32) && !defined(__sunos__)
            // isSelf() only does the necessary comparisons on os x and linux (SERVER-14165)
            if (!repl::isSelf(*iter))
                continue;
#else
            if (iter->port() != serverGlobalParams.port)
                continue;
            if (iter->host() != "localhost" && iter->host() != "127.0.0.1")
                continue;
#endif
            masterSameProcess = true;
            break;
        }

        if (masterSameProcess) {
            if (opts.fromDB == toDBName) {
                // guard against an "infinite" loop
                /* if you are replicating, the local.sources config may be wrong if you get this */
                errmsg = "can't clone from self (localhost).";
                return false;
            }
        }

        {
            // setup connection
            if (_conn.get()) {
                // nothing to do
            }
            else if ( !masterSameProcess ) {
                auto_ptr<DBClientBase> con( cs.connect( errmsg ));
                if ( !con.get() )
                    return false;
                if (!repl::replAuthenticate(con.get()))
                    return false;
                
                _conn = con;
            }
            else {
                _conn.reset(new DBDirectClient(txn));
            }
        }

        list<BSONObj> toClone;
        if ( clonedColls ) clonedColls->clear();
        {
            /* todo: we can put these releases inside dbclient or a dbclient specialization.
               or just wait until we get rid of global lock anyway.
               */
            Lock::TempRelease tempRelease(txn->lockState());

            list<BSONObj> raw = _conn->getCollectionInfos( opts.fromDB );
            for ( list<BSONObj>::iterator it = raw.begin(); it != raw.end(); ++it ) {
                BSONObj collection = *it;

                LOG(2) << "\t cloner got " << collection << endl;

                BSONElement collectionOptions = collection["options"];
                if ( collectionOptions.isABSONObj() ) {
                    Status parseOptionsStatus = CollectionOptions().parse(collectionOptions.Obj());
                    if ( !parseOptionsStatus.isOK() ) {
                        errmsg = str::stream() << "invalid collection options: " << collection
                                               << ", reason: " << parseOptionsStatus.reason();
                        return false;
                    }
                }

                BSONElement e = collection.getField("name");
                if ( e.eoo() ) {
                    string s = "bad collection object " + collection.toString();
                    massert( 10290 , s.c_str(), false);
                }
                verify( !e.eoo() );
                verify( e.type() == String );

                NamespaceString ns( opts.fromDB, e.valuestr() );

                if( ns.isSystem() ) {
                    /* system.users and s.js is cloned -- but nothing else from system.
                     * system.indexes is handled specially at the end*/
                    if( legalClientSystemNS( ns.ns() , true ) == 0 ) {
                        LOG(2) << "\t\t not cloning because system collection" << endl;
                        continue;
                    }
                }
                if( !ns.isNormal() ) {
                    LOG(2) << "\t\t not cloning because has $ ";
                    continue;
                }

                if( opts.collsToIgnore.find( ns.ns() ) != opts.collsToIgnore.end() ){
                    LOG(2) << "\t\t ignoring collection " << ns;
                    continue;
                }
                else {
                    LOG(2) << "\t\t not ignoring collection " << ns;
                }

                if ( clonedColls ) clonedColls->insert( ns.ns() );
                toClone.push_back( collection.getOwned() );
            }
        }

        if ( opts.syncData ) {
            for ( list<BSONObj>::iterator i=toClone.begin(); i != toClone.end(); i++ ) {
                BSONObj collection = *i;
                LOG(2) << "  really will clone: " << collection << endl;
                const char* collectionName = collection["name"].valuestr();
                BSONObj options = collection.getObjectField("options");

                NamespaceString from_name( opts.fromDB, collectionName );
                NamespaceString to_name( toDBName, collectionName );

                // Copy releases the lock, so we need to re-load the database. This should probably
                // throw if the database has changed in between, but for now preserve the existing
                // behaviour.
                bool unused;
                Database* db = dbHolder().getOrCreate(txn, toDBName, unused);

                /* we defer building id index for performance - building it in batch is much faster */
                Status createStatus = userCreateNS( txn, db, to_name.ns(), options,
                                                    opts.logForRepl, false );
                if ( !createStatus.isOK() ) {
                    errmsg = str::stream() << "failed to create collection \""
                                           << to_name.ns() << "\": "
                                           << createStatus.reason();
                    return false;
                }

                LOG(1) << "\t\t cloning " << from_name << " -> " << to_name << endl;
                Query q;
                if( opts.snapshot )
                    q.snapshot();

                copy(txn,
                     toDBName,
                     from_name,
                     to_name,
                     opts.logForRepl,
                     masterSameProcess,
                     opts.slaveOk,
                     opts.mayYield,
                     opts.mayBeInterrupted,
                     q);

                {
                    /* we need dropDups to be true as we didn't do a true snapshot and this is before applying oplog operations
                       that occur during the initial sync.  inDBRepair makes dropDups be true.
                    */
                    bool old = inDBRepair;
                    try {
                        inDBRepair = true;
                        Collection* c = db->getCollection( txn, to_name );
                        if ( c )
                            c->getIndexCatalog()->ensureHaveIdIndex(txn);
                        inDBRepair = old;
                    }
                    catch(...) {
                        inDBRepair = old;
                        throw;
                    }
                }
            }
        }

        // now build the indexes
        if ( opts.syncIndexes ) {
            for ( list<BSONObj>::iterator i=toClone.begin(); i != toClone.end(); i++ ) {
                BSONObj collection = *i;
                log() << "copying indexes for: " << collection;

                const char* collectionName = collection["name"].valuestr();

                NamespaceString from_name( opts.fromDB, collectionName );
                NamespaceString to_name( toDBName, collectionName );

                copyIndexes(txn,
                            toDBName,
                            from_name,
                            to_name,
                            opts.logForRepl,
                            masterSameProcess,
                            opts.slaveOk,
                            opts.mayYield,
                            opts.mayBeInterrupted );
            }
        }

        return true;
    }

} // namespace mongo
