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

#include "mongo/db/repl/repl_coordinator_legacy.h"

#include <boost/thread/thread.hpp>

#include "mongo/base/status.h"
#include "mongo/bson/optime.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/instance.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/connections.h"
#include "mongo/db/repl/master_slave.h"
#include "mongo/db/repl/member.h"
#include "mongo/db/repl/oplog.h" // for newRepl()
#include "mongo/db/repl/repl_set_heartbeat_args.h"
#include "mongo/db/repl/repl_set_heartbeat_response.h"
#include "mongo/db/repl/repl_set_seed_list.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replset_commands.h"
#include "mongo/db/repl/rs.h"
#include "mongo/db/repl/rs_initiate.h"
#include "mongo/db/repl/write_concern.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/map_util.h"

namespace mongo {

    MONGO_LOG_DEFAULT_COMPONENT_FILE(::mongo::logger::LogComponent::kReplication);

namespace repl {

    LegacyReplicationCoordinator::LegacyReplicationCoordinator(const ReplSettings& settings) :
            _settings(settings) {
        // this is ok but micros or combo with some rand() and/or 64 bits might be better --
        // imagine a restart and a clock correction simultaneously (very unlikely but possible...)
        _rbid = (int) curTimeMillis64();
    }
    LegacyReplicationCoordinator::~LegacyReplicationCoordinator() {}

    void LegacyReplicationCoordinator::startReplication(TopologyCoordinator*,
                                                        ReplicationExecutor::NetworkInterface*) {
        // if we are going to be a replica set, we aren't doing other forms of replication.
        if (!_settings.replSet.empty()) {
            if (_settings.slave || _settings.master) {
                log() << "***" << endl;
                log() << "ERROR: can't use --slave or --master replication options with --replSet";
                log() << "***" << endl;
            }
            newRepl();

            ReplSetSeedList *replSetSeedList = new ReplSetSeedList(_settings.replSet);
            boost::thread t(stdx::bind(&startReplSets, replSetSeedList));
        } else {
            startMasterSlave();
        }
    }

    void LegacyReplicationCoordinator::shutdown() {
        if (getReplicationMode() == modeReplSet) {
            theReplSet->shutdown();
        }
    }

    ReplSettings& LegacyReplicationCoordinator::getSettings() {
        return _settings;
    }

    ReplicationCoordinator::Mode LegacyReplicationCoordinator::getReplicationMode() const {
        if (theReplSet) {
            return modeReplSet;
        } else if (_settings.slave || _settings.master) {
            return modeMasterSlave;
        }
        return modeNone;
    }

    MemberState LegacyReplicationCoordinator::getCurrentMemberState() const {
        invariant(getReplicationMode() == modeReplSet);
        return theReplSet->state();
    }

    ReplicationCoordinator::StatusAndDuration LegacyReplicationCoordinator::awaitReplication(
            const OperationContext* txn,
            const OpTime& ts,
            const WriteConcernOptions& writeConcern) {

        Timer timeoutTimer;

        if (writeConcern.wNumNodes <= 1 && writeConcern.wMode.empty()) {
            // no desired replication check
            return StatusAndDuration(Status::OK(), Milliseconds(timeoutTimer.millis()));
        }

        const Mode replMode = getReplicationMode();
        if (replMode == modeNone || serverGlobalParams.configsvr) {
            // no replication check needed (validated above)
            return StatusAndDuration(Status::OK(), Milliseconds(timeoutTimer.millis()));
        }

        if (writeConcern.wMode == "majority" && replMode == modeMasterSlave) {
            // with master/slave, majority is equivalent to w=1
            return StatusAndDuration(Status::OK(), Milliseconds(timeoutTimer.millis()));
        }

        try {
            while (1) {
                if (!writeConcern.wMode.empty()) {
                    if (opReplicatedEnough(ts, writeConcern.wMode)) {
                        return StatusAndDuration(Status::OK(), Milliseconds(timeoutTimer.millis()));
                    }
                }
                else if (opReplicatedEnough(ts, writeConcern.wNumNodes)) {
                    return StatusAndDuration(Status::OK(), Milliseconds(timeoutTimer.millis()));
                }

                if (writeConcern.wTimeout > 0 && timeoutTimer.millis() >= writeConcern.wTimeout) {
                    return StatusAndDuration(Status(ErrorCodes::ExceededTimeLimit,
                                                     "waiting for replication timed out"),
                                              Milliseconds(timeoutTimer.millis()));
                }

                if (writeConcern.wTimeout == -1) {
                    return StatusAndDuration(Status(ErrorCodes::ExceededTimeLimit,
                                                     "replication not finished when checked"),
                                              Milliseconds(timeoutTimer.millis()));
                }

                // TODO (dannenberg) is this the best sleep amount?
                sleepmillis(1);
                txn->checkForInterrupt();
            }

        }
        catch (const DBException& ex) {
            return StatusAndDuration(ex.toStatus(), Milliseconds(timeoutTimer.millis()));
        }
    }

