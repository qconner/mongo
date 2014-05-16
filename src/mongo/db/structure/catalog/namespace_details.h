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
#include "mongo/db/namespace_string.h"
#include "mongo/db/structure/catalog/index_details.h"
#include "mongo/db/structure/catalog/namespace.h"
#include "mongo/db/structure/catalog/namespace_index.h"

namespace mongo {

    class Collection;
    class OperationContext;

    /* deleted lists -- linked lists of deleted records -- are placed in 'buckets' of various sizes
       so you can look for a deleterecord about the right size.
    */
    const int Buckets = 19;
    const int MaxBucket = 18;

    extern int bucketSizes[];

#pragma pack(1)
    /* NamespaceDetails : this is the "header" for a collection that has all its details.
       It's in the .ns file and this is a memory mapped region (thus the pack pragma above).
    */
    class NamespaceDetails {
    public:
        enum { NIndexesMax = 64, NIndexesExtra = 30, NIndexesBase  = 10 };

    private:

        /*-------- data fields, as present on disk : */

        DiskLoc _firstExtent;
        DiskLoc _lastExtent;

        /* NOTE: capped collections v1 override the meaning of deletedList.
                 deletedList[0] points to a list of free records (DeletedRecord's) for all extents in
                 the capped namespace.
                 deletedList[1] points to the last record in the prev extent.  When the "current extent"
                 changes, this value is updated.  !deletedList[1].isValid() when this value is not
                 yet computed.
        */
        DiskLoc _deletedList[Buckets];

        // ofs 168 (8 byte aligned)
        struct Stats {
            // datasize and nrecords MUST Be adjacent code assumes!
            long long datasize; // this includes padding, but not record headers
            long long nrecords;
        } _stats;

        int _lastExtentSize;
        int _nIndexes;

        // ofs 192
        IndexDetails _indexes[NIndexesBase];

        // ofs 352 (16 byte aligned)
        int _isCapped;                         // there is wasted space here if I'm right (ERH)
        int _maxDocsInCapped;                  // max # of objects for a capped table, -1 for inf.

        double _paddingFactor;                 // 1.0 = no padding.
        // ofs 368 (16)
        int _systemFlagsOld; // things that the system sets/cares about

        DiskLoc _capExtent; // the "current" extent we're writing too for a capped collection
        DiskLoc _capFirstNewRecord;

        unsigned short _dataFileVersion;       // NamespaceDetails version.  So we can do backward compatibility in the future. See filever.h
        unsigned short _indexFileVersion;
        unsigned long long _multiKeyIndexBits;

        // ofs 400 (16)
        unsigned long long _reservedA;
        long long _extraOffset;               // where the $extra info is located (bytes relative to this)

        int _indexBuildsInProgress;            // Number of indexes currently being built

        int _userFlags;
        char _reserved[72];
        /*-------- end data 496 bytes */
    public:
        explicit NamespaceDetails( const DiskLoc &loc, bool _capped );

        class Extra {
            long long _next;
        public:
            IndexDetails details[NIndexesExtra];
        private:
            unsigned reserved2;
            unsigned reserved3;
            Extra(const Extra&) { verify(false); }
            Extra& operator=(const Extra& r) { verify(false); return *this; }
        public:
            Extra() { }
            long ofsFrom(NamespaceDetails *d) {
                return ((char *) this) - ((char *) d);
            }
            void init() { memset(this, 0, sizeof(Extra)); }
            Extra* next(const NamespaceDetails *d) const {
                if( _next == 0 ) return 0;
                return (Extra*) (((char *) d) + _next);
            }
            void setNext(OperationContext* txn, long ofs);
            void copy(NamespaceDetails *d, const Extra& e) {
                memcpy(this, &e, sizeof(Extra));
                _next = 0;
            }
        };
        Extra* extra() const {
            if( _extraOffset == 0 ) return 0;
            return (Extra *) (((char *) this) + _extraOffset);
        }
        /* add extra space for indexes when more than 10 */
        Extra* allocExtra( OperationContext* txn,
                           const StringData& ns,
                           NamespaceIndex& ni,
                           int nindexessofar );

        void copyingFrom( OperationContext* txn,
                          const char* thisns,
                          NamespaceIndex& ni,
                          NamespaceDetails *src); // must be called when renaming a NS to fix up extra

    public:
        const DiskLoc& capExtent() const { return _capExtent; }
        void setCapExtent( OperationContext* txn, const DiskLoc& loc );

        const DiskLoc& capFirstNewRecord() const { return _capFirstNewRecord; }
        void setCapFirstNewRecord( OperationContext* txn, const DiskLoc& loc );

    public:

        const DiskLoc& firstExtent() const { return _firstExtent; }
        void setFirstExtent( OperationContext* txn, const DiskLoc& loc );

        const DiskLoc& lastExtent() const { return _lastExtent; }
        void setLastExtent( OperationContext* txn, const DiskLoc& loc );

        void setFirstExtentInvalid( OperationContext* txn );
        void setLastExtentInvalid( OperationContext* txn );


        long long dataSize() const { return _stats.datasize; }
        long long numRecords() const { return _stats.nrecords; }

        void incrementStats( OperationContext* txn,
                             long long dataSizeIncrement,
                             long long numRecordsIncrement );

        void setStats( OperationContext* txn,
                       long long dataSizeIncrement,
                       long long numRecordsIncrement );


        bool isCapped() const { return _isCapped; }
        long long maxCappedDocs() const;
        void setMaxCappedDocs( OperationContext* txn, long long max );

        int lastExtentSize() const { return _lastExtentSize; }
        void setLastExtentSize( OperationContext* txn, int newMax );

        const DiskLoc& deletedListEntry( int bucket ) const { return _deletedList[bucket]; }
        void setDeletedListEntry( OperationContext* txn, int bucket, const DiskLoc& loc );

