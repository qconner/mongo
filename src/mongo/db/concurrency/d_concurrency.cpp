/**
 *    Copyright (C) 2008-2014 MongoDB Inc.
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

#include "mongo/db/concurrency/d_concurrency.h"

#include <string>

#include "mongo/db/concurrency/locker.h"
#include "mongo/db/global_environment_experiment.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/server_parameters.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/stacktrace.h"

// oplog locking
// no top level read locks
// system.profile writing
// oplog now
// yielding

namespace mongo {

    //  SERVER-14668: Remove or invert sense once MMAPv1 CLL can be default
    MONGO_EXPORT_STARTUP_SERVER_PARAMETER(enableCollectionLocking, bool, true);


    DBTryLockTimeoutException::DBTryLockTimeoutException() {}
    DBTryLockTimeoutException::~DBTryLockTimeoutException() throw() { }

    class AcquiringParallelWriter {
    public:

        AcquiringParallelWriter(Locker* ls)
            : _ls(ls) {

            _ls->setLockPendingParallelWriter(true);
        }

        ~AcquiringParallelWriter() {
            _ls->setLockPendingParallelWriter(false);
        }

    private:
        Locker* const _ls;
    };


    RWLockRecursive &Lock::ParallelBatchWriterMode::_batchLock = *(new RWLockRecursive("special"));
    void Lock::ParallelBatchWriterMode::iAmABatchParticipant(Locker* lockState) {
        lockState->setIsBatchWriter(true);
    }


    Lock::ScopedLock::ScopedLock(Locker* lockState)
        : _lockState(lockState) {

        if (!_lockState->isBatchWriter()) {
            AcquiringParallelWriter a(_lockState);
            _pbws_lk.reset(new RWLockRecursive::Shared(ParallelBatchWriterMode::_batchLock));
        }
    }


    Lock::TempRelease::TempRelease(Locker* lockState)
        : _lockState(lockState),
          _lockSnapshot(),
          _locksReleased(_lockState->saveLockStateAndUnlock(&_lockSnapshot)) {

    }

    Lock::TempRelease::~TempRelease() {
        if (_locksReleased) {
            invariant(!_lockState->isLocked());
            _lockState->restoreLockState(_lockSnapshot);
        }
    }


    Lock::GlobalWrite::GlobalWrite(Locker* lockState, unsigned timeoutms)
        : ScopedLock(lockState) {

        LockResult result = _lockState->lockGlobal(MODE_X, timeoutms);
        if (result == LOCK_TIMEOUT) {
            throw DBTryLockTimeoutException();
        }
    }

    Lock::GlobalWrite::~GlobalWrite() {
        // If the lock state is R, this means downgrade happened and this is only for fsyncLock.
        invariant(_lockState->isW() || _lockState->isR());

        _lockState->unlockAll();
    }


    Lock::GlobalRead::GlobalRead(Locker* lockState, unsigned timeoutms)
        : ScopedLock(lockState) {

        LockResult result = _lockState->lockGlobal(MODE_S, timeoutms);
        if (result == LOCK_TIMEOUT) {
            throw DBTryLockTimeoutException();
        }
    }

    Lock::GlobalRead::~GlobalRead() {
        _lockState->unlockAll();
    }


    Lock::DBLock::DBLock(Locker* lockState, const StringData& db, LockMode mode)
        : ScopedLock(lockState),
          _id(RESOURCE_DATABASE, db),
          _lockState(lockState),
          _mode(mode) {

        massert(28539, "need a valid database name", !db.empty() && nsIsDbOnly(db));

        const bool isRead = (_mode == MODE_S || _mode == MODE_IS);

        _lockState->lockGlobal(isRead ? MODE_IS : MODE_IX);
        if (supportsDocLocking() || enableCollectionLocking) {
            _lockState->lock(_id, _mode);
        }
        else {
            _lockState->lock(_id, isRead ? MODE_S : MODE_X);
        }
    }

    Lock::DBLock::~DBLock() {
        _lockState->unlock(_id);

        _lockState->unlockAll();
    }

    void Lock::DBLock::relockWithMode(const LockMode newMode) {
        const bool wasRead = (_mode == MODE_S || _mode == MODE_IS);
        const bool isRead = (newMode == MODE_S || newMode == MODE_IS);

        invariant (!_lockState->inAWriteUnitOfWork()); // 2PL would delay the unlocking
        invariant(!wasRead || isRead); // Not allowed to change global intent

        _lockState->unlock(_id);
        _mode = newMode;

        if (supportsDocLocking() || enableCollectionLocking) {
            _lockState->lock(_id, _mode);
            dassert(_lockState->isLockHeldForMode(_id, _mode));
        }
        else {
            LockMode effectiveMode = isRead ? MODE_S : MODE_X;
            _lockState->lock(_id, effectiveMode);
            dassert(_lockState->isLockHeldForMode(_id, effectiveMode));
        }
    }


    Lock::CollectionLock::CollectionLock(Locker* lockState,
                                         const StringData& ns,
                                         LockMode mode)
        : _id(RESOURCE_COLLECTION, ns),
          _lockState(lockState) {
        const bool isRead = (mode == MODE_S || mode == MODE_IS);
        massert(28538, "need a non-empty collection name", nsIsFull(ns));
        dassert(_lockState->isDbLockedForMode(nsToDatabaseSubstring(ns),
                                              isRead ? MODE_IS : MODE_IX));
        if (supportsDocLocking()) {
            _lockState->lock(_id, mode);
        } else if (enableCollectionLocking) {
            _lockState->lock(_id, isRead ? MODE_S : MODE_X);
        }
    }

    Lock::CollectionLock::~CollectionLock() {
        if (supportsDocLocking() || enableCollectionLocking) {
            _lockState->unlock(_id);
        }
    }

    void Lock::CollectionLock::relockWithMode(LockMode mode, Lock::DBLock& dbLock ) {
        if (supportsDocLocking() || enableCollectionLocking) {
            _lockState->unlock(_id);
        }

        dbLock.relockWithMode( mode );

        if (supportsDocLocking() || enableCollectionLocking) {
            _lockState->lock(_id, mode);
        }
    }

namespace {
    boost::mutex oplogSerialization; // for OplogIntentWriteLock
} // namespace

    Lock::OplogIntentWriteLock::OplogIntentWriteLock(Locker* lockState)
          : _lockState(lockState),
            _serialized(false) {
        _lockState->lock(resourceIdOplog, MODE_IX);
    }

    Lock::OplogIntentWriteLock::~OplogIntentWriteLock() {
        if (_serialized) {
            oplogSerialization.unlock();
        }
        _lockState->unlock(resourceIdOplog);
    }

    void Lock::OplogIntentWriteLock::serializeIfNeeded() {
        if (!supportsDocLocking() && !_serialized) {
            oplogSerialization.lock();
            _serialized = true;
        }
    }

    Lock::ResourceLock::ResourceLock(Locker* lockState, ResourceId rid, LockMode mode)
            : _rid(rid),
              _lockState(lockState) {
        _lockState->lock(_rid, mode);
    }

    Lock::ResourceLock::~ResourceLock() {
        _lockState->unlock(_rid);
    }


    writelocktry::writelocktry(Locker* lockState, int tryms) :
        _got( false ),
        _dbwlock( NULL )
    { 
        try { 
            _dbwlock.reset(new Lock::GlobalWrite(lockState, tryms));
        }
        catch ( DBTryLockTimeoutException & ) {
            return;
        }
        _got = true;
    }

    writelocktry::~writelocktry() {

    }


    // note: the 'already' concept here might be a bad idea as a temprelease wouldn't notice it is nested then
    readlocktry::readlocktry(Locker* lockState, int tryms) :
        _got( false ),
        _dbrlock( NULL )
    {
        try { 
            _dbrlock.reset(new Lock::GlobalRead(lockState, tryms));
        }
        catch ( DBTryLockTimeoutException & ) {
            return;
        }
        _got = true;
    }

    readlocktry::~readlocktry() {

    }
}
