#ifndef FILE_SERVER_H
#define FILE_SERVER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Start HTTP file server on given port, serving files from SD card.
 * Returns ESP_OK on success. */
esp_err_t file_server_start(int port);

/* Stop file server */
void file_server_stop(void);

/* Check if server is running */
bool file_server_is_running(void);

/* Get number of requests served */
uint32_t file_server_get_request_count(void);

#endif /* FILE_SERVER_H */
