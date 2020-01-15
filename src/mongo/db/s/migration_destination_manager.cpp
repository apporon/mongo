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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/migration_destination_manager.h"

#include <list>
#include <vector>

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/ops/write_ops_exec.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/migration_util.h"
#include "mongo/db/s/move_timing_helper.h"
#include "mongo/db/s/persistent_task_store.h"
#include "mongo/db/s/range_deletion_task_gen.h"
#include "mongo/db/s/sharding_runtime_d_params_gen.h"
#include "mongo/db/s/sharding_statistics.h"
#include "mongo/db/s/start_chunk_clone_request.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/remove_saver.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/stdx/chrono.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/log.h"
#include "mongo/util/producer_consumer_queue.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {

const auto getMigrationDestinationManager =
    ServiceContext::declareDecoration<MigrationDestinationManager>();

const WriteConcernOptions kMajorityWriteConcern(WriteConcernOptions::kMajority,
                                                // Note: Even though we're setting UNSET here,
                                                // kMajority implies JOURNAL if journaling is
                                                // supported by mongod and
                                                // writeConcernMajorityJournalDefault is set to true
                                                // in the ReplSetConfig.
                                                WriteConcernOptions::SyncMode::UNSET,
                                                -1);

/**
 * Returns a human-readabale name of the migration manager's state.
 */
std::string stateToString(MigrationDestinationManager::State state) {
    switch (state) {
        case MigrationDestinationManager::READY:
            return "ready";
        case MigrationDestinationManager::CLONE:
            return "clone";
        case MigrationDestinationManager::CATCHUP:
            return "catchup";
        case MigrationDestinationManager::STEADY:
            return "steady";
        case MigrationDestinationManager::COMMIT_START:
            return "commitStart";
        case MigrationDestinationManager::DONE:
            return "done";
        case MigrationDestinationManager::FAIL:
            return "fail";
        case MigrationDestinationManager::ABORT:
            return "abort";
        default:
            MONGO_UNREACHABLE;
    }
}

bool isInRange(const BSONObj& obj,
               const BSONObj& min,
               const BSONObj& max,
               const BSONObj& shardKeyPattern) {
    ShardKeyPattern shardKey(shardKeyPattern);
    BSONObj k = shardKey.extractShardKeyFromDoc(obj);
    return k.woCompare(min) >= 0 && k.woCompare(max) < 0;
}

/**
 * Checks if an upsert of a remote document will override a local document with the same _id but in
 * a different range on this shard. Must be in WriteContext to avoid races and DBHelper errors.
 *
 * TODO: Could optimize this check out if sharding on _id.
 */
bool willOverrideLocalId(OperationContext* opCtx,
                         const NamespaceString& nss,
                         BSONObj min,
                         BSONObj max,
                         BSONObj shardKeyPattern,
                         Database* db,
                         BSONObj remoteDoc,
                         BSONObj* localDoc) {
    *localDoc = BSONObj();
    if (Helpers::findById(opCtx, db, nss.ns(), remoteDoc, *localDoc)) {
        return !isInRange(*localDoc, min, max, shardKeyPattern);
    }

    return false;
}

/**
 * Returns true if the majority of the nodes and the nodes corresponding to the given writeConcern
 * (if not empty) have applied till the specified lastOp.
 */
bool opReplicatedEnough(OperationContext* opCtx,
                        const repl::OpTime& lastOpApplied,
                        const WriteConcernOptions& writeConcern) {
    WriteConcernResult writeConcernResult;
    writeConcernResult.wTimedOut = false;

    Status majorityStatus =
        waitForWriteConcern(opCtx, lastOpApplied, kMajorityWriteConcern, &writeConcernResult);
    if (!majorityStatus.isOK()) {
        if (!writeConcernResult.wTimedOut) {
            uassertStatusOK(majorityStatus);
        }
        return false;
    }

    // Enforce the user specified write concern after "majority" so it covers the union of the 2
    // write concerns in case the user's write concern is stronger than majority
    WriteConcernOptions userWriteConcern(writeConcern);
    userWriteConcern.wTimeout = -1;
    writeConcernResult.wTimedOut = false;

    Status userStatus =
        waitForWriteConcern(opCtx, lastOpApplied, userWriteConcern, &writeConcernResult);
    if (!userStatus.isOK()) {
        if (!writeConcernResult.wTimedOut) {
            uassertStatusOK(userStatus);
        }
        return false;
    }
    return true;
}

/**
 * Create the migration clone request BSON object to send to the source shard.
 *
 * 'sessionId' unique identifier for this migration.
 */
BSONObj createMigrateCloneRequest(const NamespaceString& nss, const MigrationSessionId& sessionId) {
    BSONObjBuilder builder;
    builder.append("_migrateClone", nss.ns());
    sessionId.append(&builder);
    return builder.obj();
}

/**
 * Create the migration transfer mods request BSON object to send to the source shard.
 *
 * 'sessionId' unique identifier for this migration.
 */
BSONObj createTransferModsRequest(const NamespaceString& nss, const MigrationSessionId& sessionId) {
    BSONObjBuilder builder;
    builder.append("_transferMods", nss.ns());
    sessionId.append(&builder);
    return builder.obj();
}

