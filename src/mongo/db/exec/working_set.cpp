/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/exec/working_set.h"

#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/service_context.h"

namespace mongo {

using std::string;

namespace dps = ::mongo::dotted_path_support;

WorkingSet::WorkingSet() : _freeList(INVALID_ID) {}

WorkingSetID WorkingSet::allocate() {
    if (_freeList == INVALID_ID) {
        // The free list is empty so we need to make a single new WSM to return. This relies on
        // vector::resize being amortized O(1) for efficient allocation. Note that the free list
        // remains empty until something is returned by a call to free().
        WorkingSetID id = _data.size();
        _data.resize(_data.size() + 1);
        _data.back().nextFreeOrSelf = id;
        return id;
    }

    // Pop the head off the free list and return it.
    WorkingSetID id = _freeList;
    _freeList = _data[id].nextFreeOrSelf;
    _data[id].nextFreeOrSelf = id;  // set to self to mark as in-use
    return id;
}

void WorkingSet::free(WorkingSetID i) {
    MemberHolder& holder = _data[i];
    verify(i < _data.size());            // ID has been allocated.
    verify(holder.nextFreeOrSelf == i);  // ID currently in use.

    // Free resources and push this WSM to the head of the freelist.
    holder.member.clear();
    holder.nextFreeOrSelf = _freeList;
    _freeList = i;
}

void WorkingSet::clear() {
    _data.clear();

    // Since working set is now empty, the free list pointer should
    // point to nothing.
    _freeList = INVALID_ID;
}

void WorkingSet::transitionToRecordIdAndIdx(WorkingSetID id) {
    WorkingSetMember* member = get(id);
    member->_state = WorkingSetMember::RID_AND_IDX;
}

void WorkingSet::transitionToRecordIdAndObj(WorkingSetID id) {
    WorkingSetMember* member = get(id);
    member->transitionToRecordIdAndObj();
}

void WorkingSet::transitionToOwnedObj(WorkingSetID id) {
    WorkingSetMember* member = get(id);
    member->transitionToOwnedObj();
}

WorkingSetMember WorkingSet::extract(WorkingSetID wsid) {
    invariant(wsid < _data.size());
    WorkingSetMember ret = std::move(_data[wsid].member);
    free(wsid);
    return ret;
}

WorkingSetID WorkingSet::emplace(WorkingSetMember&& wsm) {
    auto wsid = allocate();
    *get(wsid) = std::move(wsm);
    return wsid;
}

//
// WorkingSetMember
//

void WorkingSetMember::clear() {
    _metadata = DocumentMetadataFields{};
    keyData.clear();
    doc = {SnapshotId(), Document()};
    _state = WorkingSetMember::INVALID;
}

WorkingSetMember::MemberState WorkingSetMember::getState() const {
    return _state;
}

void WorkingSetMember::transitionToOwnedObj() {
    invariant(doc.value().isOwned());
    _state = OWNED_OBJ;
}

void WorkingSetMember::transitionToRecordIdAndObj() {
    _state = WorkingSetMember::RID_AND_OBJ;
}

bool WorkingSetMember::hasRecordId() const {
    return _state == RID_AND_IDX || _state == RID_AND_OBJ;
}

bool WorkingSetMember::hasObj() const {
    return _state == OWNED_OBJ || _state == RID_AND_OBJ;
}

bool WorkingSetMember::hasOwnedObj() const {
    return _state == OWNED_OBJ || _state == RID_AND_OBJ;
}

void WorkingSetMember::makeObjOwnedIfNeeded() {
    if (_state == RID_AND_OBJ && !doc.value().isOwned()) {
        doc.value() = doc.value().getOwned();
    }
}

bool WorkingSetMember::getFieldDotted(const string& field, BSONElement* out) const {
    // If our state is such that we have an object, use it.
    if (hasObj()) {
        // The document must not be modified. Otherwise toBson() call would create a temporary BSON
        // that would get destroyed at the end of this function. *out would then point to dangling
        // memory.
        invariant(!doc.value().isModified());
        *out = dps::extractElementAtPath(doc.value().toBson(), field);
        return true;
    }

    // Our state should be such that we have index data/are covered.
    if (auto outOpt = IndexKeyDatum::getFieldDotted(keyData, field)) {
        *out = outOpt.get();
        return true;
    } else {
        return false;
    }
}

size_t WorkingSetMember::getMemUsage() const {
    size_t memUsage = 0;

    if (hasRecordId()) {
        memUsage += sizeof(RecordId);
    }

    if (hasObj()) {
        memUsage += doc.value().getApproximateSize();
    }

    for (size_t i = 0; i < keyData.size(); ++i) {
        const IndexKeyDatum& keyDatum = keyData[i];
        memUsage += keyDatum.keyData.objsize();
    }

    return memUsage;
}

void WorkingSetMember::resetDocument(SnapshotId snapshot, const BSONObj& obj) {
    doc.setSnapshotId(snapshot);
    MutableDocument md(std::move(doc.value()));
    md.reset(obj, false);
    doc.value() = md.freeze();
}

void WorkingSetMember::serializeForSorter(BufBuilder& buf) const {
    // It is not legal to serialize a Document which has metadata attached to it. Any metadata must
    // reside directly in the WorkingSetMember.
    invariant(!doc.value().metadata());

    buf.appendChar(static_cast<char>(_state));

    if (hasObj()) {
        doc.value().serializeForSorter(buf);
        buf.appendNum(static_cast<unsigned long long>(doc.snapshotId().toNumber()));
    }

    if (_state == RID_AND_IDX) {
        // First append the number of index keys, and then encode them in series.
        buf.appendNum(static_cast<char>(keyData.size()));
        for (auto&& indexKeyDatum : keyData) {
            indexKeyDatum.indexKeyPattern.serializeForSorter(buf);
            indexKeyDatum.keyData.serializeForSorter(buf);
            buf.appendNum(indexKeyDatum.indexId);
            buf.appendNum(static_cast<unsigned long long>(indexKeyDatum.snapshotId.toNumber()));
        }
    }

    if (hasRecordId()) {
        buf.appendNum(recordId.repr());
    }

    _metadata.serializeForSorter(buf);
}

WorkingSetMember WorkingSetMember::deserializeForSorter(BufReader& buf,
                                                        const SorterDeserializeSettings&) {
    WorkingSetMember wsm;

    // First decode the state, which instructs us on how to interpret the rest of the buffer.
    wsm._state = static_cast<MemberState>(buf.read<char>());

    if (wsm.hasObj()) {
        wsm.doc.setValue(
            Document::deserializeForSorter(buf, Document::SorterDeserializeSettings{}));
        auto snapshotIdRepr = buf.read<LittleEndian<uint64_t>>();
        auto snapshotId = snapshotIdRepr ? SnapshotId{snapshotIdRepr} : SnapshotId{};
        wsm.doc.setSnapshotId(snapshotId);
    }

    if (wsm.getState() == WorkingSetMember::RID_AND_IDX) {
        auto numKeys = buf.read<char>();
        wsm.keyData.reserve(numKeys);
        for (auto i = 0; i < numKeys; ++i) {
            auto indexKeyPattern =
                BSONObj::deserializeForSorter(buf, BSONObj::SorterDeserializeSettings{}).getOwned();
            auto indexKey =
                BSONObj::deserializeForSorter(buf, BSONObj::SorterDeserializeSettings{}).getOwned();
            auto indexId = buf.read<LittleEndian<unsigned int>>();
            auto snapshotIdRepr = buf.read<LittleEndian<uint64_t>>();
            auto snapshotId = snapshotIdRepr ? SnapshotId{snapshotIdRepr} : SnapshotId{};
            wsm.keyData.push_back(IndexKeyDatum{
                std::move(indexKeyPattern), std::move(indexKey), indexId, snapshotId});
        }
    }

    if (wsm.hasRecordId()) {
        wsm.recordId = RecordId{buf.read<LittleEndian<int64_t>>()};
    }

    DocumentMetadataFields::deserializeForSorter(buf, &wsm._metadata);

    return wsm;
}

WorkingSetRegisteredIndexId WorkingSet::registerIndexAccessMethod(
    const IndexAccessMethod* indexAccess) {
    for (WorkingSetRegisteredIndexId i = 0; i < _registeredIndexes.size(); ++i) {
        if (_registeredIndexes[i] == indexAccess) {
            return i;
        }
    }

    _registeredIndexes.push_back(indexAccess);
    return _registeredIndexes.size() - 1;
}

}  // namespace mongo
