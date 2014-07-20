/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/base/status.h"
#include "mongo/db/repl/repl_coordinator.h"

namespace mongo {
namespace repl {

    /**
     * A mock ReplicationCoordinator.  Currently it is extremely simple and exists solely to link
     * into dbtests.
     */
    class ReplicationCoordinatorMock : public ReplicationCoordinator {
        MONGO_DISALLOW_COPYING(ReplicationCoordinatorMock);

    public:

        ReplicationCoordinatorMock(const ReplSettings& settings);
        virtual ~ReplicationCoordinatorMock();

        virtual void startReplication(TopologyCoordinator* topCoord,
                                      ReplicationExecutor::NetworkInterface* network);

        virtual void shutdown();

        virtual bool isShutdownOkay() const;

        virtual ReplSettings& getSettings();

        virtual bool isReplEnabled() const;

        virtual Mode getReplicationMode() const;

        virtual MemberState getCurrentMemberState() const;

        virtual ReplicationCoordinator::StatusAndDuration awaitReplication(
                const OperationContext* txn,
                const OpTime& ts,
                const WriteConcernOptions& writeConcern);

        virtual ReplicationCoordinator::StatusAndDuration awaitReplicationOfLastOp(
                const OperationContext* txn,
                const WriteConcernOptions& writeConcern);

        virtual Status stepDown(bool force,
                                const Milliseconds& waitTime,
                                const Milliseconds& stepdownTime);

        virtual Status stepDownAndWaitForSecondary(const Milliseconds& initialWaitTime,
                                                   const Milliseconds& stepdownTime,
                                                   const Milliseconds& postStepdownWaitTime);

        virtual bool isMasterForReportingPurposes();

        virtual bool canAcceptWritesForDatabase(const StringData& dbName);

        virtual Status checkIfWriteConcernCanBeSatisfied(
                const WriteConcernOptions& writeConcern) const;

        virtual Status canServeReadsFor(const NamespaceString& ns, bool slaveOk);

        virtual bool shouldIgnoreUniqueIndex(const IndexDescriptor* idx);

        virtual Status setLastOptime(const OID& rid, const OpTime& ts);

        virtual OID getElectionId();

        virtual void processReplSetGetStatus(BSONObjBuilder* result);

        virtual bool setMaintenanceMode(bool activate);

        virtual Status processReplSetMaintenance(bool activate, BSONObjBuilder* resultObj);

        virtual Status processReplSetSyncFrom(const std::string& target,
                                              BSONObjBuilder* resultObj);

        virtual Status processReplSetFreeze(int secs, BSONObjBuilder* resultObj);

        virtual Status processHeartbeat(const BSONObj& cmdObj, 
                                        BSONObjBuilder* resultObj);

        virtual Status processReplSetReconfig(OperationContext* txn,
                                              const ReplSetReconfigArgs& args,
                                              BSONObjBuilder* resultObj);

        virtual Status processReplSetInitiate(OperationContext* txn,
                                              const BSONObj& configObj,
                                              BSONObjBuilder* resultObj);

        virtual Status processReplSetGetRBID(BSONObjBuilder* resultObj);

        virtual void incrementRollbackID();

        virtual Status processReplSetFresh(const ReplSetFreshArgs& args,
                                           BSONObjBuilder* resultObj);

        virtual Status processReplSetElect(const ReplSetElectArgs& args,
                                           BSONObjBuilder* resultObj);

        virtual Status processReplSetUpdatePosition(const BSONArray& updates,
                                                    BSONObjBuilder* resultObj);

        virtual Status processReplSetUpdatePositionHandshake(const BSONObj& handshake,
                                                             BSONObjBuilder* resultObj);

        virtual bool processHandshake(const OID& remoteID, const BSONObj& handshake);

        virtual void waitUpToOneSecondForOptimeChange(const OpTime& ot);

        virtual bool buildsIndexes();

        virtual std::vector<BSONObj> getHostsWrittenTo(const OpTime& op);

    private:

        ReplSettings _settings;
    };

} // namespace repl
} // namespace mongo
