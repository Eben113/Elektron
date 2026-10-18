idf_component_register(
    SRCS "publisher.cpp"
    INCLUDE_DIRS "."
    REQUIRES sensor arduino-esp32 TinyGSM PubSubClient
    PRIV_REQUIRES esp_timer)