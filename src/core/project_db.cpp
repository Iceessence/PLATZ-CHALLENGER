#include "core/project_db.h"
#include "platform/platform_paths.h"

#include <sqlite3.h>
#include <sstream>

namespace platz::core {

ProjectDatabase::~ProjectDatabase() {
    Close();
}

void ProjectDatabase::Close() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool ProjectDatabase::ExecSql(const std::string& sql, std::string* outError) const {
    if (!db_) {
        if (outError) *outError = "Database is not open";
        return false;
    }
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        if (outError) {
            *outError = errMsg ? std::string(errMsg) : ("SQLite error code " + std::to_string(rc));
        }
        if (errMsg) sqlite3_free(errMsg);
        return false;
    }
    return true;
}

bool ProjectDatabase::OpenOrCreate(
    const std::filesystem::path& dbFilePath,
    const std::string& defaultProjectName,
    std::string* outError) {
    Close();
    std::error_code ec;
    if (!dbFilePath.parent_path().empty()) {
        std::filesystem::create_directories(dbFilePath.parent_path(), ec);
    }

    int rc = sqlite3_open_v2(
        dbFilePath.string().c_str(),
        &db_,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr);
    if (rc != SQLITE_OK || !db_) {
        if (outError) *outError = "Failed to open SQLite database at " + dbFilePath.string();
        Close();
        return false;
    }
    dbPath_ = dbFilePath;

    if (!ExecSql("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON;", outError)) {
        Close();
        return false;
    }

    // Check if project_meta already exists and verify schema version BEFORE making any schema alterations (N-26)
    {
        sqlite3_stmt* stmt = nullptr;
        const char* checkSql = "SELECT schema_version FROM project_meta LIMIT 1;";
        if (sqlite3_prepare_v2(db_, checkSql, -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                int32_t existingVer = sqlite3_column_int(stmt, 0);
                sqlite3_finalize(stmt);
                if (existingVer > kCurrentProjectSchemaVersion || existingVer <= 0) {
                    if (outError) {
                        *outError = "Incompatible project schema_version=" + std::to_string(existingVer) +
                                    " (supported schema_version=" + std::to_string(kCurrentProjectSchemaVersion) +
                                    "). Refusing destructive open (N-26).";
                    }
                    Close();
                    return false;
                }
            } else {
                sqlite3_finalize(stmt);
            }
        }
    }

    const char* schemaSql = R"SQL(
        CREATE TABLE IF NOT EXISTS project_meta (
            id INTEGER PRIMARY KEY CHECK (id = 1),
            project_uuid TEXT NOT NULL,
            name TEXT NOT NULL,
            schema_version INTEGER NOT NULL,
            head_revision_id INTEGER NOT NULL,
            created_utc TEXT NOT NULL,
            updated_utc TEXT NOT NULL
        );

        CREATE TABLE IF NOT EXISTS revisions (
            revision_id INTEGER PRIMARY KEY AUTOINCREMENT,
            parent_revision_id INTEGER NOT NULL,
            branch_name TEXT NOT NULL,
            revision_hash TEXT NOT NULL,
            author TEXT NOT NULL,
            summary TEXT NOT NULL,
            timestamp_utc TEXT NOT NULL,
            state_snapshot_json TEXT NOT NULL
        );

        CREATE TABLE IF NOT EXISTS objects (
            object_id INTEGER PRIMARY KEY,
            name TEXT NOT NULL,
            parent_id INTEGER NOT NULL,
            layer TEXT NOT NULL,
            visible INTEGER NOT NULL,
            locked INTEGER NOT NULL,
            components_json TEXT NOT NULL
        );

        CREATE TABLE IF NOT EXISTS assets (
            asset_id INTEGER PRIMARY KEY,
            logical_name TEXT NOT NULL,
            asset_type TEXT NOT NULL,
            content_sha256 TEXT NOT NULL,
            byte_size INTEGER NOT NULL,
            source_rel_path TEXT NOT NULL,
            lineage_json TEXT NOT NULL,
            license_info TEXT NOT NULL,
            generation_version INTEGER NOT NULL
        );

        CREATE TABLE IF NOT EXISTS runtime_saves (
            slot_name TEXT PRIMARY KEY,
            source_authoring_revision INTEGER NOT NULL,
            elapsed_play_time_sec REAL NOT NULL,
            timestamp_utc TEXT NOT NULL,
            runtime_state_json TEXT NOT NULL
        );
    )SQL";

    if (!ExecSql(schemaSql, outError)) {
        Close();
        return false;
    }

    // Ensure row 1 exists in project_meta
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM project_meta;", -1, &stmt, nullptr) == SQLITE_OK) {
        int count = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            count = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);

        if (count == 0) {
            std::string now = platform::UtcIso8601Now();
            std::string uuid = "PLZ-" + platform::ComputeSha256Hex(defaultProjectName + now).substr(0, 16);
            sqlite3_stmt* ins = nullptr;
            const char* insSql =
                "INSERT INTO project_meta (id, project_uuid, name, schema_version, head_revision_id, created_utc, updated_utc) "
                "VALUES (1, ?1, ?2, ?3, 0, ?4, ?4);";
            if (sqlite3_prepare_v2(db_, insSql, -1, &ins, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(ins, 1, uuid.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(ins, 2, defaultProjectName.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(ins, 3, kCurrentProjectSchemaVersion);
                sqlite3_bind_text(ins, 4, now.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(ins);
                sqlite3_finalize(ins);
            }
        }
    }

    return true;
}

bool ProjectDatabase::GetProjectMetadata(
    std::string* outUuid,
    std::string* outName,
    int32_t* outSchemaVersion,
    RevisionId* outHeadRevision,
    std::string* outError) const {
    if (!db_) {
        if (outError) *outError = "Database is not open";
        return false;
    }
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT project_uuid, name, schema_version, head_revision_id FROM project_meta WHERE id = 1;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        if (outError) *outError = sqlite3_errmsg(db_);
        return false;
    }
    bool ok = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        if (outUuid) *outUuid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (outName) *outName = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (outSchemaVersion) *outSchemaVersion = sqlite3_column_int(stmt, 2);
        if (outHeadRevision) *outHeadRevision = static_cast<RevisionId>(sqlite3_column_int64(stmt, 3));
        ok = true;
    } else if (outError) {
        *outError = "project_meta row missing";
    }
    sqlite3_finalize(stmt);
    return ok;
}

