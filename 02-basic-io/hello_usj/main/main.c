// J19（USB Serial/JTAG）で Hello World と、1 行ずつのエコー（ESP32-P4X-Function-EV-Board、ESP-IDF v6.0.3）
// sdkconfig.defaults でコンソールの主を USB Serial/JTAG にしている（既定のままだと J19 は出力だけ）
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"

void app_main(void)
{
    // 入力も受けるため、USB Serial/JTAG のドライバを入れて stdin/stdout をドライバ経由にする
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    usb_serial_jtag_vfs_use_driver();
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CRLF);  // 端末の CR+LF を LF 1 つにする
    setvbuf(stdin, NULL, _IONBF, 0);

    printf("Hello world! (J19 USB Serial/JTAG)\n");
    printf("type a line and press Enter\n");

    char line[64];
    int n = 0;
    while (fgets(line, sizeof(line), stdin) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        printf("[%d] echo: %s\n", ++n, line);
    }
}
