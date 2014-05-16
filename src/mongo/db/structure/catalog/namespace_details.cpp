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

#include "mongo/pch.h"

#include "mongo/db/structure/catalog/namespace_details.h"

#include <algorithm>
#include <list>

#include "mongo/base/counter.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/db.h"
#include "mongo/db/index_legacy.h"
#include "mongo/db/json.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/structure/catalog/hashtab.h"
#include "mongo/db/operation_context.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/startup_test.h"


namespace mongo {


    BSONObj idKeyPattern = fromjson("{\"_id\":1}");

    NamespaceDetails::NamespaceDetails( const DiskLoc &loc, bool capped ) {
        BOOST_STATIC_ASSERT( sizeof(NamespaceDetails::Extra) <= sizeof(NamespaceDetails) );

        /* be sure to initialize new fields here -- doesn't default to zeroes the way we use it */
        _firstExtent = _lastExtent = _capExtent = loc;
        _stats.datasize = _stats.nrecords = 0;
        _lastExtentSize = 0;
        _nIndexes = 0;
        _isCapped = capped;
        _maxDocsInCapped = 0x7fffffff; // no limit (value is for pre-v2.3.2 compatibility)
        _paddingFactor = 1.0;
        _systemFlagsOld = 0;
        _userFlags = 0;
        _capFirstNewRecord = DiskLoc();
        // Signal that we are on first allocation iteration through extents.
        _capFirstNewRecord.setInvalid();
        // For capped case, signal that we are doing initial extent allocation.
        if ( capped ) {
            // WAS: cappedLastDelRecLastExtent().setInvalid();
            _deletedList[1].setInvalid();
        }
        verify( sizeof(_dataFileVersion) == 2 );
        _dataFileVersion = 0;
        _indexFileVersion = 0;
        _multiKeyIndexBits = 0;
        _reservedA = 0;
        _extraOffset = 0;
        _indexBuildsInProgress = 0;
        memset(_reserved, 0, sizeof(_reserved));
    }

    NamespaceDetails::Extra* NamespaceDetails::allocExtra( OperationContext* txn,
                                                           const StringData& ns,
                                                           NamespaceIndex& ni,
                                                           int nindexessofar) {
        Lock::assertWriteLocked(ns);

        int i = (nindexessofar - NIndexesBase) / NIndexesExtra;
        verify( i >= 0 && i <= 1 );

        Namespace fullns( ns );
        Namespace extrans( fullns.extraName(i) ); // throws UserException if ns name too long

        massert( 10350, "allocExtra: base ns missing?", this );
        massert( 10351, "allocExtra: extra already exists", ni.details(extrans) == 0 );

        Extra temp;
        temp.init();

        ni.add_ns( txn, extrans, reinterpret_cast<NamespaceDetails*>( &temp ) );
        Extra* e = reinterpret_cast<NamespaceDetails::Extra*>( ni.details( extrans ) );

        long ofs = e->ofsFrom(this);
        if( i == 0 ) {
            verify( _extraOffset == 0 );
            *txn->recoveryUnit()->writing(&_extraOffset) = ofs;
            verify( extra() == e );
        }
        else {
            Extra *hd = extra();
            verify( hd->next(this) == 0 );
            hd->setNext(txn, ofs);
        }
        return e;
    }

    bool NamespaceDetails::setIndexIsMultikey(OperationContext* txn, int i, bool multikey) {
        massert(16577, "index number greater than NIndexesMax", i < NIndexesMax );

        unsigned long long mask = 1ULL << i;

        if (multikey) {
            // Shortcut if the bit is already set correctly
            if (_multiKeyIndexBits & mask) {
                return false;
            }

            *txn->recoveryUnit()->writing(&_multiKeyIndexBits) |= mask;
        }
        else {
            // Shortcut if the bit is already set correctly
            if (!(_multiKeyIndexBits & mask)) {
                return false;
            }

            // Invert mask: all 1's except a 0 at the ith bit
            mask = ~mask;
            *txn->recoveryUnit()->writing(&_multiKeyIndexBits) &= mask;
        }

        return true;
    }

    IndexDetails& NamespaceDetails::getNextIndexDetails(OperationContext* txn,
                                                        Collection* collection) {
        IndexDetails *id;
        try {
            id = &idx(getTotalIndexCount(), true);
        }
        catch(DBException&) {
            allocExtra(txn,
                       collection->ns().ns(),
                       collection->_database->namespaceIndex(),
                       getTotalIndexCount());
            id = &idx(getTotalIndexCount(), false);
        }
        return *id;
    }

    IndexDetails& NamespaceDetails::idx(int idxNo, bool missingExpected) {
        if( idxNo < NIndexesBase ) {
            IndexDetails& id = _indexes[idxNo];
            return id;
        }
        Extra *e = extra();
        if ( ! e ) {
            if ( missingExpected )
                throw MsgAssertionException( 13283 , "Missing Extra" );
            massert(14045, "missing Extra", e);
        }
        int i = idxNo - NIndexesBase;
        if( i >= NIndexesExtra ) {
            e = e->next(this);
            if ( ! e ) {
                if ( missingExpected )
                    throw MsgAssertionException( 14823 , "missing extra" );
                massert(14824, "missing Extra", e);
            }
            i -= NIndexesExtra;
        }
        return e->details[i];
    }


