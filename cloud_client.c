#include "cloud_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "simplelink.h"
#include "uart_if.h"
#include "utils/network_utils.h"

#define CLOUD_HOST "hipazwfbtjr6ch7df6pcnljue40gbszk.lambda-url.us-east-1.on.aws"
#define CLOUD_PORT 443
#define CLOUD_PATH_DETECT "/?mode=detect"
#define CLOUD_PATH_ENROLL "/?mode=enroll"
#define CLOUD_PATH_AUTH "/?mode=auth"
#define CLOUD_PATH_CLEAR "/?mode=clear"

#define CLOUD_DATE 30
#define CLOUD_MONTH 5
#define CLOUD_YEAR 2026
#define CLOUD_HOUR 15
#define CLOUD_MINUTE 0
#define CLOUD_SECOND 0

#define CLOUD_TX_HEADER_SIZE 384
#define CLOUD_RX_SIZE 1024
#define CLOUD_SEND_CHUNK_BYTES 1024

static int gCloudSocket = -1;

static int Cloud_OpenSocket(void)
{
    UART_PRINT("Cloud: connecting TLS...\n\r");

    gCloudSocket = tls_connect();
    if(gCloudSocket < 0) {
        UART_PRINT("Cloud: TLS failed: %d\n\r", gCloudSocket);
        return gCloudSocket;
    }

    return 0;
}

static int Cloud_SetTime(void)
{
    long retVal;

    g_time.tm_day = CLOUD_DATE;
    g_time.tm_mon = CLOUD_MONTH;
    g_time.tm_year = CLOUD_YEAR;
    g_time.tm_hour = CLOUD_HOUR;
    g_time.tm_min = CLOUD_MINUTE;
    g_time.tm_sec = CLOUD_SECOND;

    retVal = sl_DevSet(SL_DEVICE_GENERAL_CONFIGURATION,
                       SL_DEVICE_GENERAL_CONFIGURATION_DATE_TIME,
                       sizeof(SlDateTime),
                       (unsigned char *)(&g_time));

    return (int)retVal;
}

static int Cloud_SendAll(const char *data, unsigned long length)
{
    long retVal;
    unsigned long sent = 0;

    while(sent < length) {
        unsigned long chunk = length - sent;

        if(chunk > CLOUD_SEND_CHUNK_BYTES) {
            chunk = CLOUD_SEND_CHUNK_BYTES;
        }

        retVal = sl_Send(gCloudSocket, data + sent, chunk, 0);
        if(retVal < 0) {
            UART_PRINT("Cloud: send failed: %ld\n\r", retVal);
            return (int)retVal;
        }

        if(retVal == 0) {
            UART_PRINT("Cloud: send stopped\n\r");
            return -1;
        }

        sent += (unsigned long)retVal;
    }

    return 0;
}

static int Cloud_PostPcm(const char *path,
                         const short *pcmA,
                         unsigned long samplesA,
                         const short *pcmB,
                         unsigned long samplesB,
                         char *response,
                         unsigned long responseLen)
{
    char header[CLOUD_TX_HEADER_SIZE];
    long retVal;
    unsigned long bodyBytes = (samplesA + samplesB) * sizeof(short);

    if((path == 0) || (response == 0) || (responseLen == 0)) {
        return -1;
    }

    if(gCloudSocket < 0) {
        retVal = Cloud_OpenSocket();
        if(retVal < 0) {
            return (int)retVal;
        }
    }

    UART_PRINT("Cloud: POST %s, %lu bytes\n\r", path, bodyBytes);

    snprintf(header,
             sizeof(header),
             "POST %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "Connection: close\r\n"
             "Content-Type: application/octet-stream\r\n"
             "Content-Length: %lu\r\n"
             "\r\n",
             path,
             CLOUD_HOST,
             bodyBytes);

    retVal = Cloud_SendAll(header, strlen(header));
    if(retVal < 0) {
        sl_Close(gCloudSocket);
        gCloudSocket = -1;
        return (int)retVal;
    }

    if((samplesA > 0) && (pcmA != 0)) {
        retVal = Cloud_SendAll((const char *)pcmA, samplesA * sizeof(short));
        if(retVal < 0) {
            sl_Close(gCloudSocket);
            gCloudSocket = -1;
            return (int)retVal;
        }
    }

    if((samplesB > 0) && (pcmB != 0)) {
        retVal = Cloud_SendAll((const char *)pcmB, samplesB * sizeof(short));
        if(retVal < 0) {
            sl_Close(gCloudSocket);
            gCloudSocket = -1;
            return (int)retVal;
        }
    }

    UART_PRINT("Cloud: upload sent, waiting response...\n\r");

    retVal = sl_Recv(gCloudSocket, response, responseLen - 1, 0);
    if(retVal < 0) {
        UART_PRINT("Cloud: receive failed: %ld\n\r", retVal);
        sl_Close(gCloudSocket);
        gCloudSocket = -1;
        return (int)retVal;
    }

    response[retVal] = '\0';

    UART_PRINT("Cloud: response received\n\r");
    UART_PRINT("%s\n\r", response);

    sl_Close(gCloudSocket);
    gCloudSocket = -1;

    return 0;
}

