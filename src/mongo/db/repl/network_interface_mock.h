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

#include <boost/thread/mutex.hpp>
#include <boost/thread/condition_variable.hpp>
#include <map>

#include "mongo/db/repl/replication_executor.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace repl {

    /**
     * Mock (replication) network implementation which can do the following:
     * -- Simulate latency (delay) on each request
     * -- Allows replacing the helper function used to return a response
     */
    class NetworkInterfaceMock : public ReplicationExecutor::NetworkInterface {
    public:
        typedef stdx::function< ResponseStatus (const ReplicationExecutor::RemoteCommandRequest&)>
                                                                                CommandProcessorFn;
        NetworkInterfaceMock();
        explicit NetworkInterfaceMock(CommandProcessorFn fn);
        virtual ~NetworkInterfaceMock();
        virtual void setExecutor(ReplicationExecutor* executor);
        virtual Date_t now();
        virtual ResponseStatus runCommand(const ReplicationExecutor::RemoteCommandRequest& request);
        virtual void runCallbackWithGlobalExclusiveLock(
                const stdx::function<void (OperationContext*)>& callback);

        /**
         * Network latency added for each remote command, defaults to 0.
         */
        void simulatedNetworkLatency(int millis);

        /**
         * Sets the current time to "newNow".  It is a fatal error for "newNow"
         * to be less than or equal to "now()".
         */
        void setNow(Date_t newNow);

        /**
         * Increments the current time by "inc".  It is a fatal error for "inc"
         * to be negative or 0.
         */
        void incrementNow(Milliseconds inc);

    protected:
        // Mutex that synchronizes access to mutable data in this class and its subclasses.
        // Fields guarded by the mutex are labled (M), below, and those that are read-only
        // in multi-threaded execution, and so unsynchronized, are labeled (R).
        boost::mutex _mutex;

    private:
        // Condition signaled when _now is updated.
        boost::condition_variable _timeElapsed;  // (M)

        // The current time reported by this instance of NetworkInterfaceMock.
        Date_t _now;                             // (M)

        // The amount of simulated network delay to introduce on all runCommand
        // operations.
        int _simulatedNetworkLatencyMillis;      // (M)

        // Function that generates the response from a request in runCommand.
        const CommandProcessorFn _helper;        // (R)

        // Pointer to the executor into which this mock is installed.  Used to signal the executor
        // when the clock changes.
        ReplicationExecutor* _executor;          // (R)
    };

    /**
     * Mock (replication) network implementation which can do the following:
     * -- Holds a map from request -> response
     * -- Block on each request, and unblock by request or all
     */
    class NetworkInterfaceMockWithMap : public NetworkInterfaceMock {
    public:

        NetworkInterfaceMockWithMap();

        /**
         * Adds a response (StatusWith<BSONObj>) for this mock to return for a given request.
         * For each request, the mock will return the corresponding response for all future calls.
         *
         * If "isBlocked" is set to true, the network will block in runCommand for the given
         * request until unblockResponse is called with "request" as an argument,
         * or unblockAll is called.
         */
        bool addResponse(const ReplicationExecutor::RemoteCommandRequest& request,
                         const StatusWith<BSONObj>& response,
                         bool isBlocked = false);

        /**
         * Unblocks response to "request" that was blocked when it was added.
         */
        void unblockResponse(const ReplicationExecutor::RemoteCommandRequest& request);

        /**
         * Unblocks all responses that were blocked when added.
         */
        void unblockAll();

    private:
        struct BlockableResponseStatus {
            BlockableResponseStatus(const ResponseStatus& r,
                                    bool blocked);

            ResponseStatus response;
            bool isBlocked;

            std::string toString() const;
        };

        typedef std::map<ReplicationExecutor::RemoteCommandRequest, BlockableResponseStatus>
                                                                                RequestResponseMap;

        // This helper will return a response from the mapped responses
        ResponseStatus _getResponseFromMap(
                                         const ReplicationExecutor::RemoteCommandRequest& request);

        // Condition signaled whenever any response is unblocked.
        boost::condition_variable _someResponseUnblocked;  // (M)

        // Map from requests to responses.
        RequestResponseMap _responses;                     // (M)
    };

}  // namespace repl
}  // namespace mongo