// Enabling / disabling these fail points pauses / resumes MigrateStatus::_go(), the thread which
// receives a chunk migration from the donor.
MONGO_FAIL_POINT_DEFINE(migrateThreadHangAtStep1);
MONGO_FAIL_POINT_DEFINE(migrateThreadHangAtStep2);
MONGO_FAIL_POINT_DEFINE(migrateThreadHangAtStep3);
MONGO_FAIL_POINT_DEFINE(migrateThreadHangAtStep4);
MONGO_FAIL_POINT_DEFINE(migrateThreadHangAtStep5);
MONGO_FAIL_POINT_DEFINE(migrateThreadHangAtStep6);

MONGO_FAIL_POINT_DEFINE(failMigrationOnRecipient);
MONGO_FAIL_POINT_DEFINE(failMigrationReceivedOutOfRangeOperation);

}  // namespace

MigrationDestinationManager::MigrationDestinationManager() = default;

MigrationDestinationManager::~MigrationDestinationManager() = default;

MigrationDestinationManager* MigrationDestinationManager::get(OperationContext* opCtx) {
    return &getMigrationDestinationManager(opCtx->getServiceContext());
}

MigrationDestinationManager::State MigrationDestinationManager::getState() const {
    stdx::lock_guard<Latch> sl(_mutex);
    return _state;
}

void MigrationDestinationManager::setState(State newState) {
    stdx::lock_guard<Latch> sl(_mutex);
    _state = newState;
    _stateChangedCV.notify_all();
}

void MigrationDestinationManager::_setStateFail(StringData msg) {
    log() << msg;
    {
        stdx::lock_guard<Latch> sl(_mutex);
        _errmsg = msg.toString();
        _state = FAIL;
        _stateChangedCV.notify_all();
    }

    _sessionMigration->forceFail(msg);
}

void MigrationDestinationManager::_setStateFailWarn(StringData msg) {
    warning() << msg;
    {
        stdx::lock_guard<Latch> sl(_mutex);
        _errmsg = msg.toString();
        _state = FAIL;
        _stateChangedCV.notify_all();
    }

    _sessionMigration->forceFail(msg);
}

bool MigrationDestinationManager::isActive() const {
    stdx::lock_guard<Latch> lk(_mutex);
    return _isActive(lk);
}

bool MigrationDestinationManager::_isActive(WithLock) const {
    return _sessionId.is_initialized();
}

void MigrationDestinationManager::report(BSONObjBuilder& b,
                                         OperationContext* opCtx,
                                         bool waitForSteadyOrDone) {
    if (waitForSteadyOrDone) {
        stdx::unique_lock<Latch> lock(_mutex);
        try {
            opCtx->waitForConditionOrInterruptFor(_stateChangedCV, lock, Seconds(1), [&]() -> bool {
                return _state != READY && _state != CLONE && _state != CATCHUP;
            });
        } catch (...) {
            // Ignoring this error because this is an optional parameter and we catch timeout
            // exceptions later.
        }
        b.append("waited", true);
    }
    stdx::lock_guard<Latch> sl(_mutex);

    b.appendBool("active", _sessionId.is_initialized());

    if (_sessionId) {
        b.append("sessionId", _sessionId->toString());
    }

    b.append("ns", _nss.ns());
    b.append("from", _fromShardConnString.toString());
    b.append("fromShardId", _fromShard.toString());
    b.append("min", _min);
    b.append("max", _max);
    b.append("shardKeyPattern", _shardKeyPattern);

    b.append("state", stateToString(_state));

    if (_state == FAIL) {
        invariant(!_errmsg.empty());
        b.append("errmsg", _errmsg);
    }

    BSONObjBuilder bb(b.subobjStart("counts"));
    bb.append("cloned", _numCloned);
    bb.append("clonedBytes", _clonedBytes);
    bb.append("catchup", _numCatchup);
    bb.append("steady", _numSteady);
    bb.done();
}

BSONObj MigrationDestinationManager::getMigrationStatusReport() {
    stdx::lock_guard<Latch> lk(_mutex);
    if (_isActive(lk)) {
        return migrationutil::makeMigrationStatusDocument(
            _nss, _fromShard, _toShard, false, _min, _max);
    } else {
        return BSONObj();
    }
}

Status MigrationDestinationManager::start(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          ScopedReceiveChunk scopedReceiveChunk,
                                          const StartChunkCloneRequest cloneRequest,
                                          const OID& epoch,
                                          const WriteConcernOptions& writeConcern) {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(!_sessionId);
    invariant(!_scopedReceiveChunk);

    auto fcvVersion = serverGlobalParams.featureCompatibility.getVersion();
    if (fcvVersion == ServerGlobalParams::FeatureCompatibility::Version::kUpgradingTo44 ||
        fcvVersion == ServerGlobalParams::FeatureCompatibility::Version::kDowngradingTo42)
        return Status(ErrorCodes::ConflictingOperationInProgress,
                      "Can't receive chunk while FCV is upgrading/downgrading");

    // Note: It is expected that the FCV cannot change while the node is donating or receiving a
    // chunk. This is guaranteed by the setFCV command serializing with donating and receiving
    // chunks via the ActiveMigrationsRegistry.
    _useFCV44Protocol =
        fcvVersion == ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo44;

    _state = READY;
    _stateChangedCV.notify_all();
    _errmsg = "";

    if (_useFCV44Protocol) {
        uassert(ErrorCodes::ConflictingOperationInProgress,
                "Missing migrationId in FCV 4.4",
                cloneRequest.hasMigrationId());

        _migrationId = cloneRequest.getMigrationId();
    }

    _nss = nss;
    _fromShard = cloneRequest.getFromShardId();
    _fromShardConnString =
        uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, _fromShard))
            ->getConnString();
    _toShard = cloneRequest.getToShardId();
    _min = cloneRequest.getMinKey();
    _max = cloneRequest.getMaxKey();
    _shardKeyPattern = cloneRequest.getShardKeyPattern();

    _epoch = epoch;

    _writeConcern = writeConcern;

    _chunkMarkedPending = false;

    _numCloned = 0;
    _clonedBytes = 0;
    _numCatchup = 0;
    _numSteady = 0;

    _sessionId = cloneRequest.getSessionId();
    _scopedReceiveChunk = std::move(scopedReceiveChunk);

    // TODO: If we are here, the migrate thread must have completed, otherwise _active above
    // would be false, so this would never block. There is no better place with the current
    // implementation where to join the thread.
    if (_migrateThreadHandle.joinable()) {
        _migrateThreadHandle.join();
    }

    _sessionMigration =
        std::make_unique<SessionCatalogMigrationDestination>(_fromShard, *_sessionId);
    ShardingStatistics::get(opCtx).countRecipientMoveChunkStarted.addAndFetch(1);

    _migrateThreadHandle = stdx::thread([this]() { _migrateThread(); });

    return Status::OK();
}

