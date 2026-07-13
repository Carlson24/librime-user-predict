#include <rime/candidate.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/segmentation.h>
#include <rime/translation.h>

#include "user_predict_context.h"
#include "user_predict_translator.h"

namespace rime {
namespace user_predict {

UserPredictTranslator::UserPredictTranslator(const Ticket& ticket)
    : Translator(ticket) {}

an<Translation> UserPredictTranslator::Query(const string& input,
                                             const Segment& segment) {
  if (!segment.HasTag("prediction"))
    return nullptr;

  auto& state = UserPredictContext::instance();
  if (!state.initialized())
    return nullptr;

  if (state.config().predict_style != "post" &&
      state.config().predict_style != "all")
    return nullptr;

  if (!engine_->context()->get_option("prediction"))
    return nullptr;

  if (state.pending_cands().empty())
    return nullptr;

  auto translation = New<FifoTranslation>();
  int count = 0;
  for (const auto& c : state.pending_cands()) {
    if (count >= state.config().max_candidates)
      break;
    auto cand =
        New<SimpleCandidate>("prediction", segment.start, segment.end, c.word);
    cand->set_quality(1000 - count);
    translation->Append(cand);
    count++;
  }
  return translation;
}

}  // namespace user_predict
}  // namespace rime
