#include <algorithm>
#include <cmath>
#include <ctime>
#include <fstream>
#include <set>

#include <utf8.h>

#include <rime/config.h>
#include <rime/dict/level_db.h>
#include <rime/service.h>

#include "user_predict_context.h"

namespace rime {
namespace user_predict {

static const char kMetaLastClean[] = "\x01last_clean_time";

static const int kCleanInterval = 259200;

static bool IsParticleCp(char32_t cp) {
  static const set<char32_t> kParticles = {
      0x5427, 0x5462, 0x5417, 0x5566, 0x561b, 0x5440, 0x6069, 0x6B38, 0x54D2,
      0x54C8, 0x54C7, 0x554A, 0x54E6, 0x5662, 0x549F, 0x8D4F, 0x54DF, 0x5466,
      0x5509, 0x5531, 0x4E48, 0x5565, 0x8C01, 0x54EA, 0x91CC, 0x513F, 0x4E86,
      0x7684, 0x8FC7, 0x597D, 0x884C, 0x5BF9, 0x6210,
  };
  return kParticles.count(cp) > 0;
}

bool UserPredictContext::IsParticle(char32_t cp) {
  return IsParticleCp(cp);
}

bool UserPredictContext::IsChineseChar(const string& c) {
  if (c.empty())
    return false;
  const char* p = c.c_str();
  uint32_t cp = utf8::unchecked::next(p);
  return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
         (cp >= 0x20000 && cp <= 0x2A6DF) || (cp >= 0x2A700 && cp <= 0x2B73F) ||
         (cp >= 0x2B740 && cp <= 0x2B81F) || (cp >= 0x2B820 && cp <= 0x2CEAF) ||
         (cp >= 0x2CEB0 && cp <= 0x2EBEF) || (cp >= 0x30000 && cp <= 0x3134F) ||
         (cp >= 0x31350 && cp <= 0x323AF) || (cp >= 0x2EBF0 && cp <= 0x2EE5F) ||
         (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0x2F800 && cp <= 0x2FA1F) ||
         (cp >= 0x2E80 && cp <= 0x2EFF) || (cp >= 0x2F00 && cp <= 0x2FDF);
}

bool UserPredictContext::IsToneSymbol(const string& text) {
  if (text.empty())
    return false;
  auto chars = GetUtf8Chars(text);
  static const set<string> kToneSymbols = {"\xEF\xBC\x81", "\xEF\xBC\x9F",
                                           "\xEF\xBC\x8C", "\xE3\x80\x82",
                                           "\xEF\xBD\x9E"};
  for (const auto& c : chars) {
    if (kToneSymbols.find(c) == kToneSymbols.end())
      return false;
  }
  return true;
}

bool UserPredictContext::IsValidCommitText(const string& text) {
  if (text.empty())
    return false;
  if (IsToneSymbol(text))
    return true;
  auto chars = GetUtf8Chars(text);
  for (const auto& c : chars) {
    if (!IsChineseChar(c))
      return false;
  }
  return true;
}

vector<string> UserPredictContext::GetUtf8Chars(const string& str) {
  vector<string> chars;
  if (str.empty())
    return chars;

  auto it = str.begin();
  while (it != str.end()) {
    auto start = it;
    utf8::unchecked::next(it);
    chars.push_back(string(start, it));
  }
  return chars;
}

size_t UserPredictContext::Utf8Len(const string& str) {
  return utf8::unchecked::distance(str.c_str(), str.c_str() + str.length());
}

vector<int> UserPredictContext::GetSuffixLengths(int len) {
  if (len >= 4)
    return {4, 3, 2};
  if (len == 3)
    return {3, 2};
  if (len == 2)
    return {2};
  if (len == 1)
    return {1};
  return {};
}

void UserPredictContext::ResetMemoryChain() {
  history_.clear();
  last_commit_.clear();
  last_commit_time_ = TimePoint{};
  predict_count_ = 0;
  is_predicting_ = false;
  pending_cands_.clear();
}

UserPredictContext::~UserPredictContext() {
  if (db_)
    db_->Close();
}

UserPredictContext& UserPredictContext::instance() {
  static UserPredictContext ctx;
  return ctx;
}

void UserPredictContext::Initialize(Config* config) {
  if (initialized_)
    return;
  initialized_ = true;

  LoadConfig(config);
  EnsureDb();
  PeriodicCleanup();
}

void UserPredictContext::Cleanup() {
  if (db_)
    db_->Close();
  db_.reset();
  initialized_ = false;
}

void UserPredictContext::LoadConfig(Config* config) {
  if (!config)
    return;

  config->GetInt("user_predict/max_candidates", &config_.max_candidates);
  config->GetInt("user_predict/max_predictions", &config_.max_predictions);

  int expiry_days;
  if (config->GetInt("user_predict/expiry_days", &expiry_days))
    config_.expiry_seconds = expiry_days * 86400;

  config->GetInt("user_predict/max_memory_branches",
                 &config_.max_memory_branches);
  config->GetDouble("user_predict/decay_rate", &config_.decay_rate);

  int timeout;
  if (config->GetInt("user_predict/context_timeout", &timeout))
    config_.context_timeout_ms = timeout;

  config->GetString("user_predict/predict_style", &config_.predict_style);

  string db_name;
  if (config->GetString("user_predict/db_name", &db_name))
    config_.db_name = db_name;
  if (config_.db_name.size() < 7 ||
      config_.db_name.compare(config_.db_name.size() - 7, 7, ".userdb") != 0) {
    config_.db_name += ".userdb";
  }
}

bool UserPredictContext::EnsureDb() {
  if (db_ && db_->loaded())
    return true;

  path db_path = Service::instance().deployer().user_data_dir / config_.db_name;

  db_ = make_unique<LevelDb>(db_path, config_.db_name);
  if (!db_->Exists()) {
    if (!db_->Open())
      return false;
  } else {
    if (!db_->Open())
      return false;
  }
  return true;
}

LevelDb* UserPredictContext::db() {
  if (!EnsureDb())
    return nullptr;
  return db_.get();
}

void UserPredictContext::PeriodicCleanup() {
  if (!db_)
    return;

  time_t now = std::time(nullptr);

  string last_clean_str;
  time_t last_clean_time = 0;
  if (db_->Fetch(kMetaLastClean, &last_clean_str)) {
    last_clean_time = static_cast<time_t>(std::stoll(last_clean_str));
  }

  if ((now - last_clean_time) <= kCleanInterval)
    return;

  auto da = db_->Query(string{});
  if (!da || da->exhausted())
    return;

  int deleted_count = 0;
  string key, value;
  while (da->GetNextRecord(&key, &value)) {
    if (key.empty() || key[0] == '\x01' || key[0] == '\0')
      continue;

    size_t pipe_pos = value.find('|');
    time_t ts = 0;
    if (pipe_pos != string::npos) {
      ts = static_cast<time_t>(std::stoll(value.substr(pipe_pos + 1)));
    }

    int limit;
    if (key.compare(0, 2, "S\t") == 0) {
      limit = INT32_MAX;
    } else {
      bool is_p_gram = (key.compare(0, 2, "P\t") == 0);
      limit = is_p_gram ? config_.p_expiry_seconds : config_.expiry_seconds;
    }

    if (ts == 0)
      ts = now - limit - 1;

    if ((now - ts) > limit) {
      db_->Erase(key);
      deleted_count++;
    }
  }

  db_->Update(kMetaLastClean, std::to_string(now));

  if (deleted_count > 0) {
    LOG(INFO) << "user_predict: cleaned " << deleted_count << " expired entries.";
  }
}

void UserPredictContext::FetchAndClean(const string& query_key,
                                       double multiplier,
                                       vector<PredictCandidate>& cands,
                                       set<string>& seen) {
  if (!db_)
    return;

  auto da = db_->Query(query_key);
  if (!da || da->exhausted()) {
    return;
  }

  time_t now = std::time(nullptr);
  int scan_count = 0;
  vector<PredictCandidate> prefix_cands;

  string key, value;
  while (da->GetNextRecord(&key, &value)) {
    if (scan_count >= config_.scan_limit)
      break;
    if (key.compare(0, query_key.size(), query_key) != 0)
      break;

    if (!key.empty() && key[0] == '\x01')
      continue;

    string word = key.substr(query_key.size());

    size_t pipe_pos = value.find('|');
    int count = 0;
    time_t ts = 0;
    if (pipe_pos == string::npos) {
      count = std::stoi(value);
    } else {
      count = std::stoi(value.substr(0, pipe_pos));
      ts = static_cast<time_t>(std::stoll(value.substr(pipe_pos + 1)));
    }

    int limit;
    if (key.compare(0, 2, "S\t") == 0) {
      limit = INT32_MAX;
    } else {
      bool is_p_gram = (key.compare(0, 2, "P\t") == 0);
      limit = is_p_gram ? config_.p_expiry_seconds : config_.expiry_seconds;
    }

    if (ts == 0)
      ts = now - limit - 1;

    if ((now - ts) > limit) {
      db_->Erase(key);
    } else if (count > 0) {
      double age_days = (now - ts) / 86400.0;
      double score = count * pow(config_.decay_rate, age_days) * multiplier;
      if (score > 0.05 && !word.empty()) {
        prefix_cands.push_back({word, score, key});
      }
    }
    scan_count++;
  }

  if (!prefix_cands.empty()) {
    std::sort(prefix_cands.begin(), prefix_cands.end(),
              [](const PredictCandidate& a, const PredictCandidate& b) {
                return a.weight > b.weight;
              });

    int i = 0;
    for (const auto& c : prefix_cands) {
      if (i >= config_.max_memory_branches) {
        db_->Update(c.db_key,
                    string{"0|"} + std::to_string(static_cast<int>(now)));
      } else {
        if (seen.find(c.word) == seen.end()) {
          cands.push_back(c);
          seen.insert(c.word);
        } else {
        }
      }
      i++;
    }
  }
}

vector<PredictCandidate> UserPredictContext::GetPredictions(
    const string& prev_commit) {
  if (prev_commit.empty() || !db_)
    return {};

  vector<PredictCandidate> cands;
  set<string> seen;

  if (!history_.empty()) {
    FetchAndClean(string{"S\t"} + history_.back() + "\t", 1000000.0, cands,
                  seen);
  }

  if (history_.size() >= 2) {
    const auto& u0 = history_[history_.size() - 2];
    const auto& u1 = history_.back();
    size_t len_u0 = Utf8Len(u0);
    size_t len_u1 = Utf8Len(u1);

    if (len_u1 <= 4 && (len_u0 + len_u1) <= 5) {
      FetchAndClean(string{"2\t"} + u0 + "\t" + u1 + "\t", 10000.0, cands,
                    seen);
    }
  }

  if (cands.size() < static_cast<size_t>(config_.max_candidates) &&
      !history_.empty()) {
    const auto& u1 = history_.back();
    auto chars = GetUtf8Chars(u1);
    size_t len_u1 = chars.size();

    int max_len = std::min(len_u1, size_t{4});
    int min_len = (len_u1 >= 2) ? 2 : 1;

    for (int l = max_len; l >= min_len; l--) {
      string lookup_u1;
      for (size_t j = len_u1 - l; j < len_u1; j++)
        lookup_u1 += chars[j];
      FetchAndClean(string{"1\t"} + lookup_u1 + "\t", 100.0, cands, seen);
      if (!cands.empty())
        break;
    }
  }

  if (cands.size() < static_cast<size_t>(config_.max_candidates)) {
    auto chars = GetUtf8Chars(prev_commit);
    auto lengths = GetSuffixLengths(static_cast<int>(chars.size()));
    for (int l : lengths) {
      string suffix;
      for (size_t j = chars.size() - l; j < chars.size(); j++)
        suffix += chars[j];
      FetchAndClean(string{"P\t"} + suffix + "\t", 1.0, cands, seen);
      if (!cands.empty())
        break;
    }
  }

  if (!cands.empty()) {
    std::sort(cands.begin(), cands.end(),
              [](const PredictCandidate& a, const PredictCandidate& b) {
                return a.weight > b.weight;
              });
  }
  return cands;
}

void UserPredictContext::RemoveCandidate(const string& word) {
  if (!db_)
    return;

  if (!pending_cands_.empty()) {
    for (const auto& c : pending_cands_) {
      if (c.word == word) {
        db_->Erase(c.db_key);
        break;
      }
    }
  }

  auto chars = GetUtf8Chars(last_commit_);
  auto lengths = GetSuffixLengths(static_cast<int>(chars.size()));
  for (int l : lengths) {
    string suffix;
    for (size_t j = chars.size() - l; j < chars.size(); j++)
      suffix += chars[j];
    string p_key = string{"P\t"} + suffix + "\t" + word;
    db_->Erase(p_key);
  }
}

int UserPredictContext::CleanExpiredEntries(const string& key_prefix) {
  if (!db_)
    return 0;

  time_t now = std::time(nullptr);
  int deleted_count = 0;

  auto da = db_->Query(key_prefix);
  if (da && !da->exhausted()) {
    string key, value;
    while (da->GetNextRecord(&key, &value)) {
      if (key.empty() || key[0] == '\x01' || key[0] == '\0')
        continue;

      size_t pipe_pos = value.find('|');
      time_t ts = 0;
      if (pipe_pos != string::npos) {
        ts = static_cast<time_t>(std::stoll(value.substr(pipe_pos + 1)));
      }

      int limit;
      if (key.compare(0, 2, "S\t") == 0) {
        limit = INT32_MAX;
      } else {
        bool is_p_gram = (key.compare(0, 2, "P\t") == 0);
        limit = is_p_gram ? config_.p_expiry_seconds : config_.expiry_seconds;
      }

      if (ts == 0)
        ts = now - limit - 1;

      if ((now - ts) > limit) {
        db_->Erase(key);
        deleted_count++;
      }
    }
  }

  ResetMemoryChain();
  return deleted_count;
}

string UserPredictContext::ExportAll() {
  if (!db_)
    return {};

  string result;
  auto da = db_->Query(string{});
  if (!da || da->exhausted())
    return result;

  string key, value;
  while (da->GetNextRecord(&key, &value)) {
    if (key.empty() || key[0] == '\x01' || key[0] == '\0')
      continue;
    result += key + "\t" + value + "\n";
  }

  ResetMemoryChain();
  return result;
}

void UserPredictContext::ImportFromFile(std::istream& in) {
  if (!db_ || !in)
    return;

  auto update_db = [&](const string& key, const string& value) {
    string old_v;
    if (db_->Fetch(key, &old_v) && !old_v.empty()) {
      size_t old_pipe = old_v.find('|');
      size_t new_pipe = value.find('|');
      time_t old_ts = 0, new_ts = 0;
      if (old_pipe != string::npos)
        old_ts = static_cast<time_t>(std::stoll(old_v.substr(old_pipe + 1)));
      if (new_pipe != string::npos)
        new_ts = static_cast<time_t>(std::stoll(value.substr(new_pipe + 1)));

      if (new_ts > old_ts)
        db_->Update(key, value);
    } else {
      db_->Update(key, value);
    }
  };

  string line;
  while (std::getline(in, line)) {
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
      line.pop_back();

    size_t tab_pos = line.find('\t');
    if (tab_pos == string::npos)
      continue;

    if (line.compare(0, 2, "S\t") == 0) {
      string ts = std::to_string(std::time(nullptr));
      string value = "1|" + ts;

      size_t pos = 0;
      vector<string> tokens;
      while (pos < line.size()) {
        size_t next = line.find('\t', pos);
        if (next == string::npos) {
          tokens.push_back(line.substr(pos));
          break;
        }
        tokens.push_back(line.substr(pos, next - pos));
        pos = next + 1;
      }

      for (size_t i = 1; i + 1 < tokens.size(); i++) {
        string key = "S\t" + tokens[i] + "\t" + tokens[i + 1];
        update_db(key, value);
      }
    } else {
      string k = line.substr(0, tab_pos);
      string v = line.substr(tab_pos + 1);
      update_db(k, v);
    }
  }

  ResetMemoryChain();
}

}  // namespace user_predict
}  // namespace rime
