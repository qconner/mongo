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

#include "mongo/platform/basic.h"

#include "mongo/db/repl/topology_coordinator_impl.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/repl/isself.h"
#include "mongo/db/repl/member.h"
#include "mongo/db/repl/repl_coordinator_global.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/db/repl/rs_sync.h" // maxSyncSourceLagSecs
#include "mongo/util/log.h"

namespace mongo {

    MONGO_LOG_DEFAULT_COMPONENT_FILE(::mongo::logger::LogComponent::kReplication);

namespace repl {

    TopologyCoordinatorImpl::TopologyCoordinatorImpl() :
        _startupStatus(PRESTART), _busyWithElectSelf(false), _blockSync(false),
        _maintenanceModeCalls(0)
    {
    }

    void TopologyCoordinatorImpl::setLastApplied(const OpTime& optime) {
        _lastApplied = optime;
    }

    void TopologyCoordinatorImpl::setCommitOkayThrough(const OpTime& optime) {
        _commitOkayThrough = optime;
    }

    void TopologyCoordinatorImpl::setLastReceived(const OpTime& optime) {
        _lastReceived = optime;
    }

    HostAndPort TopologyCoordinatorImpl::getSyncSourceAddress() const {
        return _syncSource->h();
    }

    void TopologyCoordinatorImpl::chooseNewSyncSource(Date_t now) {
        // if we have a target we've requested to sync from, use it
/*
  This should be a HostAndPort.
*/
        // XXX Eric
/*
        if (_forceSyncTarget) {
            Member* target = _forceSyncTarget;
            _forceSyncTarget = 0;
            sethbmsg( str::stream() << "syncing to: " << target->fullName() << " by request", 0);
            return target;
        }
*/

        // wait for 2N pings before choosing a sync target
        int needMorePings = _currentConfig.members.size()*2 - HeartbeatInfo::numPings;

        if (needMorePings > 0) {
            OCCASIONALLY log() << "waiting for " << needMorePings 
                               << " pings from other members before syncing";
            return;
        }

        // If we are only allowed to sync from the primary, set that
        if (!_currentConfig.chainingAllowed) {
            // Sets NULL if we cannot reach the primary
            _syncSource = _currentPrimary;
        }

        // find the member with the lowest ping time that has more data than me

        // Find primary's oplog time. Reject sync candidates that are more than
        // maxSyncSourceLagSecs seconds behind.
        OpTime primaryOpTime;
        if (_currentPrimary)
            primaryOpTime = _currentPrimary->hbinfo().opTime;
        else
            // choose a time that will exclude no candidates, since we don't see a primary
            primaryOpTime = OpTime(maxSyncSourceLagSecs, 0);

        if (primaryOpTime.getSecs() < static_cast<unsigned int>(maxSyncSourceLagSecs)) {
            // erh - I think this means there was just a new election
            // and we don't yet know the new primary's optime
            primaryOpTime = OpTime(maxSyncSourceLagSecs, 0);
        }

        OpTime oldestSyncOpTime(primaryOpTime.getSecs() - maxSyncSourceLagSecs, 0);

        Member *closest = 0;

        // Make two attempts.  The first attempt, we ignore those nodes with
        // slave delay higher than our own.  The second attempt includes such
        // nodes, in case those are the only ones we can reach.
        // This loop attempts to set 'closest'.
        for (int attempts = 0; attempts < 2; ++attempts) {
            for (Member *m = _otherMembers.head(); m; m = m->next()) {
                if (!m->syncable())
                    continue;

                if (m->state() == MemberState::RS_SECONDARY) {
                    // only consider secondaries that are ahead of where we are
                    if (m->hbinfo().opTime <= _lastApplied)
                        continue;
                    // omit secondaries that are excessively behind, on the first attempt at least.
                    if (attempts == 0 &&
                        m->hbinfo().opTime < oldestSyncOpTime)
                        continue;
                }

                // omit nodes that are more latent than anything we've already considered
                if (closest &&
                    (m->hbinfo().ping > closest->hbinfo().ping))
                    continue;

                if (attempts == 0 &&
                    (_currentConfig.self->slaveDelay < m->config().slaveDelay 
                     || m->config().hidden)) {
                    continue; // skip this one in the first attempt
                }

                std::map<HostAndPort,Date_t>::iterator vetoed = _syncSourceBlacklist.find(m->h());
                if (vetoed != _syncSourceBlacklist.end()) {
                    // Do some veto housekeeping

                    // if this was on the veto list, check if it was vetoed in the last "while".
                    // if it was, skip.
                    if (vetoed->second >= now) {
                        if (now % 5 == 0) {
                            log() << "replSet not trying to sync from " << (*vetoed).first
                                  << ", it is vetoed for " << ((*vetoed).second - now) 
                                  << " more seconds" << rsLog;
                        }
                        continue;
                    }
                    _syncSourceBlacklist.erase(vetoed);
                    // fall through, this is a valid candidate now
                }
                // This candidate has passed all tests; set 'closest'
                closest = m;
            }
            if (closest) break; // no need for second attempt
        }

        if (!closest) {
            return;
        }

        sethbmsg( str::stream() << "syncing to: " << closest->fullName(), 0);
        _syncSource = closest;
    }
    
