/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/ops/write_ops_parsers.h"

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using write_ops::UpdateOpEntry;
using write_ops::DeleteOpEntry;

namespace {

// The specified limit to the number of operations that can be included in a single write command.
// This is an attempt to avoid a large number of errors resulting in a reply that exceeds 16MB. It
// doesn't fully ensure that goal, but it reduces the probability of it happening. This limit should
// not be used if the protocol changes to avoid the 16MB limit on reply size.
const size_t kMaxWriteBatchSize = 1000;

template <class T>
void checkOpCountForCommand(const T& op, size_t numOps) {
    uassert(ErrorCodes::InvalidLength,
            str::stream() << "Write batch sizes must be between 1 and " << kMaxWriteBatchSize
                          << ". Got "
                          << numOps
                          << " operations.",
            numOps != 0 && numOps <= kMaxWriteBatchSize);

    const auto& stmtIds = op.getWriteCommandBase().getStmtIds();
    uassert(ErrorCodes::InvalidLength,
            "Number of statement ids must match the number of batch entries",
            !stmtIds || stmtIds->size() == numOps);
}

}  // namespace

namespace write_ops {

bool readMultiDeleteProperty(const BSONElement& limitElement) {
    // Using a double to avoid throwing away illegal fractional portion. Don't want to accept 0.5
    // here
    const double limit = limitElement.numberDouble();
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "The limit field in delete objects must be 0 or 1. Got " << limit,
            limit == 0 || limit == 1);

    return limit == 0;
}

void writeMultiDeleteProperty(bool isMulti, StringData fieldName, BSONObjBuilder* builder) {
    builder->append(fieldName, isMulti ? 0 : 1);
}

int32_t getStmtIdForWriteAt(const WriteCommandBase& writeCommandBase, size_t writePos) {
    const auto& stmtIds = writeCommandBase.getStmtIds();

    if (stmtIds) {
        return stmtIds->at(writePos);
    }

    const int32_t kFirstStmtId = 0;
    return kFirstStmtId + writePos;
}

}  // namespace write_ops

InsertOp InsertOp::parse(const OpMsgRequest& request) {
    auto insertOp = Insert::parse(IDLParserErrorContext("insert"), request);

    if (insertOp.getNamespace().isSystemDotIndexes()) {
        // This is only for consistency with sharding.
        uassert(ErrorCodes::InvalidLength,
                "Insert commands to system.indexes are limited to a single insert",
                insertOp.getDocuments().size() == 1);
    }

    checkOpCountForCommand(insertOp, insertOp.getDocuments().size());
    return {std::move(insertOp)};
}

InsertOp InsertOp::parseLegacy(const Message& msgRaw) {
    DbMessage msg(msgRaw);

    InsertOp op(NamespaceString(msg.getns()));

    {
        write_ops::WriteCommandBase writeCommandBase;
        writeCommandBase.setBypassDocumentValidation(false);
        writeCommandBase.setOrdered(!(msg.reservedField() & InsertOption_ContinueOnError));
        op.setWriteCommandBase(std::move(writeCommandBase));
    }

    uassert(ErrorCodes::InvalidLength, "Need at least one object to insert", msg.moreJSObjs());

    op.setDocuments([&] {
        std::vector<BSONObj> documents;
        while (msg.moreJSObjs()) {
            documents.push_back(msg.nextJsObj());
        }

        return documents;
    }());

    return op;
}

UpdateOp UpdateOp::parse(const OpMsgRequest& request) {
    auto updateOp = Update::parse(IDLParserErrorContext("update"), request);

    checkOpCountForCommand(updateOp, updateOp.getUpdates().size());
    return {std::move(updateOp)};
}

UpdateOp UpdateOp::parseLegacy(const Message& msgRaw) {
    DbMessage msg(msgRaw);

    UpdateOp op(NamespaceString(msg.getns()));

    {
        write_ops::WriteCommandBase writeCommandBase;
        writeCommandBase.setBypassDocumentValidation(false);
        writeCommandBase.setOrdered(true);
        op.setWriteCommandBase(std::move(writeCommandBase));
    }

    op.setUpdates([&] {
        std::vector<write_ops::UpdateOpEntry> updates;
        updates.emplace_back();

        // Legacy updates only allowed one update per operation. Layout is flags, query, update.
        auto& singleUpdate = updates.back();
        const int flags = msg.pullInt();
        singleUpdate.setUpsert(flags & UpdateOption_Upsert);
        singleUpdate.setMulti(flags & UpdateOption_Multi);
        singleUpdate.setQ(msg.nextJsObj());
        singleUpdate.setU(msg.nextJsObj());

        return updates;
    }());

    return op;
}

DeleteOp DeleteOp::parse(const OpMsgRequest& request) {
    auto deleteOp = Delete::parse(IDLParserErrorContext("delete"), request);

    checkOpCountForCommand(deleteOp, deleteOp.getDeletes().size());
    return {std::move(deleteOp)};
}

DeleteOp DeleteOp::parseLegacy(const Message& msgRaw) {
    DbMessage msg(msgRaw);

    DeleteOp op(NamespaceString(msg.getns()));

    {
        write_ops::WriteCommandBase writeCommandBase;
        writeCommandBase.setBypassDocumentValidation(false);
        writeCommandBase.setOrdered(true);
        op.setWriteCommandBase(std::move(writeCommandBase));
    }

    op.setDeletes([&]() {
        std::vector<write_ops::DeleteOpEntry> deletes;
        deletes.emplace_back();

        // Legacy deletes only allowed one delete per operation. Layout is flags, query.
        auto& singleDelete = deletes.back();
        const int flags = msg.pullInt();
        singleDelete.setMulti(!(flags & RemoveOption_JustOne));
        singleDelete.setQ(msg.nextJsObj());

        return deletes;
    }());

    return op;
}

}  // namespace mongo
