#include <ctime>
#include <fstream>

#include <X11/keysym.h>
#include <utf8.h>

#include <rime/candidate.h>
#include <rime/common.h>
#include <rime/composition.h>
#include <rime/config.h>
#include <rime/context.h>
#include <rime/dict/level_db.h>
#include <rime/engine.h>
#include <rime/key_event.h>
#include <rime/schema.h>
#include <rime/service.h>

#include "user_predict_context.h"
#include "user_predict_processor.h"

namespace rime {
namespace user_predict {

namespace {

bool IsPunctuationKeycode(int ch) {
  return (ch >= XK_exclam && ch <= XK_slash) ||
         (ch >= XK_colon && ch <= XK_at) ||
         (ch >= XK_bracketleft && ch <= XK_grave) ||
         (ch >= XK_braceleft && ch <= XK_asciitilde);
}

}  // namespace

UserPredictProcessor::UserPredictProcessor(const Ticket& ticket)
    : Processor(ticket) {
  auto& ctx = UserPredictContext::instance();
  ctx.Initialize(engine_->schema()->config());

  Context* context = engine_->context();
  commit_connection_ =
      context->commit_notifier().connect([this](Context* c) { OnCommit(c); });
  update_connection_ =
      context->update_notifier().connect([this](Context* c) { OnUpdate(c); });
  delete_connection_ =
      context->delete_notifier().connect([this](Context* c) { OnDelete(c); });
}

UserPredictProcessor::~UserPredictProcessor() {
  commit_connection_.disconnect();
  update_connection_.disconnect();
  delete_connection_.disconnect();
}

ProcessResult UserPredictProcessor::ProcessKeyEvent(const KeyEvent& key_event) {
  auto& ctx = UserPredictContext::instance();
  if (!ctx.initialized())
    return kNoop;

  Context* context = engine_->context();
  if (!context)
    return kNoop;

  if (key_event.release())
    return kNoop;

  int ch = key_event.keycode();
  string input = context->input();

  if (ch == XK_BackSpace) {
    auto current_time = std::chrono::steady_clock::now();
    bool is_safe_to_undo = !context->IsComposing() || ctx.is_predicting();

    if (is_safe_to_undo && !ctx.undo_stack().empty()) {
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                         current_time - ctx.last_action_time())
                         .count();
      if (elapsed <= ctx.config().context_timeout_ms) {
        RollbackLastWrite();
        ctx.last_action_time() = current_time;
      } else {
        ctx.undo_stack().clear();
      }
    }

    just_committed_ = false;
    if (ctx.is_predicting()) {
      ctx.ResetMemoryChain();
      context->Clear();
      return kAccepted;
    }
  }

  if (ch == XK_space && !context->composition().empty() &&
      context->composition().back().HasTag("prediction")) {
    if (!ctx.config().use_space_to_commit) {
      engine_->sink()(" ");
      return kAccepted;
    }
    return kNoop;
  }

  if (ctx.is_predicting()) {
    if ((ch >= XK_0 && ch <= XK_9) || (ch >= XK_KP_0 && ch <= XK_KP_9)) {
      int digit = (ch >= XK_KP_0 && ch <= XK_KP_9) ? ch - XK_KP_0 : ch - XK_0;
      ctx.ResetMemoryChain();
      context->Clear();
      engine_->CommitText(std::to_string(digit));
      return kAccepted;
    }

    if (ch == XK_BackSpace || ch == XK_Escape) {
      ctx.ResetMemoryChain();
      context->Clear();
      return kAccepted;
    }

    if (IsPunctuationKeycode(ch)) {
      return kNoop;
    }

    ctx.is_predicting() = false;
    ctx.predict_count() = 0;
    ctx.pending_cands().clear();
    context->Clear();
    return kNoop;
  }

  if (just_committed_ && ch != XK_BackSpace && !key_event.shift() &&
      !key_event.ctrl() && !key_event.alt()) {
    just_committed_ = false;
  }

  if (!context->IsComposing()) {
    if (ch == XK_Return || ch == XK_KP_Enter || ch == XK_space) {
      ctx.ResetMemoryChain();
      return kNoop;
    }
  }

  if (context->HasMenu() && (key_event.shift() || key_event.ctrl()) &&
      (ch == XK_Delete || ch == XK_BackSpace)) {
    auto cand = context->GetSelectedCandidate();
    if (cand && cand->type() == "predict") {
      ctx.RemoveCandidate(cand->text());
      context->Clear();
      ctx.ResetMemoryChain();
      return kAccepted;
    }
  }

