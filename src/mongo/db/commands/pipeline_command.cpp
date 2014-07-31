/**
 * Copyright (c) 2011-2014 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects for
 * all of the code used other than as permitted herein. If you modify file(s)
 * with this exception, you may extend this exception to your version of the
 * file(s), but you are not obligated to do so. If you do not wish to do so,
 * delete this exception statement from your version. If you delete this
 * exception statement from all source files in the program, then also delete
 * it in the license file.
 */

#include "mongo/pch.h"

#include <boost/smart_ptr.hpp>
#include <vector>

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/commands.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/pipeline/accumulator.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/pipeline_d.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/find_constants.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/storage_options.h"

namespace mongo {

namespace {

    /**
     * Stage for pulling results out from an aggregation pipeline.
     *
     * XXX: move this stage to the exec/ directory.
     */
    class PipelineProxyStage : public PlanStage {
    public:
        PipelineProxyStage(intrusive_ptr<Pipeline> pipeline,
                           const boost::shared_ptr<PlanExecutor>& child,
                           WorkingSet* ws)
            : _pipeline(pipeline)
            , _includeMetaData(_pipeline->getContext()->inShard) // send metadata to merger
            , _childExec(child)
            , _ws(ws)
        {}

        virtual StageState work(WorkingSetID* out) {
            if (!out) {
                return PlanStage::FAILURE;
            }

            if (!_stash.empty()) {
                *out = _ws->allocate();
                WorkingSetMember* member = _ws->get(*out);
                member->obj = _stash.back();
                _stash.pop_back();
                member->state = WorkingSetMember::OWNED_OBJ;
                return PlanStage::ADVANCED;
            }

            if (boost::optional<BSONObj> next = getNextBson()) {
                *out = _ws->allocate();
                WorkingSetMember* member = _ws->get(*out);
                member->obj = *next;
                member->state = WorkingSetMember::OWNED_OBJ;
                return PlanStage::ADVANCED;
            }

            return PlanStage::IS_EOF;
        }

        virtual bool isEOF() {
            if (!_stash.empty())
                return false;

            if (boost::optional<BSONObj> next = getNextBson()) {
                _stash.push_back(*next);
                return false;
            }

            return true;
        }

        // propagate to child executor if still in use
        virtual void invalidate(const DiskLoc& dl, InvalidationType type) {
            if (boost::shared_ptr<PlanExecutor> exec = _childExec.lock()) {
                exec->invalidate(dl, type);
            }
        }

        // Manage our OperationContext. We intentionally don't propagate to the child
        // Runner as that is handled by DocumentSourceCursor as it needs to.
        virtual void saveState() {
            _pipeline->getContext()->opCtx = NULL;
        }
        virtual void restoreState(OperationContext* opCtx) {
            _pipeline->getContext()->opCtx = opCtx;
        }

        /**
         * Make obj the next object returned by getNext().
         */
        void pushBack(const BSONObj& obj) {
            _stash.push_back(obj);
        }

        //
        // These should not be used.
        //

        virtual PlanStageStats* getStats() { return NULL; }
        virtual CommonStats* getCommonStats() { return NULL; }
        virtual SpecificStats* getSpecificStats() { return NULL; }

        // Not used.
        virtual std::vector<PlanStage*> getChildren() const {
            vector<PlanStage*> empty;
            return empty;
        }

        // Not used.
        virtual StageType stageType() const { return STAGE_PIPELINE_PROXY; }

    private:
        boost::optional<BSONObj> getNextBson() {
            if (boost::optional<Document> next = _pipeline->output()->getNext()) {
                if (_includeMetaData) {
                    return next->toBsonWithMetaData();
                }
                else {
                    return next->toBson();
                }
            }

            return boost::none;
        }

        // Things in the _stash sould be returned before pulling items from _pipeline.
        const intrusive_ptr<Pipeline> _pipeline;
        vector<BSONObj> _stash;
        const bool _includeMetaData;
        boost::weak_ptr<PlanExecutor> _childExec;

        // Not owned by us.
        WorkingSet* _ws;
    };
}

