project(rime-user-predict)
cmake_minimum_required(VERSION 3.10)

set(USER_PREDICT_VERSION 1.0.0)
set(USER_PREDICT_SOVERSION 1)

aux_source_directory(src user_predict_src)

set(user_predict_library rime-user-predict)
set(user_predict_deps ${rime_library})
set(user_predict_modules "user_predict")

add_library(rime-user-predict-objs OBJECT ${user_predict_src})
if(BUILD_SHARED_LIBS)
  set_target_properties(rime-user-predict-objs
    PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
endif()

set(plugin_name ${user_predict_library} PARENT_SCOPE)
set(plugin_objs $<TARGET_OBJECTS:rime-user-predict-objs> PARENT_SCOPE)
set(plugin_deps ${user_predict_deps} PARENT_SCOPE)
set(plugin_modules ${user_predict_modules} PARENT_SCOPE)
