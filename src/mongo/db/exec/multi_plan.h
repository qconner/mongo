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

#include "mongo/db/diskloc.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/query/plan_ranker.h"

namespace mongo {

    /**
     * This stage outputs its mainChild, and possibly it's backup child
     * and also updates the cache.
     *
     * Preconditions: Valid DiskLoc.
     *
     */
    class MultiPlanStage : public PlanStage {
    public:
        /** Takes no ownership */
        MultiPlanStage(const Collection* collection, CanonicalQuery* cq);

        virtual ~MultiPlanStage();

        virtual bool isEOF();

        virtual StageState work(WorkingSetID* out);

        virtual void prepareToYield();

        virtual void recoverFromYield();

        virtual void invalidate(const DiskLoc& dl, InvalidationType type);

        virtual PlanStageStats* getStats();

        /** Takes ownership of QuerySolution and PlanStage. not of WorkingSet */
        void addPlan(QuerySolution* solution, PlanStage* root, WorkingSet* sharedWs);

        /**
         * Runs all plans added by addPlan, ranks them, and picks a best.  Deletes all loser plans.
         * All further calls to getNext(...) will return results from the best plan.
         */
        void pickBestPlan();

	/** Return true if a best plan has been chosen  */
        bool bestPlanChosen() const;

        /** Return the index of the best plan chosen, for testing */
        int bestPlanIdx() const;

        /** Returns the QuerySolution for the best plan, or NULL if no best plan */
        QuerySolution* bestSolution();

        /**
         * Returns true if a backup plan was picked.
         * This is the case when the best plan has a blocking stage.
         * Exposed for testing.
         */
        bool hasBackupPlan() const;

    private:
        /**
         * Have all our candidate plans do something.
         * If all our candidate plans fail, *objOut will contain
         * information on the failure.
         */
        bool workAllPlans();
        void allPlansSaveState();
        void allPlansRestoreState();

        static const int kNoSuchPlan = -1;

        // not owned here
        const Collection* _collection;

        // The query that we're trying to figure out the best solution to.
        // not owned here
        CanonicalQuery* _query;

        // Candidate plans.  Owned here.
        std::vector<CandidatePlan> _candidates;

        // Candidate plans' stats. Owned here.
        std::vector<PlanStageStats*> _candidateStats;

        // index into _candidates, of the winner of the plan competition
        // uses -1 / kNoSuchPlan when best plan is not (yet) known
        int _bestPlanIdx;

        // index into _candidates, of the backup plan for sort
        // uses -1 / kNoSuchPlan when best plan is not (yet) known
        int _backupPlanIdx;

        // Did all plans fail while we were running them?  Note that one plan can fail
        // during normal execution of the plan competition.  Here is an example:
        //
        // Plan 1: collection scan with sort.  Sort runs out of memory.
        // Plan 2: ixscan that provides sort.  Won't run out of memory.
        //
        // We want to choose plan 2 even if plan 1 fails.
        bool _failure;

        // If everything fails during the plan competition, we can't pick one.
        size_t _failureCount;

        // if pickBestPlan fails, this is set to the wsid of the statusMember
        // returned by ::work() 
        WorkingSetID _statusMemberId;

        // Stats
        CommonStats _commonStats;
        MultiPlanStats _specificStats;
    };

}  // namespace mongo
