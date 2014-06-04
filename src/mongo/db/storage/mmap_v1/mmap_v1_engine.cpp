// mmap_v1_engine.cpp

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

#include "mongo/db/storage/mmap_v1/mmap_v1_engine.h"

#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/index/2d_access_method.h"
#include "mongo/db/index/btree_access_method.h"
#include "mongo/db/index/btree_based_access_method.h"
#include "mongo/db/index/fts_access_method.h"
#include "mongo/db/index/hash_access_method.h"
#include "mongo/db/index/haystack_access_method.h"
#include "mongo/db/index/s2_access_method.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/structure/catalog/namespace_details.h"
#include "mongo/db/structure/catalog/namespace_details_collection_entry.h"
#include "mongo/db/structure/catalog/namespace_details_rsv1_metadata.h"
#include "mongo/db/structure/record_store_v1_capped.h"
#include "mongo/db/structure/record_store_v1_simple.h"

namespace mongo {

    MONGO_EXPORT_SERVER_PARAMETER(newCollectionsUsePowerOf2Sizes, bool, true);

    MMAP1DatabaseCatalogEntry::MMAP1DatabaseCatalogEntry( OperationContext* txn,
                                                          const StringData& name,
                                                          const StringData& path,
                                                          bool directoryPerDB )
        : DatabaseCatalogEntry( name ),
          _path( path.toString() ),
          _extentManager( name, path, directoryPerDB ),
          _namespaceIndex( _path, name.toString() ) {

        try {
            _checkDuplicateUncasedNames();

            Status s = _extentManager.init(txn);
            if ( !s.isOK() ) {
                msgasserted( 16966, str::stream() << "_extentManager.init failed: " << s.toString() );
            }

            // If already exists, open.  Otherwise behave as if empty until
            // there's a write, then open.

            if ( _namespaceIndex.pathExists() ) {
                _namespaceIndex.init( txn );

                // upgrade freelist
                NamespaceString oldFreeList( name, "$freelist" );
                NamespaceDetails* details = _namespaceIndex.details( oldFreeList.ns() );
                if ( details ) {
                    if ( !details->firstExtent.isNull() ) {
                        _extentManager.freeExtents(txn,
                                                   details->firstExtent,
                                                   details->lastExtent);
                    }
                    _namespaceIndex.kill_ns( txn, oldFreeList.ns() );
                }
            }
        }
        catch(std::exception& e) {
            log() << "warning database " << path << " " << name << " could not be opened";
            DBException* dbe = dynamic_cast<DBException*>(&e);
            if ( dbe != 0 ) {
                log() << "DBException " << dbe->getCode() << ": " << e.what() << endl;
            }
            else {
                log() << e.what() << endl;
            }
            _extentManager.reset();
            throw;
        }


    }

    MMAP1DatabaseCatalogEntry::~MMAP1DatabaseCatalogEntry() {
    }

    void MMAP1DatabaseCatalogEntry::getCollectionNamespaces( std::list<std::string>* tofill ) const {
        _namespaceIndex.getCollectionNamespaces( tofill );
    }

    void MMAP1DatabaseCatalogEntry::_checkDuplicateUncasedNames() const {
        string duplicate = Database::duplicateUncasedName(name(), _path);
        if ( !duplicate.empty() ) {
            stringstream ss;
            ss << "db already exists with different case already have: [" << duplicate
               << "] trying to create [" << name() << "]";
            uasserted( DatabaseDifferCaseCode , ss.str() );
        }
    }

    namespace {
        int _massageExtentSize( const ExtentManager* em, long long size ) {
            if ( size < em->minSize() )
                return em->minSize();
            if ( size > em->maxSize() )
                return em->maxSize();
            return static_cast<int>( size );
        }
    }