    void TopologyCoordinatorImpl::blacklistSyncSource(const HostAndPort& host, Date_t until) {
        _syncSourceBlacklist[host] = until;
    }

    void TopologyCoordinatorImpl::registerConfigChangeCallback(const ConfigChangeCallbackFn& fn) {
        _configChangeCallbacks.push_back(fn);
    }

    void TopologyCoordinatorImpl::registerStateChangeCallback(const StateChangeCallbackFn& fn) {
        _stateChangeCallbacks.push_back(fn);
    }
        
    // Applier calls this to notify that it's now safe to transition from SECONDARY to PRIMARY
    void TopologyCoordinatorImpl::signalDrainComplete()  {
        // TODO
        
    }

    void TopologyCoordinatorImpl::relinquishPrimary(OperationContext* txn) {
        LOG(2) << "replSet attempting to relinquish" << endl;
        invariant(txn->lockState()->isWriteLocked());
        if (_memberState != MemberState::RS_PRIMARY) {
            // Already relinquished?
            log() << "replSet warning attempted to relinquish but not primary";
            return;
        }
        
        log() << "replSet relinquishing primary state" << rsLog;
        _changeMemberState(MemberState::RS_SECONDARY);

        // close sockets that were talking to us so they don't blithly send many writes that
        // will fail with "not master" (of course client could check result code, but in
        // case they are not)
        log() << "replSet closing client sockets after relinquishing primary" << rsLog;
        //MessagingPort::closeAllSockets(ScopedConn::keepOpen);
        // XXX Eric: what to do here?
    }

    // election entry point
    void TopologyCoordinatorImpl::_electSelf(Date_t now) {
        verify( !_currentConfig.self->arbiterOnly );
        verify( _currentConfig.self->slaveDelay == 0 );
        try {
            // XXX Eric
            //            _electSelf(now);
        }
        catch (RetryAfterSleepException&) {
            throw;
        }
        catch (VoteException& ) { // Eric: XXX
            log() << "replSet not trying to elect self as responded yea to someone else recently" 
                  << rsLog;
        }
        catch (const DBException& e) {
            log() << "replSet warning caught unexpected exception in electSelf() " << e.toString() 
                  << rsLog;
        }
        catch (...) {
            log() << "replSet warning caught unexpected exception in electSelf()" << rsLog;
        }
    }

    // Produce a reply to a RAFT-style RequestVote RPC; this is MongoDB ReplSetFresh command
    // The caller should validate that the message is for the correct set, and has the required data
    void TopologyCoordinatorImpl::prepareRequestVoteResponse(const Date_t now,
                                                             const BSONObj& cmdObj,
                                                             std::string& errmsg,
                                                             BSONObjBuilder& result) {

        string who = cmdObj["who"].String();
        int cfgver = cmdObj["cfgver"].Int();
        OpTime opTime(cmdObj["opTime"].Date());

        bool weAreFresher = false;
        if( _currentConfig.version > cfgver ) {
            log() << "replSet member " << who << " is not yet aware its cfg version "
                  << cfgver << " is stale" << rsLog;
            result.append("info", "config version stale");
            weAreFresher = true;
        }
        // check not only our own optime, but any other member we can reach
        else if( opTime < _commitOkayThrough ||
                 opTime < _latestKnownOpTime())  {
            weAreFresher = true;
        }
        result.appendDate("opTime", _lastApplied.asDate());
        result.append("fresher", weAreFresher);

        bool doVeto = _shouldVeto(cmdObj, errmsg);
        result.append("veto",doVeto);
        if (doVeto) {
            result.append("errmsg", errmsg);
        }
    }

