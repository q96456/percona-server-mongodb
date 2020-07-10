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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault
#include "mongo/platform/basic.h"

#include <string>
#include <vector>

#include "mongo/base/status_with.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/find_and_modify.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_connection.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/commands/cluster_commands_common.h"
#include "mongo/s/commands/cluster_explain.h"
#include "mongo/s/commands/cluster_write.h"
#include "mongo/s/commands/sharded_command_processing.h"
#include "mongo/s/commands/strategy.h"
#include "mongo/s/grid.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/timer.h"
#include "mongo/util/log.h"
#include "mongo/executor/async_timer_asio.h"
namespace mongo {
namespace {

using std::shared_ptr;
using std::string;
using std::vector;

class FindAndModifyCmd : public Command {
public:
    FindAndModifyCmd() : Command("findAndModify", false, "findandmodify") {}

    bool slaveOk() const override {
        return true;
    }

    bool adminOnly() const override {
        return false;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) override {
        find_and_modify::addPrivilegesRequiredForFindAndModify(this, dbname, cmdObj, out);
    }

    Status explain(OperationContext* opCtx,
                   const std::string& dbName,
                   const BSONObj& cmdObj,
                   ExplainCommon::Verbosity verbosity,
                   const rpc::ServerSelectionMetadata& serverSelectionMetadata,
                   BSONObjBuilder* out) const override {
        const NamespaceString nss(parseNsCollectionRequired(dbName, cmdObj));

        auto routingInfo =
            uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));

        shared_ptr<ChunkManager> chunkMgr;
        shared_ptr<Shard> shard;

        if (!routingInfo.cm()) {
            shard = routingInfo.primary();
        } else {
            chunkMgr = routingInfo.cm();

            const BSONObj query = cmdObj.getObjectField("query");

            BSONObj collation;
            BSONElement collationElement;
            auto collationElementStatus =
                bsonExtractTypedField(cmdObj, "collation", BSONType::Object, &collationElement);
            if (collationElementStatus.isOK()) {
                collation = collationElement.Obj();
            } else if (collationElementStatus != ErrorCodes::NoSuchKey) {
                return collationElementStatus;
            }

            StatusWith<BSONObj> status = _getShardKey(opCtx, *chunkMgr, query);
            if (!status.isOK()) {
                return status.getStatus();
            }

            BSONObj shardKey = status.getValue();
            auto chunk = chunkMgr->findIntersectingChunk(shardKey, collation);

            auto shardStatus =
                Grid::get(opCtx)->shardRegistry()->getShard(opCtx, chunk->getShardId());
            if (!shardStatus.isOK()) {
                return shardStatus.getStatus();
            }

            shard = shardStatus.getValue();
        }

        BSONObjBuilder explainCmd;
        int options = 0;
        ClusterExplain::wrapAsExplain(
            cmdObj, verbosity, serverSelectionMetadata, &explainCmd, &options);

        // Time how long it takes to run the explain command on the shard.
        Timer timer;

        BSONObjBuilder result;
        bool ok = _runCommand(opCtx, chunkMgr, shard->getId(), nss, explainCmd.obj(), result);
        long long millisElapsed = timer.millis();

        if (!ok) {
            BSONObj res = result.obj();
            return Status(ErrorCodes::OperationFailed,
                          str::stream() << "Explain for findAndModify failed: " << res);
        }

        Strategy::CommandResult cmdResult;
        cmdResult.shardTargetId = shard->getId();
        cmdResult.target = shard->getConnString();
        cmdResult.result = result.obj();

        vector<Strategy::CommandResult> shardResults;
        shardResults.push_back(cmdResult);

        return ClusterExplain::buildExplainResult(
            opCtx, shardResults, ClusterExplain::kSingleShard, millisElapsed, out);
    }

