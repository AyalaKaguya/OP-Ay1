idf_component_register(SRCS "app_nand.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES esp-mosaico-bsp spi_nand_flash esp_blockdev joltwallet__littlefs
                  esp_driver_spi esp_driver_gpio log)