    bool TopologyCoordinatorImpl::_shouldVeto(const BSONObj& cmdObj, string& errmsg) const {
        // don't veto older versions
        if (cmdObj["id"].eoo()) {
            // they won't be looking for the veto field
            return false;
        }

        unsigned id = cmdObj["id"].Int();
        const Member* primary = _currentPrimary;
        const Member* hopeful = _getConstMember(id);
        const Member* highestPriority = _getHighestPriorityElectable();

        if (!hopeful) {
            errmsg = str::stream() << "replSet couldn't find member with id " << id;
            return true;
        }

        if (_currentPrimary && (_commitOkayThrough >= hopeful->hbinfo().opTime)) {
            // hbinfo is not updated, so we have to check the primary's last optime separately
            errmsg = str::stream() << "I am already primary, " << hopeful->fullName() <<
                " can try again once I've stepped down";
            return true;
        }

        if (_currentPrimary &&
                (hopeful->hbinfo().id() != primary->hbinfo().id()) &&
                (primary->hbinfo().opTime >= hopeful->hbinfo().opTime)) {
            // other members might be aware of more up-to-date nodes
            errmsg = str::stream() << hopeful->fullName() <<
                " is trying to elect itself but " << primary->fullName() <<
                " is already primary and more up-to-date";
            return true;
        }

        if (highestPriority &&
            highestPriority->config().priority > hopeful->config().priority) {
            errmsg = str::stream() << hopeful->fullName() << " has lower priority than " <<
                highestPriority->fullName();
            return true;
        }

        if (!_electableSet.count(id)) {
            errmsg = str::stream() << "I don't think " << hopeful->fullName() <<
                " is electable";
            return true;
        }

        return false;
    }

    namespace {
        const size_t LeaseTime = 3;
    } // namespace

    // produce a reply to a received electCmd
    void TopologyCoordinatorImpl::prepareElectCmdResponse(const Date_t now,
                                                          const BSONObj& cmdObj,
                                                          BSONObjBuilder& result) {

        //TODO: after eric's checkin, add executer stuff and error if cancelled
        DEV log() << "replSet received elect msg " << cmdObj.toString() << rsLog;
        else LOG(2) << "replSet received elect msg " << cmdObj.toString() << rsLog;

        string setName = cmdObj["setName"].String();
        unsigned whoid = cmdObj["whoid"].Int();
        int cfgver = cmdObj["cfgver"].Int();
        OID round = cmdObj["round"].OID();
        int myver = _currentConfig.version;

        const Member* primary = _currentPrimary;
        const Member* hopeful = _getConstMember(whoid);
        const Member* highestPriority = _getHighestPriorityElectable();

        int vote = 0;
        if( setName != _currentConfig.replSetName ) {
            log() << "replSet error received an elect request for '" << setName
                  << "' but our setName name is '" << _currentConfig.replSetName << "'" << rsLog;
        }
        else if( myver < cfgver ) {
            // we are stale.  don't vote
        }
        else if( myver > cfgver ) {
            // they are stale!
            log() << "replSet electCmdReceived info got stale version # during election" << rsLog;
            vote = -10000;
        }
        else if( !hopeful ) {
            log() << "replSet electCmdReceived couldn't find member with id " << whoid << rsLog;
            vote = -10000;
        }
        else if( primary && _memberState == MemberState::RS_PRIMARY ) {
            log() << "I am already primary, " << hopeful->fullName()
                  << " can try again once I've stepped down" << rsLog;
            vote = -10000;
        }
        else if (primary) {
            log() << hopeful->fullName() << " is trying to elect itself but " <<
                  primary->fullName() << " is already primary" << rsLog;
            vote = -10000;
        }
        else if( highestPriority &&
                 highestPriority->config().priority > hopeful->config().priority) {
            log() << hopeful->fullName() << " has lower priority than "
                  << highestPriority->fullName();
            vote = -10000;
        }
        else {
            try {
                if( _lastVote.when + LeaseTime >= now && _lastVote.who != whoid ) {
                    LOG(1) << "replSet not voting yea for " << whoid
                           << " voted for " << _lastVote.who << ' ' << now-_lastVote.when
                           << " secs ago" << rsLog;
                    //TODO: remove exception, and change control flow?
                    throw VoteException();
                }
                _lastVote.when = now;
                _lastVote.who = whoid;
                vote = _currentConfig.self->votes;
                dassert( hopeful->id() == whoid );
                log() << "replSet info voting yea for " <<  hopeful->fullName()
                      << " (" << whoid << ')' << rsLog;
            }
            catch(VoteException&) {
                log() << "replSet voting no for " << hopeful->fullName()
                      << " already voted for another" << rsLog;
            }
        }

        result.append("vote", vote);
        result.append("round", round);
    }