  return kNoop;
}

void UserPredictProcessor::RollbackLastWrite() {
  auto& ctx = UserPredictContext::instance();
  auto& stack = ctx.undo_stack();
  if (stack.empty())
    return;

  auto keys_to_undo = stack.back();
  stack.pop_back();

  auto* db = ctx.db();
  if (!db)
    return;

  for (const auto& [k, v] : keys_to_undo) {
    if (v.empty()) {
      db->Erase(k);
    } else {
      db->Update(k, v);
    }
  }
}

void UserPredictProcessor::OnCommit(Context* ctx) {
  auto& state = UserPredictContext::instance();
  if (!state.initialized())
    return;

  string text = ctx->GetCommitText();
  if (text.empty())
    return;

  if (state.IsPunctuation(text)) {
    auto current_time = std::chrono::steady_clock::now();
    state.last_commit_time() = current_time;
    state.last_action_time() = current_time;
    bool prediction_on = ctx->get_option("prediction");
    if (prediction_on && state.config().predict_style != "off") {
      state.pending_cands() = state.GetPredictions(state.last_commit());
    }
    if (state.config().predict_style == "post" ||
        state.config().predict_style == "all") {
      need_create_predict_segment_ = true;
    }
    return;
  }

  if (!state.IsValidCommitText(text)) {
    state.ResetMemoryChain();
    return;
  }

  auto current_time = std::chrono::steady_clock::now();
  if (!state.last_commit().empty()) {
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       current_time - state.last_commit_time())
                       .count();
    if (elapsed > state.config().context_timeout_ms) {
      state.ResetMemoryChain();
    }
  }

  if (!state.is_predicting()) {
    state.is_predicting() = true;
    state.predict_count() = 1;
  } else {
    state.predict_count()++;
  }

  bool is_sgram_prediction = false;
  for (const auto& c : state.pending_cands()) {
    if (c.word == text && c.db_key.compare(0, 2, "S\t") == 0) {
      is_sgram_prediction = true;
      break;
    }
  }

  if (!is_sgram_prediction &&
      state.predict_count() > state.config().max_predictions) {
    state.is_predicting() = false;
    state.predict_count() = 0;
    state.pending_cands().clear();
    return;
  }

  LearnCommit(text);

  state.last_commit_time() = current_time;
  state.last_action_time() = current_time;
  just_committed_ = true;

  bool prediction_on = ctx->get_option("prediction");
  if ((is_sgram_prediction ||
       state.predict_count() <= state.config().max_predictions) &&
      prediction_on) {
    if (state.config().predict_style != "off") {
      state.pending_cands() = state.GetPredictions(state.last_commit());
    }
    if (state.config().predict_style == "post" ||
        state.config().predict_style == "all") {
      need_create_predict_segment_ = true;
    }
    if (state.config().predict_style != "post" &&
        state.config().predict_style != "all") {
      state.predict_count() = 0;
      state.is_predicting() = false;
    }
  } else {
    state.predict_count() = 0;
    state.is_predicting() = false;
    state.pending_cands().clear();
  }
}

void UserPredictProcessor::LearnCommit(const string& text) {
  auto& state = UserPredictContext::instance();
  auto* db = state.db();
  if (!db)
    return;

  db->BeginTransaction();

  map<string, string> last_written_keys;

  auto update_memory = [&](const string& key) {
    string val;
    db->Fetch(key, &val);
    last_written_keys[key] = val;

    time_t now = std::time(nullptr);
    if (val.empty()) {
      db->Update(key, string{"1|"} + std::to_string(static_cast<int>(now)));
    } else {
      size_t pipe_pos = val.find('|');
      int count = 1;
      time_t ts = 0;
      if (pipe_pos != string::npos) {
        count = std::stoi(val.substr(0, pipe_pos));
        ts = static_cast<time_t>(std::stoll(val.substr(pipe_pos + 1)));
      } else {
        count = std::stoi(val);
      }

      time_t age = now - ts;
      int limit = state.config().expiry_seconds;

      if (age > limit) {
        db->Update(key, string{"1|"} + std::to_string(static_cast<int>(now)));
      } else {
        db->Update(key, std::to_string(count + 1) + "|" +
                            std::to_string(static_cast<int>(now)));
      }
    }
  };

  bool should_record = true;
  bool is_terminal_symbol = false;
  auto text_chars = UserPredictContext::GetUtf8Chars(text);
  int len_text = static_cast<int>(text_chars.size());

  if (len_text > 4) {
    should_record = false;
  }

  if (should_record && UserPredictContext::IsToneSymbol(text)) {
    auto prev_chars = UserPredictContext::GetUtf8Chars(state.last_commit());
    if (!prev_chars.empty()) {
      const string& last_char = prev_chars.back();
      const char* p = last_char.c_str();
      char32_t cp = utf8::unchecked::next(p);
      if (!UserPredictContext::IsParticle(cp)) {
        should_record = false;
        state.ResetMemoryChain();
      } else {
        is_terminal_symbol = true;
      }
    }
  }

  if (should_record && state.last_commit() == text) {
    should_record = false;
  }

  if (should_record && state.history().size() >= 2) {
    if (text == state.history()[state.history().size() - 2]) {
      should_record = false;
      state.history().pop_back();
      state.last_commit() =
          state.history().empty() ? string{} : state.history().back();
    }
  }

  if (should_record) {
    bool text_is_tone = UserPredictContext::IsToneSymbol(text);

    if (!state.last_commit().empty()) {
      auto u1_chars = UserPredictContext::GetUtf8Chars(state.last_commit());
      int len_u1 = static_cast<int>(u1_chars.size());

      auto lengths = UserPredictContext::GetSuffixLengths(len_u1);
      for (int l : lengths) {
        if (l < len_u1 || len_u1 >= 4) {
          string suffix;
          for (int j = len_u1 - l; j < len_u1; j++)
            suffix += u1_chars[j];
          update_memory(string{"P\t"} + suffix + "\t" + text);
        }
      }

      if (len_u1 <= 4 && !state.history().empty()) {
        update_memory(string{"1\t"} + state.last_commit() + "\t" + text);
      }

      if (len_u1 <= 4 && state.history().size() >= 2) {
        const auto& u0 = state.history()[state.history().size() - 2];
        int len_u0 = static_cast<int>(UserPredictContext::Utf8Len(u0));
        if ((len_u0 + len_u1) <= 5) {
          update_memory(string{"2\t"} + u0 + "\t" + state.last_commit() + "\t" +
                        text);
        }
      }
    }

    if (len_text == 4) {
      string part1 = text_chars[0] + text_chars[1];
      string part2 = text_chars[2] + text_chars[3];
      bool is_known_prefix = false;
      for (const auto& prefix : {"1", "P"}) {
        string query_key = string{prefix} + "\t" + part1 + "\t";
        auto da = db->Query(query_key);
        if (da && !da->exhausted()) {
          string k, v;
          if (da->GetNextRecord(&k, &v) &&
              k.compare(0, query_key.size(), query_key) == 0) {
            is_known_prefix = true;
            break;
          }
        }
      }
      if (is_known_prefix) {
        update_memory(string{"1\t"} + part1 + "\t" + part2);
      }
    }
  }

  if (is_terminal_symbol) {
    state.ResetMemoryChain();
  } else if (state.last_commit() != text) {
    state.history().push_back(text);
    if (state.history().size() > 2)
      state.history().erase(state.history().begin());
    state.last_commit() = text;
  }

  db->CommitTransaction();

  auto& undo_stack = state.undo_stack();
  if (!last_written_keys.empty()) {
    undo_stack.push_back(last_written_keys);
    if (undo_stack.size() > 3)
      undo_stack.pop_front();
  }
}