    ReplicationCoordinator::StatusAndDuration 
            LegacyReplicationCoordinator::awaitReplicationOfLastOp(
                    const OperationContext* txn,
                    const WriteConcernOptions& writeConcern) {
        return awaitReplication(txn, cc().getLastOp(), writeConcern);
    }

    Status LegacyReplicationCoordinator::stepDown(OperationContext* txn, 
                                                  bool force,
                                                  const Milliseconds& waitTime,
                                                  const Milliseconds& stepdownTime) {
        return _stepDownHelper(txn, force, waitTime, stepdownTime, Milliseconds(0));
    }

    Status LegacyReplicationCoordinator::stepDownAndWaitForSecondary(
            OperationContext* txn,
            const Milliseconds& initialWaitTime,
            const Milliseconds& stepdownTime,
            const Milliseconds& postStepdownWaitTime) {
        return _stepDownHelper(txn, false, initialWaitTime, stepdownTime, postStepdownWaitTime);
    }

namespace {
    /**
     * Waits up to timeout milliseconds for one secondary to get within threshold milliseconds
     * of us.
     */
    Status _waitForSecondary(const ReplicationCoordinator::Milliseconds& timeout,
                             const ReplicationCoordinator::Milliseconds& threshold) {
        if (theReplSet->getConfig().members.size() <= 1) {
            return Status(ErrorCodes::ExceededTimeLimit,
                          mongoutils::str::stream() << "no secondaries within " <<
                                  threshold.total_seconds() << " seconds of my optime");
        }

        long long timeoutTime, now, start;
        timeoutTime = now = start = curTimeMillis64()/1000;
        timeoutTime += timeout.total_seconds();

        OpTime lastOp = repl::theReplSet->lastOpTimeWritten;
        OpTime closest = repl::theReplSet->lastOtherElectableOpTime();
        long long int diff = lastOp.getSecs() - closest.getSecs();
        while (now <= timeoutTime && (diff < 0 || diff > threshold.total_seconds())) {
            sleepsecs(1);
            now = curTimeMillis64() / 1000;

            lastOp = repl::theReplSet->lastOpTimeWritten;
            closest = repl::theReplSet->lastOtherElectableOpTime();
            diff = lastOp.getSecs() - closest.getSecs();
        }

        if (diff < 0) {
            // not our problem but we'll wait until things settle down
            return Status(ErrorCodes::SecondaryAheadOfPrimary,
                          "someone is ahead of the primary?");
        }
        if (diff > threshold.total_seconds()) {
            return Status(ErrorCodes::ExceededTimeLimit,
                          mongoutils::str::stream() << "no secondaries within " <<
                                  threshold.total_seconds() << " seconds of my optime");
        }
        return Status::OK();
    }
} // namespace

    Status LegacyReplicationCoordinator::_stepDownHelper(OperationContext* txn,
                                                         bool force,
                                                         const Milliseconds& initialWaitTime,
                                                         const Milliseconds& stepdownTime,
                                                         const Milliseconds& postStepdownWaitTime) {
        invariant(getReplicationMode() == modeReplSet);
        if (!getCurrentMemberState().primary()) {
            return Status(ErrorCodes::NotMaster, "not primary so can't step down");
        }

        if (!force) {
            Status status = _waitForSecondary(initialWaitTime, Milliseconds(10 * 1000));
            if (!status.isOK()) {
                return status;
            }
        }

        // step down
        bool worked = repl::theReplSet->stepDown(txn, stepdownTime.total_seconds());
        if (!worked) {
            return Status(ErrorCodes::NotMaster, "not primary so can't step down");
        }

        if (postStepdownWaitTime.total_milliseconds() > 0) {
            log() << "waiting for secondaries to catch up" << endl;

            // The only caller of this with a non-zero postStepdownWaitTime is
            // stepDownAndWaitForSecondary, and the only caller of that is the shutdown command
            // which doesn't actually care if secondaries failed to catch up here, so we ignore the
            // return status of _waitForSecondary
            _waitForSecondary(postStepdownWaitTime, Milliseconds(0));
        }
        return Status::OK();
    }

    bool LegacyReplicationCoordinator::isMasterForReportingPurposes() {
        // we must check replSet since because getReplicationMode() isn't aware of modeReplSet
        // until theReplSet is initialized
        if (_settings.usingReplSets()) {
            if (theReplSet && getCurrentMemberState().primary()) {
                return true;
            }
            return false;
        }

        if (!_settings.slave)
            return true;

        if (replAllDead) {
            return false;
        }

        if (_settings.master) {
            // if running with --master --slave, allow.
            return true;
        }

        return false;
    }