    // produce a reply to a heartbeat
    void TopologyCoordinatorImpl::prepareHeartbeatResponse(
            const ReplicationExecutor::CallbackData& data,
            Date_t now,
            const BSONObj& cmdObj,
            BSONObjBuilder* resultObj,
            Status* result) {
        if (data.status == ErrorCodes::CallbackCanceled) {
            *result = Status(ErrorCodes::ShutdownInProgress, "replication system is shutting down");
            return;
        }

        if( cmdObj["pv"].Int() != 1 ) {
            *result = Status(ErrorCodes::BadValue, "incompatible replset protocol version");
            return;
        }

        // Verify that replica set names match
        std::string rshb = std::string(cmdObj.getStringField("replSetHeartbeat"));
        const ReplSettings& replSettings = getGlobalReplicationCoordinator()->getSettings();
        if (replSettings.ourSetName() != rshb) {
            *result = Status(ErrorCodes::BadValue, "repl set names do not match");
            log() << "replSet set names do not match, our cmdline: " << replSettings.replSet
                  << rsLog;
            log() << "replSet rshb: " << rshb << rsLog;
            resultObj->append("mismatch", true);
            return;
        }

        // This is a replica set
        resultObj->append("rs", true);

/*
        if( cmdObj["checkEmpty"].trueValue() ) {
            // Eric: XXX takes read lock; only used for initial sync heartbeat
            resultObj->append("hasData", replHasDatabases());
        }
*/

        // Verify that the config's replset name matches
        if (_currentConfig.replSetName != cmdObj.getStringField("replSetHeartbeat")) {
            *result = Status(ErrorCodes::BadValue, "repl set names do not match (2)");
            resultObj->append("mismatch", true);
            return; 
        }
        resultObj->append("set", _currentConfig.replSetName);

        resultObj->append("state", _memberState.s);
        if (_memberState == MemberState::RS_PRIMARY) {
            resultObj->appendDate("electionTime", _electionTime.asDate());
        }

        // Are we electable
        resultObj->append("e", _electableSet.find(_self->id()) != _electableSet.end());
        // Heartbeat status message
        resultObj->append("hbmsg", _getHbmsg());
        resultObj->append("time", now);
        resultObj->appendDate("opTime", _lastApplied.asDate());

        if (_syncSource) {
            resultObj->append("syncingTo", _syncSource->fullName());
        }

        int v = _currentConfig.version;
        resultObj->append("v", v);
        // Deliver new config if caller's version is older than ours
        if( v > cmdObj["v"].Int() )
            *resultObj << "config" << _currentConfig.asBson();

        // Resolve the caller's id in our Member list
        Member* from = NULL;
        if (cmdObj.hasField("fromId")) {
            if (v == cmdObj["v"].Int()) {
                from = _getMutableMember(cmdObj["fromId"].Int());
            }
        }
        if (!from) {
            // Can't find the member, so we leave out the stateDisagreement field
            *result = Status::OK();
            return;
        }

        // if we thought that this node is down, let it know
        if (!from->hbinfo().up()) {
            resultObj->append("stateDisagreement", true);
        }

        // note that we got a heartbeat from this node
        from->get_hbinfo().lastHeartbeatRecv = now;
        *result = Status::OK();
    }


    Member* TopologyCoordinatorImpl::_getMutableMember(unsigned id) {
        if( _self && id == _self->id() ) return _self;

        for( Member *m = _otherMembers.head(); m; m = m->next() )
            if( m->id() == id )
                return m;
        return NULL;
    }

    const Member* TopologyCoordinatorImpl::_getConstMember(unsigned id) const {
        if( _self && id == _self->id() ) return _self;

        for( Member *m = _otherMembers.head(); m; m = m->next() )
            if( m->id() == id )
                return m;
        return NULL;
    }


