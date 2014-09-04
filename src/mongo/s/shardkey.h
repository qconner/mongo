// shardkey.h

/**
*    Copyright (C) 2008 10gen Inc.
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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/db/keypattern.h"
#include "mongo/s/shard_key_pattern.h"

namespace mongo {

    /**
     * THIS FUNCTIONALITY IS DEPRECATED
     * Everything BSON related in this file should migrate gradually to s/shard_key_pattern.h, new
     * functionality should not go here.
     */

    class Chunk;
    class FieldRangeSet;

    /* A ShardKeyPattern is a pattern indicating what data to extract from the object to make the shard key from.
       Analogous to an index key pattern.
    */
    class ShardKeyPattern {
    public:
        ShardKeyPattern( BSONObj p = BSONObj() );

        /**
           global min is the lowest possible value for this key
           e.g. { num : MinKey }
         */
        BSONObj globalMin() const { return gMin; }

        /**
           global max is the highest possible value for this key
         */
        BSONObj globalMax() const { return gMax; }

        /**
           @return whether or not obj has all fields in this shard key pattern
           e.g.
             ShardKey({num:1}).hasShardKey({ name:"joe", num:3 }) is true
             ShardKey({"a.b":1}).hasShardKey({ "a.b":"joe"}) is true
             ShardKey({"a.b":1}).hasShardKey({ "a": {"b":"joe"}}) is true

             ShardKey({num:1}).hasShardKey({ name:"joe"}) is false
             ShardKey({num:1}).hasShardKey({ name:"joe", num:{$gt:3} }) is false

             see unit test for more examples
         */
        bool hasShardKey( const BSONObj& doc ) const;

        /**
         * Same as the above, but disallow certain shard key values which are interpreted for
         * targeting as a multi-shard query (i.e. RegExes)
         */
        bool hasTargetableShardKey( const BSONObj& doc ) const;

        BSONObj key() const { return pattern.toBSON(); }

        std::string toString() const;

        /**
         * DEPRECATED function to return a shard key from either a document or a query expression.
         * Always prefer the more specific keypattern.h extractKeyFromXXX functions instead.
         * TODO: Eliminate completely.
         */
        BSONObj extractKeyFromQueryOrDoc(const BSONObj& from) const;

        BSONObj extendRangeBound( const BSONObj& bound , bool makeUpperInclusive ) const {
            return pattern.extendRangeBound( bound , makeUpperInclusive );
        }

        /**
         * @return
         * true if this shard key is compatible with a unique index on 'uniqueIndexPattern'.
         *      Primarily this just checks whether 'this' is a prefix of 'uniqueIndexPattern',
         *      However it does not need to be an exact syntactic prefix due to "hashed"
         *      indexes or mismatches in ascending/descending order.  Also, uniqueness of the
         *      _id field is guaranteed by the generation process (or by the user) so every
         *      index that begins with _id is unique index compatible with any shard key.
         *      Examples:
         *        shard key {a : 1} is compatible with a unique index on {_id : 1}
         *        shard key {a : 1} is compatible with a unique index on {a : 1 , b : 1}
         *        shard key {a : 1} is compatible with a unique index on {a : -1 , b : 1 }
         *        shard key {a : "hashed"} is compatible with a unique index on {a : 1}
         *        shard key {a : 1} is not compatible with a unique index on {b : 1}
         *        shard key {a : "hashed" , b : 1 } is not compatible with unique index on { b : 1 }
         *      Note:
         *        this method assumes that 'uniqueIndexPattern' is a valid index pattern,
         *        and is capable of being a unique index.  A pattern like { k : "hashed" }
         *        is never capable of being a unique index, and thus is an invalid setting
         *        for the 'uniqueIndexPattern' argument.
         */
        bool isUniqueIndexCompatible( const KeyPattern& uniqueIndexPattern ) const;

    private:
        KeyPattern pattern;
        BSONObj gMin;
        BSONObj gMax;

        /* question: better to have patternfields precomputed or not?  depends on if we use copy constructor often. */
        std::set<std::string> patternfields;
    };

    // See note above - do not use in new code
    inline BSONObj ShardKeyPattern::extractKeyFromQueryOrDoc(const BSONObj& from) const {
        BSONObj k = pattern.extractShardKeyFromQuery( from );
        uassert(13334, "Shard Key must be less than 512 bytes", k.objsize() < kMaxShardKeySize);
        return k;
    }

}
