idf_component_register(
    SRCS "app_main.c"
         "lighting.c"
         "lamp_state.c"
         "button.c"
         "audio_in.c"
         "i2s_mic.c"
         "uac_mic.c"
         "voice_pipeline.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_i2s esp_driver_gpio esp_driver_ledc nvs_flash esp_psram
    PRIV_REQUIRES esp_timer
)