    BSONObj TopologyCoordinatorImpl::ReplicaSetConfig::asBson() const {
        // Default values for fields are omitted.
        BSONObjBuilder b;
        b << "_id" << self->_id;
        b.append("host", self->h.toString());
        if( self->votes != 1 ) b << "votes" << self->votes;
        if( self->priority != 1.0 ) b << "priority" << self->priority;
        if( self->arbiterOnly ) b << "arbiterOnly" << true;
        if( self->slaveDelay ) b << "slaveDelay" << self->slaveDelay;
        if( self->hidden ) b << "hidden" << self->hidden;
        if( !self->buildIndexes ) b << "buildIndexes" << self->buildIndexes;
        if( !self->tags.empty() ) {
            BSONObjBuilder a;
            for( map<string,string>::const_iterator i = self->tags.begin(); 
                 i != self->tags.end(); i++ )
                a.append((*i).first, (*i).second);
            b.append("tags", a.done());
        }
        return b.obj();
    }

    // update internal state with heartbeat response, and run topology checks
    HeartbeatResultAction TopologyCoordinatorImpl::updateHeartbeatInfo(
                                                        Date_t now,
                                                        const HeartbeatInfo& newInfo) {
        // Fill in the new heartbeat data for the appropriate member
        for (Member *m = _otherMembers.head(); m; m=m->next()) {
            if (m->id() == newInfo.id()) {
                m->get_hbinfo().updateFromLastPoll(newInfo);
                break;
            }
        }

        // Don't bother to make any changes if we are an election candidate
        if (_busyWithElectSelf) return None;

        // ex-checkelectableset begins here
        unsigned int latestOp = _latestKnownOpTime().getSecs();
        
        // make sure the electable set is up-to-date
        if (_aMajoritySeemsToBeUp()
            && !_currentConfig.self->arbiterOnly    // not an arbiter
            && (_currentConfig.self->priority > 0)  // not priority 0
            && (_stepDownUntil <= now)              // stepDown timer has expired
            && (_memberState == MemberState::RS_SECONDARY)
            // we are within 10 seconds of primary
            && (latestOp == 0 || _lastApplied.getSecs() >= latestOp - 10)) {
            _electableSet.insert(_currentConfig.self->_id);
        }
        else {
            _electableSet.erase(_currentConfig.self->_id);
        }

        // check if we should ask the primary (possibly ourselves) to step down
        const Member* highestPriority = _getHighestPriorityElectable();
        const Member* primary = _currentPrimary;
        
        if (primary && highestPriority &&
            highestPriority->config().priority > primary->config().priority &&
            // if we're stepping down to allow another member to become primary, we
            // better have another member (latestOp), and it should be up-to-date
            latestOp != 0 && highestPriority->hbinfo().opTime.getSecs() >= latestOp - 10) {
            log() << "stepping down " << primary->fullName() << " (priority " <<
                primary->config().priority << "), " << highestPriority->fullName() <<
                " is priority " << highestPriority->config().priority << " and " <<
                (latestOp - highestPriority->hbinfo().opTime.getSecs()) << " seconds behind";

            // Are we primary?
            // TODO: remove isSefl check
            if (isSelf(primary->h())) {
                // replSetStepDown tries to acquire the same lock
                // msgCheckNewState takes, so we can't call replSetStepDown on
                // ourselves.
                // XXX Eric: schedule relinquish
                //rs->relinquish();
                return StepDown;
            }
            else {
                // We are not primary.  Step down the remote node.
                BSONObj cmd = BSON( "replSetStepDown" << 1 );
/*                ScopedConn conn(primary->fullName());
                BSONObj result;
                // XXX Eric: schedule stepdown command

                try {
                    if (!conn.runCommand("admin", cmd, result, 0)) {
                        log() << "stepping down " << primary->fullName()
                              << " failed: " << result << endl;
                    }
                }
                catch (DBException &e) {
                    log() << "stepping down " << primary->fullName() << " threw exception: "
                          << e.toString() << endl;
                }

*/
                return StepDown;
            }
        }


        // ex-checkauth begins here
        {
            int down = 0, authIssue = 0, total = 0;

            for( Member *m = _otherMembers.head(); m; m=m->next() ) {
                total++;

                // all authIssue servers will also be not up
                if (!m->hbinfo().up()) {
                    down++;
                    if (m->hbinfo().authIssue) {
                        authIssue++;
                    }
                }
            }

            // if all nodes are down or failed auth AND at least one failed
            // auth, go into recovering.  If all nodes are down, stay a
            // secondary.
            if (authIssue > 0 && down == total) {
                log() << "replset error could not reach/authenticate against any members";

                if (_currentPrimary == _self) {
                    log() << "auth problems, relinquishing primary" << rsLog;
                    // XXX Eric: schedule relinquish
                    //rs->relinquish();

                    return StepDown;
                }

                _blockSync = true;
                // syncing is how we get into SECONDARY state, so we'll be stuck in
                // RECOVERING until we unblock
                _changeMemberState(MemberState::RS_RECOVERING);
            }
            else {
                _blockSync = false;
            }
        }

        // If a remote is primary, check that it is still up.
        if (_currentPrimary && _currentPrimary->id() != _self->id()) {
            if (!_currentPrimary->hbinfo().up() || 
                !_currentPrimary->hbinfo().hbstate.primary()) {
                _currentPrimary = NULL;
            }
        }

        // Scan the member list's heartbeat data for who is primary, and update ourselves if it's
        // not what _currentPrimary is.
        {
            const Member* remotePrimary(NULL);
            Member* m = _otherMembers.head();
            while (m) {
                DEV verify( m != _self );
                if( m->state().primary() && m->hbinfo().up() ) {
                    if( remotePrimary ) {
                        /* two other nodes think they are primary (asynchronously polled) -- wait for things to settle down. */
                        log() << "replSet info two primaries (transiently)" << rsLog;
                        return None;
                    }
                    remotePrimary = m;
                }
                m = m->next();
            }

            if (remotePrimary) {
                // If it's the same as last time, don't do anything further.
                if (_currentPrimary == remotePrimary) {
                    return None;
                }
                // Clear last heartbeat message on ourselves (why?)
                _self->lhb() = "";

                // insanity: this is what actually puts arbiters into ARBITER state
                if (_currentConfig.self->arbiterOnly) {
                    _changeMemberState(MemberState::RS_ARBITER);
                    return None;
                }

                // If we are also primary, this is a problem.  Determine who should step down.
                if (_memberState == MemberState::RS_PRIMARY) {
                    OpTime remoteElectionTime = remotePrimary->hbinfo().electionTime;
                    log() << "replset: another primary seen with election time " 
                          << remoteElectionTime; 
                    // Step down whoever has the older election time.
                    if (remoteElectionTime > _electionTime) {
                        log() << "stepping down; another primary was elected more recently";
                        // XXX Eric: schedule a relinquish
                        //rs->relinquish();
                        // after completion, set currentprimary to remotePrimary.
                        return StepDown;
                    }
                    else {
                        // else, stick around
                        log() << "another PRIMARY detected but it should step down"
                            " since it was elected earlier than me";
                        return None;
                    }
                }

                _currentPrimary = remotePrimary;
                return None;
            }
            /* didn't find anyone who is currently primary */
        }

        // If we are primary, check if we can still see majority of the set;
        // stepdown if we can't.
        if (_currentPrimary) {
            /* we must be primary */
            fassert(18505, _currentPrimary == _self);

            if (_shouldRelinquish()) {
                log() << "can't see a majority of the set, relinquishing primary" << rsLog;
                // XXX Eric: schedule a relinquish
                //rs->relinquish();
                return StepDown;

            }

            return None;
        }

        // At this point, there is no primary anywhere.  Check to see if we should become an
        // election candidate.

        // If we can't elect ourselves due to config, can't become a candidate.
        if (!_currentConfig.self->arbiterOnly       // not an arbiter
            && (_currentConfig.self->priority > 0)  // not priority 0
            && (_stepDownUntil <= now)              // stepDown timer has expired
            && (_memberState == MemberState::RS_SECONDARY)) {
            OCCASIONALLY log() << "replSet I don't see a primary and I can't elect myself";
            return None;
        }

        // If we can't see a majority, can't become a candidate.
        if (!_aMajoritySeemsToBeUp()) {
            static Date_t last;
            static int n = 0;
            int ll = 0;
            if( ++n > 5 ) ll++;
            if( last + 60 > now ) ll++;
            LOG(ll) << "replSet can't see a majority, will not try to elect self" << rsLog;
            last = now;
            return None;
        }

        // If we can't elect ourselves due to the current electable set;
        // we are in the set if we are within 10 seconds of the latest known op (via heartbeats)
        if (!(_electableSet.find(_self->id()) != _electableSet.end())) {
            // we are too far behind to become primary
            return None;
        }

        // All checks passed, become a candidate and start election proceedings.

        // don't try to do further elections & such while we are already working on one.
        _busyWithElectSelf = true; 
        return StartElection;

    // XXX: schedule an election
/*
        try {
            rs->elect.electSelf();
        }
        catch(RetryAfterSleepException&) {
            // we want to process new inbounds before trying this again.  so we just put a checkNewstate in the queue for eval later. 
            requeue();
        }
        catch(...) {
            log() << "replSet error unexpected assertion in rs manager" << rsLog;
        }
        
    }

*/
        _busyWithElectSelf = false;
        return None;
    }

