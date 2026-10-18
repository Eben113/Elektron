idf_component_register(
    SRCS "node.cpp"
    INCLUDE_DIRS "."
    REQUIRES ZMPT101B arduino-esp32 freertos
    PRIV_REQUIRES esp_timer)