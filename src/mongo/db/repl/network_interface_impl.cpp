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

#include "mongo/db/repl/network_interface_impl.h"

#include "mongo/client/connpool.h"
#include "mongo/db/d_concurrency.h"
#include "mongo/db/lockstate.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace repl {

    NetworkInterfaceImpl::NetworkInterfaceImpl() {}
    NetworkInterfaceImpl::~NetworkInterfaceImpl() {}

    Date_t NetworkInterfaceImpl::now() {
        return curTimeMillis64();
    }

    namespace {
        // Duplicated in mock impl
        StatusWith<int> getTimeoutMillis(Date_t expDate) {
            // check for timeout
            int timeout = 0;
            if (expDate != ReplicationExecutor::kNoExpirationDate) {
                Date_t nowDate = curTimeMillis64();
                timeout = expDate >= nowDate ? expDate - nowDate :
                                               ReplicationExecutor::kNoTimeout.total_milliseconds();
                if (timeout < 0 ) {
                    return StatusWith<int>(ErrorCodes::ExceededTimeLimit,
                                               str::stream() << "Went to run command,"
                                               " but it was too late. Expiration was set to "
                                                             << expDate);
                }
            }
            return StatusWith<int>(timeout);
        }
    } //namespace

    StatusWith<BSONObj> NetworkInterfaceImpl::runCommand(
            const ReplicationExecutor::RemoteCommandRequest& request) {

        try {
            BSONObj output;

            StatusWith<int> timeoutStatus = getTimeoutMillis(request.expirationDate);
            if (!timeoutStatus.isOK())
                return StatusWith<BSONObj>(timeoutStatus.getStatus());

            int timeout = timeoutStatus.getValue();
            ScopedDbConnection conn(request.target.toString(), timeout);
            conn->runCommand(request.dbname, request.cmdObj, output);
            conn.done();
            return StatusWith<BSONObj>(output);
        }
        catch (const DBException& ex) {
            return StatusWith<BSONObj>(ex.toStatus());
        }
        catch (const std::exception& ex) {
            return StatusWith<BSONObj>(
                    ErrorCodes::UnknownError,
                    mongoutils::str::stream() <<
                    "Sending command " << request.cmdObj << " on database " << request.dbname <<
                    " over network to " << request.target.toString() << " received exception " <<
                    ex.what());
        }
    }

    void NetworkInterfaceImpl::runCallbackWithGlobalExclusiveLock(
            const stdx::function<void ()>& callback) {

        LockState lockState;
        Lock::GlobalWrite lk(&lockState);
        callback();
    }

}  // namespace repl
} // namespace mongo