repl::OpTime MigrationDestinationManager::cloneDocumentsFromDonor(
    OperationContext* opCtx,
    std::function<void(OperationContext*, BSONObj)> insertBatchFn,
    std::function<BSONObj(OperationContext*)> fetchBatchFn) {

    SingleProducerSingleConsumerQueue<BSONObj>::Options options;
    options.maxQueueDepth = 1;

    SingleProducerSingleConsumerQueue<BSONObj> batches(options);
    repl::OpTime lastOpApplied;

    stdx::thread inserterThread{[&] {
        Client::initKillableThread("chunkInserter", opCtx->getServiceContext());

        auto inserterOpCtx = Client::getCurrent()->makeOperationContext();
        auto consumerGuard = makeGuard([&] {
            batches.closeConsumerEnd();
            lastOpApplied = repl::ReplClientInfo::forClient(inserterOpCtx->getClient()).getLastOp();
        });

        try {
            while (true) {
                auto nextBatch = batches.pop(inserterOpCtx.get());
                auto arr = nextBatch["objects"].Obj();
                if (arr.isEmpty()) {
                    return;
                }
                insertBatchFn(inserterOpCtx.get(), arr);
            }
        } catch (...) {
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            opCtx->getServiceContext()->killOperation(lk, opCtx, ErrorCodes::Error(51008));
            log() << "Batch insertion failed " << causedBy(redact(exceptionToStatus()));
        }
    }};
    auto inserterThreadJoinGuard = makeGuard([&] {
        batches.closeProducerEnd();
        inserterThread.join();
    });

    while (true) {
        opCtx->checkForInterrupt();

        auto res = fetchBatchFn(opCtx);

        opCtx->checkForInterrupt();
        batches.push(res.getOwned(), opCtx);
        auto arr = res["objects"].Obj();
        if (arr.isEmpty()) {
            inserterThreadJoinGuard.dismiss();
            inserterThread.join();
            opCtx->checkForInterrupt();
            break;
        }
    }

    return lastOpApplied;
}

Status MigrationDestinationManager::abort(const MigrationSessionId& sessionId) {
    stdx::lock_guard<Latch> sl(_mutex);

    if (!_sessionId) {
        return Status::OK();
    }

    if (!_sessionId->matches(sessionId)) {
        return {ErrorCodes::CommandFailed,
                str::stream() << "received abort request from a stale session "
                              << sessionId.toString() << ". Current session is "
                              << _sessionId->toString()};
    }

    _state = ABORT;
    _stateChangedCV.notify_all();
    _errmsg = "aborted";

    return Status::OK();
}

void MigrationDestinationManager::abortWithoutSessionIdCheck() {
    stdx::lock_guard<Latch> sl(_mutex);
    _state = ABORT;
    _stateChangedCV.notify_all();
    _errmsg = "aborted without session id check";
}

Status MigrationDestinationManager::startCommit(const MigrationSessionId& sessionId) {

    stdx::unique_lock<Latch> lock(_mutex);

    if (_state != STEADY) {
        return {ErrorCodes::CommandFailed,
                str::stream() << "Migration startCommit attempted when not in STEADY state."
                              << " Sender's session is " << sessionId.toString()
                              << (_sessionId ? (". Current session is " + _sessionId->toString())
                                             : ". No active session on this shard.")};
    }

    // In STEADY state we must have active migration
    invariant(_sessionId);

    // This check guards against the (unusual) situation where the current donor shard has stalled,
    // during which the recipient shard crashed or timed out, and then began serving as a recipient
    // or donor for another migration.
    if (!_sessionId->matches(sessionId)) {
        return {ErrorCodes::CommandFailed,
                str::stream() << "startCommit received commit request from a stale session "
                              << sessionId.toString() << ". Current session is "
                              << _sessionId->toString()};
    }

    _sessionMigration->finish();
    _state = COMMIT_START;
    _stateChangedCV.notify_all();

    auto const deadline = Date_t::now() + Seconds(30);
    while (_sessionId) {
        if (stdx::cv_status::timeout ==
            _isActiveCV.wait_until(lock, deadline.toSystemTimePoint())) {
            _errmsg = str::stream() << "startCommit timed out waiting, " << _sessionId->toString();
            _state = FAIL;
            _stateChangedCV.notify_all();
            return {ErrorCodes::CommandFailed, _errmsg};
        }
    }
    if (_state != DONE) {
        return {ErrorCodes::CommandFailed, "startCommit failed, final data failed to transfer"};
    }

    return Status::OK();
}

