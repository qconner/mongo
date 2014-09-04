// @file keypattern.h - Utilities for manipulating index/shard key patterns.

/**
*    Copyright (C) 2012 10gen Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/unordered_set.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/db/query/index_bounds.h"
#include "mongo/db/matcher/matchable.h"

namespace mongo {

    /**
     * A BoundList contains intervals specified by inclusive start
     * and end bounds.  The intervals should be nonoverlapping and occur in
     * the specified direction of traversal.  For example, given a simple index {i:1}
     * and direction +1, one valid BoundList is: (1, 2); (4, 6).  The same BoundList
     * would be valid for index {i:-1} with direction -1.
     */
    typedef std::vector<std::pair<BSONObj,BSONObj> > BoundList;

    /** A KeyPattern is an expression describing a transformation of a document into a
     *  document key.  Document keys are used to store documents in indices and to target
     *  sharded queries.
     *
     *  Examples:
     *    { a : 1 }
     *    { a : 1 , b  : -1 }
     *    { a : "hashed" }
     */
    class KeyPattern {
    public:

        //maximum number of intervals produced by $in queries.
        static const unsigned MAX_IN_COMBINATIONS = 4000000;

        /*
         * We are allowing implicit conversion from BSON
         */
        KeyPattern( const BSONObj& pattern );

        /*
         *  Returns a BSON representation of this KeyPattern.
         */
        BSONObj toBSON() const { return _pattern; }

        /**
         * Is the provided key pattern the index over the ID field?
         * The always required ID index is always {_id: 1} or {_id: -1}.
         */
        static bool isIdKeyPattern(const BSONObj& pattern);

        /**
         * Is the provided key pattern ordered increasing or decreasing or not?
         */
        static bool isOrderedKeyPattern(const BSONObj& pattern);

        /* Takes a BSONObj whose field names are a prefix of the fields in this keyPattern, and
         * outputs a new bound with MinKey values appended to match the fields in this keyPattern
         * (or MaxKey values for descending -1 fields). This is useful in sharding for
         * calculating chunk boundaries when tag ranges are specified on a prefix of the actual
         * shard key, or for calculating index bounds when the shard key is a prefix of the actual
         * index used.
         *
         * @param makeUpperInclusive If true, then MaxKeys instead of MinKeys will be appended, so
         * that the output bound will compare *greater* than the bound being extended (note that
         * -1's in the keyPattern will swap MinKey/MaxKey vals. See examples).
         *
         * Examples:
         * If this keyPattern is {a : 1}
         *   extendRangeBound( {a : 55}, false) --> {a : 55}
         *
         * If this keyPattern is {a : 1, b : 1}
         *   extendRangeBound( {a : 55}, false) --> {a : 55, b : MinKey}
         *   extendRangeBound( {a : 55}, true ) --> {a : 55, b : MaxKey}
         *
         * If this keyPattern is {a : 1, b : -1}
         *   extendRangeBound( {a : 55}, false) --> {a : 55, b : MaxKey}
         *   extendRangeBound( {a : 55}, true ) --> {a : 55, b : MinKey}
         */
        BSONObj extendRangeBound( const BSONObj& bound , bool makeUpperInclusive ) const;

        std::string toString() const{ return toBSON().toString(); }

        /**
         * Given a document, extracts the shard key corresponding to the key pattern.
         * Warning: assumes that there is a *single* key to be extracted!
         *
         * Examples:
         *  If 'this' KeyPattern is { a  : 1 }
         *   { a: "hi" , b : 4} --> returns { a : "hi" }
         *   { c : 4 , a : 2 } -->  returns { a : 2 }
         *   { b : 2 }  (bad input, don't call with this)
         *   { a : [1,2] }  (bad input, don't call with this)
         *  If 'this' KeyPattern is { a  : "hashed" }
         *   { a: 1 } --> returns { a : NumberLong("5902408780260971510")  }
         *  If 'this' KeyPattern is { 'a.b' : 1 }
         *   { a : { b : "hi" } } --> returns { a : "hi" }
         */
        BSONObj extractShardKeyFromDoc(const BSONObj& doc) const;

        /**
         * Given a MatchableDocument, extracts the shard key corresponding to the key pattern.
         * See above.
         */
        BSONObj extractShardKeyFromMatchable(const MatchableDocument& matchable) const;

        /**
         * Given a query expression, extracts the shard key corresponding to the key pattern.
         *
         * NOTE: This generally is similar to the above, however "a.b" fields in the query (which
         * are invalid document fields) may match "a.b" fields in the shard key pattern.
         *
         * Examples:
         *  If the key pattern is { a : 1 }
         *   { a : "hi", b : 4 } --> returns { a : "hi" }
         *  If the key pattern is { 'a.b' : 1 }
         *   { a : { b : "hi" } } --> returns { 'a.b' : "hi" }
         *   { 'a.b' : "hi" } --> returns { 'a.b' : "hi" }
         */
        BSONObj extractShardKeyFromQuery(const BSONObj& query) const;

        /**
         * Return an ordered list of bounds generated using this KeyPattern and the
         * bounds from the IndexBounds.  This function is used in sharding to
         * determine where to route queries according to the shard key pattern.
         *
         * Examples:
         *
         * Key { a: 1 }, Bounds a: [0] => { a: 0 } -> { a: 0 }
         * Key { a: 1 }, Bounds a: [2, 3) => { a: 2 } -> { a: 3 }  // bound inclusion ignored.
         *
         * The bounds returned by this function may be a superset of those defined
         * by the constraints.  For instance, if this KeyPattern is {a : 1, b: 1}
         * Bounds: { a : {$in : [1,2]} , b : {$in : [3,4,5]} }
         *         => {a : 1 , b : 3} -> {a : 1 , b : 5}, {a : 2 , b : 3} -> {a : 2 , b : 5}
         *
         * If the IndexBounds are not defined for all the fields in this keypattern, which
         * means some fields are unsatisfied, an empty BoundList could return.
         *
         */
        static BoundList flattenBounds( const BSONObj& keyPattern, const IndexBounds& indexBounds );

    private:
        BSONObj _pattern;
    };

} // namespace mongo