bool ProjectDatabase::CommitRevisionTransaction(
    const std::string& projectName,
    RevisionId expectedParentRevision,
    const std::string& branchName,
    const std::string& author,
    const std::string& summary,
    const std::vector<SceneObject>& objects,
    const std::vector<AssetMetadata>& assets,
    RevisionId* outNewRevisionId,
    std::string* outError,
    bool injectFaultBeforeCommit) {
    if (!db_) {
        if (outError) *outError = "Database is not open";
        return false;
    }

    if (!ExecSql("BEGIN IMMEDIATE TRANSACTION;", outError)) {
        return false;
    }

    auto rollbackAndFail = [&](const std::string& reason) -> bool {
        ExecSql("ROLLBACK;");
        if (outError) *outError = reason;
        return false;
    };

    RevisionId currentHead = 0;
    if (!GetProjectMetadata(nullptr, nullptr, nullptr, &currentHead, outError)) {
        return rollbackAndFail("Failed reading current head revision");
    }

    if (currentHead != 0 && expectedParentRevision != currentHead) {
        return rollbackAndFail(
            "Stale revision conflict (N-02 / PLZ-ARCH-002): expected parent revision " +
            std::to_string(expectedParentRevision) + ", but authoritative head is " +
            std::to_string(currentHead));
    }

    nlohmann::json stateSnapshot;
    stateSnapshot["projectName"] = projectName;
    stateSnapshot["branchName"] = branchName;
    nlohmann::json objsArr = nlohmann::json::array();
    for (const auto& obj : objects) {
        objsArr.push_back(obj.ToJson());
    }
    nlohmann::json assetsArr = nlohmann::json::array();
    for (const auto& ast : assets) {
        assetsArr.push_back(ast.ToJson());
    }
    stateSnapshot["objects"] = std::move(objsArr);
    stateSnapshot["assets"] = std::move(assetsArr);

    std::string snapshotStr = stateSnapshot.dump();
    std::string revHash = platform::ComputeSha256Hex(snapshotStr);
    std::string now = platform::UtcIso8601Now();

    RevisionId newRevisionId = 0;
    {
        sqlite3_stmt* stmt = nullptr;
        const char* sql =
            "INSERT INTO revisions (parent_revision_id, branch_name, revision_hash, author, summary, timestamp_utc, state_snapshot_json) "
            "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7);";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return rollbackAndFail(sqlite3_errmsg(db_));
        }
        sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(currentHead));
        sqlite3_bind_text(stmt, 2, branchName.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, revHash.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, author.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, summary.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, now.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 7, snapshotStr.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            std::string err = sqlite3_errmsg(db_);
            sqlite3_finalize(stmt);
            return rollbackAndFail("Failed inserting revision: " + err);
        }
        sqlite3_finalize(stmt);
        newRevisionId = static_cast<RevisionId>(sqlite3_last_insert_rowid(db_));
    }

    if (!ExecSql("DELETE FROM objects; DELETE FROM assets;", outError)) {
        return rollbackAndFail("Failed clearing live object/asset projection tables");
    }

    for (const auto& obj : objects) {
        sqlite3_stmt* stmt = nullptr;
        const char* sql =
            "INSERT INTO objects (object_id, name, parent_id, layer, visible, locked, components_json) "
            "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7);";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return rollbackAndFail(sqlite3_errmsg(db_));
        }
        std::string compStr = obj.ToJson().dump();
        sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(obj.id));
        sqlite3_bind_text(stmt, 2, obj.name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(obj.parentId));
        sqlite3_bind_text(stmt, 4, obj.layer.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 5, obj.visible ? 1 : 0);
        sqlite3_bind_int(stmt, 6, obj.locked ? 1 : 0);
        sqlite3_bind_text(stmt, 7, compStr.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            std::string err = sqlite3_errmsg(db_);
            sqlite3_finalize(stmt);
            return rollbackAndFail("Failed inserting object: " + err);
        }
        sqlite3_finalize(stmt);
    }

    for (const auto& ast : assets) {
        sqlite3_stmt* stmt = nullptr;
        const char* sql =
            "INSERT INTO assets (asset_id, logical_name, asset_type, content_sha256, byte_size, source_rel_path, lineage_json, license_info, generation_version) "
            "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9);";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return rollbackAndFail(sqlite3_errmsg(db_));
        }
        sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(ast.assetId));
        sqlite3_bind_text(stmt, 2, ast.logicalName.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, ast.assetType.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, ast.contentSha256.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 5, static_cast<sqlite3_int64>(ast.byteSize));
        sqlite3_bind_text(stmt, 6, ast.sourceRelPath.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 7, ast.lineageJson.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 8, ast.licenseInfo.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 9, static_cast<sqlite3_int64>(ast.generationVersion));
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            std::string err = sqlite3_errmsg(db_);
            sqlite3_finalize(stmt);
            return rollbackAndFail("Failed inserting asset: " + err);
        }
        sqlite3_finalize(stmt);
    }

    if (injectFaultBeforeCommit) {
        return rollbackAndFail("Simulated crash/interruption before revision head advancement (N-03)");
    }

    {
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "UPDATE project_meta SET name = ?1, head_revision_id = ?2, updated_utc = ?3 WHERE id = 1;";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return rollbackAndFail(sqlite3_errmsg(db_));
        }
        sqlite3_bind_text(stmt, 1, projectName.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(newRevisionId));
        sqlite3_bind_text(stmt, 3, now.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            std::string err = sqlite3_errmsg(db_);
            sqlite3_finalize(stmt);
            return rollbackAndFail("Failed advancing head_revision_id: " + err);
        }
        sqlite3_finalize(stmt);
    }

    if (!ExecSql("COMMIT;", outError)) {
        ExecSql("ROLLBACK;");
        return false;
    }

    if (outNewRevisionId) *outNewRevisionId = newRevisionId;
    return true;
}