    bool LegacyReplicationCoordinator::canAcceptWritesForDatabase(const StringData& dbName) {
        // we must check replSet since because getReplicationMode() isn't aware of modeReplSet
        // until theReplSet is initialized
        if (_settings.usingReplSets()) {
            if (theReplSet && getCurrentMemberState().primary()) {
                return true;
            }
            return dbName == "local";
        }

        if (!_settings.slave)
            return true;

        // TODO(dannenberg) replAllDead is bad and should be removed when master slave is removed
        if (replAllDead) {
            return dbName == "local";
        }

        if (_settings.master) {
            // if running with --master --slave, allow.
            return true;
        }

        return dbName == "local";
    }

    Status LegacyReplicationCoordinator::canServeReadsFor(OperationContext* txn,
                                                          const NamespaceString& ns,
                                                          bool slaveOk) {
        if (txn->getClient()->isGod()) {
            return Status::OK();
        }
        if (canAcceptWritesForDatabase(ns.db())) {
            return Status::OK();
        }
        if (getReplicationMode() == modeMasterSlave && _settings.slave == SimpleSlave) {
            return Status::OK();
        }
        if (slaveOk) {
            if (getReplicationMode() == modeMasterSlave || getReplicationMode() == modeNone) {
                return Status::OK();
            }
            if (getCurrentMemberState().secondary()) {
                return Status::OK();
            }
            return Status(ErrorCodes::NotMasterOrSecondaryCode,
                         "not master or secondary; cannot currently read from this replSet member");
        }
        return Status(ErrorCodes::NotMasterNoSlaveOkCode,
                      "not master and slaveOk=false");

    }

    bool LegacyReplicationCoordinator::shouldIgnoreUniqueIndex(const IndexDescriptor* idx) {
        if (!idx->unique()) {
            return false;
        }
        if (!theReplSet) {
            return false;
        }
        // see SERVER-6671
        MemberState ms = theReplSet->state();
        if (! ((ms == MemberState::RS_STARTUP2) ||
               (ms == MemberState::RS_RECOVERING) ||
               (ms == MemberState::RS_ROLLBACK))) {
            return false;
        }
        // 2 is the oldest oplog version where operations
        // are fully idempotent.
        if (theReplSet->oplogVersion < 2) {
            return false;
        }
        // Never ignore _id index
        if (idx->isIdIndex()) {
            return false;
        }

        return true;
    }

    Status LegacyReplicationCoordinator::setLastOptime(OperationContext* txn,
                                                       const OID& rid,
                                                       const OpTime& ts) {
        {
            boost::lock_guard<boost::mutex> lock(_mutex);
            if (ts <= mapFindWithDefault(_slaveOpTimeMap, rid, OpTime())) {
                // Only update if ts is newer than what we have already
                return Status::OK();
            }
            BSONObj config = mapFindWithDefault(_ridConfigMap, rid, BSONObj());
            LOG(2) << "received notification that node with RID " << rid << " and config " << config
                    << " has reached optime: " << ts.toStringPretty();

            if (rid != getMyRID(txn)) {
                // TODO(spencer): Remove this invariant for backwards compatibility
                invariant(!config.isEmpty());
                // This is what updates the progress information used for satisfying write concern
                // and wakes up threads waiting for replication.
                if (!updateSlaveTracking(BSON("_id" << rid), config, ts)) {
                    return Status(ErrorCodes::NodeNotFound,
                                  str::stream() << "could not update node with _id: "
                                          << config["_id"].Int()
                                          << " because it cannot be found in current ReplSetConfig");
                }
            }

            // This updates the _slaveOpTimeMap which is used for forwarding slave progress
            // upstream in chained replication.
            LOG(2) << "Updating our knowledge of the replication progress for node with RID " <<
                    rid << " to be at optime " << ts;
            _slaveOpTimeMap[rid] = ts;
        }

        if (getReplicationMode() == modeReplSet && !getCurrentMemberState().primary()) {
            // pass along if we are not primary
            theReplSet->syncSourceFeedback.forwardSlaveProgress();
        }
        return Status::OK();
    }
    
    OID LegacyReplicationCoordinator::getElectionId() {
        return theReplSet->getElectionId();
    }


    OID LegacyReplicationCoordinator::getMyRID(OperationContext* txn) {
        Mode mode = getReplicationMode();
        if (mode == modeReplSet) {
            return theReplSet->syncSourceFeedback.getMyRID();
        } else if (mode == modeMasterSlave) {
            ReplSource source(txn);
            return source.getMyRID();
        }
        invariant(false); // Don't have an RID if no replication is enabled
    }

