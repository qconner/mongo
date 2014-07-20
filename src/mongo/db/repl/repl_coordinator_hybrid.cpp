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

#include "mongo/platform/basic.h"

#include "mongo/db/repl/repl_coordinator_hybrid.h"

namespace mongo {

    MONGO_LOG_DEFAULT_COMPONENT_FILE(::mongo::logger::LogComponent::kReplication);

namespace repl {

    HybridReplicationCoordinator::HybridReplicationCoordinator(const ReplSettings& settings) :
            _legacy(settings), _impl(settings) {}
    HybridReplicationCoordinator::~HybridReplicationCoordinator() {}

    void HybridReplicationCoordinator::startReplication(
            TopologyCoordinator* topCoord,
            ReplicationExecutor::NetworkInterface* network) {
        _legacy.startReplication(topCoord, network);
        _impl.startReplication(topCoord, network);
    }

    void HybridReplicationCoordinator::shutdown() {
        _legacy.shutdown();
        _impl.shutdown();
    }

    bool HybridReplicationCoordinator::isShutdownOkay() const {
        bool legacyResponse = _legacy.isShutdownOkay();
        return legacyResponse;
    }

    ReplSettings& HybridReplicationCoordinator::getSettings() {
        ReplSettings& legacySettings = _legacy.getSettings();
        return legacySettings;
    }

    ReplicationCoordinator::Mode HybridReplicationCoordinator::getReplicationMode() const {
        Mode legacyMode = _legacy.getReplicationMode();
        return legacyMode;
    }

    MemberState HybridReplicationCoordinator::getCurrentMemberState() const {
        MemberState legacyState = _legacy.getCurrentMemberState();
        return legacyState;
    }

    ReplicationCoordinator::StatusAndDuration HybridReplicationCoordinator::awaitReplication(
            const OperationContext* txn,
            const OpTime& ts,
            const WriteConcernOptions& writeConcern) {
        StatusAndDuration legacyStatus = _legacy.awaitReplication(txn, ts, writeConcern);
        return legacyStatus;
    }

    ReplicationCoordinator::StatusAndDuration 
            HybridReplicationCoordinator::awaitReplicationOfLastOp(
                    const OperationContext* txn,
                    const WriteConcernOptions& writeConcern) {
        StatusAndDuration legacyStatus = _legacy.awaitReplicationOfLastOp(txn, writeConcern);
        return legacyStatus;
    }

    Status HybridReplicationCoordinator::stepDown(bool force,
                                                  const Milliseconds& waitTime,
                                                  const Milliseconds& stepdownTime) {
        Status legacyStatus = _legacy.stepDown(force, waitTime, stepdownTime);
        Status implStatus = _impl.stepDown(force, waitTime, stepdownTime);
        return legacyStatus;
    }

    Status HybridReplicationCoordinator::stepDownAndWaitForSecondary(
            const Milliseconds& initialWaitTime,
            const Milliseconds& stepdownTime,
            const Milliseconds& postStepdownWaitTime) {
        Status legacyStatus = _legacy.stepDownAndWaitForSecondary(initialWaitTime,
                                                                  stepdownTime,
                                                                  postStepdownWaitTime);
        Status implStatus = _impl.stepDownAndWaitForSecondary(initialWaitTime,
                                                              stepdownTime,
                                                              postStepdownWaitTime);
        return legacyStatus;
    }

    bool HybridReplicationCoordinator::isMasterForReportingPurposes() {
        bool legacyResponse = _legacy.isMasterForReportingPurposes();
        _impl.isMasterForReportingPurposes();
        return legacyResponse;
    }

    bool HybridReplicationCoordinator::canAcceptWritesForDatabase(const StringData& dbName) {
        bool legacyResponse = _legacy.canAcceptWritesForDatabase(dbName);
        _impl.canAcceptWritesForDatabase(dbName);
        return legacyResponse;
    }

