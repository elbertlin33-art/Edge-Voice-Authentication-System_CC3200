#ifndef __CLOUD_CLIENT_H__
#define __CLOUD_CLIENT_H__

#ifdef __cplusplus
extern "C"
{
#endif

typedef enum {
    CLOUD_COMMAND_AUTHENTICATE = 0,
    CLOUD_COMMAND_ENROLL,
    CLOUD_COMMAND_CLEAR,
    CLOUD_COMMAND_ERROR
} CloudCommand_t;

typedef struct {
    CloudCommand_t command;
    int ok;
    int passed;
    int userId;
    int score;
    char word[32];
} CloudResult_t;

int Cloud_Init(void);
int Cloud_ProcessVoice(const short *pcm,
                       unsigned long samples,
                       CloudResult_t *result);
CloudCommand_t Cloud_DetectCommand(const short *pcmA,
                                   unsigned long samplesA,
                                   const short *pcmB,
                                   unsigned long samplesB);
int Cloud_EnrollVoice(const short *pcmA,
                      unsigned long samplesA,
                      const short *pcmB,
                      unsigned long samplesB,
                      int *userId);
int Cloud_ClearProfiles(void);
int Cloud_AuthenticateVoice(const short *pcmA,
                            unsigned long samplesA,
                            const short *pcmB,
                            unsigned long samplesB,
                            int *userId,
                            int *score);

#ifdef __cplusplus
}
#endif

#endif
