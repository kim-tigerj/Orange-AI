#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <stdexcept>
extern "C" {
#include "lib/sqlite/sqlite3.h"
}
#include <json/json.h>
#include "Utils.h"

namespace orange {

/**
 * OrangeDB: Orange Code 전용 고성능 데이터 엔진
 * - 외부 의존성 없이 독립적으로 동작
 * - Prepared Statement 캐싱 및 JSON 자동 바인딩 지원
 * - 인과관계 추적(seq, parent_seq) 내장
 */
class CDatabase {
public:
    static CDatabase& Instance() {
        static CDatabase instance;
        return instance;
    }

    struct TracingContext {
        uint64_t parent_seq = 0;
        std::string run_uid;
    } m_ctx;

    void Open(const std::wstring& dbPath) {
        for (auto& it : m_cache) sqlite3_finalize(it.second);
        m_cache.clear();
        if (m_db) sqlite3_close(m_db);
        
        std::string pathUtf8 = WideToUtf8(dbPath);
        if (sqlite3_open(pathUtf8.c_str(), &m_db) != SQLITE_OK) {
            std::string err = sqlite3_errmsg(m_db);
            sqlite3_close(m_db);
            m_db = nullptr;
            throw std::runtime_error("OrangeDB Open Failed: " + err);
        }

        // Orange 최적화 설정
        Execute("PRAGMA journal_mode=WAL;");
        Execute("PRAGMA synchronous=NORMAL;");
        Execute("PRAGMA foreign_keys=ON;");
        
        InitSchema();
    }

    // 결과가 필요 없는 명령 실행
    void Execute(const std::string& sql) {
        char* errMsg = nullptr;
        if (sqlite3_exec(m_db, sql.c_str(), nullptr, nullptr, &errMsg) != SQLITE_OK) {
            std::string err = errMsg ? errMsg : "Unknown";
            if (errMsg) sqlite3_free(errMsg);
            throw std::runtime_error("SQL Error: " + err + " [" + sql + "]");
        }
    }

    // JSON 파라미터를 사용하는 고속 쿼리 (Caching 지원)
    void Run(const std::string& name, const std::string& sql, const Json::Value& params = Json::Value()) {
        sqlite3_stmt* stmt = Prepare(name, sql);
        if (!stmt) return;

        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);

        // 1. JSON 데이터 바인딩
        if (params.isObject()) {
            for (auto const& key : params.getMemberNames()) {
                int idx = sqlite3_bind_parameter_index(stmt, (":" + key).c_str());
                if (idx > 0) BindValue(stmt, idx, params[key]);
            }
        }

        // 2. 전역 추적 필드 자동 바인딩
        int idx;
        if ((idx = sqlite3_bind_parameter_index(stmt, ":parent_seq")) > 0) 
            sqlite3_bind_int64(stmt, idx, m_ctx.parent_seq);
        if ((idx = sqlite3_bind_parameter_index(stmt, ":run_uid")) > 0)
            sqlite3_bind_text(stmt, idx, m_ctx.run_uid.c_str(), -1, SQLITE_TRANSIENT);

        if (sqlite3_step(stmt) == SQLITE_ERROR) {
            throw std::runtime_error("Query Execution Failed: " + std::string(sqlite3_errmsg(m_db)));
        }
    }

    // 콜백 기반 데이터 조회
    typedef std::function<bool(const Json::Value& row)> RowCallback;
    void Fetch(const std::string& name, const std::string& sql, const Json::Value& params, RowCallback cb) {
        sqlite3_stmt* stmt = Prepare(name, sql);
        if (!stmt) return;

        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        
        if (params.isObject()) {
            for (auto const& key : params.getMemberNames()) {
                int idx = sqlite3_bind_parameter_index(stmt, (":" + key).c_str());
                if (idx > 0) BindValue(stmt, idx, params[key]);
            }
        }

        int colCount = sqlite3_column_count(stmt);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            Json::Value row;
            for (int i = 0; i < colCount; i++) {
                const char* colName = sqlite3_column_name(stmt, i);
                int type = sqlite3_column_type(stmt, i);
                if (type == SQLITE_INTEGER) row[colName] = (Json::Int64)sqlite3_column_int64(stmt, i);
                else if (type == SQLITE_FLOAT) row[colName] = sqlite3_column_double(stmt, i);
                else if (type == SQLITE_TEXT) row[colName] = (const char*)sqlite3_column_text(stmt, i);
                else if (type == SQLITE_NULL) row[colName] = Json::Value::null;
            }
            if (!cb(row)) break;
        }
    }

    ~CDatabase() {
        for (auto& it : m_cache) sqlite3_finalize(it.second);
        if (m_db) sqlite3_close(m_db);
    }