    void LegacyReplicationCoordinator::prepareReplSetUpdatePositionCommand(
            OperationContext* txn,
            BSONObjBuilder* cmdBuilder) {
        invariant(getReplicationMode() == modeReplSet);
        boost::lock_guard<boost::mutex> lock(_mutex);
        cmdBuilder->append("replSetUpdatePosition", 1);
        // create an array containing objects each member connected to us and for ourself
        BSONArrayBuilder arrayBuilder(cmdBuilder->subarrayStart("optimes"));
        OID myID = getMyRID(txn);
        {
            for (SlaveOpTimeMap::const_iterator itr = _slaveOpTimeMap.begin();
                    itr != _slaveOpTimeMap.end(); ++itr) {
                const OID& rid = itr->first;
                const BSONObj& config = mapFindWithDefault(_ridConfigMap, rid, BSONObj());
                BSONObjBuilder entry(arrayBuilder.subobjStart());
                entry.append("_id", rid);
                entry.append("optime", itr->second);
                // SERVER-14550 Even though the "config" field isn't used on the other end in 2.8,
                // we need to keep sending it for 2.6 compatibility.
                // TODO(spencer): Remove this after 2.8 is released.
                if (rid == myID) {
                    entry.append("config", theReplSet->myConfig().asBson());
                }
                else {
                    entry.append("config", config);
                }
            }
        }
    }

    void LegacyReplicationCoordinator::prepareReplSetUpdatePositionCommandHandshakes(
            OperationContext* txn,
            std::vector<BSONObj>* handshakes) {
        invariant(getReplicationMode() == modeReplSet);
        boost::lock_guard<boost::mutex> lock(_mutex);
        // handshake obj for us
        BSONObjBuilder cmd;
        cmd.append("replSetUpdatePosition", 1);
        BSONObjBuilder sub (cmd.subobjStart("handshake"));
        sub.append("handshake", getMyRID(txn));
        sub.append("member", theReplSet->selfId());
        sub.append("config", theReplSet->myConfig().asBson());
        sub.doneFast();
        handshakes->push_back(cmd.obj());

        // handshake objs for all chained members
        for (OIDMemberMap::const_iterator itr = _ridMemberMap.begin();
             itr != _ridMemberMap.end(); ++itr) {
            BSONObjBuilder cmd;
            cmd.append("replSetUpdatePosition", 1);
            // outer handshake indicates this is a handshake command
            // inner is needed as part of the structure to be passed to gotHandshake
            BSONObjBuilder subCmd (cmd.subobjStart("handshake"));
            subCmd.append("handshake", itr->first);
            subCmd.append("member", itr->second->id());
            subCmd.append("config", itr->second->config().asBson());
            subCmd.doneFast();
            handshakes->push_back(cmd.obj());
        }
    }

    Status LegacyReplicationCoordinator::processReplSetGetStatus(BSONObjBuilder* result) {
        theReplSet->summarizeStatus(*result);
        return Status::OK();
    }

    void LegacyReplicationCoordinator::processReplSetGetConfig(BSONObjBuilder* result) {
        result->append("config", theReplSet->config().asBson());
    }

    bool LegacyReplicationCoordinator::setMaintenanceMode(OperationContext* txn, bool activate) {
        return theReplSet->setMaintenanceMode(txn, activate);
    }