    const IndexDetails& NamespaceDetails::idx(int idxNo, bool missingExpected) const {
        if( idxNo < NIndexesBase ) {
            const IndexDetails& id = _indexes[idxNo];
            return id;
        }
        const Extra *e = extra();
        if ( ! e ) {
            if ( missingExpected )
                throw MsgAssertionException( 17421 , "Missing Extra" );
            massert(17422, "missing Extra", e);
        }
        int i = idxNo - NIndexesBase;
        if( i >= NIndexesExtra ) {
            e = e->next(this);
            if ( ! e ) {
                if ( missingExpected )
                    throw MsgAssertionException( 17423 , "missing extra" );
                massert(17424, "missing Extra", e);
            }
            i -= NIndexesExtra;
        }
        return e->details[i];
    }


    NamespaceDetails::IndexIterator::IndexIterator(const NamespaceDetails *_d,
                                                   bool includeBackgroundInProgress) {
        d = _d;
        i = 0;
        n = includeBackgroundInProgress ? d->getTotalIndexCount() : d->_nIndexes;
    }

    // must be called when renaming a NS to fix up extra
    void NamespaceDetails::copyingFrom( OperationContext* txn,
                                        const char* thisns,
                                        NamespaceIndex& ni,
                                        NamespaceDetails* src) {
        _extraOffset = 0; // we are a copy -- the old value is wrong.  fixing it up below.
        Extra *se = src->extra();
        int n = NIndexesBase;
        if( se ) {
            Extra *e = allocExtra(txn, thisns, ni, n);
            while( 1 ) {
                n += NIndexesExtra;
                e->copy(this, *se);
                se = se->next(src);
                if( se == 0 ) break;
                Extra *nxt = allocExtra(txn, thisns, ni, n);
                e->setNext( txn, nxt->ofsFrom(this) );
                e = nxt;
            }
            verify( _extraOffset );
        }
    }

    NamespaceDetails* NamespaceDetails::writingWithoutExtra( OperationContext* txn ) {
        return txn->recoveryUnit()->writing( this );
    }


    // XXX - this method should go away
    NamespaceDetails *NamespaceDetails::writingWithExtra( OperationContext* txn ) {
        for( Extra *e = extra(); e; e = e->next( this ) ) {
            txn->recoveryUnit()->writing( e );
        }
        return writingWithoutExtra( txn );
    }

    void NamespaceDetails::setMaxCappedDocs( OperationContext* txn, long long max ) {
        massert( 16499,
                 "max in a capped collection has to be < 2^31 or -1",
                 validMaxCappedDocs( &max ) );
        _maxDocsInCapped = max;
    }

    bool NamespaceDetails::validMaxCappedDocs( long long* max ) {
        if ( *max <= 0 ||
             *max == numeric_limits<long long>::max() ) {
            *max = 0x7fffffff;
            return true;
        }

        if ( *max < ( 0x1LL << 31 ) ) {
            return true;
        }

        return false;
    }

    long long NamespaceDetails::maxCappedDocs() const {
        verify( isCapped() );
        if ( _maxDocsInCapped == 0x7fffffff )
            return numeric_limits<long long>::max();
        return _maxDocsInCapped;
    }

    /* ------------------------------------------------------------------------- */

    void NamespaceDetails::setLastExtentSize( OperationContext* txn, int newMax ) {
        if ( _lastExtentSize == newMax )
            return;
        txn->recoveryUnit()->writingInt(_lastExtentSize) = newMax;
    }

    void NamespaceDetails::incrementStats( OperationContext* txn,
                                           long long dataSizeIncrement,
                                           long long numRecordsIncrement ) {

        // durability todo : this could be a bit annoying / slow to record constantly
        Stats* s = txn->recoveryUnit()->writing( &_stats );
        s->datasize += dataSizeIncrement;
        s->nrecords += numRecordsIncrement;
    }

    void NamespaceDetails::setStats( OperationContext* txn,
                                     long long dataSize,
                                     long long numRecords ) {
        Stats* s = txn->recoveryUnit()->writing( &_stats );
        s->datasize = dataSize;
        s->nrecords = numRecords;
    }

    void NamespaceDetails::setFirstExtent( OperationContext* txn,
                                           const DiskLoc& loc ) {
        *txn->recoveryUnit()->writing( &_firstExtent ) = loc;
    }

    void NamespaceDetails::setLastExtent( OperationContext* txn,
                                          const DiskLoc& loc ) {
        *txn->recoveryUnit()->writing( &_lastExtent ) = loc;
    }

    void NamespaceDetails::setCapExtent( OperationContext* txn,
                                         const DiskLoc& loc ) {
        *txn->recoveryUnit()->writing( &_capExtent ) = loc;
    }

