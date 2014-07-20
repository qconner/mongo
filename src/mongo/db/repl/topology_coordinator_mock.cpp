/**
 *    Copyright 2014 MongoDB Inc.
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

#include "mongo/db/repl/topology_coordinator_mock.h"

namespace mongo {
namespace repl {

    TopologyCoordinatorMock::TopologyCoordinatorMock() {}

    void TopologyCoordinatorMock::setLastApplied(const OpTime& optime) {}

    void TopologyCoordinatorMock::setCommitOkayThrough(const OpTime& optime) {}

    void TopologyCoordinatorMock::setLastReceived(const OpTime& optime) {}

    HostAndPort TopologyCoordinatorMock::getSyncSourceAddress() const {
        return HostAndPort();
    }

    void TopologyCoordinatorMock::chooseNewSyncSource(Date_t now) {}

    void TopologyCoordinatorMock::blacklistSyncSource(const HostAndPort& host, Date_t until) {}

    void TopologyCoordinatorMock::registerConfigChangeCallback(const ConfigChangeCallbackFn& fn) {}

    void TopologyCoordinatorMock::registerStateChangeCallback(const StateChangeCallbackFn& fn) {}

    void TopologyCoordinatorMock::signalDrainComplete()  {}

    void TopologyCoordinatorMock::relinquishPrimary(OperationContext* txn) {}

    void TopologyCoordinatorMock::prepareRequestVoteResponse(const Date_t now,
                                                             const BSONObj& cmdObj,
                                                             std::string& errmsg,
                                                             BSONObjBuilder& result) {}

    void TopologyCoordinatorMock::prepareElectCmdResponse(const Date_t now,
                                                          const BSONObj& cmdObj,
                                                          BSONObjBuilder& result) {}

    void TopologyCoordinatorMock::prepareHeartbeatResponse(const ReplicationExecutor::CallbackData&,
                                                           Date_t now,
                                                           const BSONObj& cmdObj, 
                                                           BSONObjBuilder* resultObj,
                                                           Status* result) {
    }

    void TopologyCoordinatorMock::prepareStatusResponse(Date_t now,
                                                        const BSONObj& cmdObj,
                                                        BSONObjBuilder& result,
                                                        unsigned uptime) {
    }

    void TopologyCoordinatorMock::prepareFreezeResponse(Date_t now,
                                                        const BSONObj& cmdObj,
                                                        BSONObjBuilder& result) {
    }

    HeartbeatResultAction TopologyCoordinatorMock::updateHeartbeatInfo(Date_t now,
                                                                   const HeartbeatInfo& newInfo) {
        return None;
    }

    void TopologyCoordinatorMock::updateConfig(const ReplicaSetConfig newConfig, const int selfId) {
    }

} // namespace repl
} // namespace mongo
