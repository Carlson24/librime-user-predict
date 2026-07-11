#include <rime_api.h>
#include <rime/common.h>
#include <rime/registry.h>

#include "user_predict_filter.h"
#include "user_predict_processor.h"
#include "user_predict_translator.h"

using namespace rime;

static void rime_user_predict_initialize() {
  LOG(INFO) << "registering components from module 'user_predict'.";
  Registry& r = Registry::instance();
  r.Register("user_predict_processor",
             new Component<user_predict::UserPredictProcessor>);
  r.Register("user_predict_translator",
             new Component<user_predict::UserPredictTranslator>);
  r.Register("user_predict_filter",
             new Component<user_predict::UserPredictFilter>);
}

static void rime_user_predict_finalize() {}

RIME_REGISTER_MODULE(user_predict)
