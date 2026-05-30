#ifndef __CLOUD_CLIENT_H__
#define __CLOUD_CLIENT_H__

#ifdef __cplusplus
extern "C"
{
#endif

typedef enum {
    CLOUD_COMMAND_AUTHENTICATE = 0,
    CLOUD_COMMAND_ENROLL,
    CLOUD_COMMAND_CLEAR
} CloudCommand_t;

int Cloud_Init(void);
CloudCommand_t Cloud_DetectCommand(const short *pcm, unsigned long samples);
int Cloud_EnrollVoice(const short *pcm, unsigned long samples, int *userId);
int Cloud_ClearProfiles(void);
int Cloud_AuthenticateVoice(const short *pcm,
                            unsigned long samples,
                            int *userId,
                            int *score);

#ifdef __cplusplus
}
#endif

#endif
