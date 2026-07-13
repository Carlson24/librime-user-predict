#ifndef RIME_USER_PREDICT_CONTEXT_H_
#define RIME_USER_PREDICT_CONTEXT_H_

#include <chrono>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include <rime/common.h>
#include <rime/dict/db.h>

namespace rime {

class Config;
class LevelDb;

namespace user_predict {

using TimePoint = std::chrono::steady_clock::time_point;

struct UserPredictConfig {
  int max_candidates = 5;
  int max_predictions = 3;
  int expiry_seconds = 90 * 24 * 3600;
  int p_expiry_seconds = 30 * 24 * 3600;
  int max_memory_branches = 15;
  double decay_rate = 0.85;
  int scan_limit = 80;
  int64_t context_timeout_ms = 30000;
  string predict_style = "reorder";
  string db_name = "user_predict";
  bool use_space_to_commit = true;
};

struct PredictCandidate {
  string word;
  double weight;
  string db_key;
};

class UserPredictContext {
 public:
  static UserPredictContext& instance();

  void Initialize(Config* config);
  void Cleanup();

  LevelDb* db();
  const UserPredictConfig& config() const { return config_; }

  vector<string>& history() { return history_; }
  string& last_commit() { return last_commit_; }
  TimePoint& last_commit_time() { return last_commit_time_; }
  int& predict_count() { return predict_count_; }
  bool& is_predicting() { return is_predicting_; }
  vector<PredictCandidate>& pending_cands() { return pending_cands_; }
  deque<map<string, string>>& undo_stack() { return undo_stack_; }
  TimePoint& last_action_time() { return last_action_time_; }
  bool initialized() const { return initialized_; }

  vector<PredictCandidate> GetPredictions(const string& prev_commit);
  void RemoveCandidate(const string& word);

  static bool IsChineseChar(const string& c);
  static bool IsToneSymbol(const string& text);
  static bool IsValidCommitText(const string& text);
  static bool IsPunctuation(const string& text);
  static vector<string> GetUtf8Chars(const string& str);
  static size_t Utf8Len(const string& str);
  static vector<int> GetSuffixLengths(int len);
  static bool IsParticle(char32_t cp);

  int CleanExpiredEntries(const string& key_prefix);
  string ExportAll();
  void ImportFromFile(std::istream& in);
  void PeriodicCleanup();

  void ResetMemoryChain();

 private:
  UserPredictContext() = default;
  ~UserPredictContext();
  UserPredictContext(const UserPredictContext&) = delete;
  UserPredictContext& operator=(const UserPredictContext&) = delete;

  void LoadConfig(Config* config);
  bool EnsureDb();
  void FetchAndClean(const string& query_key,
                     double multiplier,
                     vector<PredictCandidate>& cands,
                     set<string>& seen);

  UserPredictConfig config_;
  the<LevelDb> db_;

  vector<string> history_;
  string last_commit_;
  TimePoint last_commit_time_;
  int predict_count_ = 0;
  bool is_predicting_ = false;
  vector<PredictCandidate> pending_cands_;
  deque<map<string, string>> undo_stack_;
  TimePoint last_action_time_;
  bool initialized_ = false;
};

}  // namespace user_predict
}  // namespace rime

#endif  // RIME_USER_PREDICT_CONTEXT_H_