bool ProjectDatabase::LoadRevisionState(
    std::optional<RevisionId> specificRevision,
    RevisionId* outLoadedRevision,
    std::string* outProjectName,
    std::vector<SceneObject>* outObjects,
    std::vector<AssetMetadata>* outAssets,
    std::string* outError) const {
    if (!db_) {
        if (outError) *outError = "Database is not open";
        return false;
    }

    std::string metaName;
    RevisionId headRev = 0;
    if (!GetProjectMetadata(nullptr, &metaName, nullptr, &headRev, outError)) {
        return false;
    }

    RevisionId targetRev = specificRevision.value_or(headRev);
    if (targetRev == 0) {
        if (outLoadedRevision) *outLoadedRevision = 0;
        if (outProjectName) *outProjectName = metaName;
        if (outObjects) outObjects->clear();
        if (outAssets) outAssets->clear();
        return true;
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT state_snapshot_json, revision_hash FROM revisions WHERE revision_id = ?1;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        if (outError) *outError = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(targetRev));

    std::string snapshotStr;
    std::string storedHash;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        snapshotStr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        storedHash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    } else {
        sqlite3_finalize(stmt);
        if (outError) *outError = "Revision " + std::to_string(targetRev) + " not found in database";
        return false;
    }
    sqlite3_finalize(stmt);

    std::string computedHash = platform::ComputeSha256Hex(snapshotStr);
    if (computedHash != storedHash) {
        if (outError) *outError = "Revision snapshot hash mismatch for revision " + std::to_string(targetRev);
        return false;
    }

    auto j = nlohmann::json::parse(snapshotStr, nullptr, false);
    if (j.is_discarded()) {
        if (outError) *outError = "Corrupt JSON in revision snapshot";
        return false;
    }

    if (outLoadedRevision) *outLoadedRevision = targetRev;
    if (outProjectName) *outProjectName = j.value("projectName", metaName);
    if (outObjects) {
        outObjects->clear();
        if (j.contains("objects") && j["objects"].is_array()) {
            for (const auto& oj : j["objects"]) {
                outObjects->push_back(SceneObject::FromJson(oj));
            }
        }
    }
    if (outAssets) {
        outAssets->clear();
        if (j.contains("assets") && j["assets"].is_array()) {
            for (const auto& aj : j["assets"]) {
                outAssets->push_back(AssetMetadata::FromJson(aj));
            }
        }
    }
    return true;
}