void MigrationDestinationManager::cloneCollectionIndexesAndOptions(OperationContext* opCtx,
                                                                   const NamespaceString& nss,
                                                                   const ShardId& fromShardId) {
    auto fromShard =
        uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, fromShardId));

    DisableDocumentValidation validationDisabler(opCtx);

    std::vector<BSONObj> donorIndexSpecs;
    BSONObj donorIdIndexSpec;
    BSONObj donorOptions;
    {
        // 0. Get the collection indexes and options from the donor shard.

        // Do not hold any locks while issuing remote calls.
        invariant(!opCtx->lockState()->isLocked());

        // Get indexes by calling listIndexes against the donor.
        auto indexes = uassertStatusOK(fromShard->runExhaustiveCursorCommand(
            opCtx,
            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
            nss.db().toString(),
            BSON("listIndexes" << nss.coll().toString()),
            Milliseconds(-1)));

        for (auto&& spec : indexes.docs) {
            donorIndexSpecs.push_back(spec);
            if (auto indexNameElem = spec[IndexDescriptor::kIndexNameFieldName]) {
                if (indexNameElem.type() == BSONType::String &&
                    indexNameElem.valueStringData() == "_id_"_sd) {
                    donorIdIndexSpec = spec;
                }
            }
        }

        // Get collection options by calling listCollections against the donor.
        auto infosRes = uassertStatusOK(fromShard->runExhaustiveCursorCommand(
            opCtx,
            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
            nss.db().toString(),
            BSON("listCollections" << 1 << "filter" << BSON("name" << nss.coll())),
            Milliseconds(-1)));

        auto infos = infosRes.docs;
        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "expected listCollections against the primary shard for "
                              << nss.toString() << " to return 1 entry, but got " << infos.size()
                              << " entries",
                infos.size() == 1);


        BSONObj entry = infos.front();

        // The entire options include both the settable options under the 'options' field in the
        // listCollections response, and the UUID under the 'info' field.
        BSONObjBuilder donorOptionsBob;

        if (entry["options"].isABSONObj()) {
            donorOptionsBob.appendElements(entry["options"].Obj());
        }

        BSONObj info;
        if (entry["info"].isABSONObj()) {
            info = entry["info"].Obj();
        }

        uassert(ErrorCodes::InvalidUUID,
                str::stream() << "The donor shard did not return a UUID for collection " << nss.ns()
                              << " as part of its listCollections response: " << entry
                              << ", but this node expects to see a UUID.",
                !info["uuid"].eoo());

        donorOptionsBob.append(info["uuid"]);

        donorOptions = donorOptionsBob.obj();
    }

    // 1. If this shard doesn't own any chunks for the collection to be cloned and the collection
    // exists locally, we drop its indexes to guarantee that no stale indexes carry over.
    bool dropNonDonorIndexes = [&]() -> bool {
        AutoGetCollection autoColl(opCtx, nss, MODE_IS);
        auto* const css = CollectionShardingRuntime::get(opCtx, nss);
        const auto optMetadata = css->getCurrentMetadataIfKnown();

        // Only attempt to drop a collection's indexes if we have valid metadata and the
        // collection is sharded.
        if (optMetadata) {
            const auto& metadata = optMetadata->get();
            if (metadata.isSharded()) {
                auto chunks = metadata.getChunks();
                if (chunks.empty()) {
                    return true;
                }
            }
        }
        return false;
    }();

    if (dropNonDonorIndexes) {
        // Determine which indexes exist on the local collection that don't exist on the donor's
        // collection.
        DBDirectClient client(opCtx);
        auto indexes = client.getIndexSpecs(nss);
        for (auto&& recipientIndex : indexes) {
            bool dropIndex = true;
            for (auto&& donorIndex : donorIndexSpecs) {
                if (recipientIndex.woCompare(donorIndex) == 0) {
                    dropIndex = false;
                    break;
                }
            }
            // If the local index doesn't exist on the donor and isn't the _id index, drop it.
            auto indexNameElem = recipientIndex[IndexDescriptor::kIndexNameFieldName];
            if (indexNameElem.type() == BSONType::String && dropIndex &&
                !IndexDescriptor::isIdIndexPattern(
                    recipientIndex[IndexDescriptor::kKeyPatternFieldName].Obj())) {
                BSONObj info;
                if (!client.runCommand(
                        nss.db().toString(),
                        BSON("dropIndexes" << nss.coll() << "index" << indexNameElem),
                        info))
                    uassertStatusOK(getStatusFromCommandResult(info));
            }
        }
    }

    {
        // 2. Create the collection (if it doesn't already exist) and create any indexes we are
        // missing (auto-heal indexes).

        // Checks that the collection's UUID matches the donor's.
        auto checkUUIDsMatch = [&](const Collection* collection) {
            uassert(ErrorCodes::NotMaster,
                    str::stream() << "Unable to create collection " << nss.ns()
                                  << " because the node is not primary",
                    repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, nss));

            boost::optional<UUID> donorUUID;
            if (!donorOptions["uuid"].eoo()) {
                donorUUID.emplace(UUID::parse(donorOptions));
            }

            uassert(ErrorCodes::InvalidUUID,
                    str::stream()
                        << "Cannot create collection " << nss.ns()
                        << " because we already have an identically named collection with UUID "
                        << collection->uuid() << ", which differs from the donor's UUID "
                        << (donorUUID ? donorUUID->toString() : "(none)")
                        << ". Manually drop the collection on this shard if it contains data from "
                           "a previous incarnation of "
                        << nss.ns(),
                    collection->uuid() == donorUUID);
        };

        // Gets the missing indexes and checks if the collection is empty (auto-healing is
        // possible).
        auto checkEmptyOrGetMissingIndexesFromDonor = [&](Collection* collection) {
            auto indexCatalog = collection->getIndexCatalog();
            auto indexSpecs = indexCatalog->removeExistingIndexesNoChecks(opCtx, donorIndexSpecs);
            if (!indexSpecs.empty()) {
                // Only allow indexes to be copied if the collection does not have any documents.
                uassert(ErrorCodes::CannotCreateCollection,
                        str::stream()
                            << "aborting, shard is missing " << indexSpecs.size() << " indexes and "
                            << "collection is not empty. Non-trivial "
                            << "index creation should be scheduled manually",
                        collection->numRecords(opCtx) == 0);
            }
            return indexSpecs;
        };

        {
            AutoGetCollection autoGetCollection(opCtx, nss, MODE_IS);

            auto collection = autoGetCollection.getCollection();
            if (collection) {
                checkUUIDsMatch(collection);
                auto indexSpecs = checkEmptyOrGetMissingIndexesFromDonor(collection);
                if (indexSpecs.empty()) {
                    return;
                }
            }
        }

        // Take the exclusive database lock if the collection does not exist or indexes are missing
        // (needs auto-heal).
        AutoGetOrCreateDb autoCreateDb(opCtx, nss.db(), MODE_X);
        auto db = autoCreateDb.getDb();

        auto collection = CollectionCatalog::get(opCtx).lookupCollectionByNamespace(opCtx, nss);
        if (collection) {
            checkUUIDsMatch(collection);
        } else {
            // We do not have a collection by this name. Create the collection with the donor's
            // options.
            WriteUnitOfWork wuow(opCtx);
            CollectionOptions collectionOptions = uassertStatusOK(CollectionOptions::parse(
                donorOptions, CollectionOptions::ParseKind::parseForStorage));
            const bool createDefaultIndexes = true;
            uassertStatusOK(db->userCreateNS(
                opCtx, nss, collectionOptions, createDefaultIndexes, donorIdIndexSpec));
            wuow.commit();
            collection = CollectionCatalog::get(opCtx).lookupCollectionByNamespace(opCtx, nss);
        }

        auto indexSpecs = checkEmptyOrGetMissingIndexesFromDonor(collection);
        if (!indexSpecs.empty()) {
            WriteUnitOfWork wunit(opCtx);
            auto fromMigrate = true;
            IndexBuildsCoordinator::get(opCtx)->createIndexesOnEmptyCollection(
                opCtx, collection->uuid(), indexSpecs, fromMigrate);
            wunit.commit();
        }
    }
}

