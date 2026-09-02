/**
 * @file config_store_nvs.h
 * NVS-backed persistence of configuration blobs (SPEC §11.5). Framework-dependent
 * adapter: it stores and retrieves the bytes produced by truing/config_blob.h and
 * verifies their integrity before persisting or handing them back. It never
 * interprets configuration itself.
 */
#ifndef TRUING_CONFIG_STORE_NVS_H
#define TRUING_CONFIG_STORE_NVS_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "truing/config_blob.h"

esp_err_t truing_config_store_init(void);
/* Persists a blob after verifying header, kind and CRC. Rejects anything malformed. */
esp_err_t truing_config_store_save(truing_blob_kind_t kind, const uint8_t *blob, size_t len);
/* Loads a blob; ESP_ERR_NVS_NOT_FOUND when nothing is provisioned for `kind`.
 * The stored bytes are integrity-checked before being returned. */
esp_err_t truing_config_store_load(truing_blob_kind_t kind, uint8_t *buf, size_t cap, size_t *len_out);
esp_err_t truing_config_store_erase(truing_blob_kind_t kind);
const char *truing_config_store_key(truing_blob_kind_t kind);

#endif /* TRUING_CONFIG_STORE_NVS_H */