    Status LegacyReplicationCoordinator::processHeartbeat(const ReplSetHeartbeatArgs& args,
                                                          ReplSetHeartbeatResponse* response) {
        if (args.getProtocolVersion() != 1) {
            return Status(ErrorCodes::BadValue, "incompatible replset protocol version");
        }

        {
            if (_settings.ourSetName() != args.getSetName()) {
                log() << "replSet set names do not match, our cmdline: " << _settings.replSet
                      << rsLog;
                log() << "replSet s: " << args.getSetName() << rsLog;
                response->noteMismatched();
                return Status(ErrorCodes::BadValue, "repl set names do not match");
            }
        }

        response->noteReplSet();
        if( (theReplSet == 0) || (theReplSet->startupStatus == ReplSetImpl::LOADINGCONFIG) ) {
            if (!args.getSenderHost().empty()) {
                scoped_lock lck( _settings.discoveredSeeds_mx );
                _settings.discoveredSeeds.insert(args.getSenderHost().toString());
            }
            response->setHbMsg("still initializing");
            return Status::OK();
        }

        if (theReplSet->name() != args.getSetName()) {
            response->noteMismatched();
            return Status(ErrorCodes::BadValue, "repl set names do not match (2)");
        }
        response->setSetName(theReplSet->name());

        MemberState currentState = theReplSet->state();
        response->setState(currentState.s);
        if (currentState == MemberState::RS_PRIMARY) {
            response->setElectionTime(theReplSet->getElectionTime().asDate());
        }

        response->setElectable(theReplSet->iAmElectable());
        response->setHbMsg(theReplSet->hbmsg());
        response->setTime((long long) time(0));
        response->setOpTime(theReplSet->lastOpTimeWritten.asDate());
        const Member *syncTarget = BackgroundSync::get()->getSyncTarget();
        if (syncTarget) {
            response->setSyncingTo(syncTarget->fullName());
        }

        int v = theReplSet->config().version;
        response->setVersion(v);
        if (v > args.getConfigVersion()) {
            ReplicaSetConfig config;
            fassert(18635, config.initialize(theReplSet->config().asBson()));
            response->setConfig(config);
        }

        Member* from = NULL;
        if (v == args.getConfigVersion() && args.getSenderId() != -1) {
            from = theReplSet->getMutableMember(args.getSenderId());
        }
        if (!from) {
            from = theReplSet->findByName(args.getSenderHost().toString());
            if (!from) {
                return Status::OK();
            }
        }

        // if we thought that this node is down, let it know
        if (!from->hbinfo().up()) {
            response->noteStateDisagreement();
        }

        // note that we got a heartbeat from this node
        theReplSet->mgr->send(stdx::bind(&ReplSet::msgUpdateHBRecv,
                                         theReplSet, from->hbinfo().id(), time(0)));


        return Status::OK();
    }

    Status LegacyReplicationCoordinator::checkReplEnabledForCommand(BSONObjBuilder* result) {
        if (!_settings.usingReplSets()) {
            if (serverGlobalParams.configsvr) {
                result->append("info", "configsvr"); // for shell prompt
            }
            return Status(ErrorCodes::NoReplicationEnabled, "not running with --replSet");
        }

        if( theReplSet == 0 ) {
            result->append("startupStatus", ReplSet::startupStatus);
            if( ReplSet::startupStatus == ReplSet::EMPTYCONFIG )
                result->append("info", "run rs.initiate(...) if not yet done for the set");
            return Status(ErrorCodes::NotYetInitialized, ReplSet::startupStatusMsg.empty() ?
                    "replset unknown error 2" : ReplSet::startupStatusMsg.get());
        }

        return Status::OK();
    }

    Status LegacyReplicationCoordinator::processReplSetReconfig(OperationContext* txn,
                                                                const ReplSetReconfigArgs& args,
                                                                BSONObjBuilder* resultObj) {

        if( args.force && !theReplSet ) {
            _settings.reconfig = args.newConfigObj.getOwned();
            resultObj->append("msg",
                              "will try this config momentarily, try running rs.conf() again in a "
                                      "few seconds");
            return Status::OK();
        }

        // TODO(dannenberg) once reconfig processing has been figured out in the impl, this should
        // be moved out of processReplSetReconfig and into the command body like all other cmds
        Status status = checkReplEnabledForCommand(resultObj);
        if (!status.isOK()) {
            return status;
        }

        if( !args.force && !theReplSet->box.getState().primary() ) {
            return Status(ErrorCodes::NotMaster,
                          "replSetReconfig command must be sent to the current replica set "
                                  "primary.");
        }

        try {
            {
                // just make sure we can get a write lock before doing anything else.  we'll
                // reacquire one later.  of course it could be stuck then, but this check lowers the
                // risk if weird things are up - we probably don't want a change to apply 30 minutes
                // after the initial attempt.
                time_t t = time(0);
                Lock::GlobalWrite lk(txn->lockState());
                if( time(0)-t > 20 ) {
                    return Status(ErrorCodes::ExceededTimeLimit,
                                  "took a long time to get write lock, so not initiating.  "
                                          "Initiate when server less busy?");
                }
            }


            scoped_ptr<ReplSetConfig> newConfig(ReplSetConfig::make(args.newConfigObj, args.force));

            log() << "replSet replSetReconfig config object parses ok, " <<
                    newConfig->members.size() << " members specified" << rsLog;

            Status status = ReplSetConfig::legalChange(theReplSet->getConfig(), *newConfig);
            if (!status.isOK()) {
                return status;
            }

            checkMembersUpForConfigChange(*newConfig, *resultObj, false);

            log() << "replSet replSetReconfig [2]" << rsLog;

            theReplSet->haveNewConfig(txn, *newConfig, true);
            ReplSet::startupStatusMsg.set("replSetReconfig'd");
        }
        catch(const DBException& e) {
            log() << "replSet replSetReconfig exception: " << e.what() << rsLog;
            return e.toStatus();
        }

        resetSlaveCache();
        return Status::OK();
    }

