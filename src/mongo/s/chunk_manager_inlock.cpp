/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/chunk_manager_inlock.h"

#include <vector>

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/query/collation/collation_index_key.h"
#include "mongo/db/query/index_bounds_builder.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/util/log.h"
#include "mongo/util/timer.h"
#include <boost/optional.hpp>

namespace mongo {
namespace {

// Used to generate sequence numbers to assign to each newly created ChunkManager
AtomicUInt32 nextCMILSequenceNumber(0);

void checkAllElementsAreOfType(BSONType type, const BSONObj& o) {
    for (const auto&& element : o) {
        uassert(ErrorCodes::ConflictingOperationInProgress,
                str::stream() << "Not all elements of " << o << " are of type " << typeName(type),
                element.type() == type);
    }
}

}  // namespace


std::string extractKeyStringInternalWithLock(const BSONObj& shardKeyValue, Ordering ordering) {
    BSONObjBuilder strippedKeyValue;
    for (const auto& elem : shardKeyValue) {
        strippedKeyValue.appendAs(elem, ""_sd);
    }

    KeyString ks(KeyString::Version::V1, strippedKeyValue.done(), ordering);
    return {ks.getBuffer(), ks.getSize()};
}


ChunkManagerWithLock::ChunkManagerWithLock(NamespaceString nss,
                                           KeyPattern shardKeyPattern,
                                           std::unique_ptr<CollatorInterface> defaultCollator,
                                           bool unique,
                                           ChunkMap chunkMap,
                                           ChunkVersion collectionVersion)
    : _sequenceNumber(nextCMILSequenceNumber.addAndFetch(1)),
      _nss(std::move(nss)),
      _shardKeyPattern(shardKeyPattern),
      _shardKeyOrdering(Ordering::make(_shardKeyPattern.toBSON())),
      _defaultCollator(std::move(defaultCollator)),
      _unique(unique),
      _chunkMap(std::move(chunkMap)),
      _shardVersions(
          _constructShardVersionMap(collectionVersion.epoch(), _chunkMap, _shardKeyOrdering)),
      _shardVersionSize(_shardVersions.size()),
      _collectionVersion(collectionVersion)
     {}

ChunkManagerWithLock::~ChunkManagerWithLock() = default;

std::shared_ptr<Chunk> ChunkManagerWithLock::findIntersectingChunk(const BSONObj& shardKey,
                                                                   const BSONObj& collation) const {
    const bool hasSimpleCollation = (collation.isEmpty() && !_defaultCollator) ||
        SimpleBSONObjComparator::kInstance.evaluate(collation == CollationSpec::kSimpleSpec);
    if (!hasSimpleCollation) {
        for (BSONElement elt : shardKey) {
            uassert(ErrorCodes::ShardKeyNotFound,
                    str::stream() << "Cannot target single shard due to collation of key "
                                  << elt.fieldNameStringData(),
                    !CollationIndexKey::isCollatableType(elt.type()));
        }
    }

    boost::shared_lock<boost::shared_mutex> lock(_mutex);
    const auto it = _chunkMap.upper_bound(_extractKeyString(shardKey));
    uassert(ErrorCodes::ShardKeyNotFound,
            str::stream() << "Cannot target single shard using key " << shardKey,
            it != _chunkMap.end() && it->second->containsKey(shardKey));

    return it->second;
}

std::shared_ptr<Chunk> ChunkManagerWithLock::findIntersectingChunkWithSimpleCollation(
    const BSONObj& shardKey) const {
    return findIntersectingChunk(shardKey, CollationSpec::kSimpleSpec);
}

const ChunkMap& ChunkManagerWithLock::chunkMap() const {
    boost::shared_lock<boost::shared_mutex> lock(_mutex);
    return _chunkMap;
}


int ChunkManagerWithLock::numChunks() const {
    boost::shared_lock<boost::shared_mutex> lock(_mutex);
    return _chunkMap.size();
}


void ChunkManagerWithLock::getShardIdsForQuery(OperationContext* txn,
                                               const BSONObj& query,
                                               const BSONObj& collation,
                                               std::set<ShardId>* shardIds) const {
    auto qr = stdx::make_unique<QueryRequest>(_nss);
    qr->setFilter(query);

    if (!collation.isEmpty()) {
        qr->setCollation(collation);
    } else if (_defaultCollator) {
        qr->setCollation(_defaultCollator->getSpec().toBSON());
    }

    std::unique_ptr<CanonicalQuery> cq =
        uassertStatusOK(CanonicalQuery::canonicalize(txn, std::move(qr), ExtensionsCallbackNoop()));

    // Query validation
    if (QueryPlannerCommon::hasNode(cq->root(), MatchExpression::GEO_NEAR)) {
        uasserted(13502, "use geoNear command rather than $near query");
    }

    // Fast path for targeting equalities on the shard key.
    auto shardKeyToFind = _shardKeyPattern.extractShardKeyFromQuery(*cq);
    if (!shardKeyToFind.isEmpty()) {
        try {
            auto chunk = findIntersectingChunk(shardKeyToFind, collation);
            shardIds->insert(chunk->getShardId());
            return;
        } catch (const DBException&) {
            // The query uses multiple shards
        }
    }

    // Transforms query into bounds for each field in the shard key
    // for example :
    //   Key { a: 1, b: 1 },
    //   Query { a : { $gte : 1, $lt : 2 },
    //            b : { $gte : 3, $lt : 4 } }
    //   => Bounds { a : [1, 2), b : [3, 4) }
    IndexBounds bounds = getIndexBoundsForQuery(_shardKeyPattern.toBSON(), *cq);
    log() << "bounds = " << bounds.toString();

    // Transforms bounds for each shard key field into full shard key ranges
    // for example :
    //   Key { a : 1, b : 1 }
    //   Bounds { a : [1, 2), b : [3, 4) }
    //   => Ranges { a : 1, b : 3 } => { a : 2, b : 4 }
    BoundList ranges = _shardKeyPattern.flattenBounds(bounds);
    std::string str;
    for (BoundList::const_iterator it = ranges.begin(); it != ranges.end(); ++it) {
        str += "first=" + it->first.toString() + ",second=" + it->second.toString() + ";";
    }

    log() << "ranges  = " << str;

    for (BoundList::const_iterator it = ranges.begin(); it != ranges.end(); ++it) {
        getShardIdsForRange(it->first /*min*/, it->second /*max*/, shardIds);

        // once we know we need to visit all shards no need to keep looping
        if (shardIds->size() == _shardVersionSize) {
            break;
        }
    }

    // SERVER-4914 Some clients of getShardIdsForQuery() assume at least one shard will be returned.
    // For now, we satisfy that assumption by adding a shard with no matches rather than returning
    // an empty set of shards.
    if (shardIds->empty()) {
        boost::shared_lock<boost::shared_mutex> lock(_mutex);
        shardIds->insert(_shardVersions.begin()->first);
    }
}

void ChunkManagerWithLock::getShardIdsForRange(const BSONObj& min,
                                               const BSONObj& max,
                                               std::set<ShardId>* shardIds) const {
    boost::shared_lock<boost::shared_mutex> lock(_mutex);
    const auto bounds = _overlappingRanges(min, max, true);
    for (auto it = bounds.first; it != bounds.second; ++it) {

        shardIds->insert(it->second->getShardId());

        // No need to iterate through the rest of the ranges, because we already know we need to use
        // all shards.
        if (shardIds->size() == _shardVersionSize) {
            break;
        }
    }
}


std::pair<ChunkMap::const_iterator,
          ChunkMap::const_iterator>
ChunkManagerWithLock::_overlappingRanges(const mongo::BSONObj& min,
                                         const mongo::BSONObj& max,
                                         bool isMaxInclusive) const {

    const auto itMin = _chunkMap.upper_bound(_extractKeyString(min));
    const auto itMax = [this, &max, isMaxInclusive]() {
        auto it = isMaxInclusive ? _chunkMap.upper_bound(_extractKeyString(max))
                                 : _chunkMap.lower_bound(_extractKeyString(max));
        return it == _chunkMap.end() ? it : ++it;
    }();

    return {itMin, itMax};
/*
    // dassert(SimpleBSONObjComparator::kInstance.evaluate(min <= max));
    // const auto begin = _rangeMapUpperBound(min);
    // auto end = _rangeMapUpperBound(max);

    // // The chunk range map must always cover the entire key space
    // invariant(begin != _chunkMap.cend());

    // // Bump the end chunk, because the second iterator in the returned pair is exclusive. There is
    // // one caveat - if the exclusive max boundary of the range looked up is the same as the
    // // inclusive min of the end chunk returned, it is still possible that the min is not in the end
    // // chunk, in which case bumping the end will result in one extra chunk claimed to cover the
    // // range.
    // if (end != _chunkMap.cend() &&
    //     (isMaxInclusive || SimpleBSONObjComparator::kInstance.evaluate(max > end->second->getMin()))) {
    //     ++end;
    // }

    // return {begin, end};
    */
}


void ChunkManagerWithLock::getAllShardIds(std::set<ShardId>* all) const {
    boost::shared_lock<boost::shared_mutex> lock(_mutex);
    std::transform(_shardVersions.begin(),
                   _shardVersions.end(),
                   std::inserter(*all, all->begin()),
                   [](const ShardVersionMap::value_type& pair) { return pair.first; });
}

IndexBounds ChunkManagerWithLock::getIndexBoundsForQuery(const BSONObj& key,
                                                         const CanonicalQuery& canonicalQuery) {
    // $text is not allowed in planning since we don't have text index on mongos.
    // TODO: Treat $text query as a no-op in planning on mongos. So with shard key {a: 1},
    //       the query { a: 2, $text: { ... } } will only target to {a: 2}.
    if (QueryPlannerCommon::hasNode(canonicalQuery.root(), MatchExpression::TEXT)) {
        IndexBounds bounds;
        IndexBoundsBuilder::allValuesBounds(key, &bounds);  // [minKey, maxKey]
        return bounds;
    }

    // Consider shard key as an index
    std::string accessMethod = IndexNames::findPluginName(key);
    dassert(accessMethod == IndexNames::BTREE || accessMethod == IndexNames::HASHED);

    // Use query framework to generate index bounds
    QueryPlannerParams plannerParams;
    // Must use "shard key" index
    plannerParams.options = QueryPlannerParams::NO_TABLE_SCAN;
    IndexEntry indexEntry(key,
                          accessMethod,
                          false /* multiKey */,
                          MultikeyPaths{},
                          false /* sparse */,
                          false /* unique */,
                          "shardkey",
                          NULL /* filterExpr */,
                          BSONObj(),
                          NULL /* collator */);
    plannerParams.indices.push_back(indexEntry);

    OwnedPointerVector<QuerySolution> solutions;
    Status status = QueryPlanner::plan(canonicalQuery, plannerParams, &solutions.mutableVector());
    uassert(status.code(), status.reason(), status.isOK());

    IndexBounds bounds;

    for (std::vector<QuerySolution*>::const_iterator it = solutions.begin();
         bounds.size() == 0 && it != solutions.end();
         it++) {
        // Try next solution if we failed to generate index bounds, i.e. bounds.size() == 0
        bounds = collapseQuerySolution((*it)->root.get());
    }

    if (bounds.size() == 0) {
        // We cannot plan the query without collection scan, so target to all shards.
        IndexBoundsBuilder::allValuesBounds(key, &bounds);  // [minKey, maxKey]
    }
    return bounds;
}

IndexBounds ChunkManagerWithLock::collapseQuerySolution(const QuerySolutionNode* node) {
    if (node->children.empty()) {
        invariant(node->getType() == STAGE_IXSCAN);

        const IndexScanNode* ixNode = static_cast<const IndexScanNode*>(node);
        return ixNode->bounds;
    }

    if (node->children.size() == 1) {
        // e.g. FETCH -> IXSCAN
        return collapseQuerySolution(node->children.front());
    }

    // children.size() > 1, assert it's OR / SORT_MERGE.
    if (node->getType() != STAGE_OR && node->getType() != STAGE_SORT_MERGE) {
        // Unexpected node. We should never reach here.
        error() << "could not generate index bounds on query solution tree: "
                << redact(node->toString());
        dassert(false);  // We'd like to know this error in testing.

        // Bail out with all shards in production, since this isn't a fatal error.
        return IndexBounds();
    }

    IndexBounds bounds;

    for (std::vector<QuerySolutionNode*>::const_iterator it = node->children.begin();
         it != node->children.end();
         it++) {
        // The first branch under OR
        if (it == node->children.begin()) {
            invariant(bounds.size() == 0);
            bounds = collapseQuerySolution(*it);
            if (bounds.size() == 0) {  // Got unexpected node in query solution tree
                return IndexBounds();
            }
            continue;
        }

        IndexBounds childBounds = collapseQuerySolution(*it);
        if (childBounds.size() == 0) {
            // Got unexpected node in query solution tree
            return IndexBounds();
        }

        invariant(childBounds.size() == bounds.size());

        for (size_t i = 0; i < bounds.size(); i++) {
            bounds.fields[i].intervals.insert(bounds.fields[i].intervals.end(),
                                              childBounds.fields[i].intervals.begin(),
                                              childBounds.fields[i].intervals.end());
        }
    }

    for (size_t i = 0; i < bounds.size(); i++) {
        IndexBoundsBuilder::unionize(&bounds.fields[i]);
    }

    return bounds;
}

bool ChunkManagerWithLock::compatibleWith(const ChunkManagerWithLock& other,
                                          const ShardId& shardName) const {
    // Return true if the shard version is the same in the two chunk managers
    // TODO: This doesn't need to be so strong, just major vs
    return other.getVersion(shardName).equals(getVersion(shardName));
}

ChunkVersion ChunkManagerWithLock::getVersion(const ShardId& shardName) const {
    boost::shared_lock<boost::shared_mutex> lock(_mutex);
    auto it = _shardVersions.find(shardName);
    if (it == _shardVersions.end()) {
        // Shards without explicitly tracked shard versions (meaning they have no chunks) always
        // have a version of (0, 0, epoch)
        return ChunkVersion(0, 0, _collectionVersion.epoch());
    }
    log()<<"getVersion by shardname = "<<shardName<<",chunkversion="<<it->second.toString();
    return it->second;
}

std::string ChunkManagerWithLock::toString() const {
    StringBuilder sb;
    sb << "ChunkManager: " << _nss.ns() << " key: " << _shardKeyPattern.toString() << '\n';

    sb << "Chunks:\n";
    for (const auto& chunk : _chunkMap) {
        sb << "\t" << chunk.second->toString() << '\n';
    }

    sb << "Ranges:\n";
    // for (const auto& entry : _chunkMapViews.chunkRangeMap) {
    //     sb << "\t" << entry.range.toString() << " @ " << entry.shardId << '\n';
    // }

    sb << "Shard versions:\n";
    for (const auto& entry : _shardVersions) {
        sb << "\t" << entry.first << ": " << entry.second.toString() << '\n';
    }
    log()<<sb.str();
    return sb.str();
}

//按start和limit获取内存中的chunks信息，用来校验mongos的内存路由和configsvr中是否一致，内部使用
std::shared_ptr<IteratorChunks> ChunkManagerWithLock::iteratorChunks(int start, int limit) const{
    toString();
    std::shared_ptr<IteratorChunks> result = std::make_shared<IteratorChunks>();
    boost::shared_lock<boost::shared_mutex> lock(_mutex);
    if(start >= (int)_chunkMap.size()){
        result->hashErr = true;
        result->errmsg = "start is more than chunksSize.";
        return result;
    }
    auto itr = _chunkMap.begin();
    std::advance(itr, start);
    if(itr == _chunkMap.end()){
        result->hashErr = true;
        result->errmsg = "iterator overflow.";
        return result;
    }
    
    for(; itr != _chunkMap.end(); itr++){
        if(limit <= 0){
            break;
        }
        BSONObjBuilder bson;
        bson.append("min",itr->second->getMin());
        bson.append("max",itr->second->getMax());
        bson.append("shard",itr->second->getShardId().toString());
        auto obj = bson.obj();
        //log()<<"bson:"<<obj.toString();
        result->bson.append(obj);
        limit --;
    }

    result->chunksSize = _chunkMap.size();
    return result;

}


ShardVersionMap ChunkManagerWithLock::_constructShardVersionMap(const OID& epoch,
                                                                const ChunkMap& chunkMap,
                                                                Ordering shardKeyOrdering) {

    ShardVersionMap shardVersions;
    Timer timer;
    int build_cnt = 0;
    boost::optional<BSONObj> firstMin = boost::none;
    boost::optional<BSONObj> lastMax = boost::none;

    log() << "chunkMap  size = " << chunkMap.size();
    ChunkMap::const_iterator current = chunkMap.cbegin();

    while (current != chunkMap.cend()) {
        build_cnt++;
        const auto& firstChunkInRange = current->second;

        // Tracks the max shard version for the shard on which the current range will reside

        auto shardVersionIt = shardVersions.find(firstChunkInRange->getShardId());
        if (shardVersionIt == shardVersions.end()) {
            log()<<"push shardid="<<firstChunkInRange->getShardId()<<" to shardVersions.";
            shardVersionIt =
                shardVersions.emplace(firstChunkInRange->getShardId(), ChunkVersion(0, 0, epoch))
                    .first;
        }

        auto& maxShardVersion = shardVersionIt->second;

        current = std::find_if(
            current,
            chunkMap.cend(),
            [&firstChunkInRange, &maxShardVersion](const ChunkMap::value_type& chunkMapEntry) {
                const auto& currentChunk = chunkMapEntry.second;

                if (currentChunk->getShardId() != firstChunkInRange->getShardId())
                    return true;

                if (currentChunk->getLastmod() > maxShardVersion)
                    maxShardVersion = currentChunk->getLastmod();

                // log() << " find_if false currentChunk->getShardId()=" << currentChunk->getShardId()
                //       << ",firstChunkInRange->getShardId()=" << firstChunkInRange->getShardId();
                return false;
            });

        const auto rangeLast = std::prev(current);

        const BSONObj rangeMin = firstChunkInRange->getMin();
        const BSONObj rangeMax = rangeLast->second->getMax();
        if (!firstMin)
            firstMin = rangeMin;

        lastMax = rangeMax;
    }
    log() << "build _constructShardVersionMap time=" << timer.millis()
          << "ms,build cnt=" << build_cnt;
    if(!chunkMap.empty()){
        invariant(!shardVersions.empty());
        invariant(firstMin.is_initialized());
        invariant(lastMax.is_initialized());

        checkAllElementsAreOfType(MinKey, firstMin.get());
        checkAllElementsAreOfType(MaxKey, lastMax.get());

    }
    
    return shardVersions;
}


std::string ChunkManagerWithLock::_extractKeyString(const BSONObj& shardKeyValue) const {
    return extractKeyStringInternalWithLock(shardKeyValue, _shardKeyOrdering);
}

std::shared_ptr<ChunkManagerWithLock> ChunkManagerWithLock::makeNew(
    NamespaceString nss,
    KeyPattern shardKeyPattern,
    std::unique_ptr<CollatorInterface> defaultCollator,
    bool unique,
    OID epoch,
    const std::vector<ChunkType>& chunks) {
    log()<<"chunk manager with lock make new. chunks.size =" << chunks.size();
    return  ChunkManagerWithLock(std::move(nss),
                                    std::move(shardKeyPattern),
                                    std::move(defaultCollator),
                                    std::move(unique),
                                    {},
                                    {0, 0, epoch})
                                    .makeUpdated(chunks);
}

//只有从无到有的构建chunkmap
std::shared_ptr<ChunkManagerWithLock> ChunkManagerWithLock::makeUpdated(
    const std::vector<ChunkType>& changedChunks) {

    const auto startingCollectionVersion = getVersion();
    Timer timer;
    auto chunkMap = _chunkMap;
    log() << "copy chunkMap time=" << timer.millis() << "ms";
    ChunkVersion collectionVersion = startingCollectionVersion;
    for (const auto& chunk : changedChunks) {
        const auto& chunkVersion = chunk.getVersion();

        uassert(ErrorCodes::ConflictingOperationInProgress,
                str::stream() << "Chunk " << chunk.genID(getns(), chunk.getMin())
                              << " has epoch different from that of the collection "
                              << chunkVersion.epoch(),
                collectionVersion.epoch() == chunkVersion.epoch());

        // Chunks must always come in incrementally sorted order
        invariant(chunkVersion >= collectionVersion);
        collectionVersion = chunkVersion;

        const auto chunkMinKeyString = _extractKeyString(chunk.getMin());
        const auto chunkMaxKeyString = _extractKeyString(chunk.getMax());

        // Returns the first chunk with a max key that is > min - implies that the chunk overlaps
        // min
        const auto low = chunkMap.upper_bound(chunkMinKeyString);

        // Returns the first chunk with a max key that is > max - implies that the next chunk cannot
        // not overlap max
        const auto high = chunkMap.upper_bound(chunkMaxKeyString);

        // Erase all chunks from the map, which overlap the chunk we got from the persistent store
        chunkMap.erase(low, high);

        // Insert only the chunk itself
        chunkMap.insert(std::make_pair(chunkMaxKeyString, std::make_shared<Chunk>(chunk)));
    }

    // If at least one diff was applied, the metadata is correct, but it might not have changed so
    // in this case there is no need to recreate the chunk manager.
    //
    // NOTE: In addition to the above statement, it is also important that we return the same chunk
    // manager object, because the write commands' code relies on changes of the chunk manager's
    // sequence number to detect batch writes not making progress because of chunks moving across
    // shards too frequently.
    if (collectionVersion == startingCollectionVersion) {
        return shared_from_this();
    }

    return std::shared_ptr<ChunkManagerWithLock>(
        new ChunkManagerWithLock(_nss,
                                 KeyPattern(getShardKeyPattern().getKeyPattern()),
                                 CollatorInterface::cloneCollator(getDefaultCollator()),
                                 isUnique(),
                                 std::move(chunkMap),
                                 collectionVersion));
}

void ChunkManagerWithLock::UpdateChunksMap(const std::vector<ChunkType>& changedChunks) {
    log()<<"chunk manager with lock UpdateChunksMap.changedChunks.size = " << changedChunks.size();

    Timer timer;
    const auto startingCollectionVersion = getVersion();
    
    ChunkVersion collectionVersion = startingCollectionVersion;

    for (const auto& chunk : changedChunks) {
        const auto& chunkVersion = chunk.getVersion();

        uassert(ErrorCodes::ConflictingOperationInProgress,
                str::stream() << "Chunk " << chunk.genID(getns(), chunk.getMin())
                              << " has epoch different from that of the collection "
                              << chunkVersion.epoch(),
                collectionVersion.epoch() == chunkVersion.epoch());

        // Chunks must always come in incrementally sorted order
        invariant(chunkVersion >= collectionVersion);
        collectionVersion = chunkVersion;
        const auto chunkMinKeyString = _extractKeyString(chunk.getMin());
        const auto chunkMaxKeyString = _extractKeyString(chunk.getMax());


        boost::unique_lock<boost::shared_mutex> lock(_mutex);
        // Returns the first chunk with a max key that is > min - implies that the chunk overlaps
        // min

        const auto low = _chunkMap.upper_bound(chunkMinKeyString);
        //const auto low = _chunkMap.upper_bound(chunkMaxKeyString);
        // Returns the first chunk with a max key that is > max - implies that the next chunk cannot
        // not overlap max
        const auto high = _chunkMap.upper_bound(chunkMaxKeyString);


        // Erase all chunks from the map, which overlap the chunk we got from the persistent store
        _chunkMap.erase(low, high);

        // Insert only the chunk itself
        //这里用下标也可以，但是insert也是安全的，因为如果这个key的chunk发生变化，在changedChunks中一定有一个chunk的max和key相等
        //low 会查到key，high会查到key的下一个，所以erase一定能删掉key的元素
        _chunkMap.insert(std::make_pair(chunkMaxKeyString, std::make_shared<Chunk>(chunk)));

        auto shardVersionIt = _shardVersions.find(chunk.getShard());
        if (shardVersionIt == _shardVersions.end()) {
            log()<<"push shardid="<<chunk.getShard()<<" to shardVersions. UpdateChunksMap";
            _shardVersions.emplace(chunk.getShard(), chunk.getVersion());
        } else if (chunk.getVersion() > shardVersionIt->second) {
            shardVersionIt->second = chunk.getVersion();
        }
    }

    boost::unique_lock<boost::shared_mutex> lock(_mutex);

    _shardVersionSize = _shardVersions.size();
    if(_collectionVersion == collectionVersion){
        //这里不用处理
    }else{
        _sequenceNumber = nextCMILSequenceNumber.addAndFetch(1);
    }
     _collectionVersion = collectionVersion;
   
    log() << "UpdateChunksMap time=" << timer.millis() << "ms";
}


    ChunkVersion ChunkManagerWithLock::getVersion() const {
        log()<<"chunk_manager_version"<<_collectionVersion.toString();
        return _collectionVersion;
    }
}  // namespace mongo
