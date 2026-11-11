/*
 * Вшитый Caddy local CA в виде, который ждёт app_config.cpp:
 * символы _binary_kouki_local_root_pem_start/_end создаёт ESP-IDF из
 * EMBED_TXTFILES (firmware/main/CMakeLists.txt); в host-стенде их
 * объявляет этот файл. Копия обязана совпадать с
 * firmware/main/certs/kouki-local-root.pem побайтно — это проверяет
 * test_config.cpp (сравнение с файлом на диске).
 */

const char _binary_kouki_local_root_pem_start[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBozCCAUmgAwIBAgIQbGdVNmq1SQsDHWfLavqiXDAKBggqhkjOPQQDAjAwMS4w\n"
    "LAYDVQQDEyVDYWRkeSBMb2NhbCBBdXRob3JpdHkgLSAyMDI2IEVDQyBSb290MB4X\n"
    "DTI2MTEwNDEyMzU1N1oXDTM2MDkxMjEyMzU1N1owMDEuMCwGA1UEAxMlQ2FkZHkg\n"
    "TG9jYWwgQXV0aG9yaXR5IC0gMjAyNiBFQ0MgUm9vdDBZMBMGByqGSM49AgEGCCqG\n"
    "SM49AwEHA0IABO8mGmUQRpqZqkWPQa3sXxVZCQrWxryxqfdTW14Y25nFme0CLNMf\n"
    "fFGkAQuWuLsEkZr+zuD8XWWmIVu6j9iR9ImjRTBDMA4GA1UdDwEB/wQEAwIBBjAS\n"
    "BgNVHRMBAf8ECDAGAQH/AgEBMB0GA1UdDgQWBBSUWJP5rT2Fs1t6tNPjM+GjqL6a\n"
    "5jAKBggqhkjOPQQDAgNIADBFAiEAn+tHAssIh/4bvqW7b7OANgIUhaOSenjD7jqf\n"
    "efK9pxMCIASr7oZZqRg38ocsskrxJCbwvWn/hRjRynEQ4Lv8Ypxe\n"
    "-----END CERTIFICATE-----\n"
    ;
/*
 * В образе ESP-IDF генерирует этот символ как конец EMBED_TXTFILES-файла.
 * Модуль его не читает (в app_config.c объявление тоже было неиспользуемым),
 * поэтому в стенде достаточно пустого массива — он нужен только линковке.
 */
const char _binary_kouki_local_root_pem_end[] = "";