    bool run(OperationContext* opCtx,
             const std::string& dbName,
             BSONObj& cmdObj,
             int options,
             std::string& errmsg,
             BSONObjBuilder& result) override {
        const NamespaceString nss = parseNsCollectionRequired(dbName, cmdObj);

        // findAndModify should only be creating database if upsert is true, but this would require
        // that the parsing be pulled into this function.
        uassertStatusOK(createShardDatabase(opCtx, nss.db()));

        auto routingInfo =
            uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));
        if (!routingInfo.cm()) {
            return _runCommand(opCtx, nullptr, routingInfo.primaryId(), nss, cmdObj, result);
        }

        const auto chunkMgr = routingInfo.cm();

        const BSONObj query = cmdObj.getObjectField("query");

        BSONObj collation;
        BSONElement collationElement;
        auto collationElementStatus =
            bsonExtractTypedField(cmdObj, "collation", BSONType::Object, &collationElement);
        if (collationElementStatus.isOK()) {
            collation = collationElement.Obj();
        } else if (collationElementStatus != ErrorCodes::NoSuchKey) {
            return appendCommandStatus(result, collationElementStatus);
        }

        BSONObj shardKey = uassertStatusOK(_getShardKey(opCtx, *chunkMgr, query));

        auto chunk = chunkMgr->findIntersectingChunk(shardKey, collation);

        const bool ok = _runCommand(opCtx, chunkMgr, chunk->getShardId(), nss, cmdObj, result);
        if (ok) {
            updateChunkWriteStatsAndSplitIfNeeded(
                opCtx, chunkMgr.get(), chunk.get(), cmdObj.getObjectField("update").objsize());
        }

        return ok;
    }

private:
    static StatusWith<BSONObj> _getShardKey(OperationContext* opCtx,
                                            const ChunkManager& chunkMgr,
                                            const BSONObj& query) {
        // Verify that the query has an equality predicate using the shard key
        StatusWith<BSONObj> status =
            chunkMgr.getShardKeyPattern().extractShardKeyFromQuery(opCtx, query);

        if (!status.isOK()) {
            return status;
        }

        BSONObj shardKey = status.getValue();

        if (shardKey.isEmpty()) {
            return Status(ErrorCodes::ShardKeyNotFound,
                          "query for sharded findAndModify must have shardkey");
        }

        return shardKey;
    }

    static bool _runCommand(OperationContext* opCtx,
                            shared_ptr<ChunkManager> chunkManager,
                            const ShardId& shardId,
                            const NamespaceString& nss,
                            const BSONObj& cmdObj,
                            BSONObjBuilder& result) {
        BSONObj res;

        const auto shard =
            uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardId));

        ShardConnection conn(shard->getConnString(), nss.ns(), chunkManager);
        Timer time;
        bool ok = conn->runCommand(nss.db().toString(), cmdObj, res);
        conn.done();
        auto optime = time.millis();
        bool slow_log = false;
        if(optime > serverGlobalParams.slowMS){
            slow_log = true;
        }

        // ErrorCodes::RecvStaleConfig is the code for RecvStaleConfigException.
        if (!ok && res.getIntField("code") == ErrorCodes::RecvStaleConfig) {
            // Command code traps this exception and re-runs
            if(slow_log){
                log()<<"FindAndModify err. target="<<shardId.toString()<<",ips:"<<shard->getConnString() <<" ;req="<<cmdObj.toString()<<" ;resp="<<res.toString()<<";optime="<< optime<<"ms";
            }
            throw RecvStaleConfigException("FindAndModify", res);
        }

        // First append the properly constructed writeConcernError. It will then be skipped
        // in appendElementsUnique.
        if (auto wcErrorElem = res["writeConcernError"]) {
            appendWriteConcernErrorToCmdResponse(shardId, wcErrorElem, result);
        }

        if(slow_log){
            log()<<"FindAndModify ok. target="<<shardId.toString()<<",ips:" << shard->getConnString() << " ;req="<<cmdObj.toString()<<"  resp="<<res.toString()<<" optime="<< optime<<"ms";
        }
        result.appendElementsUnique(res);

        return ok;
    }

} findAndModifyCmd;

}  // namespace
}  // namespace mongo