    bool TopologyCoordinatorImpl::_shouldRelinquish() const {
        int vUp = _currentConfig.self->votes;
        for ( Member *m = _otherMembers.head(); m; m = m->next() ) {
            if (m->hbinfo().up()) {
                vUp += m->config().votes;
            }
        }

        return !( vUp * 2 > _totalVotes() );
    }

    bool TopologyCoordinatorImpl::_aMajoritySeemsToBeUp() const {
        int vUp = _currentConfig.self->votes;
        for ( Member *m = _otherMembers.head(); m; m=m->next() )
            vUp += m->hbinfo().up() ? m->config().votes : 0;
        return vUp * 2 > _totalVotes();
    }

    void TopologyCoordinatorImpl::ReplicaSetConfig::calculateMajorityNumber() {
        int total = members.size();
        int nonArbiters = total;
        int strictMajority = total/2+1;

        for (std::vector<MemberConfig>::iterator it = members.begin(); 
             it < members.end();
             it++) {
            if ((*it).arbiterOnly) {
                nonArbiters--;
            }
        }

        // majority should be all "normal" members if we have something like 4
        // arbiters & 3 normal members
        majorityNumber = (strictMajority > nonArbiters) ? nonArbiters : strictMajority;
 
    }

    int TopologyCoordinatorImpl::_totalVotes() const {
        static int complain = 0;
        int vTot = _currentConfig.self->votes;
        for( Member *m = _otherMembers.head(); m; m=m->next() )
            vTot += m->config().votes;
        if( vTot % 2 == 0 && vTot && complain++ == 0 )
            log() << "replSet warning: even number of voting members in replica set config - "
                     "add an arbiter or set votes to 0 on one of the existing members" << rsLog;
        return vTot;
    }