void MigrationDestinationManager::_migrateThread() {
    Client::initKillableThread("migrateThread", getGlobalServiceContext());
    auto opCtx = Client::getCurrent()->makeOperationContext();

    if (AuthorizationManager::get(opCtx->getServiceContext())->isAuthEnabled()) {
        AuthorizationSession::get(opCtx->getClient())->grantInternalAuthorization(opCtx.get());
    }

    try {
        _migrateDriver(opCtx.get());
    } catch (...) {
        _setStateFail(str::stream() << "migrate failed: " << redact(exceptionToStatus()));
    }

    if (!_useFCV44Protocol) {
        if (getState() != DONE) {
            _forgetPending(opCtx.get(), ChunkRange(_min, _max));
        }
    }

    stdx::lock_guard<Latch> lk(_mutex);
    _sessionId.reset();
    _scopedReceiveChunk.reset();
    _isActiveCV.notify_all();
}

void MigrationDestinationManager::_migrateDriver(OperationContext* opCtx) {
    invariant(isActive());
    invariant(_sessionId);
    invariant(_scopedReceiveChunk);
    invariant(!_min.isEmpty());
    invariant(!_max.isEmpty());

    log() << "Starting receiving end of migration of chunk " << redact(_min) << " -> "
          << redact(_max) << " for collection " << _nss.ns() << " from " << _fromShard
          << " at epoch " << _epoch.toString() << " with session id " << *_sessionId;

    MoveTimingHelper timing(
        opCtx, "to", _nss.ns(), _min, _max, 6 /* steps */, &_errmsg, ShardId(), ShardId());

    const auto initialState = getState();

    if (initialState == ABORT) {
        error() << "Migration abort requested before it started";
        return;
    }

    invariant(initialState == READY);

    {
        cloneCollectionIndexesAndOptions(opCtx, _nss, _fromShard);

        timing.done(1);
        migrateThreadHangAtStep1.pauseWhileSet();
    }

    auto fromShard =
        uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, _fromShard));

    const UUID collectionUuid = [&] {
        AutoGetCollection autoGetCollection(opCtx, _nss, MODE_IS);
        return autoGetCollection.getCollection()->uuid();
    }();

    {
        const ChunkRange range(_min, _max);

        // 2. Ensure any data which might have been left orphaned in the range being moved has been
        // deleted.
        if (_useFCV44Protocol) {
            while (migrationutil::checkForConflictingDeletions(opCtx, range, collectionUuid)) {
                LOG(0) << "Migration paused because range overlaps with a "
                          "range that is scheduled for deletion: collection: "
                       << _nss.ns() << " range: " << redact(range.toString());

                auto status = CollectionShardingRuntime::waitForClean(opCtx, _nss, _epoch, range);

                if (!status.isOK()) {
                    _setStateFail(redact(status.reason()));
                    return;
                }

                opCtx->sleepFor(Milliseconds(1000));
            }

            RangeDeletionTask recipientDeletionTask(
                _migrationId, _nss, collectionUuid, _fromShard, range, CleanWhenEnum::kNow);
            recipientDeletionTask.setPending(true);

            migrationutil::persistRangeDeletionTaskLocally(opCtx, recipientDeletionTask);
        } else {
            // Synchronously delete any data which might have been left orphaned in the range
            // being moved, and wait for completion

            auto cleanupCompleteFuture = _notePending(opCtx, range);
            // Wait for the range deletion to report back
            if (!cleanupCompleteFuture.getNoThrow(opCtx).isOK()) {
                _setStateFail(redact(cleanupCompleteFuture.getNoThrow(opCtx).reason()));
                return;
            }

            // Wait for any other, overlapping queued deletions to drain
            auto status = CollectionShardingRuntime::waitForClean(opCtx, _nss, _epoch, range);
            if (!status.isOK()) {
                _setStateFail(redact(status.reason()));
                return;
            }
        }

        timing.done(2);
        migrateThreadHangAtStep2.pauseWhileSet();
    }

    repl::OpTime lastOpApplied;
    {
        // 3. Initial bulk clone
        setState(CLONE);

        _sessionMigration->start(opCtx->getServiceContext());

        const BSONObj migrateCloneRequest = createMigrateCloneRequest(_nss, *_sessionId);

        _chunkMarkedPending = true;  // no lock needed, only the migrate thread looks.

        auto assertNotAborted = [&](OperationContext* opCtx) {
            opCtx->checkForInterrupt();
            uassert(50748, "Migration aborted while copying documents", getState() != ABORT);
        };

        auto insertBatchFn = [&](OperationContext* opCtx, BSONObj arr) {
            auto it = arr.begin();
            while (it != arr.end()) {
                int batchNumCloned = 0;
                int batchClonedBytes = 0;
                const int batchMaxCloned = migrateCloneInsertionBatchSize.load();

                assertNotAborted(opCtx);

                write_ops::Insert insertOp(_nss);
                insertOp.getWriteCommandBase().setOrdered(true);
                insertOp.setDocuments([&] {
                    std::vector<BSONObj> toInsert;
                    while (it != arr.end() &&
                           (batchMaxCloned <= 0 || batchNumCloned < batchMaxCloned)) {
                        const auto& doc = *it;
                        BSONObj docToClone = doc.Obj();
                        toInsert.push_back(docToClone);
                        batchNumCloned++;
                        batchClonedBytes += docToClone.objsize();
                        ++it;
                    }
                    return toInsert;
                }());

                const WriteResult reply = performInserts(opCtx, insertOp, true);

                for (unsigned long i = 0; i < reply.results.size(); ++i) {
                    uassertStatusOKWithContext(
                        reply.results[i],
                        str::stream() << "Insert of " << insertOp.getDocuments()[i] << " failed.");
                }

                {
                    stdx::lock_guard<Latch> statsLock(_mutex);
                    _numCloned += batchNumCloned;
                    ShardingStatistics::get(opCtx).countDocsClonedOnRecipient.addAndFetch(
                        batchNumCloned);
                    _clonedBytes += batchClonedBytes;
                }
                if (_writeConcern.needToWaitForOtherNodes()) {
                    repl::ReplicationCoordinator::StatusAndDuration replStatus =
                        repl::ReplicationCoordinator::get(opCtx)->awaitReplication(
                            opCtx,
                            repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp(),
                            _writeConcern);
                    if (replStatus.status.code() == ErrorCodes::WriteConcernFailed) {
                        warning() << "secondaryThrottle on, but doc insert timed out; "
                                     "continuing";
                    } else {
                        uassertStatusOK(replStatus.status);
                    }
                }

                sleepmillis(migrateCloneInsertionBatchDelayMS.load());
            }
        };

        auto fetchBatchFn = [&](OperationContext* opCtx) {
            auto res = uassertStatusOKWithContext(
                fromShard->runCommand(opCtx,
                                      ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                      "admin",
                                      migrateCloneRequest,
                                      Shard::RetryPolicy::kNoRetry),
                "_migrateClone failed: ");

            uassertStatusOKWithContext(Shard::CommandResponse::getEffectiveStatus(res),
                                       "_migrateClone failed: ");

            return res.response;
        };

        // If running on a replicated system, we'll need to flush the docs we cloned to the
        // secondaries
        lastOpApplied = cloneDocumentsFromDonor(opCtx, insertBatchFn, fetchBatchFn);

        timing.done(3);
        migrateThreadHangAtStep3.pauseWhileSet();

        if (MONGO_unlikely(failMigrationOnRecipient.shouldFail())) {
            _setStateFail(str::stream() << "failing migration after cloning " << _numCloned
                                        << " docs due to failMigrationOnRecipient failpoint");
            return;
        }
    }

    const BSONObj xferModsRequest = createTransferModsRequest(_nss, *_sessionId);

    {
        // 4. Do bulk of mods
        setState(CATCHUP);

        while (true) {
            auto res = uassertStatusOKWithContext(
                fromShard->runCommand(opCtx,
                                      ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                      "admin",
                                      xferModsRequest,
                                      Shard::RetryPolicy::kNoRetry),
                "_transferMods failed: ");

            uassertStatusOKWithContext(Shard::CommandResponse::getEffectiveStatus(res),
                                       "_transferMods failed: ");

            const auto& mods = res.response;

            if (mods["size"].number() == 0) {
                break;
            }

            if (!_applyMigrateOp(opCtx, mods, &lastOpApplied)) {
                continue;
            }

            const int maxIterations = 3600 * 50;

            int i;
            for (i = 0; i < maxIterations; i++) {
                opCtx->checkForInterrupt();

                if (getState() == ABORT) {
                    log() << "Migration aborted while waiting for replication at catch up stage";
                    return;
                }

                if (opReplicatedEnough(opCtx, lastOpApplied, _writeConcern))
                    break;

                if (i > 100) {
                    log() << "secondaries having hard time keeping up with migrate";
                }

                sleepmillis(20);
            }

            if (i == maxIterations) {
                _setStateFail("secondary can't keep up with migrate");
                return;
            }
        }

        timing.done(4);
        migrateThreadHangAtStep4.pauseWhileSet();
    }

    {
        // Pause to wait for replication. This will prevent us from going into critical section
        // until we're ready.

        log() << "Waiting for replication to catch up before entering critical section";

        auto awaitReplicationResult = repl::ReplicationCoordinator::get(opCtx)->awaitReplication(
            opCtx, lastOpApplied, _writeConcern);
        uassertStatusOKWithContext(awaitReplicationResult.status,
                                   awaitReplicationResult.status.codeString());

        log() << "Chunk data replicated successfully.";
    }

    {
        // 5. Wait for commit
        setState(STEADY);

        bool transferAfterCommit = false;
        while (getState() == STEADY || getState() == COMMIT_START) {
            opCtx->checkForInterrupt();

            // Make sure we do at least one transfer after recv'ing the commit message. If we
            // aren't sure that at least one transfer happens *after* our state changes to
            // COMMIT_START, there could be mods still on the FROM shard that got logged
            // *after* our _transferMods but *before* the critical section.
            if (getState() == COMMIT_START) {
                transferAfterCommit = true;
            }

            auto res = uassertStatusOKWithContext(
                fromShard->runCommand(opCtx,
                                      ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                      "admin",
                                      xferModsRequest,
                                      Shard::RetryPolicy::kNoRetry),
                "_transferMods failed in STEADY STATE: ");

            uassertStatusOKWithContext(Shard::CommandResponse::getEffectiveStatus(res),
                                       "_transferMods failed in STEADY STATE: ");

            auto mods = res.response;

            if (mods["size"].number() > 0 && _applyMigrateOp(opCtx, mods, &lastOpApplied)) {
                continue;
            }

            if (getState() == ABORT) {
                log() << "Migration aborted while transferring mods";
                return;
            }

            // We know we're finished when:
            // 1) The from side has told us that it has locked writes (COMMIT_START)
            // 2) We've checked at least one more time for un-transmitted mods
            if (getState() == COMMIT_START && transferAfterCommit == true) {
                if (_flushPendingWrites(opCtx, lastOpApplied)) {
                    break;
                }
            }

            // Only sleep if we aren't committing
            if (getState() == STEADY)
                sleepmillis(10);
        }

        if (getState() == FAIL) {
            _setStateFail("timed out waiting for commit");
            return;
        }

        timing.done(5);
        migrateThreadHangAtStep5.pauseWhileSet();
    }

    _sessionMigration->join();
    if (_sessionMigration->getState() == SessionCatalogMigrationDestination::State::ErrorOccurred) {
        _setStateFail(redact(_sessionMigration->getErrMsg()));
        return;
    }

    setState(DONE);

    timing.done(6);
    migrateThreadHangAtStep6.pauseWhileSet();
}

