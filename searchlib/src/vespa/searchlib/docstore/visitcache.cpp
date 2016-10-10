// Copyright 2016 Yahoo Inc. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#include "visitcache.h"

namespace search {
namespace docstore {

using vespalib::ConstBufferRef;
using vespalib::LockGuard;
using vespalib::DataBuffer;
using vespalib::alloc::Alloc;
using vespalib::alloc::MemoryAllocator;
using vespalib::DefaultAlloc;

KeySet::KeySet(uint32_t key) :
    _keys()
{
    _keys.push_back(key);
}

KeySet::KeySet(const IDocumentStore::LidVector &keys) :
    _keys(keys)
{
    std::sort(_keys.begin(), _keys.end());
}

bool
KeySet::contains(const KeySet &rhs) const {
    return std::includes(_keys.begin(), _keys.end(), rhs._keys.begin(), rhs._keys.end());
}

BlobSet::BlobSet() :
    _positions(),
    _buffer(DefaultAlloc::create(0, 16 * MemoryAllocator::HUGEPAGE_SIZE), 0)
{ }

namespace {

size_t getBufferSize(const BlobSet::Positions & p) {
    return p.empty() ? 0 : p.back().offset() + p.back().size();
}

}

BlobSet::BlobSet(const Positions & positions, Alloc && buffer) :
    _positions(positions),
    _buffer(std::move(buffer), getBufferSize(_positions))
{
}

void
BlobSet::append(uint32_t lid, ConstBufferRef blob) {
    _positions.emplace_back(lid, getBufferSize(_positions), blob.size());
    _buffer.write(blob.c_str(), blob.size());
}

ConstBufferRef
BlobSet::get(uint32_t lid) const
{
    ConstBufferRef buf;
    for (LidPosition pos : _positions) {
        if (pos.lid() == lid) {
            buf = ConstBufferRef(_buffer.c_str() + pos.offset(), pos.size());
            break;
        }
    }
    return buf;
}

CompressedBlobSet::CompressedBlobSet() :
    _compression(document::CompressionConfig::Type::LZ4),
    _positions(),
    _buffer()
{
}

CompressedBlobSet::CompressedBlobSet(const document::CompressionConfig &compression, const BlobSet & uncompressed) :
    _compression(compression.type),
    _positions(uncompressed.getPositions()),
    _buffer()
{
    if ( ! _positions.empty() ) {
        DataBuffer compressed;
        ConstBufferRef org = uncompressed.getBuffer();
        _compression = document::compress(compression, org, compressed, false);
        _buffer.resize(compressed.getDataLen());
        memcpy(_buffer, compressed.getData(), compressed.getDataLen());
    }
}

BlobSet
CompressedBlobSet::getBlobSet() const
{
    // These are frequent lage allocations that are to expensive to mmap.
    DataBuffer uncompressed(0, 1, DefaultAlloc::create(0, 16 * MemoryAllocator::HUGEPAGE_SIZE));
    if ( ! _positions.empty() ) {
        document::decompress(_compression, getBufferSize(_positions), ConstBufferRef(_buffer.c_str(), _buffer.size()), uncompressed, false);
    }
    return BlobSet(_positions, uncompressed.stealBuffer());
}

size_t CompressedBlobSet::size() const {
    return _positions.capacity() * sizeof(BlobSet::Positions::value_type) + _buffer.size();
}

namespace {

class VisitCollector : public IBufferVisitor
{
public:
    VisitCollector() :
        _blobSet()
    { }
    void visit(uint32_t lid, ConstBufferRef buf) override;
    const BlobSet & getBlobSet() const { return _blobSet; }
private:
    BlobSet _blobSet;
};

void
VisitCollector::visit(uint32_t lid, ConstBufferRef buf) {
    if (buf.size() > 0) {
        _blobSet.append(lid, buf);
    }
}

}

bool
VisitCache::BackingStore::read(const KeySet &key, CompressedBlobSet &blobs) const {
    VisitCollector collector;
    _backingStore.read(key.getKeys(), collector);
    blobs = CompressedBlobSet(_compression, collector.getBlobSet());
    return ! blobs.empty();
}

VisitCache::VisitCache(IDataStore &store, size_t cacheSize, const document::CompressionConfig &compression) :
    _store(store, compression),
    _cache(std::make_unique<Cache>(_store, cacheSize))
{
}

VisitCache::Cache::IdSet
VisitCache::Cache::findSetsContaining(const LockGuard &, const KeySet & keys) const {
    IdSet found;
    for (uint32_t subKey : keys.getKeys()) {
        const auto foundLid = _lid2Id.find(subKey);
        if (foundLid != _lid2Id.end()) {
            found.insert(foundLid->second);
        }
    }
    return found;
}

CompressedBlobSet
VisitCache::Cache::readSet(const KeySet & key)
{
    if (!key.empty()) {
        {
            auto cacheGuard = getGuard();
            if (!hasKey(cacheGuard, key)) {
                locateAndInvalidateOtherSubsets(cacheGuard, key);
            }
        }
        return read(key);
    }
    return CompressedBlobSet();
}

void
VisitCache::Cache::locateAndInvalidateOtherSubsets(const LockGuard & cacheGuard, const KeySet & keys)
{
    // Due to the implementation of insert where the global lock is released and the fact
    // that 2 overlapping keysets kan have different keys and use different ValueLock
    // We do have a theoretical issue.
    // The reason it is theoretical is that for all practical purpose this inconsitency
    // is prevented by the storage layer above alloing only one visit/mutating operation to a single bucket.
    // So for that reason we will just merge this one to get testing started. 
    // The final fix will come in 2 days.
    IdSet otherSubSets = findSetsContaining(cacheGuard, keys);
    for (uint64_t keyId : otherSubSets) {
        invalidate(cacheGuard, _id2KeySet[keyId]);
    }
}

CompressedBlobSet
VisitCache::read(const IDocumentStore::LidVector & lids) const {
    KeySet key(lids);
    return _cache->readSet(lids);
}

void
VisitCache::remove(uint32_t key) {
    _cache->removeKey(key);
}

CacheStats
VisitCache::getCacheStats() const {
    return CacheStats(_cache->getHit(), _cache->getMiss(), _cache->size(), _cache->sizeBytes());
}

VisitCache::Cache::Cache(BackingStore & b, size_t maxBytes) :
    Parent(b, maxBytes)
{ }

void
VisitCache::Cache::removeKey(uint32_t subKey) {
    // Need to take hashLock
    auto cacheGuard = getGuard();
    const auto foundLid = _lid2Id.find(subKey);
    if (foundLid != _lid2Id.end()) {
        K keySet = _id2KeySet[foundLid->second];
        invalidate(cacheGuard, keySet);
    }
}

void
VisitCache::Cache::onInsert(const K & key) {
    uint32_t first(key.getKeys().front());
    _id2KeySet[first] = key;
    for(uint32_t subKey : key.getKeys()) {
        _lid2Id[subKey] = first;
    }
}

void
VisitCache::Cache::onRemove(const K & key) {
    for (uint32_t subKey : key.getKeys()) {
        _lid2Id.erase(subKey);
    }
    _id2KeySet.erase(key.getKeys().front());
}

}
}