void UserPredictProcessor::OnUpdate(Context* ctx) {
  auto& state = UserPredictContext::instance();
  if (!state.initialized())
    return;

  string input = ctx->input();

  if (input == "/clean") {
    ctx->Clear();
    int deleted = state.CleanExpiredEntries(string{});
    engine_->CommitText("Predict DB cleaned: " + std::to_string(deleted) +
                        " expired entries removed.");
    return;
  }

  if (input == "/outpredict") {
    ctx->Clear();
    string data = state.ExportAll();
    if (!data.empty()) {
      path export_path =
          Service::instance().deployer().user_data_dir / "predict_export.txt";
      std::ofstream out(export_path.u8string());
      if (out.is_open()) {
        out << data;
        out.close();
      }
    }
    return;
  }

  if (input == "/inpredict") {
    ctx->Clear();
    path import_path =
        Service::instance().deployer().user_data_dir / "predict_import.txt";
    std::ifstream in(import_path.u8string());
    if (in.is_open()) {
      state.ImportFromFile(in);
    }
    return;
  }

  if (self_updating_)
    return;

  if ((state.config().predict_style == "post" ||
       state.config().predict_style == "all") &&
      !ctx->IsComposing() && ctx->get_option("prediction") &&
      !state.pending_cands().empty() && need_create_predict_segment_) {
    CreatePredictSegment(ctx);
    need_create_predict_segment_ = false;
    predict_segment_created_ = true;
    return;
  }

  if (state.is_predicting() && ctx->input().empty() &&
      !need_create_predict_segment_) {
    if (predict_segment_created_) {
      predict_segment_created_ = false;
    } else {
      state.is_predicting() = false;
      state.predict_count() = 0;
      state.pending_cands().clear();
      ctx->Clear();
    }
  }
}

void UserPredictProcessor::CreatePredictSegment(Context* ctx) {
  self_updating_ = true;
  Segment seg(ctx->input().length(), ctx->input().length());
  seg.tags.insert("prediction");
  seg.tags.insert("placeholder");
  ctx->composition().AddSegment(seg);
  if (!ctx->composition().empty())
    ctx->composition().back().tags.erase("raw");
  ctx->update_notifier()(ctx);
  self_updating_ = false;
}

void UserPredictProcessor::OnDelete(Context* ctx) {
  auto& state = UserPredictContext::instance();
  if (!state.initialized())
    return;

  if (!ctx || !ctx->composition().empty())
    return;

  auto comp = &ctx->composition();
  if (comp->empty())
    return;

  auto seg = &comp->back();
  auto idx = seg->selected_index;
  auto cand = seg->GetCandidateAt(idx);

  if (cand && cand->type() == "predict") {
    state.RemoveCandidate(cand->text());
    ctx->Clear();
    state.ResetMemoryChain();
  }
}

}  // namespace user_predict
}  // namespace rime