private:
    CDatabase() : m_db(nullptr) {}
    sqlite3* m_db;
    std::map<std::string, sqlite3_stmt*> m_cache;

    sqlite3_stmt* Prepare(const std::string& name, const std::string& sql) {
        auto it = m_cache.find(name);
        if (it != m_cache.end()) return it->second;

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            return nullptr;
        }
        m_cache[name] = stmt;
        return stmt;
    }

    void BindValue(sqlite3_stmt* stmt, int idx, const Json::Value& v) {
        if (v.isInt64() || v.isInt()) sqlite3_bind_int64(stmt, idx, v.asInt64());
        else if (v.isUInt64() || v.isUInt()) sqlite3_bind_int64(stmt, idx, (sqlite3_int64)v.asUInt64());
        else if (v.isDouble()) sqlite3_bind_double(stmt, idx, v.asDouble());
        else if (v.isBool()) sqlite3_bind_int(stmt, idx, v.asBool());
        else if (v.isString()) sqlite3_bind_text(stmt, idx, v.asCString(), -1, SQLITE_TRANSIENT);
        else if (v.isNull()) sqlite3_bind_null(stmt, idx);
        else if (v.isObject() || v.isArray()) {
            std::string s = Json::writeString(Json::StreamWriterBuilder(), v);
            sqlite3_bind_text(stmt, idx, s.c_str(), -1, SQLITE_TRANSIENT);
        }
    }

    void InitSchema() {
        // Orange Code의 자율 오케스트레이션에 최적화된 "One Brain" 스키마

        // 1. 위계 구조 (목표 -> 프로젝트 -> 채팅)
        Execute(
            "CREATE TABLE IF NOT EXISTS Workflow ("
            "  key TEXT PRIMARY KEY,"         // 'goal:id', 'proj:id', 'chat:key'
            "  type TEXT NOT NULL,"           // 'goal', 'project', 'chat'
            "  parent_key TEXT,"              // 부모 위계 키
            "  title TEXT,"
            "  status TEXT,"                  // planning, in_progress, done 등
            "  progress INTEGER DEFAULT 0,"
            "  supervisor_id TEXT,"
            "  manager_id TEXT,"
            "  worker_id TEXT,"
            "  meta JSON,"                    // purpose, criteria, summary 등
            "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
            "  updated_at DATETIME DEFAULT CURRENT_TIMESTAMP"
            ");"
        );

        // 2. 실시간 활동 및 감사 로그 (TraceLog)
        Execute(
            "CREATE TABLE IF NOT EXISTS TraceLog ("
            "  seq INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  parent_seq INTEGER DEFAULT 0,"
            "  run_uid TEXT,"
            "  actor TEXT NOT NULL,"          // manager:pid, worker:pid
            "  intent TEXT,"                  // 무엇을 하는가
            "  command TEXT,"                 // 실행 명령
            "  payload JSON,"                 // 상세 데이터
            "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP"
            ");"
        );

        // 3. 실시간 인스턴스 활동 (ActorActivity) - 충돌 감지 및 세션 공유용
        Execute(
            "CREATE TABLE IF NOT EXISTS ActorActivity ("
            "  actor_id TEXT PRIMARY KEY,"    // manager:pid
            "  kind TEXT,"                    // manager, worker, supervisor
            "  pid INTEGER,"
            "  session_key TEXT,"
            "  workflow_key TEXT,"            // 현재 집중하고 있는 위계 키
            "  intent TEXT,"
            "  payload JSON,"
            "  updated_at DATETIME DEFAULT CURRENT_TIMESTAMP"
            ");"
        );

        // 4. 4축 리뷰 및 평가 결과
        Execute(
            "CREATE TABLE IF NOT EXISTS Reviews ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  workflow_key TEXT NOT NULL,"
            "  reviewer_id TEXT NOT NULL,"
            "  functional TEXT,"              // pass, partial, weak, fail
            "  implementation TEXT,"
            "  relation TEXT,"
            "  impact TEXT,"
            "  comment TEXT,"
            "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
            "  FOREIGN KEY(workflow_key) REFERENCES Workflow(key) ON DELETE CASCADE"
            ");"
        );

        // 5. 채팅 상세 데이터 (ChatBlocks, ChatAttachments) - Workflow(key)와 연동
        Execute(
            "CREATE TABLE IF NOT EXISTS ChatBlocks ("
            "  chat_key TEXT NOT NULL,"
            "  seq INTEGER NOT NULL,"
            "  role TEXT NOT NULL,"           // user, assistant, tool 등
            "  text TEXT NOT NULL,"
            "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
            "  PRIMARY KEY(chat_key, seq),"
            "  FOREIGN KEY(chat_key) REFERENCES Workflow(key) ON DELETE CASCADE"
            ");"
        );

        Execute(
            "CREATE TABLE IF NOT EXISTS ChatAttachments ("
            "  chat_key TEXT NOT NULL,"
            "  id TEXT NOT NULL,"
            "  kind TEXT NOT NULL,"
            "  name TEXT NOT NULL,"
            "  original_path TEXT,"
            "  stored_path TEXT NOT NULL,"
            "  thumbnail_path TEXT,"
            "  mime TEXT,"
            "  size_bytes INTEGER NOT NULL DEFAULT 0,"
            "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
            "  PRIMARY KEY(chat_key, id),"
            "  FOREIGN KEY(chat_key) REFERENCES Workflow(key) ON DELETE CASCADE"
            ");"
        );

        Execute(
            "CREATE TABLE IF NOT EXISTS prompt ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  scope TEXT NOT NULL DEFAULT 'global',"
            "  key TEXT NOT NULL,"
            "  priority INTEGER NOT NULL DEFAULT 100,"
            "  active INTEGER NOT NULL DEFAULT 1,"
            "  source TEXT,"
            "  content TEXT NOT NULL,"
            "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
            "  updated_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
            "  UNIQUE(scope, key)"
            ");"
        );

        Execute(
            "CREATE TABLE IF NOT EXISTS job ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  chat_key TEXT NOT NULL,"
            "  title TEXT NOT NULL,"
            "  instruction_type TEXT NOT NULL DEFAULT 'normal',"
            "  priority INTEGER NOT NULL DEFAULT 100,"
            "  status TEXT NOT NULL DEFAULT 'not_started',"
            "  progress INTEGER NOT NULL DEFAULT 0,"
            "  detail_count INTEGER NOT NULL DEFAULT 0,"
            "  started_at DATETIME,"
            "  ended_at DATETIME,"
            "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
            "  updated_at DATETIME DEFAULT CURRENT_TIMESTAMP"
            ");"
        );

        Execute(
            "CREATE TABLE IF NOT EXISTS job_detail ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  job_id INTEGER NOT NULL,"
            "  title TEXT NOT NULL,"
            "  status TEXT NOT NULL DEFAULT 'not_started',"
            "  progress INTEGER NOT NULL DEFAULT 0,"
            "  started_at DATETIME,"
            "  ended_at DATETIME,"
            "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
            "  updated_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
            "  FOREIGN KEY(job_id) REFERENCES job(id) ON DELETE CASCADE"
            ");"
        );

        // 인덱스 최적화
        Execute("CREATE INDEX IF NOT EXISTS idx_workflow_parent ON Workflow(parent_key);");
        Execute("CREATE INDEX IF NOT EXISTS idx_workflow_type ON Workflow(type);");
        Execute("CREATE INDEX IF NOT EXISTS idx_trace_parent ON TraceLog(parent_seq);");
        Execute("CREATE INDEX IF NOT EXISTS idx_trace_actor ON TraceLog(actor);");
        Execute("CREATE INDEX IF NOT EXISTS idx_reviews_workflow ON Reviews(workflow_key);");
        Execute("CREATE INDEX IF NOT EXISTS idx_chat_blocks_key ON ChatBlocks(chat_key);");
        Execute("CREATE INDEX IF NOT EXISTS idx_prompt_active ON prompt(active, priority DESC);");
        Execute("CREATE INDEX IF NOT EXISTS idx_job_chat_priority ON job(chat_key, priority DESC, updated_at DESC);");
        Execute("CREATE INDEX IF NOT EXISTS idx_job_detail_job ON job_detail(job_id);");
    }
};

} // namespace orange
