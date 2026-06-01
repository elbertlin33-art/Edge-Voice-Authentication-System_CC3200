#include "cloud_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "simplelink.h"
#include "uart_if.h"
#include "utils/network_utils.h"

#define CLOUD_HOST "hipazwfbtjr6ch7df6pcnljue40gbszk.lambda-url.us-east-1.on.aws"
#define CLOUD_PORT 443
#define CLOUD_PATH_PROCESS "/?mode=process"
#define CLOUD_PATH_STATUS "/?mode=status"
#define CLOUD_PATH_ENROLL_PASSWORD "/?mode=enroll_password"
#define CLOUD_PATH_ENROLL_NAME "/?mode=enroll_name"
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
#define CLOUD_RX_SIZE 2048
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
    unsigned long received = 0;

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

    while(received < (responseLen - 1)) {
        retVal = sl_Recv(gCloudSocket,
                         response + received,
                         responseLen - 1 - received,
                         0);

        if(retVal > 0) {
            received += (unsigned long)retVal;
            continue;
        }

        if(retVal == 0) {
            break;
        }

        if(received > 0) {
            break;
        }

        UART_PRINT("Cloud: receive failed: %ld\n\r", retVal);
        sl_Close(gCloudSocket);
        gCloudSocket = -1;
        return (int)retVal;
    }

    response[received] = '\0';

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

static void ParseStringAfter(const char *response,
                             const char *key,
                             char *out,
                             unsigned long outLen)
{
    const char *found;
    unsigned long i = 0;

    if((response == 0) || (key == 0) || (out == 0) || (outLen == 0)) {
        return;
    }

    out[0] = '\0';
    found = strstr(response, key);
    if(found == 0) {
        return;
    }

    found += strlen(key);
    while((*found != '\0') && (*found != ':')) {
        found++;
    }

    if(*found == ':') {
        found++;
    }

    while((*found == ' ') || (*found == '"')) {
        found++;
    }

    while((found[i] != '\0') &&
          (found[i] != '"') &&
          (found[i] != ',') &&
          (found[i] != '}') &&
          (i < (outLen - 1))) {
        out[i] = found[i];
        i++;
    }

    out[i] = '\0';
}

static CloudCommand_t ParseCommand(const char *response)
{
    char command[16];

    ParseStringAfter(response, "\"command\"", command, sizeof(command));

    if(strcmp(command, "enroll") == 0) {
        return CLOUD_COMMAND_ENROLL;
    }

    if(strcmp(command, "clear") == 0) {
        return CLOUD_COMMAND_CLEAR;
    }

    if(strcmp(command, "auth") == 0) {
        return CLOUD_COMMAND_AUTHENTICATE;
    }

    return CLOUD_COMMAND_ERROR;
}

