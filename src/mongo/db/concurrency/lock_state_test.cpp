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

#include <boost/scoped_ptr.hpp>
#include <boost/thread/thread.hpp>

#include "mongo/unittest/unittest.h"
#include "mongo/db/concurrency/lock_mgr.h"
#include "mongo/db/concurrency/lock_state.h"


namespace mongo {
namespace newlm {

    class TrackingLockGrantNotification : public LockGrantNotification {
    public:
        TrackingLockGrantNotification() : numNotifies(0), lastResult(LOCK_INVALID) {

        }

        virtual void notify(const ResourceId& resId, LockResult result) {
            numNotifies++;
            lastResId = resId;
            lastResult = result;
        }

    public:
        int numNotifies;

        ResourceId lastResId;
        LockResult lastResult;
    };

    
    TEST(Locker, BasicLockNoConflict) {
        Locker locker(1);
        TrackingLockGrantNotification notify;

        const ResourceId resId(RESOURCE_COLLECTION, std::string("TestDB.collection"));

        ASSERT(LOCK_OK == locker.lockExtended(resId, MODE_X, &notify));
        ASSERT(locker.isLockHeldForMode(resId, MODE_X));
        ASSERT(locker.isLockHeldForMode(resId, MODE_S));

        locker.unlock(resId);

        ASSERT(!locker.isLockHeldForMode(resId, MODE_NONE));
    }

    // Randomly acquires and releases locks, just to make sure that no assertions pop-up
    TEST(Locker, RandomizedAcquireRelease) {
        // TODO: Make sure to print the seed
    }

} // namespace newlm
} // namespace mongo