bool MigrationDestinationManager::_applyMigrateOp(OperationContext* opCtx,
                                                  const BSONObj& xfer,
                                                  repl::OpTime* lastOpApplied) {
    invariant(lastOpApplied);

    bool didAnything = false;

    // Deleted documents
    if (xfer["deleted"].isABSONObj()) {
        boost::optional<RemoveSaver> rs;
        if (serverGlobalParams.moveParanoia) {
            rs.emplace("moveChunk", _nss.ns(), "removedDuring");
        }

        BSONObjIterator i(xfer["deleted"].Obj());
        while (i.more()) {
            AutoGetCollection autoColl(opCtx, _nss, MODE_IX);
            uassert(ErrorCodes::ConflictingOperationInProgress,
                    str::stream() << "Collection " << _nss.ns()
                                  << " was dropped in the middle of the migration",
                    autoColl.getCollection());

            BSONObj id = i.next().Obj();

            // Do not apply delete if doc does not belong to the chunk being migrated
            BSONObj fullObj;
            if (Helpers::findById(opCtx, autoColl.getDb(), _nss.ns(), id, fullObj)) {
                if (!isInRange(fullObj, _min, _max, _shardKeyPattern)) {
                    if (MONGO_unlikely(failMigrationReceivedOutOfRangeOperation.shouldFail())) {
                        MONGO_UNREACHABLE;
                    }
                    continue;
                }
            }

            if (rs) {
                uassertStatusOK(rs->goingToDelete(fullObj));
            }

            deleteObjects(opCtx,
                          autoColl.getCollection(),
                          _nss,
                          id,
                          true /* justOne */,
                          false /* god */,
                          true /* fromMigrate */);

            *lastOpApplied = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
            didAnything = true;
        }
    }

    // Inserted or updated documents
    if (xfer["reload"].isABSONObj()) {
        BSONObjIterator i(xfer["reload"].Obj());
        while (i.more()) {
            AutoGetCollection autoColl(opCtx, _nss, MODE_IX);
            uassert(ErrorCodes::ConflictingOperationInProgress,
                    str::stream() << "Collection " << _nss.ns()
                                  << " was dropped in the middle of the migration",
                    autoColl.getCollection());

            BSONObj updatedDoc = i.next().Obj();

            // do not apply insert/update if doc does not belong to the chunk being migrated
            if (!isInRange(updatedDoc, _min, _max, _shardKeyPattern)) {
                if (MONGO_unlikely(failMigrationReceivedOutOfRangeOperation.shouldFail())) {
                    MONGO_UNREACHABLE;
                }
                continue;
            }

            BSONObj localDoc;
            if (willOverrideLocalId(opCtx,
                                    _nss,
                                    _min,
                                    _max,
                                    _shardKeyPattern,
                                    autoColl.getDb(),
                                    updatedDoc,
                                    &localDoc)) {
                const std::string errMsg = str::stream()
                    << "cannot migrate chunk, local document " << redact(localDoc)
                    << " has same _id as reloaded remote document " << redact(updatedDoc);
                warning() << errMsg;

                // Exception will abort migration cleanly
                uasserted(16977, errMsg);
            }

            // We are in write lock here, so sure we aren't killing
            Helpers::upsert(opCtx, _nss.ns(), updatedDoc, true);

            *lastOpApplied = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
            didAnything = true;
        }
    }

    return didAnything;
}

