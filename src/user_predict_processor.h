#ifndef RIME_USER_PREDICT_PROCESSOR_H_
#define RIME_USER_PREDICT_PROCESSOR_H_

#include <rime/common.h>
#include <rime/processor.h>

namespace rime {

class Context;

namespace user_predict {

class UserPredictProcessor : public Processor {
 public:
  explicit UserPredictProcessor(const Ticket& ticket);
  ~UserPredictProcessor();

  ProcessResult ProcessKeyEvent(const KeyEvent& key_event) override;

 private:
  void OnCommit(Context* ctx);
  void OnUpdate(Context* ctx);
  void OnDelete(Context* ctx);
  void RollbackLastWrite();
  void LearnCommit(const string& text);
  void CreatePredictSegment(Context* ctx);

  connection commit_connection_;
  connection update_connection_;
  connection delete_connection_;
  bool just_committed_ = false;
  bool self_updating_ = false;
};

}  // namespace user_predict
}  // namespace rime

#endif  // RIME_USER_PREDICT_PROCESSOR_H_