    static bool isCursorCommand(BSONObj cmdObj) {
        BSONElement cursorElem = cmdObj["cursor"];
        if (cursorElem.eoo())
            return false;

        uassert(16954, "cursor field must be missing or an object",
                cursorElem.type() == Object);

        BSONObj cursor = cursorElem.embeddedObject();
        BSONElement batchSizeElem = cursor["batchSize"];
        if (batchSizeElem.eoo()) {
            uassert(16955, "cursor object can't contain fields other than batchSize",
                cursor.isEmpty());
        }
        else {
            uassert(16956, "cursor.batchSize must be a number",
                    batchSizeElem.isNumber());

            // This can change in the future, but for now all negatives are reserved.
            uassert(16957, "Cursor batchSize must not be negative",
                    batchSizeElem.numberLong() >= 0);
        }

        return true;
    }

    static void handleCursorCommand(OperationContext* txn,
                                    const string& ns,
                                    ClientCursorPin* pin,
                                    PlanExecutor* exec,
                                    const BSONObj& cmdObj,
                                    BSONObjBuilder& result) {

        ClientCursor* cursor = pin ? pin->c() : NULL;
        if (pin) {
            invariant(cursor);
            invariant(cursor->getExecutor() == exec);
            invariant(cursor->isAggCursor);
        }

        BSONElement batchSizeElem = cmdObj.getFieldDotted("cursor.batchSize");
        const long long batchSize = batchSizeElem.isNumber()
                                    ? batchSizeElem.numberLong()
                                    : 101; // same as query

        // can't use result BSONObjBuilder directly since it won't handle exceptions correctly.
        BSONArrayBuilder resultsArray;
        const int byteLimit = MaxBytesToReturnToClientAtOnce;
        BSONObj next;
        for (int objCount = 0; objCount < batchSize; objCount++) {
            // The initial getNext() on a PipelineProxyStage may be very expensive so we don't
            // do it when batchSize is 0 since that indicates a desire for a fast return.
            if (exec->getNext(&next, NULL) != PlanExecutor::ADVANCED) {
                if (pin) pin->deleteUnderlying();
                // make it an obvious error to use cursor or executor after this point
                cursor = NULL;
                exec = NULL;
                break;
            }

            if (resultsArray.len() + next.objsize() > byteLimit) {
                // Get the pipeline proxy stage wrapped by this PlanExecutor.
                PipelineProxyStage* proxy = static_cast<PipelineProxyStage*>(exec->getRootStage());
                // too big. next will be the first doc in the second batch
                proxy->pushBack(next);
                break;
            }

            resultsArray.append(next);
        }

        // NOTE: exec->isEOF() can have side effects such as writing by $out. However, it should
        // be relatively quick since if there was no pin then the input is empty. Also, this
        // violates the contract for batchSize==0. Sharding requires a cursor to be returned in that
        // case. This is ok for now however, since you can't have a sharded collection that doesn't
        // exist.
        const bool canReturnMoreBatches = pin;
        if (!canReturnMoreBatches && exec && !exec->isEOF()) {
            // msgasserting since this shouldn't be possible to trigger from today's aggregation
            // language. The wording assumes that the only reason pin would be null is if the
            // collection doesn't exist.
            msgasserted(17391, str::stream()
                << "Aggregation has more results than fit in initial batch, but can't "
                << "create cursor since collection " << ns << " doesn't exist");
        }

        if (cursor) {
            // If a time limit was set on the pipeline, remaining time is "rolled over" to the
            // cursor (for use by future getmore ops).
            cursor->setLeftoverMaxTimeMicros( txn->getCurOp()->getRemainingMaxTimeMicros() );
        }

        BSONObjBuilder cursorObj(result.subobjStart("cursor"));
        cursorObj.append("id", cursor ? cursor->cursorid() : 0LL);
        cursorObj.append("ns", ns);
        cursorObj.append("firstBatch", resultsArray.arr());
        cursorObj.done();
    }


    class PipelineCommand :
        public Command {
    public:
        PipelineCommand() :Command(Pipeline::commandName) {} // command is called "aggregate"

        // Locks are managed manually, in particular by DocumentSourceCursor.
        virtual bool isWriteCommandForConfigServer() const { return false; }
        virtual bool slaveOk() const { return false; }
        virtual bool slaveOverrideOk() const { return true; }
        virtual void help(stringstream &help) const {
            help << "{ pipeline: [ { $operator: {...}}, ... ]"
                 << ", explain: <bool>"
                 << ", allowDiskUse: <bool>"
                 << ", cursor: {batchSize: <number>}"
                 << " }"
                 << endl
                 << "See http://dochub.mongodb.org/core/aggregation for more details."
                 ;
        }

        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            Pipeline::addRequiredPrivileges(this, dbname, cmdObj, out);
        }

