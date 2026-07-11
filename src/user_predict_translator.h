#ifndef RIME_USER_PREDICT_TRANSLATOR_H_
#define RIME_USER_PREDICT_TRANSLATOR_H_

#include <rime/common.h>
#include <rime/translator.h>

namespace rime {
namespace user_predict {

class UserPredictTranslator : public Translator {
 public:
  explicit UserPredictTranslator(const Ticket& ticket);

  an<Translation> Query(const string& input, const Segment& segment) override;
};

}  // namespace user_predict
}  // namespace rime

#endif  // RIME_USER_PREDICT_TRANSLATOR_H_