std::vector<RevisionRecord> ProjectDatabase::ListRevisions() const {
    std::vector<RevisionRecord> list;
    if (!db_) return list;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT revision_id, parent_revision_id, branch_name, revision_hash, author, summary, timestamp_utc "
        "FROM revisions ORDER BY revision_id DESC;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            RevisionRecord r;
            r.revisionId = static_cast<RevisionId>(sqlite3_column_int64(stmt, 0));
            r.parentRevisionId = static_cast<RevisionId>(sqlite3_column_int64(stmt, 1));
            r.branchName = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            r.revisionHash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            r.author = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
            r.summary = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
            r.timestampUtc = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
            list.push_back(std::move(r));
        }
        sqlite3_finalize(stmt);
    }
    return list;
}

bool ProjectDatabase::SaveRuntimeCheckpoint(const RuntimeCheckpointRecord& checkpoint, std::string* outError) {
    if (!db_) {
        if (outError) *outError = "Database is not open";
        return false;
    }
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO runtime_saves (slot_name, source_authoring_revision, elapsed_play_time_sec, timestamp_utc, runtime_state_json) "
        "VALUES (?1, ?2, ?3, ?4, ?5) "
        "ON CONFLICT(slot_name) DO UPDATE SET "
        "source_authoring_revision=excluded.source_authoring_revision, "
        "elapsed_play_time_sec=excluded.elapsed_play_time_sec, "
        "timestamp_utc=excluded.timestamp_utc, "
        "runtime_state_json=excluded.runtime_state_json;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        if (outError) *outError = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_text(stmt, 1, checkpoint.slotName.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(checkpoint.sourceAuthoringRevision));
    sqlite3_bind_double(stmt, 3, checkpoint.elapsedPlayTimeSec);
    sqlite3_bind_text(stmt, 4, checkpoint.timestampUtc.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, checkpoint.runtimeStateJson.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    if (!ok && outError) *outError = sqlite3_errmsg(db_);
    sqlite3_finalize(stmt);
    return ok;
}

std::optional<RuntimeCheckpointRecord> ProjectDatabase::LoadRuntimeCheckpoint(
    const std::string& slotName,
    std::string* outError) const {
    if (!db_) {
        if (outError) *outError = "Database is not open";
        return std::nullopt;
    }
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT slot_name, source_authoring_revision, elapsed_play_time_sec, timestamp_utc, runtime_state_json "
        "FROM runtime_saves WHERE slot_name = ?1;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        if (outError) *outError = sqlite3_errmsg(db_);
        return std::nullopt;
    }
    sqlite3_bind_text(stmt, 1, slotName.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<RuntimeCheckpointRecord> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        RuntimeCheckpointRecord rec;
        rec.slotName = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        rec.sourceAuthoringRevision = static_cast<RevisionId>(sqlite3_column_int64(stmt, 1));
        rec.elapsedPlayTimeSec = sqlite3_column_double(stmt, 2);
        rec.timestampUtc = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        rec.runtimeStateJson = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        result = std::move(rec);
    } else if (outError) {
        *outError = "Runtime save slot not found: " + slotName;
    }
    sqlite3_finalize(stmt);
    return result;
}

bool ProjectDatabase::WriteSchemaVersionForTesting(
    const std::filesystem::path& dbFilePath,
    int32_t forcedSchemaVersion,
    std::string* outError) {
    sqlite3* raw = nullptr;
    if (sqlite3_open(dbFilePath.string().c_str(), &raw) != SQLITE_OK) {
        if (outError) *outError = "Failed opening SQLite db for test";
        if (raw) sqlite3_close(raw);
        return false;
    }
    std::string sql = "UPDATE project_meta SET schema_version = " + std::to_string(forcedSchemaVersion) + " WHERE id = 1;";
    char* err = nullptr;
    int rc = sqlite3_exec(raw, sql.c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        if (outError && err) *outError = err;
        if (err) sqlite3_free(err);
        sqlite3_close(raw);
        return false;
    }
    sqlite3_close(raw);
    return true;
}

} // namespace platz::core