    Status HybridReplicationCoordinator::canServeReadsFor(const NamespaceString& ns, bool slaveOk) {
        Status legacyStatus = _legacy.canServeReadsFor(ns, slaveOk);
        Status implStatus = _impl.canServeReadsFor(ns, slaveOk);
        return legacyStatus;
    }

    bool HybridReplicationCoordinator::shouldIgnoreUniqueIndex(const IndexDescriptor* idx) {
        bool legacyResponse = _legacy.shouldIgnoreUniqueIndex(idx);
        _impl.shouldIgnoreUniqueIndex(idx);
        return legacyResponse;
    }

    Status HybridReplicationCoordinator::setLastOptime(const OID& rid,
                                                       const OpTime& ts) {
        Status legacyStatus = _legacy.setLastOptime(rid, ts);
        Status implStatus = _impl.setLastOptime(rid, ts);
        return legacyStatus;
    }
    
    OID HybridReplicationCoordinator::getElectionId() {
        OID legacyOID = _legacy.getElectionId();
        _impl.getElectionId();
        return legacyOID;
    }

    OID HybridReplicationCoordinator::getMyRID() {
        OID legacyRID = _legacy.getMyRID();
        _impl.getMyRID();
        return legacyRID;
    }

    void HybridReplicationCoordinator::prepareReplSetUpdatePositionCommand(BSONObjBuilder* result) {
        _legacy.prepareReplSetUpdatePositionCommand(result);
        BSONObjBuilder implResult;
        _impl.prepareReplSetUpdatePositionCommand(&implResult);
    }

    void HybridReplicationCoordinator::processReplSetGetStatus(BSONObjBuilder* result) {
        _legacy.processReplSetGetStatus(result);
        BSONObjBuilder implResult;
        _impl.processReplSetGetStatus(&implResult);
    }

    bool HybridReplicationCoordinator::setMaintenanceMode(bool activate) {
        bool legacyResponse = _legacy.setMaintenanceMode(activate);
        _impl.setMaintenanceMode(activate);
        return legacyResponse;
    }

    Status HybridReplicationCoordinator::processHeartbeat(const BSONObj& cmdObj, 
                                                          BSONObjBuilder* resultObj) {
        Status legacyStatus = _legacy.processHeartbeat(cmdObj, resultObj);
        BSONObjBuilder implResult;
        return legacyStatus;
    }

    Status HybridReplicationCoordinator::processReplSetReconfig(OperationContext* txn,
                                                                const ReplSetReconfigArgs& args,
                                                                BSONObjBuilder* resultObj) {
        Status legacyStatus = _legacy.processReplSetReconfig(txn, args, resultObj);
        BSONObjBuilder implResult;
        Status implStatus = _impl.processReplSetReconfig(txn, args, &implResult);
        return legacyStatus;
    }

    Status HybridReplicationCoordinator::processReplSetInitiate(OperationContext* txn,
                                                                const BSONObj& givenConfig,
                                                                BSONObjBuilder* resultObj) {
        Status legacyStatus = _legacy.processReplSetInitiate(txn, givenConfig, resultObj);
        BSONObjBuilder implResult;
        Status implStatus = _impl.processReplSetInitiate(txn, givenConfig, &implResult);
        return legacyStatus;
    }

    Status HybridReplicationCoordinator::processReplSetGetRBID(BSONObjBuilder* resultObj) {
        Status legacyStatus = _legacy.processReplSetGetRBID(resultObj);
        BSONObjBuilder implResult;
        Status implStatus = _impl.processReplSetGetRBID(&implResult);
        return legacyStatus;
    }

    Status HybridReplicationCoordinator::processReplSetFresh(const ReplSetFreshArgs& args,
                                                             BSONObjBuilder* resultObj) {
        Status legacyStatus = _legacy.processReplSetFresh(args, resultObj);
        BSONObjBuilder implResult;
        Status implStatus = _impl.processReplSetFresh(args, &implResult);
        return legacyStatus;
    }