    void NamespaceDetails::setCapFirstNewRecord( OperationContext* txn,
                                                 const DiskLoc& loc ) {
        *txn->recoveryUnit()->writing( &_capFirstNewRecord ) = loc;
    }

    void NamespaceDetails::setFirstExtentInvalid( OperationContext* txn ) {
        *txn->recoveryUnit()->writing( &_firstExtent ) = DiskLoc().setInvalid();
    }

    void NamespaceDetails::setLastExtentInvalid( OperationContext* txn ) {
        *txn->recoveryUnit()->writing( &_lastExtent ) = DiskLoc().setInvalid();
    }

    void NamespaceDetails::setDeletedListEntry( OperationContext* txn,
                                                int bucket, const DiskLoc& loc ) {
        *txn->recoveryUnit()->writing( &_deletedList[bucket] ) = loc;
    }

    bool NamespaceDetails::setUserFlag( OperationContext* txn, int flags ) {
        if ( ( _userFlags & flags ) == flags )
            return false;
        
        txn->recoveryUnit()->writingInt(_userFlags) |= flags;
        return true;
    }

    bool NamespaceDetails::clearUserFlag( OperationContext* txn, int flags ) {
        if ( ( _userFlags & flags ) == 0 )
            return false;

        txn->recoveryUnit()->writingInt(_userFlags) &= ~flags;
        return true;
    }

    bool NamespaceDetails::replaceUserFlags( OperationContext* txn, int flags ) {
        if ( flags == _userFlags )
            return false;

        txn->recoveryUnit()->writingInt(_userFlags) = flags;
        return true;
    }

    void NamespaceDetails::setPaddingFactor( OperationContext* txn, double paddingFactor ) {
        if ( paddingFactor == _paddingFactor )
            return;

        if ( isCapped() )
            return;

        *txn->recoveryUnit()->writing(&_paddingFactor) = paddingFactor;
    }

    /* remove bit from a bit array - actually remove its slot, not a clear
       note: this function does not work with x == 63 -- that is ok
             but keep in mind in the future if max indexes were extended to
             exactly 64 it would be a problem
    */
    unsigned long long removeAndSlideBit(unsigned long long b, int x) {
        unsigned long long tmp = b;
        return
            (tmp & ((((unsigned long long) 1) << x)-1)) |
            ((tmp >> (x+1)) << x);
    }

    void NamespaceDetails::_removeIndexFromMe( OperationContext* txn, int idxNumber ) {

        // TODO: don't do this whole thing, do it piece meal for readability
        NamespaceDetails* d = writingWithExtra( txn );

        // fix the _multiKeyIndexBits, by moving all bits above me down one
        d->_multiKeyIndexBits = removeAndSlideBit(d->_multiKeyIndexBits, idxNumber);

        if ( idxNumber >= _nIndexes )
            d->_indexBuildsInProgress--;
        else
            d->_nIndexes--;

        for ( int i = idxNumber; i < getTotalIndexCount(); i++ )
            d->idx(i) = d->idx(i+1);

        d->idx( getTotalIndexCount() ) = IndexDetails();
    }

    void NamespaceDetails::swapIndex( OperationContext* txn, int a, int b ) {

        // flip main meta data
        IndexDetails temp = idx(a);
        *txn->recoveryUnit()->writing(&idx(a)) = idx(b);
        *txn->recoveryUnit()->writing(&idx(b)) = temp;

        // flip multi key bits
        bool tempMultikey = isMultikey(a);
        setIndexIsMultikey( txn, a, isMultikey(b) );
        setIndexIsMultikey( txn, b, tempMultikey );
    }

    void NamespaceDetails::orphanDeletedList( OperationContext* txn ) {
        for( int i = 0; i < Buckets; i++ ) {
            *txn->recoveryUnit()->writing(&_deletedList[i]) = DiskLoc();
        }
    }

    int NamespaceDetails::_catalogFindIndexByName(const Collection* coll,
                                                  const StringData& name,
                                                  bool includeBackgroundInProgress) const {
        IndexIterator i = ii(includeBackgroundInProgress);
        while( i.more() ) {
            const BSONObj obj = coll->docFor(i.next().info);
            if ( name == obj.getStringField("name") )
                return i.pos()-1;
        }
        return -1;
    }

    void NamespaceDetails::Extra::setNext( OperationContext* txn,
                                           long ofs ) {
        *txn->recoveryUnit()->writing(&_next) = ofs;
    }

    /* ------------------------------------------------------------------------- */

    class IndexUpdateTest : public StartupTest {
    public:
        void run() {
            verify( removeAndSlideBit(1, 0) == 0 );
            verify( removeAndSlideBit(2, 0) == 1 );
            verify( removeAndSlideBit(2, 1) == 0 );
            verify( removeAndSlideBit(255, 1) == 127 );
            verify( removeAndSlideBit(21, 2) == 9 );
            verify( removeAndSlideBit(0x4000000000000001ULL, 62) == 1 );
        }
    } iu_unittest;

} // namespace mongo
