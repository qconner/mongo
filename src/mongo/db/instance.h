// instance.h : Global state functions.
//

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

#pragma once

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage_options.h"

namespace mongo {

    extern std::string dbExecCommand;

    /** a high level recording of operations to the database - sometimes used for diagnostics 
        and debugging.
        */
    class DiagLog {
        std::ofstream *f; // note this is never freed
        /* 0 = off; 1 = writes, 2 = reads, 3 = both
           7 = log a few reads, and all writes.
        */
        int level;
        mongo::mutex mutex;
        void openFile();

    public:
        DiagLog();
        int getLevel() const { return level; }
        /**
         * @return old
         */
        int setLevel( int newLevel );
        void flush();
        void writeop(char *data,int len);
        void readop(char *data, int len);
    };

    extern DiagLog _diaglog;

    void assembleResponse( OperationContext* txn,
                           Message& m,
                           DbResponse& dbresponse,
                           const HostAndPort &client );

    /**
     * Embedded calls to the local server using the DBClientBase API without going over the network.
     *
     * Caller does not need to lock, that is handled within.
     *
     * All operations are performed within the scope of a passed-in OperationContext (except when
     * using the deprecated constructor). You must ensure that the OperationContext is valid when
     * calling into any function. If you ever need to change the OperationContext, that can be done
     * without the overhead of creating a new DBDirectClient by calling setOpCtx(), after which all
     * operations will use the new OperationContext.
     */
    class DBDirectClient : public DBClientBase {
    public:
        DBDirectClient(); // DEPRECATED
        DBDirectClient(OperationContext* txn);

        void setOpCtx(OperationContext* txn) { _txn = txn; };

        using DBClientBase::query;

        virtual std::auto_ptr<DBClientCursor> query(const std::string &ns, Query query, int nToReturn = 0, int nToSkip = 0,
                                               const BSONObj *fieldsToReturn = 0, int queryOptions = 0, int batchSize = 0);

        virtual bool isFailed() const {
            return false;
        }

        virtual bool isStillConnected() {
            return true;
        }

        virtual std::string toString() const {
            return "DBDirectClient";
        }
        virtual std::string getServerAddress() const {
            return "localhost"; // TODO: should this have the port?
        }
        virtual bool call( Message &toSend, Message &response, bool assertOk=true , std::string * actualServer = 0 );
        virtual void say( Message &toSend, bool isRetry = false , std::string * actualServer = 0 );
        virtual void sayPiggyBack( Message &toSend ) {
            // don't need to piggy back when connected locally
            return say( toSend );
        }

        virtual void killCursor( long long cursorID );

        virtual bool callRead( Message& toSend , Message& response ) {
            return call( toSend , response );
        }
        
        virtual unsigned long long count(const std::string &ns, const BSONObj& query = BSONObj(), int options=0, int limit=0, int skip=0 );
        
        virtual ConnectionString::ConnectionType type() const { return ConnectionString::MASTER; }

        double getSoTimeout() const { return 0; }

        virtual bool lazySupported() const { return true; }

        virtual QueryOptions _lookupAvailableOptions();

    private:
        static HostAndPort _clientHost;
        boost::scoped_ptr<OperationContext> _txnOwned;
        OperationContext* _txn; // Points either to _txnOwned or a passed-in transaction.
    };

    void maybeCreatePidFile();

    void exitCleanly( ExitCode code );

} // namespace mongo