static int Cloud_ParseResult(const char *response, CloudResult_t *result)
{
    char status[16];

    if((response == 0) || (result == 0)) {
        return -1;
    }

    if(!ResponseHas(response, "\"ok\"") || !ResponseHas(response, "true")) {
        UART_PRINT("Cloud: no valid result\n\r");
        return -1;
    }

    memset(result, 0, sizeof(CloudResult_t));

    result->ok = 1;
    result->command = ParseCommand(response);
    ParseStringAfter(response, "\"result\"", status, sizeof(status));
    result->passed = (strcmp(status, "pass") == 0);
    result->userId = ParseIntAfter(response, "\"user_id\"", 0);
    result->score = ParseIntAfter(response, "\"score\"", 0);
    ParseStringAfter(response, "\"word\"", result->word, sizeof(result->word));
    ParseStringAfter(response, "\"words\"", result->words, sizeof(result->words));
    ParseStringAfter(response, "\"transcript\"",
                     result->transcript,
                     sizeof(result->transcript));
    ParseStringAfter(response, "\"reason\"", result->reason, sizeof(result->reason));
    ParseStringAfter(response, "\"user_name\"",
                     result->userName,
                     sizeof(result->userName));
    ParseStringAfter(response, "\"password\"",
                     result->password,
                     sizeof(result->password));

    if((result->words[0] == '\0') && (result->transcript[0] != '\0')) {
        strncpy(result->words, result->transcript, sizeof(result->words) - 1);
        result->words[sizeof(result->words) - 1] = '\0';
    }

    UART_PRINT("Cloud: word = %s\n\r", result->word);
    UART_PRINT("Cloud: words = %s\n\r", result->words);
    if(result->userName[0] != '\0') {
        UART_PRINT("Cloud: user_name = %s\n\r", result->userName);
    }
    if(result->password[0] != '\0') {
        UART_PRINT("Cloud: password = %s\n\r", result->password);
    }
    if(result->reason[0] != '\0') {
        UART_PRINT("Cloud: reason = %s\n\r", result->reason);
    }
    UART_PRINT("Cloud: command = %d result = %s user = %d score = %d\n\r",
               result->command,
               status,
               result->userId,
               result->score);

    if(result->command == CLOUD_COMMAND_ERROR) {
        return -1;
    }

    return 0;
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

int Cloud_CheckUsers(void)
{
    char response[CLOUD_RX_SIZE];
    int retVal;
    int userCount;

    UART_PRINT("Cloud: checking enrolled users\n\r");

    retVal = Cloud_PostPcm(CLOUD_PATH_STATUS,
                           0,
                           0,
                           0,
                           0,
                           response,
                           sizeof(response));
    if(retVal < 0) {
        UART_PRINT("Cloud: status POST failed: %d\n\r", retVal);
        return retVal;
    }

    if(!ResponseHas(response, "\"ok\"") || !ResponseHas(response, "true")) {
        UART_PRINT("Cloud: invalid status response\n\r");
        return -1;
    }

    userCount = ParseIntAfter(response, "\"user_count\"", -1);
    UART_PRINT("Cloud: user_count = %d\n\r", userCount);

    if(userCount < 0) {
        return -1;
    }

    return (userCount > 0) ? 1 : 0;
}

int Cloud_ProcessVoice(const short *pcm,
                       unsigned long samples,
                       CloudResult_t *result)
{
    char response[CLOUD_RX_SIZE];
    int retVal;

    if(result == 0) {
        return -1;
    }

    memset(result, 0, sizeof(CloudResult_t));
    result->command = CLOUD_COMMAND_ERROR;

    UART_PRINT("Cloud: process voice\n\r");

    retVal = Cloud_PostPcm(CLOUD_PATH_PROCESS,
                           pcm,
                           samples,
                           0,
                           0,
                           response,
                           sizeof(response));
    if(retVal < 0) {
        UART_PRINT("Cloud: process POST failed: %d\n\r", retVal);
        return retVal;
    }

    return Cloud_ParseResult(response, result);
}

int Cloud_EnrollPassword(const short *pcm,
                         unsigned long samples,
                         CloudResult_t *result)
{
    char response[CLOUD_RX_SIZE];
    int retVal;

    if(result == 0) {
        return -1;
    }

    UART_PRINT("Cloud: enroll password\n\r");

    retVal = Cloud_PostPcm(CLOUD_PATH_ENROLL_PASSWORD,
                           pcm,
                           samples,
                           0,
                           0,
                           response,
                           sizeof(response));
    if(retVal < 0) {
        UART_PRINT("Cloud: enroll password POST failed: %d\n\r", retVal);
        return retVal;
    }

    return Cloud_ParseResult(response, result);
}

int Cloud_EnrollName(const short *pcm,
                     unsigned long samples,
                     CloudResult_t *result)
{
    char response[CLOUD_RX_SIZE];
    int retVal;

    if(result == 0) {
        return -1;
    }

    UART_PRINT("Cloud: enroll name\n\r");

    retVal = Cloud_PostPcm(CLOUD_PATH_ENROLL_NAME,
                           pcm,
                           samples,
                           0,
                           0,
                           response,
                           sizeof(response));
    if(retVal < 0) {
        UART_PRINT("Cloud: enroll name POST failed: %d\n\r", retVal);
        return retVal;
    }

    return Cloud_ParseResult(response, result);
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