    OpTime TopologyCoordinatorImpl::_latestKnownOpTime() const {
        OpTime latest(0,0);

        for( Member *m = _otherMembers.head(); m; m=m->next() ) {
            if (!m->hbinfo().up()) {
                continue;
            }

            if (m->hbinfo().opTime > latest) {
                latest = m->hbinfo().opTime;
            }
        }

        return latest;
    }

    const Member* TopologyCoordinatorImpl::_getHighestPriorityElectable() const {
        const Member* max = NULL;
        std::set<unsigned int>::iterator it = _electableSet.begin();
        while (it != _electableSet.end()) {
            const Member* temp = _getConstMember(*it);
            if (!temp) {
                log() << "couldn't find member: " << *it << endl;
                it++;
                continue;
            }
            if (!max || max->config().priority < temp->config().priority) {
                max = temp;
            }
            it++;
        }

        return max;
    }

    void TopologyCoordinatorImpl::_changeMemberState(const MemberState& newMemberState) {
        if (_memberState == newMemberState) {
            // nothing to do
            return;
        }
        _memberState = newMemberState;
        log() << "replSet " << _memberState.toString() << rsLog;

        for (std::vector<StateChangeCallbackFn>::const_iterator it = _stateChangeCallbacks.begin();
             it != _stateChangeCallbacks.end(); ++it) {
            (*it)(_memberState);
        }
    }