    static HostAndPort someHostAndPortForMe() {
        const char* ips = serverGlobalParams.bind_ip.c_str();
        while (*ips) {
            std::string ip;
            const char* comma = strchr(ips, ',');
            if (comma) {
                ip = std::string(ips, comma - ips);
                ips = comma + 1;
            }
            else {
                ip = std::string(ips);
                ips = "";
            }
            HostAndPort h = HostAndPort(ip, serverGlobalParams.port);
            if (!h.isLocalHost()) {
                return h;
            }
        }

        std::string h = getHostName();
        verify(!h.empty());
        verify(h != "localhost");
        return HostAndPort(h, serverGlobalParams.port);
    }

    Status LegacyReplicationCoordinator::processReplSetInitiate(OperationContext* txn,
                                                                const BSONObj& givenConfig,
                                                                BSONObjBuilder* resultObj) {

        log() << "replSet replSetInitiate admin command received from client" << rsLog;

        if (!_settings.usingReplSets()) {
            return Status(ErrorCodes::NoReplicationEnabled, "server is not running with --replSet");
        }

        if( theReplSet ) {
            resultObj->append("info",
                              "try querying " + rsConfigNs + " to see current configuration");
            return Status(ErrorCodes::AlreadyInitialized, "already initialized");
        }

        try {
            {
                // just make sure we can get a write lock before doing anything else.  we'll
                // reacquire one later.  of course it could be stuck then, but this check lowers the
                // risk if weird things are up.
                time_t t = time(0);
                Lock::GlobalWrite lk(txn->lockState());
                if( time(0)-t > 10 ) {
                    return Status(ErrorCodes::ExceededTimeLimit,
                                  "took a long time to get write lock, so not initiating.  "
                                          "Initiate when server less busy?");
                }

                /* check that we don't already have an oplog.  that could cause issues.
                   it is ok if the initiating member has *other* data than that.
                   */
                BSONObj o;
                if( Helpers::getFirst(txn, rsoplog, o) ) {
                    return Status(ErrorCodes::AlreadyInitialized,
                                  rsoplog + string(" is not empty on the initiating member.  "
                                          "cannot initiate."));
                }
            }

            if( ReplSet::startupStatus == ReplSet::BADCONFIG ) {
                resultObj->append("info", ReplSet::startupStatusMsg.get());
                return Status(ErrorCodes::InvalidReplicaSetConfig,
                              "server already in BADCONFIG state (check logs); not initiating");
            }
            if( ReplSet::startupStatus != ReplSet::EMPTYCONFIG ) {
                resultObj->append("startupStatus", ReplSet::startupStatus);
                resultObj->append("info", _settings.replSet);
                return Status(ErrorCodes::InvalidReplicaSetConfig,
                              "all members and seeds must be reachable to initiate set");
            }

            BSONObj configObj;
            if (!givenConfig.isEmpty()) {
                configObj = givenConfig;
            } else {
                resultObj->append("info2", "no configuration explicitly specified -- making one");
                log() << "replSet info initiate : no configuration specified.  "
                        "Using a default configuration for the set" << rsLog;

                string name;
                vector<HostAndPort> seeds;
                set<HostAndPort> seedSet;
                parseReplSetSeedList(_settings.replSet, name, seeds, seedSet); // may throw...

                BSONObjBuilder b;
                b.append("_id", name);
                BSONObjBuilder members;
                HostAndPort me = someHostAndPortForMe();
                members.append("0", BSON( "_id" << 0 << "host" << me.toString() ));
                resultObj->append("me", me.toString());
                for( unsigned i = 0; i < seeds.size(); i++ ) {
                    members.append(BSONObjBuilder::numStr(i+1),
                                   BSON( "_id" << i+1 << "host" << seeds[i].toString()));
                }
                b.appendArray("members", members.obj());
                configObj = b.obj();
                log() << "replSet created this configuration for initiation : " <<
                        configObj.toString() << rsLog;
            }

            scoped_ptr<ReplSetConfig> newConfig;
            try {
                newConfig.reset(ReplSetConfig::make(configObj));
            } catch (const DBException& e) {
                log() << "replSet replSetInitiate exception: " << e.what() << rsLog;
                return Status(ErrorCodes::InvalidReplicaSetConfig,
                              mongoutils::str::stream() << "couldn't parse cfg object " << e.what());
            }

            if( newConfig->version > 1 ) {
                return Status(ErrorCodes::InvalidReplicaSetConfig,
                              "can't initiate with a version number greater than 1");
            }

            log() << "replSet replSetInitiate config object parses ok, " <<
                    newConfig->members.size() << " members specified" << rsLog;

            checkMembersUpForConfigChange(*newConfig, *resultObj, true);

            log() << "replSet replSetInitiate all members seem up" << rsLog;

            createOplog(txn);

            Lock::GlobalWrite lk(txn->lockState());
            BSONObj comment = BSON( "msg" << "initiating set");
            newConfig->saveConfigLocally(txn, comment);
            log() << "replSet replSetInitiate config now saved locally.  "
                "Should come online in about a minute." << rsLog;
            resultObj->append("info",
                              "Config now saved locally.  Should come online in about a minute.");
            ReplSet::startupStatus = ReplSet::SOON;
            ReplSet::startupStatusMsg.set("Received replSetInitiate - "
                                          "should come online shortly.");
        }
        catch(const DBException& e ) {
            return e.toStatus();
        }

        return Status::OK();
    }

