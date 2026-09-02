#include "config_store_nvs.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "cfg_store";
#define TRUING_NVS_NAMESPACE "truing_cfg"

const char *truing_config_store_key(truing_blob_kind_t kind)
{
    switch (kind) {
    case TRUING_BLOB_KIND_WHEEL_CLASS:
        return "wheel";
    case TRUING_BLOB_KIND_SOLVER:
        return "solver";
    case TRUING_BLOB_KIND_CHAIN_PROFILE:
        return "chain";
    case TRUING_BLOB_KIND_TENSION_MODEL_PROFILE:
        return "tmodel";
    case TRUING_BLOB_KIND_MACHINE_PROFILE:
        return "machine";
    default:
        return NULL;
    }
}

esp_err_t truing_config_store_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs erase (%s); erasing", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err != ESP_OK) {
            return err;
        }
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t truing_config_store_save(truing_blob_kind_t kind, const uint8_t *blob, size_t len)
{
    const char *key = truing_config_store_key(kind);
    if (key == NULL || blob == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    truing_blob_kind_t stored_kind = TRUING_BLOB_KIND_UNSET;
    const truing_blob_result_t check = truing_blob_peek(blob, len, &stored_kind, NULL, NULL);
    if (check != TRUING_BLOB_OK || stored_kind != kind) {
        ESP_LOGE(TAG, "refusing to persist malformed %s blob (%s)", truing_blob_kind_str(kind), truing_blob_result_str(check));
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(TRUING_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, key, blob, len);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t truing_config_store_load(truing_blob_kind_t kind, uint8_t *buf, size_t cap, size_t *len_out)
{
    const char *key = truing_config_store_key(kind);
    if (key == NULL || buf == NULL || len_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *len_out = 0u;
    nvs_handle_t h;
    esp_err_t err = nvs_open(TRUING_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;   /* ESP_ERR_NVS_NOT_FOUND when the namespace was never written */
    }
    size_t len = 0u;
    err = nvs_get_blob(h, key, NULL, &len);
    if (err == ESP_OK) {
        if (len > cap) {
            err = ESP_ERR_NVS_INVALID_LENGTH;
        } else {
            err = nvs_get_blob(h, key, buf, &len);
        }
    }
    nvs_close(h);
    if (err != ESP_OK) {
        return err;
    }
    truing_blob_kind_t stored_kind = TRUING_BLOB_KIND_UNSET;
    const truing_blob_result_t check = truing_blob_peek(buf, len, &stored_kind, NULL, NULL);
    if (check != TRUING_BLOB_OK || stored_kind != kind) {
        ESP_LOGE(TAG, "stored %s blob failed integrity check (%s)", truing_blob_kind_str(kind), truing_blob_result_str(check));
        return ESP_ERR_INVALID_CRC;
    }
    *len_out = len;
    return ESP_OK;
}

esp_err_t truing_config_store_erase(truing_blob_kind_t kind)
{
    const char *key = truing_config_store_key(kind);
    if (key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(TRUING_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(h, key);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}