bool MigrationDestinationManager::_flushPendingWrites(OperationContext* opCtx,
                                                      const repl::OpTime& lastOpApplied) {
    if (!opReplicatedEnough(opCtx, lastOpApplied, _writeConcern)) {
        repl::OpTime op(lastOpApplied);
        static Occasionally sampler;
        if (sampler.tick()) {
            log() << "migrate commit waiting for a majority of slaves for '" << _nss.ns() << "' "
                  << redact(_min) << " -> " << redact(_max) << " waiting for: " << op;
        }
        return false;
    }

    log() << "migrate commit succeeded flushing to secondaries for '" << _nss.ns() << "' "
          << redact(_min) << " -> " << redact(_max);

    return true;
}

SharedSemiFuture<void> MigrationDestinationManager::_notePending(OperationContext* opCtx,
                                                                 ChunkRange const& range) {

    AutoGetCollection autoColl(opCtx, _nss, MODE_X);
    auto* const css = CollectionShardingRuntime::get(opCtx, _nss);
    const auto optMetadata = css->getCurrentMetadataIfKnown();

    // This can currently happen because drops aren't synchronized with in-migrations. The idea for
    // checking this here is that in the future we shouldn't have this problem.
    if (!optMetadata || !(*optMetadata)->isSharded() ||
        (*optMetadata)->getCollVersion().epoch() != _epoch) {
        return Status{ErrorCodes::StaleShardVersion,
                      str::stream()
                          << "Not marking chunk " << redact(range.toString())
                          << " as pending because the epoch of " << _nss.ns() << " changed"};
    }

    // Start clearing any leftovers that would be in the new chunk
    auto cleanupCompleteFuture = css->beginReceive(range);
    if (cleanupCompleteFuture.isReady() && !cleanupCompleteFuture.getNoThrow(opCtx).isOK()) {
        return cleanupCompleteFuture.getNoThrow(opCtx).withContext(
            str::stream() << "Collection " << _nss.ns() << " range " << redact(range.toString())
                          << " migration aborted");
    }
    return cleanupCompleteFuture;
}

void MigrationDestinationManager::_forgetPending(OperationContext* opCtx, ChunkRange const& range) {
    if (!_chunkMarkedPending) {  // (no lock needed, only the migrate thread looks at this.)
        return;  // no documents can have been moved in, so there is nothing to clean up.
    }

    UninterruptibleLockGuard noInterrupt(opCtx->lockState());
    AutoGetCollection autoColl(opCtx, _nss, MODE_IX);
    auto* const css = CollectionShardingRuntime::get(opCtx, _nss);
    const auto optMetadata = css->getCurrentMetadataIfKnown();

    // This can currently happen because drops aren't synchronized with in-migrations. The idea for
    // checking this here is that in the future we shouldn't have this problem.
    if (!optMetadata || !(*optMetadata)->isSharded() ||
        (*optMetadata)->getCollVersion().epoch() != _epoch) {
        LOG(0) << "No need to forget pending chunk " << redact(range.toString())
               << " because the epoch for " << _nss.ns() << " changed";
        return;
    }

    css->forgetReceive(range);
}

}  // namespace mongo
