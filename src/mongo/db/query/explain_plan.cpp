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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

// THIS FILE IS DEPRECATED -- the old explain implementation is being replaced

#include "mongo/db/query/explain_plan.h"

#include "mongo/db/query/explain.h"
#include "mongo/db/query/stage_types.h"
#include "mongo/db/query/type_explain.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using mongoutils::str::stream;

    namespace {

        bool isOrStage(StageType stageType) {
            return stageType == STAGE_OR || stageType == STAGE_SORT_MERGE;
        }

        bool isNearStage(StageType stageType) {
            return stageType == STAGE_GEO_NEAR_2D || stageType == STAGE_GEO_NEAR_2DSPHERE;
        }

        bool isIntersectPlan(const PlanStageStats& stats) {
            if (stats.stageType == STAGE_AND_HASH || stats.stageType == STAGE_AND_SORTED) {
                return true;
            }
            for (size_t i = 0; i < stats.children.size(); ++i) {
                if (isIntersectPlan(*stats.children[i])) {
                    return true;
                }
            }
            return false;
        }

        void getLeafNodes(const PlanStageStats& stats, vector<const PlanStageStats*>* leafNodesOut) {
            if (0 == stats.children.size()) {
                leafNodesOut->push_back(&stats);
            }
            for (size_t i = 0; i < stats.children.size(); ++i) {
                getLeafNodes(*stats.children[i], leafNodesOut);
            }
        }

        const PlanStageStats* findNode(const PlanStageStats* root, StageType type) {
            if (root->stageType == type) {
                return root;
            }
            for (size_t i = 0; i < root->children.size(); ++i) {
                const PlanStageStats* ret = findNode(root->children[i], type);
                if (NULL != ret) {
                    return ret;
                }
            }
            return NULL;
        }

    }  // namespace

    Status explainIntersectPlan(const PlanStageStats& stats, TypeExplain** explainOut, bool fullDetails) {
        auto_ptr<TypeExplain> res(new TypeExplain);
        res->setCursor("Complex Plan");
        res->setN(stats.common.advanced);

        // Sum the various counters at the leaves.
        vector<const PlanStageStats*> leaves;
        getLeafNodes(stats, &leaves);

        long long nScanned = 0;
        long long nScannedObjects = 0;
        for (size_t i = 0; i < leaves.size(); ++i) {
            TypeExplain* leafExplain;
            explainPlan(*leaves[i], &leafExplain, false);
            nScanned += leafExplain->getNScanned();
            nScannedObjects += leafExplain->getNScannedObjects();
            delete leafExplain;
        }

        res->setNScanned(nScanned);
        // XXX: this isn't exactly "correct" -- for ixscans we have to find out if it's part of a
        // subtree rooted at a fetch, etc. etc.  do we want to just add the # of advances of a
        // fetch node minus the number of alreadyHasObj for those nodes?
        res->setNScannedObjects(nScannedObjects);

        uint64_t chunkSkips = 0;
        const PlanStageStats* shardFilter = findNode(&stats, STAGE_SHARDING_FILTER);
        if (NULL != shardFilter) {
            const ShardingFilterStats* sfs
                = static_cast<const ShardingFilterStats*>(shardFilter->specific.get());
            chunkSkips = sfs->chunkSkips;
        }

        res->setNChunkSkips(chunkSkips);

        if (fullDetails) {
            res->setNYields(stats.common.yields);
            BSONObjBuilder bob;
            Explain::statsToBSON(stats, &bob);
            res->stats = bob.obj();
        }

        *explainOut = res.release();
        return Status::OK();
    }

    namespace {

        Status explainPlan(const PlanStageStats& stats, TypeExplain** explainOut,
                           bool fullDetails, bool covered) {
            //
            // Temporary explain for index intersection
            //

            if (isIntersectPlan(stats)) {
                return explainIntersectPlan(stats, explainOut, fullDetails);
            }

            //
            // Legacy explain implementation
            //

            // Descend the plan looking for structural properties:
            // + Are there any OR clauses?  If so, explain each branch.
            // + What type(s) are the leaf nodes and what are their properties?
            // + Did we need a sort?

            bool sortPresent = false;
            size_t chunkSkips = 0;


            // XXX: TEMPORARY HACK - GEONEAR explains like OR queries (both have children) until the
            // new explain framework makes this file go away.
            const PlanStageStats* orStage = NULL;
            const PlanStageStats* root = &stats;
            const PlanStageStats* leaf = root;

            while (leaf->children.size() > 0) {
                // We shouldn't be here if there are any ANDs
                if (leaf->children.size() > 1) {
                    verify(isOrStage(leaf->stageType) || isNearStage(leaf->stageType));
                }

                if (isOrStage(leaf->stageType) || isNearStage(leaf->stageType)) {
                    orStage = leaf;
                    break;
                }

                if (leaf->stageType == STAGE_FETCH) {
                    covered = false;
                }

                if (leaf->stageType == STAGE_SORT) {
                    sortPresent = true;
                }

                if (STAGE_SHARDING_FILTER == leaf->stageType) {
                    const ShardingFilterStats* sfs
                        = static_cast<const ShardingFilterStats*>(leaf->specific.get());
                    chunkSkips = sfs->chunkSkips;
                }

                leaf = leaf->children[0];
            }

            auto_ptr<TypeExplain> res(new TypeExplain);

            // Accounting for 'nscanned' and 'nscannedObjects' is specific to the kind of leaf:
            //
            // + on collection scan, both are the same; all the documents retrieved were
            //   fetched in practice. To get how many documents were retrieved, one simply
            //   looks at the number of 'advanced' in the stats.
            //
            // + on an index scan, we'd neeed to look into the index scan cursor to extract the
            //   number of keys that cursor retrieved, and into the stage's stats 'advanced' for
            //   nscannedObjects', which would be the number of keys that survived the IXSCAN
            //   filter. Those keys would have been FETCH-ed, if a fetch is present.

            if (orStage != NULL) {
                size_t nScanned = 0;
                size_t nScannedObjects = 0;
                const std::vector<PlanStageStats*>& children = orStage->children;
                for (std::vector<PlanStageStats*>::const_iterator it = children.begin();
                     it != children.end();
                     ++it) {
                    TypeExplain* childExplain = NULL;
                    explainPlan(**it, &childExplain, false /* no full details */, covered);
                    if (childExplain) {
                        // Override child's indexOnly value if we have a non-covered
                        // query (implied by a FETCH stage).
                        //
                        // As we run explain on each child, explainPlan() sets indexOnly
                        // based only on the information in each child. This does not
                        // consider the possibility of a FETCH stage above the OR/MERGE_SORT
                        // stage, in which case the child's indexOnly may be erroneously set
                        // to true.
                        if (!covered && childExplain->isIndexOnlySet()) {
                            childExplain->setIndexOnly(false);
                        }

                        // 'res' takes ownership of 'childExplain'.
                        res->addToClauses(childExplain);
                        nScanned += childExplain->getNScanned();
                        nScannedObjects += childExplain->getNScannedObjects();
                    }
                }
                // We set the cursor name for backwards compatibility with 2.4.
                if (isOrStage(leaf->stageType)) {
                    res->setCursor("QueryOptimizerCursor");
                }
                else {
                    if (leaf->stageType == STAGE_GEO_NEAR_2D)
                        res->setCursor("GeoSearchCursor");
                    else
                        res->setCursor("S2NearCursor");

                    res->setIndexOnly(false);
                    res->setIsMultiKey(false);
                }
                res->setNScanned(nScanned);
                res->setNScannedObjects(nScannedObjects);
            }
            else if (leaf->stageType == STAGE_COLLSCAN) {
                CollectionScanStats* csStats = static_cast<CollectionScanStats*>(leaf->specific.get());
                res->setCursor("BasicCursor");
                res->setNScanned(csStats->docsTested);
                res->setNScannedObjects(csStats->docsTested);
                res->setIndexOnly(false);
                res->setIsMultiKey(false);
            }
            else if (leaf->stageType == STAGE_TEXT) {
                TextStats* tStats = static_cast<TextStats*>(leaf->specific.get());
                res->setCursor("TextCursor");
                res->setNScanned(tStats->keysExamined);
                res->setNScannedObjects(tStats->fetches);
            }
            else if (leaf->stageType == STAGE_IXSCAN) {
                IndexScanStats* indexStats = static_cast<IndexScanStats*>(leaf->specific.get());
                verify(indexStats);
                string direction = indexStats->direction > 0 ? "" : " reverse";
                res->setCursor(indexStats->indexType + " " + indexStats->indexName + direction);
                res->setNScanned(indexStats->keysExamined);

                // If we're covered, that is, no FETCH is present, then, by definition,
                // nScannedObject would be zero because no full document would have been fetched
                // from disk.
                res->setNScannedObjects(covered ? 0 : leaf->common.advanced);

                res->setIndexBounds(indexStats->indexBounds);
                res->setIsMultiKey(indexStats->isMultiKey);
                res->setIndexOnly(covered);
            }
            else if (leaf->stageType == STAGE_DISTINCT) {
                DistinctScanStats* dss = static_cast<DistinctScanStats*>(leaf->specific.get());
                verify(dss);
                res->setCursor("DistinctCursor");
                res->setN(dss->keysExamined);
                res->setNScanned(dss->keysExamined);
                // Distinct hack stage is fully covered.
                res->setNScannedObjects(0);
            }
            else {
                return Status(ErrorCodes::InternalError, "cannot interpret execution plan");
            }

            // How many documents did the query return?
            res->setN(root->common.advanced);
            res->setScanAndOrder(sortPresent);
            res->setNChunkSkips(chunkSkips);

            // Statistics for the plan (appear only in a detailed mode)
            // TODO: if we can get this from the runner, we can kill "detailed mode"
            if (fullDetails) {
                res->setNYields(root->common.yields);
                BSONObjBuilder bob;
                Explain::statsToBSON(*root, &bob);
                res->stats = bob.obj();
            }

            *explainOut = res.release();
            return Status::OK();
        }

    }  // namespace

    Status explainPlan(const PlanStageStats& stats, TypeExplain** explainOut, bool fullDetails) {
        // This function merely calls a recursive helper of the same name.  The boolean "covered" is
        // used to determine the value of nscannedObjects for subtrees along the way.  Recursive
        // calls will pass false for "covered" if a fetch stage has been seen at that point in the
        // traversal.
        const bool covered = true;
        return explainPlan(stats, explainOut, fullDetails, covered);
    }

} // namespace mongo
