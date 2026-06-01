idf_component_register(
    SRCS main.c effects.c display.c
    INCLUDE_DIRS .
    REQUIRES esp_camera esp_bt esp_spp driver nvs_flash fatfs sdmmc
)
