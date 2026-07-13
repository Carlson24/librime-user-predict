#include <algorithm>

#include <utf8.h>

#include <rime/candidate.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/translation.h>

#include "user_predict_context.h"
#include "user_predict_filter.h"

namespace rime {
namespace user_predict {

class UserPredictTranslation : public PrefetchTranslation {
 public:
  UserPredictTranslation(an<Translation> translation,
                         const map<string, int>& reorder_map)
      : PrefetchTranslation(translation) {
    Rearrange(reorder_map);
  }

 private:
  void Rearrange(const map<string, int>& reorder_map) {
    if (exhausted())
      return;

    int count = 0;
    int target_len = 0;
    size_t target_end = 0;
    const int max_scan = 20;

    struct Boosted {
      an<Candidate> cand;
      int rank;
      int index;
    };
    vector<Boosted> boosted;
    CandidateQueue normal;

    while (!translation_->exhausted()) {
      count++;
      auto cand = translation_->Peek();
      string text = cand->text();
      string ct = cand->type();
      int current_len = UserPredictContext::Utf8Len(text);

      if (count == 1) {
        target_len = current_len;
        target_end = cand->end();
      }

      bool length_mismatch = (cand->end() != target_end) ||
                             (count > 1 && current_len != target_len);

      if (ct == "raw" || ct == "english" || length_mismatch ||
          count > max_scan) {
        break;
      }

      auto it = reorder_map.find(text);
      if (it != reorder_map.end() && current_len == target_len) {
        boosted.push_back({cand, it->second, count});
      } else {
        normal.push_back(cand);
      }

      translation_->Next();
    }

    std::stable_sort(boosted.begin(), boosted.end(),
                     [](const Boosted& a, const Boosted& b) {
                       if (a.rank == b.rank)
                         return a.index < b.index;
                       return a.rank < b.rank;
                     });

    for (auto& b : boosted)
      cache_.push_back(b.cand);
    for (auto& n : normal)
      cache_.push_back(n);
  }
};

UserPredictFilter::UserPredictFilter(const Ticket& ticket) : Filter(ticket) {}

an<Translation> UserPredictFilter::Apply(an<Translation> translation,
                                         CandidateList* candidates) {
  auto& state = UserPredictContext::instance();
  if (!state.initialized())
    return translation;

  Context* ctx = engine_->context();
  if (!ctx)
    return translation;

  if (!ctx->get_option("prediction"))
    return translation;

  if (state.config().predict_style != "reorder" &&
      state.config().predict_style != "all")
    return translation;

  if (BuildReorderMap()) {
    return New<UserPredictTranslation>(translation, f_reorder_map_);
  }

  return translation;
}

bool UserPredictFilter::BuildReorderMap() {
  auto& state = UserPredictContext::instance();

  if (f_last_commit_ == state.last_commit())
    return !f_reorder_map_.empty();

  f_last_commit_ = state.last_commit();
  f_reorder_map_.clear();

  size_t u1_len = UserPredictContext::Utf8Len(state.last_commit());
  bool is_context_valid = false;

  if (state.history().size() >= 2) {
    size_t u0_len = UserPredictContext::Utf8Len(
        state.history()[state.history().size() - 2]);
    if ((u0_len + u1_len) >= 3)
      is_context_valid = true;
  } else {
    if (u1_len >= 2)
      is_context_valid = true;
  }

  if (is_context_valid && state.config().predict_style != "off") {
    auto preds = state.GetPredictions(state.last_commit());
    int rank = 0;
    for (const auto& p : preds) {
      f_reorder_map_[p.word] = rank++;
    }
  }

  return !f_reorder_map_.empty();
}

}  // namespace user_predict
}  // namespace rime