        virtual bool run(OperationContext* txn, const string &db, BSONObj &cmdObj, int options, string &errmsg,
                         BSONObjBuilder &result, bool fromRepl) {

            string ns = parseNs(db, cmdObj);

            intrusive_ptr<ExpressionContext> pCtx = new ExpressionContext(txn, NamespaceString(ns));
            pCtx->tempDir = storageGlobalParams.dbpath + "/_tmp";

            /* try to parse the command; if this fails, then we didn't run */
            intrusive_ptr<Pipeline> pPipeline = Pipeline::parseCommand(errmsg, cmdObj, pCtx);
            if (!pPipeline.get())
                return false;

#if _DEBUG
            // This is outside of the if block to keep the object alive until the pipeline is finished.
            BSONObj parsed;
            if (!pPipeline->isExplain() && !pCtx->inShard) {
                // Make sure all operations round-trip through Pipeline::toBson()
                // correctly by reparsing every command on DEBUG builds. This is
                // important because sharded aggregations rely on this ability.
                // Skipping when inShard because this has already been through the
                // transformation (and this unsets pCtx->inShard).
                parsed = pPipeline->serialize().toBson();
                pPipeline = Pipeline::parseCommand(errmsg, parsed, pCtx);
                verify(pPipeline);
            }
#endif

            PlanExecutor* exec = NULL;
            scoped_ptr<ClientCursorPin> pin; // either this OR the execHolder will be non-null
            auto_ptr<PlanExecutor> execHolder;
            {
                // This will throw if the sharding version for this connection is out of date. The
                // lock must be held continuously from now until we have we created both the output
                // ClientCursor and the input executor. This ensures that both are using the same
                // sharding version that we synchronize on here. This is also why we always need to
                // create a ClientCursor even when we aren't outputting to a cursor. See the comment
                // on ShardFilterStage for more details.
                Client::ReadContext ctx(txn, ns);

                Collection* collection = ctx.ctx().db()->getCollection(txn, ns);

                // This does mongod-specific stuff like creating the input PlanExecutor and adding
                // it to the front of the pipeline if needed.
                boost::shared_ptr<PlanExecutor> input = PipelineD::prepareCursorSource(txn,
                                                                                       collection,
                                                                                       pPipeline,
                                                                                       pCtx);
                pPipeline->stitch();

                // Create the PlanExecutor which returns results from the pipeline. The WorkingSet
                // ('ws') and the PipelineProxyStage ('proxy') will be owned by the created
                // PlanExecutor.
                auto_ptr<WorkingSet> ws(new WorkingSet());
                auto_ptr<PipelineProxyStage> proxy(
                    new PipelineProxyStage(pPipeline, input, ws.get()));
                if (NULL == collection) {
                    execHolder.reset(new PlanExecutor(ws.release(), proxy.release(), ns));
                }
                else {
                    execHolder.reset(new PlanExecutor(ws.release(), proxy.release(), collection));
                }
                exec = execHolder.get();

                if (!collection && input) {
                    // If we don't have a collection, we won't be able to register any executors, so
                    // make sure that the input PlanExecutor (likely wrapping an EOFStage) doesn't
                    // need to be registered.
                    invariant(!input->collection());
                }

                if (collection) {
                    ClientCursor* cursor = new ClientCursor(collection, execHolder.release());
                    cursor->isAggCursor = true; // enable special locking behavior
                    pin.reset(new ClientCursorPin(collection, cursor->cursorid()));
                    // Don't add any code between here and the start of the try block.
                }
            }

            try {
                // Unless set to true, the ClientCursor created above will be deleted on block exit.
                bool keepCursor = false;

                // If both explain and cursor are specified, explain wins.
                if (pPipeline->isExplain()) {
                    result << "stages" << Value(pPipeline->writeExplainOps());
                }
                else if (isCursorCommand(cmdObj)) {
                    handleCursorCommand(txn, ns, pin.get(), exec, cmdObj, result);
                    keepCursor = true;
                }
                else {
                    pPipeline->run(result);
                }

                if (!keepCursor && pin) pin->deleteUnderlying();
            }
            catch (...) {
                // Clean up cursor on way out of scope.
                if (pin) pin->deleteUnderlying();
                throw;
            }
            // Any code that needs the cursor pinned must be inside the try block, above.

            return true;
        }
    } cmdPipeline;

} // namespace mongo
