/**
 *    Copyright (C) 2013-2014 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/db/catalog/database.h"
#include "mongo/db/exec/collection_scan.h"
#include "mongo/db/exec/fetch.h"
#include "mongo/db/exec/index_scan.h"
#include "mongo/db/exec/multi_plan.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/query/single_solution_runner.h"
#include "mongo/db/instance.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/dbtests/dbtests.h"

namespace QueryMultiPlanRunner {

    /**
     * Create query solution.
     */
    QuerySolution* createQuerySolution() {
        std::auto_ptr<QuerySolution> soln(new QuerySolution());
        soln->cacheData.reset(new SolutionCacheData());
        soln->cacheData->solnType = SolutionCacheData::COLLSCAN_SOLN;
        soln->cacheData->tree.reset(new PlanCacheIndexTree());
        return soln.release();
    }

    class MultiPlanRunnerBase {
    public:
        MultiPlanRunnerBase() { }

        virtual ~MultiPlanRunnerBase() {
            _client.dropCollection(ns());
        }

        void addIndex(const BSONObj& obj) {
            _client.ensureIndex(ns(), obj);
        }

        IndexDescriptor* getIndex(Database* db, const BSONObj& obj) {
            const Collection* collection = db->getCollection( ns() );
            return collection->getIndexCatalog()->findIndexByKeyPattern(obj);
        }

        void insert(const BSONObj& obj) {
            _client.insert(ns(), obj);
        }

        void remove(const BSONObj& obj) {
            _client.remove(ns(), obj);
        }

        static const char* ns() { return "unittests.QueryStageMultiPlanRunner"; }

    private:
        static DBDirectClient _client;
    };

    DBDirectClient MultiPlanRunnerBase::_client;

    // Basic ranking test: collection scan vs. highly selective index scan.  Make sure we also get
    // all expected results out as well.
    class MPRCollectionScanVsHighlySelectiveIXScan : public MultiPlanRunnerBase {
    public:
        void run() {
            Client::WriteContext ctx(ns());

            const int N = 5000;
            for (int i = 0; i < N; ++i) {
                insert(BSON("foo" << (i % 10)));
            }

            addIndex(BSON("foo" << 1));

            // Plan 0: IXScan over foo == 7
            // Every call to work() returns something so this should clearly win (by current scoring
            // at least).
            IndexScanParams ixparams;
            ixparams.descriptor = getIndex(ctx.ctx().db(), BSON("foo" << 1));
            ixparams.bounds.isSimpleRange = true;
            ixparams.bounds.startKey = BSON("" << 7);
            ixparams.bounds.endKey = BSON("" << 7);
            ixparams.bounds.endKeyInclusive = true;
            ixparams.direction = 1;

            const Collection* coll = ctx.ctx().db()->getCollection(ns());

            auto_ptr<WorkingSet> sharedWs(new WorkingSet());
            IndexScan* ix = new IndexScan(ixparams, sharedWs.get(), NULL);
            auto_ptr<PlanStage> firstRoot(new FetchStage(sharedWs.get(), ix, NULL, coll));

            // Plan 1: CollScan with matcher.
            CollectionScanParams csparams;
            csparams.collection = ctx.ctx().db()->getCollection( ns() );
            csparams.direction = CollectionScanParams::FORWARD;

            // Make the filter.
            BSONObj filterObj = BSON("foo" << 7);
            StatusWithMatchExpression swme = MatchExpressionParser::parse(filterObj);
            verify(swme.isOK());
            auto_ptr<MatchExpression> filter(swme.getValue());
            // Make the stage.
            auto_ptr<PlanStage> secondRoot(new CollectionScan(csparams, sharedWs.get(),
                                                              filter.get()));

            // Hand the plans off to the runner.
            CanonicalQuery* cq = NULL;
            verify(CanonicalQuery::canonicalize(ns(), BSON("foo" << 7), &cq).isOK());
            verify(NULL != cq);

            MultiPlanStage* mps = new MultiPlanStage(ctx.ctx().db()->getCollection(ns()),cq);
            mps->addPlan(createQuerySolution(), firstRoot.release(), sharedWs.get());
            mps->addPlan(createQuerySolution(), secondRoot.release(), sharedWs.get());

            // Plan 0 aka the first plan aka the index scan should be the best.
            mps->pickBestPlan();
            ASSERT(mps->bestPlanChosen());
            ASSERT_EQUALS(0, mps->bestPlanIdx());

            SingleSolutionRunner sr(
                ctx.ctx().db()->getCollection(ns()),
                cq,
                mps->bestSolution(),
                mps,
                sharedWs.release()
            );

            // Get all our results out.
            int results = 0;
            BSONObj obj;
            while (Runner::RUNNER_ADVANCED == sr.getNext(&obj, NULL)) {
                ASSERT_EQUALS(obj["foo"].numberInt(), 7);
                ++results;
            }

            ASSERT_EQUALS(results, N / 10);
        }
    };

    class All : public Suite {
    public:
        All() : Suite( "query_multi_plan_runner" ) { }

        void setupTests() {
            add<MPRCollectionScanVsHighlySelectiveIXScan>();
        }
    }  queryMultiPlanRunnerAll;

}  // namespace QueryMultiPlanRunner