static int ResponseHas(const char *response, const char *text)
{
    return ((response != 0) && (text != 0) && (strstr(response, text) != 0));
}

static int ParseIntAfter(const char *response, const char *key, int defaultValue)
{
    const char *found;

    if((response == 0) || (key == 0)) {
        return defaultValue;
    }

    found = strstr(response, key);
    if(found == 0) {
        return defaultValue;
    }

    found += strlen(key);
    while((*found == ' ') || (*found == ':')) {
        found++;
    }

    return atoi(found);
}

int Cloud_Init(void)
{
    long retVal;

    g_app_config.host = CLOUD_HOST;
    g_app_config.port = CLOUD_PORT;

    UART_PRINT("Cloud: connecting Wi-Fi...\n\r");
    retVal = connectToAccessPoint();
    if(retVal < 0) {
        UART_PRINT("Cloud: Wi-Fi failed: %ld\n\r", retVal);
        return (int)retVal;
    }

    UART_PRINT("Cloud: setting time...\n\r");
    retVal = Cloud_SetTime();
    if(retVal < 0) {
        UART_PRINT("Cloud: set time failed: %ld\n\r", retVal);
        return (int)retVal;
    }

    retVal = Cloud_OpenSocket();
    if(retVal < 0) {
        return (int)retVal;
    }

    sl_Close(gCloudSocket);
    gCloudSocket = -1;

    UART_PRINT("Cloud: ready\n\r");
    return 0;
}

CloudCommand_t Cloud_DetectCommand(const short *pcmA,
                                   unsigned long samplesA,
                                   const short *pcmB,
                                   unsigned long samplesB)
{
    char response[CLOUD_RX_SIZE];
    int retVal;

    UART_PRINT("Cloud: detect command\n\r");

    retVal = Cloud_PostPcm(CLOUD_PATH_DETECT,
                           pcmA,
                           samplesA,
                           pcmB,
                           samplesB,
                           response,
                           sizeof(response));
    if(retVal < 0) {
        UART_PRINT("Cloud: detect POST failed: %d\n\r", retVal);
        UART_PRINT("Cloud: detect failed, using auth\n\r");
        return CLOUD_COMMAND_AUTHENTICATE;
    }

    if(ResponseHas(response, "\"command\"") && ResponseHas(response, "\"enroll\"")) {
        UART_PRINT("Cloud: command = enroll\n\r");
        return CLOUD_COMMAND_ENROLL;
    }

    if(ResponseHas(response, "\"command\"") && ResponseHas(response, "\"clear\"")) {
        UART_PRINT("Cloud: command = clear\n\r");
        return CLOUD_COMMAND_CLEAR;
    }

    UART_PRINT("Cloud: command = auth\n\r");
    return CLOUD_COMMAND_AUTHENTICATE;
}

int Cloud_EnrollVoice(const short *pcmA,
                      unsigned long samplesA,
                      const short *pcmB,
                      unsigned long samplesB,
                      int *userId)
{
    char response[CLOUD_RX_SIZE];
    int retVal;

    UART_PRINT("Cloud: enroll voice\n\r");

    retVal = Cloud_PostPcm(CLOUD_PATH_ENROLL,
                           pcmA,
                           samplesA,
                           pcmB,
                           samplesB,
                           response,
                           sizeof(response));
    if(retVal < 0) {
        UART_PRINT("Cloud: enroll POST failed: %d\n\r", retVal);
        return -1;
    }

    if(userId != 0) {
        *userId = ParseIntAfter(response, "\"user_id\"", 0);
    }

    return ResponseHas(response, "\"enrolled\"") ? 0 : -1;
}

int Cloud_ClearProfiles(void)
{
    char response[CLOUD_RX_SIZE];
    int retVal;

    UART_PRINT("Cloud: clear profiles\n\r");

    retVal = Cloud_PostPcm(CLOUD_PATH_CLEAR, 0, 0, 0, 0, response, sizeof(response));
    if(retVal < 0) {
        UART_PRINT("Cloud: clear POST failed: %d\n\r", retVal);
        return -1;
    }

    return ResponseHas(response, "\"cleared\"") ? 0 : -1;
}

int Cloud_AuthenticateVoice(const short *pcmA,
                            unsigned long samplesA,
                            const short *pcmB,
                            unsigned long samplesB,
                            int *userId,
                            int *score)
{
    char response[CLOUD_RX_SIZE];
    int retVal;

    UART_PRINT("Cloud: authenticate voice\n\r");

    retVal = Cloud_PostPcm(CLOUD_PATH_AUTH,
                           pcmA,
                           samplesA,
                           pcmB,
                           samplesB,
                           response,
                           sizeof(response));
    if(retVal < 0) {
        UART_PRINT("Cloud: auth POST failed: %d\n\r", retVal);
        return -1;
    }

    if(userId != 0) {
        *userId = ParseIntAfter(response, "\"user_id\"", 0);
    }

    if(score != 0) {
        *score = ParseIntAfter(response, "\"score\"", 0);
    }

    return ResponseHas(response, "\"pass\"") ? 0 : -1;
}