    Status HybridReplicationCoordinator::processReplSetElect(const ReplSetElectArgs& args,
                                                             BSONObjBuilder* resultObj) {
        Status legacyStatus = _legacy.processReplSetElect(args, resultObj);
        BSONObjBuilder implResult;
        Status implStatus = _impl.processReplSetElect(args, &implResult);
        return legacyStatus;
    }

    void HybridReplicationCoordinator::incrementRollbackID() {
        _legacy.incrementRollbackID();
        _impl.incrementRollbackID();
    }

    Status HybridReplicationCoordinator::processReplSetFreeze(int secs, BSONObjBuilder* resultObj) {
        Status legacyStatus = _legacy.processReplSetFreeze(secs, resultObj);
        BSONObjBuilder implResult;
        Status implStatus = _impl.processReplSetFreeze(secs, &implResult);
        return legacyStatus;
    }

    Status HybridReplicationCoordinator::processReplSetMaintenance(bool activate,
                                                                   BSONObjBuilder* resultObj) {
        Status legacyStatus = _legacy.processReplSetMaintenance(activate, resultObj);
        BSONObjBuilder implResult;
        Status implStatus = _impl.processReplSetMaintenance(activate, &implResult);
        return legacyStatus;
    }

    Status HybridReplicationCoordinator::processReplSetSyncFrom(const std::string& target,
                                                                BSONObjBuilder* resultObj) {
        Status legacyStatus = _legacy.processReplSetSyncFrom(target, resultObj);
        BSONObjBuilder implResult;
        Status implStatus = _impl.processReplSetSyncFrom(target, &implResult);
        return legacyStatus;
    }

    Status HybridReplicationCoordinator::processReplSetUpdatePosition(const BSONArray& updates,
                                                                      BSONObjBuilder* resultObj) {
        Status legacyStatus = _legacy.processReplSetUpdatePosition(updates, resultObj);
        BSONObjBuilder implResult;
        Status implStatus = _impl.processReplSetUpdatePosition(updates, &implResult);
        return legacyStatus;
    }

    Status HybridReplicationCoordinator::processReplSetUpdatePositionHandshake(
            const BSONObj& handshake,
            BSONObjBuilder* resultObj) {
        Status legacyStatus = _legacy.processReplSetUpdatePositionHandshake(handshake, resultObj);
        BSONObjBuilder implResult;
        Status implStatus = _impl.processReplSetUpdatePositionHandshake(handshake, &implResult);
        return legacyStatus;
    }

    bool HybridReplicationCoordinator::processHandshake(const OID& remoteID,
                                                        const BSONObj& handshake) {
        bool legacyResponse = _legacy.processHandshake(remoteID, handshake);
        _impl.processHandshake(remoteID, handshake);
        return legacyResponse;
    }

    void HybridReplicationCoordinator::waitUpToOneSecondForOptimeChange(const OpTime& ot) {
        _legacy.waitUpToOneSecondForOptimeChange(ot);
        //TODO(spencer) switch to _impl.waitUpToOneSecondForOptimeChange(ot); once implemented
    }

    bool HybridReplicationCoordinator::buildsIndexes() {
        bool legacyResponse = _legacy.buildsIndexes();
        _impl.buildsIndexes();
        return legacyResponse;
    }

    vector<BSONObj> HybridReplicationCoordinator::getHostsWrittenTo(const OpTime& op) {
        vector<BSONObj> legacyResponse = _legacy.getHostsWrittenTo(op);
        vector<BSONObj> implResponse = _impl.getHostsWrittenTo(op);
        return legacyResponse;
    }

    Status HybridReplicationCoordinator::checkIfWriteConcernCanBeSatisfied(
            const WriteConcernOptions& writeConcern) const {
        Status legacyStatus = _legacy.checkIfWriteConcernCanBeSatisfied(writeConcern);
        Status implStatus = _impl.checkIfWriteConcernCanBeSatisfied(writeConcern);
        return legacyStatus;
    }

} // namespace repl
} // namespace mongo
