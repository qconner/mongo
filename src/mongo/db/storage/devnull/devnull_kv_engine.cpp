// devnull_kv_engine.cpp

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

#include "mongo/db/storage/devnull/devnull_kv_engine.h"

#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/sorted_data_interface.h"

namespace mongo {

    class EmptyRecordIterator: public RecordIterator {
    public:
        virtual bool isEOF() { return true; }
        virtual DiskLoc curr() { return DiskLoc(); }
        virtual DiskLoc getNext() { return DiskLoc(); }
        virtual void invalidate(const DiskLoc& dl) { }
        virtual void saveState() { }
        virtual bool restoreState(OperationContext* txn) { return false; }
        virtual RecordData dataFor( const DiskLoc& loc ) const {
            invariant( false );
        }
    };

    class DevNullRecordStore : public RecordStore {
    public:
        DevNullRecordStore( const StringData& ns, const CollectionOptions& options )
            : RecordStore( ns ), _options( options ) {
            _numInserts = 0;
            _dummy = BSON( "_id" << 1 );
        }

        virtual const char* name() const { return "devnull"; }

        virtual void setCappedDeleteCallback(CappedDocumentDeleteCallback*){}

        virtual long long dataSize( OperationContext* txn ) const { return 0; }

        virtual long long numRecords( OperationContext* txn ) const { return 0; }

        virtual bool isCapped() const { return _options.capped; }

        virtual int64_t storageSize( OperationContext* txn,
                                     BSONObjBuilder* extraInfo = NULL,
                                     int infoLevel = 0 ) const {
            return 0;
        }

        virtual RecordData dataFor( OperationContext* txn, const DiskLoc& loc) const {
            return RecordData( _dummy.objdata(), _dummy.objsize() );
        }

        virtual bool findRecord( OperationContext* txn, const DiskLoc& loc, RecordData* rd ) const {
            return false;
        }

        virtual void deleteRecord( OperationContext* txn, const DiskLoc& dl ) {}

        virtual StatusWith<DiskLoc> insertRecord( OperationContext* txn,
                                                  const char* data,
                                                  int len,
                                                  bool enforceQuota ) {
            _numInserts++;
            return StatusWith<DiskLoc>( DiskLoc( 6, 4 ) );
        }

        virtual StatusWith<DiskLoc> insertRecord( OperationContext* txn,
                                                  const DocWriter* doc,
                                                  bool enforceQuota ) {
            _numInserts++;
            return StatusWith<DiskLoc>( DiskLoc( 6, 4 ) );
        }

        virtual StatusWith<DiskLoc> updateRecord( OperationContext* txn,
                                                  const DiskLoc& oldLocation,
                                                  const char* data,
                                                  int len,
                                                  bool enforceQuota,
                                                  UpdateMoveNotifier* notifier ) {
            return StatusWith<DiskLoc>( oldLocation );
        }

        virtual Status updateWithDamages( OperationContext* txn,
                                          const DiskLoc& loc,
                                          const RecordData& oldRec,
                                          const char* damageSource,
                                          const mutablebson::DamageVector& damages ) {
            return Status::OK();
        }

        virtual RecordIterator* getIterator( OperationContext* txn,
                                             const DiskLoc& start,
                                             const CollectionScanParams::Direction& dir ) const {
            return new EmptyRecordIterator();
        }

        virtual RecordIterator* getIteratorForRepair( OperationContext* txn ) const {
            return new EmptyRecordIterator();
        }

        virtual std::vector<RecordIterator*> getManyIterators( OperationContext* txn ) const {
            std::vector<RecordIterator*> v;
            v.push_back( new EmptyRecordIterator() );
            return v;
        }

        virtual Status truncate( OperationContext* txn ) { return Status::OK(); }

        virtual void temp_cappedTruncateAfter(OperationContext* txn,
                                              DiskLoc end,
                                              bool inclusive) { }

        virtual bool compactSupported() const { return false; }
        virtual Status compact( OperationContext* txn,
                                RecordStoreCompactAdaptor* adaptor,
                                const CompactOptions* options,
                                CompactStats* stats ) { return Status::OK(); }

        virtual Status validate( OperationContext* txn,
                                 bool full, bool scanData,
                                 ValidateAdaptor* adaptor,
                                 ValidateResults* results, BSONObjBuilder* output ) const {
            return Status::OK();
        }

        virtual void appendCustomStats( OperationContext* txn,
                                        BSONObjBuilder* result,
                                        double scale ) const {
            result->appendNumber( "numInserts", _numInserts );
        }

        virtual Status touch( OperationContext* txn, BSONObjBuilder* output ) const {
            return Status::OK();
        }

        virtual Status setCustomOption( OperationContext* txn,
                                        const BSONElement& option,
                                        BSONObjBuilder* info = NULL ) {
            return Status::OK();
        }

    private:
        CollectionOptions _options;
        long long _numInserts;
        BSONObj _dummy;
    };

    RecordStore* DevNullKVEngine::getRecordStore( OperationContext* opCtx,
                                                  const StringData& ns,
                                                  const StringData& ident,
                                                  const CollectionOptions& options ) {
        return new DevNullRecordStore( ns, options );
    }

    SortedDataInterface* DevNullKVEngine::getSortedDataInterface( OperationContext* opCtx,
                                                                  const StringData& ident,
                                                                  const IndexDescriptor* desc ) {
        return NULL;
    }

}
