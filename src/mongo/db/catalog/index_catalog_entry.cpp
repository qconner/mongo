// index_catalog_entry.cpp

/**
*    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/catalog/index_catalog_entry.h"

#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/structure/head_manager.h"

namespace mongo {

    class HeadManagerImpl : public HeadManager {
    public:
        HeadManagerImpl(IndexCatalogEntry* ice) : _catalogEntry(ice) { }
        virtual ~HeadManagerImpl() { }

        const DiskLoc getHead() const {
            return _catalogEntry->head();
        }

        void setHead(OperationContext* txn, const DiskLoc newHead) {
            _catalogEntry->setHead(txn, newHead);
        }

    private:
        // Not owned here.
        IndexCatalogEntry* _catalogEntry;
    };

    IndexCatalogEntry::IndexCatalogEntry( const StringData& ns,
                                          CollectionCatalogEntry* collection,
                                          IndexDescriptor* descriptor,
                                          CollectionInfoCache* infoCache )
        : _ns( ns.toString() ),
          _collection( collection ),
          _descriptor( descriptor ),
          _infoCache( infoCache ),
          _accessMethod( NULL ),
          _headManager(new HeadManagerImpl(this)),
          _ordering( Ordering::make( descriptor->keyPattern() ) ),
          _isReady( false ) {
        _descriptor->_cachedEntry = this;
    }

    IndexCatalogEntry::~IndexCatalogEntry() {
        _descriptor->_cachedEntry = NULL; // defensive

        delete _headManager;
        delete _accessMethod;
        delete _descriptor;
    }

    void IndexCatalogEntry::init( IndexAccessMethod* accessMethod ) {
        verify( _accessMethod == NULL );
        _accessMethod = accessMethod;

        _isReady = _catalogIsReady();
        _head = _catalogHead();
        _isMultikey = _catalogIsMultikey();
    }

    const DiskLoc& IndexCatalogEntry::head() const {
        DEV verify( _head == _catalogHead() );
        return _head;
    }

    bool IndexCatalogEntry::isReady() const {
        DEV verify( _isReady == _catalogIsReady() );
        return _isReady;
    }

    bool IndexCatalogEntry::isMultikey() const {
        DEV verify( _isMultikey == _catalogIsMultikey() );
        return _isMultikey;
    }

    // ---

    void IndexCatalogEntry::setIsReady( bool newIsReady ) {
        _isReady = newIsReady;
        verify( isReady() == newIsReady );
    }

    void IndexCatalogEntry::setHead( OperationContext* txn, DiskLoc newHead ) {
        _collection->setIndexHead( txn,
                                   _descriptor->indexName(),
                                   newHead );
        _head = newHead;
    }

    void IndexCatalogEntry::setMultikey( OperationContext* txn ) {
        if ( isMultikey() )
            return;
        if ( _collection->setIndexIsMultikey( txn,
                                              _descriptor->indexName(),
                                              true ) ) {
            if ( _infoCache ) {
                LOG(1) << _ns << ": clearing plan cache - index "
                       << _descriptor->keyPattern() << " set to multi key.";
                _infoCache->clearQueryCache();
            }
        }
        _isMultikey = true;
    }

    // ----

    bool IndexCatalogEntry::_catalogIsReady() const {
        return _collection->isIndexReady( _descriptor->indexName() );
    }

    DiskLoc IndexCatalogEntry::_catalogHead() const {
        return _collection->getIndexHead( _descriptor->indexName() );
    }

    bool IndexCatalogEntry::_catalogIsMultikey() const {
        return _collection->isIndexMultikey( _descriptor->indexName() );
    }

    // ------------------

    const IndexCatalogEntry* IndexCatalogEntryContainer::find( const IndexDescriptor* desc ) const {
        if ( desc->_cachedEntry )
            return desc->_cachedEntry;

        for ( const_iterator i = begin(); i != end(); ++i ) {
            const IndexCatalogEntry* e = *i;
            if ( e->descriptor() == desc )
                    return e;
        }
        return NULL;
    }

    IndexCatalogEntry* IndexCatalogEntryContainer::find( const IndexDescriptor* desc ) {
        if ( desc->_cachedEntry )
            return desc->_cachedEntry;

        for ( iterator i = begin(); i != end(); ++i ) {
            IndexCatalogEntry* e = *i;
            if ( e->descriptor() == desc )
                return e;
        }
        return NULL;
    }

    IndexCatalogEntry* IndexCatalogEntryContainer::find( const string& name ) {
        for ( iterator i = begin(); i != end(); ++i ) {
            IndexCatalogEntry* e = *i;
            if ( e->descriptor()->indexName() == name )
                return e;
        }
        return NULL;
    }

    bool IndexCatalogEntryContainer::remove( const IndexDescriptor* desc ) {
        for ( std::vector<IndexCatalogEntry*>::iterator i = _entries.mutableVector().begin();
              i != _entries.mutableVector().end();
              ++i ) {
            IndexCatalogEntry* e = *i;
            if ( e->descriptor() != desc )
                continue;
            _entries.mutableVector().erase( i );
            delete e;
            return true;
        }
        return false;
    }

}  // namespace mongo