    Status MMAP1DatabaseCatalogEntry::createCollection( OperationContext* txn,
                                                        const StringData& ns,
                                                        const CollectionOptions& options,
                                                        bool allocateDefaultSpace ) {
        _namespaceIndex.init( txn );

        if ( _namespaceIndex.details( ns ) ) {
            return Status( ErrorCodes::NamespaceExists,
                           str::stream() << "namespace already exists: " << ns );
        }

        BSONObj optionsAsBSON = options.toBSON();
        _addNamespaceToNamespaceCollection( txn, ns, &optionsAsBSON );

        _namespaceIndex.add_ns( txn, ns, DiskLoc(), options.capped );

        // allocation strategy set explicitly in flags or by server-wide default
        if ( !options.capped ) {
            NamespaceDetailsRSV1MetaData md( ns,
                                             _namespaceIndex.details( ns ),
                                             _getNamespaceRecordStore( txn, ns ) );

            if ( options.flagsSet ) {
                md.setUserFlag( txn, options.flags );
            }
            else if ( newCollectionsUsePowerOf2Sizes ) {
                md.setUserFlag( txn, NamespaceDetails::Flag_UsePowerOf2Sizes );
            }
        }
        else if ( options.cappedMaxDocs > 0 ) {
            txn->recoveryUnit()->writingInt( _namespaceIndex.details( ns )->maxDocsInCapped ) =
                options.cappedMaxDocs;
        }

        if ( allocateDefaultSpace ) {
            scoped_ptr<RecordStoreV1Base> rs( _getRecordStore( txn, ns ) );
            if ( options.initialNumExtents > 0 ) {
                int size = _massageExtentSize( &_extentManager, options.cappedSize );
                for ( int i = 0; i < options.initialNumExtents; i++ ) {
                    rs->increaseStorageSize( txn, size, -1 );
                }
            }
            else if ( !options.initialExtentSizes.empty() ) {
                for ( size_t i = 0; i < options.initialExtentSizes.size(); i++ ) {
                    int size = options.initialExtentSizes[i];
                    size = _massageExtentSize( &_extentManager, size );
                    rs->increaseStorageSize( txn, size, -1 );
                }
            }
            else if ( options.capped ) {
                // normal
                do {
                    // Must do this at least once, otherwise we leave the collection with no
                    // extents, which is invalid.
                    int sz = _massageExtentSize( &_extentManager,
                                                 options.cappedSize - rs->storageSize() );
                    sz &= 0xffffff00;
                    rs->increaseStorageSize( txn, sz, -1 );
                } while( rs->storageSize() < options.cappedSize );
            }
            else {
                rs->increaseStorageSize( txn, _extentManager.initialSize( 128 ), -1 );
            }
        }

        return Status::OK();
    }

    CollectionCatalogEntry* MMAP1DatabaseCatalogEntry::getCollectionCatalogEntry( OperationContext* txn,
                                                                                  const StringData& ns ) {
        NamespaceDetails* details = _namespaceIndex.details( ns );
        if ( !details ) {
            return NULL;
        }

        return new NamespaceDetailsCollectionCatalogEntry( ns,
                                                           details,
                                                           _getIndexRecordStore( txn ),
                                                           this );
    }

    RecordStore* MMAP1DatabaseCatalogEntry::getRecordStore( OperationContext* txn,
                                                            const StringData& ns ) {
        return _getRecordStore( txn, ns );
    }

    RecordStoreV1Base* MMAP1DatabaseCatalogEntry::_getRecordStore( OperationContext* txn,
                                                                   const StringData& ns ) {

        // XXX TODO - CACHE

        NamespaceString nss( ns );
        NamespaceDetails* details = _namespaceIndex.details( ns );
        if ( !details ) {
            return NULL;
        }

        auto_ptr<NamespaceDetailsRSV1MetaData> md( new NamespaceDetailsRSV1MetaData( ns,
                                                                                     details,
                                                                                     _getNamespaceRecordStore( txn, ns ) ) );

        if ( details->isCapped ) {
            return new CappedRecordStoreV1( txn,
                                            NULL, //TOD(ERH) this will blow up :)
                                            ns,
                                            md.release(),
                                            &_extentManager,
                                            nss.coll() == "system.indexes" );
        }

        return new SimpleRecordStoreV1( txn,
                                        ns,
                                        md.release(),
                                        &_extentManager,
                                        nss.coll() == "system.indexes" );
    }

