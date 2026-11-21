idf_component_register(
    SRCS "main.c"
         "app_zombie.c"
         "zombie_game.c"
         "zombie_store.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_hw_support esp_timer nvs_flash
)
