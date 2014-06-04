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

#include <string>
#include <vector>

#include "mongo/db/repl/topology_coordinator.h"
#include "mongo/bson/optime.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/util/concurrency/list.h"

namespace mongo {
namespace repl {

    class TagSubgroup;

    class TopologyCoordinatorImpl : public TopologyCoordinator {
    public:
        TopologyCoordinatorImpl();
        virtual ~TopologyCoordinatorImpl() {};
        
        virtual void setLastApplied(const OpTime& optime);
        virtual void setCommitOkayThrough(const OpTime& optime);
        virtual void setLastReceived(const OpTime& optime);

        virtual int getSelfSlaveDelay() const;
        virtual bool getChainingAllowedFlag() const;

        // For use with w:majority write concern
        virtual int getMajorityNumber() const;

        // ReplCoord needs to know the state to implement certain public functions
        virtual MemberState getMemberState() const;

        // Looks up _syncSource's address and returns it, for use by the Applier
        virtual HostAndPort getSyncSourceAddress() const;
        // Chooses and sets a new sync source, based on our current knowledge of the world
        virtual void chooseNewSyncSource(); // this is basically getMemberToSyncTo()
        // Do not choose a member as a sync source for a while; 
        // call this when we have reason to believe it's a bad choice (do we need this?)
        // (currently handled by _veto in rs_initialsync)
        virtual void blacklistSyncSource(Member* member);

        // Add function pointer to callback list; call function when config changes
        // Applier needs to know when things like chainingAllowed or slaveDelay change. 
        // ReplCoord needs to know when things like the tag sets change.
        virtual void registerConfigChangeCallback(Callback_t);
        
        // Applier calls this to notify that it's now safe to transition from SECONDARY to PRIMARY
        virtual void signalDrainComplete();

        // election entry point
        virtual void electSelf();

        // produce a reply to a RAFT-style RequestVote RPC; this is MongoDB ReplSetFresh command
        virtual bool prepareRequestVoteResponse(const BSONObj& cmdObj, 
                                                std::string& errmsg, 
                                                BSONObjBuilder& result);

        // produce a reply to a received electCmd
        virtual void prepareElectCmdResponse(const BSONObj& cmdObj, BSONObjBuilder& result);

        // produce a reply to a heartbeat
        virtual bool prepareHeartbeatResponse(const BSONObj& cmdObj, 
                                              std::string& errmsg, 
                                              BSONObjBuilder& result);

        // update internal state with heartbeat response
        virtual void updateHeartbeatInfo(const HeartbeatInfo& newInfo);
        
    private:

        void _calculateMajorityNumber(); // possibly not needed

        OpTime _lastApplied;  // the last op that the applier has actually written to the data
        OpTime _commitOkayThrough; // the primary's latest op that won't get rolled back
        OpTime _lastReceived; // the last op we have received from our sync source


        MemberState _memberState;
        int _majorityNumber; // for w:majority writes
        
        // the member we currently believe is primary, if one exists
        const Member *_currentPrimary;
        // the member we are currently syncing from
        // NULL if no sync source (we are primary, or we cannot connect to anyone yet)
        const Member* _syncSource; 
        // These members are not chosen as sync sources for a period of time, due to connection
        // issues with them
        std::map<std::string, time_t> _syncSourceBlacklist;


        class MemberConfig {
        public:
        MemberConfig() : 
            _id(-1), 
                votes(1), 
                priority(1.0), 
                arbiterOnly(false), 
                slaveDelay(0), 
                hidden(false), 
                buildIndexes(true) { }
            int _id;              /* ordinal */
            unsigned votes;       /* how many votes this node gets. default 1. */
            HostAndPort h;
            double priority;      /* 0 means can never be primary */
            bool arbiterOnly;
            int slaveDelay;       /* seconds.  */
            bool hidden;          /* if set, don't advertise to drivers in isMaster. */
                                  /* for non-primaries (priority 0) */
            bool buildIndexes;    /* if false, do not create any non-_id indexes */
            std::map<std::string,std::string> tags;     /* tagging for data center, rack, etc. */
        private:
            std::set<TagSubgroup*> _groups; // the subgroups this member belongs to
        };

        struct ReplicaSetConfig {
            std::vector<MemberConfig> members;
            std::string replSetName;
            int version;
            MemberConfig* self;

            /**
             * If replication can be chained. If chaining is disallowed, it can still be explicitly
             * enabled via the replSetSyncFrom command, but it will not happen automatically.
             */
            bool chainingAllowed;

        } _currentConfig;

        Member* _self;
        List1<Member> _otherMembers; // all members of the set EXCEPT _self.

        // do these need settors?  the current code has no way to change these values.
        struct HeartbeatOptions {
        HeartbeatOptions() :  heartbeatSleepMillis(2000), 
                heartbeatTimeoutMillis(10000),
                heartbeatConnRetries(2) 
            { }
            
            unsigned heartbeatSleepMillis;
            unsigned heartbeatTimeoutMillis;
            unsigned heartbeatConnRetries ;

            void check() {
                uassert(17490, "bad replset heartbeat option", heartbeatSleepMillis >= 10);
                uassert(17491, "bad replset heartbeat option", heartbeatTimeoutMillis >= 10);
            }

            bool operator==(const HeartbeatOptions& r) const {
                return (heartbeatSleepMillis==r.heartbeatSleepMillis && 
                        heartbeatTimeoutMillis==r.heartbeatTimeoutMillis &&
                        heartbeatConnRetries==r.heartbeatConnRetries);
            }
        } _heartbeatOptions;


    };

} // namespace repl
} // namespace mongo