    void TopologyCoordinatorImpl::prepareStatusResponse(Date_t now,
                                                        const BSONObj& cmdObj,
                                                        BSONObjBuilder& result,
                                                        unsigned uptime) {
        // output for each member
        vector<BSONObj> membersOut;
        MemberState myState = _memberState;

        // add self
        {
            BSONObjBuilder bb;
            bb.append("_id", (int) _self->id());
            bb.append("name", _self->fullName());
            bb.append("health", 1.0);
            bb.append("state", (int)myState.s);
            bb.append("stateStr", myState.toString());
            bb.append("uptime", uptime);
            if (!_self->config().arbiterOnly) {
                bb.appendTimestamp("optime", _lastApplied.asDate());
                bb.appendDate("optimeDate", _lastApplied.getSecs() * 1000LL);
            }

            if (_maintenanceModeCalls) {
                bb.append("maintenanceMode", _maintenanceModeCalls);
            }

            std::string s = _getHbmsg();
            if( !s.empty() )
                bb.append("infoMessage", s);

            if (myState == MemberState::RS_PRIMARY) {
                bb.append("electionTime", _electionTime);
                bb.appendDate("electionDate", _electionTime.asDate());
            }
            bb.append("self", true);
            membersOut.push_back(bb.obj());
        }

        // add other members
        Member* m = _otherMembers.head();
        while( m ) {
            BSONObjBuilder bb;
            bb.append("_id", (int) m->id());
            bb.append("name", m->fullName());
            double h = m->hbinfo().health;
            bb.append("health", h);
            bb.append("state", (int) m->state().s);
            if( h == 0 ) {
                // if we can't connect the state info is from the past
                // and could be confusing to show
                bb.append("stateStr", "(not reachable/healthy)");
            }
            else {
                bb.append("stateStr", m->state().toString());
            }
            bb.append("uptime",
                     (unsigned) (m->hbinfo().upSince ? (time(0)-m->hbinfo().upSince) : 0));
            if (!m->config().arbiterOnly) {
                bb.appendTimestamp("optime", m->hbinfo().opTime.asDate());
                bb.appendDate("optimeDate", m->hbinfo().opTime.getSecs() * 1000LL);
            }
            bb.appendTimeT("lastHeartbeat", m->hbinfo().lastHeartbeat);
            bb.appendTimeT("lastHeartbeatRecv", m->hbinfo().lastHeartbeatRecv);
            bb.append("pingMs", m->hbinfo().ping);
            string s = m->lhb();
            if( !s.empty() )
                bb.append("lastHeartbeatMessage", s);

            if (m->hbinfo().authIssue) {
                bb.append("authenticated", false);
            }

            string syncingTo = m->hbinfo().syncingTo;
            if (!syncingTo.empty()) {
                bb.append("syncingTo", syncingTo);
            }

            if (m->state() == MemberState::RS_PRIMARY) {
                bb.appendTimestamp("electionTime", m->hbinfo().electionTime.asDate());
                bb.appendDate("electionDate", m->hbinfo().electionTime.getSecs() * 1000LL);
            }

            membersOut.push_back(bb.obj());
            m = m->next();
        }

        // sort members bson
        sort(membersOut.begin(), membersOut.end());

        result.append("set", _currentConfig.replSetName);
        result.appendTimeT("date", now);
        result.append("myState", myState.s);

        // Add sync source info
        if ( _syncSource &&
            (myState != MemberState::RS_PRIMARY) &&
            (myState != MemberState::RS_SHUNNED) ) {
            result.append("syncingTo", _syncSource->fullName());
        }

        result.append("members", membersOut);
        /* TODO: decide where this lands
        if( replSetBlind )
            result.append("blind",true); // to avoid confusion if set...
                                         // normally never set except for testing.
        */
    }
    void TopologyCoordinatorImpl::prepareFreezeResponse(Date_t now,
                                                        const BSONObj& cmdObj,
                                                        BSONObjBuilder& result) {
        int secs = (int) cmdObj.firstElement().numberInt();

        if (secs == 0) {
            _stepDownUntil = now;
            log() << "replSet info 'unfreezing'" << rsLog;
            result.append("info","unfreezing");
        }
        else {
            if( secs == 1 )
                result.append("warning", "you really want to freeze for only 1 second?");

            if (_memberState != MemberState::RS_PRIMARY) {
                _stepDownUntil = now + secs;
                log() << "replSet info 'freezing' for " << secs << " seconds" << rsLog;
            }
            else {
                log() << "replSet info received freeze command but we are primary" << rsLog;
            }
        }
    }

    void TopologyCoordinatorImpl::updateConfig(const ReplicaSetConfig newConfig, const int selfId) {
    }
} // namespace repl
} // namespace mongo
