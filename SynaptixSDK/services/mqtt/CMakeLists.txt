# services/mqtt - bridge between SimplePLC plc_mqtt and the module's MQTT client.
add_library(sx_mqtt STATIC sx_mqtt.c)
target_include_directories(sx_mqtt PUBLIC ${CMAKE_CURRENT_LIST_DIR})
# app_config.h holds the default broker.
target_include_directories(sx_mqtt PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../app)
target_link_libraries(sx_mqtt PUBLIC splc sx_st67 PRIVATE stm32cubemx)