        void orphanDeletedList( OperationContext* txn );

        /**
         * @param max in and out, will be adjusted
         * @return if the value is valid at all
         */
        static bool validMaxCappedDocs( long long* max );

        /* when a background index build is in progress, we don't count the index in nIndexes until
           complete, yet need to still use it in _indexRecord() - thus we use this function for that.
        */
        int getTotalIndexCount() const { return _nIndexes + _indexBuildsInProgress; }

        int getCompletedIndexCount() const { return _nIndexes; }

        int getIndexBuildsInProgress() const { return _indexBuildsInProgress; }

        enum UserFlags {
            Flag_UsePowerOf2Sizes = 1 << 0
        };

        IndexDetails& idx(int idxNo, bool missingExpected = false );
        const IndexDetails& idx(int idxNo, bool missingExpected = false ) const;

        class IndexIterator {
        public:
            int pos() { return i; } // note this is the next one to come
            bool more() { return i < n; }
            const IndexDetails& next() { return d->idx(i++); }
        private:
            friend class NamespaceDetails;
            int i, n;
            const NamespaceDetails *d;
            IndexIterator(const NamespaceDetails *_d, bool includeBackgroundInProgress);
        };

        IndexIterator ii( bool includeBackgroundInProgress = false ) const {
            return IndexIterator(this, includeBackgroundInProgress);
        }

        /* multikey indexes are indexes where there are more than one key in the index
             for a single document. see multikey in docs.
           for these, we have to do some dedup work on queries.
        */
        bool isMultikey(int i) const { return (_multiKeyIndexBits & (((unsigned long long) 1) << i)) != 0; }

        /**
         * @return - if any state was changed
         */
        bool setIndexIsMultikey(OperationContext* txn, int i, bool multikey = true);

        /**
         * This fetches the IndexDetails for the next empty index slot. The caller must populate
         * returned object.  This handles allocating extra index space, if necessary.
         */
        IndexDetails& getNextIndexDetails(OperationContext* txn, Collection* collection);

        double paddingFactor() const { return _paddingFactor; }

        void setPaddingFactor( OperationContext* txn, double paddingFactor );

        /* called to indicate that an update fit in place.  
           fits also called on an insert -- idea there is that if you had some mix and then went to
           pure inserts it would adapt and PF would trend to 1.0.  note update calls insert on a move
           so there is a double count there that must be adjusted for below.

           todo: greater sophistication could be helpful and added later.  for example the absolute 
                 size of documents might be considered -- in some cases smaller ones are more likely 
                 to grow than larger ones in the same collection? (not always)
        */
        void paddingFits( OperationContext* txn ) {
            MONGO_SOMETIMES(sometimes, 4) { // do this on a sampled basis to journal less
                double x = max(1.0, _paddingFactor - 0.001 );
                setPaddingFactor( txn, x );
            }
        }
        void paddingTooSmall( OperationContext* txn ) {
            MONGO_SOMETIMES(sometimes, 4) { // do this on a sampled basis to journal less       
                /* the more indexes we have, the higher the cost of a move.  so we take that into 
                   account herein.  note on a move that insert() calls paddingFits(), thus
                   here for example with no inserts and nIndexes = 1 we have
                   .001*4-.001 or a 3:1 ratio to non moves -> 75% nonmoves.  insert heavy 
                   can pushes this down considerably. further tweaking will be a good idea but 
                   this should be an adequate starting point.
                */
                double N = min(_nIndexes,7) + 3;
                double x = min(2.0,_paddingFactor + (0.001 * N));
                setPaddingFactor( txn, x );
            }
        }

        int userFlags() const { return _userFlags; }
        bool isUserFlagSet( int flag ) const { return _userFlags & flag; }

        /**
         * these methods only modify NamespaceDetails and do not
         * sync changes back to system.namespaces
         * a typical call might
         if ( nsd->setUserFlag( 4 ) ) {
            nsd->syncUserFlags();
         }
         * these methods all return true iff only something was modified
         */
        bool setUserFlag( OperationContext* txn, int flag );
        bool clearUserFlag( OperationContext* txn, int flag );
        bool replaceUserFlags( OperationContext* txn, int flags );

        NamespaceDetails *writingWithoutExtra( OperationContext* txn );

        /** Make all linked Extra objects writeable as well */
        NamespaceDetails *writingWithExtra( OperationContext* txn );

        /**
         * Returns the offset of the specified index name within the array of indexes. Must be
         * passed-in the owning collection to resolve the index record entries to objects.
         *
         * @return > 0 if index name was found, -1 otherwise.
         */
        int _catalogFindIndexByName(const Collection* coll,
                                    const StringData& name, 
                                    bool includeBackgroundInProgress) const;

    private:

        void _removeIndexFromMe( OperationContext* txn, int idx );

        /**
         * swaps all meta data for 2 indexes
         * a and b are 2 index ids, whose contents will be swapped
         * must have a lock on the entire collection to do this
         */
        void swapIndex( OperationContext* txn, int a, int b );

        friend class IndexCatalog;
        friend class IndexCatalogEntry;

        /** Update cappedLastDelRecLastExtent() after capExtent changed in cappedTruncateAfter() */
        void cappedTruncateLastDelUpdate();
        BOOST_STATIC_ASSERT( NIndexesMax <= NIndexesBase + NIndexesExtra*2 );
        BOOST_STATIC_ASSERT( NIndexesMax <= 64 ); // multiKey bits
        BOOST_STATIC_ASSERT( sizeof(NamespaceDetails::Extra) == 496 );
    }; // NamespaceDetails
    BOOST_STATIC_ASSERT( sizeof(NamespaceDetails) == 496 );
#pragma pack()

} // namespace mongo