    Status LegacyReplicationCoordinator::processReplSetGetRBID(BSONObjBuilder* resultObj) {
        resultObj->append("rbid", _rbid);
        return Status::OK();
    }

namespace {
    bool _shouldVeto(unsigned id, std::string* errmsg) {
        const Member* primary = theReplSet->box.getPrimary();
        const Member* hopeful = theReplSet->findById(id);
        const Member *highestPriority = theReplSet->getMostElectable();

        if (!hopeful) {
            *errmsg = str::stream() << "replSet couldn't find member with id " << id;
            return true;
        }

        if (theReplSet->isPrimary() &&
            theReplSet->lastOpTimeWritten >= hopeful->hbinfo().opTime) {
            // hbinfo is not updated, so we have to check the primary's last optime separately
            *errmsg = str::stream() << "I am already primary, " << hopeful->fullName() <<
                " can try again once I've stepped down";
            return true;
        }

        if (primary &&
                (hopeful->hbinfo().id() != primary->hbinfo().id()) &&
                (primary->hbinfo().opTime >= hopeful->hbinfo().opTime)) {
            // other members might be aware of more up-to-date nodes
            *errmsg = str::stream() << hopeful->fullName() <<
                " is trying to elect itself but " << primary->fullName() <<
                " is already primary and more up-to-date";
            return true;
        }

        if (highestPriority &&
            highestPriority->config().priority > hopeful->config().priority) {
            *errmsg = str::stream() << hopeful->fullName() << " has lower priority than " <<
                highestPriority->fullName();
            return true;
        }

        if (!theReplSet->isElectable(id)) {
            *errmsg = str::stream() << "I don't think " << hopeful->fullName() <<
                " is electable";
            return true;
        }

        return false;
    }
} // namespace

    Status LegacyReplicationCoordinator::processReplSetFresh(const ReplSetFreshArgs& args,
                                                             BSONObjBuilder* resultObj){
        if( args.setName != theReplSet->name() ) {
            return Status(ErrorCodes::ReplicaSetNotFound,
                          str::stream() << "wrong repl set name. Expected: " <<
                                  theReplSet->name() << ", received: " << args.setName);
        }

        bool weAreFresher = false;
        if( theReplSet->config().version > args.cfgver ) {
            log() << "replSet member " << args.who << " is not yet aware its cfg version " <<
                    args.cfgver << " is stale" << rsLog;
            resultObj->append("info", "config version stale");
            weAreFresher = true;
        }
        // check not only our own optime, but any other member we can reach
        else if( args.opTime < theReplSet->lastOpTimeWritten ||
                 args.opTime < theReplSet->lastOtherOpTime())  {
            weAreFresher = true;
        }
        resultObj->appendDate("opTime", theReplSet->lastOpTimeWritten.asDate());
        resultObj->append("fresher", weAreFresher);

        std::string errmsg;
        bool veto = _shouldVeto(args.id, &errmsg);
        resultObj->append("veto", veto);
        if (veto) {
            resultObj->append("errmsg", errmsg);
        }

        return Status::OK();
    }

    Status LegacyReplicationCoordinator::processReplSetElect(const ReplSetElectArgs& args,
                                                             BSONObjBuilder* resultObj) {
        theReplSet->electCmdReceived(args.set, args.whoid, args.cfgver, args.round, resultObj);
        return Status::OK();
    }

    void LegacyReplicationCoordinator::incrementRollbackID() {
        ++_rbid;
    }

    Status LegacyReplicationCoordinator::processReplSetFreeze(int secs, BSONObjBuilder* resultObj) {
        if (theReplSet->freeze(secs)) {
            if (secs == 0) {
                resultObj->append("info","unfreezing");
            }
        }
        if (secs == 1) {
            resultObj->append("warning", "you really want to freeze for only 1 second?");
        }
        return Status::OK();
    }

