#ifndef RIME_USER_PREDICT_FILTER_H_
#define RIME_USER_PREDICT_FILTER_H_

#include <map>
#include <string>

#include <rime/common.h>
#include <rime/filter.h>

namespace rime {
namespace user_predict {

class UserPredictFilter : public Filter {
 public:
  explicit UserPredictFilter(const Ticket& ticket);

  an<Translation> Apply(an<Translation> translation,
                        CandidateList* candidates) override;

 private:
  string f_last_commit_;
  map<string, int> f_reorder_map_;
  bool BuildReorderMap();
};

}  // namespace user_predict
}  // namespace rime

#endif  // RIME_USER_PREDICT_FILTER_H_