    IndexAccessMethod* MMAP1DatabaseCatalogEntry::getIndex( OperationContext* txn,
                                                            const CollectionCatalogEntry* collection,
                                                            IndexCatalogEntry* entry ) {
        const string& type = entry->descriptor()->getAccessMethodName();

        string ns = collection->ns().ns();

        if ( IndexNames::TEXT == type ||
             entry->descriptor()->getInfoElement("expireAfterSeconds").isNumber() ) {
            NamespaceDetailsRSV1MetaData md( ns,
                                             _namespaceIndex.details( ns ),
                                             _getNamespaceRecordStore( txn, ns ) );
            md.setUserFlag( txn, NamespaceDetails::Flag_UsePowerOf2Sizes );
        }

        RecordStore* rs = _getRecordStore( txn, entry->descriptor()->indexNamespace() );
        invariant( rs );

        if (IndexNames::HASHED == type)
            return new HashAccessMethod( entry, rs );

        if (IndexNames::GEO_2DSPHERE == type)
            return new S2AccessMethod( entry, rs );

        if (IndexNames::TEXT == type)
            return new FTSAccessMethod( entry, rs );

        if (IndexNames::GEO_HAYSTACK == type)
            return new HaystackAccessMethod( entry, rs );

        if ("" == type)
            return new BtreeAccessMethod( entry, rs );

        if (IndexNames::GEO_2D == type)
            return new TwoDAccessMethod( entry, rs );

        log() << "Can't find index for keyPattern " << entry->descriptor()->keyPattern();
        fassertFailed(17489);
    }

    RecordStoreV1Base* MMAP1DatabaseCatalogEntry::_getIndexRecordStore( OperationContext* txn ) {
        NamespaceString nss( name(), "system.indexes" );
        RecordStoreV1Base* rs = _getRecordStore( txn, nss.ns() );
        if ( rs != NULL )
            return rs;
        CollectionOptions options;
        Status status = createCollection( txn, nss.ns(), options, true );
        massertStatusOK( status );
        rs = _getRecordStore( txn, nss.ns() );
        invariant( rs );
        return rs;
    }

    RecordStoreV1Base* MMAP1DatabaseCatalogEntry::_getNamespaceRecordStore( OperationContext* txn,
                                                                            const StringData& whosAsking) {
        NamespaceString nss( name(), "system.namespaces" );
        if ( nss == whosAsking )
            return NULL;
        RecordStoreV1Base* rs = _getRecordStore( txn, nss.ns() );
        if ( rs != NULL )
            return rs;
        CollectionOptions options;
        Status status = createCollection( txn, nss.ns(), options, true );
        massertStatusOK( status );
        rs = _getRecordStore( txn, nss.ns() );
        invariant( rs );
        return rs;

    }

    void MMAP1DatabaseCatalogEntry::_addNamespaceToNamespaceCollection( OperationContext* txn,
                                                                        const StringData& ns,
                                                                        const BSONObj* options ) {
        if ( nsToCollectionSubstring( ns ) == "system.namespaces" ) {
            // system.namespaces holds all the others, so it is not explicitly listed in the catalog.
            return;
        }

        BSONObjBuilder b;
        b.append("name", ns);
        if ( options && !options->isEmpty() )
            b.append("options", *options);
        BSONObj obj = b.done();

        RecordStoreV1Base* rs = _getNamespaceRecordStore( txn, ns );
        invariant( rs );
        StatusWith<DiskLoc> loc = rs->insertRecord( txn, obj.objdata(), obj.objsize(), -1 );
        massertStatusOK( loc.getStatus() );
    }

}