    Status LegacyReplicationCoordinator::processReplSetMaintenance(OperationContext* txn,
                                                                   bool activate,
                                                                   BSONObjBuilder* resultObj) {
        if (!setMaintenanceMode(txn, activate)) {
            if (theReplSet->isPrimary()) {
                return Status(ErrorCodes::NotSecondary, "primaries can't modify maintenance mode");
            }
            else {
                return Status(ErrorCodes::OperationFailed, "already out of maintenance mode");
            }
        }

        return Status::OK();
    }

    Status LegacyReplicationCoordinator::processReplSetSyncFrom(const std::string& target,
                                                                BSONObjBuilder* resultObj) {
        resultObj->append("syncFromRequested", target);

        return theReplSet->forceSyncFrom(target, resultObj);
    }

    Status LegacyReplicationCoordinator::processReplSetUpdatePosition(OperationContext* txn,
                                                                      const BSONArray& updates,
                                                                      BSONObjBuilder* resultObj) {
        BSONForEach(elem, updates) {
            BSONObj entry = elem.Obj();
            OID id = entry["_id"].OID();
            OpTime ot = entry["optime"]._opTime();
            Status status = setLastOptime(txn, id, ot);
            if (!status.isOK()) {
                return status;
            }
        }
        return Status::OK();
    }

    Status LegacyReplicationCoordinator::processReplSetUpdatePositionHandshake(
            const OperationContext* txn, const BSONObj& cmdObj, BSONObjBuilder* resultObj) {

        OID rid = cmdObj["handshake"].OID();
        Status status = processHandshake(txn, rid, cmdObj);
        if (!status.isOK()) {
            return status;
        }

        // if we're a replset and aren't primary, pass the handshake along
        if (theReplSet && !theReplSet->isPrimary()) {
            theReplSet->syncSourceFeedback.forwardSlaveHandshake();
        }
        return Status::OK();
    }

    Status LegacyReplicationCoordinator::processHandshake(const OperationContext* txn,
                                                          const OID& remoteID,
                                                          const BSONObj& handshake) {
        LOG(2) << "Received handshake " << handshake << " from node with RID " << remoteID;

        boost::lock_guard<boost::mutex> lock(_mutex);
        BSONObj configObj;
        if (handshake.hasField("config")) {
            configObj = handshake["config"].Obj().getOwned();
        } else {
            configObj = BSON("host" << txn->getClient()->clientAddress(true) <<
                             "upgradeNeeded" << true);
        }
        _ridConfigMap[remoteID] = configObj;

        if (getReplicationMode() != modeReplSet) {
            return Status::OK();
        }

        if (!handshake.hasField("member")) {
            return Status(ErrorCodes::ProtocolError,
                          str::stream() << "Handshake object did not contain \"member\" field.  "
                                  "Handshake" << handshake);
        }

        int memberID = handshake["member"].Int();
        Member* member = theReplSet->getMutableMember(memberID);
        // it is possible that a node that was removed in a reconfig tried to handshake this node
        // in that case, the Member will no longer be in theReplSet's _members List and member
        // will be NULL
        if (!member) {
            return Status(ErrorCodes::NodeNotFound,
                          str::stream() << "Node with replica set member ID " << memberID <<
                                  " could not be found in replica set config during handshake");
        }

        _ridMemberMap[remoteID] = member;
        theReplSet->syncSourceFeedback.forwardSlaveHandshake();
        return Status::OK();
    }

    void LegacyReplicationCoordinator::waitUpToOneSecondForOptimeChange(const OpTime& ot) {
        repl::waitUpToOneSecondForOptimeChange(ot);
    }

    bool LegacyReplicationCoordinator::buildsIndexes() {
        return theReplSet->buildIndexes();
    }

    vector<BSONObj> LegacyReplicationCoordinator::getHostsWrittenTo(const OpTime& op) {
        return repl::getHostsWrittenTo(op);
    }

    Status LegacyReplicationCoordinator::checkIfWriteConcernCanBeSatisfied(
            const WriteConcernOptions& writeConcern) const {
        // TODO: rewrite this method with the correct version. Note that this just a
        // temporary stub for secondary throttle.

        if (getReplicationMode() == ReplicationCoordinator::modeReplSet) {
            if (writeConcern.wNumNodes > 1 && theReplSet->config().getMajority() <= 1) {
                return Status(ErrorCodes::CannotSatisfyWriteConcern, "not enough nodes");
            }
        }

        return Status::OK();
    }

    BSONObj LegacyReplicationCoordinator::getGetLastErrorDefault() {
        if (getReplicationMode() == modeReplSet) {
            return theReplSet->getLastErrorDefault;
        }
        return BSONObj();
    }

    bool LegacyReplicationCoordinator::isReplEnabled() const {
        return _settings.usingReplSets() || _settings.slave || _settings.master;
    }

} // namespace repl
} // namespace mongo
